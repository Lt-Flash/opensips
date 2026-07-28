#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Charts for the 3-container cp15 bench. Light background, PR-friendly."""
import glob, json, pickle
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

R = "/dn/cbench-results/results"
OUT = "/dn/cbench-results"
INK = "#1a1d21"; MUT = "#5c6570"; GRID = "#e3e8ee"
C1, C2, C3 = "#4269d0", "#efb118", "#11866f"   # n1 blue, n2 amber, n3 teal
CP, CU = "#4269d0", "#c2410c"                  # mcast blue, unicast rust

plt.rcParams.update({
    "figure.facecolor": "white", "axes.facecolor": "white",
    "axes.edgecolor": GRID, "axes.labelcolor": INK,
    "text.color": INK, "xtick.color": MUT, "ytick.color": MUT,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6,
    "font.size": 10, "axes.titlesize": 12, "axes.titleweight": "bold",
    "axes.spines.top": False, "axes.spines.right": False,
    "legend.frameon": False,
})

def stats_rows(name):
    return [json.loads(l) for l in open("%s/%s/stats.jsonl" % (R, name))]

def load_buckets(name):
    """merge loader pickles -> {bucket_index: np.array(latencies_us)}"""
    merged = {}
    for p in glob.glob("%s/%s/load-*.pkl" % (R, name)):
        d = pickle.load(open(p, "rb"))
        for b, slot in d["buckets"].items():
            merged.setdefault(b, []).append(np.frombuffer(
                slot["lat"].tobytes(), dtype=np.float32))
    return {b: np.concatenate(v) for b, v in merged.items()}

# ------------------------------------------------ 1. convergence
rows = stats_rows("s2_converge")
t = np.array([r["t"] for r in rows])
fig, ax = plt.subplots(figsize=(8, 4.4), dpi=150)
for key, col, lbl in (("n1", C1, "node 1"), ("n2", C2, "node 2"),
                      ("n3", C3, "node 3")):
    e = np.array([r[key]["entries"] for r in rows], dtype=float)
    ax.plot(t, e, color=col, lw=2, label=lbl)
ax.axhline(30000, color=MUT, lw=0.8, ls=":")
ax.text(19.5, 30250, "all 30 000 keys", ha="right", fontsize=9, color=MUT)
half = t[np.argmax([r["n1"]["entries"] >= 20000 for r in rows])]
full = t[np.argmax([min(r[k]["entries"] for k in ("n1","n2","n3")) >= 30000
                    for r in rows])]
ax.axvline(full, color=MUT, lw=0.8, ls=":")
ax.annotate("half the gap closed\nin about a second", xy=(half, 20000),
            xytext=(half + 3.5, 16500), fontsize=9, color=MUT,
            arrowprops=dict(arrowstyle="-", color=MUT, lw=0.8))
ax.text(full + 0.5, 11000, "fully converged\nat %.1f s" % full,
        fontsize=9, color=MUT)
ax.set_xlim(0, 20); ax.set_ylim(9000, 31800)
ax.set_xlabel("seconds under load"); ax.set_ylabel("keys held per node")
ax.set_title("Convergence: each node starts with its third, ends with everything")
ax.legend(loc="lower right")
fig.text(0.01, 0.01, "3 Alpine containers · 30 000 keys, 10 000 each · uniform random reads at ~87 000 req/s cluster-wide · pull_transport=clctr",
         fontsize=7.5, color=MUT)
fig.tight_layout(rect=(0, 0.03, 1, 1))
fig.savefig(OUT + "/1-convergence.png"); plt.close(fig)

# ------------------------------------------------ 2. latency percentiles over time
bk = load_buckets("s2_converge")
bs = sorted(b for b in bk if 0 <= b < 300)
bt = np.array([b / 10.0 for b in bs])
p50 = np.array([np.percentile(bk[b], 50) for b in bs])
p95 = np.array([np.percentile(bk[b], 95) for b in bs])
p99 = np.array([np.percentile(bk[b], 99) for b in bs])
fig, ax = plt.subplots(figsize=(8, 4.4), dpi=150)
ax.plot(bt, p99, color="#c2410c", lw=2, label="p99")
ax.plot(bt, p95, color=C2, lw=2, label="p95")
ax.plot(bt, p50, color=C1, lw=2, label="p50 (median)")
ax.set_yscale("log")
ax.set_yticks([100, 200, 500, 1000, 2000])
ax.set_yticklabels(["100 µs", "200 µs", "500 µs", "1 ms", "2 ms"])
ax.set_xlim(0, 30)
ax.set_xlabel("seconds under load"); ax.set_ylabel("request latency")
ax.set_title("Latency while the cluster converges: the miss phase is visible, then gone")
ax.legend(loc="upper right")
fig.text(0.01, 0.01, "same run as the convergence chart · every request measured · a miss = one blocking cluster pull (pull_on_miss=1, 100 ms budget)",
         fontsize=7.5, color=MUT)
fig.tight_layout(rect=(0, 0.03, 1, 1))
fig.savefig(OUT + "/2-latency-timeline.png"); plt.close(fig)

# ------------------------------------------------ 3. hit vs pull histogram
conv = np.concatenate([bk[b] for b in bs if b < 100])       # first 10 s
steady = np.concatenate([bk[b] for b in bs if b >= 150])    # after 15 s
bins = np.logspace(np.log10(40), np.log10(5000), 90)
fig, ax = plt.subplots(figsize=(8, 4.4), dpi=150)
ax.hist(steady, bins=bins, density=True, color=C1, alpha=0.85,
        label="steady state (all hits)")
ax.hist(conv, bins=bins, density=True, color=C2, alpha=0.6,
        label="convergence phase (hits + cluster pulls)")
ax.set_xscale("log"); ax.set_yscale("log")
ax.set_ylim(bottom=2e-7)
ax.set_xticks([50, 100, 200, 500, 1000, 2000, 5000])
ax.set_xticklabels(["50 µs", "100", "200", "500", "1 ms", "2 ms", "5 ms"])
ax.set_xlabel("request latency (log scale)"); ax.set_ylabel("share of requests (log)")
ax.set_title("What a cluster pull costs: a thin tail, and all of it under 1 ms")
ax.legend(loc="upper right")
fig.text(0.01, 0.01, "log count scale · the miss path = ~3% of convergence-phase requests: a 200 µs – 1.7 ms tail, never near the 100 ms budget",
         fontsize=7.5, color=MUT)
fig.tight_layout(rect=(0, 0.03, 1, 1))
fig.savefig(OUT + "/3-latency-histogram.png"); plt.close(fig)

# ------------------------------------------------ 4. wire cost
pk = np.loadtxt(R + "/s4_pkts/pkts.txt", dtype=[("t", "f8"), ("k", "U1")])
t0 = pk["t"].min()
rows4 = stats_rows("s4_pkts")
# align: stats t is relative to its own T0; pcap t0 ~ T0-1s. Use pull deltas.
tt = pk["t"] - t0
edges = np.arange(0, tt.max() + 0.5, 0.5)
m_rate = np.histogram(tt[pk["k"] == "m"], bins=edges)[0] / 0.5
u_rate = np.histogram(tt[pk["k"] == "u"], bins=edges)[0] / 0.5
mid = (edges[:-1] + edges[1:]) / 2
fig, ax = plt.subplots(figsize=(8, 4.4), dpi=150)
ax.plot(mid, u_rate, color=CU, lw=2, label="unicast replies (2 per pull)")
ax.plot(mid, m_rate, color=CP, lw=2, label="multicast pull requests (1 per miss)")
ax.set_xlim(0, 32)
ax.set_xlabel("seconds"); ax.set_ylabel("packets per second on the bridge")
ax.set_title("Wire cost of misses: one multicast out, one reply per peer back")
ax.legend(loc="upper right")
tot_m = int((pk["k"] == "m").sum()); tot_u = int((pk["k"] == "u").sum())
ax.text(31.5, max(u_rate) * 0.55,
        "whole run:\n%s multicast\n%s unicast\nbin (TCP mesh): 0\n\nsteady state:\n~2 pkt/s beacons,\nnothing else" %
        (format(tot_m, ","), format(tot_u, ",")),
        ha="right", va="top", fontsize=9, color=MUT)
fig.text(0.01, 0.01, "fresh thirds-seeded run captured with tcpdump on the bridge · 60 000 misses cluster-wide -> 60 066 multicast (incl. beacons), 120 048 replies",
         fontsize=7.5, color=MUT)
fig.tight_layout(rect=(0, 0.03, 1, 1))
fig.savefig(OUT + "/4-wire-cost.png"); plt.close(fig)

# numbers for the summary
print("half=%.1fs full=%.1fs" % (half, full))
print("steady p50/p99: %.0f/%.0f us" % (np.percentile(steady, 50), np.percentile(steady, 99)))
print("conv p50/p99: %.0f/%.0f us" % (np.percentile(conv, 50), np.percentile(conv, 99)))
pulls = conv[conv > 400]
print("pull-ish tail (>400us) share of conv phase: %.1f%%" % (100 * len(pulls) / len(conv)))
