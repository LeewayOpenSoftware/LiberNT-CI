/*
 * PROJECT:     LiberNT logon recovery
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Shared declarations for LogonRecovery.exe
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#include <windows.h>
#include <strsafe.h>
#include <wluiclient.h>
#include <ntsecapi.h>
#include <credser.h>

#include <wine/debug.h>

#include "resource.h"

/* Exit codes winlogon's supervision can tell apart (same values as the host). */
#define LRC_EXIT_OK                 0
#define LRC_EXIT_BAD_ARGUMENTS      1
#define LRC_EXIT_IPC_FAILED         2
#define LRC_EXIT_UI_FAILED          4

#define LRC_HEARTBEAT_MS            2000
#define LRC_HEARTBEAT_TIMER         1

#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL     ((NTSTATUS)0xC0000023L)
#endif

/* CPUS_* values the recovery component understands (credentialprovider.idl). */
#define LRC_CPUS_LOGON              1
#define LRC_CPUS_UNLOCK             2
#define LRC_CPUS_CHANGE_PASSWORD    3

typedef struct _LRC_OPTIONS
{
    GUID LaunchId;
    DWORD Scenario;
    PCWSTR Endpoint;            /* test build only; NULL means the production endpoint */
    BYTE ServerSid[SECURITY_MAX_SID_SIZE];
    PCWSTR TraceFile;           /* test build only */
    PCWSTR AutoSubmit;          /* test build only: "user;password" typed and sent at RequestCredentials */
} LRC_OPTIONS, *PLRC_OPTIONS;

extern LRC_OPTIONS g_Options;
extern WLUI_CONNECTION g_Connection;

/* Window messages the IPC thread posts to the dialog; lParam owns a heap WLUI_COMMAND. */
#define WM_LRC_COMMAND      (WM_APP + 1)
#define WM_LRC_IPC_LOST     (WM_APP + 2)

VOID LrcTrace(_In_ PCSTR Event, _In_opt_ PCWSTR Detail);

#ifdef LOGONUI_TEST_HOOKS
BOOL LrcTestParseOption(_In_ PCWSTR Arg, _Inout_ PLRC_OPTIONS Options);
VOID LrcTestTrace(_In_ PCWSTR TraceFile, _In_ PCSTR Event, _In_opt_ PCWSTR Detail);
#endif
