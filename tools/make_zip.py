"""The release archive: the previous release's archive with files/iphlpapi.dll replaced by the signed build.
    python tools/make_zip.py <version> <previous version>
Everything else (installer, readmes, xvidcore, goh_snapshots.ini) is taken over byte for byte, names included."""
import os, sys, zipfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ver, prev = sys.argv[1], sys.argv[2]
src = os.path.join(ROOT, 'dist', 'GoH_TOTAL_MP_OVERHAUL_%s.zip' % prev)
out = os.path.join(ROOT, 'dist', 'GoH_TOTAL_MP_OVERHAUL_%s.zip' % ver)
dll = open(os.path.join(ROOT, 'build', 'release', 'iphlpapi.dll'), 'rb').read()
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as zout:
    for info in zin.infolist():
        data = dll if info.filename.endswith('/files/iphlpapi.dll') else zin.read(info)
        zout.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)
with zipfile.ZipFile(out) as z, zipfile.ZipFile(src) as zp:
    assert z.testzip() is None
    assert [i.filename for i in z.infolist()] == [i.filename for i in zp.infolist()]
    assert z.read('GoH_TOTAL_MP_OVERHAUL/files/iphlpapi.dll') == dll
print(out, os.path.getsize(out), 'same list as', prev, ': True, dll same: True')
