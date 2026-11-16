/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests that the process image name reports the on-disk file name
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

#include <tlhelp32.h>

static
BOOL
GetFinalComponent(_In_ PCWSTR Path, _Out_ PWSTR Component, _In_ SIZE_T ComponentSize)
{
    PCWSTR LastBackslash = wcsrchr(Path, L'\\');
    HRESULT Result;

    if (!LastBackslash || !*(LastBackslash + 1))
        return FALSE;
    Result = StringCchCopyW(Component, ComponentSize, LastBackslash + 1);
    return SUCCEEDED(Result);
}

START_TEST(ProcessImageNameCase)
{
    WCHAR Image[MAX_PATH], Component[MAX_PATH];
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo = { 0 };
    HANDLE Snapshot;
    PROCESSENTRY32W Entry;
    BOOL FoundInToolHelp;

    /* The on-disk name is lowercase; ask for it with an uppercase extension */
    if (!ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\NOTEPAD.EXE",
                                   Image, ARRAYSIZE(Image)) ||
        GetFileAttributesW(Image) == INVALID_FILE_ATTRIBUTES)
    {
        skip("notepad.exe is not available: %lu\n", GetLastError());
        return;
    }

    if (!CreateProcessW(Image, Image, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcessW failed with %lu\n", GetLastError());
        return;
    }

    /* QueryFullProcessImageNameW must report the stored file name */
    {
        WCHAR Queried[MAX_PATH * 2];
        DWORD QueriedSize = ARRAYSIZE(Queried);

        ok(QueryFullProcessImageNameW(ProcessInfo.hProcess, 0, Queried, &QueriedSize),
           "QueryFullProcessImageNameW failed with %lu\n", GetLastError());
        ok(GetFinalComponent(Queried, Component, ARRAYSIZE(Component)),
           "no final component in '%ls'\n", Queried);
        ok(!lstrcmpiW(Component, L"notepad.exe"),
           "QueryFullProcessImageNameW reports '%ls', expected the on-disk notepad.exe\n", Component);
        ok(lstrcmpW(Component, L"NOTEPAD.EXE") != 0,
           "QueryFullProcessImageNameW kept the requested spelling '%ls'\n", Component);
    }

    /* ToolHelp must report the stored file name as well */
    FoundInToolHelp = FALSE;
    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Snapshot != INVALID_HANDLE_VALUE)
    {
        Entry.dwSize = sizeof(Entry);
        if (Process32FirstW(Snapshot, &Entry))
        {
            do
            {
                if (Entry.th32ProcessID == ProcessInfo.dwProcessId)
                {
                    FoundInToolHelp = TRUE;
                    ok(!lstrcmpiW(Entry.szExeFile, L"notepad.exe"),
                       "ToolHelp reports '%ls', expected the on-disk notepad.exe\n", Entry.szExeFile);
                    ok(lstrcmpW(Entry.szExeFile, L"NOTEPAD.EXE") != 0,
                       "ToolHelp kept the requested spelling '%ls'\n", Entry.szExeFile);
                    break;
                }
            } while (Process32NextW(Snapshot, &Entry));
        }
        CloseHandle(Snapshot);
    }
    else
    {
        skip("CreateToolhelp32Snapshot failed with %lu\n", GetLastError());
    }
    ok(FoundInToolHelp, "the started process %lu was not in the ToolHelp snapshot\n", ProcessInfo.dwProcessId);

    TerminateProcess(ProcessInfo.hProcess, 0);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
}
