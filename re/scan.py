"""Every address-looking constant in the DLL's sources, carried to the new build: python -I scan.py old.pkl new.pkl [files...]"""
import re, sys, glob, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from port import Port
p = Port(sys.argv[1], sys.argv[2])
files = sys.argv[3:] or sorted(glob.glob('../dll/*.c') + glob.glob('../dll/*.cpp') + glob.glob('../dll/*.h'))
seen = collections.OrderedDict()
for f in files:
    for n, line in enumerate(open(f, encoding='utf8', errors='replace'), 1):
        for m in re.finditer(r'0x([0-9a-fA-F]{5,7})\b', line):
            v = int(m.group(1), 16)
            if 0x1000 <= v < 0x1800000:
                seen.setdefault(v, []).append('%s:%d' % (os.path.basename(f), n))
bad = 0
for v, where in seen.items():
    n, how = p.map(v)
    ok = n is not None and how in ('exact', 'near') or (n is not None and how.startswith('data') and 'SPLIT' not in how)
    bad += not ok
    print('%s %7x -> %7s  %-14s %s' % (' ' if ok else '!', v, '%x' % n if n is not None else '?', how, ' '.join(where[:3])))
print('total', len(seen), 'to look at by hand', bad)
