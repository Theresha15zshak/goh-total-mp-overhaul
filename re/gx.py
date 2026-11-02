"""Static index of call_to_arms.exe: functions (.pdata), RIP-relative xrefs,
direct calls, RTTI vtables. Built once and cached next to this file. Another exe of the game (the server):
GX_EXE=<path> GX_CACHE=<index file>.

    python -I gx.py str  <regex>        strings + who references them
    python -I gx.py fn   <rva>          disassemble the function containing rva
    python -I gx.py callers <rva>       direct callers of a function
    python -I gx.py xref <rva>          RIP-relative references to an address
    python -I gx.py cls  <regex>        RTTI classes -> vtable rva and slots
"""
import os, re, sys, struct, pickle, bisect

EXE = os.environ.get('GX_EXE', r'C:\Program Files (x86)\Steam\steamapps\common\Call to Arms - Gates of Hell\binaries\x64\call_to_arms.exe')
HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.environ.get('GX_CACHE', os.path.join(HERE, 'gx_index.pkl'))
BASE = 0x140000000

d = open(EXE, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsz = struct.unpack_from('<H', d, pe + 20)[0]
IMAGE = struct.unpack_from('<I', d, pe + 24 + 56)[0]   # SizeOfImage
secs = []
for i in range(nsec):
    n, vs, va, rs, ra = struct.unpack_from('<8sIIII', d, pe + 24 + optsz + 40 * i)
    secs.append((n.rstrip(b'\0').decode(), va, vs, ra, rs))
S = {s[0]: s for s in secs}


def off(rva):
    for n, va, vs, ra, rs in secs:
        if va <= rva < va + rs:
            return rva - va + ra
    return None


def rva_of(o):
    for n, va, vs, ra, rs in secs:
        if ra <= o < ra + rs:
            return o - ra + va


def cstr(rva, wide=False):
    o = off(rva)
    if o is None:
        return None
    if wide:
        m = re.match(rb'((?:[\x20-\x7e]\x00){2,})\x00\x00', d[o:o + 512])
        return m.group(1).decode('utf-16le') if m else None
    m = re.match(rb'([\x09\x20-\x7e]{2,})\x00', d[o:o + 512])
    return m.group(1).decode() if m else None


def build():
    _, tva, _, tra, trs = S['.text']
    text = d[tra:tra + trs]
    # functions from .pdata
    _, pva, pvs, pra, _ = S['.pdata']
    funcs = []
    for i in range(pvs // 12):
        b, e, u = struct.unpack_from('<III', d, pra + 12 * i)
        if b:
            funcs.append((b, e))
    funcs.sort()
    # RIP-relative operands: [REX] op modrm(mod=00,rm=101) disp32
    xref = {}
    pat = re.compile(rb'[\x40-\x4f]?(?:\x8d|\x8b|\x89|\x80|\x83|\x81|\x39|\x3b|\x38|\x3a|\xc7|\xc6|\x0f[\x10\x11\x28\x2e\x2f\xb6\xb7])[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', re.S)
    for m in pat.finditer(text):
        p = m.end()
        disp = struct.unpack_from('<i', text, p)[0]
        op = m.group()
        imm = 0
        if op[-2:-1] in (b'\x80', b'\x83', b'\xc6'):
            imm = 1
        elif op[-2:-1] in (b'\x81', b'\xc7'):
            imm = 4
        tgt = tva + p + 4 + imm + disp
        if 0x1000 <= tgt < IMAGE:
            xref.setdefault(tgt, []).append(tva + m.start())
    calls = {}
    for m in re.finditer(rb'\xe8', text):
        p = m.start()
        if p + 5 > len(text):
            continue
        tgt = tva + p + 5 + struct.unpack_from('<i', text, p + 1)[0]
        if tva <= tgt < tva + trs:
            calls.setdefault(tgt, []).append(tva + p)
    # RTTI: CompleteObjectLocator -> TypeDescriptor name; vtable = COL ptr slot + 8
    vt = {}
    _, rva_, rvs, rra, rrs = S['.rdata']
    names = {}
    for m in re.finditer(rb'\.\?AV[\x20-\x7e]+?@@\x00', d):
        names[rva_of(m.start()) - 0x10] = m.group()[4:-3].decode()
    for o in range(rra, rra + rrs - 8, 8):
        q = struct.unpack_from('<Q', d, o)[0]
        if not (BASE < q < BASE + IMAGE):
            continue
        col = q - BASE
        co = off(col)
        if co is None or struct.unpack_from('<I', d, co)[0] != 1:
            continue
        td = struct.unpack_from('<I', d, co + 12)[0]
        if td in names and struct.unpack_from('<I', d, co + 20)[0] == col:
            vt.setdefault(names[td], []).append(rva_of(o) + 8)
    idx = dict(funcs=funcs, xref=xref, calls=calls, vt=vt)
    pickle.dump(idx, open(CACHE, 'wb'))
    return idx


idx = pickle.load(open(CACHE, 'rb')) if os.path.exists(CACHE) else build()
FB = [f[0] for f in idx['funcs']]


def func_of(rva):
    i = bisect.bisect_right(FB, rva) - 1
    if i >= 0 and idx['funcs'][i][0] <= rva < idx['funcs'][i][1]:
        return idx['funcs'][i]


def strings(rx):
    r = re.compile(rx.encode(), re.I)
    _, va, vs, ra, rs = S['.rdata']
    out = []
    for m in re.finditer(rb'[\x09\x20-\x7e]{3,}\x00', d[ra:ra + rs]):
        if r.search(m.group()[:-1]):
            out.append((va + m.start(), m.group()[:-1].decode(), False))
    for m in re.finditer(rb'(?:[\x20-\x7e]\x00){3,}\x00\x00', d[ra:ra + rs]):
        s = m.group()[:-2].decode('utf-16le')
        if r.search(s.encode()):
            out.append((va + m.start(), s, True))
    return out


def dis(start, end, notes=True):
    sys.path.insert(0, os.environ.get('CAPSTONE_PATH', ''))
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    o = off(start)
    lines = []
    for ins in md.disasm(d[o:o + (end - start)], BASE + start):
        a = ins.address - BASE
        c = ''
        m = re.search(r'\[rip ([+-]) 0x([0-9a-f]+)\]', ins.op_str)
        if m:
            t = ins.address + ins.size + int(m.group(2), 16) * (1 if m.group(1) == '+' else -1) - BASE
            c = ' ; %x' % t
            s = cstr(t) or cstr(t, True)
            if s and notes:
                c += ' ' + repr(s[:80])
            for k, v in idx['vt'].items():
                if t in v:
                    c += ' vtbl ' + k
        m = re.match(r'0x([0-9a-f]+)$', ins.op_str)
        if m and (ins.mnemonic == 'call' or ins.mnemonic[0] == 'j'):
            c = ' ; %x' % (int(m.group(1), 16) - BASE)
        lines.append('%x: %-7s %s%s' % (a, ins.mnemonic, ins.op_str, c))
    return lines


if __name__ == '__main__':
    cmd, arg = sys.argv[1], sys.argv[2]
    if cmd == 'str':
        for a, s, w in strings(arg)[:60]:
            refs = idx['xref'].get(a, [])
            fs = sorted({(func_of(r) or (r, r))[0] for r in refs})
            print('%x %s%r  refs=%d fns=%s' % (a, 'L' if w else '', s[:100], len(refs), ' '.join('%x' % f for f in fs[:8])))
    elif cmd == 'fn':
        f = func_of(int(arg, 16))
        print('func %x-%x' % f)
        print('\n'.join(dis(*f)))
    elif cmd == 'dis':   # leaf functions have no .pdata entry
        a = int(arg, 16)
        n = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x40
        print('\n'.join(dis(a, a + n)))
    elif cmd == 'callers':
        for c in idx['calls'].get(int(arg, 16), []):
            f = func_of(c)
            print('%x in %s' % (c, '%x' % f[0] if f else '?'))
    elif cmd == 'xref':
        for c in idx['xref'].get(int(arg, 16), []):
            f = func_of(c)
            print('%x in %s' % (c, '%x' % f[0] if f else '?'))
    elif cmd == 'cls':
        for k, v in sorted(idx['vt'].items()):
            if re.search(arg, k, re.I):
                print(k, ' '.join('%x' % x for x in v))
