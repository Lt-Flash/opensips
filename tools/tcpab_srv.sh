# tcpab_srv.sh <MODE> <CELL> <OUT>  - on 225: start server in MODE, sample 10 s (roles via fifo), strace -c TCP main 8 s, stop
set -u
MODE=$1 CELL=$2 OUT=$3; mkdir -p $OUT
ACTION=start TMPL=srv_tcp.cfg.tmpl PREFIX=/opt/opensips-udpbal IP=192.0.2.11 EXTRA="tcp_dispatch_mode=\"$MODE\"" bash /tmp/udpbal/srvctl.sh | grep -E 'STARTED|FAILED'
sleep 7
FIFO=/tmp/opensips_fifo python3 -u /tmp/udpbal/probe4.py 10 > $OUT/probe.txt 2>&1
TM=$(FIFO=/tmp/opensips_fifo python3 - <<'PY'
import os,subprocess,json,time
rp='/tmp/mirplx'; 
try: os.unlink(rp)
except: pass
os.mkfifo(rp); os.chmod(rp,0o666)
rd=subprocess.Popen(['timeout','5','cat',rp],stdout=subprocess.PIPE,text=True); time.sleep(0.1)
open('/tmp/opensips_fifo','w').write(':mirplx:{"jsonrpc":"2.0","method":"ps","params":[],"id":1}\n')
d=json.loads(rd.communicate()[0])['result']['Processes']; os.unlink(rp)
print([p['PID'] for p in d if p['Type']=='TCP main'][0])
PY
)
echo "tcp main pid $TM" >> $OUT/probe.txt
timeout 20 strace -c -f -p $TM -o $OUT/strace.txt & sp=$!; sleep 8; kill -INT $sp 2>/dev/null; wait $sp 2>/dev/null
ACTION=stop bash /tmp/udpbal/srvctl.sh >/dev/null
