## Summary

This PR introduces **`cachedb_perf`** — a new, from-scratch `cachedb` backend for large, high-churn local caches, selected by URL scheme (**`perf://`**). I'm trying to build a much faster local cache module by combining a cache-conscious, lock-free-read design (CLHT / MemC3 lineage) with what recent Linux kernels make possible: overcommit hugetlb pools, shmem THP, `MADV_COLLAPSE`, `MADV_POPULATE_WRITE` and swap pinning.

It implements the same `cachedb_funcs` vtable as every other backend, so **any module taking a `cachedb_url` works unchanged**, and core script usage (`cache_store("perf", ...)`) only changes the backend name. v1 is a **single-node in-memory cache** with optional `db_*` persistence — whole collections can be saved to and loaded from an SQL backend, so state survives a restart (see the **DB persistence** section below). What it deliberately does **not** do is `cachedb_local`-style per-operation replication (`cluster_id` write-through); cross-node sharing is instead a shared-DB refresh model (the `perf_sync` cluster-sync below), not a streamed op log.

Draft mainly to land multi-node validation of the cluster-sync path and optional index refinements — but the module is functionally complete (data ops, expiry, runtime growth, statistics, huge-page arena, a full introspection MI, observability events, `db_*` persistence, and a `perf_sync` cluster refresh), validated by built-in selftests, a script-level end-to-end suite, and a multi-process correctness soak.

## Motivation

Found while benchmarking `topology_hiding`'s cacheDB state backend (#4114): setting `cache_collections "th=16"` cut the load balancer's CPU from **45% to 29%** at 4000 CPS. The root cause is structural: `cachedb_local`'s hash table is sized **once** from `cache_collections` and never resized. The default is 512 buckets, most deployments never set the parameter, and at 50 000 entries that is a load factor of ~98 — roughly 50 string compares and 50 dependent cache misses per lookup. The module also exports zero statistics, so the cliff is invisible in production.

Rather than progressively rewriting a module every deployment depends on, this is a clean backend: operators opt in per collection by changing a URL.

## Configuration

Every parameter is optional — with none set you get a single `default` collection at 16384 buckets, plain shm, growth and the expiry sweep on. In practice you declare the collection(s) you use and point consumers at them.

| modparam | type | default | what it does |
|---|---|---|---|
| `cache_collections` | string | `default` (`14` → 16384 buckets) | Declares collections as `name` or `name=size`, `;`-separated. `size` is the log2 of the *initial* bucket count, clamped to `[4, 24]`; the table grows past it at runtime, so it is a starting point, not a ceiling. The `cachedb_local` `/r` replication marker is rejected — this cache is single-node. |
| `cachedb_url` | string | `perf://` (the `default` collection) | Connection URL(s) that scripts and other modules resolve. The collection is the URL's db part (`perf:///th`) or, equivalently, its host part (`perf://th`); `perf://` alone selects `default`. Prefix a group (`perf:grp:///th`) to address a specific URL from the script. Naming an undeclared collection is a startup error. Repeatable. |
| `expiry_sweep_period` | int (seconds) | `1` | How often expired records are *reclaimed*. Expiry itself is instant — an expired entry reads as absent the moment it lapses; the sweep only frees the memory, hint-guided so idle collections cost next to nothing. `0` disables reclamation (expired entries hold their memory until overwritten). |
| `growth_load_factor` | int | `2` | Target entries-per-bucket the maintenance timer grows the table toward — the knob that keeps the ~84 ns bucket shape as the cache scales. `0` disables growth (fixed-size table, i.e. `cachedb_local` behaviour). |
| `growth_budget` | int | `4096` | Cap on bucket splits per maintenance tick, so a single growth pass can never stall the timer. |
| `arena_hugepage_mb` | int (MB) | `0` (off) | Size of the 2 MB huge-page arena reservation (mlock-pinned, created pre-fork, shared by all workers). Chases the best tier by *trying* the hugetlb → THP → `MADV_COLLAPSE` → 4K ladder; `0` uses plain demand-faulted shm. Wants `LimitMEMLOCK=infinity`; warns and continues unpinned otherwise. |
| `arena_selftest` | int | `0` | Run the arena selftest at startup and **fail startup** on any mismatch. Permanent, cheap diagnostic. |
| `htable_selftest` | int | `0` | Run the hash-table + growth selftest at startup and **fail startup** on any mismatch. |
| `db_url` | string | *(unset)* | A `db_*` (SQL) backend to persist collections to (the matching `db_*` module must be loaded). Unset = no persistence. |
| `db_table` | string | `cachedb_perf` | Table holding the persisted rows. |
| `db_mode` | int | `0` (off) | Automatic persistence for `persist_collections`: `1` = load at startup, `2` = load at startup + save on graceful shutdown. `0` = MI-only. |
| `persist_collections` | string | *(none)* | CSV of collections that `db_mode` auto-loads/saves. `perf_save`/`perf_load` still work on any collection on demand. |
| `sync_cluster_id` | int | `0` (off) | Cluster to signal on `perf_sync` (needs `clusterer` loaded + a `db_url`). Soft: with either missing, `perf_sync` degrades to a DB save. |

**The motivating setup — topology_hiding state backend:**

```
loadmodule "cachedb_perf.so"
modparam("cachedb_perf", "cache_collections", "th=16")   # 2^16=65536 buckets to start; grows with call volume
modparam("cachedb_perf", "expiry_sweep_period", 1)
modparam("cachedb_perf", "arena_hugepage_mb", 512)       # optional: 512 MB of 2M huge pages

loadmodule "topology_hiding.so"
modparam("topology_hiding", "th_state_url", "perf:///th")
# force_dialog is independent of this: with force_dialog=1 the dialog
# module holds INVITE calls and the cache serves the non-INVITE dialogs
# (SUBSCRIBE/NOTIFY/OPTIONS/INFO/PUBLISH); with force_dialog=0 INVITE
# state (when session-timed) flows through the cache too.
```

**Generic script cache + glob operations:**

```
modparam("cachedb_perf", "cache_collections", "sessions")
modparam("cachedb_perf", "cachedb_url", "perf:///sessions")   # groupless default -> "sessions"

route {
    cache_store("perf", "session-$ci", "$var(state)", 3600);   # -> "sessions"
    cache_fetch("perf", "session-$ci", $var(state));
    # the collection arg is optional: omitted, the glob ops use the groupless
    # cachedb_url's collection (here "sessions") - the same place cache_store
    # above writes. Pass it explicitly to target another collection.
    perf_del("session-$ci-*", "sessions");                  # glob delete -> count
    perf_mget("session-*", $avp(k), $avp(v), "sessions");   # matches -> index-paired AVPs
}
```

### MI commands

The full management interface — the operator visibility `cachedb_local` never had. Every command is lock-free (seqlock reads), so a key scan or dump never stalls SIP traffic; every name carries the `perf_` prefix to match the script functions and stay clear of the core's bare `get`/`set`. Each is invoked module-namespaced as **`cachedb_perf:<command>`** (OpenSIPS 4.0+). Arguments in `<>` are required, `[]` optional; an omitted `collection` means the groupless `cachedb_url`'s collection (where `cache_store("perf", …)` writes).

| command | arguments | returns / effect |
|---|---|---|
| `perf_stats` | `[collection]` | per-collection stats: entries, buckets, load factor, overflow occupancy, hits/misses/stores/removes/expired/destroyed, hit rate (with a note that reflects the measured value), seqlock retries (and per-1k-reads), plus arena bytes/chunks and the achieved memory tier. No arg = every collection |
| `perf_stats_reset` | `[collection]` | re-baseline the cumulative counters so the next reading covers a fresh interval instead of a lifetime average — useful after a restart, when the miss burst from dialogs older than the cache drags the hit rate down long after it has recovered. The counters are never rewound (each process owns its counter cache line); only a baseline is recorded and the difference reported. Live gauges — entries, buckets, overflow, load factor, arena — are unaffected |
| `perf_keys` | `<glob> [collection] [limit]` | names **and TTL** of keys matching the shell glob, bounded (default limit 1000; the reply flags truncation). The `KEYS` equivalent |
| `perf_scan` | `<cursor> [glob] [count]` | cursor-based incremental iteration (Redis `SCAN`) over the default collection: start at cursor `0`, repeat with the returned cursor until it comes back `0`. `count` bounds the buckets visited per call. The answer for a large cache, where `perf_keys` would truncate |
| `perf_dump` | `<glob> [collection] [limit]` | like `perf_keys` but **includes values** — opt-in, never the default |
| `perf_get` | `<key> [collection]` | one key: its value, remaining TTL (`-1` = never) and size |
| `perf_set` | `<key> <value> [ttl] [collection]` | write one key; `ttl` in seconds (`0` or omitted = never expires) |
| `perf_ttl` | `<glob> <ttl> [collection]` | re-arm the TTL of **every key matching the glob** without rewriting the value (the versionless bump — one atomic `expires` store, readers undisturbed); `ttl` in seconds (`0` = never). Returns the count updated. A literal key matches exactly one |
| `perf_del` | `<glob> [collection]` | delete every key matching the glob; returns the count. The MI face of the `perf_del()` script function |
| `perf_save` | `[collection]` | snapshot a collection to the `db_url` backend (all declared collections if none named) |
| `perf_load` | `[collection]` | restore a collection from the `db_url` backend |
| `perf_sync` | `[collection]` | save to the DB, then signal the cluster to reload it (save-then-broadcast); also a script function |

MI parameters are named, so any sensible subset resolves — e.g. `perf_keys <glob> limit=N` without a collection, or `perf_set <key> <value> collection=C` without a ttl.

Note the argument order: the glob-taking commands (`perf_keys`, `perf_dump`, `perf_del`, `perf_ttl`) take the **glob first** and the collection second, so `perf_dump mycoll` looks for keys *named* `mycoll` rather than dumping that collection — use `perf_dump "*" mycoll`. Only `perf_get`/`perf_set` lead with a key. Quote globs, or the shell expands them before opensips-cli sees them.

```
opensips-cli -x mi cachedb_perf:perf_stats
opensips-cli -x mi cachedb_perf:perf_stats_reset            # fresh interval for the rates
opensips-cli -x mi cachedb_perf:perf_stats_reset th         # just one collection

opensips-cli -x mi cachedb_perf:perf_keys "*"               # every key in the default collection
opensips-cli -x mi cachedb_perf:perf_keys "session-*" th 50 # glob, collection, limit
opensips-cli -x mi cachedb_perf:perf_scan 0                 # then: …:perf_scan <returned-cursor> … until 0
opensips-cli -x mi cachedb_perf:perf_dump "profile-*"       # names AND values
opensips-cli -x mi cachedb_perf:perf_dump "*" th 20         # a sample of one collection

opensips-cli -x mi cachedb_perf:perf_get session-abc123
opensips-cli -x mi cachedb_perf:perf_set greeting hello 300
opensips-cli -x mi cachedb_perf:perf_ttl "session-*" 1800   # re-arm matching keys to 30 min
opensips-cli -x mi cachedb_perf:perf_del "session-abc*"
```

`perf_stats` counters are cumulative since startup (or the last `perf_stats_reset`), so a hit rate read straight after a restart is dominated by sequential requests for dialogs older than the cache and recovers only as those age out. Either reset once the cache has warmed, or — better for monitoring — poll twice and difference the counters.

### Events

Four EVI events let a script or monitor react to the cache. Each is gated by `evi_probe_event()`, so with no subscriber it costs one shared read, and none sit on the lock-free get/set path.

| event | when | parameters |
|---|---|---|
| `E_CACHEDB_PERF_EXPIRED` | a record was reaped (opt-in per collection via `event_expired_collections`) | `collection`, `key` |
| `E_CACHEDB_PERF_NOMEM` | a write was **dropped because the arena is full** | `collection`, `key`, `size` |
| `E_CACHEDB_PERF_GROWN` | the table grew itself | `collection`, `prev_buckets`, `buckets`, `splits`, `entries` |
| `E_CACHEDB_PERF_MEM_DEGRADED` | huge pages requested but the arena landed below hugetlb (once at boot) | `requested_mb`, `tier`, `backing`, `overcommit_pages` |
| `E_CACHEDB_PERF_SYNCED` | this node reloaded a collection because a peer issued `perf_sync` | `collection`, `source_node` |

```
event_route[E_CACHEDB_PERF_NOMEM] {
    xlog("L_ERR", "cachedb_perf full: dropped $param(key) ($param(size) B) in $param(collection)\n");
}
```

### DB persistence

With `db_url` set, a whole collection can be persisted to any `db_*` (SQL) backend. The **DB is a shared, durable store; the cache is an in-memory view over it.** A save is a full snapshot — the collection's rows are deleted and every live entry re-inserted; a load restores them. TTLs are stored as **absolute wall-clock time** so they survive a restart (the cache's own expiry is monotonic ticks, which reset on reboot); already-expired rows are skipped on both save and load, and native counters round-trip as their decimal value. This is single-node durability; cross-node sharing over the same DB is `perf_sync` (below), still **not** per-operation replication.

```
loadmodule "db_mysql.so"
modparam("cachedb_perf", "cache_collections", "sessions")
modparam("cachedb_perf", "db_url", "mysql://opensips:pw@localhost/opensips")
modparam("cachedb_perf", "db_mode", 2)              # load at startup, save on graceful shutdown
modparam("cachedb_perf", "persist_collections", "sessions")
```
```
# on demand, from opensips-cli:
opensips-cli -x mi cachedb_perf:perf_save sessions   # -> {"collections":1,"saved":N}
opensips-cli -x mi cachedb_perf:perf_load sessions   # -> {"collections":1,"loaded":N}
```

The table (default `cachedb_perf`) has four columns: `collection` (string), `pkey` (string), `pvalue` (BLOB, binary-safe), `expires` (int, absolute unix time, `0` = never). The startup load runs before the workers fork, so every worker starts with a warm cache.

> ⚠️ **A save/load is a full, blocking snapshot** — one SQL statement per entry, synchronous in the issuing process. On a large collection (this module targets millions of entries) or a slow backend (`db_text`, `db_sqlite`), it can take a long time and stall that process for its duration. It is a **maintenance / bootstrap** operation — startup warm-up, shutdown flush, an occasional snapshot or a `perf_sync` refresh — **never on a per-request path or a tight timer.** If you need durable per-key writes on every operation, this is the wrong tool.

### Cluster sync

`perf_sync [collection]` (MI **and** script function) builds on the same DB: it saves the collection, then signals the cluster over `clusterer` to reload it — **one message per sync, not per operation**, so the hot path is untouched. A reload **overwrites** a peer's copy from the DB, so it's for single-writer / read-replica topologies (one authority updates the DB, the others refresh); a node reloads and raises `E_CACHEDB_PERF_SYNCED`. With no clusterer / `sync_cluster_id` 0 it degrades to a DB save. Same blocking cost as `perf_save`, so: an occasional refresh, not a live primitive.

```
loadmodule "clusterer.so"          # before cachedb_perf (a soft dep also enforces init order)
modparam("clusterer", "my_node_id", 1)          # 2, 3 on the other nodes
loadmodule "cachedb_perf.so"
modparam("cachedb_perf", "cache_collections", "profiles")
modparam("cachedb_perf", "db_url", "mysql://opensips:pw@dbhost/opensips")
modparam("cachedb_perf", "sync_cluster_id", 1)

# on the authority, after it updated "profiles":
#   opensips-cli -x mi cachedb_perf:perf_sync profiles   # save + tell peers to reload
#   perf_sync("profiles");                                # same, from script

event_route[E_CACHEDB_PERF_SYNCED] {   # fires on each replica after its reload
    xlog("L_INFO", "reloaded $param(collection) from node $param(source_node)\n");
}
```

The capability registers with the clusterer, so it shows in its `clusterer_list_cap` MI — verified on a single-node cluster (with `cachedb_perf` loaded *before* `clusterer`, confirming the soft dependency reorders init):

```
$ opensips-cli -x mi clusterer_list_cap
{
  "Clusters": [
    { "cluster_id": 1,
      "Capabilities": [
        { "name": "cachedb-perf-sync", "state": "Ok", "enabled": "yes" }
      ] } ]
}
```

### Enabling the kernel memory backing

The huge-page arena (`arena_hugepage_mb`) climbs a **detect-by-trying** ladder at `mod_init`: it attempts each tier in turn and keeps the best one the running kernel actually grants — you do **not** pick a tier, you enable what you can and the module reports what it got. The tiers, fastest to slowest (the cost is the isolated 2 MB pointer-chase from §5 of the study):

| tier | kernel feature the module uses | one-time admin action | cost (2M chase) | swap-pinning |
|---|---|---|---|---|
| **1 (fastest)** | overcommit hugetlb pool + `MAP_HUGETLB` | one `sysctl` | **177 → 125 ns (1.42×)** | inherent — hugetlb is unswappable, no `mlock` needed |
| **2** | shmem THP + `MADV_HUGEPAGE` | one `sysfs` write | 177 → 158 ns | via `mlock` (see below) |
| **3** | `MADV_COLLAPSE` after fill | none (kernel ≥ 6.1) | 177 → 156 ns | via `mlock` (see below) |
| **4 (baseline)** | plain demand-faulted 4 KB | — | 177 ns | via `mlock` (still reserved+pinned) |

**Tier 1 — overcommit hugetlb** (the one to prefer: on-demand, nothing held while the cache is small, and no memlock grant needed). Allow enough on-demand 2 MB pages for the arena (`arena_hugepage_mb / 2`, plus a small margin):

```bash
sysctl -w vm.nr_overcommit_hugepages=320          # e.g. a 512 MB arena = 256 pages + margin
echo 'vm.nr_overcommit_hugepages = 320' > /etc/sysctl.d/60-opensips-hugepages.conf
```

**Tier 2 — shmem THP** (used if tier 1 is unavailable). Put shmem THP in `advise` so it honours the module's `MADV_HUGEPAGE`:

```bash
echo advise  > /sys/kernel/mm/transparent_hugepage/shmem_enabled
echo madvise > /sys/kernel/mm/transparent_hugepage/enabled
```

**Tier 3 — `MADV_COLLAPSE`** needs no sysctl (kernel ≥ 6.1); on some 6.12 builds it also wants tier 2's `shmem_enabled=advise`. **Tier 4** is the default and needs nothing.

**Swap-pinning (`mlock`) — tiers 2–4 only.** When tier 1 is unavailable the arena is a regular shared mapping, which the module `mlock`-pins pre-fork so it can't be swapped out from under the lock-free readers. systemd's default `LimitMEMLOCK=65536` (64 KB) makes that `mlock` fail on any real arena — the module then warns and runs **unpinned (swappable)**; the huge pages still form, they are just not pinned. Tier 1 (`MAP_HUGETLB`) is exempt and needs none of this. To pin tiers 2–4, grant it once:

```bash
mkdir -p /etc/systemd/system/opensips.service.d
printf '[Service]\nLimitMEMLOCK=infinity\n' > /etc/systemd/system/opensips.service.d/memlock.conf
systemctl daemon-reload
```

Turn it on and confirm what landed:

```
modparam("cachedb_perf", "arena_hugepage_mb", 512)   # 0 (default) = plain shm, tier 4
```
```bash
opensips-cli -x mi cachedb_perf:perf_stats     # -> memory_tier (1 hugetlb .. 4 plain 4K) + memory_backing
```

`mod_init` also logs the achieved tier and, when it falls short of tier 1, the exact `sysctl` to reach it and the measured cost of running without it.

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
- Two kernel subtleties learned the hard way (both documented in the code comments): **shmem THP requires the VA and the shmem file offset to be congruent mod 2M** (a range VA-aligned inside an unaligned mapping is silently ineligible — `THPeligible: 0`, collapse `EINVAL`; the fix is reserve `PROT_NONE`, then `MAP_FIXED` the shmem at a 2M boundary), and **a shmem `MADV_COLLAPSE` creates the huge folio without PMD-mapping the caller** — verify via the `ShmemHugePages` meminfo delta, not smaps.
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

Caveats: the single load generator is unstable at 100k (some cachedb_perf mid-rungs showed generator-side failures at low LB CPU — discarded); and the huge pages here are whole-shm THP that benefits all three equally — the module's *own* huge-page arena is a separate mechanism, measured on its own in the next section.

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

The same walker backs the **introspection MI** (full command table in the **MI commands** section above) — the operator visibility `cachedb_local` never had, and lock-free so a key scan never stalls SIP traffic. `perf_scan` is the answer for a large cache where `perf_keys` would truncate: its cursor is an ascending bucket index, so it stays valid across a concurrent resize and returns every entry present throughout at least once — without Redis's reverse-binary cursor masking, because the table only grows (buckets never move).

## Status

- [x] Module shell, URL/collection parsing (size clamped to [4,24] — `1 << size` on an unbounded unsigned is UB), memory-tier probe with actionable sysctl guidance
- [x] Slab arena (size classes, per-process allocation, donation/refill pools)
- [x] Table core: 64B buckets, SWAR tag scan, seqlock reads with full copy-out validation, versionless TTL bump, overflow
- [x] cachedb vtable: get/set/remove/add/sub/get_counter + native counters; `iter_keys`
- [x] `perf_del` / `perf_mget` / `perf_mget_json`
- [x] Selftests + script-level end-to-end suite
- [x] Expiry sweep — hint-routed (per-bucket min-expires hints in sweep-friendly parallel arrays, 16 per cache line; the hot TTL-bump path never writes them), timer-driven via `expiry_sweep_period` (default 1 s), reclamation through the global pool strictly after lock release
- [x] Statistics — per-process sharded counters (one 64-byte line per process, summed only at read time; a shared `update_stat` counter would recreate the 0.72× collapse measured above), exported as ten `cachedb_perf:` core stats and a per-collection `perf_stats` MI (load factor, overflow, seqlock retries/1k, backing tier, `expired`/`destroyed`, and a hit rate whose accompanying note follows the measured value rather than asserting a verdict). `perf_stats_reset` re-baselines the cumulative counters for a fresh measurement interval without a restart, leaving live gauges alone
- [x] Linear-hash growth + maintenance timer — the table now resizes itself (the thing `cachedb_local` fundamentally cannot do): one-bucket-at-a-time splits driven from the single-process maintenance timer, no rehash, overflow left findable; `growth_load_factor` keeps the bucket shape as entries scale. Verified: 1000 entries → 484 splits → 500 buckets, all keys intact
- [x] Introspection MI — `perf_keys` / `perf_scan` / `perf_dump` / `perf_get` / `perf_set` / `perf_ttl` / `perf_del` as MI commands, all lock-free (a key scan never stalls writers, unlike `cachedb_local`'s). `perf_scan` is cursor-based (Redis SCAN): an ascending bucket cursor, stable across a concurrent resize, every entry returned at least once. Verified over a datagram MI
- [x] Observability events (EVI) — `E_CACHEDB_PERF_EXPIRED` (per reaped key, opt-in per collection), `E_CACHEDB_PERF_NOMEM` (a write dropped because the arena is full), `E_CACHEDB_PERF_GROWN` (a table resized, with the before/after span), `E_CACHEDB_PERF_MEM_DEGRADED` (huge pages requested but the arena landed below hugetlb). Each `evi_probe_event()`-gated (free with no subscriber) and off the hot path; verified end-to-end over `event_route`s
- [x] Huge-page arena backing — 2M-aligned mlock-pinned reservation via the detect-by-trying ladder (`arena_hugepage_mb`), lock-free bump from it, shm_malloc fallback; measured +7–13% (see above)
- [x] Multi-process correctness soak — forked worker processes hammer one live backend (get/set/remove/add) while the maintenance timer splits buckets underneath them, checking four invariants: no torn read, no lost update, no lost key across splits, no crash/UAF. **Found and fixed a real fork-safety bug** (see below). Post-fix: 8 processes, 24M ops, 3093 concurrent splits, 0 crashes, `torn_reads=0`, counter sum == adds, all immortals intact; clean under the `Q_MALLOC_DBG` redzone allocator and under all three core allocators (`F_MALLOC` / `Q_MALLOC` / `HP_MALLOC`, driving both pkg and the arena's shm chunk backing)
- [x] End-to-end `th_state_url` benchmark against `cachedb_local` (50k held calls) and against dialog-based topology hiding (100k held calls) — both sections above
- [x] DB persistence — whole-collection save/load to any `db_*` backend (`perf_save`/`perf_load` MI, plus `db_mode` startup-load / shutdown-save), TTLs kept as absolute wall-clock time so they survive a restart. Verified with `db_sqlite` (save → shutdown-save → startup-load, values intact, TTL decremented correctly across the cycle). Rows whose wall-clock expiry has passed are dropped at load rather than merely skipped — otherwise, with `db_mode=1` or after any shutdown that was not graceful, dead rows accumulate in the table indefinitely. Single-node durability, not replication
- [x] **Cluster sync (`perf_sync`)** — MI command + script function that saves this node's collection to the DB, then signals peers over the `clusterer` API to reload it (one message per *sync*, not per operation); each peer reloads from the DB and raises `E_CACHEDB_PERF_SYNCED`. Soft dependency — degrades to a DB save with no broadcast if clusterer/`sync_cluster_id` is absent. A pull-from-DB refresh model for single-writer/read-replica topologies — deliberately *not* `/r`-style per-operation replication. Verified on a single-node cluster: the capability registers and lists in the clusterer's `clusterer_list_cap` MI (`cachedb-perf-sync`, state Ok), a soft `DEP_SILENT` clusterer dependency reorders init so it works regardless of load order, and `perf_sync` degrades cleanly with no clusterer — no crashes. The multi-node broadcast→reload fan-out follows the `ratelimit` clusterer pattern and awaits a two-node run

### Correctness: what the multi-process soak caught

A lock-free read path plus a table that resizes itself under live traffic is exactly the kind of code where a single-process selftest passes and production still corrupts memory. So the soak (`bench/cdbstress.c`) runs the real thing: 8 worker **processes** on one shared backend, a get/set/remove/add mix, with the maintenance timer splitting buckets the whole time. Every value is written all-bytes-equal so a torn read is visible; counters are hammered with `add(+1)` so a lost update shows as a shortfall; a set of keys is inserted once and never removed so a split that drops one shows as a miss.

It failed inside a second — a segfault on an *impossible* size class (88) read out of a cell's class byte. Root cause: after `fork()` every child holds a copy-on-write copy of the parent's private allocator hoard (same bump pointer, same free-list cell addresses), and `pcache_arena_child_init` had each child *donate* that hoard to the global pool. The identical physical cells were enqueued once per child, popped by several processes at once, and written through concurrently — one process's value byte landed on another's class id. The fix: a child drops its inherited copy and carves its own chunk on first use, never donating cells it doesn't own. After it, the full soak is clean — 24M ops, 3093 concurrent splits, no torn reads, counter sum equals total adds, every immortal key intact — and equally clean under the `Q_MALLOC_DBG` redzone allocator and under each of OpenSIPS' three core allocators (`F_MALLOC`, `Q_MALLOC`, `HP_MALLOC`), which back both the per-process state and the arena's shm chunk allocation.

### And what production caught that the soak did not

The soak runs a single collection with generous TTLs, so it never combined
overflow chaining with expiry reclamation — and that combination was the one
that mattered. On a live SBC the module crashed repeatedly inside the arena's
free path, on an impossible size class, with topology-hiding keys going
missing.

`struct povf`, the overflow node, had its `next` pointer at **offset 0** — but
byte 0 of every arena cell is the size class, read by both free paths. Linking
a node wrote the pointer's low byte over the class id (0x40/0x80/0xC0 →
"class" 64/128/192), indexing past the 21-entry class table and corrupting the
pool, so the crash surfaced far from the cause. It needs overflow *and* the
expiry sweep together to trigger, which is why a table with room to spare never
showed it.

The fix reserves byte 0 in `struct povf` and hardens both free paths to log and
leak on an invalid class rather than corrupt. Reproduced deliberately
afterwards — a churn set with a 2 s TTL, a 1 s sweep and growth enabled catches
it in under a minute — and the soak was extended to cover the same shape.

The standalone rig behind every figure above lives in `modules/cachedb_perf/bench/` (`make run`, no OpenSIPS build needed) — full measurement history, every rejected alternative and why — so the numbers here are reproducible rather than asserted.


