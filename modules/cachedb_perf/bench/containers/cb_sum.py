#!/usr/bin/env python3
"""Quick scenario summary from loader pickles + stats.jsonl."""
import glob, json, pickle, sys
R = sys.argv[1]
tot = 0; codes = {}; lats = []
for p in glob.glob(R + "/load-*.pkl"):
    d = pickle.load(open(p, "rb"))
    tot += d["total"]
    for b in d["buckets"].values():
        for c, n in b["codes"].items(): codes[c] = codes.get(c, 0) + n
        lats.extend(b["lat"])
lats.sort()
def pct(p): return lats[min(len(lats)-1, int(len(lats)*p))]
rows = [json.loads(l) for l in open(R + "/stats.jsonl")]
dur = rows[-1]["t"] - rows[0]["t"]
first, last = rows[0], rows[-1]
print("requests=%d  rate=%.0f/s  codes=%s" % (tot, tot/dur, codes))
print("latency us: p50=%.0f p95=%.0f p99=%.0f p999=%.0f max=%.0f"
      % (pct(.5), pct(.95), pct(.99), pct(.999), lats[-1]))
for n in ("n1","n2","n3"):
    a, b = first[n], last[n]
    dh = (b["hits"] or 0)-(a["hits"] or 0); dm = (b["misses"] or 0)-(a["misses"] or 0)
    cl_a, cl_b = a["cluster"], b["cluster"]
    dp = cl_b.get("pulls_requested",0)-cl_a.get("pulls_requested",0)
    dmc = b["mcast_tx"]-a["mcast_tx"]
    print("%s: hits+%d misses+%d pulls+%d mcast_tx+%d (%.1f pkt/s)"
          % (n, dh, dm, dp, dmc, dmc/dur))
