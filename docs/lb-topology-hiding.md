# Load balancer with topology hiding, and with dialogs

Measured on 2026-10-23 and 24: F_MALLOC against HG_MALLOC behind an OpenSIPS load balancer, three ways - with stateful topology hiding (TH) keeping its state in an external replicated cache, with TH keeping its state in process memory, and with dialog tracking alone. The question each table answers: the highest call rate the balancer carries cleanly, and how it fails above that.

**Summary.** With TH state in process memory the balancer carries 18,000 calls/s cleanly on HG_MALLOC in `shared` and `exclusive` (16,000 in all three receive modes) and 8,000 on F_MALLOC; with dialog tracking alone 14,000-16,000 on HG_MALLOC, while F_MALLOC already queues at 8,000 and collapses at 10,000; with TH state in an external cache 10,000 on HG_MALLOC, where the cache's network round trip sets the limit near 12,000, and 6,000 on F_MALLOC. Above its limit F_MALLOC collapses with its CPU per call climbing; HG_MALLOC's CPU per call stays flat or falls until it saturates.

## Setup

- **Server:** the setup B host of [benchmarks.md](benchmarks.md#what-was-measured-where) (16-vCPU KVM guest, Xeon E5-2699 v4, Debian 13, Linux 6.12, no hugetlb pool), 8 UDP workers, one listener, the three receive modes of [OpenSIPS PR #4365](https://github.com/OpenSIPS/opensips/pull/4365) (`shared` is the current code); HG_MALLOC `-m 512:4096 -M 64:512`, F_MALLOC `-m 2048 -M 32`. The build is the source of [benchmarks after the review fixes](benchmarks-after-review.md#setup-b-calls-relayed-by-a-stateful-proxy), setup B - an OpenSIPS 4.1 tree with HG_MALLOC, review fixes included, and other pending patches, #4365 among them - with the external cache's client module added.
- **Balancer script:** every initial INVITE goes through `dispatcher` (round robin over 12 destinations); `tm` relays everything.
  - **TH:** `topology_hiding("U")` on the INVITE, `topology_hiding_match()` on in-dialog requests, the state stored per call with a 60 s TTL (`th_state_ttl`) through a `cachedb` back-end:
    - *external cache:* [perfcached](https://github.com/Lt-Flash/perfcached), three nodes in 4-vCPU containers on the same LAN (0.4-0.7 ms round trip), every store and lookup a synchronous network request;
    - *process memory:* `cachedb_perf` ([OpenSIPS PR #4118](https://github.com/OpenSIPS/opensips/pull/4118)), no network.
  - **Dialog only:** `record_route()` and `create_dialog()` on the INVITE, no TH; in-dialog requests are matched to their dialog by Call-ID and tags (`match_dialog()`, the default `DID_FALLBACK`), because the load generators' UAS does not echo Record-Route.
- **Load:** sipp on three other hosts, 4 UAC and 4 UAS on each; a call is INVITE, 100, 180, 200, ACK, BYE, 200 - 6 datagrams into the server and 7 out, verified with a packet capture for the dialog script and from the receive counters for every script. 30 s cells, two per rate and mode, the server restarted for every cell.
- **Socket buffers:** OpenSIPS's default (512 KB on the socket), or `maxbuffer` 32 MB with `net.core.rmem_max` raised to 32 MB for the run, which the kernel turns into **64 MB** on the socket.
- **Verdicts,** per mode, from both cells:
  - *clean* - the setup B rule (every load generator reports, at least 98% of the offered calls complete, at most 0.1% fail) with p50 at most 10 ms and p99 at most 500 ms (sipp's response-time histogram, bucket upper bounds);
  - *holds* - the rule, with longer latency: calls queue in the socket buffer, up to a second and more at p99;
  - *fails* - the rule not met;
  - *collapses* - no load generator finishes its calls while the server drops datagrams by the hundred thousand.
- **CPU** is every OpenSIPS process's, sampled over 15 s of each cell, per 1,000 calls/s, in percent of one core.

## Results

### TH state in perfcached, 512 KB socket buffer

| calls/s | F_MALLOC: shared / auto / exclusive | F_MALLOC: CPU % per 1k calls/s | HG_MALLOC: shared / auto / exclusive | HG_MALLOC: CPU % per 1k calls/s |
|---|---|---|---|---|
| 6,000 | clean / clean / clean | 64-67 | - | - |
| 8,000 | fails / fails / fails | 63-64 | clean / clean / clean | 42-46 |
| 10,000 | - | - | - | - |

### TH state in perfcached, 64 MB socket buffer

| calls/s | F_MALLOC: shared / auto / exclusive | F_MALLOC: CPU % per 1k calls/s | HG_MALLOC: shared / auto / exclusive | HG_MALLOC: CPU % per 1k calls/s |
|---|---|---|---|---|
| 8,000 | holds / holds / holds | 56-59 | clean / clean / clean | 42-47 |
| 10,000 | collapses / collapses / collapses | - | clean / clean / clean | 40-43 |
| 12,000 | - | - | holds / holds / holds | 35-41 |
| 14,000 | - | - | - | - |

### TH state in process memory (cachedb_perf), 64 MB socket buffer

| calls/s | F_MALLOC: shared / auto / exclusive | F_MALLOC: CPU % per 1k calls/s | HG_MALLOC: shared / auto / exclusive | HG_MALLOC: CPU % per 1k calls/s |
|---|---|---|---|---|
| 8,000 | clean / clean / clean | 54-60 | - | - |
| 10,000 | holds / holds / holds | 54-57 | - | - |
| 12,000 | collapses / holds / fails | 40-57 | clean / clean / clean | 36-39 |
| 14,000 | collapses / collapses / collapses | - | clean / clean / clean | 36-38 |
| 16,000 | - | - | clean / clean / clean | 35-38 |
| 18,000 | - | - | clean / holds / clean | 36-38 |
| 20,000 | - | - | holds / holds / holds | 32-36 |
| 22,000 | - | - | holds / collapses / holds | 33-34 |
| 24,000 | - | - | collapses / collapses / collapses | - |

### Dialog only, no TH, 64 MB socket buffer

| calls/s | F_MALLOC: shared / auto / exclusive | F_MALLOC: CPU % per 1k calls/s | HG_MALLOC: shared / auto / exclusive | HG_MALLOC: CPU % per 1k calls/s |
|---|---|---|---|---|
| 8,000 | holds / holds / holds | 64-73 | - | - |
| 10,000 | collapses / collapses / collapses | - | - | - |
| 12,000 | collapses / collapses / collapses | - | clean / clean / clean | 37-41 |
| 14,000 | - | - | clean / clean / clean | 39-42 |
| 16,000 | - | - | clean / holds / clean | 40-41 |
| 18,000 | - | - | holds / holds / holds | 36-39 |
| 20,000 | - | - | collapses / collapses / collapses | - |

A dash: not run - rates below a clean one, and rates above a collapse, were skipped - or cut short: F_MALLOC at 10,000/s with the 512 KB buffer and HG_MALLOC at 14,000/s through the external cache stopped after one cell, which collapsed.

## What the tables show

- **Clean capacity, F_MALLOC -> HG_MALLOC:** TH in process memory 8,000 -> 18,000 calls/s (16,000 in all three modes); dialog only - (F_MALLOC queues already at 8,000) -> 14,000 in all modes, 16,000 in `shared` and `exclusive`; TH through the external cache 6,000 -> 10,000.
- **CPU per call** at the rates both allocators carry: TH through the external cache at 8,000/s, 56-64% (F_MALLOC) against 42-47% (HG_MALLOC); TH in process memory at 12,000/s, 40-57% against 36-39%. HG_MALLOC's figure stays between 32% and 47% at every rate it carries; F_MALLOC's climbs as it nears its limit, to 84-89% (TH in memory, 14,000/s) and 123-231% (dialog only, 10,000-12,000/s) in the `shared` and `exclusive` cells that collapse - workers spinning on its shared-memory lock rather than relaying calls.
- **The external cache's limit:** from 12,000 calls/s with HG_MALLOC, calls queue although OpenSIPS uses about 5.5 of its 8 workers' worth of CPU and each cache node about a quarter of one core (measured during a 14,000/s cell) - the workers wait on the synchronous TH lookups. With the state in process memory the same balancer carries 18,000.
- **Bigger socket buffers** let F_MALLOC hold 8,000 calls/s through the external cache only by queueing: every call completes, at a p50 of up to 1 s and beyond. HG_MALLOC completed every call at 8,000 with either buffer - about 3,000 drops per sampling window with 512 KB, recovered by retransmission, none with 64 MB - and at 10,000 with 64 MB, in each case without queueing.
- **`auto` has the longest tail** at the top rates, probably because with only 12 load-generator sockets the reuseport hash spreads them unevenly over the 8 workers; traffic from many sources would spread better.

## What differed between passes

- The first pass through the external cache filled two of its limits, so it was discarded and run again after raising them: deletes outran the tombstone table (`tombstone_retain_max`, raised from 250,000 to its maximum, 1,000,000 - at these rates it still fills, which only shortens how long a delete is remembered across a partition), and the write-ahead log overran twice, once sending a node through a heal that refused writes for a moment (raised from 8 x 64 MB to 48 x 64 MB). The cache nodes also had their containers raised from 1 to 4 GB and their arena from 256 MB to 2 GB for this test, and the TH collection was sized at 2^22 buckets.
- The first dialog-only pass was discarded: its script relayed in-dialog requests only along a route the load generators never sent, so ACKs were dropped and BYEs answered 404. Found from the receive counters (10.5 datagrams in per call instead of 6) and a packet capture; corrected as described above.
- Two HG_MALLOC dialog-only cells at 12,000/s lost every load generator's summary while the server counted 10,800-12,000 calls/s with no drops; they are left out, and the rate was run again (the six cells of the rerun are all clean).

