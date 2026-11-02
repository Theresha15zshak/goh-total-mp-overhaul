"""Make the builds in build/ the official ones: each is signed for the mod's updater (tools/sign.py, the key outside
the repository) and its SHA-256 goes on the server's list (server/src/versions.js), the earlier builds of the same
kind staying there as old ones. The DLL compares itself with that list, and offers the update when it is old.

    python tools/build.py && python tools/build.py competitive && python tools/release.py [--no-server]

--no-server: only signs (the server's list stays as it is: nobody is told about the new build yet).
Then the GitHub release gets the assets the updater downloads (dll/update.c), named as ASSETS says:
    gh release create <tag> <zip> build/iphlpapi.dll build/iphlpapi.dll.sig ...  (see the names printed)

Needs wrangler signed in (server/). Release only what is given to players: any DLL on the list counts as
official, and one built after it (even from the same code) does not.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sign  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILDS = [('full', os.path.join(ROOT, 'build', 'iphlpapi.dll')),
          ('competitive', os.path.join(ROOT, 'build', 'competitive', 'iphlpapi.dll'))]
# the release's file names per kind, as dll/update.c downloads them
ASSETS = {'full': 'iphlpapi.dll', 'competitive': 'iphlpapi-competitive.dll'}


def d1(sql):
    r = subprocess.run('npx wrangler d1 execute goh-online --remote --command "%s"' % sql, shell=True,
                       cwd=os.path.join(ROOT, 'server'), capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stdout + r.stderr)


def main():
    server = '--no-server' not in sys.argv[1:]
    now = int(time.time())
    out = os.path.join(ROOT, 'build', 'release')
    os.makedirs(out, exist_ok=True)
    for kind, path in BUILDS:
        if not os.path.exists(path):
            print('no', kind, 'build, skipped')
            continue
        data = open(path, 'rb').read()
        sha = hashlib.sha256(data).hexdigest()
        version = open(path + '.version').read().strip()
        assert all(c.isalnum() or c in '.-' for c in version), version
        sig = sign.sign(data)
        assert sign.verify(data, sig)
        open(path + '.sig', 'wb').write(sig)
        shutil.copyfile(path, os.path.join(out, ASSETS[kind]))
        open(os.path.join(out, ASSETS[kind] + '.sig'), 'wb').write(sig)
        if server:
            d1("UPDATE mod_versions SET current = 0 WHERE kind = '%s'" % kind)
            d1("INSERT INTO mod_versions (sha256, version, kind, released_at, current) VALUES ('%s', '%s', '%s', %d, 1) "
               "ON CONFLICT(sha256) DO UPDATE SET current = 1" % (sha, version, kind, now))
        print(kind, version, sha, 'signed' + (', on the server' if server else ', NOT on the server'))
    print('release assets:', out)


if __name__ == '__main__':
    main()
