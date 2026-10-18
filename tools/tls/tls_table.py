#!/usr/bin/env python3
# tls_table.py <results-dir> [label]
import sys, os, glob, json, re, statistics as st
RES=sys.argv[1]; LABEL=sys.argv[2] if len(sys.argv)>2 else os.path.basename(RES)
MODES=['shared','auto','exclusive']
cells={}
for f in glob.glob(os.path.join(RES,'*_*_r*/sum.json')):
    rate,rest=os.path.basename(os.path.dirname(f)).split('_',1); mode,rep=rest.rsplit('_',1)
    mode='auto' if mode=='per_worker' else mode
    cells.setdefault((int(rate),mode),{})[rep]=json.load(open(f))
ceil={}
for (r,mo),reps in cells.items():
    if len(reps)>=2 and all(c['holds'] for c in reps.values()): ceil[mo]=max(ceil.get(mo,0), r)
tried={mo:sorted({r for (r,m) in cells if m==mo}, reverse=True) for mo in MODES}
common=min([ceil[m] for m in MODES if m in ceil] or [0])
def m(xs):
    xs=[x for x in xs if x is not None and x==x]; return st.mean(xs) if xs else float('nan')
print(f"### {LABEL}\n")
print("| mode | highest sustained calls/s | vs shared | rates tried (failed above the ceiling) |")
print("|---|---|---|---|")
for mo in MODES:
    c=ceil.get(mo); s=ceil.get('shared')
    above=[r for r in tried[mo] if c and r>c]; fr=min(above) if above else None
    dd=max((x.get('dispatch_drops',0) for x in cells.get((fr,mo),{}).values()), default=None) if fr else None
    print(f"| {mo} | {c if c else 'none held'} | {'' if mo=='shared' or not (c and s) else f'{(c-s)/s*100:+.0f}%'} | {', '.join(str(r) for r in tried[mo])} |")
if not common: sys.exit(0)
print(f"\nSame rate for all modes: {common} calls/s\n")
rows=[('CPU % per 1000 calls/s','cpu_per_kcps',1),('TCP main CPU %','tcp_main_cpu',1),('context switches per call','cs_per_call',2),
      ('syscalls per call','sys_per_call',1),('sched_yield per call','yield_per_call',2),('wasted empty reads (EAGAIN) per call','eagain_per_call',2)]
print("| | shared | auto | exclusive |\n|---|---|---|---|")
base={}
for name,key,dp in rows:
    vals={mo:m([c[key] for c in cells.get((common,mo),{}).values()]) for mo in MODES}
    out=[]
    for mo in MODES:
        v=vals[mo]; b=vals['shared']
        out.append(f"{v:.{dp}f}" + ("" if mo=='shared' or not (b and b==b and v==v) or b==0 else f" ({(v-b)/b*100:+.0f}%)"))
    print(f"| {name} | " + " | ".join(out) + " |")
sp=[]
for mo in MODES:
    reps=cells.get((common,mo),{}); w=[c['tcp_workers'] for c in reps.values() if c['tcp_workers']]
    sp.append(f"cv {m([x['cv'] for x in w]):.0f}% ({min(x['min'] for x in w):.1f}-{max(x['max'] for x in w):.1f}%)" if w else "n/a")
print("| TCP worker CPU spread | " + " | ".join(sp) + " |")
cl=[f"{max(c['client_gw_max'] for c in cells.get((common,mo),{}).values())}%" for mo in MODES]
print("| busiest client gateway process | " + " | ".join(cl) + " |")
