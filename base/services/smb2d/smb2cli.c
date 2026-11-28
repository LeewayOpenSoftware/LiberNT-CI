/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Console test tool for the SMB2 client engine (no kernel redirector needed)
 *              smb2cli [-u user [-d domain] [-p password]] host share [dir [path] | get path]
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "smb2client.h"

static int ListEntry(const SMB2_DIRENT *e, void *ctx)
{
    (void)ctx;
    wprintf(L"%c %12I64u  %ls\n", (e->Attributes & FILE_ATTRIBUTE_DIRECTORY) ? 'd' : '-', e->EndOfFile, e->Name);
    return 1;
}

static void Usage(void)
{
    wprintf(L"usage: smb2cli [-u user [-d domain] [-p password]] host share [dir [path] | get path]\n");
}

int wmain(int argc, WCHAR **argv)
{
    const WCHAR *user = NULL, *domain = NULL, *pass = NULL, *host, *share, *cmd = L"dir", *path = L"";
    SMB2_CONN *c = NULL;
    uint32_t st, tid = 0;
    SMB2_FILE_ID fid;
    int i = 1, rc = 1;

    for (; i + 1 < argc && argv[i][0] == L'-'; i += 2)
    {
        if (!wcscmp(argv[i], L"-u"))
            user = argv[i + 1];
        else if (!wcscmp(argv[i], L"-d"))
            domain = argv[i + 1];
        else if (!wcscmp(argv[i], L"-p"))
            pass = argv[i + 1];
        else
            break;
    }
    if (argc - i < 2)
    {
        Usage();
        return 2;
    }
    host = argv[i++];
    share = argv[i++];
    if (i < argc)
        cmd = argv[i++];
    if (i < argc)
        path = argv[i++];

    st = Smb2Connect(host, 445, &c);
    if (st) { wprintf(L"connect: 0x%08lx\n", st); return 1; }
    st = Smb2Negotiate(c);
    if (st) { wprintf(L"negotiate: 0x%08lx\n", st); goto out; }
    wprintf(L"dialect 0x%04x, signing %ls\n", Smb2ConnDialect(c), Smb2ConnSigned(c) ? L"on" : L"off");
    st = Smb2SessionSetup(c, host, user, domain, pass);
    if (st) { wprintf(L"session setup: 0x%08lx\n", st); goto out; }
    wprintf(L"session established, signing %ls\n", Smb2ConnSigned(c) ? L"on" : L"off");
    st = Smb2TreeConnect(c, host, share, &tid);
    if (st) { wprintf(L"tree connect: 0x%08lx\n", st); goto out; }

    if (!wcscmp(cmd, L"dir"))
    {
        /* FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_DIRECTORY_FILE */
        st = Smb2Create(c, tid, path, 0x00100081, SMB2_FILE_OPEN, 0x00000001, &fid, NULL, NULL);
        if (st) { wprintf(L"open directory: 0x%08lx\n", st); goto tree; }
        st = Smb2QueryDirectory(c, tid, &fid, ListEntry, NULL);
        Smb2Close(c, tid, &fid);
        if (st) { wprintf(L"query directory: 0x%08lx\n", st); goto tree; }
        rc = 0;
    }
    else if (!wcscmp(cmd, L"get"))
    {
        static uint8_t buf[SMB2_MAX_IO];
        uint64_t off = 0, size = 0;
        uint32_t n;

        /* FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_NON_DIRECTORY_FILE */
        st = Smb2Create(c, tid, path, 0x00100081, SMB2_FILE_OPEN, 0x00000040, &fid, &size, NULL);
        if (st) { wprintf(L"open file: 0x%08lx\n", st); goto tree; }
        for (;;)
        {
            st = Smb2Read(c, tid, &fid, off, sizeof(buf), buf, &n);
            if (st == SMB2_STATUS_END_OF_FILE)
                break;
            if (st) { wprintf(L"read: 0x%08lx\n", st); break; }
            fwrite(buf, 1, n, stdout);
            off += n;
            if (!n)
                break;
        }
        Smb2Close(c, tid, &fid);
        fwprintf(stderr, L"%I64u of %I64u bytes\n", off, size);
        rc = (st && st != SMB2_STATUS_END_OF_FILE) ? 1 : 0;
    }
    else
    {
        Usage();
    }
tree:
    Smb2TreeDisconnect(c, tid);
    Smb2Logoff(c);
out:
    Smb2Disconnect(c);
    return rc;
}
