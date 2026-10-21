[clusterer_controller](../README.md) / Failure detection and elections

# Failure detection and elections

## Failure detection and elections

A fixed timeout either fires late or fires wrongly; the module learns from the network instead, and asks before it acts:

| Mechanism | What it does | Based on |
|---|---|---|
| Learned master watchdog | every member keeps the mean and deviation of the gaps between the master's keepalives and suspects the master only past mean + 4 deviations + a margin (3 s floor, 15 s cap); each false suspicion widens the margin, which then decays | Chen, Toueg and Aguilera, *On the Quality of Service of Failure Detectors* (DSN 2000); Hayashibara et al., *The φ Accrual Failure Detector* (SRDS 2004); TCP's estimator - Jacobson, *Congestion Avoidance and Control* (SIGCOMM 1988), RFC 6298 |
| Asking before electing | one multicast question, "who still hears the master?": anyone who does holds the election, two confirmations (every peer, when fewer) elect, silence means "maybe I am the one cut off" and one more period | Lifeguard's suspicion (next row) and the multi-observer agreement of Suresh et al., *Stable and Consistent Membership at Scale with Rapid* (USENIX ATC 2018) |
| Local health | a node in trouble - its own process stalled, timers late, the kernel dropping its packets, questions unanswered - counts 0..8 and multiplies its own watchdog and purge window by that count plus one (watchdog cap 30 s) | Dadgar, Phillips and Currey, *Lifeguard: Local Health Awareness for More Accurate Failure Detection* (arXiv:1707.00788) |
| PreVote | the term rises only on a confirmed election, a GOODBYE, a maintenance handover or a founding; a node that elected itself while cut off keeps its term and cannot take over the cluster at the heal | Ongaro, *Consensus: Bridging Theory and Practice* (PhD dissertation, Stanford, 2014), section 9.6 |
| Heal to the majority | masters are ranked on the members they hear, then term, then IP | - |
| Stall check | a node whose own process was paused does not judge its peers by the time it lost; a master that was paused stands down | - |
| Optional tag release | a master hearing fewer than half of its members for a whole election window gives up the active tag (*release_tag_when_isolated*, off by default) | Alquraan et al., *An Analysis of Network-Partitioning Failures in Cloud Systems* (OSDI 2018) |
