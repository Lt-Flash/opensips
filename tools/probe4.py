#!/usr/bin/env python3
# per-process cpu/wakeups over a window, with OpenSIPS roles from `opensips-cli -x mi ps`
import os, time, sys, json, subprocess, re, statistics
W = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
CLK = os.sysconf('SC_CLK_TCK')
def roles_fifo():
    # MI 'ps' straight through the mi_fifo (the CLI on some nodes has no 'ps' command)
    fifo = os.environ.get('FIFO')
    for l in (open('/etc/opensips/opensips-cli.cfg') if not fifo and os.path.exists('/etc/opensips/opensips-cli.cfg') else []):
        if l.strip().startswith('fifo_file'): fifo = re.split(r'[:=]', l, 1)[1].strip()
    if not fifo or not os.path.exists(fifo): return {}
    rp = f'/tmp/mirpl{os.getpid()}'
    try:
        os.mkfifo(rp); os.chmod(rp, 0o666)
        rd = subprocess.Popen(['timeout', '5', 'cat', rp], stdout=subprocess.PIPE, text=True)
        time.sleep(0.1)
        with open(fifo, 'w') as f: f.write(':%s:{"jsonrpc":"2.0","method":"ps","params":[],"id":1}\n' % os.path.basename(rp))
        out = rd.communicate(timeout=6)[0]
        d = json.loads(out)['result']; return {p['PID']: (p['ID'], p['Type']) for p in d['Processes']}
    except Exception:
        return {}
    finally:
        try: os.unlink(rp)
        except Exception: pass
def roles():
    try:
        out = subprocess.run(['opensips-cli', '-x', 'mi', 'ps'], capture_output=True, text=True, timeout=10).stdout
        d = json.loads(out); return {p['PID']: (p['ID'], p['Type']) for p in d['Processes']}
    except Exception:
        return roles_fifo()
def pids():
    return sorted(int(p) for p in os.listdir('/proc') if p.isdigit() and
                  open(f'/proc/{p}/comm').read().strip() == 'opensips')
def snap(p):
    st = open(f'/proc/{p}/stat').read().rsplit(')', 1)[1].split()
    vcs = 0
    for l in open(f'/proc/{p}/status'):
        if l.startswith('voluntary_ctxt_switches'): vcs = int(l.split()[1])
    return int(st[11]) + int(st[12]), vcs
def udp():
    lines = [l for l in open('/proc/net/snmp') if l.startswith('Udp:')]
    d = dict(zip(lines[0].split()[1:], lines[1].split()[1:])); return int(d['InDatagrams']), int(d['RcvbufErrors'])
def cpu_all():
    l = open('/proc/stat').readline().split(); v = [int(x) for x in l[1:]]
    return sum(v), v[3] + v[4], v[6]  # total, idle+iowait, softirq
R = roles(); P = pids(); a = {p: snap(p) for p in P}; ua = udp(); ca = cpu_all(); t0 = time.time()
time.sleep(W)
b = {p: snap(p) for p in P if os.path.exists(f'/proc/{p}')}; ub = udp(); cb = cpu_all(); dt = time.time() - t0
ncpu = os.cpu_count()
tot_j = cb[0] - ca[0]; host_busy = 100.0 * (1 - (cb[1] - ca[1]) / tot_j) if tot_j else 0
si = 100.0 * (cb[2] - ca[2]) / tot_j * ncpu if tot_j else 0
rows = []; tot = 0.0; totv = 0
for p in P:
    if p not in b: continue
    cpu = (b[p][0] - a[p][0]) / CLK / dt * 100; v = (b[p][1] - a[p][1]) / dt
    pid, typ = R.get(p, ('?', '?')); rows.append((pid, p, typ, cpu, v)); tot += cpu; totv += v
pin = (ub[0] - ua[0]) / dt
print(f"window {dt:.1f}s ncpu {ncpu} host_busy {host_busy:.1f}% softirq {si:.1f}%cpu UDP_in {pin:.0f}/s rcvbuferr {ub[1]-ua[1]} opensips_cpu {tot:.1f}% vcs {totv:.0f}/s wakeups/pkt {totv/max(pin,1):.2f}")
groups = {}
for pid, p, typ, cpu, v in rows:
    key = typ if typ.startswith('SIP receiver') or typ.startswith('TCP receiver') else None
    if key: groups.setdefault(key, []).append(cpu)
for k, cs in sorted(groups.items()):
    cv = statistics.pstdev(cs) / statistics.mean(cs) * 100 if len(cs) > 1 and statistics.mean(cs) > 0 else 0
    print(f"group {k}: n={len(cs)} sum={sum(cs):.1f}% min={min(cs):.1f} max={max(cs):.1f} cv={cv:.0f}% [{' '.join(f'{c:.1f}' for c in cs)}]")
for pid, p, typ, cpu, v in rows:
    print(f"{str(pid):>3} {p:>8} {cpu:6.1f} {v:8.0f}  {typ}")
