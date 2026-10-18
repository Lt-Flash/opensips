#!/usr/bin/env python3
# cellsum.py <celldir> <offered total cps> <secs> -> one line; also writes sum.json
import sys, os, re, glob, json
O, rate, D = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
ok=fail=retr=tmo=0; tt=0.0; rt=[]; rep={}
for f in glob.glob(os.path.join(O,'sipp_*.txt')):
    s=open(f, errors='replace').read()
    g=lambda rx: int((re.search(rx,s) or [0,'0'])[1])
    ok+=g(r'Successful call\s*\|\s*\d+\s*\|\s*(\d+)'); fail+=g(r'Failed call\s*\|\s*\d+\s*\|\s*(\d+)')
    m=re.findall(r'^\s+(?:INVITE|BYE) ---------->\s+\d+\s+(\d+)', s, re.M); retr+=sum(int(x) for x in m)
    m=re.findall(r'(\d+):(\d+):(\d+):(\d+)\s+\|\s*\d+\s+[\d.]+ cps', s)
    tm=re.search(r'\s([\d.]+) s\s+\d+\s+[\d.]+:\d+', s)
    if tm: tt=max(tt, float(tm.group(1)))
    for lo,hi,n in re.findall(r'(\d+) ms <= n <\s+(\d+) ms :\s+(\d+)', s): rep[int(hi)]=rep.get(int(hi),0)+int(n)
    m=re.search(r'n >=\s+(\d+) ms\s+:\s+(\d+)', s)
    if m: rep[10**9]=rep.get(10**9,0)+int(m.group(2))
def pct(p):
    tot=sum(rep.values()); acc=0
    for hi in sorted(rep):
        acc+=rep[hi]
        if tot and acc>=p*tot: return hi
    return None
P={}
pf=os.path.join(O,'perf.csv')
if os.path.exists(pf):
    for l in open(pf):
        f=l.strip().split(',')
        if len(f)>3 and f[0] and f[0][0].isdigit(): P[f[2]]=float(f[0])
pr=open(os.path.join(O,'probe.txt')).read() if os.path.exists(os.path.join(O,'probe.txt')) else ''
g=lambda rx: float((re.search(rx,pr) or [0,'nan'])[1])
win=g(r'window ([\d.]+)s'); pin=g(r'UDP in (\d+)/s'); cpu=g(r'total opensips cpu ([\d.]+)%'); rbe=g(r'rcvbuferr (\d+)'); si=g(r'softirq ([\d.]+)%')
wk=[float(x) for x in re.findall(r'^\s*\d+\s+\d+\s+([\d.]+)\s+\d+\s+\d+\s*$', pr, re.M)]
wk=sorted(wk, reverse=True)[:8]
cps_win = pin/6.0 if pin==pin else float('nan')   # 6 datagrams in per call (INVITE,180,200,ACK,BYE,200)
achieved = ok/tt if tt else 0
calls_win = cps_win*win if win==win else float('nan')
per=lambda k: P.get(k, float('nan'))/calls_win if calls_win else float('nan')
cli=open(os.path.join(O,'clientcpu.txt')).read().split() if os.path.exists(os.path.join(O,'clientcpu.txt')) else []
climax=max([int(x) for x in cli if x.isdigit()] or [0])
nscr=sum(1 for f in glob.glob(os.path.join(O,'sipp_*.txt')) if 'Successful call' in open(f,errors='replace').read())
mode=open(os.path.join(O,'mode.txt')).read().replace('\n',' | ').strip() if os.path.exists(os.path.join(O,'mode.txt')) else ''
S=dict(offered=rate, ok=ok, fail=fail, retrans=retr, achieved=round(achieved,1), runtime=tt, p50=pct(.5), p99=pct(.99),
       udp_in=pin, cps_window=round(cps_win,1), cpu=cpu, cpu_per_kcps=round(cpu/cps_win*1000,2) if cps_win else None,
       cs_per_call=round(per('context-switches'),2), sys_per_call=round(per('raw_syscalls:sys_enter'),2),
       recv_per_call=round(per('syscalls:sys_enter_recvfrom'),2), eagain_per_call=round(per('syscalls:sys_exit_recvfrom'),2),
       epoll_per_call=round(per('syscalls:sys_enter_epoll_wait'),2), futex_per_call=round(per('syscalls:sys_enter_futex'),2),
       sendto_per_call=round(per('syscalls:sys_enter_sendto'),2), taskclock_ms_per_call=round(per('task-clock'),4),
       rcvbuferr=rbe, softirq=si, screens=nscr, workers=wk, client_max_cpu=climax, mode=mode)
json.dump(S, open(os.path.join(O,'sum.json'),'w'))
print(f"ok {ok} fail {fail} retr {retr} achieved {achieved:.0f}/s (window {cps_win:.0f}/s) cpu {cpu:.0f}% = {S['cpu_per_kcps']}%/kcps | cs/call {S['cs_per_call']} sys/call {S['sys_per_call']} recv {S['recv_per_call']} eagain {S['eagain_per_call']} epoll {S['epoll_per_call']} | rbe {rbe:.0f} p50<{S['p50']}ms p99<{S['p99']}ms | client max {climax}% | screens {nscr} | {mode[:60]}")
