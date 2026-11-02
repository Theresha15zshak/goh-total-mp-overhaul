"""Find a piece of call_to_arms.exe code in call_to_arms_server.exe (same engine, other addresses): the
client bytes from rva on, with call targets and rip-relative displacements as wildcards, searched in the
server's .text. For each rip-relative operand of the match it prints the client and the server target.

    python -I twin.py <client rva> [bytes, hex, default 60]

Needs CAPSTONE_PATH like gx.py.
"""
import os, re, struct, sys

GAME = r'C:\Program Files (x86)\Steam\steamapps\common\Call to Arms - Gates of Hell\binaries\x64'
BASE = 0x140000000


class Exe:
    def __init__(self, name):
        self.d = open(os.path.join(GAME, name), 'rb').read()
        pe = struct.unpack_from('<I', self.d, 0x3c)[0]
        nsec = struct.unpack_from('<H', self.d, pe + 6)[0]
        optsz = struct.unpack_from('<H', self.d, pe + 20)[0]
        self.secs = {}
        for i in range(nsec):
            n, vs, va, rs, ra = struct.unpack_from('<8sIIII', self.d, pe + 24 + optsz + 40 * i)
            self.secs[n.rstrip(b'\0').decode()] = (va, vs, ra, rs)

    def off(self, rva):
        for va, vs, ra, rs in self.secs.values():
            if va <= rva < va + rs:
                return rva - va + ra

    def rva(self, o):
        for va, vs, ra, rs in self.secs.values():
            if ra <= o < ra + rs:
                return o - ra + va


def signature(exe, rva, n):
    sys.path.insert(0, os.environ.get('CAPSTONE_PATH', ''))
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    o = exe.off(rva)
    pat, rips = b'', []
    for ins in md.disasm(exe.d[o:o + n + 16], BASE + rva):
        if ins.address - BASE >= rva + n:
            break
        raw = bytes(ins.bytes)
        mask = bytearray(len(raw))
        if ins.mnemonic in ('call', 'jmp') and raw[0] in (0xe8, 0xe9):
            mask[1:5] = b'\1\1\1\1'
        disp_at = ins.disp_offset if 'rip' in ins.op_str else 0
        if disp_at:
            mask[disp_at:disp_at + 4] = b'\1\1\1\1'
            rips.append((ins.address - BASE - rva, disp_at, len(raw), ins.mnemonic + ' ' + ins.op_str))
        for b, m in zip(raw, mask):
            pat += b'.' if m else re.escape(bytes([b]))
    return pat, rips


def main():
    rva = int(sys.argv[1], 16)
    n = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x60
    cl, sv = Exe('call_to_arms.exe'), Exe('call_to_arms_server.exe')
    pat, rips = signature(cl, rva, n)
    va, vs, ra, rs = sv.secs['.text']
    hits = [m.start() for m in re.finditer(pat, sv.d[ra:ra + rs], re.S)]
    print('client %x: %d match(es) in the server: %s' % (rva, len(hits), ' '.join('%x' % (va + h) for h in hits[:8])))
    if len(hits) != 1:
        return
    srva = va + hits[0]
    for at, disp_at, size, text in rips:
        def target(exe, base):
            o = exe.off(base + at)
            return base + at + size + struct.unpack_from('<i', exe.d, o + disp_at)[0]
        print('  +%-4x %-50s client %x  server %x' % (at, text[:50], target(cl, rva), target(sv, srva)))


if __name__ == '__main__':
    main()
