#!/usr/bin/env python3
# ChaseGuard - length-preserving rename (MWCrashFix -> ChaseGuard, both 10 characters).
# License: GPL-3.0 (see ../LICENSE.txt)
import re
d=bytearray(open('MWCrashFix.asi','rb').read()); n0=len(d)
for m in list(re.finditer(rb'(?i)mwcrashfix',bytes(d))):
    d[m.start():m.end()]={b'MWCrashFix':b'ChaseGuard',b'mwcrashfix':b'chaseguard'}[m.group()]
assert len(d)==n0
open('ChaseGuard.asi','wb').write(d)
print("written ChaseGuard.asi")
