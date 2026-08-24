
[[monomorphism]]

[[stdlib]]


Profiling 1:
I tried to run macOS `sample` as the sampling profiler, but this machine refused process inspection without elevated privileges:

`sample cannot examine process ...; try running with sudo`

So I could not produce true OS sampling stacks. I did run a `RelWithDebInfo` profiling build and a phase-timing harness over the entire example suite: 192 `.noria` programs, 100 rounds, 19,200 successful compiler runs.

**Compiler Phase Hot Paths**

|Phase|Time|Share|
|---|---|---|
|`resolveImports`|12.39s|46.8%|
|`monomorphizeGenerics`|8.21s|31.0%|
|LLVM IR codegen|3.32s|12.6%|
|typecheck|1.06s|4.0%|
|lex|1.05s|4.0%|
|parse|0.43s|1.6%|

**Main Bottlenecks**

The hot compiler areas are:

1. Import resolution in [src/ModuleResolver.cpp (line 471)](/Users/matthewmcrae/lang/src/ModuleResolver.cpp:471), especially merging imported stdlib modules and dependency closure.
2. Generic monomorphization in [src/Monomorphize.cpp (line 85)](/Users/matthewmcrae/lang/src/Monomorphize.cpp:85), especially specialization collection, cloning, substitution, and expansion.
3. Codegen in [src/Codegen.cpp](/Users/matthewmcrae/lang/src/Codegen.cpp), but it is much less dominant than import + mono.

**Slowest Example Compiles**

The slowest individual programs are all stdlib/generic ADT-heavy:

- `complex_sequence_set_pair.noria`: 6.58ms
- `dictionary_hashmap_str_tombstone.noria`: 4.44ms
- `set_hashmap_ops.noria`: 4.39ms
- `dictionary_hashmap_bool_i32.noria`: 4.25ms
- `dictionary_hashmap_contains_remove.noria`: 4.21ms

**Takeaway**

Parsing is not the bottleneck. The compiler’s heat is in repeated stdlib import resolution plus generic specialization/monomorphization. The first optimization targets I’d look at are caching resolved stdlib modules/dependency merges across compiler invocations, then reducing repeated clone/substitute work during monomorphization.


Improvements made:

Generic specializations (especially from stdlib imports) eat up most of the current compilation time.

Introduced caching (LFU eviction policy) for stdlib generic specializations and generic specialziations i ngeneral. This cache deep copies the AST for specializations to allow that portion of AST to be joined to a new programs AST instead of re-computing everything.

LFU eviction policy chosen over LRU because stdlib are the biggest offender and are frequently used.  Eviction policy used to prevent bloating memory.




Profiling 2:

**Current Code, Cache Enabled**

| Phase                    | Time  | Share | Count  |
| ------------------------ | ----- | ----- | ------ |
| `monomorphize_cached`    | 7.25s | 43.1% | 19,600 |
| `resolve_imports_cached` | 4.44s | 26.3% | 6,400  |
| `codegen`                | 3.05s | 18.1% | 19,600 |
| `typecheck`              | 0.86s | 5.1%  | 19,600 |
| `lex`                    | 0.75s | 4.5%  | 19,600 |
| `parse`                  | 0.49s | 2.9%  | 19,600 |

Total wall: `20.15s`

Cache state at end:

- Parsed stdlib modules: `13`
- Stdlib specializations: `203`

Net result: `27.69s -> 20.15s`, about a `27%` improvement in the compiler-only phase harness. The biggest win is import resolution: `13.09s -> 4.44s`.


Adding cache sensitivity, dont cache small generics, only cache deep reccursive generics, or generics above a size threshold (confgigurable)

make round by round typechecking only check new types, do not go through the entire ast each time, this is expensive

same thing for rewrites

Profiling 3 results:

| Phase                    |  Time | Share |  Count |
| ------------------------ | ----: | ----: | -----: |
| `monomorphize_cached`    | 2.29s | 32.5% | 19,600 |
| `resolve_imports_cached` | 1.88s | 26.7% |  6,400 |
| `codegen`                | 1.73s | 24.5% | 19,600 |
| `lex_parse`              | 0.66s |  9.4% | 19,600 |
| `typecheck`              | 0.49s |  6.9% | 19,600 |







