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