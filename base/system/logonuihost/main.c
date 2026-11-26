/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Process entry: arguments, crash policy, IPC, UI loop
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "logonuihost.h"
#include <shellapi.h>

WINE_DEFAULT_DEBUG_CHANNEL(logonuihost);

LUH_OPTIONS g_Options;
WLUI_CONNECTION g_Connection;

static BOOL
LuhParseGuid(
    _In_ PCWSTR Text,
    _Out_ GUID *Guid)
{
    RPC_WSTR Copy;
    WCHAR Buffer[40];
    size_t Length;

    /* Accept {...} or bare; UuidFromString wants the bare form. */
    if (FAILED(StringCchLengthW(Text, ARRAYSIZE(Buffer), &Length)) || Length < 36)
        return FALSE;
    if (Text[0] == L'{')
    {
        if (Length != 38 || Text[37] != L'}')
            return FALSE;
        StringCchCopyNW(Buffer, ARRAYSIZE(Buffer), Text + 1, 36);
    }
    else
    {
        if (Length != 36)
            return FALSE;
        StringCchCopyW(Buffer, ARRAYSIZE(Buffer), Text);
    }
    Copy = (RPC_WSTR)Buffer;
    return UuidFromStringW(Copy, Guid) == RPC_S_OK;
}

BOOL
LuhParseCommandLine(
    _In_ int argc,
    _In_reads_(argc) WCHAR **argv,
    _Out_ PLUH_OPTIONS Options)
{
    BOOL HaveLaunchId = FALSE;
    int i;

    ZeroMemory(Options, sizeof(*Options));
    Options->Scenario = 1; /* CPUS_LOGON */
    if (!WluiGetLocalSystemSid((PSID)Options->ServerSid))
        return FALSE;

    for (i = 1; i < argc; i++)
    {
        PCWSTR Arg = argv[i];

        if (_wcsnicmp(Arg, L"/launchid:", 10) == 0)
        {
            if (!LuhParseGuid(Arg + 10, &Options->LaunchId))
                return FALSE;
            HaveLaunchId = TRUE;
        }
        else if (_wcsnicmp(Arg, L"/scenario:", 10) == 0)
        {
            PWSTR End = NULL;
            ULONG Value = wcstoul(Arg + 10, &End, 10);
            if (End == Arg + 10 || *End != UNICODE_NULL || Value == 0 || Value > 5)
                return FALSE;
            Options->Scenario = Value;
        }
        else if (_wcsicmp(Arg, L"/fallback") == 0)
        {
            Options->Fallback = TRUE;
        }
#ifdef LOGONUI_TEST_HOOKS
        else if (LuhTestParseOption(Arg, Options))
        {
            continue;
        }
#endif
        else
        {
            return FALSE;
        }
    }

    return HaveLaunchId;
}

VOID
LuhTrace(
    _In_ PCSTR Event,
    _In_opt_ PCWSTR Detail)
{
    TRACE("%s %ls\n", Event, Detail ? Detail : L"");
#ifdef LOGONUI_TEST_HOOKS
    if (g_Options.TraceFile)
        LuhTestTrace(g_Options.TraceFile, Event, Detail);
#endif
}

/* §8.10 f: a crash must not leave a dump with credential memory behind. */
static LONG WINAPI
LuhUnhandledException(
    _In_ EXCEPTION_POINTERS *Pointers)
{
    ERR("unhandled exception 0x%08lx, exiting without a dump\n",
        Pointers->ExceptionRecord->ExceptionCode);
    TerminateProcess(GetCurrentProcess(), LUH_EXIT_UI_FAILED);
    return EXCEPTION_EXECUTE_HANDLER;
}

int
WINAPI
wWinMain(
    _In_ HINSTANCE Instance,
    _In_opt_ HINSTANCE Previous,
    _In_ LPWSTR CommandLine,
    _In_ int ShowCommand)
{
    WCHAR **argv;
    int argc, Result;
    HWND Window;

    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CommandLine);
    UNREFERENCED_PARAMETER(ShowCommand);

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(LuhUnhandledException);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == NULL || !LuhParseCommandLine(argc, argv, &g_Options))
    {
        ERR("bad command line\n");
        if (argv) LocalFree(argv);
        return LUH_EXIT_BAD_ARGUMENTS;
    }
    LocalFree(argv);

    if (!LuhIpcConnect())
        return LUH_EXIT_IPC_FAILED;

    Window = LuhUiCreate(Instance);
    if (Window == NULL)
    {
        LuhIpcExiting(WluiExitInternalError);
        LuhIpcStop();
        return LUH_EXIT_UI_FAILED;
    }

    if (!LuhIpcStart(Window))
    {
        DestroyWindow(Window);
        LuhIpcStop();
        return LUH_EXIT_IPC_FAILED;
    }

    Result = LuhUiRun(Window);
    LuhIpcStop();
    SecureZeroMemory(&g_Options, sizeof(g_Options));
    return Result;
}
