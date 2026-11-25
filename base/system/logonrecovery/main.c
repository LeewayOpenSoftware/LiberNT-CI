/*
 * PROJECT:     LiberNT logon recovery
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Minimal supervised recovery component: a resource dialog that talks the
 *              same wlui interface as the LogonUI host (PHASE3-PLAN 8.11 a, commit 9).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "logonrecovery.h"

WINE_DEFAULT_DEBUG_CHANNEL(logonrecovery);

LRC_OPTIONS g_Options;
WLUI_CONNECTION g_Connection;

/* The only provider identity the recovery role may claim (winlogon checks it, P4). */
static const GUID CLSID_PasswordProvider =
    { 0x60b78e88, 0xead8, 0x445c, { 0x9c, 0xfd, 0x0b, 0x87, 0xf7, 0x4e, 0xa6, 0xcd } };

static HANDLE g_PullThread;
static HANDLE g_StopEvent;
static HWND g_Dialog;
static LONG g_IpcLostPosted;
static DWORD g_PendingLoss;
static DWORD g_Scenario;            /* page in front: 0 = none yet */
static BOOL g_Change;               /* the change-password form is shown */
static BOOL g_Busy;                 /* a submission is with winlogon; inputs disabled */
static int g_ExitCode = LRC_EXIT_OK;

/* ---- tracing ---------------------------------------------------------------------- */

VOID
LrcTrace(
    _In_ PCSTR Event,
    _In_opt_ PCWSTR Detail)
{
    TRACE("%s %ls\n", Event, Detail ? Detail : L"");
#ifdef LOGONUI_TEST_HOOKS
    if (g_Options.TraceFile)
        LrcTestTrace(g_Options.TraceFile, Event, Detail);
#endif
}

/* ---- command line ------------------------------------------------------------------ */

static BOOL
LrcParseGuid(
    _In_ PCWSTR Text,
    _Out_ GUID *Guid)
{
    WCHAR Buffer[40];
    SIZE_T Length = wcslen(Text);

    /* {xxxxxxxx-...} with or without the braces */
    if (Length == 38 && Text[0] == L'{' && Text[37] == L'}')
    {
        CopyMemory(Buffer, Text + 1, 36 * sizeof(WCHAR));
        Buffer[36] = UNICODE_NULL;
    }
    else if (Length == 36)
    {
        StringCchCopyW(Buffer, ARRAYSIZE(Buffer), Text);
    }
    else
    {
        return FALSE;
    }
    return UuidFromStringW((RPC_WSTR)Buffer, Guid) == RPC_S_OK;
}

/* Winlogon passes plain tokens separated by spaces; nothing is quoted but the image path. */
static BOOL
LrcParseCommandLine(
    _In_ PCWSTR CommandLine,
    _Out_ PLRC_OPTIONS Options)
{
    BOOL HaveLaunchId = FALSE;
    PCWSTR Cursor = CommandLine;
    static WCHAR Arg[MAX_PATH];

    ZeroMemory(Options, sizeof(*Options));
    Options->Scenario = LRC_CPUS_LOGON;
    if (!WluiGetLocalSystemSid((PSID)Options->ServerSid))
        return FALSE;

    /* Skip the image path, quoted or not. */
    if (*Cursor == L'"')
    {
        Cursor++;
        while (*Cursor && *Cursor != L'"') Cursor++;
        if (*Cursor == L'"') Cursor++;
    }
    else
    {
        while (*Cursor && *Cursor != L' ') Cursor++;
    }

    for (;;)
    {
        SIZE_T Length = 0;

        while (*Cursor == L' ') Cursor++;
        if (*Cursor == UNICODE_NULL)
            break;
        while (Cursor[Length] && Cursor[Length] != L' ') Length++;
        if (Length >= ARRAYSIZE(Arg))
            return FALSE;
        CopyMemory(Arg, Cursor, Length * sizeof(WCHAR));
        Arg[Length] = UNICODE_NULL;
        Cursor += Length;

        if (_wcsnicmp(Arg, L"/launchid:", 10) == 0)
        {
            if (!LrcParseGuid(Arg + 10, &Options->LaunchId))
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
            /* The launcher adds it for the N2 tier; the recovery dialog has no other tier. */
        }
#ifdef LOGONUI_TEST_HOOKS
        else if (LrcTestParseOption(Arg, Options))
        {
            continue;
        }
#endif
        else
        {
            return FALSE;
        }
    }

    SecureZeroMemory(Arg, sizeof(Arg));
    return HaveLaunchId;
}

/* ---- IPC ----------------------------------------------------------------------------- */

static VOID
LrcIpcLost(
    _In_ DWORD Error)
{
    WCHAR Text[16];

    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu", Error);
    LrcTrace("ipc-lost", Text);
    if (g_Dialog == NULL)
    {
        if (g_PendingLoss == 0)
            g_PendingLoss = Error ? Error : ERROR_GEN_FAILURE;
        return;
    }
    if (InterlockedExchange(&g_IpcLostPosted, 1) == 0)
        PostMessageW(g_Dialog, WM_LRC_IPC_LOST, Error, 0);
}

/* Pulls commands until the stop event is set or the channel fails (same model as the host). */
static DWORD WINAPI
LrcPullThread(
    _In_ LPVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);

    while (WaitForSingleObject(g_StopEvent, 0) != WAIT_OBJECT_0)
    {
        WLUI_COMMAND *Command = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Command));
        DWORD Error;

        if (Command == NULL)
        {
            LrcIpcLost(ERROR_OUTOFMEMORY);
            break;
        }
        Error = WluiClientWaitForCommand(&g_Connection, Command);
        if (Error != ERROR_SUCCESS)
        {
            HeapFree(GetProcessHeap(), 0, Command);
            if (Error == ERROR_BUSY)
            {
                Sleep(100);
                continue;
            }
            LrcIpcLost(Error);
            break;
        }
        if (Command->Type == WluiCmdNone)
        {
            HeapFree(GetProcessHeap(), 0, Command);
            continue;
        }
        Command->Text[ARRAYSIZE(Command->Text) - 1] = UNICODE_NULL;
        Command->User[ARRAYSIZE(Command->User) - 1] = UNICODE_NULL;
        if (!PostMessageW(g_Dialog, WM_LRC_COMMAND, 0, (LPARAM)Command))
        {
            SecureZeroMemory(Command, sizeof(*Command));
            HeapFree(GetProcessHeap(), 0, Command);
            break;
        }
    }
    return 0;
}

static BOOL
LrcIpcStart(
    _In_ HWND Dialog)
{
    g_Dialog = Dialog;
    if (g_PendingLoss != 0)
    {
        DWORD Error = g_PendingLoss;
        g_PendingLoss = 0;
        LrcIpcLost(Error);
    }
    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_StopEvent == NULL)
        return FALSE;
    g_PullThread = CreateThread(NULL, 0, LrcPullThread, NULL, 0, NULL);
    return g_PullThread != NULL;
}

static VOID
LrcIpcStop(VOID)
{
    BOOL ThreadGone = TRUE;

    if (g_StopEvent)
        SetEvent(g_StopEvent);
    if (g_PullThread)
    {
        /* The thread usually sits inside the server's wait with the binding in use; a binding
         * freed under it would be a use-after-free, so it is left for process exit. */
        ThreadGone = (WaitForSingleObject(g_PullThread, 500) == WAIT_OBJECT_0);
        CloseHandle(g_PullThread);
        g_PullThread = NULL;
    }
    if (g_StopEvent)
    {
        CloseHandle(g_StopEvent);
        g_StopEvent = NULL;
    }
    if (ThreadGone)
        WluiDisconnect(&g_Connection);
}

static VOID
LrcIpcExiting(
    _In_ WLUI_EXIT_REASON Reason)
{
    WCHAR Text[16];

    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%u", Reason);
    LrcTrace("exiting", Text);
    if (g_Connection.Binding != NULL)
        WluiClientExiting(&g_Connection, Reason);
}

/* ---- the dialog ------------------------------------------------------------------- */

static VOID
LrcSetText(
    _In_ HWND Dialog,
    _In_ int Control,
    _In_ UINT StringId,
    _In_opt_ PCWSTR Argument)
{
    WCHAR Format[256], Text[320];

    if (LoadStringW(GetModuleHandleW(NULL), StringId, Format, ARRAYSIZE(Format)) == 0)
        Format[0] = UNICODE_NULL;
    if (Argument)
        StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Argument);
    else
        StringCchCopyW(Text, ARRAYSIZE(Text), Format);
    SetDlgItemTextW(Dialog, Control, Text);
}

static VOID
LrcShowControl(
    _In_ HWND Dialog,
    _In_ int Control,
    _In_ BOOL Show)
{
    ShowWindow(GetDlgItem(Dialog, Control), Show ? SW_SHOW : SW_HIDE);
}

static VOID
LrcEnableInputs(
    _In_ HWND Dialog,
    _In_ BOOL Enable)
{
    static const int Controls[] = { IDC_USER, IDC_PASSWORD, IDC_OLD, IDC_NEW, IDC_CONFIRM, IDC_SIGNIN };
    SIZE_T i;

    for (i = 0; i < ARRAYSIZE(Controls); i++)
        EnableWindow(GetDlgItem(Dialog, Controls[i]), Enable);
    g_Busy = !Enable;
}

/* Clears every secret the controls hold; an empty edit keeps no copy of the old text. */
static VOID
LrcClearSecrets(
    _In_ HWND Dialog)
{
    static const int Controls[] = { IDC_PASSWORD, IDC_OLD, IDC_NEW, IDC_CONFIRM };
    SIZE_T i;

    for (i = 0; i < ARRAYSIZE(Controls); i++)
        SetDlgItemTextW(Dialog, Controls[i], L"");
}

/* Lays the form out for the scenario: logon/unlock = user+password; change = user+old+new+confirm. */
static VOID
LrcShowPage(
    _In_ HWND Dialog,
    _In_ DWORD Scenario,
    _In_ DWORD Flags)
{
    BOOL Change = (Scenario == LRC_CPUS_CHANGE_PASSWORD) || (Flags & WLUI_FLAG_FORCED_CHANGE);
    BOOL Power = (Flags & WLUI_FLAG_NO_POWER) == 0;

    g_Scenario = Scenario;
    g_Change = Change;
    LrcSetText(Dialog, IDC_TITLE,
               Change ? IDS_TITLE_CHANGE : (Scenario == LRC_CPUS_UNLOCK ? IDS_TITLE_UNLOCK : IDS_TITLE_LOGON), NULL);
    LrcShowControl(Dialog, IDC_PASSWORD_LABEL, !Change);
    LrcShowControl(Dialog, IDC_PASSWORD, !Change);
    LrcShowControl(Dialog, IDC_OLD_LABEL, Change);
    LrcShowControl(Dialog, IDC_OLD, Change);
    LrcShowControl(Dialog, IDC_NEW_LABEL, Change);
    LrcShowControl(Dialog, IDC_NEW, Change);
    LrcShowControl(Dialog, IDC_CONFIRM_LABEL, Change);
    LrcShowControl(Dialog, IDC_CONFIRM, Change);
    LrcShowControl(Dialog, IDC_RESTART, Power);
    LrcShowControl(Dialog, IDC_SHUTDOWN, Power);
    SetDlgItemTextW(Dialog, IDC_STATUS, L"");
    LrcClearSecrets(Dialog);
    LrcEnableInputs(Dialog, TRUE);
    SetFocus(GetDlgItem(Dialog, GetWindowTextLengthW(GetDlgItem(Dialog, IDC_USER)) ? (Change ? IDC_OLD : IDC_PASSWORD) : IDC_USER));
    LrcTrace("page", Change ? L"change" : (Scenario == LRC_CPUS_UNLOCK ? L"unlock" : L"logon"));
}

/* "DOMAIN\user" or "user@domain" -> Domain and User; a bare name is a local account. */
static VOID
LrcSplitAccount(
    _Inout_ PWSTR Account,
    _Out_ PCWSTR *Domain,
    _Out_ PCWSTR *User)
{
    PWSTR Separator = wcschr(Account, L'\\');

    *Domain = NULL;
    *User = Account;
    if (Separator)
    {
        *Separator = UNICODE_NULL;
        *Domain = Account;
        *User = Separator + 1;
        return;
    }
    Separator = wcschr(Account, L'@');
    if (Separator)
    {
        *Separator = UNICODE_NULL;
        *Domain = Separator + 1;
    }
}

/* Packs the form with credser and hands it to winlogon; every copy is wiped afterwards. */
static VOID
LrcSubmit(
    _In_ HWND Dialog)
{
    WCHAR Account[CREDSER_MAX_NAME_CHARS * 2 + 2];
    WCHAR Password[CREDSER_MAX_PASSWORD_CHARS + 1];
    WCHAR NewPassword[CREDSER_MAX_PASSWORD_CHARS + 1];
    WCHAR Confirm[CREDSER_MAX_PASSWORD_CHARS + 1];
    PCWSTR Domain, User;
    PVOID Blob = NULL;
    ULONG Size = 0;
    NTSTATUS Status;
    DWORD Error = ERROR_SUCCESS;
    WCHAR Detail[32];

    if (g_Busy || g_Scenario == 0)
        return;

    GetDlgItemTextW(Dialog, IDC_USER, Account, ARRAYSIZE(Account));
    GetDlgItemTextW(Dialog, g_Change ? IDC_OLD : IDC_PASSWORD, Password, ARRAYSIZE(Password));
    if (g_Change)
    {
        GetDlgItemTextW(Dialog, IDC_NEW, NewPassword, ARRAYSIZE(NewPassword));
        GetDlgItemTextW(Dialog, IDC_CONFIRM, Confirm, ARRAYSIZE(Confirm));
        if (wcscmp(NewPassword, Confirm) != 0)
        {
            LrcSetText(Dialog, IDC_STATUS, IDS_MISMATCH, NULL);
            SecureZeroMemory(NewPassword, sizeof(NewPassword));
            SecureZeroMemory(Confirm, sizeof(Confirm));
            SecureZeroMemory(Password, sizeof(Password));
            SetDlgItemTextW(Dialog, IDC_NEW, L"");
            SetDlgItemTextW(Dialog, IDC_CONFIRM, L"");
            return;
        }
    }
    if (Account[0] == UNICODE_NULL)
    {
        SetFocus(GetDlgItem(Dialog, IDC_USER));
        goto wipe;
    }
    LrcSplitAccount(Account, &Domain, &User);

    /* Two calls: size, then pack. */
    if (g_Change)
        Status = CredSerPackChangePassword(Domain, User, Password, NewPassword, NULL, &Size);
    else
        Status = CredSerPackLogon(g_Scenario == LRC_CPUS_UNLOCK ? KerbWorkstationUnlockLogon : KerbInteractiveLogon,
                                  Domain, User, Password, NULL, NULL, &Size);
    if (Status == STATUS_BUFFER_TOO_SMALL || (NT_SUCCESS(Status) && Size != 0))
    {
        Blob = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
        if (Blob == NULL)
            goto wipe;
        if (g_Change)
            Status = CredSerPackChangePassword(Domain, User, Password, NewPassword, Blob, &Size);
        else
            Status = CredSerPackLogon(g_Scenario == LRC_CPUS_UNLOCK ? KerbWorkstationUnlockLogon : KerbInteractiveLogon,
                                      Domain, User, Password, NULL, Blob, &Size);
    }
    if (!NT_SUCCESS(Status))
    {
        StringCchPrintfW(Detail, ARRAYSIZE(Detail), L"0x%08lX", Status);
        LrcTrace("pack-failed", Detail);
        LrcSetText(Dialog, IDC_STATUS, IDS_REFUSED, NULL);
        goto wipe;
    }

    LrcEnableInputs(Dialog, FALSE);
    /* The recovery role has no LSA handle: package 0 means winlogon's MSV1_0 (P4, server side). */
    Error = WluiClientSubmitSerialization(&g_Connection, g_Scenario, &CLSID_PasswordProvider, 0, Blob, Size);
    LrcTrace("submitted", Error == ERROR_SUCCESS ? L"ok" : L"refused");
    if (Error != ERROR_SUCCESS)
    {
        WCHAR Text[256], Format[128];
        if (LoadStringW(GetModuleHandleW(NULL), IDS_REFUSED, Format, ARRAYSIZE(Format)) == 0)
            Format[0] = UNICODE_NULL;
        StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Error);
        SetDlgItemTextW(Dialog, IDC_STATUS, Text);
        LrcEnableInputs(Dialog, TRUE);
    }

wipe:
    if (Blob)
    {
        SecureZeroMemory(Blob, Size);
#ifdef LOGONUI_TEST_HOOKS
        {
            const BYTE *Bytes = Blob;
            ULONG i, Dirty = 0;
            for (i = 0; i < Size; i++)
                Dirty |= Bytes[i];
            LrcTrace("zeroed", Dirty ? L"0" : L"1");
        }
#endif
        HeapFree(GetProcessHeap(), 0, Blob);
    }
    SecureZeroMemory(Account, sizeof(Account));
    SecureZeroMemory(Password, sizeof(Password));
    SecureZeroMemory(NewPassword, sizeof(NewPassword));
    SecureZeroMemory(Confirm, sizeof(Confirm));
    LrcClearSecrets(Dialog);
}

#ifdef LOGONUI_TEST_HOOKS
/* Test build: "/autosubmit:user;password" types the pair and submits at the first page. */
static VOID
LrcAutoSubmit(
    _In_ HWND Dialog)
{
    WCHAR Copy[128];
    PWSTR Separator;

    if (g_Options.AutoSubmit == NULL)
        return;
    StringCchCopyW(Copy, ARRAYSIZE(Copy), g_Options.AutoSubmit);
    Separator = wcschr(Copy, L';');
    if (Separator == NULL)
        return;
    *Separator = UNICODE_NULL;
    SetDlgItemTextW(Dialog, IDC_USER, Copy);
    SetDlgItemTextW(Dialog, g_Change ? IDC_OLD : IDC_PASSWORD, Separator + 1);
    if (g_Change)
    {
        SetDlgItemTextW(Dialog, IDC_NEW, Separator + 1);
        SetDlgItemTextW(Dialog, IDC_CONFIRM, Separator + 1);
    }
    SecureZeroMemory(Copy, sizeof(Copy));
    LrcSubmit(Dialog);
}
#endif

static VOID
LrcLeave(
    _In_ HWND Dialog,
    _In_ WLUI_EXIT_REASON Reason,
    _In_ int ExitCode)
{
    g_ExitCode = ExitCode;
    KillTimer(Dialog, LRC_HEARTBEAT_TIMER);
    LrcClearSecrets(Dialog);
    LrcIpcExiting(Reason);
    DestroyWindow(Dialog);
}

static VOID
LrcOnCommand(
    _In_ HWND Dialog,
    _In_ WLUI_COMMAND *Command)
{
    WCHAR Detail[32];

    switch (Command->Type)
    {
        case WluiCmdRequestCredentials:
            LrcShowPage(Dialog, Command->Scenario ? Command->Scenario : g_Options.Scenario, Command->Flags);
#ifdef LOGONUI_TEST_HOOKS
            LrcAutoSubmit(Dialog);
#endif
            break;

        case WluiCmdDisplayStatus:
            SetDlgItemTextW(Dialog, IDC_STATUS, Command->Text);
            LrcEnableInputs(Dialog, FALSE);
            LrcTrace("status", Command->Text);
            break;

        case WluiCmdDisplayWelcome:
            LrcSetText(Dialog, IDC_STATUS, IDS_WELCOME, Command->User);
            LrcEnableInputs(Dialog, FALSE);
            LrcTrace("welcome", Command->User);
            break;

        case WluiCmdReportResult:
            if (Command->Text[0])
            {
                SetDlgItemTextW(Dialog, IDC_STATUS, Command->Text);
            }
            else
            {
                WCHAR Text[128], Format[96];
                if (LoadStringW(GetModuleHandleW(NULL), IDS_FAILED, Format, ARRAYSIZE(Format)) == 0)
                    Format[0] = UNICODE_NULL;
                StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Command->Status);
                SetDlgItemTextW(Dialog, IDC_STATUS, Text);
            }
            StringCchPrintfW(Detail, ARRAYSIZE(Detail), L"0x%08lX", Command->Status);
            LrcTrace("result", Detail);
            LrcClearSecrets(Dialog);
            LrcEnableInputs(Dialog, TRUE);
            SetFocus(GetDlgItem(Dialog, g_Change ? IDC_OLD : IDC_PASSWORD));
            break;

        case WluiCmdReadyForDesktopSwitch:
            /* No fade here: the dialog simply leaves once the desktop behind is painted. */
            LrcLeave(Dialog, WluiExitAfterDesktopSwitch, LRC_EXIT_OK);
            break;

        case WluiCmdDismiss:
            LrcLeave(Dialog, WluiExitDismissed, LRC_EXIT_OK);
            break;

        default:
            WARN("unknown command %u ignored\n", Command->Type);
            break;
    }
}

static INT_PTR CALLBACK
LrcDialogProc(
    _In_ HWND Dialog,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (Message)
    {
        case WM_INITDIALOG:
            SendDlgItemMessageW(Dialog, IDC_USER, EM_LIMITTEXT, CREDSER_MAX_NAME_CHARS * 2, 0);
            SendDlgItemMessageW(Dialog, IDC_PASSWORD, EM_LIMITTEXT, CREDSER_MAX_PASSWORD_CHARS, 0);
            SendDlgItemMessageW(Dialog, IDC_OLD, EM_LIMITTEXT, CREDSER_MAX_PASSWORD_CHARS, 0);
            SendDlgItemMessageW(Dialog, IDC_NEW, EM_LIMITTEXT, CREDSER_MAX_PASSWORD_CHARS, 0);
            SendDlgItemMessageW(Dialog, IDC_CONFIRM, EM_LIMITTEXT, CREDSER_MAX_PASSWORD_CHARS, 0);
            LrcEnableInputs(Dialog, FALSE);     /* nothing is sent before winlogon asks */
            return TRUE;

        case WM_TIMER:
            if (wParam == LRC_HEARTBEAT_TIMER)
            {
                /* Synchronous on purpose: a UI thread that cannot complete it is a hang to winlogon. */
                DWORD Error = WluiClientHeartbeat(&g_Connection);
                if (Error != ERROR_SUCCESS)
                    LrcIpcLost(Error);
            }
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDC_SIGNIN:
                case IDOK:
                    LrcSubmit(Dialog);
                    return TRUE;
                case IDC_RESTART:
                case IDC_SHUTDOWN:
                {
                    DWORD Error = WluiClientRequestAction(&g_Connection,
                        LOWORD(wParam) == IDC_RESTART ? WluiActionRestart : WluiActionShutdown);
                    LrcTrace("action", Error == ERROR_SUCCESS ? L"ok" : L"refused");
                    return TRUE;
                }
                case IDCANCEL:
                    return TRUE;        /* Escape closes nothing: only winlogon dismisses */
            }
            break;

        case WM_LRC_COMMAND:
        {
            WLUI_COMMAND *Command = (WLUI_COMMAND *)lParam;
            LrcOnCommand(Dialog, Command);
            SecureZeroMemory(Command, sizeof(*Command));
            HeapFree(GetProcessHeap(), 0, Command);
            return TRUE;
        }

        case WM_LRC_IPC_LOST:
            /* 995: winlogon ended the launch; anything else: the channel is gone. Leave either way. */
            LrcLeave(Dialog, WluiExitInternalError, (DWORD)wParam == ERROR_OPERATION_ABORTED ? LRC_EXIT_OK : LRC_EXIT_IPC_FAILED);
            return TRUE;

        case WM_CLOSE:
            return TRUE;

        case WM_DESTROY:
            PostQuitMessage(g_ExitCode);
            return TRUE;
    }
    return FALSE;
}

/* ---- process ---------------------------------------------------------------------- */

/* 8.10 f: a crash must not leave a dump with credential memory behind. */
static LONG WINAPI
LrcUnhandledException(
    _In_ EXCEPTION_POINTERS *Pointers)
{
    ERR("unhandled exception 0x%08lx, exiting without a dump\n",
        Pointers->ExceptionRecord->ExceptionCode);
    TerminateProcess(GetCurrentProcess(), LRC_EXIT_UI_FAILED);
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
    HWND Dialog;
    MSG Msg;
    DWORD Error;

    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CommandLine);
    UNREFERENCED_PARAMETER(ShowCommand);

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(LrcUnhandledException);

    if (!LrcParseCommandLine(GetCommandLineW(), &g_Options))
    {
        ERR("bad command line\n");
        return LRC_EXIT_BAD_ARGUMENTS;
    }

    /* P1: the client names the server; a non-SYSTEM endpoint owner fails the connect. */
    if (WluiConnect(g_Options.Endpoint, (PSID)g_Options.ServerSid, &g_Options.LaunchId, &g_Connection) != RPC_S_OK)
        return LRC_EXIT_IPC_FAILED;

    Dialog = CreateDialogParamW(Instance, MAKEINTRESOURCEW(IDD_RECOVERY), NULL, LrcDialogProc, 0);
    if (Dialog == NULL)
    {
        LrcIpcExiting(WluiExitInternalError);
        LrcIpcStop();
        return LRC_EXIT_UI_FAILED;
    }
    if (!LrcIpcStart(Dialog))
    {
        DestroyWindow(Dialog);
        LrcIpcStop();
        return LRC_EXIT_IPC_FAILED;
    }

    /* The first frame is on screen before Ready, as the host does (6.3). */
    ShowWindow(Dialog, SW_SHOW);
    UpdateWindow(Dialog);
    GdiFlush();
    Error = WluiClientReady(&g_Connection);
    LrcTrace("ready", NULL);
    if (Error != ERROR_SUCCESS)
        LrcIpcLost(Error);
    SetTimer(Dialog, LRC_HEARTBEAT_TIMER, LRC_HEARTBEAT_MS, NULL);

    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        if (!IsDialogMessageW(Dialog, &Msg))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
    }

    LrcIpcStop();
    SecureZeroMemory(&g_Options, sizeof(g_Options));
    return g_ExitCode;
}
