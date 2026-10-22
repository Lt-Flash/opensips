[clusterer_controller](../README.md) / Measurements

# Measurements

## Network-namespace suite

*test/netns_tests.sh* (see [Automated tests](testing.md#automated-tests)) runs four nodes in network namespaces on one host, every node a member of two clusters that share one port; faults are real - iptables cuts that must show dropped packets, SIGSTOP/SIGCONT pauses, SIGKILL, netem loss and jitter. On the current head it runs 168 checks, T1 to T34; all pass, built with gcc 13 and *-Werror* on the module.

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
| Refusals anyone can provoke, once a minute | T30: a 20 s flood from one address, no password | 848 to 940 log lines on every node | within the budget of 30 |
| A cluster uuid names another cluster only when authenticated | T30: garbage stamped with random uuids | all 16 foreign_clusters slots filled, the alarm logged | nothing recorded or logged |
| Only the handshake and the beacon under the bootstrap key | T30: sealed MASTER_ALIVEs from 20 addresses that are no member | 8 of them taken as members, with node_id 0 | none |
| A beacon must carry what every build sends | T30: short or out-of-range beacons, sealed | the master demoted itself into a merge with an address that runs nothing | dropped before ranking |
| JOIN_REQ and NODE_ASSIGN refuse what is not an address or a node_id | T30: JOIN_REQs naming an empty or binary address | the master processed them, answering with assignments and member lists or with refusals | dropped |
| A JOIN_REJECT is bound to the request it refuses | T32: a recorded refusal sent to a member joining with the right password | it stopped | it joins; a refusal it earns still stops it |
| - | T31: three members started at once, five times; all four cold at once, three times | passed on the build before - no change needed | one master, distinct node ids, every time |
| The sender is what a packet seals (wire version 3) | T33: a member's stop recorded, the member started again, the record sent from the bridge's address | its old GOODBYE removed it | refused as packets from its previous life; it stays |
| JOIN_REQs and beacons sent again refused; replies to the node itself | T34: a member's JOIN_REQ recorded as it rejoins, sent again from a listening socket on the bridge | the master admitted it again, twice, and sent 16 datagrams to the bridge | not admitted again, nothing sent, its node_id kept |

The unit tests (*test/test.c*, 30 checks) cover what a cluster of four never reaches. The build before these changes fails 12 of them:

| Change | Check | Build before the change | With the change |
|---|---|---|---|
| A replay window starts from the first packet after a reset | a sender numbered past 16.7M, newly learned or after a reset | refused for as long as it runs | heard at once |
| | a replayed old packet taking the mark first | the live stream refused | through within three packets |
| A node that knows no term takes no absurd one | term 0xFFFFFFFF on a fresh node | accepted | refused |
| Node ids stay within 1..256 | every id in use or held | 257 handed out | a held id reused; none when all 256 are in use |

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
| the same 4.1 build with the module at 9c6f01644f (capability bits, statistics, events) | 51 of 51 | 51 of 51 | |
| the same 4.1 build with the module at 9b9846c5d2 (*release_tag_when_isolated* also covers a member cut off alone) | 51 of 51 | 51 of 51 | |
| the same 4.1 build with the module at e39633277d (input checks, JOIN_REJECT bound to its request) | 51 of 51 | 51 of 51 | |
| the same 4.1 build with the module at 7f88b8ebe7 (wire version 3, replay windows anchored afresh) | 51 of 51 | 51 of 51 | both clusters switched as a whole |
| the same 4.1 build with the module at the PR head bc2c8ace43 (replies to the node itself, JOIN_REQs and beacons sent again refused) | 51 of 51 | 51 of 51 | latest run, below |

Latest run, on the module exactly as in the PR: the killed master was replaced in 5.3 s (SBC) and 5.0 s (registrar); a stopped master handed over as soon as its GOODBYE arrived - 0.5 s on the registrars, 3.5 s on the SBCs, whose shutdown took that long to reach the GOODBYE; under 30% loss the member's learned wait peaked at 5.9 s and 6.4 s without an election; in R6c the restarted registrar dropped 3 packets that reached it before its identity and kept running; no refusal of any kind in any node's log.

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
