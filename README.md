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

## cachedb_perf: the study and the measurements behind the module

Everything that was measured while building the `perf://` backend — the
index-structure shootout, the concurrency and read-protocol experiments,
the memory-backing tiers, the in-process comparison with `cachedb_local`
and `cachedb_redis`, the end-to-end topology-hiding runs at 50k and 100k
held calls, the huge-page arena, and what the soak and production caught —
moved here from the upstream pull request so the PR stays short. The
graphs are served from this repository's `cachedb-perf-assets` branch.

#### The allocation-free read — `get_buf` (new optional cachedb endpoint)

Profiling the read path (production `F_MALLOC` allocator) showed the biggest *removable* cost is not the lookup — it is the vtable contract. `get()` must return a value the caller owns and `pkg_free()`s, so every hit pays a `pkg_malloc` + `pkg_free` + second `memcpy` that exist only to satisfy ownership: a fixed **~70 ns/op** (15% of a 470 ns lookup over 50 000 entries; 30% of a 236 ns cache-resident one). `memcpy` itself never even appears in the profile.

So this PR adds a small, **optional** core endpoint — `get_buf()`, advertised by `CACHEDB_CAP_GET_BUF` — that reads into a buffer the caller already owns: no allocation, one copy instead of two. Any backend may implement it; every caller keeps `get()` as the fallback, and nothing about the existing endpoints changes (in particular `get_counter()` keeps its documented `{-2,-1,0}` return set).

![get vs get_buf](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/getbuf-ab.png)

| 50k entries, 200-byte values, 100% reads | run 1 | run 2 | run 3 | median |
|---|---|---|---|---|
| `get()` ns/op | 395.3 | 414.2 | 397.7 | **397.7** |
| `get_buf()` ns/op | 290.1 | 271.0 | 372.7 | **290.1 (−27%)** |

Runs alternate get/get_buf in one binary to cancel warm-up bias; run-to-run variance on this box is real (one pair shows only −6%), so the honest claim is **20–27%**.

The contract is written for the failure cases, since those are what callers get wrong: the buffer must be private to the calling process; it may be written **speculatively** and abandoned, so its contents are undefined on any non-hit; `*vlen`/`*needed` are zeroed before anything else; and a value that does not fit reports its size via `*needed` while `*vlen` stays 0 — `{buf, *vlen}` is always a valid `str`. Inside cachedb_perf both entry points share one implementation of the seqlock read (optimistic loop, lock fallback, re-route retry, overflow leg, expiry), so they cannot disagree about which record they return.

The first consumer is `topology_hiding`'s `th_state_url` path (#4114): the conversion is written and tested (both the in-place path and the too-small→allocated fallback), and will be pushed to that PR **once this one merges**, since it needs this core endpoint to compile.

#### Enabling the kernel memory backing

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

### The study

Everything below was measured, not assumed — the benchmark rig ships in-tree (`modules/cachedb_perf/bench/`, `make run`, no OpenSIPS build needed) and every figure is reproducible. Hosts: Xeon E5-2699 v4, kernels 5.4 / 6.8 / 6.12; the NUMA numbers come from a vNUMA-pinned two-socket guest on the same silicon. The rig models structures and cache behaviour (single process, threads); it ranks designs rather than predicting server throughput.

#### 1. The index structure

![structure shootout](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/01-structure-shootout.png)

| design | @512 buckets (shipped default) | @65536 buckets |
|---|---|---|
| chained + `strncmp` (`cachedb_local` today) | 2484 ns | 111 ns |
| chained + hash cached in node | 1837 ns | 86 ns |
| sorted array per bucket + binary search | 134 ns | 100 ns |
| **64B cache-line bucket + 1-byte tags (this module)** | **84 ns** | |
| flat open addressing (rejected: stop-the-world resize) | 78 ns | |

Load factor alone is a **20× spread**. The chosen design is within 8% of the fastest structure measured, and the fastest one (flat open addressing) is impossible to resize across processes in shm. Also checked: `core_hash()` is *not* at fault (chi²/df 0.65–1.18 vs FNV-1a on thids, dialog ids, AoRs and call-ids — statistically indistinguishable), so the module keeps it.

#### 2. Concurrency — an honest negative result

![concurrency scaling](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/02-concurrency-scaling.png)

The hypothesis was that `cachedb_local`'s write-lock-on-every-read destroys scaling. **It does not**: with a well-sized table workers rarely collide on a bucket lock, and it scales 8.4× on 8 threads. The 4× gap is a **per-operation constant factor** (no atomic RMW on reads, one cache line per bucket, tag filtering) — not a scaling win. Against the shipped 512-bucket default the gap is ~90×.

#### 3. The read protocol — measured before being believed

![read protocols](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/04-read-protocols.png)

Readers take no locks: a per-bucket seqlock with bounded retries and a sleeping-lock fallback. What makes this legal in OpenSIPS specifically: shm is mapped once before fork and never unmapped, so a stale pointer read is garbage-but-not-a-fault, and the version re-check discards it — the value is always copied out inside the optimistic section, with every length clamped and every pointer extent-checked before use.

The one credible alternative (QSBR / pointer-publication, no version check at all) was implemented in the rig and **rejected on the numbers**: identical at 100% reads (on x86/TSO the version loads hit the already-loaded bucket line — the seqlock is free) and ahead only under single-hot-bucket write contention that SIP traffic doesn't exhibit (seqlock retries measured at 1.2 per 1000 reads on a uniform 95/5 mix). The useful piece survived without any grace-period machinery: a byte-identical `set()` that only refreshes the TTL — the dominant write in the motivating workload — takes the bucket lock but skips the version bumps and the memcpy entirely. One atomic `expires` store; concurrent readers of the bucket are undisturbed.

#### 4. What was rejected: write staging and queueing

![write staging](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/05-write-staging.png)

| queued writes, 8-thread budget | applied Mops/s | vs direct | ring full |
|---|---|---|---|
| 8 direct writers | **116.3** | 1.00× | — |
| 7 producers + 1 consumer | 24.1 | 0.21× | 99% |
| 4 producers + 4 consumers | 54.6 | 0.47× | 97% |

A shared staging buffer **loses** throughput as threads are added — one atomic append offset is a hotter point of coordination than thousands of bucket locks. Queued writes do less than half the work of writing directly, and break read-your-writes semantics. The rule this established shapes the whole module: per-process regions win for *allocation* (the arena uses them — zero atomics on the alloc fast path), but never for staging live entries.

#### 5. Modern-kernel memory backing

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

#### 6. Expiry

| strategy | per sweep (50k entries, ~13 due) | locks/sweep |
|---|---|---|
| full sweep, lock every bucket | 1.31 ms | 65 536 |
| **per-bucket `min_expires`, unlocked skip** | **0.044 ms** | **13** |
| timer wheel, O(expired) | 0.0005 ms | — |

Even the full sweep is 0.13% of a core — expiry is a *memory reclamation* problem (entries squatting up to `cache_clean_period`), not a CPU problem. The `min_expires` hint gets 30× for zero hot-path cost; the wheel's further 84× buys nothing and costs 74 ns/insert plus 16 B/entry.

#### 7. NUMA — measured on a pinned two-socket testbed

| binding | dependent pointer chase |
|---|---|
| local (same socket) | 146.5 ns |
| remote (cross-socket) | 194.6 ns (**+33%**) |

Measured in-guest on a Proxmox VM with vNUMA bound per host socket (`numaN: ...,hostnodes=N,policy=bind`, dual E5-2699 v4 host — plain `numa: 1` without `hostnodes` fabricates topology over one memory domain and measures nothing). Two consequences, both already reflected in the design rather than motivating changes:

- Cross-socket **reads** of a shared cache cannot be sharded away — a worker reading an entry written on the other socket pays the remote latency however memory is partitioned; only replication avoids it. So the table is deliberately not NUMA-sharded (and §2 shows there is no lock contention for sharding to relieve either).
- The **write** side is node-local by construction: the arena's per-process chunk ownership means each worker faults — and therefore first-touch places — its own records on its own node.

Where NUMA does matter for the roadmap: page walks against remote memory amplify TLB-miss cost, so the huge-page backing is expected to be worth *more* on two sockets than the 1.42× measured on one — to be quantified in the end-to-end benchmark. One refinement from the same testbed: with `pdpe1gb` exposed, 1 GB pages *are* allocatable at runtime on a fresh boot (2 granted right after boot) — the earlier "unobtainable" holds only once uptime fragments memory. The ruling against them stands on arithmetic: 2 M pages already give a ≤1 GB arena full TLB coverage on this hardware.

### Measured: cachedb_perf vs cachedb_local, in-process

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

#### Is it only fast at reads? The mix swept, with `cachedb_redis` for scale

The numbers above use the `th_store` access pattern (95% get / 5% set), which invites a fair question: is this just a read cache that gives the win back on writes? It is not. Sweeping the read/write mix with `cdbbench` driving the `cachedb_funcs` vtable **directly** — no consumer module in the path — gives:

![write/read mix](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/write-mix.png)

| ns/op (median of 3), 50 000 entries, 200-byte values | 1 worker | 4 workers | 8 workers |
|---|---|---|---|
| **100% writes** — cachedb_perf | **562** | **639** | **632** |
| **100% writes** — cachedb_local | 3123 | 4239 | 6226 |
| **100% writes** — cachedb_redis (loopback) | 161 138 | 257 458 | 366 320 |
| 50/50 — perf / local | 401 / 2638 | 549 / 3527 | 546 / 5014 |
| 95% reads — perf / local | 328 / 2583 | 404 / 2866 | 385 / 2775 |

**cachedb_perf is 5.6–9.9× faster than cachedb_local on *pure writes*, and the margin widens with concurrency** — cachedb_perf stays flat (562 → 639 → 632 ns as workers go 1 → 4 → 8) while cachedb_local degrades (3123 → 4239 → 6226). Writers in both take a bucket lock; the difference is what happens inside it. `cachedb_local`'s `add`/`set` path parses the stored value, reformats it and **reallocs the entry under the lock**, so the critical section grows with contention; cachedb_perf writes fixed-width fields in place under a seqlock bracket.

`cachedb_redis` over loopback is 250–580× slower here. That is not a criticism of Redis — it is the cost of a synchronous round trip per operation, and it is exactly why a local cache exists. Use Redis when state genuinely must be shared between nodes; use `perf://` when it must not leave the box.

#### End-to-end: TH under 50 000 held calls

Same LB, topology_hiding with `th_state_url` pointing at each backend (65536 buckets), ramped while holding ~50 000 concurrent calls. "Sustained" means <5% failures **and** peak concurrency ≤75k (actually still holding 50k, not backlogging):

| offered CPS | th + cachedb_perf | th + cachedb_local |
|---|---|---|
| 4000 | 3875 achieved, 3.0% fail, 54k held | 3941 achieved, 1.4% fail, 57k held |
| 6000 | **5775 achieved, 3.7% fail, 68k held** ✓ | 5490 achieved, 8.5% fail, 94k (backlogging) ✗ |
| 8000 | 6670, 16.6% fail (overloaded) | 5874, 26.6% fail (overloaded) |

cachedb_perf **sustains the 6000-CPS rung where cachedb_local breaks** — a sustained-ceiling lift from ~3941 to ~5775 CPS (**~1.5×**) at 50k live th_store states. The end-to-end gain is smaller than the isolated-cache 2.3–7.8× because SIP processing is the larger share of per-call cost, but it lands exactly where the cache matters: the high-concurrency point where cachedb_local's lock-on-every-read serializes the workers.

#### At 100 000 concurrent calls: cachedb_perf-TH vs dialog-TH vs no-TH

Pushing to **100 000 held calls**, comparing three topology-hiding strategies on the same LB (huge pages / THP enabled): cachedb_perf-backed th_store, the in-memory dialog module (`force_dialog`), and plain record-routing with no topology hiding at all.

![100k three-way](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/conc100k-th-compare.png)

| offered CPS (≈100k held) | no-TH (rr) | cachedb_perf-TH | dialog-TH |
|---|---|---|---|
| 2000 | 55% CPU, 0% fail | 67% CPU, 2.9% | 85% CPU, 0.1% |
| 3000 | 73% CPU, 0.1% | — | **100% CPU**, 1.0% |
| 4000 | 96% CPU, 0.1% | 93% CPU, 1.3% | 100% CPU, **8.2% (breaks)** |

At 100k concurrency **cachedb_perf-TH is nearly as cheap as doing no topology hiding at all** — it tracks the no-TH curve and holds 4000 CPS at 93% CPU. **dialog-TH is the loser here**: it saturates CPU by 3000 CPS, breaks at 4000, and carries ~2.5× the resident memory (a full per-dialog state machine + timers vs one compact th_store entry). This is a crossover from lower concurrency, where dialog leads — cachedb_perf's flat per-entry cost wins as the live-state count climbs.

Caveats: the single load generator is unstable at 100k (some cachedb_perf mid-rungs showed generator-side failures at low LB CPU — discarded); and the huge pages here are whole-shm THP that benefits all three equally — the module's *own* huge-page arena is a separate mechanism, measured on its own in the next section.

#### Re-verified end to end, with the backend's own counters as proof

The runs above were re-done with one addition: **every rung asserts, over MI, that the backend under test actually did the work** — `cachedb_perf:perf_stats` must show stores in the `th` collection and zero dialogs; the dialog arm must show `dialog:processed_dialogs` and zero cache stores; the no-TH arm neither. A rung that fails its assertion is reported invalid and discarded rather than silently contributing a number. All twelve rungs passed.

![three-way, asserted](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/th-3way-asserted.png)

| 50 000 held calls | no TH | cachedb_perf-TH | dialog-TH |
|---|---|---|---|
| CPU @2000 CPS | 41% | 43% | 41% |
| CPU @3000 CPS | 47% | **52%** | 60% — 3.1% calls lost |
| CPU @4000 CPS | 54% | **58%** | 69% — **10.3% calls lost** |
| RSS @4000 CPS | 6.6 GB | **6.9 GB** | 12.1 GB |
| peak concurrency @4000 | 49 989 | 50 033 | 56 504 (backlogging) |

At 4000 CPS `cachedb_perf`-backed topology hiding costs **4 CPU points and 4% more memory than doing no topology hiding at all**, while holding concurrency at exactly 50k. Dialog-backed hiding costs 15 CPU points, 1.8× the memory, and is dropping 10.3% of calls — its rising "concurrency" is a backlog, not held calls. This is the same result as the 100k run above, now with the cache proven to have been exercised rather than assumed.

#### Huge-page arena (CP-20)

The arena can now back its chunks with **2 MB huge pages** instead of 4 K (modparam `arena_hugepage_mb`; the reservation is 2M-aligned, mlock-pinned, created pre-fork and shared by all workers). Measured on the real module (−O3, 8 workers, medians of repeated runs):

![CP-20 huge pages](https://raw.githubusercontent.com/Lt-Flash/opensips/cachedb-perf-assets/cp20-hugepages.png)

| condition | 4K pages | huge pages | gain |
|---|---|---|---|
| near-empty (clustered working set) | 31.7 Mops/s | 33.8 Mops/s | +7% |
| 50 000 resident (~13 MB working set) | 21.2 Mops/s | 24.0 Mops/s | **+13%** |

The gain is larger at 50k, where the working set spreads across enough memory to thrash the 4K TLB — exactly the case huge pages relieve. It's below the 1.19–1.43× the pointer-chase showed in isolation because each operation also pays for the hash, the tag scan and a copy of the value. Detection is by *trying* each tier (hugetlb → THP → collapse → 4K), never by kernel version; `mlock` wants `LimitMEMLOCK=infinity` and warns-and-continues otherwise.

### Design in brief

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

### Script interface

Single-key operations go through the core cache functions unchanged. The module's own multi-key operations are **`perf_`-prefixed with Redis verbs** — deliberately *not* the `cachedb_local` parity names (`cache_remove_chunk` / `fetch_chunk`), so migrating those two calls requires a script change; everything else is drop-in:

```
perf_del("session-*");                          # glob delete -> count
perf_mget("user-*", $avp(k), $avp(v));          # matches -> index-paired AVPs
perf_mget_json("*", $var(j));                   # -> {"hits":"6","user-alice":"a1",...}
```

All three ride one lock-free walker (Redis SCAN-class guarantee) with binary-safe JSON escaping; `iter_keys` uses the same walker. Two startup selftest modparams (`arena_selftest`, `htable_selftest`) ship as permanent diagnostics and fail startup on any mismatch.

The same walker backs the **introspection MI** (full command table in the **MI commands** section above) — the operator visibility `cachedb_local` never had, and lock-free so a key scan never stalls SIP traffic. `perf_scan` is the answer for a large cache where `perf_keys` would truncate: its cursor is an ascending bucket index, so it stays valid across a concurrent resize and returns every entry present throughout at least once — without Redis's reverse-binary cursor masking, because the table only grows (buckets never move).

### Status

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
- [x] **Portability** — built and exercised outside the usual glibc/x86 dev box, in containers: **Alpine 3.24** (musl 1.2, gcc 15.2) and **RHEL 9 / UBI9** (glibc 2.34, gcc 11.5). Zero warnings on either, with the arena/table selftests and the multi-process soak passing on both. The module needed no conditional compilation for musl; the one portability defect the exercise turned up was in the core rather than here (`lib/url.c` undefining `_GNU_SOURCE` before the headers that need it, which hides `clock_gettime`/`ctime_r` on musl), submitted separately as #4119
- [x] End-to-end `th_state_url` benchmark against `cachedb_local` (50k held calls) and against dialog-based topology hiding (100k held calls) — both sections above
- [x] DB persistence — whole-collection save/load to any `db_*` backend (`perf_save`/`perf_load` MI, plus `db_mode` startup-load / shutdown-save), TTLs kept as absolute wall-clock time so they survive a restart. The snapshot runs in a single transaction where the backend supports one, which makes it both fast and atomic — 30 000 entries save in 0.35 s on `db_sqlite` (against ~60 s and an aborted process before), and an interrupted save now rolls back rather than replacing a good snapshot with a partial one. Verified end to end with `db_sqlite` (save → shutdown-save → startup-load, values intact, TTL decremented across the cycle) and with `db_redis`, which takes no transaction and measures 5.05 s for the same rows. Rows whose wall-clock expiry has passed are dropped at load rather than merely skipped — otherwise, with `db_mode=1` or after any shutdown that was not graceful, dead rows accumulate in the table indefinitely. Single-node durability, not replication
- [x] **Cluster sync (`perf_sync`)** — MI command + script function that saves this node's collection to the DB, then signals peers over the `clusterer` API to reload it (one message per *sync*, not per operation); each peer reloads from the DB and raises `E_CACHEDB_PERF_SYNCED`. Soft dependency — degrades to a DB save with no broadcast if clusterer/`sync_cluster_id` is absent. A pull-from-DB refresh model for single-writer/read-replica topologies — deliberately *not* `/r`-style per-operation replication. Verified on a single-node cluster: the capability registers and lists in the clusterer's `clusterer_list_cap` MI (`cachedb-perf-sync`, state Ok), a soft `DEP_SILENT` clusterer dependency reorders init so it works regardless of load order, and `perf_sync` degrades cleanly with no clusterer — no crashes. Each collection also reports `last_sync_out` / `last_sync_in` / `last_sync_source` in `perf_stats` (shown only when `sync_cluster_id` is set): the clusterer's own `clusterer_list_cap` state of `Ok` for this capability only means *registered and enabled* — the module registers with `startup_sync=0` and takes no part in the clusterer's startup data-sync — so the honest convergence signal lives in the module's own stats. The multi-node broadcast→reload fan-out follows the `ratelimit` clusterer pattern and is **verified on a two-node cluster**: with both nodes pointed at one shared `db_sqlite` file and linked over `proto_bin`, `cachedb-perf-sync` registers on both, `perf_sync` on node 1 returns `{collections:1, saved:3, broadcast:1}`, and node 2 logs `cluster sync: reloading <sync> from DB (issued by node 1)`, reloads every key with its TTL intact and raises `E_CACHEDB_PERF_SYNCED` carrying `source_node=1`. Verified in both directions (node 2 → node 1 likewise, `source_node=2`)

- [x] **Read/write mix swept** — 5.6–9.9× over `cachedb_local` on pure writes (flat vs degrading as workers rise), and an assertion-verified end-to-end three-way where perf-TH lands within 4% of no-TH on CPU and memory
- [x] **Allocation-free read** — the optional `get_buf()` cachedb endpoint (`CACHEDB_CAP_GET_BUF`) and its cachedb_perf implementation; one shared read path for both entry points; measured 397.7 → 290.1 ns/op at the median (table above)

#### Correctness: what the multi-process soak caught

A lock-free read path plus a table that resizes itself under live traffic is exactly the kind of code where a single-process selftest passes and production still corrupts memory. So the soak (`bench/cdbstress.c`) runs the real thing: 8 worker **processes** on one shared backend, a get/set/remove/add mix, with the maintenance timer splitting buckets the whole time. Every value is written all-bytes-equal so a torn read is visible; counters are hammered with `add(+1)` so a lost update shows as a shortfall; a set of keys is inserted once and never removed so a split that drops one shows as a miss.

It failed inside a second — a segfault on an *impossible* size class (88) read out of a cell's class byte. Root cause: after `fork()` every child holds a copy-on-write copy of the parent's private allocator hoard (same bump pointer, same free-list cell addresses), and `pcache_arena_child_init` had each child *donate* that hoard to the global pool. The identical physical cells were enqueued once per child, popped by several processes at once, and written through concurrently — one process's value byte landed on another's class id. The fix: a child drops its inherited copy and carves its own chunk on first use, never donating cells it doesn't own. After it, the full soak is clean — 24M ops, 3093 concurrent splits, no torn reads, counter sum equals total adds, every immortal key intact — and equally clean under the `Q_MALLOC_DBG` redzone allocator and under each of OpenSIPS' three core allocators (`F_MALLOC`, `Q_MALLOC`, `HP_MALLOC`), which back both the per-process state and the arena's shm chunk allocation.

#### And what production caught that the soak did not

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

#### And what an adversarial design review caught before it shipped

Designing `get_buf` began with a red-team pass over the read/write protocol, which found three latent defects in the *existing* code — each now a separate commit:

1. **A seqlock write-ordering hole on weak memory models.** Every version bump used a RELEASE RMW; release is one-way and permits the payload stores that follow to be observed first, so on aarch64/ppc64le a reader could sample an even version, copy a half-written record, re-check the same even version and accept the tear. All bumps are now ACQ_REL (the same reason the kernel writes `seq++; smp_wmb()`); x86-64 was never affected, which is exactly why no soak had caught it.
2. **`rflags` survived an in-place overwrite.** Storing an 8-byte string over a key that had been a native counter left `PCACHE_F_INT` set, and the read path then formatted the ASCII as an int64: `cache_store("…","12345678")` read back as `4050765991979987505`. The flag is now cleared inside the version bracket.
3. **The copy clamp could wrap.** `PCACHE_REC_HDR + klen + vlen > bound` on unsigned values wraps for a torn `vlen`, skipping the clamp on the one path that needs it; it is now subtractive and cannot wrap.

The standalone rig behind every figure above lives in `modules/cachedb_perf/bench/` (`make run`, no OpenSIPS build needed) — full measurement history, every rejected alternative and why — so the numbers here are reproducible rather than asserted.












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
