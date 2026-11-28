/*
 * PROJECT:     Orosi Logon Reminder
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Command-line contract of wlrmdr.exe
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#pragma once

#include <windef.h>

/* -s value that means "no auto-dismiss" (any negative value is treated the same) */
#define WLRMDR_TIMEOUT_INFINITE (-1L)

/* Local\<prefix><pid of the wlrmdr process>; winlogon signals it to dismiss the reminder */
#define WLRMDR_STOP_EVENT_PREFIX L"Local\\Wl_Stop_Reminder_Event_"

/* Process exit code when the reminder was dismissed through the stop event while a dialog was up */
#define WLRMDR_EXIT_DISMISSED ERROR_OPERATION_ABORTED

typedef struct _WLRMDR_ARGS
{
    LONG TimeoutMs;       /* -s, negative = infinite; infinite when -s is absent */
    ULONG Flags;          /* -f, NOTIFYICONDATA dwInfoFlags shape (low nibble = icon, 0x10 = no sound) */
    ULONG Action;         /* -a, scenario selector; only 3 (timed shutdown) has been observed */
    BOOL HasAction;
    PCWSTR Title;         /* -t, NULL when absent */
    PCWSTR Message;       /* -m, NULL when absent: no dialog is shown */
} WLRMDR_ARGS;

/* argv[0] is skipped; unknown switches and switches with a missing or non-numeric value are ignored */
VOID WlrParseCmdLine(INT argc, PWSTR *argv, WLRMDR_ARGS *Args);
