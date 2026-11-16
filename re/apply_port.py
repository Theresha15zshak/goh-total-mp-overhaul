"""Rewrites the address constants in dll/ for a new build of the game.
    python -I apply_port.py [--write]
Client addresses go by the client's indexes, the server rows (named below) by the server's. Prints what it
could not carry; without --write nothing is changed."""
import os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from port import Port

WRITE = '--write' in sys.argv
client = Port(os.path.join(HERE, 'gx_index_1067_old.pkl'), os.path.join(HERE, 'gx_index.pkl'))
server = Port(os.path.join(HERE, 'gx_server_1067_old.pkl'), os.path.join(HERE, 'gx_server.pkl'))
DLL = os.path.join(HERE, '..', 'dll')
# lines whose constants are the server exe's: (file, text the line contains)
SERVER_LINES = [('pathing.c', '{0x10ee430,'), ('balance.c', '{"call_to_arms_server.exe",')]
# server ? A : B on one line: the first constant of each pair is the server's
SERVER_FIRST = [('always_day.c', 'const uintptr_t vt = server ?')]
NOT_ADDRESSES = {0x1700000, 0x1707000}      # image size bounds, set by hand

todo = []
for name in sorted(os.listdir(DLL)):
    if not name.endswith(('.c', '.cpp', '.h')):
        continue
    path = os.path.join(DLL, name)
    text = open(path, encoding='utf8', newline='').read()
    out = []
    for n, line in enumerate(text.split('\n'), 1):
        srv_line = any(f == name and key in line for f, key in SERVER_LINES)
        srv_first = any(f == name and key in line for f, key in SERVER_FIRST)
        code = line.split('//')[0] if '//' in line and not line.lstrip().startswith('//') else line
        count = [0]

        def sub(m):
            v = int(m.group(1), 16)
            if m.start() >= len(code) or not (0x1000 <= v < 0x1800000) or v in NOT_ADDRESSES:
                return m.group(0)
            use_server = srv_line or (srv_first and m.group(0) in re.findall(r'\?\s*(0x[0-9a-fA-F]+)', line))
            p = server if use_server else client
            new, how = p.map(v)
            ok = new is not None and (how in ('exact', 'near') or (how.startswith('data') and 'SPLIT' not in how))
            start = new is not None and how == 'changed' and p.fio(v) >= 0 and p.fo[p.fio(v)][0] == v
            if ok or start:
                if start:
                    todo.append('%s:%d  %x -> %x  its function CHANGED: check the hook' % (name, n, v, new))
                return '0x%x' % new
            todo.append('%s:%d  %x  %s%s' % (name, n, v, how, ' (server)' if use_server else ''))
            return m.group(0)
        if line.lstrip().startswith('//'):
            out.append(line)
        else:
            out.append(re.sub(r'0x([0-9a-fA-F]{5,7})\b', sub, line))
    new_text = '\n'.join(out)
    if new_text != text:
        changed = sum(1 for a, b in zip(text.split('\n'), out) if a != b)
        print('%-16s %d lines' % (name, changed))
        if WRITE:
            open(path, 'w', encoding='utf8', newline='').write(new_text)
print('\nby hand:')
for t in todo:
    print(' ', t)
