/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Buffers, UTF-8, file helpers with flush, path rules
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "libservicing.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---- growable buffer --------------------------------------------------- */

void LnBufInit(LN_BUF *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void LnBufFree(LN_BUF *b)
{
    free(b->data);
    LnBufInit(b);
}

DWORD LnBufAppend(LN_BUF *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap)
    {
        size_t cap = b->cap ? b->cap : 256;
        char *p;

        while (cap < b->len + n + 1)
            cap *= 2;
        p = realloc(b->data, cap);
        if (!p)
            return ERROR_NOT_ENOUGH_MEMORY;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
    return ERROR_SUCCESS;
}

DWORD LnBufAppendStr(LN_BUF *b, const char *s)
{
    return LnBufAppend(b, s, strlen(s));
}

DWORD LnBufAppendJsonString(LN_BUF *b, const char *s)
{
    DWORD err = LnBufAppend(b, "\"", 1);

    for (; !err && *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        char tmp[8];

        if (c == '"' || c == '\\')
        {
            tmp[0] = '\\'; tmp[1] = (char)c;
            err = LnBufAppend(b, tmp, 2);
        }
        else if (c == '\n') err = LnBufAppend(b, "\\n", 2);
        else if (c == '\r') err = LnBufAppend(b, "\\r", 2);
        else if (c == '\t') err = LnBufAppend(b, "\\t", 2);
        else if (c < 0x20)
        {
            static const char hex[] = "0123456789abcdef";
            tmp[0] = '\\'; tmp[1] = 'u'; tmp[2] = '0'; tmp[3] = '0';
            tmp[4] = hex[c >> 4]; tmp[5] = hex[c & 15];
            err = LnBufAppend(b, tmp, 6);
        }
        else
            err = LnBufAppend(b, (const char *)&c, 1);
    }
    return err ? err : LnBufAppend(b, "\"", 1);
}

DWORD LnBufAppendJsonWide(LN_BUF *b, const WCHAR *w)
{
    char *u = LnWideToUtf8(w);
    DWORD err;

    if (!u)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnBufAppendJsonString(b, u);
    free(u);
    return err;
}

/* ---- UTF-8 ------------------------------------------------------------- */

char *LnWideToUtf8(const WCHAR *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;

    if (n <= 0)
        return NULL;
    s = malloc((size_t)n);
    if (s && !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL))
    {
        free(s);
        return NULL;
    }
    return s;
}

WCHAR *LnUtf8ToWide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    WCHAR *w;

    if (n <= 0)
        return NULL;
    w = malloc((size_t)n * sizeof(WCHAR));
    if (w && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n))
    {
        free(w);
        return NULL;
    }
    return w;
}

/* ---- files ------------------------------------------------------------- */

DWORD LnReadFileAll(const WCHAR *path, BYTE **data, size_t *len)
{
    HANDLE h;
    LARGE_INTEGER size;
    BYTE *buf;
    DWORD got;

    *data = NULL;
    *len = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (256LL * 1024 * 1024))
    {
        DWORD err = GetLastError();
        CloseHandle(h);
        return err ? err : ERROR_FILE_TOO_LARGE;
    }
    buf = malloc((size_t)size.QuadPart + 1);
    if (!buf)
    {
        CloseHandle(h);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    if (size.QuadPart && (!ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart))
    {
        DWORD err = GetLastError();
        free(buf);
        CloseHandle(h);
        return err ? err : ERROR_READ_FAULT;
    }
    CloseHandle(h);
    buf[size.QuadPart] = 0;
    *data = buf;
    *len = (size_t)size.QuadPart;
    return ERROR_SUCCESS;
}

BOOL LnPathExists(const WCHAR *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

DWORD LnEnsureDir(const WCHAR *path)
{
    WCHAR tmp[MAX_PATH];
    size_t i, n = wcslen(path);

    if (n == 0 || n >= MAX_PATH)
        return ERROR_BAD_PATHNAME;
    wcscpy(tmp, path);
    for (i = 1; i <= n; i++)
    {
        if (tmp[i] == L'\\' || tmp[i] == 0)
        {
            WCHAR saved = tmp[i];

            if (i == 2 && tmp[1] == L':')
                continue;
            tmp[i] = 0;
            if (!CreateDirectoryW(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                return GetLastError();
            tmp[i] = saved;
        }
    }
    return ERROR_SUCCESS;
}

static void ParentOf(const WCHAR *path, WCHAR *out)
{
    WCHAR *p;

    wcscpy(out, path);
    p = wcsrchr(out, L'\\');
    if (p)
        *p = 0;
}

static DWORD FlushPath(const WCHAR *path)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok;

    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();
    ok = FlushFileBuffers(h);
    CloseHandle(h);
    return ok ? ERROR_SUCCESS : GetLastError();
}

DWORD LnWriteFileAtomic(const WCHAR *path, const void *data, size_t len)
{
    WCHAR tmp[MAX_PATH], parent[MAX_PATH];
    HANDLE h;
    DWORD wrote, err;

    if (wcslen(path) + 5 >= MAX_PATH)
        return ERROR_BAD_PATHNAME;
    ParentOf(path, parent);
    err = LnEnsureDir(parent);
    if (err)
        return err;

    wcscpy(tmp, path);
    wcscat(tmp, L".tmp");
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (len && (!WriteFile(h, data, (DWORD)len, &wrote, NULL) || wrote != (DWORD)len))
    {
        err = GetLastError();
        CloseHandle(h);
        DeleteFileW(tmp);
        return err ? err : ERROR_WRITE_FAULT;
    }
    if (!FlushFileBuffers(h))
    {
        err = GetLastError();
        CloseHandle(h);
        DeleteFileW(tmp);
        return err;
    }
    CloseHandle(h);
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        err = GetLastError();
        DeleteFileW(tmp);
        return err;
    }
    return ERROR_SUCCESS;
}

DWORD LnCopyFileFlush(const WCHAR *src, const WCHAR *dst)
{
    WCHAR parent[MAX_PATH];
    DWORD err;

    ParentOf(dst, parent);
    err = LnEnsureDir(parent);
    if (err)
        return err;
    if (!CopyFileW(src, dst, FALSE))
        return GetLastError();
    return FlushPath(dst);
}

BOOL LnIsSafeRelativePath(const WCHAR *path)
{
    const WCHAR *p = path;

    if (!path || !*path)
        return FALSE;
    if (path[0] == L'\\' || path[0] == L'/' || wcschr(path, L':') || wcschr(path, L'/'))
        return FALSE;

    while (*p)
    {
        const WCHAR *e = wcschr(p, L'\\');
        size_t n = e ? (size_t)(e - p) : wcslen(p);
        size_t i;

        if (n == 0)                                 /* empty component */
            return FALSE;
        if ((n == 1 && p[0] == L'.') || (n == 2 && p[0] == L'.' && p[1] == L'.'))
            return FALSE;
        for (i = 0; i < n; i++)
        {
            if (p[i] < 0x20 || p[i] == L'*' || p[i] == L'?' || p[i] == L'"' || p[i] == L'<' ||
                p[i] == L'>' || p[i] == L'|')
                return FALSE;
        }
        if (p[n - 1] == L'.' || p[n - 1] == L' ')   /* trailing dot/space aliases another name */
            return FALSE;
        if (!e)
            break;
        p = e + 1;
    }
    return TRUE;
}

static void PutNum(char *p, unsigned v, int digits)
{
    while (digits--)
    {
        p[digits] = (char)('0' + (v % 10));
        v /= 10;
    }
}

void LnNowUtc(char out[32])
{
    SYSTEMTIME st;

    GetSystemTime(&st);
    PutNum(out, st.wYear, 4);          out[4] = '-';
    PutNum(out + 5, st.wMonth, 2);     out[7] = '-';
    PutNum(out + 8, st.wDay, 2);       out[10] = 'T';
    PutNum(out + 11, st.wHour, 2);     out[13] = ':';
    PutNum(out + 14, st.wMinute, 2);   out[16] = ':';
    PutNum(out + 17, st.wSecond, 2);   out[19] = 'Z';
    out[20] = 0;
}

WCHAR *LnJoin(const WCHAR *a, const WCHAR *b)
{
    size_t la = wcslen(a), lb = wcslen(b);
    WCHAR *r = malloc((la + lb + 2) * sizeof(WCHAR));

    if (!r)
        return NULL;
    wcscpy(r, a);
    if (la && a[la - 1] != L'\\')
        wcscat(r, L"\\");
    wcscat(r, b);
    return r;
}

void LnI64ToStr(long long v, char out[24])
{
    char tmp[24];
    int n = 0, i = 0;
    unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1ULL : (unsigned long long)v;

    if (v < 0)
        out[i++] = '-';
    do
    {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    while (n)
        out[i++] = tmp[--n];
    out[i] = 0;
}
