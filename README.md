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
  lookup rate (3,000/s, 337 s), flat from entry 0 to entry 1M.
* Per-request SIP round-trip: **cold cross-node pull p50 1.4 ms** (p95 9 ms,
  p99 37 ms), **warm local hit p50 0.46 ms** (p99 0.78 ms), both independent
  of collection size.
* Tuned run bookkeeping: pulls requested == received == stored == entries ==
  1,000,000; timeouts, slot starvation, orphans, misses all zero.
* Warm re-sweep: 1M lookups, 100% answered locally, zero further pulls.

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
* DB-less operation trades: a crashed node's never-pulled records are lost
  until re-REGISTER, and `sum(owned_contacts)` counts only living owners.
* Mixed-version clusters must keep `pull_authoritative_serve` off until
  every node understands the HELD answer.
