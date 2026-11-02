// dll/update.c away from the game: the signature check (a file signed by tools/sign.py passes, a changed byte or
// another signature does not) and putting a file in place of a DLL that is loaded, as the mod's own is.
//   update_test <signed file> <scratch dir>     (the signed file has its .sig next to it)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dll/goh_replay.h"

// what update.c takes from the rest of the mod
int lang_en(void) { return 1; }
void rc_log(const char *fmt, ...) { (void)fmt; }
char g_integrity_newest[32], g_integrity_newest_sha[65], g_integrity_kind[16];
int update_put_in_place(const wchar_t *self, const unsigned char *dll, size_t n);

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n);
    fread(b, 1, *n, f);
    fclose(f);
    return b;
}

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; }

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    char sig_path[MAX_PATH];
    snprintf(sig_path, sizeof sig_path, "%s.sig", argv[1]);
    size_t n = 0, sn = 0;
    unsigned char *file = slurp(argv[1], &n), *sig = slurp(sig_path, &sn);
    if (!file || !sig) { printf("no file or signature\n"); return 2; }
    check(update_signature_ok(file, n, sig, sn), "the signed file passes");
    file[n / 2] ^= 1;
    check(!update_signature_ok(file, n, sig, sn), "one byte changed: refused");
    file[n / 2] ^= 1;
    sig[10] ^= 1;
    check(!update_signature_ok(file, n, sig, sn), "another signature: refused");
    sig[10] ^= 1;
    check(!update_signature_ok(file, n, sig, 63), "a short signature: refused");

    // a loaded DLL renamed away and a new file in its place
    wchar_t dir[MAX_PATH], self[MAX_PATH], old[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, dir, MAX_PATH);
    swprintf(self, MAX_PATH, L"%ls\\iphlpapi.dll", dir);
    swprintf(old, MAX_PATH, L"%ls\\iphlpapi.old.dll", dir);
    DeleteFileW(old);
    CopyFileW(L"C:\\Windows\\System32\\version.dll", self, FALSE);
    HMODULE loaded = LoadLibraryExW(self, NULL, DONT_RESOLVE_DLL_REFERENCES);
    check(loaded != NULL, "a DLL loaded from the scratch dir");
    check(update_put_in_place(self, file, n), "the new file put in place of the loaded one");
    size_t m = 0;
    char selfa[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, self, -1, selfa, MAX_PATH, NULL, NULL);
    unsigned char *now = slurp(selfa, &m);
    check(now && m == n && !memcmp(now, file, n), "iphlpapi.dll is the new file");
    check(GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES, "the old one kept as iphlpapi.old.dll");
    FreeLibrary(loaded);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
