#!/usr/bin/env python3
# per-call count of each syscall, per mode, from the dedicated breakdown cells
import sys, os, re, glob
RES=sys.argv[1]; rate=sys.argv[2]; MODES=['shared','auto','exclusive']
data={}; calls={}
for mo in MODES:
    O=os.path.join(RES, f'{rate}_{mo}_r1')
    pr=open(os.path.join(O,'probe.txt')).read(); w=float(re.search(r'window ([\d.]+)s',pr).group(1)); pin=float(re.search(r'UDP in (\d+)/s',pr).group(1))
    calls[mo]=pin/6*w; d={}
    for l in open(os.path.join(O,'perf_all.csv')):
        f=l.strip().split(',')
        if len(f)>3 and f[0] and f[0][0].isdigit(): d[f[2].replace('syscalls:sys_enter_','')]=float(f[0])
    data[mo]=d
keys=sorted({k for d in data.values() for k,v in d.items() if v/calls[MODES[0]]>=0.05}, key=lambda k:-data['shared'].get(k,0))
print('| syscall per call | ' + ' | '.join(MODES) + ' | auto vs shared | exclusive vs shared |')
print('|---|' + '---|'*(len(MODES)+2))
tot={mo:sum(v for k,v in data[mo].items() if k!='context-switches')/calls[mo] for mo in MODES}
for k in keys+['TOTAL']:
    vals={mo:(tot[mo] if k=='TOTAL' else data[mo].get(k,0)/calls[mo]) for mo in MODES}
    pc=lambda a,b: f'{(b-a)/a*100:+.0f}%' if a else ('new' if b else '')
    print(f"| {k} | " + ' | '.join(f'{vals[mo]:.2f}' for mo in MODES) + f" | {pc(vals['shared'],vals['auto'])} | {pc(vals['shared'],vals['exclusive'])} |")
