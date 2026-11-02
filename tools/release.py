"""Make the builds in build/ the official ones: their SHA-256 go on the server's list (server/src/versions.js),
the earlier builds of the same kind stay there as old ones. The DLL compares itself with that list.

    python tools/build.py && python tools/build.py competitive && python tools/release.py

Needs wrangler signed in (server/). Release only what is given to players: any DLL on the list counts as
official, and one built after it (even from the same code) does not.
"""
import hashlib
import os
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILDS = [('full', os.path.join(ROOT, 'build', 'iphlpapi.dll')),
          ('competitive', os.path.join(ROOT, 'build', 'competitive', 'iphlpapi.dll'))]


def d1(sql):
    r = subprocess.run('npx wrangler d1 execute goh-online --remote --command "%s"' % sql, shell=True,
                       cwd=os.path.join(ROOT, 'server'), capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stdout + r.stderr)


def main():
    now = int(time.time())
    for kind, path in BUILDS:
        if not os.path.exists(path):
            print('no', kind, 'build, skipped')
            continue
        sha = hashlib.sha256(open(path, 'rb').read()).hexdigest()
        version = open(path + '.version').read().strip()
        assert all(c.isalnum() or c in '.-' for c in version), version
        d1("UPDATE mod_versions SET current = 0 WHERE kind = '%s'" % kind)
        d1("INSERT INTO mod_versions (sha256, version, kind, released_at, current) VALUES ('%s', '%s', '%s', %d, 1) "
           "ON CONFLICT(sha256) DO UPDATE SET current = 1" % (sha, version, kind, now))
        print(kind, version, sha)


if __name__ == '__main__':
    main()
