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

## Observability

* Exact fleet contact count: `sum(owned_contacts)` across nodes (ownership
  is disjoint, so the sum is exact — a Prometheus recording rule away).
* Per-node convergence: one `perf_cluster_size ul` MI call — live entry
  counts from every node, unreachable nodes flagged.
* Inventory: the ledger (`WHERE expires > now`) or the concatenation of
  every node's `ul_dump owned_only=1`. Do **not** full-`ul_dump` a large
  table over MI-datagram — the reply can neither be built in pkg nor fit a
  datagram, and the attempt wedges the node.

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
