#!/usr/bin/env python3
"""Sampler: perf_stats from each node + per-container multicast packet
counters (host-side veth statistics), every 0.25 s, to JSONL."""
import json, socket, subprocess, sys, time

SECS = float(sys.argv[1])
OUT = sys.argv[2]
T0 = float(sys.argv[3])

def mi(n, method, params=None):
    c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    c.settimeout(3)
    try:
        c.sendto(json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
            "params": params if params is not None else []}).encode(),
            ("10.94.0.1%d" % n, 8787))
        return json.loads(c.recv(262144)).get("result", {})
    except Exception:
        return {}
    finally:
        c.close()

# map each container to its host-side veth via eth0's iflink
veth = {}
links = subprocess.run(["ip", "-o", "link"], capture_output=True,
                       text=True).stdout
for n in (1, 2, 3):
    idx = subprocess.run(["nerdctl", "exec", "n%d" % n, "cat",
                          "/sys/class/net/eth0/iflink"],
                         capture_output=True, text=True).stdout.strip()
    for line in links.splitlines():
        if line.startswith(idx + ":"):
            veth[n] = line.split(":")[1].strip().split("@")[0]
print("veths:", veth)

def mcast(n):
    try:
        with open("/sys/class/net/%s/statistics/multicast" % veth[n]) as f:
            return int(f.read())
    except Exception:
        return -1

out = open(OUT, "w")
end = T0 + SECS
while time.time() < end:
    t = time.time() - T0
    row = {"t": round(t, 3)}
    for n in (1, 2, 3):
        st = mi(n, "perf_stats")
        th = next((c for c in st.get("collections", [])
                   if c.get("name") == "th"), {})
        row["n%d" % n] = {
            "entries": th.get("entries"),
            "hits": th.get("hits"),
            "misses": th.get("misses"),
            "cluster": st.get("cluster", {}),
            "mcast_tx": mcast(n),
        }
    out.write(json.dumps(row) + "\n")
    out.flush()
    time.sleep(0.25)
out.close()
print("collector done")
