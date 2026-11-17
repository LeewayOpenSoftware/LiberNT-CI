/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests that the process image name reports the on-disk file name
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
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

static
VOID
ExpectStoredName(_In_ PCWSTR RequestedImage, _In_ PCWSTR StoredName)
{
    WCHAR Component[MAX_PATH];
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo = { 0 };
    WCHAR Queried[MAX_PATH * 2];
    DWORD QueriedSize;
    HANDLE Snapshot;
    PROCESSENTRY32W Entry;
    BOOL FoundInToolHelp;

    if (!CreateProcessW(RequestedImage, (LPWSTR)RequestedImage, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcessW(%ls) failed with %lu\n", RequestedImage, GetLastError());
        return;
    }

    /* QueryFullProcessImageNameW must report the stored file name */
    QueriedSize = ARRAYSIZE(Queried);
    ok(QueryFullProcessImageNameW(ProcessInfo.hProcess, 0, Queried, &QueriedSize),
       "QueryFullProcessImageNameW failed with %lu\n", GetLastError());
    ok(GetFinalComponent(Queried, Component, ARRAYSIZE(Component)),
       "no final component in '%ls'\n", Queried);
    ok(lstrcmpW(Component, StoredName) == 0,
       "QueryFullProcessImageNameW reports '%ls', expected the on-disk %ls\n", Component, StoredName);

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
                    ok(lstrcmpW(Entry.szExeFile, StoredName) == 0,
                       "ToolHelp reports '%ls', expected the on-disk %ls\n", Entry.szExeFile, StoredName);
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

START_TEST(ProcessImageNameCase)
{
    WCHAR ExactImage[MAX_PATH], UpperImage[MAX_PATH];

    /* The on-disk name is lowercase; ask for it with an uppercase extension */
    if (!ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\NOTEPAD.EXE",
                                   UpperImage, ARRAYSIZE(UpperImage)) ||
        GetFileAttributesW(UpperImage) == INVALID_FILE_ATTRIBUTES)
    {
        skip("notepad.exe is not available: %lu\n", GetLastError());
        return;
    }
    ok(GetFinalComponent(UpperImage, ExactImage, ARRAYSIZE(ExactImage)),
       "no final component in '%ls'\n", UpperImage);
    ok(lstrcmpiW(ExactImage, L"notepad.exe") == 0, "the image is '%ls', expected notepad.exe\n", ExactImage);
    ExactImage[0] = UNICODE_NULL;
    if (!ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\notepad.exe",
                                   ExactImage, ARRAYSIZE(ExactImage)))
    {
        skip("no room for the exact-case image path\n");
        return;
    }

    /* An exact-case open keeps the name unchanged */
    ExpectStoredName(ExactImage, L"notepad.exe");

    /* A differently-cased request still reports the stored spelling */
    ExpectStoredName(UpperImage, L"notepad.exe");
}
