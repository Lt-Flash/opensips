#!/usr/bin/env python3
import os, time, sys
W = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
CLK = os.sysconf('SC_CLK_TCK')
def pids():
    out = []
    for p in os.listdir('/proc'):
        if p.isdigit():
            try:
                if open(f'/proc/{p}/comm').read().strip() == 'opensips': out.append(int(p))
            except Exception: pass
    return sorted(out)
def snap(p):
    st = open(f'/proc/{p}/stat').read().rsplit(')', 1)[1].split()
    ut, stt, psr = int(st[11]), int(st[12]), int(st[36])
    vcs = nvcs = 0
    for l in open(f'/proc/{p}/status'):
        if l.startswith('voluntary_ctxt_switches'): vcs = int(l.split()[1])
        elif l.startswith('nonvoluntary_ctxt_switches'): nvcs = int(l.split()[1])
    return ut, stt, vcs, nvcs, psr
def udp():
    lines = [l for l in open('/proc/net/snmp') if l.startswith('Udp:')]
    d = dict(zip(lines[0].split()[1:], lines[1].split()[1:]))
    return int(d['InDatagrams']), int(d['OutDatagrams']), int(d['RcvbufErrors'])
def ipis():
    for l in open('/proc/interrupts'):
        f = l.split()
        if f and f[0] == 'RES:': return sum(int(x) for x in f[1:] if x.isdigit())
    return 0
def softirq():
    l = open('/proc/stat').readline().split()
    return int(l[7]), sum(int(x) for x in l[1:])  # softirq jiffies, total jiffies (all cpus)
P = pids(); a = {p: snap(p) for p in P}; ua = udp(); ia = ipis(); sa = softirq(); t0 = time.time()
time.sleep(W)
P2 = pids(); b = {p: snap(p) for p in P if p in P2}; ub = udp(); ib = ipis(); sb = softirq(); dt = time.time() - t0
pin = (ub[0]-ua[0])/dt
print(f"window {dt:.2f}s UDP in {pin:.0f}/s out {(ub[1]-ua[1])/dt:.0f}/s rcvbuferr {ub[2]-ua[2]} RES_IPI {(ib-ia)/dt:.0f}/s "
      f"softirq {(sb[0]-sa[0])/CLK/dt*100:.1f}% of one cpu ({(sb[0]-sa[0])/CLK/max(1,(ub[0]-ua[0]))*1e6:.1f} us/pkt)")
rows = []; tot = 0.0; totv = 0
for p in P:
    if p not in b: continue
    cpu = (b[p][0]+b[p][1]-a[p][0]-a[p][1]) / CLK / dt * 100
    v = (b[p][2]-a[p][2])/dt
    rows.append((p, cpu, v, (b[p][3]-a[p][3])/dt)); tot += cpu; totv += v
print(f"total opensips cpu {tot:.1f}%  total vcs {totv:.0f}/s  ({totv/max(pin,1):.2f} wakeups per in-packet)")
print(f"{'idx':>3} {'pid':>8} {'cpu%':>6} {'vcs/s':>8} {'nvcs/s':>7}")
for i, (p, cpu, v, nv) in enumerate(rows):
    print(f"{i:>3} {p:>8} {cpu:6.1f} {v:8.0f} {nv:7.0f}")
