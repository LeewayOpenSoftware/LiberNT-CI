/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test-only switches, built into logonuihost_test.exe only
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "logonuihost.h"
#include <sddl.h>

#ifndef LOGONUI_TEST_HOOKS
#error testhooks.c belongs to logonuihost_test.exe only
#endif

/* The marker string nohooks.cmake looks for in the shipped binary. */
static const char LuhTestMarker[] = "LogonUiHostTest";

static WCHAR g_Endpoint[64];
static WCHAR g_TraceFile[MAX_PATH];

BOOL
LuhTestParseOption(
    _In_ PCWSTR Arg,
    _Inout_ PLUH_OPTIONS Options)
{
    if (_wcsnicmp(Arg, L"/endpoint:", 10) == 0)
    {
        if (FAILED(StringCchCopyW(g_Endpoint, ARRAYSIZE(g_Endpoint), Arg + 10)))
            return FALSE;
        Options->Endpoint = g_Endpoint;
        return TRUE;
    }
    if (_wcsnicmp(Arg, L"/expect-server-sid:", 19) == 0)
    {
        PSID Sid = NULL;
        BOOL Ok = ConvertStringSidToSidW(Arg + 19, &Sid) &&
                  GetLengthSid(Sid) <= SECURITY_MAX_SID_SIZE &&
                  CopySid(SECURITY_MAX_SID_SIZE, (PSID)Options->ServerSid, Sid);
        if (Sid) LocalFree(Sid);
        return Ok;
    }
    if (_wcsnicmp(Arg, L"/trace:", 7) == 0)
    {
        if (FAILED(StringCchCopyW(g_TraceFile, ARRAYSIZE(g_TraceFile), Arg + 7)))
            return FALSE;
        Options->TraceFile = g_TraceFile;
        return TRUE;
    }
    return FALSE;
}

VOID
LuhTestTrace(
    _In_ PCWSTR TraceFile,
    _In_ PCSTR Event,
    _In_opt_ PCWSTR Detail)
{
    HANDLE File;
    CHAR Line[600];
    DWORD Written;

    File = CreateFileW(TraceFile, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return;
    StringCchPrintfA(Line, ARRAYSIZE(Line), "%s %lu %s %ls\r\n", LuhTestMarker, GetTickCount(), Event,
                     Detail ? Detail : L"");
    WriteFile(File, Line, (DWORD)strlen(Line), &Written, NULL);
    CloseHandle(File);
}
