#!/usr/bin/env python3
# A/B table from results-test/<ARM>/<CELL>/{probe_<node>.txt, sipp_*.txt}
import sys, os, re, glob
RES = sys.argv[1]; A = sys.argv[2]; B = sys.argv[3]; cells = sys.argv[4:]
def probe(path):
    if not os.path.exists(path): return None
    L = open(path).read().splitlines()
    if not L: return None
    h = L[0]; g = lambda rx: float((re.search(rx, h) or [0, 'nan'])[1])
    d = dict(cpu=g(r'opensips_cpu ([\d.]+)%'), host=g(r'host_busy ([\d.]+)%'), pin=g(r'UDP_in (\d+)/s'),
             wpp=g(r'wakeups/pkt ([\d.]+)'), si=g(r'softirq ([\d.]+)%'), groups={})
    for l in L[1:]:
        m = re.match(r'group (.+?): n=(\d+) sum=([\d.]+)% min=([\d.]+) max=([\d.]+) cv=(\d+)% \[(.*)\]', l)
        if m: d['groups'][m.group(1)] = dict(n=int(m.group(2)), sum=float(m.group(3)), min=float(m.group(4)), max=float(m.group(5)), cv=int(m.group(6)), cpus=m.group(7))
    return d
def sipp(d):
    ok = fail = retr = 0; secs = 0.0
    for f in glob.glob(os.path.join(d, 'sipp_*.txt')):
        s = open(f).read()
        m = re.search(r'Successful call\s*\|\s*\d+\s*\|\s*(\d+)', s); ok += int(m.group(1)) if m else 0
        m = re.search(r'Failed call\s*\|\s*\d+\s*\|\s*(\d+)', s); fail += int(m.group(1)) if m else 0
        m = re.search(r'Retransmissions\s*\|\s*(\d+)', s); retr += int(m.group(1)) if m else 0
        m = re.search(r'Total-time.*?\n.*?(\d+):(\d+):(\d+):(\d+)', s)
        if m: secs = max(secs, int(m.group(1))*3600+int(m.group(2))*60+int(m.group(3))+int(m.group(4))/1000)
    return ok, fail, retr, secs
def pct(a, b): return 'n/a' if not a else f"{(b-a)/a*100:+.0f}%"
for cell in cells:
    da, db = os.path.join(RES, A, cell), os.path.join(RES, B, cell)
    oa, fa, ra, sa = sipp(da); ob, fb, rb, sb = sipp(db)
    print(f"\n## cell {cell}")
    print(f"| metric | A={A} | B={B} | change |\n|---|---|---|---|")
    print(f"| sipp successful calls (3 hosts) | {oa} | {ob} | {pct(oa, ob)} |")
    print(f"| sipp failed / retransmissions | {fa} / {ra} | {fb} / {rb} | |")
    if sa and sb: print(f"| achieved CPS (calls / run time) | {oa/sa:.0f} | {ob/sb:.0f} | {pct(oa/sa, ob/sb)} |")
    for n in (191, 192, 193, 194, 195, 196):
        pa, pb = probe(os.path.join(da, f'probe_{n}.txt')), probe(os.path.join(db, f'probe_{n}.txt'))
        if not pa or not pb: continue
        print(f"| .{n} opensips CPU (sum of procs) | {pa['cpu']:.1f}% | {pb['cpu']:.1f}% | {pct(pa['cpu'], pb['cpu'])} |")
        print(f"| .{n} host busy / softirq | {pa['host']:.1f}% / {pa['si']:.1f}% | {pb['host']:.1f}% / {pb['si']:.1f}% | {pct(pa['host'], pb['host'])} |")
        print(f"| .{n} UDP in/s, wakeups per packet | {pa['pin']:.0f}, {pa['wpp']:.2f} | {pb['pin']:.0f}, {pb['wpp']:.2f} | {pct(pa['wpp'], pb['wpp'])} |")
        for gk in sorted(set(pa['groups']) | set(pb['groups'])):
            ga, gb = pa['groups'].get(gk), pb['groups'].get(gk)
            if not ga or not gb: continue
            short = gk.replace('SIP receiver ', '')
            print(f"| .{n} {short}: workers, spread min-max (cv) | n={ga['n']} {ga['min']:.1f}-{ga['max']:.1f} (cv {ga['cv']}%) [{ga['cpus']}] | n={gb['n']} {gb['min']:.1f}-{gb['max']:.1f} (cv {gb['cv']}%) [{gb['cpus']}] | cv {ga['cv']}%→{gb['cv']}% |")
