## Summary

This PR introduces **`cachedb_perf`** — a new, from-scratch `cachedb` backend for large, high-churn local caches, selected by URL scheme (**`perf://`**). I'm trying to build a much faster local cache module by combining a cache-conscious, lock-free-read design (CLHT / MemC3 lineage) with what recent Linux kernels make possible: overcommit hugetlb pools, shmem THP, `MADV_COLLAPSE`, `MADV_POPULATE_WRITE` and swap pinning.

It implements the same `cachedb_funcs` vtable as every other backend, so **any module taking a `cachedb_url` works unchanged**, and core script usage (`cache_store("perf", ...)`) only changes the backend name. v1 is deliberately a **single-node in-memory cache**: no clusterer replication, no restart persistency — deployments sharing state via `cachedb_local` + `cluster_id` are out of scope for now.

Draft because the roadmap below is about half done — but the module already runs as a complete cache, validated by built-in selftests and a script-level end-to-end suite.

## Motivation

Found while benchmarking `topology_hiding`'s cacheDB state backend (#4114): setting `cache_collections "th=16"` cut the load balancer's CPU from **45% to 29%** at 4000 CPS. The root cause is structural: `cachedb_local`'s hash table is sized **once** from `cache_collections` and never resized. The default is 512 buckets, most deployments never set the parameter, and at 50 000 entries that is a load factor of ~98 — roughly 50 string compares and 50 dependent cache misses per lookup. The module also exports zero statistics, so the cliff is invisible in production.

Rather than progressively rewriting a module every deployment depends on, this is a clean backend: operators opt in per collection by changing a URL.

## The study

Everything below was measured, not assumed — the benchmark rig ships in-tree (`modules/cachedb_perf/bench/`, `make run`, no OpenSIPS build needed) and every figure is reproducible. Hosts: Xeon E5-2699 v4, kernels 5.4 / 6.8 / 6.12; the NUMA numbers come from a vNUMA-pinned two-socket guest on the same silicon. The rig models structures and cache behaviour (single process, threads); it ranks designs rather than predicting server throughput.

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
- **Every tier is detected at runtime by *trying it*, never by kernel version** — the 6.8/6.12 `MADV_COLLAPSE` divergence proves version checks lie, and they lie in both directions: a later 6.12 (6.12.96, Debian 13) collapses fine with `shmem_enabled=never` again — same major version, opposite behaviour, and the probe silently got the better tier. The module already does this in `mod_init`: it reports the achieved tier and, when hugetlb is unavailable, logs the exact sysctl and the measured cost of running without it.
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

### 7. NUMA — measured on a pinned two-socket testbed

| binding | dependent pointer chase |
|---|---|
| local (same socket) | 146.5 ns |
| remote (cross-socket) | 194.6 ns (**+33%**) |

Measured in-guest on a Proxmox VM with vNUMA bound per host socket (`numaN: ...,hostnodes=N,policy=bind`, dual E5-2699 v4 host — plain `numa: 1` without `hostnodes` fabricates topology over one memory domain and measures nothing). Two consequences, both already reflected in the design rather than motivating changes:

- Cross-socket **reads** of a shared cache cannot be sharded away — a worker reading an entry written on the other socket pays the remote latency however memory is partitioned; only replication avoids it. So the table is deliberately not NUMA-sharded (and §2 shows there is no lock contention for sharding to relieve either).
- The **write** side is node-local by construction: the arena's per-process chunk ownership means each worker faults — and therefore first-touch places — its own records on its own node.

Where NUMA does matter for the roadmap: page walks against remote memory amplify TLB-miss cost, so the huge-page backing is expected to be worth *more* on two sockets than the 1.42× measured on one — to be quantified in the end-to-end benchmark. One refinement from the same testbed: with `pdpe1gb` exposed, 1 GB pages *are* allocatable at runtime on a fresh boot (2 granted right after boot) — the earlier "unobtainable" holds only once uptime fragments memory. The ruling against them stands on arithmetic: 2 M pages already give a ≤1 GB arena full TLB coverage on this hardware.

## Measured: cachedb_perf vs cachedb_local, in-process

The bench rig above ranks *designs* in a single process. This measures the **real modules** — real OpenSIPS 4.1-dev, real shared memory, N real worker **processes** — driving the `th_store` access pattern (16-byte thid keys, 200-byte values, 95% get / 5% set). Both backends did byte-for-byte identical work (same 22.8 M hit count). Release build (`-O3`), `Q_MALLOC`, pinned to one 8-core socket.

![throughput](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/cp17-throughput.png)

**Same conditions — both collections sized to 65536 buckets (`th=16`):**

| condition | cachedb_perf | cachedb_local | perf faster |
|---|---|---|---|
| no load (near-empty), 8 workers | **29.7 Mops/s** (271 ns/op) | 9.9 Mops/s (812 ns) | **3.0×** |
| 50 000 resident, 8 workers | **18.0 Mops/s** (448 ns/op) | 7.9 Mops/s (1013 ns) | **2.3×** |
| 50 000 resident, 1 worker | 1.8 Mops/s (558 ns) | 1.2 Mops/s (853 ns) | 1.5× |

![scaling and cliff](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/cp17-scaling-cliff.png)

Two effects drive the gap. **Scaling:** cachedb_perf's lock-free reads scale **10.0×** from 1→8 workers vs cachedb_local's **6.6×** (it takes a bucket lock on every read). **The default:** most deployments never set `cache_collections`, so cachedb_local runs at its 512-bucket default — at 50k entries that is a load factor of ~98, **3529 ns/op**, and cachedb_perf is **7.8× faster** than cachedb_local as typically shipped.

| at 50 000 resident, 8 workers | ns per operation |
|---|---|
| cachedb_perf (65536 buckets) | **448 ns** |
| cachedb_local, tuned (65536 buckets) | 1013 ns |
| cachedb_local, default (512 buckets) | 3529 ns |

Honest notes: numbers include a real `pkg_malloc`+`free` of the 200-byte value on every get (the th_store copy-out), so this is per-operation cost, not a bare lookup. This deliberately isolates the **cache** from the SIP layer. In a full LB the per-call cost is SIP parsing, header manipulation and transaction state plus the th_store put/get — there is no per-call encryption (th_store values are stored in the clear; the only crypto is one cheap MD5 to derive the key), so the cache is a direct share of that cost. An end-to-end run under 50 000 held calls confirms the direction below.

### End-to-end: TH under 50 000 held calls

Same LB, topology_hiding with `th_state_url` pointing at each backend (65536 buckets), ramped while holding ~50 000 concurrent calls. "Sustained" means <5% failures **and** peak concurrency ≤75k (actually still holding 50k, not backlogging):

| offered CPS | th + cachedb_perf | th + cachedb_local |
|---|---|---|
| 4000 | 3875 achieved, 3.0% fail, 54k held | 3941 achieved, 1.4% fail, 57k held |
| 6000 | **5775 achieved, 3.7% fail, 68k held** ✓ | 5490 achieved, 8.5% fail, 94k (backlogging) ✗ |
| 8000 | 6670, 16.6% fail (overloaded) | 5874, 26.6% fail (overloaded) |

cachedb_perf **sustains the 6000-CPS rung where cachedb_local breaks** — a sustained-ceiling lift from ~3941 to ~5775 CPS (**~1.5×**) at 50k live th_store states. The end-to-end gain is smaller than the isolated-cache 2.3–7.8× because SIP processing is the larger share of per-call cost, but it lands exactly where the cache matters: the high-concurrency point where cachedb_local's lock-on-every-read serializes the workers.

### At 100 000 concurrent calls: cachedb_perf-TH vs dialog-TH vs no-TH

Pushing to **100 000 held calls**, comparing three topology-hiding strategies on the same LB (huge pages / THP enabled): cachedb_perf-backed th_store, the in-memory dialog module (`force_dialog`), and plain record-routing with no topology hiding at all.

![100k three-way](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/conc100k-th-compare.png)

| offered CPS (≈100k held) | no-TH (rr) | cachedb_perf-TH | dialog-TH |
|---|---|---|---|
| 2000 | 55% CPU, 0% fail | 67% CPU, 2.9% | 85% CPU, 0.1% |
| 3000 | 73% CPU, 0.1% | — | **100% CPU**, 1.0% |
| 4000 | 96% CPU, 0.1% | 93% CPU, 1.3% | 100% CPU, **8.2% (breaks)** |

At 100k concurrency **cachedb_perf-TH is nearly as cheap as doing no topology hiding at all** — it tracks the no-TH curve and holds 4000 CPS at 93% CPU. **dialog-TH is the loser here**: it saturates CPU by 3000 CPS, breaks at 4000, and carries ~2.5× the resident memory (a full per-dialog state machine + timers vs one compact th_store entry). This is a crossover from lower concurrency, where dialog leads — cachedb_perf's flat per-entry cost wins as the live-state count climbs.

Caveats: the single load generator is unstable at 100k (some cachedb_perf mid-rungs showed generator-side failures at low LB CPU — discarded); and the huge pages here are whole-shm THP that benefits all three equally — the module's *own* huge-page arena (next) is separate work.

### Huge-page arena (CP-20)

The arena can now back its chunks with **2 MB huge pages** instead of 4 K (modparam `arena_hugepage_mb`; the reservation is 2M-aligned, mlock-pinned, created pre-fork and shared by all workers). Measured on the real module (−O3, 8 workers, medians of repeated runs):

![CP-20 huge pages](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/cp20-hugepages.png)

| condition | 4K pages | huge pages | gain |
|---|---|---|---|
| near-empty (clustered working set) | 31.7 Mops/s | 33.8 Mops/s | +7% |
| 50 000 resident (~13 MB working set) | 21.2 Mops/s | 24.0 Mops/s | **+13%** |

The gain is larger at 50k, where the working set spreads across enough memory to thrash the 4K TLB — exactly the case huge pages relieve. It's below the 1.19–1.43× the pointer-chase showed in isolation because each operation also pays for the hash, the tag scan and a copy of the value. Detection is by *trying* each tier (hugetlb → THP → collapse → 4K), never by kernel version; `mlock` wants `LimitMEMLOCK=infinity` and warns-and-continues otherwise.

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
- **Growth**: segmented directory + linear hashing — buckets never move, splits happen one bucket at a time (driven from the single maintenance timer), and the routing word is re-checked on a miss. The table **grows at runtime** instead of being sized once — the load-factor cliff that motivates this whole module cannot form.
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
- [x] Expiry sweep — hint-routed (per-bucket min-expires hints in sweep-friendly parallel arrays, 16 per cache line; the hot TTL-bump path never writes them), timer-driven via `expiry_sweep_period` (default 1 s), reclamation through the global pool strictly after lock release
- [x] Statistics — per-process sharded counters (one 64-byte line per process, summed only at read time; a shared `update_stat` counter would recreate the 0.72× collapse measured above), exported as ten `cachedb_perf:` core stats and a per-collection `perf_stats` MI (load factor, overflow, seqlock retries/1k, backing tier)
- [x] Linear-hash growth + maintenance timer — the table now resizes itself (the thing `cachedb_local` fundamentally cannot do): one-bucket-at-a-time splits driven from the single-process maintenance timer, no rehash, overflow left findable; `growth_load_factor` keeps the bucket shape as entries scale. Verified: 1000 entries → 484 splits → 500 buckets, all keys intact
- [ ] Introspection MI (`perf_keys` / `perf_scan` / `perf_dump` / `perf_get` / `perf_set` / `perf_del` / `perf_stats`)
- [x] Huge-page arena backing — 2M-aligned mlock-pinned reservation via the detect-by-trying ladder (`arena_hugepage_mb`), lock-free bump from it, shm_malloc fallback; measured +7–13% (see above)
- [x] Multi-process correctness soak — forked worker processes hammer one live backend (get/set/remove/add) while the maintenance timer splits buckets underneath them, checking four invariants: no torn read, no lost update, no lost key across splits, no crash/UAF. **Found and fixed a real fork-safety bug** (see below). Post-fix: 8 processes, 24M ops, 3093 concurrent splits, 0 crashes, `torn_reads=0`, counter sum == adds, all immortals intact; clean under the `Q_MALLOC_DBG` redzone allocator
- [ ] End-to-end `th_state_url` benchmark against `cachedb_local` and dialog-based topology hiding

### Correctness: what the multi-process soak caught

A lock-free read path plus a table that resizes itself under live traffic is exactly the kind of code where a single-process selftest passes and production still corrupts memory. So the soak (`bench/cdbstress.c`) runs the real thing: 8 worker **processes** on one shared backend, a get/set/remove/add mix, with the maintenance timer splitting buckets the whole time. Every value is written all-bytes-equal so a torn read is visible; counters are hammered with `add(+1)` so a lost update shows as a shortfall; a set of keys is inserted once and never removed so a split that drops one shows as a miss.

It failed inside a second — a segfault on an *impossible* size class (88) read out of a cell's class byte. Root cause: after `fork()` every child holds a copy-on-write copy of the parent's private allocator hoard (same bump pointer, same free-list cell addresses), and `pcache_arena_child_init` had each child *donate* that hoard to the global pool. The identical physical cells were enqueued once per child, popped by several processes at once, and written through concurrently — one process's value byte landed on another's class id. The fix: a child drops its inherited copy and carves its own chunk on first use, never donating cells it doesn't own. After it, the full soak is clean — 24M ops, 3093 concurrent splits, no torn reads, counter sum equals total adds, every immortal key intact — and equally clean under the `Q_MALLOC_DBG` redzone allocator.

`modules/cachedb_perf/DESIGN.md` and `bench/` are in-tree as working documents for reviewers — full measurement history, every rejected alternative and why; they will be dropped before this leaves draft.
