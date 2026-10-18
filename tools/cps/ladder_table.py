#!/usr/bin/env python3
import sys, os, json, glob, statistics as st
RES=sys.argv[1]; MODES=['shared','auto','exclusive']
cells={}
for f in glob.glob(os.path.join(RES,'*_*_r*/sum.json')):
    rate,mode,rep=os.path.basename(os.path.dirname(f)).split('_')
    cells.setdefault((int(rate),mode),[]).append(json.load(open(f)))
rates=sorted({r for r,_ in cells})
def m(xs):
    xs=[x for x in xs if x is not None and x==x]; return st.mean(xs) if xs else float('nan')
def holds(c):
    sent=c['offered']*30
    return c['screens']==12 and c['fail']<=0.001*max(c['ok']+c['fail'],1) and c['ok']/30.0>=0.98*c['offered']
def cv(w):
    w=[x for x in w if x>0]; return st.pstdev(w)/st.mean(w)*100 if len(w)>1 and st.mean(w)>0 else float('nan')
print("| offered calls/s | mode | completed calls/s | failed calls | CPU % per 1000 calls/s | context switches / call | syscalls / call | wasted EAGAIN reads / call | epoll_wait / call | futex / call | UDP rcvbuf drops | 200 OK p99 | worker CPU spread (cv) | holds (both reps) |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
best={}
for r in rates:
    for mo in MODES:
        cs=cells.get((r,mo),[])
        if not cs: continue
        h=all(holds(c) for c in cs) and len(cs)>=2
        if h: best[mo]=max(best.get(mo,0), r)
        fail=sum(c['fail'] for c in cs); tot=sum(c['ok']+c['fail'] for c in cs)
        p99=max([c['p99'] for c in cs if c['p99']] or [0])
        print(f"| {r} | {mo} | {m([c['ok']/30.0 for c in cs]):.0f} | {fail} ({fail/tot*100 if tot else 0:.2f}%) | {m([c['cpu_per_kcps'] for c in cs]):.1f} | {m([c['cs_per_call'] for c in cs]):.2f} | {m([c['sys_per_call'] for c in cs]):.1f} | {m([c['eagain_per_call'] for c in cs]):.2f} | {m([c['epoll_per_call'] for c in cs]):.2f} | {m([c['futex_per_call'] for c in cs]):.2f} | {sum(c['rcvbuferr'] for c in cs if c['rcvbuferr']==c['rcvbuferr']):.0f} | <{p99} ms | {m([cv(c['workers']) for c in cs]):.0f}% | {'yes' if h else 'NO'} |")
print()
print("| mode | highest sustained rate (calls/s) | vs shared |")
print("|---|---|---|")
for mo in MODES:
    b=best.get(mo); s=best.get('shared')
    print(f"| {mo} | {b if b else 'none held'} | {'' if mo=='shared' or not (b and s) else f'{(b-s)/s*100:+.0f}%'} |")
