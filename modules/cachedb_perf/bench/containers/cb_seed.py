#!/usr/bin/env python3
"""Seed keys via one X-Seed request per node; verify entry counts."""
import json, socket, sys, time
def mi(n, method, params=None):
    c=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); c.settimeout(5)
    c.sendto(json.dumps({"jsonrpc":"2.0","id":1,"method":method,
        "params":params if params is not None else []}).encode(),("10.94.0.1%d"%n,8787))
    r=json.loads(c.recv(262144)).get("result",{}); c.close(); return r
def seed(n, frm, cnt):
    s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("10.94.0.1",0)); p=s.getsockname()[1]; s.settimeout(60)
    m=("OPTIONS sip:seed@10.94.0.1%d:5060 SIP/2.0\r\n"
       "Via: SIP/2.0/UDP 10.94.0.1:%d;branch=z9hG4bK-seed%d\r\n"
       "From: <sip:s@x>;tag=s%d\r\nTo: <sip:s@x>\r\nCall-ID: seed-%d-%d\r\n"
       "CSeq: 1 OPTIONS\r\nX-Seed: %d\r\nX-Cnt: %d\r\n"
       "Max-Forwards: 5\r\nContent-Length: 0\r\n\r\n")%(n,p,n,n,n,frm,frm,cnt)
    s.sendto(m.encode(),("10.94.0.1%d"%n,5060))
    code=s.recvfrom(2048)[0].split(b" ",2)[1]; s.close(); return code
mode=sys.argv[1]
for n in (1,2,3):
    mi(n,"perf_del",{"glob":"th:*","collection":"th"})
if mode=="all":
    for n in (1,2,3): print("n%d seed:"%n, seed(n,0,30000))
elif mode=="thirds":
    for n,f in ((1,0),(2,10000),(3,20000)): print("n%d seed %d..:"%(n,f), seed(n,f,10000))
time.sleep(0.5)
for n in (1,2,3):
    st=mi(n,"perf_stats")
    th=next(c for c in st["collections"] if c["name"]=="th")
    print("n%d entries:"%n, th["entries"])
