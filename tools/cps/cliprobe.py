#!/usr/bin/env python3
# cpu% of every sipp process over a window
import os, time, sys
W=float(sys.argv[1]) if len(sys.argv)>1 else 5
CLK=os.sysconf('SC_CLK_TCK')
def ps():
    r={}
    for p in os.listdir('/proc'):
        if p.isdigit():
            try:
                if open(f'/proc/{p}/comm').read().strip()=='sipp':
                    st=open(f'/proc/{p}/stat').read().rsplit(')',1)[1].split(); r[int(p)]=int(st[11])+int(st[12])
            except Exception: pass
    return r
a=ps(); time.sleep(W); b=ps()
print(' '.join('%.0f' % ((b[p]-a[p])/CLK/W*100) for p in sorted(b) if p in a))
