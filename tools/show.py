import os
import re, sys
L = open(os.environ.get('XIOS_ASM', 're/xios.asm')).read().split('\n')
def idx(a):
    for i, l in enumerate(L):
        if (";%04x" % a) in l:
            return i
    return None
lo = int(sys.argv[1], 16); hi = int(sys.argv[2], 16)
a, b = idx(lo), idx(hi)
if a is None or b is None:
    print("range not found", a, b); sys.exit(1)
for l in L[a:b]:
    print(re.sub(r'\t+', '  ', l)[:48])
