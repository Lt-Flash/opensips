#!/usr/bin/env python3
# max cpu% of any sipp and of any opensips (gateway) process over a window
import os, time, sys
W=float(sys.argv[1]) if len(sys.argv)>1 else 5
CLK=os.sysconf('SC_CLK_TCK')
def ps():
    r={}
    for p in os.listdir('/proc'):
        if p.isdigit():
            try:
                c=open(f'/proc/{p}/comm').read().strip()
                if c in ('sipp','opensips'):
                    st=open(f'/proc/{p}/stat').read().rsplit(')',1)[1].split(); r[int(p)]=(c,int(st[11])+int(st[12]))
            except Exception: pass
    return r
a=ps(); time.sleep(W); b=ps()
m={'sipp':0.0,'opensips':0.0}; tot={'sipp':0.0,'opensips':0.0}
for p,(c,t) in b.items():
    if p in a:
        v=(t-a[p][1])/CLK/W*100; m[c]=max(m[c],v); tot[c]+=v
print(f"sipp_max={m['sipp']:.0f} gw_max={m['opensips']:.0f} gw_total={tot['opensips']:.0f}")
