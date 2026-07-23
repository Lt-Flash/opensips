#!/usr/bin/env python3
"""cachedb_perf PR charts - measured data from modules/cachedb_perf/DESIGN.md"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "charts")
os.makedirs(OUT, exist_ok=True)

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
BLUE = "#2a78d6"     # slot 1 - cachedb_perf / hero
ORANGE = "#eb6834"   # slot 2
AQUA = "#1baf7a"     # slot 3
GRAY = "#b4b2aa"     # context bars (cachedb_local)

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["DejaVu Sans"],
    "figure.facecolor": SURFACE,
    "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE,
    "text.color": INK,
    "axes.edgecolor": AXIS,
    "axes.labelcolor": INK2,
    "xtick.color": MUTED,
    "ytick.color": INK2,
    "axes.grid": False,
})

def style_ax(ax, xgrid=True):
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)
    ax.spines["bottom"].set_color(AXIS)
    if xgrid:
        ax.xaxis.grid(True, color=GRID, linewidth=0.8, zorder=0)
    ax.tick_params(length=0)

def save(fig, name):
    fig.savefig(os.path.join(OUT, name), dpi=170, bbox_inches="tight",
                pad_inches=0.25)
    plt.close(fig)
    print("wrote", name)

# ---------------------------------------------------------------- chart 1
# Index structure shootout (bench/structs.c): ns per successful lookup,
# 50k keys, 16B keys, 200B values
labels = [
    "cachedb_perf 64B bucket + tags",
    "cachedb_local, perfectly sized (65536)",
    "sorted-array bucket (in-place fix)",
    "cachedb_local, hash cached in node",
    "cachedb_local, shipped default (512)",
]
vals = [84, 111, 134, 1837, 2484]
cols = [BLUE, GRAY, GRAY, GRAY, GRAY]

fig, ax = plt.subplots(figsize=(9.2, 4.4))
bars = ax.barh(labels, vals, color=cols, height=0.62, zorder=3)
style_ax(ax)
for b, v in zip(bars, vals):
    ax.text(v + 28, b.get_y() + b.get_height() / 2, f"{v:,} ns",
            va="center", ha="left", fontsize=10.5, color=INK)
ax.set_xlim(0, 2820)
ax.set_xlabel("ns per successful point lookup  (lower is better)", fontsize=10)
ax.set_title("Lookup cost: 30x against the shipped default, 1.3x against "
             "cachedb_local's best case",
             fontsize=12.5, loc="left",
             color=INK, pad=14, fontweight="bold")
ax.text(0, 1.02, "50 000 keys, 16-byte keys, 200-byte values - bench/structs.c",
        transform=ax.transAxes, fontsize=9.5, color=INK2)
save(fig, "01-structure-shootout.png")

# ---------------------------------------------------------------- chart 2
# Concurrency scaling (bench/concur.c): Mops/s, 100% read
threads = [1, 2, 4, 8]
cur = [8.57, 18.83, 36.13, 71.96]
new = [35.74, 69.35, 135.66, 288.95]

fig, ax = plt.subplots(figsize=(8.6, 4.8))
style_ax(ax, xgrid=False)
ax.yaxis.grid(True, color=GRID, linewidth=0.8, zorder=0)
ax.plot(threads, new, color=BLUE, linewidth=2, marker="o", markersize=7,
        zorder=3, label="cachedb_perf (lock-free reads)")
ax.plot(threads, cur, color=GRAY, linewidth=2, marker="o", markersize=7,
        zorder=3, label="cachedb_local (lock on every read)")
for x, y in zip(threads, new):
    ax.annotate(f"{y:.0f}", (x, y), textcoords="offset points",
                xytext=(0, 9), ha="center", fontsize=9.5, color=INK)
for x, y in zip(threads, cur):
    ax.annotate(f"{y:.0f}", (x, y), textcoords="offset points",
                xytext=(0, 9), ha="center", fontsize=9.5, color=INK2)
ax.set_xticks(threads)
ax.set_xlabel("threads", fontsize=10)
ax.set_ylabel("Mops/s, 100% read", fontsize=10)
ax.set_xlim(0.6, 8.6)
ax.set_ylim(0, 330)
ax.legend(frameon=False, fontsize=10, loc="upper left")
ax.set_title("Both scale linearly (8.4x vs 8.1x at 8 threads) - the 4x is a "
             "per-operation constant factor",
             fontsize=12.5, loc="left", color=INK, pad=14, fontweight="bold")
ax.text(0, 1.02, "65 536 buckets, 50 000 keys - bench/concur.c; "
        "the lock is uncontended, not the bottleneck",
        transform=ax.transAxes, fontsize=9.5, color=INK2)
save(fig, "02-concurrency-scaling.png")

# ---------------------------------------------------------------- chart 3
# Memory backing tiers (bench/hugetlb2.c, kernel 6.8)
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11.6, 4.2))

t_labels = ["MAP_HUGETLB\n(overcommit pool)", "MADV_COLLAPSE\n(post-fill)",
            "MADV_HUGEPAGE\n(shmem THP)", "plain 4K pages"]
t_vals = [125, 156, 158, 177]
t_cols = [BLUE, GRAY, GRAY, GRAY]
bars = ax1.barh(t_labels, t_vals, color=t_cols, height=0.6, zorder=3)
style_ax(ax1)
for b, v in zip(bars, t_vals):
    ax1.text(v + 3, b.get_y() + b.get_height() / 2, f"{v} ns",
             va="center", fontsize=10.5, color=INK)
ax1.set_xlim(0, 205)
ax1.invert_yaxis()
ax1.set_xlabel("dependent pointer chase, ns  (lower is better)", fontsize=10)
ax1.set_title("Chase latency by backing: 1.42x", fontsize=11.5, loc="left",
              color=INK, fontweight="bold", pad=10)

p_labels = ["hugetlb fault\n(2M granularity)", "MADV_POPULATE_WRITE\n(5.14+)",
            "touch per 4K page", "memset (never)"]
p_vals = [85, 177, 291, 1414]
p_cols = [BLUE, GRAY, GRAY, GRAY]
bars = ax2.barh(p_labels, p_vals, color=p_cols, height=0.6, zorder=3)
style_ax(ax2)
for b, v in zip(bars, p_vals):
    ax2.text(v + 22, b.get_y() + b.get_height() / 2, f"{v:,} ms",
             va="center", fontsize=10.5, color=INK)
ax2.set_xlim(0, 1650)
ax2.invert_yaxis()
ax2.set_xlabel("pre-fault of 256 MB, ms  (lower is better)", fontsize=10)
ax2.set_title("Pre-fault cost: 4-5x cheaper at 2M", fontsize=11.5, loc="left",
              color=INK, fontweight="bold", pad=10)

fig.suptitle("Modern-kernel memory backing, measured on 6.8 - the module "
             "probes each route at startup by trying it",
             fontsize=12.5, x=0.045, ha="left", color=INK, fontweight="bold")
fig.subplots_adjust(top=0.80, wspace=0.55)
save(fig, "03-memory-backing.png")

# ---------------------------------------------------------------- chart 4
# Read-path protocols (bench/rpath.c), 8 threads
mixes = [
    ("100% read, uniform", [162.1, 157.4, 166.2]),
    ("95/5 read/write, uniform", [112.7, 120.8, 125.6]),
    ("50/50, one hot key", [4.66, 6.13, 6.77]),
]
proto_names = ["seqlock\n(as designed)", "+ versionless\nTTL bump", "QSBR\n(rejected)"]
proto_cols = [BLUE, AQUA, GRAY]

fig, axes = plt.subplots(1, 3, figsize=(11.6, 4.0))
for ax, (mix, vals2) in zip(axes, mixes):
    bars = ax.bar(range(3), vals2, color=proto_cols, width=0.62, zorder=3)
    style_ax(ax, xgrid=False)
    ax.yaxis.grid(True, color=GRID, linewidth=0.8, zorder=0)
    for i, (b, v) in enumerate(zip(bars, vals2)):
        ax.text(b.get_x() + b.get_width() / 2, v * 1.02,
                f"{v:g}", ha="center", va="bottom", fontsize=10, color=INK)
    ax.set_xticks(range(3))
    ax.set_xticklabels(proto_names, fontsize=8.6, color=INK2)
    ax.set_ylim(0, max(vals2) * 1.18)
    ax.set_title(mix, fontsize=10.5, color=INK2, pad=8)
axes[0].set_ylabel("Mops/s, 8 threads", fontsize=10)
fig.suptitle("Read-path protocols: the seqlock costs nothing at 100% reads; "
             "the versionless TTL bump keeps most of QSBR's win for free",
             fontsize=12.5, x=0.045, ha="left", color=INK, fontweight="bold")
fig.subplots_adjust(top=0.78, wspace=0.32)
save(fig, "04-read-protocols.png")

# ---------------------------------------------------------------- chart 5
# Write staging (bench/wbuf.c): Mops/s at 1 vs 8 threads
fig, ax = plt.subplots(figsize=(8.6, 4.8))
style_ax(ax, xgrid=False)
ax.yaxis.grid(True, color=GRID, linewidth=0.8, zorder=0)
series = [
    ("per-process buffers", [35.76, 291.72], AQUA),
    ("per-bucket locks (direct)", [20.16, 130.12], BLUE),
    ("shared staging buffer", [25.67, 18.52], ORANGE),
]
for name, ys, c in series:
    ax.plot([1, 8], ys, color=c, linewidth=2, marker="o", markersize=7,
            zorder=3, label=name)
    ax.annotate(f"{ys[1]:g}", (8, ys[1]), textcoords="offset points",
                xytext=(10, -3), fontsize=10, color=INK)
ax.set_xticks([1, 8])
ax.set_xlabel("threads", fontsize=10)
ax.set_ylabel("applied writes, Mops/s", fontsize=10)
ax.set_xlim(0, 9.6)
ax.set_ylim(0, 330)
ax.legend(frameon=False, fontsize=10, loc="upper left")
ax.set_title("Write staging: one shared atomic append head scales NEGATIVELY "
             "(0.72x) - coordination concentrated, not removed",
             fontsize=12.5, loc="left", color=INK, pad=14, fontweight="bold")
ax.text(0, 1.02, "bench/wbuf.c - why the arena allocates from per-process "
        "chunks and the table keeps per-bucket locks",
        transform=ax.transAxes, fontsize=9.5, color=INK2)
save(fig, "05-write-staging.png")

print("all charts written to", OUT)
