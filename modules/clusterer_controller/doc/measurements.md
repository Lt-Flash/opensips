[clusterer_controller](../README.md) / Measurements

# Measurements

## Network-namespace suite

*test/netns_tests.sh* (see [Automated tests](testing.md#automated-tests)) runs four nodes in network namespaces on one host, every node a member of two clusters that share one port; faults are real - iptables cuts that must show dropped packets, SIGSTOP/SIGCONT pauses, SIGKILL, netem loss and jitter. On the current head it runs 120 checks, T1 to T27; all pass, built with gcc 13 and *-Werror* on the module.

Every change was first shown to fail without itself - the same suite against the build before it:

| Change | Check | Build before the change | With the change |
|---|---|---|---|
| A paused node does not judge its peers | T13: a member paused 15 s | elects itself, purges its peers | neither |
| | T14: the master paused 15 s | purges its peers | one master once it wakes |
| Learned master watchdog | T19: the master killed | - | new master in 5.4 s |
| | T20: 25% loss and 300 ± 300 ms jitter into one member for 90 s | - | no election; its wait rises to about 5.5 s and back to 3 s |
| Asking before electing | T16 / T21: a member that alone stops hearing the master | - | holds the election; the master logs which member cannot hear it |
| PreVote | T22: a member cut off for 25 s | - | elects itself, keeps the term; the cluster's term unchanged |
| Local health | T23: 1.3 s pause bursts | - | health peaks at 8, no election, no purge, back to 0 |
| Kernel drops counted | T15: a flood into one member | - | 3094 drops counted |
| Re-deriving the same key keeps the sequence | T20 | 108 packets refused as replays | none |
| The new master takes the tag at once | T17 / T19 | the tag stays on backup after a maintenance handover; after a failover it waits for the purge | taken at the election |
| clusterer drops packets before identity | T26: the master killed and restarted at once | the restarted node segfaults | survives |
| Remote maintenance, full level | T25 | 4 of 7 checks fail | all pass |
| Capability bits | T27: a member on a build without the master probe, the master killed | survivors wait for answers: failover 9.5 s after four "may be the one cut off" waits | elect at once: 3.0 s |

## Test cluster

Two clusters of three nodes on the test network, one VLAN with multicast: cluster 1 is three SBCs (KVM virtual machines), cluster 2 three registrar nodes (LXC containers), all Ubuntu 20.04 running their normal SIP configuration with *use_controller=1* and *manage_shtags=1*. *test/rtests.sh* drives the faults from another host over SSH and checks every node over *mi_http*:

| Test | Fault | Checked |
|---|---|---|
| R0 | - | one master named by every node, every member has a node_id, the cluster identity, the master holds the active tag |
| R1 | a member restarted | back with the same node_id; master unchanged |
| R2 | a member paused 35 s (past the purge window) | does not elect itself or purge, reports the stall, keeps its node_id |
| R3 | the master paused 20 s | the others elect; the woken master stands down; the tag moves |
| R4 | a member stops hearing the master only (10 s) | holds the election; the master logs which member cannot hear it |
| R5 | a member cut off both ways for 25 s | suspects itself first, keeps the term, heals to the cluster's master |
| R6 | the master killed (SIGKILL) | replaced on its peers' confirmation; the new master holds the tag at once; back with its node_id |
| R6b | the master stopped | a new master without the watchdog |
| R6c | the master killed and restarted at once | the restarted node survives the new master's first tag announcement |
| R7 | maintenance: the backup, then the master | out of election and tags, the master hands over at once, a pin is refused, back in service |
| R8 | 30% of the master's packets into one member lost for 90 s | its learned wait rises above 3 s; no election |
| R9 | another node put into full maintenance | every node lists it, the others disable it in clusterer, then enable it again |
| - | logs | no packet refused as a replay, no crash |

| Build | SBC cluster | Registrar cluster | |
|---|---|---|---|
| first wire-v2 build | 40 of 41 | 40 of 41 | R7: after a maintenance handover the new master never took the tag - fixed |
| + the tag fix | 43 of 43 | 42 of 43 | a restarted registrar segfaulted on the new master's tag announcement - fixed in clusterer |
| + the clusterer fix | 45 of 45 | 45 of 45 | |
| + remote maintenance, full level | 51 of 51 | 51 of 51 | |
| OpenSIPS 4.1 build with the module | 51 of 51 | 51 of 51 | latest run, below |

Latest run: the killed master was replaced in 6.2 s (SBC) and 4.9 s (registrar); a stopped master handed over in 0.5 s; under 30% loss the member's learned wait peaked at 8.7 s and 9.4 s without an election; in R6c the restarted registrar dropped 3 packets that reached it before its identity and kept running.

```bash title="real-cluster tests - on a test cluster only, it takes masters down on purpose"
MI_ROOT=mi ./test/rtests.sh 1 10.0.0.11 10.0.0.12 10.0.0.13
```

## Under CPU load

The four-node rig on a 16-core host, 300 s per level, busy loops at normal priority on none, twice and eight times the cores: **no** master timeout, probe, held election, self-stall, role change or purge at any level. The controller's worker does little, and scheduling delay under CPU saturation stays far below the 3 s floor; a paused process - not CPU load - is the real false-failover risk, which the stall check covers.

## Earlier revision

Before wire version 2, the same rig against the build before it (36 checks: 36 pass, 16 on the earlier build):

| Check | previous build | this build |
|---|---|---|
| two clusters on one port: duplicate/foreign multicast warnings | 140 | **0** |
| node that left cleanly, then a newcomer joins: newcomer takes its id | yes (id 3) | **no** |
| ... the node returns with its old id | no (got 4) | **yes** |
| heal between masters with different settings: nodes still alive | 3 of 4 (one `exit`ed) | **4 of 4**, both warn, no merge |
| SIGKILL restart after >1024 messages: non-master receives the restarted node's traffic | 30/30 | **30/30** |
| node on the previous wire version | - | refused, named in the log; cluster unaffected |
| separate deployment, *identical* settings, same group/password/`cluster_id`, healed after 36 s | **absorbed** (4 members) | **refused**, both sides log the other's uuid, 3 + 1 |
| partition of the *same* cluster (member isolated past the purge window), healed | heals | **heals**, same uuid, one master |
| cold start: one uuid on every node; restarted node adopts it; `uuid=` pin = the name's v8 UUID on every node | - | yes / yes / yes |
| crashes / CRITICAL lines (excluding a pre-existing core TCP_main message on BIN reconnect, present in both builds) | 2 | **0** |

## HA behaviour tests


The following tests were performed on a three-node cluster (nodes *A*=10.22.23.191, *B*=10.22.23.192, *C*=10.22.23.193) to verify correct failover, tag stability, and no-steal-on-join behaviour. The sharing tag under test is *vip1* in cluster 1. All nodes run with *manage_shtags=1*.

Tag state was queried after each operation via:

```bash
opensips-cli -x mi clusterer_list_shtags
```

### Baseline

All three nodes running. B holds the active tag; A and C are backup.

```opensips
A (10.22.23.191)  svc=active  tag=backup
B (10.22.23.192)  svc=active  tag=active
C (10.22.23.193)  svc=active  tag=backup
```

### Test 1 — Stop the active node

B (active) is stopped. The remaining nodes must elect a new active holder. B must rejoin as backup and must not steal the tag from whichever node became active.

```opensips
# Stop B
A  svc=active  tag=backup
B  svc=inactive tag=(down)
C  svc=active  tag=active   <-- C promoted

# Start B
A  svc=active  tag=backup
B  svc=active  tag=backup   <-- rejoined as backup
C  svc=active  tag=active   <-- C retains active
```

**Result: PASS.** Failover within the dead-node detection window; rejoining node did not steal the active tag.

### Test 2 — Stop a backup node

A (backup) is stopped. The active tag must remain on C without any transition. A must rejoin as backup.

```opensips
# Stop A
A  svc=inactive tag=(down)
B  svc=active  tag=backup
C  svc=active  tag=active   <-- unchanged

# Start A
A  svc=active  tag=backup   <-- rejoined as backup
B  svc=active  tag=backup
C  svc=active  tag=active   <-- still active
```

**Result: PASS.** Removing a backup node causes no tag movement; rejoining node started in backup state.

### Test 3 — Stop both backup nodes

A and B (both backup) are stopped simultaneously. The lone remaining node C must retain the active tag. A and B must rejoin as backup.

```opensips
# Stop A and B
A  svc=inactive tag=(down)
B  svc=inactive tag=(down)
C  svc=active  tag=active   <-- unchanged, lone node

# Start A, then B
A  svc=active  tag=backup   <-- rejoined as backup
B  svc=active  tag=backup   <-- rejoined as backup
C  svc=active  tag=active   <-- still active
```

**Result: PASS.** Active node remained stable while running alone; both rejoining nodes came up in backup state.

### Test 4 — Stop active node and one backup

B (active) and C (backup) are stopped. The sole remaining node A must become active. B and C must rejoin as backup.

```opensips
# Stop B and C
A  svc=active  tag=active   <-- A promoted, now lone node
B  svc=inactive tag=(down)
C  svc=inactive tag=(down)

# Start B
A  svc=active  tag=active   <-- retains active
B  svc=active  tag=backup   <-- rejoined as backup
C  svc=inactive tag=(down)

# Start C
A  svc=active  tag=active   <-- retains active
B  svc=active  tag=backup
C  svc=active  tag=backup   <-- rejoined as backup
```

**Result: PASS.** The surviving node correctly claimed the active tag; each rejoining node started in backup state without challenging the active holder.

### Test 5 — Full cluster restart

All three nodes are stopped (full outage). Nodes are then started one at a time. The first node up must self-elect as active (no peers available to sync from). Subsequent nodes must join as backup and must not steal the active tag.

```opensips
# All stopped
A  svc=inactive tag=(down)
B  svc=inactive tag=(down)
C  svc=inactive tag=(down)

# Start A first
A  svc=active  tag=active   <-- first/lone seed, self-synced
B  svc=inactive tag=(down)
C  svc=inactive tag=(down)

# Start B
A  svc=active  tag=active   <-- retains active
B  svc=active  tag=backup   <-- joined as backup, did not steal
C  svc=inactive tag=(down)

# Start C
A  svc=active  tag=active   <-- retains active
B  svc=active  tag=backup
C  svc=active  tag=backup   <-- joined as backup, did not steal
```

**Result: PASS.** The first node to start elected itself active and no spurious sync errors were logged. Each subsequent node joined as backup without challenging the active holder.

### Test 6 — Multiple clusters over one BIN socket

Two controller clusters (*id=1* and *id=2*) are configured on all three nodes. Each cluster has its own multicast endpoint (a distinct port is required - two clusters on the same multicast address:port are rejected at startup with *duplicate multicast*), but both advertise the *same* BIN socket (*bin:IP:3857*). Each cluster must form independently and its replication must stay isolated over the shared socket.

```opensips
modparam("clusterer", "cluster_options", "cluster_id=1, use_controller=1")
modparam("clusterer", "cluster_options", "cluster_id=2, use_controller=1")
modparam("clusterer_controller", "cluster",
    "id=1,multicast=239.0.90.1:3333,bin_socket=bin:IP:3857")
modparam("clusterer_controller", "cluster",
    "id=2,multicast=239.0.90.1:3334,bin_socket=bin:IP:3857")

# both clusters converge, each with its own master/backup/member roles;
# clusterer_list shows cluster 1 and cluster 2 both using bin:IP:3857
```

**Result: PASS.** Both clusters formed independently (each `member_count=3` with consistent roles), the BIN links of both were `Up` over the single shared socket, and there were zero decrypt / cross-talk / foreign-cluster errors on any node - the *cluster_id* in each BIN packet keeps the two clusters' replication traffic separate. A control test confirmed that configuring both clusters on the same multicast address:port is refused at startup.

### Summary

| Test | Scenario | Result |
|---|---|---|
| 1 | Stop active node; rejoin | PASS |
| 2 | Stop backup node; rejoin | PASS |
| 3 | Stop both backups simultaneously; rejoin | PASS |
| 4 | Stop active + one backup; rejoin | PASS |
| 5 | Full cluster restart; sequential startup | PASS |
| 6 | Two clusters sharing one BIN socket (distinct multicast) | PASS |

In all five failover tests (1–5): exactly one node held the active sharing tag at all times (including during the failure window), and no rejoining node stole the active tag from the current holder. Test 6 additionally confirmed that two clusters can share a single BIN socket with fully isolated replication.
