/*
 * The core's package maker (core/src/appx/mg_appx.c) on a PC, to try it where makeappx and signtool are at hand:
 *
 *   appx-tool <folder> <out.appx> <key file> <certificate file> [<publisher>]
 *
 * packs every file under <folder> (AppxManifest.xml among them; a block map, content types, signature or AppxMetadata
 * there is left out) into <out.appx>, signed with the key, made with its certificate on the first use (subject
 * <publisher>, CN=Computershik by default). Built by tools/build-appx-tool.sh; tools/appx-verify.py checks what it made.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "appx/mg_appx.h"

static int add_folder(mg_appx *a, const wchar_t *root, const wchar_t *rel)
{
    wchar_t pattern[1024];
    WIN32_FIND_DATAW f;
    HANDLE h;
    int count = 0;
    swprintf(pattern, 1024, L"%ls\\%ls%ls*", root, rel, rel[0] ? L"\\" : L"");
    h = FindFirstFileW(pattern, &f);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        wchar_t sub[1024], full[1024];
        char name[1024], path[2048];
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
        swprintf(sub, 1024, L"%ls%ls%ls", rel, rel[0] ? L"\\" : L"", f.cFileName);
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!_wcsicmp(sub, L"AppxMetadata")) continue;
            count += add_folder(a, root, sub);
            continue;
        }
        if (!_wcsicmp(sub, L"AppxBlockMap.xml") || !_wcsicmp(sub, L"[Content_Types].xml") || !_wcsicmp(sub, L"AppxSignature.p7x")) continue;
        swprintf(full, 1024, L"%ls\\%ls", root, sub);
        WideCharToMultiByte(CP_UTF8, 0, sub, -1, name, sizeof(name), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, full, -1, path, sizeof(path), NULL, NULL);
        for (char *s = name; *s; s++)
            if (*s == '\\') *s = '/';
        if (!mg_appx_add_file(a, name, path)) return -100000;
        count++;
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return count;
}

int wmain(int argc, wchar_t **argv)
{
    char out[1024], key[1024], cert[1024], publisher[256] = "CN=Computershik", err[300];
    mg_appx_key *k;
    mg_appx *a;
    int n;
    if (argc < 5) {
        fprintf(stderr, "appx-tool <folder> <out.appx> <key file> <certificate file> [<publisher>]\n");
        return 2;
    }
    WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, out, sizeof(out), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, key, sizeof(key), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, argv[4], -1, cert, sizeof(cert), NULL, NULL);
    if (argc > 5) WideCharToMultiByte(CP_UTF8, 0, argv[5], -1, publisher, sizeof(publisher), NULL, NULL);
    k = mg_appx_key_open(key, cert, publisher, err, sizeof(err));
    if (!k) {
        fprintf(stderr, "key: %s\n", err);
        return 1;
    }
    a = mg_appx_begin(out, err, sizeof(err));
    if (!a) {
        fprintf(stderr, "begin: %s\n", err);
        return 1;
    }
    n = add_folder(a, argv[1], L"");
    if (!mg_appx_finish(a, k, err, sizeof(err))) {
        fprintf(stderr, "finish: %s\n", err);
        return 1;
    }
    printf("%d files packed into %s\n", n, out);
    mg_appx_key_close(k);
    return 0;
}
