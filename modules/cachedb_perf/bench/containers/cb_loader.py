#!/usr/bin/env python3
"""Load generator: each process hammers one node with OPTIONS carrying a
random key; the node's route does cache_fetch (blocking pull on miss).
Latencies land in 100 ms buckets as float32 arrays; each process dumps a
pickle at the end."""
import array, os, pickle, random, socket, sys, time

NODE = sys.argv[1]            # 10.94.0.11 / .12 / .13
SECS = float(sys.argv[2])
NKEYS = int(sys.argv[3])
OUT = sys.argv[4]             # output pickle path
T0 = float(sys.argv[5])       # shared epoch so buckets align across procs

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("10.94.0.1", 0))      # host side of the cp15 bridge
port = s.getsockname()[1]
s.settimeout(2.0)

TPL = ("OPTIONS sip:bench@%s:5060 SIP/2.0\r\n"
       "Via: SIP/2.0/UDP 10.94.0.1:%d;branch=z9hG4bK-%%d\r\n"
       "From: <sip:l@x>;tag=%%d\r\nTo: <sip:b@x>\r\n"
       "Call-ID: cb-%d-%%d\r\nCSeq: 1 OPTIONS\r\n"
       "X-K: k%%d\r\nMax-Forwards: 5\r\nContent-Length: 0\r\n\r\n"
       ) % (NODE, port, os.getpid())
DST = (NODE, 5060)

buckets = {}   # int(t*10) -> {"lat": array('f'), "codes": {code: n}}
rng = random.Random(os.getpid())
end = T0 + SECS
n = 0
while True:
    now = time.time()
    if now >= end:
        break
    k = rng.randrange(NKEYS)
    n += 1
    msg = (TPL % (n, n, n, k)).encode()
    t1 = time.perf_counter()
    try:
        s.sendto(msg, DST)
        d, _ = s.recvfrom(2048)
        code = int(d.split(b" ", 2)[1])
    except socket.timeout:
        code = 0
    lat = (time.perf_counter() - t1) * 1e6   # us
    b = int((now - T0) * 10)
    slot = buckets.get(b)
    if slot is None:
        slot = buckets[b] = {"lat": array.array("f"), "codes": {}}
    slot["lat"].append(lat)
    slot["codes"][code] = slot["codes"].get(code, 0) + 1

with open(OUT, "wb") as f:
    pickle.dump({"node": NODE, "buckets": buckets, "total": n}, f)
print("%s pid=%d sent=%d" % (NODE, os.getpid(), n))
