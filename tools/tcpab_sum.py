#!/usr/bin/env python3
import re, os, glob, statistics, sys
RES = sys.argv[1]
rows = {}
for d in sorted(glob.glob(os.path.join(RES, '*_*_[123]'))):
    mode, cell, rep = os.path.basename(d).rsplit('_', 2)
    P = open(os.path.join(d, 'probe.txt')).read()
    tm = re.search(r'^\s*\d+\s+\d+\s+([\d.]+)\s+(\d+)\s+TCP main', P, re.M)
    wk = re.findall(r'^\s*\d+\s+\d+\s+([\d.]+)\s+(\d+)\s+TCP receiver', P, re.M)
    tot = re.search(r'opensips_cpu ([\d.]+)%', P)
    S = open(os.path.join(d, 'strace.txt')).read() if os.path.exists(os.path.join(d, 'strace.txt')) else ''
    sc = {m.group(2): int(m.group(1)) for m in re.finditer(r'^\s*[\d.]+\s+[\d.]+\s+\d+\s+(\d+)\s+(?:\d+\s+)?(\w+)$', S, re.M)}
    ok = re.search(r'Successful call \| \d+ \| (\d+)', open(os.path.join(d, 'sipp.txt')).read() or '')
    rows.setdefault((mode, cell), []).append(dict(tm_cpu=float(tm.group(1)), tm_vcs=int(tm.group(2)), wk=[float(c) for c, v in wk], wk_vcs=[int(v) for c, v in wk], tot=float(tot.group(1)), sc=sc, ok=ok.group(1) if ok else '?'))
def avg(xs): return statistics.mean(xs)
for cell in ('tn', 't1'):
    print(f"\n## cell {cell}")
    print("| mode | TCP main cpu% (3 reps) | TCP main vcs/s | workers cpu% (rep1) | worker cv% | opensips total | syscalls/8s: futex / epoll_wait / sendto / write / read | sipp ok |")
    print("|---|---|---|---|---|---|---|---|")
    for mode in ('shared', 'per_worker'):
        rs = rows.get((mode, cell), [])
        if not rs: continue
        w = rs[0]['wk']; cv = statistics.pstdev(w)/statistics.mean(w)*100 if statistics.mean(w) else 0
        sc = rs[0]['sc']
        tmc = ' '.join('%.1f' % r['tm_cpu'] for r in rs); wks = ' '.join('%.1f' % c for c in w); oks = ','.join(r['ok'] for r in rs)
        print("| %s | %s (avg %.1f) | %.0f | %s | %.0f%% | %.1f%% | %d / %d / %d / %d / %d | %s |" % (mode, tmc, avg([r['tm_cpu'] for r in rs]), avg([r['tm_vcs'] for r in rs]), wks, cv, avg([r['tot'] for r in rs]), sc.get('futex',0), sc.get('epoll_wait',0), sc.get('sendto',0), sc.get('write',0), sc.get('read',0), oks))
