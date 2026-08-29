# Performance engineering

Noria's compiler performance work began with phase measurements, not a presumed hot path. The main finding was that parsing was cheap relative to repeatedly resolving source-written standard-library modules and cloning, checking, and rewriting generic specializations. The implemented response combines bounded process-local caching with less repeated generic work.

This page separates the measured result from the current design and states what the historical experiment can and cannot prove. For broader architecture, see [ENGINEERING.md](ENGINEERING.md); for build and test commands, see the [project overview](README.md).

## Result and attribution

The final historical experiment processed 196 Noria inputs for 100 rounds: 19,600 in-process compilations through LLVM IR generation. In the same benchmark environment, aggregate timed compiler phases changed as follows:

| Implementation point | Aggregate time | Change from baseline |
| --- | ---: | ---: |
| Before reusable AST-component caching | 27.69s | baseline |
| Parsed-module and stdlib-specialization cache | 20.15s | 27.2% reduction |
| Selective admission + frontier-only specialization work | 7.05s | 74.5% reduction; 3.93× faster |

The full 3.93× result should not be attributed to LFU caching alone. Caching delivered the first 27.2%; the larger final change also stopped admitting cheap-to-recompute specializations and avoided rechecking/reworking the entire specialized AST on every monomorphization round.

The final phase totals were:

| Phase | Aggregate time | Share | Invocations |
| --- | ---: | ---: | ---: |
| Monomorphization | 2.29s | 32.5% | 19,600 |
| Import resolution | 1.88s | 26.7% | 6,400 |
| LLVM IR generation | 1.73s | 24.5% | 19,600 |
| Lexing + parsing | 0.66s | 9.4% | 19,600 |
| Type checking | 0.49s | 6.9% | 19,600 |
| **Total** | **7.05s** | **100%** | — |

Import resolution has fewer invocations because only inputs with imports enter that phase.

## Measurement method

The experiment used a `RelWithDebInfo` compiler build and an in-process phase-timing harness over the example corpus. Each input was compiled repeatedly through LLVM IR generation so the workload included lexing, parsing, module loading, semantic checking, monomorphization, and code generation rather than isolating a parser microbenchmark.

macOS process sampling was attempted first, but the development environment denied process inspection without elevated privileges. The investigation therefore used explicit phase timers. Repetition was used to make the import-resolution and monomorphization costs large enough to distinguish from single-run noise.

Three controls matter when interpreting the result:

- the before/after totals were captured in one development environment on the same workload;
- all reported values are aggregate phase time for a repeated in-process workload, not a promise about a user's single-file latency;
- the process-local cache is warm across calls, so the workload is closer to a long-lived consumer of `compileSource()` than to unrelated CLI processes.

## What changed

### Parsed module and specialization reuse

[`CompilerCache`](../include/noria/CompilerCache.hpp) owns two mutex-protected LFU caches:

| Cache | Capacity | Admission |
| --- | ---: | --- |
| Parsed stdlib modules | 64 entries | Every parsed stdlib module |
| Stdlib function/struct specializations | 256 entries | Functions with at least 1 KiB computed AST weight; structs with at least eight fields |

Cache keys include the canonicalized stdlib root, module origin, specialization kind, and deterministic mangled name as applicable. This prevents two stdlib roots or two concrete specializations from sharing an entry accidentally.

ASTs are cloned both when stored and retrieved. The extra copying is deliberate: return inference and specialization rewriting mutate the working AST, so returning a shared cached object would make later compilations depend on earlier ones. [`compiler_cache_test.cpp`](../tests/compiler_cache_test.cpp) mutates a retrieved clone and confirms the retained entry remains unchanged.

The cache assumes files under a canonical stdlib root do not change during the process lifetime. [`CompilerCache::clear()`](../include/noria/CompilerCache.hpp) provides explicit invalidation; a long-running compiler service would need content hashes or file metadata in its keys.

### LFU policy and data structures

[`LFUCache`](../include/noria/LfuCache.hpp) groups entries into frequency buckets and keeps direct indexes from keys and frequencies to list nodes. Within the minimum-frequency bucket it evicts the oldest entry, making ties deterministic. Both indexes use Noria's own open-addressed [`HashTable`](../include/noria/HashTable.hpp), which uses double hashing and tombstones.

LFU was chosen because frequently reused stdlib components should survive one-off specializations. Capacity bounds prevent a long-lived process from retaining every concrete instantiation. [`lfu_cache_test.cpp`](../tests/lfu_cache_test.cpp) covers promotion, tie-breaking, capacity churn, duplicate admission, disabled capacity, and index reuse after `clear()`.

### Frontier-only generic work

Monomorphization is a worklist, not a whole-module retry loop. Each round sorts and deduplicates pending requests, emits only unseen concrete functions/structs, type-checks the newly emitted frontier, and discovers requests reachable from that frontier. Generic templates are stripped only after the worklist closes.

This changes the scaling variable from "all specializations emitted so far × number of rounds" toward "new specializations in this round." Deterministic sorting and mangling also make cache keys stable and emitted IR reproducible. Cycle links plus round/expansion limits bound pathological generic growth.

## Generated-code performance choices

Noria does not yet have a maintained runtime benchmark suite, so the statements below are design-level complexity claims backed by implementation and behavior tests rather than measured throughput claims.

- Implementation tags are erased by monomorphization; choosing `arr`, `list`, `bst`, or `hashmap` adds no runtime dispatch.
- Array-backed Sequence append uses geometric growth for amortized O(1) push; indexed access is O(1).
- List-backed Sequence append is O(1), while indexed access is O(n).
- Hashmap uses open addressing, tombstones, and resize at 75% load for expected O(1) lookup.
- BST is deliberately unbalanced, so operations are O(h) and degrade to O(n) on adversarial insertion order.
- Heap is implemented over Sequence. Array backing gives the expected logarithmic push/pop; list backing preserves semantics but makes repeated indexed access expensive.
- Scalar-only lexical scopes are marked so code generation can skip managed-value drop traversal for those scopes.

The largest accepted runtime tradeoff is deep-copy value semantics for managed aggregates. Borrowed parameters and moved owned returns avoid some copies, but copying a large array, string-owning container, or managed struct remains O(n). There is no reference counting, copy-on-write, or borrow checker.

## Reproducibility status

The 27.69s → 7.05s result is useful evidence of the optimization decision, but it is not a regression-grade benchmark in the current repository:

- the original timing harness is not checked in;
- machine model, OS version, compiler version, and run-to-run distributions were not recorded with the result;
- the language corpus has grown since the measurement;
- current CI validates cache correctness but does not enforce latency, throughput, or memory thresholds.

Accordingly, the result should be cited as a historical controlled in-process measurement, not as a reproducible current benchmark or a single-compile speedup. The most valuable next measurement work is a checked-in benchmark target that records toolchain/hardware metadata, separates cold and warm caches, reports median and tail distributions over multiple processes, and measures peak retained memory alongside time. Runtime follow-up should benchmark managed copies and each ADT implementation across input sizes rather than relying only on asymptotic analysis.

## Verification pointers

| Concern | Code | Tests |
| --- | --- | --- |
| Cache capacity, admission, clone isolation, keys | [`CompilerCache.cpp`](../src/CompilerCache.cpp) | [`compiler_cache_test.cpp`](../tests/compiler_cache_test.cpp) |
| LFU eviction and index integrity | [`LfuCache.hpp`](../include/noria/LfuCache.hpp), [`HashTable.hpp`](../include/noria/HashTable.hpp) | [`lfu_cache_test.cpp`](../tests/lfu_cache_test.cpp), [`hash_table_test.cpp`](../tests/hash_table_test.cpp) |
| Frontier specialization and deduplication | [`src/monomorphize/`](../src/monomorphize/) | [`generics_test.cpp`](../tests/generics_test.cpp) and end-to-end IR assertions |
| ADT behavior across representations | [`stdlib/`](../stdlib/) | `sequence_arr_list_conformance.noria`, container reference-model fixtures, and native checks in [`run_examples.sh`](../tests/run_examples.sh) |
