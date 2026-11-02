"""Update signatures: ECDSA P-256 with Windows' own BCrypt (no packages), the same API the DLL checks them with.

The private key lives outside the repository, in KEY_FILE: whoever has it can ship updates every player's mod
installs by itself, so keep a copy somewhere safe (a USB stick, a password manager) and never commit it. Lost,
no update can be signed any more: a new key goes into the next DLL and everyone installs that one by hand.

    python tools/sign.py keygen        once: makes KEY_FILE and tools/update_pubkey.txt (the public half, committed)
    python tools/sign.py verify FILE   checks FILE.sig against tools/update_pubkey.txt
Signing itself is done by tools/release.py (sign()).
"""
import ctypes
import hashlib
import os
import sys
from ctypes import byref, c_void_p, c_ulong, create_string_buffer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEY_FILE = os.path.join(os.path.expanduser('~'), '.goh_total_mp_overhaul', 'update_signing_key.bin')
PUB_FILE = os.path.join(ROOT, 'tools', 'update_pubkey.txt')

bc = ctypes.WinDLL('bcrypt')
ALG = 'ECDSA_P256'


def _ok(status, what):
    if status:
        raise SystemExit('%s failed: 0x%08x' % (what, status & 0xffffffff))


def _alg():
    h = c_void_p()
    _ok(bc.BCryptOpenAlgorithmProvider(byref(h), ctypes.c_wchar_p(ALG), None, 0), 'BCryptOpenAlgorithmProvider')
    return h


def _export(key, kind):
    n = c_ulong()
    _ok(bc.BCryptExportKey(key, None, ctypes.c_wchar_p(kind), None, 0, byref(n), 0), 'BCryptExportKey')
    buf = create_string_buffer(n.value)
    _ok(bc.BCryptExportKey(key, None, ctypes.c_wchar_p(kind), buf, n, byref(n), 0), 'BCryptExportKey')
    return buf.raw[:n.value]


def _import(blob, kind):
    alg, key = _alg(), c_void_p()
    _ok(bc.BCryptImportKeyPair(alg, None, ctypes.c_wchar_p(kind), byref(key), blob, len(blob), 0), 'BCryptImportKeyPair')
    return key


def keygen():
    if os.path.exists(KEY_FILE):
        raise SystemExit('%s exists already: a new key would make every installed mod refuse the next update' % KEY_FILE)
    alg, key = _alg(), c_void_p()
    _ok(bc.BCryptGenerateKeyPair(alg, byref(key), 256, 0), 'BCryptGenerateKeyPair')
    _ok(bc.BCryptFinalizeKeyPair(key, 0), 'BCryptFinalizeKeyPair')
    os.makedirs(os.path.dirname(KEY_FILE), exist_ok=True)
    open(KEY_FILE, 'wb').write(_export(key, 'ECCPRIVATEBLOB'))
    open(PUB_FILE, 'w').write(_export(key, 'ECCPUBLICBLOB').hex() + '\n')
    print('private key:', KEY_FILE, '(back it up, never commit it)')
    print('public key: ', PUB_FILE)


def sign(data):
    """64 bytes, r then s, over the SHA-256 of data"""
    key = _import(open(KEY_FILE, 'rb').read(), 'ECCPRIVATEBLOB')
    h = hashlib.sha256(data).digest()
    sig, n = create_string_buffer(64), c_ulong()
    _ok(bc.BCryptSignHash(key, None, h, 32, sig, 64, byref(n), 0), 'BCryptSignHash')
    return sig.raw[:n.value]


def verify(data, sig):
    key = _import(bytes.fromhex(open(PUB_FILE).read().strip()), 'ECCPUBLICBLOB')
    h = hashlib.sha256(data).digest()
    return bc.BCryptVerifySignature(key, None, h, 32, sig, len(sig), 0) == 0


if __name__ == '__main__':
    if sys.argv[1:] == ['keygen']:
        keygen()
    elif len(sys.argv) == 3 and sys.argv[1] == 'verify':
        f = sys.argv[2]
        print('signature good' if verify(open(f, 'rb').read(), open(f + '.sig', 'rb').read()) else 'signature BAD')
    else:
        print(__doc__)
