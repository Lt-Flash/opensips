## Summary

This PR introduces **`cachedb_perf`** — a new, from-scratch `cachedb` backend for large, high-churn local caches, selected by URL scheme (**`perf://`**). I'm trying to build a much faster local cache module by combining a cache-conscious, lock-free-read design (CLHT / MemC3 lineage) with what recent Linux kernels make possible: overcommit hugetlb pools, shmem THP, `MADV_COLLAPSE`, `MADV_POPULATE_WRITE` and swap pinning.

It implements the same `cachedb_funcs` vtable as every other backend, so **any module taking a `cachedb_url` works unchanged**, and core script usage (`cache_store("perf", ...)`) only changes the backend name. v1 is deliberately a **single-node in-memory cache**: no clusterer replication, no restart persistency — deployments sharing state via `cachedb_local` + `cluster_id` are out of scope for now.

Draft because the roadmap below is about half done — but the module already runs as a complete cache, validated by built-in selftests and a script-level end-to-end suite.

## Motivation

Found while benchmarking `topology_hiding`'s cacheDB state backend (#4114): setting `cache_collections "th=16"` cut the load balancer's CPU from **45% to 29%** at 4000 CPS. The root cause is structural: `cachedb_local`'s hash table is sized **once** from `cache_collections` and never resized. The default is 512 buckets, most deployments never set the parameter, and at 50 000 entries that is a load factor of ~98 — roughly 50 string compares and 50 dependent cache misses per lookup. The module also exports zero statistics, so the cliff is invisible in production.

Rather than progressively rewriting a module every deployment depends on, this is a clean backend: operators opt in per collection by changing a URL.

## The study

Everything below was measured, not assumed — the benchmark rig ships in-tree (`modules/cachedb_perf/bench/`, `make run`, no OpenSIPS build needed) and every figure is reproducible. Hosts: Xeon E5-2699 v4, kernels 5.4 / 6.8 / 6.12. The rig models structures and cache behaviour (single process, threads); it ranks designs rather than predicting server throughput.

### 1. The index structure

![structure shootout](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/01-structure-shootout.png)

| design | @512 buckets (shipped default) | @65536 buckets |
|---|---|---|
| chained + `strncmp` (`cachedb_local` today) | 2484 ns | 111 ns |
| chained + hash cached in node | 1837 ns | 86 ns |
| sorted array per bucket + binary search | 134 ns | 100 ns |
| **64B cache-line bucket + 1-byte tags (this module)** | **84 ns** | |
| flat open addressing (rejected: stop-the-world resize) | 78 ns | |

Load factor alone is a **20× spread**. The chosen design is within 8% of the fastest structure measured, and the fastest one (flat open addressing) is impossible to resize across processes in shm. Also checked: `core_hash()` is *not* at fault (chi²/df 0.65–1.18 vs FNV-1a on thids, dialog ids, AoRs and call-ids — statistically indistinguishable), so the module keeps it.

### 2. Concurrency — an honest negative result

![concurrency scaling](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/02-concurrency-scaling.png)

The hypothesis was that `cachedb_local`'s write-lock-on-every-read destroys scaling. **It does not**: with a well-sized table workers rarely collide on a bucket lock, and it scales 8.4× on 8 threads. The 4× gap is a **per-operation constant factor** (no atomic RMW on reads, one cache line per bucket, tag filtering) — not a scaling win. Against the shipped 512-bucket default the gap is ~90×.

### 3. The read protocol — measured before being believed

![read protocols](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/04-read-protocols.png)

Readers take no locks: a per-bucket seqlock with bounded retries and a sleeping-lock fallback. What makes this legal in OpenSIPS specifically: shm is mapped once before fork and never unmapped, so a stale pointer read is garbage-but-not-a-fault, and the version re-check discards it — the value is always copied out inside the optimistic section, with every length clamped and every pointer extent-checked before use.

The one credible alternative (QSBR / pointer-publication, no version check at all) was implemented in the rig and **rejected on the numbers**: identical at 100% reads (on x86/TSO the version loads hit the already-loaded bucket line — the seqlock is free) and ahead only under single-hot-bucket write contention that SIP traffic doesn't exhibit (seqlock retries measured at 1.2 per 1000 reads on a uniform 95/5 mix). The useful piece survived without any grace-period machinery: a byte-identical `set()` that only refreshes the TTL — the dominant write in the motivating workload — takes the bucket lock but skips the version bumps and the memcpy entirely. One atomic `expires` store; concurrent readers of the bucket are undisturbed.

### 4. What was rejected: write staging and queueing

![write staging](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/05-write-staging.png)

| queued writes, 8-thread budget | applied Mops/s | vs direct | ring full |
|---|---|---|---|
| 8 direct writers | **116.3** | 1.00× | — |
| 7 producers + 1 consumer | 24.1 | 0.21× | 99% |
| 4 producers + 4 consumers | 54.6 | 0.47× | 97% |

A shared staging buffer **loses** throughput as threads are added — one atomic append offset is a hotter point of coordination than thousands of bucket locks. Queued writes do less than half the work of writing directly, and break read-your-writes semantics. The rule this established shapes the whole module: per-process regions win for *allocation* (the arena uses them — zero atomics on the alloc fast path), but never for staging live entries.

### 5. Modern-kernel memory backing

![memory backing](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/03-memory-backing.png)

OpenSIPS shm today is demand-faulted 4K pages — no `MAP_POPULATE`, no hugepages, no `madvise`, no `mlock` anywhere in `mem/`. A multi-hundred-MB cache pays for that in TLB misses. Four routes to 2M pages, ranked as a runtime fallback ladder:

| route | admin action | 6.8 | 6.12 | chase latency |
|---|---|---|---|---|
| `vm.nr_overcommit_hugepages` + `MAP_HUGETLB` | one sysctl | works, **no reservation held** | works | **177→125 ns (1.42×)** |
| THP-shmem: `shmem_enabled=advise` + `MADV_HUGEPAGE` | one sysfs write | works | works | 177→158 ns |
| `MADV_COLLAPSE` after fill | **none** | works even with `shmem_enabled=never` | EINVAL — needs `advise` | 177→156 ns |
| plain 4K | — | — | — | baseline |

Key findings:
- **Overcommit hugetlb removes the classic reservation objection**: pages are taken from free memory at fault time and returned on exit — nothing is held hostage while the cache is small. Pre-faulting is also 4–5× cheaper at 2M granularity.
- **Every tier is detected at runtime by *trying it*, never by kernel version** — the 6.8/6.12 `MADV_COLLAPSE` divergence proves version checks lie. The module already does this in `mod_init`: it reports the achieved tier and, when hugetlb is unavailable, logs the exact sysctl and the measured cost of running without it.
- Two kernel subtleties learned the hard way (both documented in-tree): **shmem THP requires the VA and the shmem file offset to be congruent mod 2M** (a range VA-aligned inside an unaligned mapping is silently ineligible — `THPeligible: 0`, collapse `EINVAL`; the fix is reserve `PROT_NONE`, then `MAP_FIXED` the shmem at a 2M boundary), and **a shmem `MADV_COLLAPSE` creates the huge folio without PMD-mapping the caller** — verify via the `ShmemHugePages` meminfo delta, not smaps.
- 1 GB pages: ruled out (runtime allocation unobtainable after any uptime, and 2M pages already give a ≤1 GB arena full STLB residency on this class of hardware).
- Swap pinning via `mlock` in `mod_init` works across fork (locks are not inherited, but the pages are shared — one pre-fork lock pins the arena for every worker). Found a production blocker on our own SBCs while checking: systemd's default `LimitMEMLOCK=65536` means `mlock` of any real arena fails — the unit needs a one-line drop-in.

### 6. Expiry

| strategy | per sweep (50k entries, ~13 due) | locks/sweep |
|---|---|---|
| full sweep, lock every bucket | 1.31 ms | 65 536 |
| **per-bucket `min_expires`, unlocked skip** | **0.044 ms** | **13** |
| timer wheel, O(expired) | 0.0005 ms | — |

Even the full sweep is 0.13% of a core — expiry is a *memory reclamation* problem (entries squatting up to `cache_clean_period`), not a CPU problem. The `min_expires` hint gets 30× for zero hot-path cost; the wheel's further 84× buys nothing and costs 74 ns/insert plus 16 B/entry.

## Design in brief

```c
struct pcache_bucket {          /* exactly one cache line - asserted */
    volatile unsigned version;  /* seqlock: even = stable, odd = writer inside */
    gen_lock_t        lock;     /* writers (+ reader fallback) */
    unsigned char     tags[6];  /* 1 byte of hash per slot - rejects ~255/256
                                   of non-matching slots without a deref */
    unsigned short    used:4,   /* slots in use */
                      owner:12; /* holder id, for dead-writer recovery */
    pcache_rec       *slot[6];
};
```

- **Slab arena**: entries live in fixed-size cells inside class-bound chunks that are *never* returned to shm — the invariant the lock-free read path stands on. Byte 0 of every cell is the class id, stamped at chunk-carve time and immutable, which is how a reader clamps a possibly-stale length without aligned chunks. Allocation state is per-process (bump chunk + private free stack per class; no atomics, no shared cache lines on the fast path).
- **Growth** (upcoming): segmented directory + linear hashing — buckets never move, splits happen one bucket at a time, and the routing word is re-checked on a miss. The table finally *grows at runtime* instead of being sized once.
- **No allocator call ever happens under a bucket lock** (`cachedb_local` nests the shm allocator inside bucket locks in five places); records are pre-built before `lock_get`, frees happen strictly after release.
- **Native counters**: `add`/`sub` store an int64 and accumulate fixed-width under the bucket lock; every user-facing read formats them as decimal. No parse/format/realloc in the critical section.
- **Overflow**: full buckets spill to a small chained side table gated by a counter readers check only after a stable miss; a key lives in its bucket or in overflow, never both.

## Script interface

Single-key operations go through the core cache functions unchanged. The module's own multi-key operations are **`perf_`-prefixed with Redis verbs** — deliberately *not* the `cachedb_local` parity names (`cache_remove_chunk` / `fetch_chunk`), so migrating those two calls requires a script change; everything else is drop-in:

```
perf_del("session-*");                          # glob delete -> count
perf_mget("user-*", $avp(k), $avp(v));          # matches -> index-paired AVPs
perf_mget_json("*", $var(j));                   # -> {"hits":"6","user-alice":"a1",...}
```

All three ride one lock-free walker (Redis SCAN-class guarantee) with binary-safe JSON escaping; `iter_keys` uses the same walker. Two startup selftest modparams (`arena_selftest`, `htable_selftest`) ship as permanent diagnostics and fail startup on any mismatch.

## Status

- [x] Module shell, URL/collection parsing (size clamped to [4,24] — `1 << size` on an unbounded unsigned is UB), memory-tier probe with actionable sysctl guidance
- [x] Slab arena (size classes, per-process allocation, donation/refill pools)
- [x] Table core: 64B buckets, SWAR tag scan, seqlock reads with full copy-out validation, versionless TTL bump, overflow
- [x] cachedb vtable: get/set/remove/add/sub/get_counter + native counters; `iter_keys`
- [x] `perf_del` / `perf_mget` / `perf_mget_json`
- [x] Selftests + script-level end-to-end suite
- [ ] Expiry sweep (expired entries are already invisible; their memory is not yet reclaimed)
- [ ] Statistics (per-process sharded — a single shared `update_stat` counter would recreate the 0.72× collapse measured above)
- [ ] Linear-hash growth + maintenance worker
- [ ] Introspection MI (`perf_keys` / `perf_scan` / `perf_dump` / `perf_get` / `perf_set` / `perf_del` / `perf_stats`)
- [ ] Huge-page arena backing (the ladder above) + `mlock` pinning
- [ ] Multi-process correctness suite; end-to-end `th_state_url` benchmark against `cachedb_local` and dialog-based topology hiding

`modules/cachedb_perf/DESIGN.md` and `bench/` are in-tree as working documents for reviewers — full measurement history, every rejected alternative and why; they will be dropped before this leaves draft.
