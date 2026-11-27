/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Shared declarations for LogonUIHost.exe
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#define COBJMACROS
#define CONST_VTABLE
#include <windows.h>
#include <strsafe.h>
#include <wluiclient.h>

#include <wine/debug.h>

/* Exit codes winlogon's supervision can tell apart. */
#define LUH_EXIT_OK                 0
#define LUH_EXIT_BAD_ARGUMENTS      1
#define LUH_EXIT_IPC_FAILED         2
#define LUH_EXIT_NOT_SUPPORTED      3
#define LUH_EXIT_UI_FAILED          4

#define LUH_HEARTBEAT_MS            2000
#define LUH_FADE_MS                 400
#define LUH_FADE_STEPS              16

typedef struct _LUH_OPTIONS
{
    GUID LaunchId;
    DWORD Scenario;
    BOOL Fallback;
    BOOL NoPower;           /* WLUI_FLAG_NO_POWER from the last RequestCredentials */
    WCHAR PrefillUser[256]; /* WLUI_COMMAND.User of the last RequestCredentials: a forced change's account */
    PCWSTR Endpoint;            /* test build only; NULL means the production endpoint */
    BYTE ServerSid[SECURITY_MAX_SID_SIZE];
    PCWSTR TraceFile;           /* test build only */
} LUH_OPTIONS, *PLUH_OPTIONS;

extern LUH_OPTIONS g_Options;
extern WLUI_CONNECTION g_Connection;

/* Window messages the IPC thread posts to the UI thread; lParam owns a heap WLUI_COMMAND. */
#define WM_LUH_COMMAND      (WM_APP + 1)
#define WM_LUH_IPC_LOST     (WM_APP + 2)
/* From the provider thread: the page model is ready (lParam = PLUH_PAGE) or not (wParam says why). */
#define WM_LUH_PAGE         (WM_APP + 3)
/* From the events sink: a field changed; lParam owns a heap LUH_FIELD_UPDATE. */
#define WM_LUH_FIELD        (WM_APP + 4)

#define LUH_PAGE_READY      0
#define LUH_PAGE_FAILED     1   /* no provider offered a credential */
#define LUH_PAGE_POLICY     2   /* policy forbids the only credential left (plan §8.10 c) */

#define LUH_PROVIDER_CALL_MS 10000
#define LUH_FAULT_TIMEOUT    1
#define LUH_FAULT_STARTUP    2
#define LUH_MAX_FIELDS       32
#define LUH_FIELD_TEXT       256
#define LUH_MAX_TILES        64
#define LUH_TILE_TEXT        128

#include <credentialprovider.h>

typedef struct _LUH_FIELD
{
    DWORD Id;
    CREDENTIAL_PROVIDER_FIELD_TYPE Type;
    CREDENTIAL_PROVIDER_FIELD_STATE State;
    CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE Interactive;
    WCHAR Label[LUH_FIELD_TEXT];
    WCHAR Value[LUH_FIELD_TEXT];
    HWND Control;
    HWND LabelControl;          /* the label row above an edit field; destroyed with the page */
} LUH_FIELD, *PLUH_FIELD;

typedef struct _LUH_PAGE
{
    PVOID Owner;                /* the provider thread that built it; stale pages are dropped */
    DWORD Scenario;
    DWORD Count;
    DWORD SubmitField;
    DWORD SubmitAdjacentTo;
    LUH_FIELD Fields[LUH_MAX_FIELDS];
    DWORD TileCount;            /* every credential of every provider; one means no tile list */
    DWORD TileSelected;         /* the tile whose fields are above */
    WCHAR TileNames[LUH_MAX_TILES][LUH_TILE_TEXT];
} LUH_PAGE, *PLUH_PAGE;

typedef enum _LUH_FIELD_UPDATE_KIND
{
    LuhFieldText = 1,
    LuhFieldState,
    LuhFieldInteractive,
    LuhFieldCheckbox
} LUH_FIELD_UPDATE_KIND;

typedef struct _LUH_FIELD_UPDATE
{
    PVOID Owner;                /* the provider thread whose credential sent it */
    DWORD Field;
    LUH_FIELD_UPDATE_KIND Kind;
    DWORD Value;
    WCHAR Text[LUH_FIELD_TEXT];
} LUH_FIELD_UPDATE, *PLUH_FIELD_UPDATE;

typedef enum _LUH_SUBMIT_RESULT
{
    LuhSubmitSent = 0,      /* serialization handed to winlogon; wait for ReportResult */
    LuhSubmitNotFinished,   /* the provider wants more input */
    LuhSubmitRefused,       /* winlogon refused the submission */
    LuhSubmitFailed,        /* the provider failed */
    LuhSubmitProviderGone   /* the provider timed out and was dropped */
} LUH_SUBMIT_RESULT;

/* providers.c */
BOOL LuhProvidersStart(_In_ HWND Window);
VOID LuhProvidersStop(VOID);
/* No page within the deadline: TRUE when restarted in fallback, FALSE when nothing is left to try. */
BOOL LuhProvidersStartupTimeout(VOID);
BOOL LuhProvidersIsCurrent(_In_ PVOID Owner);
BOOL LuhProviderSetString(_In_ DWORD Field, _In_ PCWSTR Text);
LUH_SUBMIT_RESULT LuhProviderSubmit(_Out_writes_(StatusChars) PWSTR StatusText, _In_ SIZE_T StatusChars);
BOOL LuhProviderReportResult(_In_ NTSTATUS Status, _In_ NTSTATUS SubStatus,
                             _Out_writes_(StatusChars) PWSTR StatusText, _In_ SIZE_T StatusChars);
VOID LuhProviderDeselect(VOID);
BOOL LuhProviderSelectTile(_In_ DWORD Index);

/* users.c */
#define LUH_MAX_USERS        64
HRESULT LuhUsersCreate(_In_opt_ PCWSTR Only, _Outptr_ ICredentialProviderUserArray **Out);

/* main.c */
BOOL LuhParseCommandLine(_In_ int argc, _In_reads_(argc) WCHAR **argv, _Out_ PLUH_OPTIONS Options);
VOID LuhTrace(_In_ PCSTR Event, _In_opt_ PCWSTR Detail);

/* ipc.c */
BOOL LuhIpcConnect(VOID);
BOOL LuhIpcStart(_In_ HWND Window);
VOID LuhIpcStop(VOID);
VOID LuhIpcHeartbeat(VOID);
VOID LuhIpcReady(VOID);
VOID LuhIpcExiting(_In_ WLUI_EXIT_REASON Reason);

/* ui.c */
HWND LuhUiCreate(_In_ HINSTANCE Instance);
int LuhUiRun(_In_ HWND Window);

/* testhooks.c, built into logonuihost_test.exe only */
#ifdef LOGONUI_TEST_HOOKS
BOOL LuhTestParseOption(_In_ PCWSTR Arg, _Inout_ PLUH_OPTIONS Options);
VOID LuhTestTrace(_In_ PCWSTR TraceFile, _In_ PCSTR Event, _In_opt_ PCWSTR Detail);
#endif
