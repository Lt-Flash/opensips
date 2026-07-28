#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""clctr vs bin comparison charts, from the two tcpdump-captured runs."""
import glob, json, pickle
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

R = "/dn/cbench-results/results"
OUT = "/dn/cbench-results"
INK = "#1a1d21"; MUT = "#5c6570"; GRID = "#e3e8ee"
CC, CB = "#4269d0", "#c2410c"          # clctr blue, bin rust — fixed everywhere

plt.rcParams.update({
    "figure.facecolor": "white", "axes.facecolor": "white",
    "axes.edgecolor": GRID, "axes.labelcolor": INK,
    "text.color": INK, "xtick.color": MUT, "ytick.color": MUT,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6,
    "font.size": 10, "axes.titlesize": 12, "axes.titleweight": "bold",
    "axes.spines.top": False, "axes.spines.right": False,
    "legend.frameon": False,
})

RUNS = {"clctr": "s4_pkts", "bin": "s4_bin"}

def stats_rows(name):
    return [json.loads(l) for l in open("%s/%s/stats.jsonl" % (R, name))]

def load_buckets(name):
    merged = {}
    for p in glob.glob("%s/%s/load-*.pkl" % (R, name)):
        d = pickle.load(open(p, "rb"))
        for b, slot in d["buckets"].items():
            merged.setdefault(b, []).append(np.frombuffer(
                slot["lat"].tobytes(), dtype=np.float32))
    return {b: np.concatenate(v) for b, v in merged.items()}

BK = {k: load_buckets(v) for k, v in RUNS.items()}
ST = {k: stats_rows(v) for k, v in RUNS.items()}

def foot(fig, txt):
    fig.text(0.01, 0.01, txt, fontsize=7.5, color=MUT)
    fig.tight_layout(rect=(0, 0.03, 1, 1))

# ---------------------------------------------- 1. convergence overlay
fig, ax = plt.subplots(figsize=(8, 4.4), dpi=150)
fulls = {}
for k, col in (("clctr", CC), ("bin", CB)):
    rows = ST[k]
    t = np.array([r["t"] for r in rows])
    e = np.array([min(r[n]["entries"] for n in ("n1", "n2", "n3"))
                  for r in rows], dtype=float)
    ax.plot(t, e, color=col, lw=2,
            label="clctr — encrypted multicast" if k == "clctr"
                  else "bin — clusterer TCP mesh")
    fulls[k] = t[np.argmax(e >= 30000)]
ax.axhline(30000, color=MUT, lw=0.8, ls=":")
for k, col in (("clctr", CC), ("bin", CB)):
    ax.axvline(fulls[k], color=col, lw=0.8, ls=":")
    ax.text(fulls[k] + 0.2, 12500 if k == "clctr" else 10500,
            "%s: %.1f s" % (k, fulls[k]), fontsize=9, color=col)
ax.set_xlim(0, 20); ax.set_ylim(9000, 31800)
ax.set_xlabel("seconds under load")
ax.set_ylabel("keys held by the slowest node")
ax.set_title("Convergence, transport vs transport: same shape, multicast finishes first")
ax.legend(loc="lower right")
foot(fig, "identical thirds-seeded runs on the same 3-container cluster · only pull_transport differs · slowest node shown (all three converge together)")
fig.savefig(OUT + "/cmp-1-convergence.png"); plt.close(fig)

# ---------------------------------------------- 2. latency percentiles overlay
fig, ax = plt.subplots(figsize=(8, 4.4), dpi=150)
for k, col in (("clctr", CC), ("bin", CB)):
    bk = BK[k]
    bs = sorted(b for b in bk if 0 <= b < 300)
    bt = np.array([b / 10.0 for b in bs])
    p99 = np.array([np.percentile(bk[b], 99) for b in bs])
    p50 = np.array([np.percentile(bk[b], 50) for b in bs])
    ax.plot(bt, p99, color=col, lw=2, label="%s p99" % k)
    ax.plot(bt, p50, color=col, lw=1.2, alpha=0.55, label="%s p50" % k)
ax.set_yscale("log")
ax.set_yticks([100, 200, 500, 1000, 2000, 5000])
ax.set_yticklabels(["100 µs", "200 µs", "500 µs", "1 ms", "2 ms", "5 ms"])
ax.set_xlim(0, 30)
ax.set_xlabel("seconds under load"); ax.set_ylabel("request latency")
ax.set_title("Latency during convergence: BIN's miss phase is taller and longer")
ax.legend(loc="upper right", ncol=2)
foot(fig, "p50 (thin) and p99 (thick) per 100 ms · medians match — the transports differ only on the miss path")
fig.savefig(OUT + "/cmp-2-latency.png"); plt.close(fig)

# ---------------------------------------------- 3. wire cost overlay
fig, ax = plt.subplots(figsize=(8, 4.4), dpi=150)
tots = {}
for k, col in (("clctr", CC), ("bin", CB)):
    pk = np.loadtxt("%s/%s/pkts.txt" % (R, RUNS[k]),
                    dtype=[("t", "f8"), ("k", "U1")])
    sel = pk["k"] != "x"
    if k == "clctr":
        sel = (pk["k"] == "m") | (pk["k"] == "u")
        lbl = "clctr: multicast + unicast replies (UDP)"
    else:
        sel = pk["k"] == "b"
        lbl = "bin: TCP segments incl. ACKs"
    tt = pk["t"][sel] - pk["t"].min()
    edges = np.arange(0, 32, 0.5)
    rate = np.histogram(tt, bins=edges)[0] / 0.5
    ax.plot((edges[:-1] + edges[1:]) / 2, rate, color=col, lw=2, label=lbl)
    tots[k] = int(sel.sum())
ax.set_xlim(0, 32)
ax.set_xlabel("seconds"); ax.set_ylabel("packets per second on the bridge")
ax.set_title("Same misses on the wire: %s UDP pkts vs %s TCP segs"
             % (format(tots["clctr"], ","), format(tots["bin"], ",")))
ax.legend(loc="upper right")
ax.text(31.5, 30000,
        "per miss:\nclctr = 1 multicast + 2 replies = 3 packets\n"
        "bin = 2 unicast asks + replies + TCP ACKs ≈ 6.8 segments",
        ha="right", va="top", fontsize=9, color=MUT)
foot(fig, "same tcpdump filter window, same 60 000 misses · clctr replies are unicast UDP; BIN asks each peer separately over TCP")
fig.savefig(OUT + "/cmp-3-wire.png"); plt.close(fig)

# ---------------------------------------------- 4. throughput overlay
fig, ax = plt.subplots(figsize=(8, 4.4), dpi=150)
for k, col in (("clctr", CC), ("bin", CB)):
    bk = BK[k]
    bs = sorted(b for b in bk if 0 <= b < 300)
    bt = np.array([b / 10.0 for b in bs])
    rate = np.array([len(bk[b]) for b in bs]) * 10.0   # per 100ms -> per s
    ax.plot(bt, rate, color=col, lw=2,
            label="clctr" if k == "clctr" else "bin")
ax.set_xlim(0, 30); ax.set_ylim(0, 100000)
ax.set_yticks([0, 25000, 50000, 75000, 100000])
ax.set_yticklabels(["0", "25k", "50k", "75k", "100k"])
ax.set_xlabel("seconds under load")
ax.set_ylabel("requests served per second")
ax.set_title("Throughput while converging: blocking pulls cost BIN more workers")
ax.legend(loc="lower right")
foot(fig, "12 synchronous loaders, 6 workers per node · every miss blocks a worker for one pull round trip — BIN's slower pull bites twice")
fig.savefig(OUT + "/cmp-4-throughput.png"); plt.close(fig)

# summary numbers
for k in ("clctr", "bin"):
    bk = BK[k]
    bs = sorted(b for b in bk if 0 <= b < 300)
    allv = np.concatenate([bk[b] for b in bs])
    conv = np.concatenate([bk[b] for b in bs if b < 100])
    print("%s: full=%.1fs  reqs=%d  p50=%.0f  conv-p99=%.0f  wire=%d"
          % (k, fulls[k], len(allv), np.percentile(allv, 50),
             np.percentile(conv, 99), tots[k]))
