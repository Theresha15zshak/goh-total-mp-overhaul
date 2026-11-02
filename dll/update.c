// The mod's updater. When the server's list says this build is old (online.c integrity_check), one button in the
// version box (ui.cpp) downloads the newest official build of the same kind from the latest GitHub release and puts
// it in place of this DLL for the next start of the game.
//
// It installs only a file that carries a signature made with the release key (tools/sign.py, ECDSA P-256; the
// private half never leaves the author's PC, the public half is built in here from tools/update_pubkey.txt) AND
// whose SHA-256 is the one the server lists as the build to have. Whoever takes over the server or the GitHub
// account still cannot make the mod install a file of theirs: they have no key to sign it with.
//
// The running DLL cannot be overwritten but can be renamed: it becomes iphlpapi.old.dll and the new one takes its
// name; the game loads the new one when it starts next. Everything here runs on a thread of its own.
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "goh_replay.h"
#include "update_key.h"         // UPDATE_PUBKEY: a BCRYPT_ECCPUBLIC_BLOB, made by tools/build.py

#define RELEASE_HOST L"github.com"
#define RELEASE_PATH L"/Theresha15zshak/goh-total-mp-overhaul/releases/latest/download/"
#define MAX_DLL (16 << 20)

volatile long g_update_state;            // UPDATE_*
volatile long g_update_pct;
char g_update_msg[160];                  // UTF-8: why it failed, or what it did

static void fail(const char *ru, const char *en, const char *log) {
    snprintf(g_update_msg, sizeof g_update_msg, "%s", TR(ru, en));
    rc_log("update: %s", log);
    g_update_state = UPDATE_FAILED;
}

// a file of the latest release, whole (GitHub answers with a redirect to its file store, which WinHTTP follows)
static unsigned char *download(HINTERNET s, const char *name, size_t max, size_t *n, int progress) {
    wchar_t path[256];
    swprintf(path, 256, L"%ls%hs", RELEASE_PATH, name);
    HINTERNET c = WinHttpConnect(s, RELEASE_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    unsigned char *out = NULL;
    *n = 0;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD st = 0, len = 0, sz = sizeof st;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &st, &sz, NULL);
        sz = sizeof len;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, NULL, &len, &sz, NULL);
        out = st == 200 ? malloc(max) : NULL;
        if (!out) rc_log("update: %s: HTTP %lu", name, st);
        DWORD got;
        while (out && *n < max && WinHttpReadData(r, out + *n, (DWORD)(max - *n), &got) && got) {
            *n += got;
            if (progress && len) g_update_pct = (long)(*n * 100 / len);
        }
        if (out && *n >= max) { free(out); out = NULL; }     // larger than any build of ours
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    return out;
}

static int sha256(const unsigned char *p, size_t n, unsigned char h[32]) {
    return !BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)p, (ULONG)n, h, 32);
}

// the signature (64 bytes, r then s) over the file's SHA-256, made with the release key
int update_signature_ok(const unsigned char *file, size_t n, const unsigned char *sig, size_t sig_n) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    unsigned char h[32];
    int ok = sizeof UPDATE_PUBKEY > 8 && sig_n == 64 && sha256(file, n, h) &&
             !BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) &&
             !BCryptImportKeyPair(alg, NULL, BCRYPT_ECCPUBLIC_BLOB, &key, (PUCHAR)UPDATE_PUBKEY, sizeof UPDATE_PUBKEY, 0) &&
             !BCryptVerifySignature(key, NULL, h, 32, (PUCHAR)sig, (ULONG)sig_n, 0);
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// this DLL's own file: renamed to iphlpapi.old.dll, the new one written in its place (put back if that fails)
int update_put_in_place(const wchar_t *self, const unsigned char *dll, size_t n) {
    wchar_t dir[MAX_PATH], fresh[MAX_PATH], old[MAX_PATH];
    wcsncpy(dir, self, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return 0;
    slash[1] = 0;
    swprintf(fresh, MAX_PATH, L"%lsiphlpapi.update.dll", dir);
    swprintf(old, MAX_PATH, L"%lsiphlpapi.old.dll", dir);
    HANDLE h = CreateFileW(fresh, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(h, dll, (DWORD)n, &put, NULL) && put == n;
    CloseHandle(h);
    if (!ok) { DeleteFileW(fresh); return 0; }
    DeleteFileW(old);
    if (!MoveFileExW(self, old, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(fresh); return 0; }
    if (!MoveFileExW(fresh, self, 0)) { MoveFileExW(old, self, 0); DeleteFileW(fresh); return 0; }
    return 1;
}

static DWORD WINAPI update_thread(LPVOID unused) {
    const char *kind = g_integrity_kind, *want = g_integrity_newest_sha;
    const char *asset = !strcmp(kind, "competitive") ? "iphlpapi-competitive.dll" : "iphlpapi.dll";
    char sig_name[64];
    snprintf(sig_name, sizeof sig_name, "%s.sig", asset);
    if (sizeof UPDATE_PUBKEY <= 8) { fail("в этой сборке нет ключа обновлений", "this build has no update key", "no key built in"); return 0; }
    if (strlen(want) != 64) { fail("сервер не назвал новую версию", "the server named no new version", "no newest SHA-256"); return 0; }
    HINTERNET s = WinHttpOpen(L"goh-mod", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"goh-mod", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { fail("нет сети", "no network", "WinHttpOpen failed"); return 0; }
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    size_t n = 0, sig_n = 0;
    unsigned char *sig = download(s, sig_name, 1024, &sig_n, 0);
    unsigned char *dll = sig ? download(s, asset, MAX_DLL, &n, 1) : NULL;
    WinHttpCloseHandle(s);
    unsigned char h[32];
    char got[65] = "";
    if (dll && sha256(dll, n, h)) for (int i = 0; i < 32; i++) snprintf(got + 2 * i, 3, "%02x", h[i]);
    wchar_t self[MAX_PATH];
    HMODULE me = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)update_thread, &me);
    if (!sig || !dll) fail("не удалось скачать с GitHub", "could not download from GitHub", "download failed");
    else if (n < 2 || dll[0] != 'M' || dll[1] != 'Z') fail("скачан не тот файл", "the download is not a DLL", "not a DLL");
    else if (!update_signature_ok(dll, n, sig, sig_n)) fail("подпись файла не сошлась: не ставлю", "the file's signature is wrong: not installed", "BAD SIGNATURE, not installed");
    else if (strcmp(got, want)) fail("файл не тот, что сервер назвал новой версией", "the file is not the one the server named", "SHA-256 is not the listed one");
    else if (!me || !GetModuleFileNameW(me, self, MAX_PATH) || !update_put_in_place(self, dll, n))
        fail("не удалось заменить файл мода (закройте игру и обновите вручную)", "could not replace the mod's file (update by hand)", "could not put the file in place");
    else {
        snprintf(g_update_msg, sizeof g_update_msg, TR("Обновлено до %s: перезапустите игру", "Updated to %s: restart the game"), g_integrity_newest);
        rc_log("update: %s (%.16s...) in place, signature good, next start loads it", g_integrity_newest, got);
        g_update_state = UPDATE_DONE;
    }
    free(sig);
    free(dll);
    return 0;
}

void update_start(void) {
    if (InterlockedCompareExchange(&g_update_state, UPDATE_WORKING, UPDATE_IDLE) != UPDATE_IDLE &&
        InterlockedCompareExchange(&g_update_state, UPDATE_WORKING, UPDATE_FAILED) != UPDATE_FAILED)
        return;
    g_update_pct = 0;
    g_update_msg[0] = 0;
    HANDLE t = CreateThread(NULL, 0, update_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else g_update_state = UPDATE_FAILED;
}

void update_open_release_page(void) {
    ShellExecuteA(NULL, "open", "https://github.com/Theresha15zshak/goh-total-mp-overhaul/releases/latest", NULL, NULL, SW_SHOWNORMAL);
}
