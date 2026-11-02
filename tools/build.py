"""Builds build/iphlpapi.dll (the whole mod) with the zig toolchain from .tools/zig.
Developer-only; players just copy the DLL into the game's binaries\\x64.

    python tools/build.py
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# `python tools/build.py competitive`: only the competitive balance switch and its F7 window
# (build/competitive/iphlpapi.dll), for testing it with other players
VARIANT = sys.argv[1] if len(sys.argv) > 1 else ''
OBJ = os.path.join(ROOT, 'build', 'obj' + ('_' + VARIANT if VARIANT else ''))
OUT = os.path.join(ROOT, 'build', VARIANT, 'iphlpapi.dll') if VARIANT else os.path.join(ROOT, 'build', 'iphlpapi.dll')
IMGUI = os.path.join(ROOT, 'third_party', 'imgui')
TARGET = ['-target', 'x86_64-windows-gnu']
CFLAGS = ['-O2', '-Wall', '-Wno-unused-function', '-I' + IMGUI, '-I' + os.path.join(IMGUI, 'backends')]

CFLAGS.append('-I' + OBJ)    # balance_data.h, made below
if VARIANT == 'competitive':
    CFLAGS.append('-DCOMPETITIVE_ONLY')

C_SRC = ['dll/goh_replay.c', 'dll/proxy_iphlpapi.c', 'dll/snap.c', 'dll/keep.c', 'dll/lz.c', 'dll/prof.c', 'dll/balance.c',
         'dll/online.c', 'dll/pvp.c', 'dll/census.c', 'dll/report.c']
CPP_SRC = ['dll/ui.cpp', 'dll/music.cpp'] + [os.path.join(IMGUI, f) for f in
                            ('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp',
                             'backends/imgui_impl_dx11.cpp', 'backends/imgui_impl_win32.cpp')]
LIBS = ['-ld3d11', '-ldxgi', '-ld3dcompiler_47', '-ldwmapi', '-limm32', '-lgdi32', '-luser32', '-lshell32',
        '-lmfplat', '-lmfreadwrite', '-lole32', '-lwinhttp', '-lbcrypt']


def zig(*args):
    env = dict(os.environ, PYTHONPATH=os.path.join(ROOT, '.tools', 'zig'))
    r = subprocess.run([sys.executable, '-m', 'ziglang'] + list(args), cwd=ROOT, env=env)
    if r.returncode:
        sys.exit(r.returncode)


def obj(src):
    o = os.path.join(OBJ, os.path.splitext(os.path.basename(src))[0] + '.o')
    s = os.path.join(ROOT, src)
    if not os.path.exists(o) or os.path.getmtime(o) < os.path.getmtime(s) or 'dll' in src.replace('\\', '/').split('/')[0]:
        print('compile', os.path.basename(src))
        zig('cc' if src.endswith('.c') else 'c++', *TARGET, *CFLAGS, '-c', s, '-o', o)
    return o


GAME = r'C:\Program Files (x86)\Steam\steamapps\common\Call to Arms - Gates of Hell'

# Competitive balance (dll/balance.c): our copy of each file, made from the game's own copy here.
# Every change is (block it sits in, text in that block, replacement of the same length).
BALANCE = [
    ('resource/gamelogic.pak', 'set/multiplayer/units/late/units_ger.set', [
        # defensive doctrine: 15 cm NbW 41 at tier 2, 12 cm sGrW 42 at tier 3 (tier delay and cooldown go
        # with the tier, each keeps its own doctrine-point price)
        (b'{"doctrine_120mm_sgrw42_late"', b'("doctrine_t2" cool(300) d(late_ger_defensive) cost(30))',
         b'("doctrine_t3" cool(480) d(late_ger_defensive) cost(30))'),
        (b'{"doctrine_150mm_nebelwerfer41_late"', b'("doctrine_t3" cool(480) d(late_ger_defensive) cost(60))',
         b'("doctrine_t2" cool(300) d(late_ger_defensive) cost(60))'),
        # the spawn menu and the decks lay purchases out in list order: the two blocks trade places. A
        # purchase's number is its place in the list (+0x50c), so dll/balance.c moves the copies already in
        # memory the same way when the switch flips (without that, a swap made the game spawn the mortar)
        ('swap', b'{"doctrine_120mm_sgrw42_late"', b'{"doctrine_150mm_nebelwerfer41_late"'),
    ]),
]


def block_at(t, start):
    """[begin, end) of the block that opens with start, up to and including its closing brace line"""
    b = t.find(start)
    assert b >= 0 and t.find(start, b + 1) < 0, start
    return b, t.index(b'\n}', b) + 2


def balance_header():
    import zipfile
    out = ['// made by tools/build.py from the game files: do not edit',
           'typedef struct { const char *pak, *path; unsigned crc; unsigned size; const unsigned char *data; } BalanceFile;']
    rows = []
    for i, (pak, path, changes) in enumerate(BALANCE):
        z = zipfile.ZipFile(os.path.join(GAME, pak))
        info = z.getinfo(path)
        t = z.read(path)
        for change in changes:
            if change[0] == 'swap':
                (a0, a1), (b0, b1) = sorted([block_at(t, change[1]), block_at(t, change[2])])
                assert a1 <= b0
                t = t[:a0] + t[b0:b1] + t[a1:b0] + t[a0:a1] + t[b1:]
                continue
            block, old, new = change
            assert len(old) == len(new), old
            b, end = block_at(t, block)
            k = t.find(old, b, end)
            assert k >= 0 and t.find(old, k + 1, end) < 0, (block, old)
            t = t[:k] + new + t[k + len(old):]
        out.append('static const unsigned char BAL_DATA_%d[] = {' % i)
        out += [' ' + ','.join(str(c) for c in t[j:j + 40]) + ',' for j in range(0, len(t), 40)]
        out.append('};')
        # crc and size of the game's own file: a game update that changes it turns the switch off
        rows.append('    {"%s", "%s", 0x%08xu, %du, BAL_DATA_%d},' % (pak, path, info.CRC, info.file_size, i))
    out += ['static const BalanceFile BALANCE_FILES[] = {'] + rows + ['};', '']
    h = os.path.join(OBJ, 'balance_data.h')
    text = '\n'.join(out)
    if not os.path.exists(h) or open(h).read() != text:
        open(h, 'w').write(text)
    return text


# Our layout of the Internet / LAN game list (dll/pvp.c): the game's own with a PVP button after JOIN.
# Same length as the game's file, so it is swapped in the stream the game opened, like the balance files:
# a commented-out debug block makes the room, the rest is padded with spaces.
PVP_BUTTON = b'''
                                                                                <BackgroundPanel backgroundResource="/interface/skin/game/frame/window_single/halo_bkg_line" sizeMode="fromContent" >
                                                                                    <Insets right="25" left="25"/>
                                                                                    <Children>
                                                                                        <MenuLinkButton id="btn_pvp">
                                                                                            <Insets left="3" right="3" />
                                                                                            <Text value="PVP" />
                                                                                        </MenuLinkButton>
                                                                                    </Children>
                                                                                </BackgroundPanel>'''


def ui_header():
    import zipfile
    pak, path = 'resource/interface/interface.pak', 'layout/PageMultiplayerCustom.xml'
    z = zipfile.ZipFile(os.path.join(GAME, pak))
    info = z.getinfo(path)
    t = z.read(path)
    a = t.index(b'<!--<StackPanel percent="0.4">')
    b = t.index(b'</StackPanel>-->', a) + len(b'</StackPanel>-->')
    u = t[:a] + t[b:]
    j = u.index(b'id="btn_join_game"')
    k = u.index(b'</BackgroundPanel>', j) + len(b'</BackgroundPanel>')
    u = u[:k] + PVP_BUTTON + u[k:]
    assert u.count(b'gap="55"') == 1
    u = u.replace(b'gap="55"', b'gap="40"')     # room for a fifth button in the row
    end = u.rindex(b'</Layout>')
    assert len(u) <= len(t)
    u = u[:end] + b' ' * (len(t) - len(u)) + u[end:]
    out = ['// made by tools/build.py from the game files: do not edit',
           'typedef struct { const char *pak, *path; unsigned crc; unsigned size; const unsigned char *data; } UiFile;',
           'static const unsigned char UI_DATA_0[] = {']
    out += [' ' + ','.join(str(c) for c in u[i:i + 40]) + ',' for i in range(0, len(u), 40)]
    out += ['};', 'static const UiFile UI_FILES[] = {',
            '    {"%s", "%s", 0x%08xu, %du, UI_DATA_0},' % (pak, path, info.CRC, info.file_size), '};', '']
    h = os.path.join(OBJ, 'ui_files.h')
    text = '\n'.join(out)
    if not os.path.exists(h) or open(h).read() != text:
        open(h, 'w').write(text)


def balance_tag(data_text):
    """version of the switch shown to players: the same for every build (full or competitive) made from the
    same dll/balance.c and the same copies, so equal tags mean the games agree on every purchase"""
    import zlib
    src = open(os.path.join(ROOT, 'dll', 'balance.c'), 'rb').read().replace(b'\r\n', b'\n')
    return '%04X' % (zlib.crc32(src + data_text.encode()) & 0xffff)


def main():
    os.makedirs(OBJ, exist_ok=True)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    tag = balance_tag(balance_header())
    ui_header()
    CFLAGS.append('-DBALANCE_TAG="%s"' % tag)
    objs = [obj(s) for s in C_SRC + CPP_SRC]
    # no -static: with -shared zig would emit an ar archive; its libc++ is linked in statically anyway
    zig('c++', *TARGET, '-shared', '-O2', '-o', OUT, *objs, *LIBS)
    print('built', OUT, os.path.getsize(OUT) // 1024, 'KB, balance version', tag)


if __name__ == '__main__':
    main()
