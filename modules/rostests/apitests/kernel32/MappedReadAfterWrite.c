/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A page-in of a file just written returns the written bytes
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Small writes can still be held in the file system's own block cache when
 * a mapped view faults the same bytes in. The page-in runs at APC_LEVEL and
 * must return the held bytes without writing them back from there.
 *
 * Runs on every directory listed in %TESTVOL% (separated by ';'); skipped
 * when it is not set.
 */

#include "precomp.h"

static void
Probe(PCSTR Path, ULONG Size, BOOL NoBuffering, UCHAR Fill)
{
    HANDLE File, Section;
    PUCHAR Buffer, View;
    DWORD Done, Flags;
    ULONG Index, Bad = 0;
    BOOL Ok;

    Flags = NoBuffering ? (FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH) : 0;
    File = CreateFileA(Path, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | Flags, NULL);
    ok(File != INVALID_HANDLE_VALUE, "%s: create failed (%lu)\n", Path, GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;

    Buffer = VirtualAlloc(NULL, Size, MEM_COMMIT, PAGE_READWRITE);
    ok(Buffer != NULL, "VirtualAlloc failed\n");
    if (Buffer == NULL)
    {
        CloseHandle(File);
        return;
    }
    for (Index = 0; Index < Size; Index++)
        Buffer[Index] = (UCHAR)(Fill + Index * 7);
    Ok = WriteFile(File, Buffer, Size, &Done, NULL);
    ok(Ok && Done == Size, "%s: write of %lu failed (%lu)\n", Path, Size, GetLastError());
    CloseHandle(File);

    /* A fresh handle and section: every page of the view is a page-in */
    File = CreateFileA(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "%s: reopen failed (%lu)\n", Path, GetLastError());
    if (File != INVALID_HANDLE_VALUE)
    {
        Section = CreateFileMappingA(File, NULL, PAGE_READONLY, 0, 0, NULL);
        ok(Section != NULL, "%s: CreateFileMapping failed (%lu)\n", Path, GetLastError());
        if (Section != NULL)
        {
            View = MapViewOfFile(Section, FILE_MAP_READ, 0, 0, 0);
            ok(View != NULL, "%s: MapViewOfFile failed (%lu)\n", Path, GetLastError());
            if (View != NULL)
            {
                for (Index = 0; Index < Size; Index++)
                {
                    if (View[Index] != Buffer[Index] && Bad++ == 0)
                    {
                        ok(0, "%s: size %lu nobuf %d: byte %lu is 0x%02x, wrote 0x%02x\n",
                           Path, Size, NoBuffering, Index, View[Index], Buffer[Index]);
                    }
                }
                ok(Bad == 0, "%s: size %lu nobuf %d: %lu bytes differ in the view\n",
                   Path, Size, NoBuffering, Bad);
                UnmapViewOfFile(View);
            }
            CloseHandle(Section);
        }
        CloseHandle(File);
    }
    DeleteFileA(Path);
    VirtualFree(Buffer, 0, MEM_RELEASE);
}

static void
ProbeDirectory(PCSTR Directory)
{
    CHAR Path[MAX_PATH];

    trace("MappedReadAfterWrite on %s\n", Directory);
    StringCbPrintfA(Path, sizeof(Path), "%s\\mapraw.tmp", Directory);
    Probe(Path, 2048, FALSE, 0x31);         /* small, past a resident record */
    Probe(Path, 4096, FALSE, 0x42);         /* one page */
    Probe(Path, 4096 * 3 + 512, FALSE, 0x53); /* a few pages, partial tail */
    Probe(Path, 4096, TRUE, 0x64);          /* non-cached write, then page-in */
    Probe(Path, 4096 * 4, TRUE, 0x75);
}

START_TEST(MappedReadAfterWrite)
{
    CHAR List[1024];
    PSTR Directory, Next;
    DWORD Length;

    Length = GetEnvironmentVariableA("TESTVOL", List, sizeof(List));
    if (Length == 0 || Length >= sizeof(List))
    {
        skip("TESTVOL is not set: no scratch volume to test on\n");
        return;
    }

    for (Directory = List; Directory != NULL && *Directory != '\0'; Directory = Next)
    {
        Next = strchr(Directory, ';');
        if (Next != NULL)
            *Next++ = '\0';
        if (*Directory != '\0')
            ProbeDirectory(Directory);
    }
}
