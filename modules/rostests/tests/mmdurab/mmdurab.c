/*
 * PROJECT:     LiberNT tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Durability stress for mapped-file writes racing FlushViewOfFile across processors
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * mmdurab run    <file> [seconds] [threads]   write, flush, then check the disk copy with a noncached read
 * mmdurab write  <file> [seconds] [threads]   write and flush, store the expected image in <file>.exp
 * mmdurab verify <file>                       after a reboot: compare <file> with <file>.exp
 *
 * One writer thread per processor stores into the shared view while another thread keeps calling
 * FlushViewOfFile, so every flush cleans dirty PTEs and shoots down the writers' TLB entries. A store
 * that lands through a stale writable translation after the page was taken as clean is never written
 * back; the final image on disk then differs from the view. Exit code 0 = pass, 1 = lost data, 2 = error.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define DURAB_PAGES     4096
#define DURAB_PAGE      4096
#define DURAB_SIZE      ((SIZE_T)DURAB_PAGES * DURAB_PAGE)
#define DURAB_MAXTHREAD 32

static volatile LONG g_Stop;
static PUCHAR g_View;
static ULONG g_Threads;
static volatile LONG64 g_Stores;
static volatile LONG64 g_Flushes;

typedef struct _WRITER
{
    ULONG Index;
    HANDLE Thread;
} WRITER;

static DWORD WINAPI
WriterThread(LPVOID Context)
{
    WRITER *Writer = Context;
    ULONG64 Generation = 0;
    ULONG Page;
    LONG64 Stores = 0;

    while (!g_Stop)
    {
        Generation++;
        /* Each writer owns every g_Threads-th page; the whole page changes so any lost store shows */
        for (Page = Writer->Index; Page < DURAB_PAGES && !g_Stop; Page += g_Threads)
        {
            volatile ULONG64 *Words = (volatile ULONG64 *)(g_View + (SIZE_T)Page * DURAB_PAGE);
            ULONG i;

            for (i = 0; i < DURAB_PAGE / sizeof(ULONG64); i += 64)
                Words[i] = (Generation << 24) ^ ((ULONG64)Page << 8) ^ i;
            Stores++;
        }
    }
    InterlockedExchangeAdd64(&g_Stores, Stores);
    return 0;
}

static DWORD WINAPI
FlusherThread(LPVOID Context)
{
    ULONG Turn = 0;

    UNREFERENCED_PARAMETER(Context);
    while (!g_Stop)
    {
        /* Alternate the whole view and a sliding window so batches of every size are cleaned */
        if (Turn & 1)
            FlushViewOfFile(g_View + (SIZE_T)((Turn * 97) % (DURAB_PAGES - 256)) * DURAB_PAGE, 256 * DURAB_PAGE);
        else
            FlushViewOfFile(g_View, 0);
        Turn++;
        InterlockedIncrement64(&g_Flushes);
    }
    return 0;
}

static int
ReadWhole(const WCHAR *Path, BOOL NoBuffering, PUCHAR Buffer, SIZE_T Size)
{
    HANDLE File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              NoBuffering ? FILE_FLAG_NO_BUFFERING : 0, NULL);
    SIZE_T Done = 0;
    DWORD Got;

    if (File == INVALID_HANDLE_VALUE)
        return 0;
    while (Done < Size)
    {
        if (!ReadFile(File, Buffer + Done, (DWORD)min(Size - Done, 1024 * 1024), &Got, NULL) || Got == 0)
            break;
        Done += Got;
    }
    CloseHandle(File);
    return Done == Size;
}

static int
WriteWhole(const WCHAR *Path, const UCHAR *Buffer, SIZE_T Size)
{
    HANDLE File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, NULL);
    SIZE_T Done = 0;
    DWORD Put;
    BOOL Ok;

    if (File == INVALID_HANDLE_VALUE)
        return 0;
    while (Done < Size)
    {
        if (!WriteFile(File, Buffer + Done, (DWORD)min(Size - Done, 1024 * 1024), &Put, NULL) || Put == 0)
            break;
        Done += Put;
    }
    Ok = FlushFileBuffers(File);
    CloseHandle(File);
    return Done == Size && Ok;
}

static int
Compare(const UCHAR *Expected, const UCHAR *Actual)
{
    ULONG Page, Lost = 0;

    for (Page = 0; Page < DURAB_PAGES; Page++)
    {
        if (memcmp(Expected + (SIZE_T)Page * DURAB_PAGE, Actual + (SIZE_T)Page * DURAB_PAGE, DURAB_PAGE))
        {
            if (Lost < 8)
                printf("mmdurab: page %lu differs on disk\n", Page);
            Lost++;
        }
    }
    printf("mmdurab: %lu of %u pages lost\n", Lost, DURAB_PAGES);
    return Lost ? 1 : 0;
}

static int
Stress(const WCHAR *Path, ULONG Seconds, ULONG Threads, BOOL CheckNow)
{
    WCHAR ExpPath[MAX_PATH];
    WRITER Writers[DURAB_MAXTHREAD];
    HANDLE File, Mapping, Flusher;
    SYSTEM_INFO Info;
    PUCHAR Expected, Actual;
    ULONG i;
    int Result;

    GetSystemInfo(&Info);
    if (!Threads)
        Threads = Info.dwNumberOfProcessors;
    if (Threads < 1)
        Threads = 1;
    if (Threads > DURAB_MAXTHREAD)
        Threads = DURAB_MAXTHREAD;
    g_Threads = Threads;
    printf("mmdurab: %lu processors, %lu writers, %lu s, %u pages\n",
           Info.dwNumberOfProcessors, Threads, Seconds, DURAB_PAGES);
    if (Info.dwNumberOfProcessors < 2)
        printf("mmdurab: WARNING: one processor, no cross-processor shootdowns are exercised\n");

    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return printf("mmdurab: create failed %lu\n", GetLastError()), 2;
    Mapping = CreateFileMappingW(File, NULL, PAGE_READWRITE, 0, (DWORD)DURAB_SIZE, NULL);
    g_View = Mapping ? MapViewOfFile(Mapping, FILE_MAP_WRITE, 0, 0, DURAB_SIZE) : NULL;
    if (!g_View)
        return printf("mmdurab: mapping failed %lu\n", GetLastError()), 2;

    for (i = 0; i < Threads; i++)
    {
        Writers[i].Index = i;
        Writers[i].Thread = CreateThread(NULL, 0, WriterThread, &Writers[i], CREATE_SUSPENDED, NULL);
        if (!Writers[i].Thread)
            return printf("mmdurab: thread failed %lu\n", GetLastError()), 2;
        if (Info.dwNumberOfProcessors > 1)
            SetThreadAffinityMask(Writers[i].Thread, (DWORD_PTR)1 << (i % Info.dwNumberOfProcessors));
    }
    Flusher = CreateThread(NULL, 0, FlusherThread, NULL, 0, NULL);
    for (i = 0; i < Threads; i++)
        ResumeThread(Writers[i].Thread);

    Sleep(Seconds * 1000);
    InterlockedExchange(&g_Stop, 1);
    for (i = 0; i < Threads; i++)
    {
        WaitForSingleObject(Writers[i].Thread, INFINITE);
        CloseHandle(Writers[i].Thread);
    }
    WaitForSingleObject(Flusher, INFINITE);
    CloseHandle(Flusher);
    printf("mmdurab: %I64d page stores, %I64d flushes\n", g_Stores, g_Flushes);

    /* Nothing writes any more: one last flush must bring the disk copy to exactly the view */
    Expected = malloc(DURAB_SIZE);
    Actual = VirtualAlloc(NULL, DURAB_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (!Expected || !Actual)
        return printf("mmdurab: out of memory\n"), 2;
    if (!FlushViewOfFile(g_View, 0) || !FlushFileBuffers(File))
        return printf("mmdurab: final flush failed %lu\n", GetLastError()), 2;
    memcpy(Expected, g_View, DURAB_SIZE);
    UnmapViewOfFile(g_View);
    CloseHandle(Mapping);
    CloseHandle(File);

    if (CheckNow)
    {
        /* Noncached read: what is on disk, not what the cache still holds */
        if (!ReadWhole(Path, TRUE, Actual, DURAB_SIZE))
            return printf("mmdurab: noncached read failed %lu\n", GetLastError()), 2;
        Result = Compare(Expected, Actual);
    }
    else
    {
        _snwprintf(ExpPath, MAX_PATH, L"%s.exp", Path);
        ExpPath[MAX_PATH - 1] = 0;
        if (!WriteWhole(ExpPath, Expected, DURAB_SIZE))
            return printf("mmdurab: writing the expected image failed %lu\n", GetLastError()), 2;
        printf("mmdurab: expected image stored; reboot, then run: mmdurab verify <file>\n");
        Result = 0;
    }
    free(Expected);
    VirtualFree(Actual, 0, MEM_RELEASE);
    return Result;
}

static int
Verify(const WCHAR *Path)
{
    WCHAR ExpPath[MAX_PATH];
    PUCHAR Expected = malloc(DURAB_SIZE), Actual = malloc(DURAB_SIZE);

    if (!Expected || !Actual)
        return printf("mmdurab: out of memory\n"), 2;
    _snwprintf(ExpPath, MAX_PATH, L"%s.exp", Path);
    ExpPath[MAX_PATH - 1] = 0;
    if (!ReadWhole(ExpPath, FALSE, Expected, DURAB_SIZE) || !ReadWhole(Path, FALSE, Actual, DURAB_SIZE))
        return printf("mmdurab: reading the images failed %lu\n", GetLastError()), 2;
    return Compare(Expected, Actual);
}

int
wmain(int argc, WCHAR **argv)
{
    ULONG Seconds = argc > 3 ? wcstoul(argv[3], NULL, 10) : 60;
    ULONG Threads = argc > 4 ? wcstoul(argv[4], NULL, 10) : 0;
    int Result;

    if (argc >= 3 && !_wcsicmp(argv[1], L"run"))
        Result = Stress(argv[2], Seconds, Threads, TRUE);
    else if (argc >= 3 && !_wcsicmp(argv[1], L"write"))
        Result = Stress(argv[2], Seconds, Threads, FALSE);
    else if (argc >= 3 && !_wcsicmp(argv[1], L"verify"))
        Result = Verify(argv[2]);
    else
    {
        printf("usage: mmdurab run|write <file> [seconds] [threads]\n       mmdurab verify <file>\n");
        return 2;
    }
    printf("mmdurab: %s\n", Result == 0 ? "PASS" : Result == 1 ? "FAIL (lost writes)" : "ERROR");
    return Result;
}
