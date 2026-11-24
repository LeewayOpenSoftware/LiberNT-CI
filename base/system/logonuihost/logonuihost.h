/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Shared declarations for LogonUIHost.exe
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

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
    PCWSTR Endpoint;            /* test build only; NULL means the production endpoint */
    BYTE ServerSid[SECURITY_MAX_SID_SIZE];
    PCWSTR TraceFile;           /* test build only */
} LUH_OPTIONS, *PLUH_OPTIONS;

extern LUH_OPTIONS g_Options;
extern WLUI_CONNECTION g_Connection;

/* Window messages the IPC thread posts to the UI thread; lParam owns a heap WLUI_COMMAND. */
#define WM_LUH_COMMAND      (WM_APP + 1)
#define WM_LUH_IPC_LOST     (WM_APP + 2)

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
