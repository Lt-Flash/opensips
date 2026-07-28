# A 3-node containerized cluster, loaded until it talks

Three Alpine containers under containerd, clustered by clusterer_controller
over encrypted multicast, each running cachedb_perf with cross-node pull.
30,000 keys, twelve synchronous loaders driving ~90,000 uniform-random reads
per second through the SIP script path, every request timed, the control
plane captured with tcpdump on the bridge.  Both pull transports measured
back to back on the identical rig.

## Building and running the rig

`imgbuild.sh` builds the image (`cp15:latest`, ~30 MB): an Alpine build
container compiles the tree and `nerdctl commit` freezes it.  Three things
will silently ruin the result if forgotten, and all three happened here
before they were written down:

- the template `Makefile.conf` ships with `-DCC_O0` **uncommented** — the
  first image benchmarked compiler-unoptimised code;
- `include_modules` must name `clusterer_controller`, or clusterer builds
  without `CLUSTERER_CTRL_SUPPORT` and rejects `cluster_options` at startup;
- the runtime must pass `-a F_MALLOC` — the default here is Q_MALLOC_DBG,
  which once measured as 91% of all cycles in a profile.

`clusterup.sh` starts the three nodes (`TRANSPORT=clctr|bin` selects the
pull transport) on a dedicated bridge network.  IGMP snooping must be
switched off on the CNI bridge or multicast dies quietly.  MI is served
over UDP because a unix datagram socket cannot reply across container
mount namespaces — the server resolves the client's path in its own
namespace and finds nothing.

`cb_seed.py all|thirds` populates the cache (everything everywhere, or a
distinct third per node); `cb_run.sh <name> <secs> <procs-per-node>` runs
loaders and the stats collector; `cb_s4.sh` wraps a thirds-seeded run in a
tcpdump capture.  When filtering that capture remember the BIN transport
is TCP: `udp port 5599` captures nothing and looks exactly like "no
traffic".

## What was measured

Warm cluster, every key on every node: **90,457 req/s, 100.0% hits, zero
pulls, zero multicast** — the feature costs nothing when it is not needed.

Thirds-seeded convergence under the same load, `clctr` vs `bin`, only the
modparam changed:

|                              | clctr (multicast) | bin (TCP mesh) |
|------------------------------|-------------------|----------------|
| slowest node fully converged | **13.2 s**        | 17.6 s         |
| convergence-phase p99        | **585 µs**        | 1319 µs        |
| throughput in the miss storm | ~50k req/s        | ~20k req/s     |
| wire, same 60,000 misses     | **180,114 UDP packets** | 367,540 TCP segments |

Per miss that is 1 multicast + (N−1) unicast replies for `clctr` —
constant on the ask side however large the cluster — against ≈6.8 TCP
segments for `bin`, which must ask each peer separately and pay ACKs.
The medians never moved: the transports differ only on the miss path.
No request in any run (10M+ total) timed out or failed.

## Data layout

Each `results/<run>/` holds `stats.jsonl` (perf_stats from all three nodes
every 250 ms: entries, hits, misses, every pull counter) and
`latency-100ms.csv` (per-100 ms request count and p50/p95/p99/max from the
full per-request record).  The two captured runs also hold `pkts.txt.gz`:
one line per control-plane packet, timestamp and kind (`m` multicast, `u`
unicast reply, `b` BIN TCP segment).  `charts/` holds the rendered graphs
and the two generators; the raw per-request pickles and pcaps (~170 MB)
stay out of the tree — regenerate them by running the rig, it takes about
a minute per scenario.

Runs: `s1_hits` / `s1_bin` warm steady state per transport; `s2_converge`
first clctr convergence (no capture); `s4_pkts` / `s4_bin` the captured
convergence pair the comparison charts are drawn from.
