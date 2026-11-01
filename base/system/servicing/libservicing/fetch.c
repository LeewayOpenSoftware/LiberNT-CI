/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Fetch servicing packages over HTTP for add-package --from
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Minimal HTTP/1.0 client on raw winsock. WinHTTP is not usable on current
 * targets (request path works, the response path fails), so this speaks just
 * enough HTTP to download the manifest, its signature and the payloads the
 * signed manifest names. The transport is untrusted: the existing verify and
 * stage path is the only authority.
 */
#include "libservicing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#define LN_FETCH_DEFAULT_CAP (64ULL * 1024ULL * 1024ULL)
#define LN_FETCH_DEFAULT_TIMEOUT_MS 30000
#define LN_FETCH_IO_CHUNK 32768

static ULONGLONG
LnFetchCapBytes(VOID)
{
    WCHAR buf[32];
    DWORD n = GetEnvironmentVariableW(L"LN_FETCH_MAX_BYTES", buf, 32);
    if (n > 0 && n < 32)
        return _wcstoui64(buf, NULL, 10);
    return LN_FETCH_DEFAULT_CAP;
}

static DWORD
LnFetchTimeoutMs(VOID)
{
    WCHAR buf[24];
    DWORD n = GetEnvironmentVariableW(L"LN_FETCH_TIMEOUT_MS", buf, 24);
    if (n > 0 && n < 24)
        return wcstoul(buf, NULL, 10);
    return LN_FETCH_DEFAULT_TIMEOUT_MS;
}

/* http://host[:port][/path...]; host is an IPv4 literal or a DNS name.
 * Everything else (https, userinfo, backslashes, empty path) is rejected. */
static DWORD
LnFetchParseUrl(const WCHAR *url, WCHAR *host, DWORD hostCch, DWORD *port, const WCHAR **path)
{
    const WCHAR *p, *slash;

    if (wcsncmp(url, L"http://", 7) != 0)
        return ERROR_INVALID_DATA;
    p = url + 7;
    if (wcschr(p, L'@') != NULL || wcschr(p, L'\\') != NULL)
        return ERROR_INVALID_DATA;

    slash = wcschr(p, L'/');
    {
        const WCHAR *colon = wcschr(p, L':');
        const WCHAR *hend = slash ? slash : p + wcslen(p);
        if (colon && colon < hend)
        {
            DWORD hp = wcstoul(colon + 1, NULL, 10);
            if (hp == 0 || hp > 65535)
                return ERROR_INVALID_DATA;
            *port = hp;
            if ((DWORD)(colon - p) >= hostCch)
                return ERROR_INVALID_DATA;
            wcsncpy(host, p, colon - p);
            host[colon - p] = 0;
        }
        else
        {
            if ((DWORD)(hend - p) == 0 || (DWORD)(hend - p) >= hostCch)
                return ERROR_INVALID_DATA;
            wcsncpy(host, p, hend - p);
            host[hend - p] = 0;
            *port = 80;
        }
    }
    if (host[0] == 0)
        return ERROR_INVALID_DATA;
    *path = slash ? slash : L"/";
    return ERROR_SUCCESS;
}

/* One GET, status 200 only, body capped. Partial output is always removed. */
DWORD
LnFetchFile(const WCHAR *url, const WCHAR *destPath)
{
    WCHAR host[256];
    const WCHAR *path;
    DWORD port, err = ERROR_SUCCESS, wsaErr = 0, timeoutMs;
    ULONGLONG cap, total = 0;
    WSADATA wd;
    SOCKET s = INVALID_SOCKET;
    struct sockaddr_in sa;
    struct hostent *he;
    CHAR req[1200], *io = NULL;
    LONG status;
    HANDLE f = INVALID_HANDLE_VALUE;
    BOOL started = FALSE;
    DWORD got, sent = 0, reqLen, bodyOff;
    CHAR *header = NULL, *hdrEnd;
    const CHAR *bodyStart;

    err = LnFetchParseUrl(url, host, 256, &port, &path);
    if (err != ERROR_SUCCESS)
        return err;

    cap = LnFetchCapBytes();
    timeoutMs = LnFetchTimeoutMs();

    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
        return ERROR_NOT_SUPPORTED;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        wsaErr = WSAGetLastError();
        err = ERROR_NOT_SUPPORTED;
        goto done;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const CHAR *)&timeoutMs, sizeof(timeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const CHAR *)&timeoutMs, sizeof(timeoutMs));

    {
        CHAR hostA[256], pathA[600];
        WideCharToMultiByte(CP_ACP, 0, host, -1, hostA, 256, NULL, NULL);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((WORD)port);
        sa.sin_addr.s_addr = inet_addr(hostA);
        if (sa.sin_addr.s_addr == INADDR_NONE)
        {
            he = gethostbyname(hostA);
            if (he == NULL || he->h_addr_list[0] == NULL)
            {
                wsaErr = WSAGetLastError();
                err = ERROR_NOT_FOUND;
                goto done;
            }
            sa.sin_addr.s_addr = *(ULONG *)he->h_addr_list[0];
        }

        if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0)
        {
            wsaErr = WSAGetLastError();
            err = ERROR_NOT_CONNECTED;
            goto done;
        }

        WideCharToMultiByte(CP_ACP, 0, path, -1, pathA, 600, NULL, NULL);
        reqLen = (DWORD)_snprintf(req, sizeof(req),
                                  "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: libernt-update\r\n"
                                  "Connection: close\r\n\r\n",
                                  pathA, hostA);
    }
    while (sent < reqLen)
    {
        int w = send(s, req + sent, reqLen - sent, 0);
        if (w <= 0)
        {
            wsaErr = WSAGetLastError();
            err = ERROR_WRITE_FAULT;
            goto done;
        }
        sent += (DWORD)w;
    }

    io = malloc(LN_FETCH_IO_CHUNK);
    header = malloc(65536);
    if (io == NULL || header == NULL)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    {
        DWORD filled = 0;
        for (;;)
        {
            int r = recv(s, header + filled, 65536 - 1 - filled, 0);
            if (r <= 0)
                break;
            filled += (DWORD)r;
            header[filled] = 0;
            if (strstr(header, "\r\n\r\n"))
                break;
            if (filled == 65535)
            {
                err = ERROR_BAD_FORMAT;
                goto done;
            }
        }
        hdrEnd = strstr(header, "\r\n\r\n");
        if (filled == 0 || strncmp(header, "HTTP/1.", 7) != 0 || hdrEnd == NULL)
        {
            err = ERROR_BAD_FORMAT;
            goto done;
        }
        status = strtol(header + 8, NULL, 10);
        if (status != 200)
        {
            err = ERROR_INVALID_DATA;
            goto done;
        }
        bodyStart = hdrEnd + 4;
        bodyOff = filled - (DWORD)(bodyStart - header);
    }

    f = CreateFileW(destPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
    {
        err = GetLastError();
        goto done;
    }
    started = TRUE;

    if (bodyOff > 0)
    {
        if (!WriteFile(f, bodyStart, bodyOff, &got, NULL))
        {
            err = GetLastError();
            goto done;
        }
        total += bodyOff;
        if (total > cap)
        {
            err = ERROR_FILE_TOO_LARGE;
            goto done;
        }
    }
    free(header);
    header = NULL;
    for (;;)
    {
        int r = recv(s, io, LN_FETCH_IO_CHUNK, 0);
        if (r == 0)
            break;
        if (r < 0)
        {
            wsaErr = WSAGetLastError();
            err = ERROR_READ_FAULT;
            goto done;
        }
        if (!WriteFile(f, io, (DWORD)r, &got, NULL))
        {
            err = GetLastError();
            goto done;
        }
        total += (ULONGLONG)(ULONG)r;
        if (total > cap)
        {
            err = ERROR_FILE_TOO_LARGE;
            goto done;
        }
    }

done:
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    if (started && err != ERROR_SUCCESS)
        DeleteFileW(destPath);
    if (s != INVALID_SOCKET)
        closesocket(s);
    free(header);
    free(io);
    WSACleanup();
    if (err == ERROR_SUCCESS && wsaErr != 0)
        err = wsaErr;
    return err;
}

/* Download manifest.json + manifest.sig + the payloads named by the signed
 * manifest into destDir (a fresh package directory). Per-payload hashes are
 * checked as they land; the add path re-verifies everything authoritatively. */
DWORD
LnFetchPackage(const WCHAR *baseUrl, const WCHAR *destDir)
{
    WCHAR *url = NULL, *file = NULL, *payloadDir = NULL;
    LN_MANIFEST m;
    DWORD err;
    size_t i;

    memset(&m, 0, sizeof(m));

    err = LnEnsureDir(destDir);
    if (err != ERROR_SUCCESS)
        return err;

    url = malloc(sizeof(WCHAR) * (wcslen(baseUrl) + 32));
    file = malloc(sizeof(WCHAR) * (wcslen(destDir) + 32));
    if (!url || !file)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }

    wcscpy(url, baseUrl);
    wcscat(url, L"/manifest.json");
    wcscpy(file, destDir);
    wcscat(file, L"\\manifest.json");
    err = LnFetchFile(url, file);
    if (err != ERROR_SUCCESS)
        goto done;

    wcscpy(url, baseUrl);
    wcscat(url, L"/manifest.sig");
    wcscpy(file, destDir);
    wcscat(file, L"\\manifest.sig");
    err = LnFetchFile(url, file);
    if (err != ERROR_SUCCESS)
        goto done;

    wcscpy(file, destDir);
    wcscat(file, L"\\manifest.json");
    err = LnManifestLoad(file, &m);
    if (err != ERROR_SUCCESS)
        goto done;

    payloadDir = LnJoin(destDir, L"payload");
    if (!payloadDir)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    err = LnEnsureDir(payloadDir);
    if (err != ERROR_SUCCESS)
        goto done;

    for (i = 0; i < m.itemCount; i++)
    {
        char hex[65];
        WCHAR *dest, *bslash;

        if (m.items[i].isDelete || !m.items[i].path)
            continue;

        dest = LnJoin(payloadDir, m.items[i].path);
        if (!dest)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            goto done;
        }

        bslash = wcsrchr(dest, L'\\');
        if (bslash)
        {
            *bslash = 0;
            err = LnEnsureDir(dest);
            *bslash = L'\\';
            if (err != ERROR_SUCCESS)
            {
                free(dest);
                goto done;
            }
        }

        wcscpy(url, baseUrl);
        wcscat(url, L"/payload/");
        wcscat(url, m.items[i].path);

        err = LnFetchFile(url, dest);
        if (err == ERROR_SUCCESS)
            err = LnSha256FileHex(dest, hex);
        if (err == ERROR_SUCCESS && strcmp(hex, m.items[i].sha256) != 0)
            err = LN_E_HASH_MISMATCH;
        free(dest);
        if (err != ERROR_SUCCESS)
            goto done;
    }

done:
    LnManifestFree(&m);
    free(payloadDir);
    free(url);
    free(file);
    return err;
}
