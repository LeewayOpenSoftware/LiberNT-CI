/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Servicing target: Online (running system) or Image (offline root)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The Image mode only reads and writes files below the given root. It never
 * loads or runs anything found there (DESIGN-v0, section 3).
 */

#include "libservicing.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define SERVICING_REL L"LiberNT\\Servicing"

static BOOL StartsWithNoCase(const WCHAR *s, const WCHAR *prefix)
{
    return _wcsnicmp(s, prefix, wcslen(prefix)) == 0;
}

BOOL LnTargetPathAllowed(const WCHAR *rel)
{
    static const WCHAR *roots[] =
    {
        L"Windows\\System32\\", L"Windows\\SysWOW64\\", L"Program Files\\", L"Program Files (x86)\\"
    };
    static const WCHAR *deniedPrefix[] =
    {
        L"Windows\\System32\\config\\", L"Windows\\System32\\boot\\", L"LiberNT\\"
    };
    static const WCHAR *deniedName[] =
    {
        L"ntoskrnl.exe", L"ntkrnlmp.exe", L"ntkrnlpa.exe", L"ntkrpamp.exe", L"ntdll.dll", L"smss.exe",
        L"csrss.exe", L"bootvid.dll", L"kdcom.dll", L"win32k.sys", L"win32kbase.sys", L"win32kfull.sys",
        L"autochk.exe", L"ntldr", L"bootmgr", L"freeldr.sys", L"freeldr.ini"
    };
    const WCHAR *base;
    size_t i;
    BOOL inRoot = FALSE;

    if (!LnIsSafeRelativePath(rel))
        return FALSE;
    for (i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
    {
        if (StartsWithNoCase(rel, roots[i]))
            inRoot = TRUE;
    }
    if (!inRoot)
        return FALSE;
    for (i = 0; i < sizeof(deniedPrefix) / sizeof(deniedPrefix[0]); i++)
    {
        if (StartsWithNoCase(rel, deniedPrefix[i]))
            return FALSE;
    }
    base = wcsrchr(rel, L'\\');
    base = base ? base + 1 : rel;
    for (i = 0; i < sizeof(deniedName) / sizeof(deniedName[0]); i++)
    {
        if (!_wcsicmp(base, deniedName[i]))
            return FALSE;
    }
    if (!_wcsnicmp(base, L"hal", 3) && wcslen(base) > 4 && !_wcsicmp(base + wcslen(base) - 4, L".dll"))
        return FALSE;                                   /* hal*.dll */
    return TRUE;
}

static BOOL IsGuid(const char *s)
{
    int i;

    if (strlen(s) != 36)
        return FALSE;
    for (i = 0; i < 36; i++)
    {
        char c = s[i];

        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (c != '-')
                return FALSE;
        }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return FALSE;
    }
    return TRUE;
}

static DWORD MakeIdentity(const LN_TARGET *t, char out[65])
{
    unsigned char seed[64];
    LARGE_INTEGER pc;
    FILETIME ft;
    DWORD pid = GetCurrentProcessId(), tick = GetTickCount();
    LN_SHA256_CTX c;
    unsigned char h[32];
    HMODULE k = GetModuleHandleW(L"kernel32.dll");

    QueryPerformanceCounter(&pc);
    GetSystemTimeAsFileTime(&ft);
    memset(seed, 0, sizeof(seed));
    memcpy(seed, &pc, sizeof(pc));
    memcpy(seed + 8, &ft, sizeof(ft));
    memcpy(seed + 16, &pid, sizeof(pid));
    memcpy(seed + 20, &tick, sizeof(tick));
    memcpy(seed + 24, &k, sizeof(k));
    LnSha256Init(&c);
    LnSha256Update(&c, seed, sizeof(seed));
    LnSha256Update(&c, t->root, wcslen(t->root) * sizeof(WCHAR));
    LnSha256Final(&c, h);
    {
        char hex[65];

        LnHexEncode(h, 32, hex);                         /* GUID text from the first 16 bytes */
        memcpy(out, hex, 8);       out[8] = '-';
        memcpy(out + 9, hex + 8, 4);   out[13] = '-';
        memcpy(out + 14, hex + 12, 4); out[18] = '-';
        memcpy(out + 19, hex + 16, 4); out[23] = '-';
        memcpy(out + 24, hex + 20, 12);
        out[36] = 0;
    }
    return ERROR_SUCCESS;
}

DWORD LnTargetOpen(LN_TARGET *t, LN_TARGET_MODE mode, const WCHAR *imageRoot, BOOL create)
{
    WCHAR win[MAX_PATH], *idPath;
    DWORD err, attrs;
    size_t n;
    BYTE *data;
    size_t len;

    memset(t, 0, sizeof(*t));
    t->mode = mode;
    if (mode == LnTargetOnline)
    {
        if (!GetSystemWindowsDirectoryW(win, MAX_PATH) || win[1] != L':')
            return ERROR_PATH_NOT_FOUND;
        t->root[0] = win[0];
        t->root[1] = L':';
        t->root[2] = L'\\';
        t->root[3] = 0;
    }
    else if (mode == LnTargetImage)
    {
        if (!imageRoot || !*imageRoot || wcslen(imageRoot) > MAX_PATH - 100)
            return ERROR_BAD_PATHNAME;
        wcscpy(t->root, imageRoot);
        n = wcslen(t->root);
        if (t->root[n - 1] != L'\\')
            wcscat(t->root, L"\\");
        attrs = GetFileAttributesW(t->root);
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            return ERROR_PATH_NOT_FOUND;
    }
    else
        return ERROR_INVALID_PARAMETER;

    wcscpy(t->servicing, t->root);
    wcscat(t->servicing, SERVICING_REL);

    idPath = LnJoin(t->servicing, L"target-id");
    if (!idPath)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnReadFileAll(idPath, &data, &len);
    if (!err)
    {
        if (len >= 36)
        {
            char id[65];

            memcpy(id, data, 36);
            id[36] = 0;
            if (IsGuid(id))
                strcpy(t->identity, id);
            else
                err = LN_E_MANIFEST_INVALID;
        }
        else
            err = LN_E_MANIFEST_INVALID;
        free(data);
    }
    else if (create && (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND))
    {
        MakeIdentity(t, t->identity);
        err = LnWriteFileAtomic(idPath, t->identity, 36);
    }
    free(idPath);
    return err;
}

WCHAR *LnTargetPath(const LN_TARGET *t, const WCHAR *rel)
{
    return LnJoin(t->servicing, rel);
}

WCHAR *LnTargetFile(const LN_TARGET *t, const WCHAR *rel)
{
    return LnJoin(t->root, rel);
}
