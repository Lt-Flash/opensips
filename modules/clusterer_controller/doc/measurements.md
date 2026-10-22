[clusterer_controller](../README.md) / Measurements

# Measurements

## Network-namespace suite

*test/netns_tests.sh* (see [Automated tests](testing.md#automated-tests)) runs four nodes in network namespaces on one host, every node a member of two clusters that share one port; faults are real - iptables cuts that must show dropped packets, SIGSTOP/SIGCONT pauses, SIGKILL, netem loss and jitter. On the current head it runs 134 checks, T1 to T29; all pass, built with gcc 13 and *-Werror* on the module.

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
| Statistics | T28 | all 6 checks fail - no statistics | all pass |
| Events | T29: the master killed, a member put into maintenance | 5 of 6 checks fail - no role, master or member events | all pass; the event route reads $cl_ctr_role while it runs |
| A joining node announces its master | T28: a member restarted by T27 | it counted no master change, and raised no master event | counts and raises it |
| A new master counts from the membership it knew (*release_tag_when_isolated*) | T24: a member cut off alone for 25 s, the option on | it elects itself and keeps the tag active - two holders | releases the tag; a member again after the heal, holding no released state |

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
| OpenSIPS 4.1 build with the module | 51 of 51 | 51 of 51 | |
| the same 4.1 build with the module at the PR head 9c6f01644f (capability bits, statistics, events) | 51 of 51 | 51 of 51 | latest run, below |

Latest run, on the module at 9c6f01644f (since then only *release_tag_when_isolated*, off by default, changed; the namespace suite covers it in T24): the killed master was replaced in 5.8 s (SBC) and 6.3 s (registrar); a stopped master handed over in 0.5 s; under 30% loss the member's learned wait peaked at 8.6 s and 5.9 s without an election; in R6c the restarted registrar dropped 2 packets that reached it before its identity and kept running.

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

The manual failover tests on a three-node cluster - the active node, a backup, both backups, the active node and a backup stopped, a full restart, two clusters on one BIN socket - are on their own page: [HA behaviour tests](ha-tests.md).
