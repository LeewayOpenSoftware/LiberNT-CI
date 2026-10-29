/*
 * PROJECT:     LiberNT kernel32 API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     kernel32 exports that used to end in a stub now work or fail explicitly
 */

#include "precomp.h"

typedef LPVOID (WINAPI *PFN_VIRTUALALLOCEXNUMA)(HANDLE, LPVOID, SIZE_T, DWORD, DWORD, DWORD);
typedef LPVOID (WINAPI *PFN_MAPVIEWOFFILEEXNUMA)(HANDLE, DWORD, DWORD, DWORD, SIZE_T, LPVOID, DWORD);
typedef BOOL (WINAPI *PFN_SETSTDHANDLEEX)(DWORD, HANDLE, PHANDLE);
typedef BOOL (WINAPI *PFN_SETAFFINITYUPDATEMODE)(HANDLE, DWORD);
typedef BOOL (WINAPI *PFN_QUERYAFFINITYUPDATEMODE)(HANDLE, LPDWORD);
typedef HANDLE (WINAPI *PFN_FINDFIRSTFILENAMEW)(LPCWSTR, DWORD, LPDWORD, PWSTR);
typedef BOOL (WINAPI *PFN_GETPIPECLIENTNAMEW)(HANDLE, LPWSTR, ULONG);
typedef BOOL (WINAPI *PFN_GETWSCHANGESEX)(HANDLE, PVOID, PDWORD);

static HMODULE g_Kernel32;

static
FARPROC
Import(PCSTR Name)
{
    FARPROC Proc = GetProcAddress(g_Kernel32, Name);

    ok(Proc != NULL, "kernel32!%s is not exported\n", Name);
    return Proc;
}

static
void
Test_Numa(void)
{
    PFN_VIRTUALALLOCEXNUMA pVirtualAllocExNuma = (PFN_VIRTUALALLOCEXNUMA)Import("VirtualAllocExNuma");
    PFN_MAPVIEWOFFILEEXNUMA pMapViewOfFileExNuma = (PFN_MAPVIEWOFFILEEXNUMA)Import("MapViewOfFileExNuma");
    PVOID Memory;
    HANDLE Mapping;
    PVOID View;

    if (pVirtualAllocExNuma)
    {
        Memory = pVirtualAllocExNuma(GetCurrentProcess(), NULL, 0x1000, MEM_RESERVE | MEM_COMMIT,
                                     PAGE_READWRITE, (DWORD)-1);
        ok(Memory != NULL, "No preferred node: error %lu\n", GetLastError());
        if (Memory)
        {
            *(volatile BYTE *)Memory = 1;
            VirtualFree(Memory, 0, MEM_RELEASE);
        }

        Memory = pVirtualAllocExNuma(GetCurrentProcess(), NULL, 0x1000, MEM_RESERVE | MEM_COMMIT,
                                     PAGE_READWRITE, 0);
        ok(Memory != NULL, "Node 0: error %lu\n", GetLastError());
        if (Memory)
            VirtualFree(Memory, 0, MEM_RELEASE);

        SetLastError(0xDEADBEEF);
        Memory = pVirtualAllocExNuma(GetCurrentProcess(), NULL, 0x1000, MEM_RESERVE | MEM_COMMIT,
                                     PAGE_READWRITE, 0x7FFF);
        ok(Memory == NULL, "A node that does not exist was accepted\n");
        ok(GetLastError() == ERROR_INVALID_PARAMETER, "Invalid node: error %lu\n", GetLastError());
        if (Memory)
            VirtualFree(Memory, 0, MEM_RELEASE);

        SetLastError(0xDEADBEEF);
        Memory = pVirtualAllocExNuma(GetCurrentProcess(), NULL, 0, MEM_COMMIT, PAGE_READWRITE, (DWORD)-1);
        ok(Memory == NULL, "A zero size allocation succeeded\n");
        ok(GetLastError() == ERROR_INVALID_PARAMETER, "Zero size: error %lu\n", GetLastError());
    }

    if (pMapViewOfFileExNuma)
    {
        Mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 0x1000, NULL);
        ok(Mapping != NULL, "CreateFileMappingW failed: %lu\n", GetLastError());
        if (Mapping)
        {
            View = pMapViewOfFileExNuma(Mapping, FILE_MAP_WRITE, 0, 0, 0, NULL, (DWORD)-1);
            ok(View != NULL, "No preferred node: error %lu\n", GetLastError());
            if (View)
            {
                *(volatile BYTE *)View = 2;
                UnmapViewOfFile(View);
            }

            SetLastError(0xDEADBEEF);
            View = pMapViewOfFileExNuma(Mapping, FILE_MAP_WRITE, 0, 0, 0, NULL, 0x7FFF);
            ok(View == NULL, "A node that does not exist was accepted\n");
            ok(GetLastError() == ERROR_INVALID_PARAMETER, "Invalid node: error %lu\n", GetLastError());
            if (View)
                UnmapViewOfFile(View);

            CloseHandle(Mapping);
        }

        SetLastError(0xDEADBEEF);
        View = pMapViewOfFileExNuma(NULL, FILE_MAP_WRITE, 0, 0, 0, NULL, (DWORD)-1);
        ok(View == NULL, "A NULL mapping handle succeeded\n");
        ok(GetLastError() == ERROR_INVALID_HANDLE, "NULL mapping: error %lu\n", GetLastError());
    }
}

static
void
Test_SetStdHandleEx(void)
{
    PFN_SETSTDHANDLEEX pSetStdHandleEx = (PFN_SETSTDHANDLEEX)Import("SetStdHandleEx");
    HANDLE Original, Previous = (HANDLE)(ULONG_PTR)0xCCCCCCCC;
    BOOL Result;

    if (!pSetStdHandleEx)
        return;

    Original = GetStdHandle(STD_ERROR_HANDLE);

    Result = pSetStdHandleEx(STD_ERROR_HANDLE, Original, &Previous);
    ok(Result, "SetStdHandleEx failed: %lu\n", GetLastError());
    ok(Previous == Original, "Previous = %p, expected %p\n", Previous, Original);

    Result = pSetStdHandleEx(STD_ERROR_HANDLE, Original, NULL);
    ok(Result, "SetStdHandleEx without a previous handle failed: %lu\n", GetLastError());
    ok(GetStdHandle(STD_ERROR_HANDLE) == Original, "The handle changed\n");

    Previous = (HANDLE)(ULONG_PTR)0xCCCCCCCC;
    SetLastError(0xDEADBEEF);
    Result = pSetStdHandleEx(0x1234, Original, &Previous);
    ok(!Result, "An invalid standard handle id succeeded\n");
    ok(GetLastError() == ERROR_INVALID_HANDLE, "Invalid id: error %lu\n", GetLastError());
    ok(Previous == (HANDLE)(ULONG_PTR)0xCCCCCCCC, "Previous was written on failure\n");
}

static
void
Test_ExplicitErrors(void)
{
    PFN_SETAFFINITYUPDATEMODE pSet = (PFN_SETAFFINITYUPDATEMODE)Import("SetProcessAffinityUpdateMode");
    PFN_QUERYAFFINITYUPDATEMODE pQuery = (PFN_QUERYAFFINITYUPDATEMODE)Import("QueryProcessAffinityUpdateMode");
    PFN_FINDFIRSTFILENAMEW pFindFirstFileNameW = (PFN_FINDFIRSTFILENAMEW)Import("FindFirstFileNameW");
    PFN_GETPIPECLIENTNAMEW pGetPipeClientName = (PFN_GETPIPECLIENTNAMEW)Import("GetNamedPipeClientComputerNameW");
    PFN_GETWSCHANGESEX pGetWsChangesEx = (PFN_GETWSCHANGESEX)Import("K32GetWsChangesEx");
    DWORD Flags = 0xCCCCCCCC, Length = 0;
    WCHAR Buffer[8];
    HANDLE Find;
    BOOL Result;

    if (pSet)
    {
        SetLastError(0xDEADBEEF);
        Result = pSet(GetCurrentProcess(), 0);
        ok(!Result, "SetProcessAffinityUpdateMode succeeded\n");
        ok(GetLastError() == ERROR_CALL_NOT_IMPLEMENTED, "error %lu\n", GetLastError());
    }

    if (pQuery)
    {
        SetLastError(0xDEADBEEF);
        Result = pQuery(GetCurrentProcess(), &Flags);
        ok(!Result, "QueryProcessAffinityUpdateMode succeeded\n");
        ok(GetLastError() == ERROR_CALL_NOT_IMPLEMENTED, "error %lu\n", GetLastError());
    }

    if (pFindFirstFileNameW)
    {
        SetLastError(0xDEADBEEF);
        Find = pFindFirstFileNameW(L"C:\\", 0, &Length, Buffer);
        ok(Find == INVALID_HANDLE_VALUE, "FindFirstFileNameW returned %p\n", Find);
        ok(GetLastError() == ERROR_CALL_NOT_IMPLEMENTED, "error %lu\n", GetLastError());
    }

    if (pGetPipeClientName)
    {
        SetLastError(0xDEADBEEF);
        Result = pGetPipeClientName(INVALID_HANDLE_VALUE, Buffer, sizeof(Buffer));
        ok(!Result, "GetNamedPipeClientComputerNameW succeeded\n");
        ok(GetLastError() == ERROR_CALL_NOT_IMPLEMENTED, "error %lu\n", GetLastError());
    }

    if (pGetWsChangesEx)
    {
        Length = 0;
        SetLastError(0xDEADBEEF);
        Result = pGetWsChangesEx(GetCurrentProcess(), Buffer, &Length);
        ok(!Result, "K32GetWsChangesEx succeeded\n");
        ok(GetLastError() == ERROR_CALL_NOT_IMPLEMENTED, "error %lu\n", GetLastError());
    }
}

START_TEST(CompatLinks)
{
    g_Kernel32 = GetModuleHandleW(L"kernel32.dll");

    Test_Numa();
    Test_SetStdHandleEx();
    Test_ExplicitErrors();
}
