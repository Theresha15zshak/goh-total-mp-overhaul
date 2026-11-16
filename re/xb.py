"""Bytes of the game's exe at an rva: python -I xb.py [-s] <rva> [n]   (-s: the server exe)"""
import struct, sys
G = r'C:/Program Files (x86)/Steam/steamapps/common/Call to Arms - Gates of Hell/binaries/x64/'
a = sys.argv[1:]
exe = 'call_to_arms.exe'
if a[0] == '-s': exe = 'call_to_arms_server.exe'; a = a[1:]
d = open(G + exe, 'rb').read(); p = struct.unpack_from('<I', d, 0x3c)[0]
secs = [struct.unpack_from('<8sIIII', d, p + 24 + struct.unpack_from('<H', d, p + 20)[0] + 40 * i) for i in range(struct.unpack_from('<H', d, p + 6)[0])]
def off(rva):
    for n, vs, va, rs, ra in secs:
        if va <= rva < va + rs: return rva - va + ra
r = int(a[0], 16); n = int(a[1]) if len(a) > 1 else 16
print(' '.join('%02x' % b for b in d[off(r):off(r) + n]))
