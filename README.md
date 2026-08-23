# usrloc pull-sharing cluster

A new OpenSIPS user-location cluster mode — `working_mode_preset
"pull-sharing-cluster"` — built on the flh stack: **cachedb_perf** cross-node
pulls over the stock **clusterer** bin transport, with an optional SQL ledger.
This branch (`feature/usrloc-pull-sharing-devel`) stacks on the
`feature/cachedb-perf-devel` branch (upstream PR #4118) and the devel tree.

**Status:** feature-complete and bench-proven at 1M contacts; 136 rig checks
green across 7 suites; pending upstream after #4118 merges.

## The idea

Every existing usrloc cluster mode either replicates eagerly (full-sharing:
every REGISTER broadcast to every node) or centralizes (federation/cachedb:
one remote store on the SIP path). Pull-sharing does neither:

* **A REGISTER is a purely local event.** The accepting node stores the
  contact in its own memory, stamps itself as owner, and tells nobody.
* **Other nodes learn on lookup.** A miss suspends the SIP transaction
  (`async(lookup(...))`), asks the cluster, absorbs the answer into local
  memory as a normal record living to its natural expiry, and resumes.
  Each node converges toward the full registered set through the traffic it
  actually serves — the pull rate decays to zero as it gets there.
* **Responsibility never converges, only data does.** Exactly one node — the
  owner — pings a contact, maintains its DB row, and (with the HELD protocol
  below) answers pulls for it with the value. Ownership moves contact-by-
  contact when a device re-registers elsewhere; deletes and moved bindings
  are the only broadcasts, because those are the only events remote copies
  must not outlive.
* **The cluster keeps every record safe.** No entry is ever dropped because
  a node died or restarted; survivors serve everything they hold to natural
  expiry. Repair rides the registration refresh interval — the universal
  repair clock — and the ledger makes it instant where configured.

## The moving parts

| Piece | What it does |
|---|---|
| `usrloc` mode `CM_PULL_SHARING` | ownership predicate (socket / `_onid` under anycast), blob publish/absorb, invalidation, ledger rules, orphan adoption, shared-tag takeover |
| `cachedb_perf` CP-15 pull | the cross-node ask: broadcast or owner-hinted, 64k-scale slot pool, negative cache, orphan/late-answer store, `perf_cluster_size` |
| `registrar` async lookup | `async(lookup("location"), resume)` — a miss suspends on the pull's eventfd; no worker ever blocks |
| SQL ledger (optional) | natural-key `(username, domain, contact)` upsert table: backup (write-back, eager deletes), restart bootstrap with expired-row sweep, lookup of last resort |
| HELD protocol (optional) | `pull_authoritative_serve`: only the owner answers a pull with the value; passive holders answer a compact "held" and are asked directly only when the owner is gone |

```mermaid
flowchart LR
    subgraph node1["every cluster node"]
        REG["registrar<br/>async lookup"] --> UL["usrloc<br/>pull-sharing mode"]
        UL --> CP["cachedb_perf<br/>ul collection + pull"]
    end
    CP <-- "pulls over bin (clusterer)" --> PEERS[("peer nodes")]
    UL -- "write-back upserts, eager deletes" --> DB[("SQL ledger<br/>optional")]
```

## How a pull works

A cold lookup suspends its transaction and asks the cluster; with
`pull_authoritative_serve` on, only the owner ships the bytes:

```mermaid
sequenceDiagram
    participant UA as caller
    participant N3 as node 3 (cold)
    participant N1 as node 1 (owner)
    participant N2 as node 2 (passive holder)
    UA->>N3: OPTIONS / INVITE alice
    Note over N3: miss - transaction suspends,<br/>no worker blocked
    N3->>N1: PULL alice
    N3->>N2: PULL alice
    N1-->>N3: value (authoritative)
    N2-->>N3: held
    Note over N3: absorb into local memory -<br/>the next lookup is a 0.46 ms local hit
    N3->>UA: 302 / routed
```

A held answer proves the record exists, so a dead or restarted-empty
owner costs one extra LAN round trip instead of a failure - even before
the clusterer notices the node is gone:

```mermaid
sequenceDiagram
    participant N3 as node 3
    participant N1 as node 1 (owner, crashed)
    participant N2 as node 2 (passive holder)
    N3--xN1: PULL bob
    N3->>N2: PULL bob
    N2-->>N3: held
    Note over N3: no value on offer, but a holder exists:<br/>one forced targeted ask
    N3->>N2: PULL bob (force)
    N2-->>N3: value (passive copy)
```

## The async lookup

The registrar exports `lookup()` as an asynchronous command, so the script
form is just:

```
async(lookup("location"), lookup_resume);
```

What happens underneath:

* If a live local record answers the request — the common case once a node
  has converged — the lookup completes inline: no suspension, no extra
  cost over the plain synchronous `lookup()`.
* On a miss, usrloc starts a cluster pull (owner-hinted when an expired
  local copy names the last owner) and hands the async framework the
  pull's **eventfd**. The SIP transaction suspends; **no worker is held**.
  The eventfd exists per pull slot and is created before the fork, which
  is what lets the reply land in whichever process the transport picked
  and still wake the one that asked.
* When the answer (or the `pull_timeout_ms` deadline) fires, a worker
  resumes the transaction: it collects the pull, absorbs the record into
  local memory, and runs the normal lookup. The resume route replies —
  guard with `t_check_trans()` as in the example above.
* Everything that cannot suspend falls back to the synchronous path
  transparently — other cluster modes, branch-AoR lookups, a pull that
  cannot start — so the script never needs a second code path.

Why it matters: a *blocking* pull occupies a worker for up to
`pull_timeout_ms`, and pull replies themselves need a worker to be
processed. Under a miss storm every worker can end up blocked in a wait
whose answer is stuck behind it in the queue — the classic reply-starvation
collapse. The async form removes that coupling entirely; combined with the
rule that **REGISTER never pulls** (a registration is stored locally and
never consults the cluster), the bench filled 1M contacts at 5,000
REGISTER/s with pulls enabled everywhere and lost nothing.

One deliberate exception: the held-only *forced re-ask* (previous section)
runs inside the resuming worker, blocking it for at most one more
`pull_timeout_ms` — a bounded price paid only when a record's owner is
gone, against a holder that proved alive milliseconds earlier.

## Minimal configuration

```
loadmodule "clusterer.so"
modparam("clusterer", "my_node_id", 1)

loadmodule "cachedb_perf.so"
modparam("cachedb_perf", "cache_collections", "ul=14")
modparam("cachedb_perf", "sync_cluster_id", 1)
modparam("cachedb_perf", "pull_on_miss", 1)
modparam("cachedb_perf", "pull_transport", "bin")
modparam("cachedb_perf", "pull_max_value", 8192)
modparam("cachedb_perf", "replicate_collections", "ul")

loadmodule "usrloc.so"
modparam("usrloc", "working_mode_preset", "pull-sharing-cluster")
modparam("usrloc", "location_cluster", 1)
modparam("usrloc", "cachedb_url", "perf://ul")
modparam("usrloc", "db_url", "mysql://opensips:pwd@10.0.0.5/opensips")  # optional

loadmodule "registrar.so"

route {
    if ($rm == "REGISTER") { save("location"); exit; }
    async(lookup("location"), lookup_resume);
}
route[lookup_resume] {
    if ($rc > 0) { t_reply(302, "Moved"); exit; }
    t_reply(404, "Not Found"); exit;
}
```

The preset arbitrates fine-tuning knobs instead of silently ignoring them:
in-envelope refinements (`sql_write_mode write-through`,
`restart_persistency none`) are honored, contradictions are startup errors.
Without `db_url` the mode runs as a pure-pull cluster.

## Tuning for async

| Knob | Default | Async guidance |
|---|---|---|
| `pull_slots` | 64 | size for the concurrent miss burst (peak miss rate × timeout), not the worker count; each slot costs one pre-fork eventfd **in every process**, so raise `open_files_limit` to match |
| `pull_timeout_ms` | 50 | 50 is priced for a blocked worker; async holds only a suspended transaction, so 200–500 is nearly free and converts loss into tail latency |
| `pull_authoritative_serve` | 0 | enable once **every** node runs this build: one value per pull instead of one per holder; degraded case costs one extra LAN round trip |

## Numbers (3 nodes, one 16-core/16GB host, 1,000,000 contacts)

* Fill: 1M REGISTERs at 5,000/s aggregate with pulls enabled everywhere —
  zero loss, zero livelock (REGISTER never touches the pull machinery).
* Convergence: a cold node absorbed the full 1M set at exactly the offered
  lookup rate (3,000/s, 337 s).
* Per-request SIP round-trip: **cold cross-node pull p50 1.4 ms** (p95 9 ms,
  p99 37 ms); **warm local hit p50 0.46 ms** (p99 0.78 ms), flat at every
  table size — the steady-state cost is size-independent.  The cold *mean*
  sits near 2 ms for the first ~600k pulls and drifts to ~6 ms late in the
  sweep: queueing under the sustained bulk pull, not record-get cost, and
  absorption never dropped below the offered rate.
* Tuned run bookkeeping: pulls requested == received == stored == entries ==
  1,000,000; timeouts, slot starvation, orphans, misses all zero.
* Warm re-sweep: 1M lookups, 100% answered locally, zero further pulls.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/pull-sharing/convergence-dark.svg">
  <img alt="Entries held on the cold node versus time: a straight climb to 1,000,000 at 337 seconds, tracking the offered 3,000/s" src="doc/pull-sharing/convergence-light.svg">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/pull-sharing/latency-dark.svg">
  <img alt="Per-request round-trip across the sweep: cold pull mean ~2 ms with p95 band, drifting up late in the sweep; warm local hits flat at 0.46 ms" src="doc/pull-sharing/latency-light.svg">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/pull-sharing/throughput-dark.svg">
  <img alt="Pulls absorbed per second versus entries already held: flat at the offered 3,000/s from zero to one million entries" src="doc/pull-sharing/throughput-light.svg">
</picture>

## Memory: HG_MALLOC v3 vs F_MALLOC

Same binary, same 1M protocol (fill 2×500k at 2,500 reg/s, cold sweep of
the third node at 3,000 lookups/s, warm re-sweep, then every record
allowed to expire), run once with `-a F_MALLOC -m 3072` and once with the
elastic `-a HG_MALLOC -m 512:3072` (v3, no auto-scaling profile). Numbers
are `shmem` real-used unless stated; "puller" is the cold node after it
absorbed all 1M records plus their cached blobs.

| | F_MALLOC | HG_MALLOC v3 |
|---|---|---|
| owner at 500k contacts (real used) | 914 MB | 893 MB |
| puller at 1M pulled (real used) | 1,953 MB | 1,761 MB (−10%) |
| of which allocator overhead (real − used) | 643 MB | 396 MB |
| mapped / committed to hold that | 3,072 MB fixed | 2,336 MB, grown in 114 × 16 MB steps |
| still used after all 1M expired (owner / puller) | 236 / 448 MB | 238 / 450 MB |
| commit after expiry | 3,072 MB | 1,110 / 2,334 MB, shrinking in granules |
| REGISTER p50 / p99 | 0.64 / 1.2 ms | 0.50 / 5.4 ms |
| cold pull p50 / p95 / p99 | 1.5 / 8.9 / 34 ms | 1.35 / 16 / 60 ms |
| warm hit p50 / p99 | 0.44 / 0.78 ms | 0.42 / 15 ms |
| requests lost during the sweeps | 0 | 210 (one stall overflowed the SIP socket) |

Read across: HG_MALLOC v3 holds the same data in ~10% less memory — most
of the difference is allocator overhead (F_MALLOC's split fragments cost
643 MB on the puller against 396 MB) — and commits only what the load
needs, about 1.3× of use, instead of a 3 GB pool mapped up front; after
the mass expiry it starts handing granules back even without an
auto-scaling profile. Both allocators return the freed records
themselves: a quarter of the peak remains in use on each (the grown hash
tables and the blob arena's chunks, which never shrink by design). The
price, on this branch of v3, is the latency tail: growth commits and
garbage-collection passes run inside the allocating worker's critical
section, so p99 is 20× F_MALLOC's on warm hits and one late-sweep stall
of ~0.5 s dropped 212 datagrams at n3's receive queue. Two operational
findings came with it: growth silently refuses when the arena could not
be pinned at start (run with `LimitMEMLOCK=infinity`, or the container
with `--ulimit memlock=-1`), and GC cadence is not a knob. Ideas for
closing the tail are filed with the allocator work; the footprint and
elasticity advantages stand today.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/pull-sharing/mem-used-dark.svg">
  <img alt="Shared memory really used versus contacts held, owner and pulling node, HG_MALLOC v3 against F_MALLOC" src="doc/pull-sharing/mem-used-light.svg">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/pull-sharing/mem-elastic-dark.svg">
  <img alt="HG_MALLOC v3 elastic arena: committed size growing in 16 MB granules just ahead of use, against F_MALLOC's fixed 3 GB pool" src="doc/pull-sharing/mem-elastic-light.svg">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/pull-sharing/mem-retention-dark.svg">
  <img alt="Peak shared memory versus what is still used after every record expired, per allocator and node role" src="doc/pull-sharing/mem-retention-light.svg">
</picture>

### With a separate cache arena

`arena_hugepage_mb` gives cachedb_perf its own reservation outside
OpenSIPS shared memory: the whole size is mapped and pinned at start-up,
walking a tier ladder — `MAP_HUGETLB` 2M pages, THP via `MADV_HUGEPAGE`,
THP via `MADV_COLLAPSE`, plain 4K — to whatever the host can deliver at
that moment (reported per node as `cachedb_perf:memory_tier_active`).
The cache's chunks are bump-allocated from it and never handed back;
once it is full, further chunks fall back to `shm_malloc`, so an
undersized arena degrades into the no-arena behaviour rather than
failing. The same 1M protocol was re-run with a 1,024 MB arena on every
node (`arena_hugepage_mb=1024`, hugetlb overcommit off, so the
reservations landed on THP-collapse or plain 4K pages); latency figures
below quote the worse of the two load generators.

| at the 1M-pulled point | F_MALLOC | HG_MALLOC v3 | F_MALLOC + arena | HG_MALLOC v3 + arena |
|---|---|---|---|---|
| puller, shm real-used | 1,953 MB | 1,761 MB | 1,522 MB | 1,390 MB |
| cache arena used (of 1,024 MB reserved) | — | — | 426 MB | 426 MB |
| shm + arena | 1,953 MB | 1,761 MB | 1,948 MB | 1,816 MB |
| committed / mapped to hold that | 3,072 MB fixed | 2,336 MB (114 × 16 MB) | 3,072 MB fixed + 1,024 MB arena | 1,488 MB (61 × 16 MB) + 1,024 MB arena |
| cold pull p50 / p95 / p99 | 1.5 / 8.9 / 34 ms | 1.35 / 16 / 60 ms | 1.4 / 5.6 / 31 ms | 1.2 / 12 / 54 ms |
| warm hit p99 | 0.78 ms | 15 ms | 0.75 ms | 0.77 ms |
| requests lost during the sweeps | 0 | 210 | 0 | 0 (8 pulls timed out in one 2.7 s stall and were answered 404) |
| shm still used after all 1M expired (owner / puller) | 236 / 448 MB | 238 / 450 MB | 23 / 23 MB | 23 / 23 MB |

Measured: the arena does not make the data smaller — shm plus arena lands
within 3% of the no-arena total in every configuration, because it is
the same cache relocated — but it changes what is left behind and the
tail. With the cache's hash tables and chunks outside shm, both
allocators return to their 23 MB start-up baseline after the mass expiry
instead of retaining a quarter of the peak (the arena's own high-water
mark, 426 MB on the puller, stays put by design), and HG_MALLOC v3's
warm-hit p99 drops from 15 ms to 0.77 ms — F_MALLOC's figure — with
zero lost requests, while it commits 1,488 MB instead of 2,336 MB for
the puller because the cache no longer flows through its growth path.
What remains on HG v3 is the cold-sweep tail (p99 54 ms against
F_MALLOC's 31 ms, including one ~2.7 s stall at the very end of the
sweep; 946 datagrams were dropped at the SIP socket over the sweep, all
recovered by the client's retransmit) and the arena's price: 1 GB pinned per node up
front, of which this workload used 426 MB.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/pull-sharing/mem-used-arena-dark.svg">
  <img alt="Shared memory really used versus contacts held, owner and pulling node: HG_MALLOC v3 and F_MALLOC as before, with the same allocators plus cachedb_perf's separate cache arena as dotted lines running about 400 MB lower at one million contacts" src="doc/pull-sharing/mem-used-arena-light.svg">
</picture>

### Controls: fixed arena (v2) and no GC (v1)

Two older HG_MALLOC generations were run through the same 1M protocol
with a fixed 3,072 MB arena (`-a HG_MALLOC -m 3072`, no cache arena) as
controls for the v3 row: v2 keeps v3's buddy allocator and inline garbage
collection (`gc_class`, `HG_GC_KEEP 0`) but can neither grow nor shrink,
so it isolates the elastic-growth machinery; v1 is the original bump/slab
allocator with neither buddy nor GC, so it isolates the GC. Four further
columns came with them: the v3 binary on a plain fixed `-m 3072`,
F_PARALLEL_MALLOC on the same binary
(`-s F_PARALLEL_MALLOC -k F_MALLOC -m 3072` — it has no pkg
implementation, so `-a` is refused at start) and v3 with an auto-scaling
profile, run twice — with the stock 30 s growth tick and with a 2 s one
(below). Latency figures quote the worse of the two load generators.

| | F_MALLOC | HG v3 | HG v2 | HG v3, fixed `-m 3072` | HG v1 | F_PARALLEL_MALLOC | HG v3 + profile | HG v3 + profile, 2 s tick | HG v3 fixed, + T1 (stats walk fixed) | HG v3 elastic, + T1 (stats walk fixed) | HG v3 elastic, + T1/T6, `shmem_enabled=advise` | HG v3 elastic, + two-phase commit (T14), `advise` | HG v3 elastic, + maintenance process, `advise` | HG v3 elastic, + maintenance process, `never` | HG v3 elastic, + whole-page large chunks (T22), `advise` | HG v3 elastic, + whole-page large chunks (T22), `never` |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| puller at 1M, shm real-used / used | 1,953 / 1,310 MB | 1,761 / 1,365 MB | 1,791 / 1,394 MB | 1,839 / 1,441 MB | crashed | 1,936 / 1,291 MB | 1,810 / 1,413 MB | 1,841 / 1,443 MB | 1,847 / 1,449 MB | 1,847 / 1,449 MB | 1,852 / 1,449 MB | 1,850 / 1,449 MB | 1,853 / 1,449 MB | 1,813 / 1,449 MB | 1,852 / 1,454 MB | 1,854 / 1,456 MB |
| mapped / committed to hold that | 3,072 MB fixed | 2,336 MB grown | 3,072 MB fixed (2,344 MB carved at peak) | 3,072 MB fixed (no cap: `hcap == hsize`) | — | 3,072 MB fixed, 32 pools | 2,496 MB grown (2,800 MB after the expiry wait) | 3,072 MB grown — the whole reservation | 3,072 MB fixed | 2,352 MB grown (115 grows, all on exhaustion) | 2,352 MB grown (115 grows, all on exhaustion) | 2,352 MB grown (115 grows, all on exhaustion) | 2,688 MB grown (136 grows, all ahead of demand) | 2,688 MB grown (136 grows, all ahead of demand) | 2,272 MB grown (110 grows, all ahead of demand) | 2,272 MB grown (110 grows, all ahead of demand) |
| cold pull p50 / p95 / p99 | 1.5 / 8.9 / 34 ms | 1.35 / 16 / 60 ms | 1.25 / 4.0 / 29 ms | 1.2 / 2.0 / 19 ms | — | 1.4 / 12.7 / 40 ms | 1.2 / 8.8 / 53 ms | 1.2 / 21 / 109 ms | 1.2 / 1.8 / 17 ms | 1.2 / 5.7 / 49 ms | 1.2 / 2.1 / 20 ms | 1.2 / 1.8 / 15 ms | 1.2 / 1.9 / 17 ms | 1.2 / 2.0 / 18 ms | 1.2 / 1.9 / 19 ms | 1.2 / 1.8 / 15 ms |
| warm hit p99 | 0.78 ms | 15 ms | 0.77 ms | 0.67 ms | — | 0.73 ms | 0.73 ms | 0.71 ms | 0.66 ms (max 4 ms) | 0.67 ms (max 28 ms) | 0.70 ms | 0.68 ms | 0.72 ms (max 4.3 ms) | 0.69 ms (max 4.4 ms) | 0.67 ms (max 4.8 ms) | 0.71 ms (max 4.7 ms) |
| warm seconds with p95 > 5 ms | 0 | 41 | 0 | 0 | — | 0 | 9 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| requests lost during the sweeps | 0 | 210 | 0 | 0 | — | 0 | 0 | 883 (+ 23 in the fill) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| shm still used after all 1M expired (owner / puller) | 236 / 448 MB | 238 / 450 MB | 238 / 450 MB | 238 / 451 MB | — | 400 / 762 MB | 238 / 450 MB | 238 / 450 MB | 238 / 451 MB | 238 / 451 MB | 238 / 452 MB | 238 / 451 MB | 238 / 451 MB | 238 / 451 MB | 238 / 451 MB | 238 / 451 MB |

Measured: take the growth away and leave the GC in, and the tail goes
with it — v2 matches F_MALLOC on every latency row (warm p99 0.77 ms, no
stall seconds, nothing lost, cold p99 29 ms) at v3's footprint (1,791 MB
at 1M, 238 / 450 MB retained after the expiry), while its GC ran at v3's
volume over the warm sweep (27k passes and 28.6k blocks returned on v2,
35k / 37k on v3). This revises the attribution given above: the periodic
warm-hit stall is not the `gc_class` passes themselves but something in
what v3 adds on top of v2 — the elastic commit/shrink machinery — which
these runs do not resolve further. v1 segfaulted in a UDP worker two
seconds into the first cross-node pulls (the fill itself completed: 1M ×
200, owners at 879 MB), so the no-GC control has no numbers.
The v3 binary itself confirms it: started with a plain `-m 3072` (no
`INIT:CAP`, so `hcap == hsize` and neither growth nor shrink can run —
the code's own definition of v2 behaviour) it is at least as clean as v2
on every row — cold p95 2.0 / p99 19 ms, warm p99 0.67 ms, zero stall
seconds, zero lost, zero kernel UDP drops — with the same GC volume
(26.7k vs 27.4k passes on the warm sweep) and the same retention, so v2
is subsumed and everything in v3's tail is the elastic path.
F_PARALLEL_MALLOC tracks F_MALLOC on latency and footprint but keeps
400 / 762 MB after the expiry, since each process's pool recycles only
its own frees. The profile runs needed a bench-only lift of the stock
validator first (`pt_scaling.c` rejects any scale-up target of 1,000 or
more as an invalid process count — a v3 bug, its own examples use 1024);
the profile was `scale up to 3072 on 35% for 1 cycles` / `scale down to
512 on 5% for 120 cycles` on an elastic `-m 512:3072` arena. With the
stock 30 s tick the proactive path is one 16 MB granule per tick: 41 of
the puller's 143 grows, of which only 11 fell in the cold sweep (the
other 102 were exhaustion grows, as without a profile), while the gate —
gross carve footprint against committed — kept adding a granule every
30 s through the idle warm sweep and the expiry wait (2,800 MB committed
on the puller, 1,744 MB per owner, none released: nothing gets under the
5% down-threshold); the tail moved towards v2 rather than away from it
(cold p99 53 vs 60 ms, 26 vs 39 stall seconds, 0 vs 210 lost, warm p99
0.73 ms with 9 stall seconds at the 30 s grow cadence), which one run
each cannot separate from run-to-run variance. A 2 s tick (a scratch
second timer, flags 0, `timer_workers=2`) made every grow proactive —
160 of 160 per node, the reserve floor never crossed, a warm sweep
without a single stall second — and the tails the worst of the table
(fill p99 31 ms with 23 REGISTERs lost, cold p99 109 ms, max 2.2 s, 47
stall seconds, 883 lost), with all three nodes at the 3,072 MB ceiling
inside five minutes: the 16 MB commit runs under the arena lock inside
whichever SIP or TCP worker reads the timer-job pipe, so the cadence only
sets how often the data path stalls. v3 has no proactive-growth counter
and no distinguishable log line for it — the split here is by tick
cadence; both to be added under T1.

T1 has since landed on the v3 branch (`feature/hg-malloc-v3-master`, lock
hold/wait histograms per reason, `gc` and `commit` timed on their own,
stall counters, `E_CORE_HG_LOCK_STALL`, proactive/exhaustion grow
attribution), and its first run on this rig found that the biggest
holder of the arena lock was the bench itself: every `shmem:` statistics
read walked the whole chunk registry under the lock (7 ms mean, up to
30 ms on the puller), so the 42–95 ms warm *maximum* of every row above —
v2 and F_MALLOC included — and part of the fixed-arena cold p99 were the
monitor's scrapes, and the same stall lands on any scraped production node
with a large arena. With that path made O(1) the fixed arena passes all
thirteen bars of the validation harness (`/dn/pullbench/validate.py`:
cold p99 ≤ 1.25× the reference, warm p99 ≤ 1 ms, zero lost, zero UDP
drops, no more than ten lock holds over 1 ms in a run, GC under 1 ms,
memory and retention within a few percent): cold p99 17 ms, warm p99 0.66 ms with a 4 ms maximum,
worst lock hold 0.8 ms. The elastic arena then has exactly one remaining
lock holder above a millisecond: the 16 MB commit inside each exhaustion
grow (`MADV_COLLAPSE` + `mlock` on the THP tier), 115 of them per 1M
absorb at a mean of 64 ms and a maximum of 126 ms, behind which SIP
workers wait up to 126 ms for a class refill — cold p99 49 ms with zero
lost requests, and every other allocator path under 1 ms.

Timing the phases of that commit split it three ways: the populating
`mlock` (14–20 ms for 16 MB), a `/proc/self/smaps` walk that verified
the backing by walking the page tables of the whole 3 GB mapping
(8–11 ms), and — only because the kernel default
`transparent_hugepage/shmem_enabled=never` denies shared memory huge
pages at fault time — a `MADV_COLLAPSE` retrofit copying the fresh 4 K
pages into huge pages (42–48 ms, three quarters of the commit). The
smaps walk is gone (the backing is now read from the huge-page counter
delta, 78 µs), and with `shmem_enabled=advise` set on the host the
collapse never runs: the last column is that configuration — the elastic
arena now matches the fixed one on every request-level figure (fill p99
1.03 ms, cold p95 2.1 / p99 20 ms, warm 0.70 ms, zero lost, zero kernel
drops, zero stall seconds) and passes 12 of the harness's 14 bars. The
two it still fails are the same fact: the ~20 ms populate of each grow
is held under the arena lock by the worker that needed the memory.

The last column is that change: the grow commit is two-phase — the
granule is reserved under the lock, populated with the lock released,
and published under it again (O(1)); a worker that runs out meanwhile
waits for the publish without the lock and retries. On the same
`advise` host the elastic arena then passes every bar of the harness —
cold p99 15 ms (the fixed arena: 17), fill p99 0.83, warm 0.68, zero
lost, zero drops, worst arena-lock hold 0.9 ms, the 414 workers that
exhausted during a populate waited 10 ms on average without holding
anything up — while committing 2,352 MB instead of a fixed 3,072. On a
host left at the kernel default `shmem_enabled=never` the lock stalls
are gone too (worst hold 1.9 ms, was 98–126 ms) but the waiters still
pay the requester's populate-plus-collapse (mean 39 ms): that residue is
what growing a granule ahead of demand would remove.

The maintenance-process columns: an elastic shm arena now gets
a dedicated core process, `HG maintenance`, which takes the arena lock
for microseconds once a second to keep the free grid above twice the
reserve floor (and runs the profile and shrink gates every thirtieth
tick), so every granule is populated in that process before any worker
needs it. On both host configurations all 136 grows were ahead of
demand, no worker ever waited, and the kernel-default `never` host now
matches the `advise` one on every request figure (fill p99 0.88, cold
p99 18 ms, zero lost) — the sysctl of the previous column is a CPU and
memory optimisation, no longer a latency prerequisite. The price is the
headroom, ~330 MB more committed than growing on exhaustion alone. Along
the way a size-bucketed free list for the large tier was tried and
reverted: it cut that tier's lock holds from 235 µs to 9 µs and made the
cold-pull p99 10 ms worse on four runs out of four, for reasons the
counters do not yet explain — a change whose only request-level effect
is a slower tail does not stay in, and the harness exists to say so.

The last two columns are the branch head. What the bucket experiment did
leave behind was a measurement: the large tier — where the cache's
256 KB chunks live — carried 868 MB of backing for 446 MB of live data
on the puller, because a 256 KB request was cut from a 512 KB buddy
block and the remainder went idle. On arenas of 256 MB and up the large
tier now carves whole 2 MB pages instead, so seven of those chunks pack
into one page: the puller commits 2,272 MB for the same 1,852 MB of
records (−416 MB against the previous column, 110 grows instead of 136,
all still ahead of demand), the owners carve 925 MB instead of 1,105,
and every latency figure stays where it was — cold p99 19 ms on the
`advise` host and 15 ms on the `never` one, warm p99 under 0.72 ms,
zero lost — so this one stays in.

Every configuration above on one sheet — memory (zoomed, with the cache
arena and the committed size where they apply), the three latency ladders
on log and linear scales, tail health, growth events and a summary table:

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/pull-sharing/allocators-overview-dark.svg">
  <img alt="Twelve-panel overview of all eighteen allocator configurations on the 1M pull-sharing bench: zoomed memory dot-plots for the pulling node, an owner node and after expiry; cold, warm and REGISTER latency ladders (p50, p95, p99) on log and linear axes; lost requests and stall-seconds; elastic growth events with the committed size; and a summary table. HG_MALLOC v2, HG_MALLOC v3 on a fixed arena and F_MALLOC have clean tails, and v3 with the T1 stats-walk fix has the cleanest (warm max 4 ms); HG_MALLOC v3 elastic has a 15 ms warm p99 and 210 lost requests; the auto-scaling profile removes the warm tail; the 2 s growth tick has the worst tails of all with 997 lost requests and the whole 3,072 MB committed" src="doc/pull-sharing/allocators-overview-light.svg">
</picture>

### Where the cache's memory lives: the backing

Every column above kept one thing constant: the cache's records sat in
cachedb_perf's own 256 KB chunks, carved out of shm (or a dedicated
reservation) and never given back — the 238 / 450 MB "still used after
all 1M expired" row is exactly those chunks. The module now decides at
startup where its cells live (`memory_backing = auto | core | own-hg |
own`): on an HG_MALLOC core it can hand the whole job to the allocator —
`core` puts every cell in the shm arena as an HG slab cell, `own-hg`
turns `arena_hugepage_mb` into a dedicated HG arena created through the
core's module-arena facade (whatever `-a` selected for the core) — and on
any other core `own` runs the module's own slot allocator, which now
reclaims: per-chunk free lists make a drained chunk provable, a reclaim
process of the module's own retires drained slots beyond `reclaim_keep`
and re-cuts them for any class, asks every process through IPC to send
its private cells home when chunks linger, and gives whole empty pages
back to shm (or punches whole 2 MB groups out of the reservation with
`MADV_REMOVE`) after a quiet window. The same 1M protocol, one column
per backing; the HG columns run the T22 head, the `own` columns run on
F_MALLOC because that is the core they exist for:

| | own chunks, never returned (T22 head, HG core) | `core`: HG cells in the shm arena | `own-hg`: a dedicated HG arena (256 → 1,024 MB) | `own` + reclaim, shm pages (F_MALLOC) | `own` + reclaim, 512 MB reservation (F_MALLOC) |
|---|---|---|---|---|---|
| puller at 1M: shm real-used + cache arena | 1,852 MB | 2,001 MB | 1,407 + 575 = 1,982 MB | 1,958 MB | 1,519 + 490 = 2,009 MB |
| owner at 500k | 893 MB | 965 MB | 678 + ~290 MB | 930 MB | 700 + 248 MB |
| still used after all 1M expired, puller (shm + arena) | 451 MB | 59 MB | 23 + 35 MB | 70 MB | 23 + 98 MB (97 of it the hash tables) |
| same, owner | 238 MB | 42 MB | 23 + 19 MB | 54 MB | 23 + 53 MB |
| cold pull p50 / p95 / p99 | 1.2 / 1.9 / 19 ms | 1.2 / 4.9 / 32 ms | 1.2 / 3.0 / 27 ms | 1.3 / 4.4 / 32 ms | 1.3 / 5.4 / 32 ms |
| cold sweep, per-second p95 in the last third | 6.0 ms | 21.4 ms | 15.2 ms | 21.9 ms | 22.9 ms |
| warm hit p99 | 0.67 ms | 0.70 ms | 0.70 ms | 0.73 ms | 0.71 ms |
| requests lost | 0 | 0 | 0 | 0 | 0 |
| harness verdict | 15 / 15 | 13 / 15 | 14 / 15 | 11 / 11 vs F_MALLOC | 11 / 11 vs F_MALLOC |

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/pull-sharing/cache-backings-dark.svg">
  <img alt="Three panels comparing the five cache memory backings: memory at peak and after expiry for the puller and an owner, the cold pull latency ladder with the warm p99, and the cold sweep's per-second p95 over the early, middle and late third of the sweep" src="doc/pull-sharing/cache-backings-light.svg">
</picture>

Three things the table says. First, the retention problem is gone in
every backing that reclaims: after the mass expiry the nodes are back
at their 42 MB start-up baseline plus what the policy keeps (one drained
slot per class, one spare page) plus the hash tables, which grow and
never shrink — 23–70 MB instead of 238–451. Second, HG's price for
managing the cells is memory, not CPU: about 7–8 % more per cell than
the module's tightly packed chunks (2,001 vs 1,852 MB on the puller), and
a profile of the late sweep shows no HG symbol above 0.6 %. Third, the
two HG-backed columns fail the cold p99 bar, and the reason is not the
allocator. The per-second trace shows all backings equal for the first
200 s of the sweep and diverging only in the last third; a host-wide
profile over that window puts the puller's `TCP main` — the dispatcher
every `bin`-over-TCP pull request and reply passes through — at 0.93–0.97
of a core in *every* configuration, the module's own chunks included.
That queue is at its edge at 1,500 pulls/s on this host, and what the HG
columns add is receive-side stalls (shm-arena lock waits of a
millisecond or more, three to four times more frequent than with own
chunks), which a saturated, per-connection serial dispatcher turns into
head-of-line blocking. The `own` columns on F_MALLOC show the same late
third as F_MALLOC itself (25–26 ms), i.e. the allocator's own tail. The
fix for that tail is therefore on the transport side — a pull transport
that does not route every message through one dispatcher, with the
receiving process only parsing and waking while the waiting worker does
the store — and it is written up, not built.

## Observability

* Exact fleet contact count: `sum(owned_contacts)` across nodes (ownership
  is disjoint, so the sum is exact — a Prometheus recording rule away).
* Per-node convergence: one `perf_cluster_size ul` MI call — live entry
  counts from every node, unreachable nodes flagged.
* Inventory: the ledger (`WHERE expires > now`) or the concatenation of
  every node's `ul_dump owned_only=1`. Do **not** full-`ul_dump` a large
  table over MI-datagram — the reply can neither be built in pkg nor fit a
  datagram, and the attempt wedges the node.

## Build

Everything the mode needs is in this tree: the four touched modules
(`usrloc`, `registrar`, `cachedb_perf`, `db_sqlite`), two cachedb core
headers and one line in `lib/reg` — 46 commits over upstream `master`, no
new external libraries. A plain checkout builds the bin-transport variant;
the encrypted `clctr` transport compiles in only when the separate
`clusterer_controller` module is present, and is not required.

Prerequisites (Debian/Ubuntu names): `build-essential bison flex` for the
core; `libsqlite3-dev` for an SQLite ledger or
`default-libmysqlclient-dev` for MySQL; `libncurses-dev` only if you want
`make menuconfig`. Nothing else for the module set below.

```sh
git clone -b feature/usrloc-pull-sharing-devel https://github.com/Lt-Flash/opensips.git
cd opensips
make -j"$(nproc)" all include_modules="cachedb_perf db_sqlite"   # db_mysql instead, or both
make install prefix=/opt/opensips-pullshare
```

`cachedb_perf`, `usrloc`, `registrar`, `clusterer` and `proto_bin` are
part of the default module set; the `db_*` ledger backends are excluded
by default upstream, hence `include_modules`. Installing into its own
prefix keeps the build side by side with a stock OpenSIPS — point `mpath`
at `<prefix>/lib64/opensips/modules/` (or `lib/` on 32-bit).

Verify:

```sh
/opt/opensips-pullshare/sbin/opensips -V | head -1          # opensips 4.1.0-dev
ls /opt/opensips-pullshare/lib64/opensips/modules/ | grep -E 'cachedb_perf|usrloc|registrar|clusterer|proto_bin'
```

Runtime module set for the mode: `proto_udp`, `proto_bin`, `tm`, `sl`,
`signaling`, `clusterer`, `cachedb_perf`, `usrloc`, `registrar`, plus a
`db_*` driver if a ledger is configured and `mi_datagram`/`mi_http` for
the MI surface. Verified 2026-08-22: a fresh shallow clone of this branch
from the public URL builds the whole tree with the command above on
Ubuntu 20.04 (gcc 9) with zero errors.

## Testing

* `scripts/…` rigs live outside the tree on the dev host: a 2-node netns
  rig (ledger round, cold start, adoption, shared-tag takeover,
  observability — drive1–6, 118 checks) and a 3-node DB-less rig for the
  HELD protocol (drive7, 18 checks: one value per multi-holder pull,
  owner-dead forced re-ask on the first attempt, clean absence untouched).
* 1M deployment bench: three `nerdctl` containers on host networking, a
  Python epoll SIP blaster (sipp's RTPSTREAM fork double-binds its socket
  at init and is unusable), an MI-datagram CSV monitor.

## Branch anatomy

Stacked on `feature/cachedb-perf-devel` (PR #4118 — cachedb_perf module,
CP-15 cross-node pull, MTU plumbing). The pull-sharing commits build up:
config surface and arbitration → ownership/blob/invalidation → ledger →
keep-everything and cold-start sweep → adoption and shared-tag takeover →
observability (`perf_cluster_size`, owned/remote stats, dump annotation) →
async lookup → `pull_slots` sizing → HELD protocol
(`pull_authoritative_serve`).

## Known limits

* `mid_registrar` is not adapted (its save-side fetches are not
  pull-aware); plain registrar is.
* CONTACT_CALLID matching is rejected (the ledger's natural key would need
  the Call-ID folded in).
* DB-less operation trades: any record that was ever pulled survives a
  crash — holders keep serving it, and a restarted owner pulls its own
  records back from them on lookup. What cannot be recovered is a record
  that existed *only* on the crashed node (never looked up from another
  node, and no ledger holding a second copy): it stays unreachable until
  its device re-registers, at most one refresh interval. Similarly
  `sum(owned_contacts)` undercounts briefly after a crash — orphaned
  records are served but ownerless — healing as the owner re-pulls or
  refreshes land on survivors. A ledger closes both gaps.
* Mixed-version clusters must keep `pull_authoritative_serve` off until
  every node understands the HELD answer.
