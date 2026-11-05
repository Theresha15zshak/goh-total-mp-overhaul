"""The range of each vehicle's main gun, from the game's files, for dll/farshoot.c: the name of a vehicle's .def, and {range a b}
or {aimRange a} of the weapon in its {place "gun"}, followed through {from "..."}, macros of .presets included.

    python tools/vehicle_ranges.py <header to write>
"""
import os
import re
import sys
import zipfile

GAME = r'C:\Program Files (x86)\Steam\steamapps\common\Call to Arms - Gates of Hell\resource'


def strip(text):
    return re.sub(r';[^\n]*', '', text)


def stuff_files(pak):
    out = {}
    for n in pak.namelist():
        if n.startswith('set/stuff/') and not os.path.basename(n).startswith('.') and not n.endswith('.pattern'):
            out.setdefault(os.path.basename(n).lower(), n)
    return out


def presets(pak):
    """(define "name" ...): where each macro sets a range"""
    ranges = {}
    for n in pak.namelist():
        if n.startswith('set/stuff/') and os.path.basename(n) == '.presets':
            text = strip(pak.read(n).decode('utf8', 'replace'))
            parts = re.split(r'\(define\s+"([^"]+)"', text)
            for name, body in zip(parts[1::2], parts[2::2]):
                m = re.search(r'\{(?:range|aimRange)\s+([0-9.]+)', body)
                if m:
                    ranges[name.lower()] = float(m.group(1))
    return ranges


def gun_range(name, files, pak, macros, depth=0):
    n = files.get(name.lower())
    if not n or depth > 8:
        return None
    text = strip(pak.read(n).decode('utf8', 'replace'))
    # the weapon's own range is ("range_200") or a {range} at its first level: a {range} deeper in is of something else (a shell's)
    m = re.search(r'\(\s*"range_(\d+)', text)
    if m:
        return macros.get('range_' + m.group(1), float(m.group(1)))
    level = 0
    for m in re.finditer(r'[{}]|\{(?:range|aimRange)\s+([0-9.]+)', text):
        if m.group(0) == '}':
            level -= 1
            continue
        level += 1
        if m.group(1) and level == 2:
            return float(m.group(1))
    m = re.search(r'\{from\s+"([^"]+)"', text)
    return gun_range(m.group(1), files, pak, macros, depth + 1) if m else None


def table():
    logic = zipfile.ZipFile(os.path.join(GAME, 'gamelogic.pak'))
    vehicles = zipfile.ZipFile(os.path.join(GAME, 'entity', '-vehicle.pak'))
    files, macros = stuff_files(logic), presets(logic)
    out, missing = {}, []
    for n in vehicles.namelist():
        low = n.lower()
        if not low.endswith('.def') or '/x/' in low:
            continue
        name = os.path.basename(low)[:-4]
        text = strip(vehicles.read(n).decode('utf8', 'replace'))
        m = re.search(r'\{place\s+"gun\d*"[^{]*(?:\{[^{}]*\}[^{]*)*?\{weapon\s+"([^"]+)"', text)
        if not m:
            continue
        r = gun_range(m.group(1).split()[-1], files, logic, macros)
        if r:
            out[name] = int(r)
        else:
            missing.append((name, m.group(1)))
    return out, missing


def main():
    out, missing = table()
    with open(sys.argv[1], 'w', encoding='ascii') as f:
        f.write('// made by tools/vehicle_ranges.py from the game\'s files\n')
        for name in sorted(out):             # sorted: the DLL looks them up by halves
            f.write('{"%s", %d},\n' % (name, out[name]))
    print('vehicle ranges:', len(out), 'vehicles,', len(missing), 'without a range', missing[:8])


if __name__ == '__main__':
    main()
