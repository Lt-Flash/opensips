#!/usr/bin/env python3
# cellsum_tls.py <celldir> <offered> <D> -> one line + sum.json ; per-call metrics normalised by completed calls
import sys, os, re, glob, json
O, rate, D = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
ok=fail=retr=0; nscr=0
for f in glob.glob(os.path.join(O,'sipp_*.txt')):
    s=open(f, errors='replace').read()
    if 'Successful call' in s: nscr+=1
    g=lambda rx: int((re.search(rx,s) or [0,'0'])[1])
    ok+=g(r'Successful call\s*\|\s*\d+\s*\|\s*(\d+)'); fail+=g(r'Failed call\s*\|\s*\d+\s*\|\s*(\d+)')
    retr+=sum(int(x) for x in re.findall(r'^\s+(?:INVITE|BYE) ---------->\s+\d+\s+(\d+)', s, re.M))
rep={}
for f in glob.glob(os.path.join(O,'sipp_*.txt')):
    s2=open(f, errors='replace').read()
    for lo,hi,nn in re.findall(r'(\d+) ms <= n <\s+(\d+) ms :\s+(\d+)', s2): rep[int(hi)]=rep.get(int(hi),0)+int(nn)
    mm=re.search(r'n >=\s+(\d+) ms\s+:\s+(\d+)', s2)
    if mm: rep[10**9]=rep.get(10**9,0)+int(mm.group(2))
def pct(p):
    tot=sum(rep.values()); acc=0
    for hi in sorted(rep):
        acc+=rep[hi]
        if tot and acc>=p*tot: return hi
    return None
P={}
if os.path.exists(os.path.join(O,'perf.csv')):
    for l in open(os.path.join(O,'perf.csv')):
        f=l.strip().split(',')
        if len(f)>3 and f[0] and f[0][0].isdigit(): P[f[2].replace('syscalls:sys_enter_','').replace('syscalls:sys_exit_recvfrom','eagain')]=float(f[0])
pr=open(os.path.join(O,'probe.txt')).read() if os.path.exists(os.path.join(O,'probe.txt')) else ''
g=lambda rx: float((re.search(rx,pr) or [0,'nan'])[1])
win=g(r'window ([\d.]+)s'); cpu=g(r'opensips_cpu ([\d.]+)%')
m=re.search(r'group TCP receiver: n=(\d+) sum=([\d.]+)% min=([\d.]+) max=([\d.]+) cv=(\d+)% \[(.*)\]', pr)
tw=dict(n=int(m.group(1)), sum=float(m.group(2)), min=float(m.group(3)), max=float(m.group(4)), cv=int(m.group(5)), cpus=m.group(6)) if m else {}
tm=re.search(r'^\s*\d+\s+\d+\s+([\d.]+)\s+\d+\s+TCP main', pr, re.M); tmain=float(tm.group(1)) if tm else float('nan')
completed=ok/D
calls_win=completed*win if win==win else float('nan')
per=lambda k: round(P.get(k,float('nan'))/calls_win,2) if calls_win else float('nan')
cli=open(os.path.join(O,'clientcpu.txt')).read() if os.path.exists(os.path.join(O,'clientcpu.txt')) else ''
smax=max([int(x) for x in re.findall(r'sipp_max=(\d+)',cli)] or [0]); gmax=max([int(x) for x in re.findall(r'gw_max=(\d+)',cli)] or [0])
mode=open(os.path.join(O,'mode.txt')).read().strip() if os.path.exists(os.path.join(O,'mode.txt')) else ''
se=open(os.path.join(O,'server_errors.txt'),errors='replace').read() if os.path.exists(os.path.join(O,'server_errors.txt')) else ''
ddrops=sum(int(m.group(1)) for m in re.finditer(r'^\s*(\d+) .*failed to dispatch TCP message', se, re.M))
serr=sum(int(m.group(1)) for m in re.finditer(r'^\s*(\d+) ', se, re.M))
S=dict(offered=rate, ok=ok, fail=fail, retrans=retr, screens=nscr, completed=round(completed,1), cpu=cpu,
       cpu_per_kcps=round(cpu/completed*1000,2) if completed else None, tcp_main_cpu=tmain, tcp_workers=tw,
       cs_per_call=per('context-switches'), sys_per_call=per('raw_syscalls:sys_enter'), yield_per_call=per('sched_yield'),
       read_per_call=per('read'), write_per_call=per('write'), recv_per_call=per('recvfrom'), eagain_per_call=per('eagain'),
       epoll_per_call=per('epoll_wait'), client_sipp_max=smax, client_gw_max=gmax, p50=pct(.5), p99=pct(.99), p999=pct(.999), dispatch_drops=ddrops, server_errors=serr, mode=mode)
S['holds']= bool(nscr>0 and fail<=0.001*max(ok+fail,1) and completed>=0.98*rate)
json.dump(S, open(os.path.join(O,'sum.json'),'w'))
print(f"{'HOLDS' if S['holds'] else 'fails'} ok {ok} fail {fail} retr {retr} done {completed:.0f}/s cpu {cpu:.0f}% = {S['cpu_per_kcps']}%/kcps tcpmain {tmain:.0f}% workers cv {tw.get('cv','?')}% [{tw.get('cpus','')}] | cs {S['cs_per_call']} sys {S['sys_per_call']} yield {S['yield_per_call']} eagain {S['eagain_per_call']} | p50<{S['p50']} p99<{S['p99']} p99.9<{S['p999']} ms | dispatch drops {ddrops} server errs {serr} | client sipp {smax}% gw {gmax}% | screens {nscr} | {mode[:70]}")
