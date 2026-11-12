/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Cache coherency of a FILE_FLAG_NO_BUFFERING write with a cached
 *              handle on the same file (T-A1 FAT, T-A5 NTFS)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * A cached read warms the cache, a second handle writes the same range with
 * FILE_FLAG_NO_BUFFERING, and a cached read must then return the new bytes.
 * If the non-cached write only flushes the cache and does not purge it, the
 * second read returns the old bytes.
 *
 * Runs on every directory listed in %TESTVOL% (separated by ';'), such as a
 * FAT and an NTFS scratch volume; skipped when it is not set.
 */

#include "precomp.h"

#define PROBE_SECTOR 4096u          /* at least any volume's sector size */
#define PROBE_REGION (PROBE_SECTOR * 4u)

static void
Probe(PCSTR Path, ULONG Offset, ULONG Length, UCHAR Fill, UCHAR Fill2)
{
    HANDLE Cached, NoBuffering;
    PUCHAR Buffer;
    DWORD Done;
    OVERLAPPED Overlapped;
    BOOL Ok;

    Cached = CreateFileA(Path, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(Cached != INVALID_HANDLE_VALUE, "%s: create failed (%lu)\n", Path, GetLastError());
    if (Cached == INVALID_HANDLE_VALUE)
        return;

    Buffer = VirtualAlloc(NULL, PROBE_REGION, MEM_COMMIT, PAGE_READWRITE);
    ok(Buffer != NULL, "VirtualAlloc failed\n");
    if (Buffer == NULL)
    {
        CloseHandle(Cached);
        return;
    }

    /* Initial content through the cache, then warm the cache with a read */
    memset(Buffer, Fill, Length);
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.Offset = Offset;
    Ok = WriteFile(Cached, Buffer, Length, &Done, &Overlapped);
    ok(Ok && Done == Length, "%s: seed write failed (%lu)\n", Path, GetLastError());
    FlushFileBuffers(Cached);

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.Offset = Offset;
    Ok = ReadFile(Cached, Buffer, Length, &Done, &Overlapped);
    ok(Ok && Done == Length && Buffer[0] == Fill, "%s: warm read failed (%lu)\n", Path, GetLastError());

    /* Overwrite the same range on a non-cached handle */
    NoBuffering = CreateFileA(Path, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
    ok(NoBuffering != INVALID_HANDLE_VALUE, "%s: NO_BUFFERING open failed (%lu)\n", Path, GetLastError());
    if (NoBuffering != INVALID_HANDLE_VALUE)
    {
        memset(Buffer, Fill2, Length);
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.Offset = Offset;
        Ok = WriteFile(NoBuffering, Buffer, Length, &Done, &Overlapped);
        ok(Ok && Done == Length, "%s: non-cached write failed (%lu)\n", Path, GetLastError());
        CloseHandle(NoBuffering);

        /* The cached handle must see the new bytes, not the warmed cache */
        memset(Buffer, 0, Length);
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.Offset = Offset;
        Ok = ReadFile(Cached, Buffer, Length, &Done, &Overlapped);
        ok(Ok && Done == Length, "%s: read after write failed (%lu)\n", Path, GetLastError());
        ok(Buffer[0] == Fill2 && Buffer[Length - 1] == Fill2,
           "%s: offset %lu length %lu: stale cache after a non-cached write (0x%02x, 0x%02x)\n",
           Path, Offset, Length, Buffer[0], Buffer[Length - 1]);
    }

    CloseHandle(Cached);
    VirtualFree(Buffer, 0, MEM_RELEASE);
}

static void
ProbeDirectory(PCSTR Directory)
{
    CHAR Path[MAX_PATH], Root[4], FileSystem[MAX_PATH] = "?";

    if (Directory[0] != '\0' && Directory[1] == ':')
    {
        Root[0] = Directory[0];
        Root[1] = ':';
        Root[2] = '\\';
        Root[3] = '\0';
        GetVolumeInformationA(Root, NULL, 0, NULL, NULL, NULL, FileSystem, sizeof(FileSystem));
    }
    trace("NoBufferingCoherency on %s (%s)\n", Directory, FileSystem);

    StringCbPrintfA(Path, sizeof(Path), "%s\\nbcoh.tmp", Directory);
    Probe(Path, 0, PROBE_SECTOR, 0xAA, 0x55);                   /* one aligned page */
    Probe(Path, PROBE_SECTOR, PROBE_SECTOR, 0x11, 0xEE);        /* the second page */
    Probe(Path, 0, PROBE_SECTOR * 2, 0x22, 0xDD);               /* across two pages */
    DeleteFileA(Path);
}

START_TEST(NoBufferingCoherency)
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
