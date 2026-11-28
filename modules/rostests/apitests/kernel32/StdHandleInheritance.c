/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests standard handle inheritance by child processes
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

#define CHILD_OUTPUT "StdHandleInheritance child output\r\n"

static
int
RunChild(VOID)
{
    HANDLE Output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD Written = 0;

    if (Output == NULL || Output == INVALID_HANDLE_VALUE)
        return 2;

    if (!WriteFile(Output, CHILD_OUTPUT, sizeof(CHILD_OUTPUT) - 1, &Written, NULL) ||
        Written != sizeof(CHILD_OUTPUT) - 1)
    {
        return 3;
    }

    return 0;
}

static
VOID
TestInheritance(
    _In_ PCWSTR Application,
    _In_ BOOL UseHandleList)
{
    SECURITY_ATTRIBUTES Security = { sizeof(Security), NULL, TRUE };
    LPPROC_THREAD_ATTRIBUTE_LIST Attributes = NULL;
    PROCESS_INFORMATION ProcessInfo;
    STARTUPINFOEXW Startup;
    WCHAR CommandLine[MAX_PATH + 64];
    HANDLE Read = NULL, Write = NULL, Previous;
    CHAR Buffer[128];
    DWORD Received = 0, ExitCode = MAXULONG;
    SIZE_T Size = 0;
    BOOL Success;

    Success = CreatePipe(&Read, &Write, &Security, 0);
    ok(Success, "[%d] CreatePipe failed with %lu\n", UseHandleList, GetLastError());
    if (!Success)
        return;

    SetHandleInformation(Read, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&Startup, sizeof(Startup));
    Startup.StartupInfo.cb = UseHandleList ? sizeof(Startup) : sizeof(Startup.StartupInfo);

    if (UseHandleList)
    {
        InitializeProcThreadAttributeList(NULL, 1, 0, &Size);
        Attributes = HeapAlloc(GetProcessHeap(), 0, Size);
        Success = Attributes && InitializeProcThreadAttributeList(Attributes, 1, 0, &Size);
        ok(Success, "InitializeProcThreadAttributeList failed with %lu\n", GetLastError());
        if (Success)
        {
            Success = UpdateProcThreadAttribute(Attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &Write,
                                                sizeof(Write), NULL, NULL);
            ok(Success, "UpdateProcThreadAttribute failed with %lu\n", GetLastError());
        }
        if (!Success)
            goto Cleanup;

        Startup.lpAttributeList = Attributes;
    }

    StringCchPrintfW(CommandLine, ARRAYSIZE(CommandLine), L"\"%s\" StdHandleInheritance child", Application);

    Previous = GetStdHandle(STD_OUTPUT_HANDLE);
    SetStdHandle(STD_OUTPUT_HANDLE, Write);
    Success = CreateProcessW(Application, CommandLine, NULL, NULL, TRUE,
                             UseHandleList ? EXTENDED_STARTUPINFO_PRESENT : 0, NULL, NULL,
                             &Startup.StartupInfo, &ProcessInfo);
    SetStdHandle(STD_OUTPUT_HANDLE, Previous);
    ok(Success, "[%d] CreateProcessW failed with %lu\n", UseHandleList, GetLastError());

    CloseHandle(Write);
    Write = NULL;
    if (!Success)
        goto Cleanup;

    ok_eq_ulong(WaitForSingleObject(ProcessInfo.hProcess, 60000), WAIT_OBJECT_0);
    ok(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode), "GetExitCodeProcess failed with %lu\n", GetLastError());
    ok(ExitCode == 0, "[%d] child exit code %lu\n", UseHandleList, ExitCode);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);

    Success = ReadFile(Read, Buffer, sizeof(Buffer) - 1, &Received, NULL);
    ok(Success, "[%d] ReadFile failed with %lu\n", UseHandleList, GetLastError());
    Buffer[Success ? Received : 0] = ANSI_NULL;
    ok(!strcmp(Buffer, CHILD_OUTPUT), "[%d] child wrote '%s'\n", UseHandleList, Buffer);

Cleanup:
    if (Attributes)
    {
        DeleteProcThreadAttributeList(Attributes);
        HeapFree(GetProcessHeap(), 0, Attributes);
    }
    if (Write)
        CloseHandle(Write);
    CloseHandle(Read);
}

static
VOID
TestUnlistedStdHandle(
    _In_ PCWSTR Application)
{
    SECURITY_ATTRIBUTES Security = { sizeof(Security), NULL, TRUE };
    LPPROC_THREAD_ATTRIBUTE_LIST Attributes;
    PROCESS_INFORMATION ProcessInfo;
    STARTUPINFOEXW Startup;
    WCHAR CommandLine[MAX_PATH + 64];
    HANDLE Read, Write, Listed;
    CHAR Buffer[128];
    DWORD Received = 0, ExitCode = MAXULONG;
    SIZE_T Size = 0;
    BOOL Success;

    if (!CreatePipe(&Read, &Write, &Security, 0))
    {
        ok(0, "CreatePipe failed with %lu\n", GetLastError());
        return;
    }
    SetHandleInformation(Read, HANDLE_FLAG_INHERIT, 0);
    Listed = CreateEventW(&Security, TRUE, FALSE, NULL);

    InitializeProcThreadAttributeList(NULL, 1, 0, &Size);
    Attributes = HeapAlloc(GetProcessHeap(), 0, Size);
    Success = Listed && Attributes && InitializeProcThreadAttributeList(Attributes, 1, 0, &Size);
    ok(Success, "InitializeProcThreadAttributeList failed with %lu\n", GetLastError());
    if (Success)
    {
        Success = UpdateProcThreadAttribute(Attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &Listed,
                                            sizeof(Listed), NULL, NULL);
        ok(Success, "UpdateProcThreadAttribute failed with %lu\n", GetLastError());
    }
    if (Success)
    {
        ZeroMemory(&Startup, sizeof(Startup));
        Startup.StartupInfo.cb = sizeof(Startup);
        Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        Startup.StartupInfo.hStdOutput = Write;
        Startup.lpAttributeList = Attributes;
        StringCchPrintfW(CommandLine, ARRAYSIZE(CommandLine), L"\"%s\" StdHandleInheritance child", Application);
        Success = CreateProcessW(Application, CommandLine, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL,
                                 NULL, &Startup.StartupInfo, &ProcessInfo);
        ok(Success, "CreateProcessW failed with %lu\n", GetLastError());
    }
    CloseHandle(Write);

    if (Success)
    {
        ok_eq_ulong(WaitForSingleObject(ProcessInfo.hProcess, 5000), WAIT_OBJECT_0);
        GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
        ok_eq_ulong(ExitCode, 3UL);
        CloseHandle(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hProcess);

        Success = ReadFile(Read, Buffer, sizeof(Buffer) - 1, &Received, NULL);
        ok(!Success && GetLastError() == ERROR_BROKEN_PIPE, "ReadFile returned %d with %lu, %lu bytes\n", Success,
           GetLastError(), Received);
    }

    if (Attributes)
    {
        DeleteProcThreadAttributeList(Attributes);
        HeapFree(GetProcessHeap(), 0, Attributes);
    }
    if (Listed)
        CloseHandle(Listed);
    CloseHandle(Read);
}

static
BOOL
StartWithHandleList(
    _In_ PCWSTR Application,
    _In_opt_ HANDLE Parent,
    _In_ HANDLE Listed,
    _In_ BOOL NullEntry,
    _In_ BOOL ListFirst,
    _Out_ PHANDLE Process)
{
    BYTE Buffer[256];
    LPPROC_THREAD_ATTRIBUTE_LIST Attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)Buffer;
    PROCESS_INFORMATION ProcessInfo;
    STARTUPINFOEXW Startup;
    WCHAR CommandLine[MAX_PATH + 64];
    HANDLE Entries[2];
    SIZE_T Size = sizeof(Buffer);
    DWORD Error, Step;
    BOOL Success;

    *Process = NULL;
    Entries[0] = Listed;
    Entries[1] = NULL;
    ZeroMemory(&Startup, sizeof(Startup));
    Startup.StartupInfo.cb = sizeof(Startup);
    if (!InitializeProcThreadAttributeList(Attributes, 2, 0, &Size))
        return FALSE;

    for (Step = 0; Step < 2; Step++)
    {
        if ((Step == 0) == !ListFirst)
        {
            if (Parent)
                UpdateProcThreadAttribute(Attributes, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &Parent, sizeof(Parent), NULL, NULL);
        }
        else
        {
            UpdateProcThreadAttribute(Attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, Entries,
                                      (NullEntry ? 2 : 1) * sizeof(HANDLE), NULL, NULL);
        }
    }

    Startup.lpAttributeList = Attributes;
    StringCchPrintfW(CommandLine, ARRAYSIZE(CommandLine), L"\"%s\" StdHandleInheritance signal %Iu", Application, (ULONG_PTR)Listed);
    Success = CreateProcessW(Application, CommandLine, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                             &Startup.StartupInfo, &ProcessInfo);
    Error = GetLastError();
    DeleteProcThreadAttributeList(Attributes);
    if (Success)
    {
        CloseHandle(ProcessInfo.hThread);
        *Process = ProcessInfo.hProcess;
    }

    SetLastError(Error);
    return Success;
}

static
VOID
TestHandleListOfParent(
    _In_ PCWSTR Application)
{
    PROCESS_INFORMATION Helper;
    STARTUPINFOW Startup;
    WCHAR CommandLine[MAX_PATH + 64];
    HANDLE Event, Mine, Plain, InParent = NULL, Process;
    DWORD Flags, Attempt, Order;
    BOOL Success;

    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(Event != NULL, "CreateEventW failed with %lu\n", GetLastError());
    if (!Event)
        return;

    Success = DuplicateHandle(GetCurrentProcess(), Event, GetCurrentProcess(), &Mine, 0, TRUE, DUPLICATE_SAME_ACCESS);
    ok(Success, "DuplicateHandle failed with %lu\n", GetLastError());
    Success = DuplicateHandle(GetCurrentProcess(), Event, GetCurrentProcess(), &Plain, 0, FALSE, DUPLICATE_SAME_ACCESS);
    ok(Success, "DuplicateHandle failed with %lu\n", GetLastError());

    Success = StartWithHandleList(Application, NULL, Mine, FALSE, FALSE, &Process);
    ok(Success, "Own inheritable handle: CreateProcessW failed with %lu\n", GetLastError());
    if (Success)
    {
        ok_eq_ulong(WaitForSingleObject(Process, 60000), WAIT_OBJECT_0);
        ok_eq_ulong(WaitForSingleObject(Event, 0), WAIT_OBJECT_0);
        CloseHandle(Process);
    }

    Success = StartWithHandleList(Application, NULL, Plain, FALSE, FALSE, &Process);
    ok(!Success && GetLastError() == ERROR_INVALID_PARAMETER, "Own handle that is not inheritable: %d with %lu\n", Success, GetLastError());
    if (Success)
        CloseHandle(Process);

    Success = StartWithHandleList(Application, NULL, (HANDLE)(ULONG_PTR)0x7FFF0, FALSE, FALSE, &Process);
    ok(!Success && GetLastError() == ERROR_INVALID_PARAMETER, "Value that is no handle: %d with %lu\n", Success, GetLastError());
    if (Success)
        CloseHandle(Process);

    /* A NULL entry is no error: the process starts (and inherits nothing, which is the kernel's part) */
    Success = StartWithHandleList(Application, NULL, Mine, TRUE, FALSE, &Process);
    ok(Success, "List with a NULL entry: CreateProcessW failed with %lu\n", GetLastError());
    if (Success)
    {
        ok_eq_ulong(WaitForSingleObject(Process, 60000), WAIT_OBJECT_0);
        CloseHandle(Process);
    }

    ZeroMemory(&Startup, sizeof(Startup));
    Startup.cb = sizeof(Startup);
    StringCchPrintfW(CommandLine, ARRAYSIZE(CommandLine), L"\"%s\" StdHandleInheritance wait", Application);
    Success = CreateProcessW(Application, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Helper);
    ok(Success, "CreateProcessW for the parent failed with %lu\n", GetLastError());
    if (!Success)
        goto Cleanup;

    /* A value that this process could not pass as its own inheritable handle */
    for (Attempt = 0; Attempt < 16; Attempt++)
    {
        Success = DuplicateHandle(GetCurrentProcess(), Event, Helper.hProcess, &InParent, 0, TRUE, DUPLICATE_SAME_ACCESS);
        if (!Success || !GetHandleInformation(InParent, &Flags) || !(Flags & HANDLE_FLAG_INHERIT))
            break;
    }

    ok(Success, "DuplicateHandle into the parent failed with %lu\n", GetLastError());
    if (Success)
    {
        for (Order = 0; Order < 2; Order++)
        {
            ResetEvent(Event);
            Success = StartWithHandleList(Application, Helper.hProcess, InParent, FALSE, Order, &Process);
            ok(Success, "[%lu] Handle of the parent: CreateProcessW failed with %lu\n", Order, GetLastError());
            if (!Success)
                continue;

            ok_eq_ulong(WaitForSingleObject(Process, 60000), WAIT_OBJECT_0);
            ok(WaitForSingleObject(Event, 0) == WAIT_OBJECT_0, "[%lu] The process did not get the handle of its parent\n", Order);
            CloseHandle(Process);
        }
    }

    TerminateProcess(Helper.hProcess, 0);
    CloseHandle(Helper.hThread);
    CloseHandle(Helper.hProcess);

Cleanup:
    CloseHandle(Plain);
    CloseHandle(Mine);
    CloseHandle(Event);
}

START_TEST(StdHandleInheritance)
{
    WCHAR Application[MAX_PATH];
    char **Arguments;
    int ArgumentCount;

    ArgumentCount = winetest_get_mainargs(&Arguments);
    if (ArgumentCount >= 3 && !strcmp(Arguments[2], "child"))
        TerminateProcess(GetCurrentProcess(), RunChild());
    if (ArgumentCount >= 3 && !strcmp(Arguments[2], "wait"))
    {
        Sleep(60000);
        TerminateProcess(GetCurrentProcess(), 0);
    }
    if (ArgumentCount >= 4 && !strcmp(Arguments[2], "signal"))
        TerminateProcess(GetCurrentProcess(), SetEvent((HANDLE)(ULONG_PTR)_strtoui64(Arguments[3], NULL, 10)) ? 0 : 4);

    if (!GetModuleFileNameW(NULL, Application, ARRAYSIZE(Application)))
    {
        ok(0, "GetModuleFileNameW failed with %lu\n", GetLastError());
        return;
    }

    TestInheritance(Application, FALSE);
    TestInheritance(Application, TRUE);
    TestUnlistedStdHandle(Application);
    TestHandleListOfParent(Application);
}
