/*
 * PROJECT:     Orosi shell32.dll
 * LICENSE:     GPL-2.0-or-later (see COPYING in the top level directory)
 * PURPOSE:     Shut Down Windows dialog: shutdown-option gating and saved state.
 * COPYRIGHT:   Origin: ReactOS msgina/shutdown.c - Lee Schroeder, Hermes Belusca-Maito,
 *                      Arnav Bhatt, Ahmed ARIF.
 *              Copyright 2026 Leeway Open Software
 */

#pragma once

/*
 * Shutdown option / selection codes: shell32's own result values (formerly
 * msgina's WLX_SHUTDOWN_STATE_* bits). ExitWindowsDialog() acts on the same
 * 0x01/0x02/0x04/0x10/0x40 codes, so no caller mapping changes.
 */
#define WLX_SHUTDOWN_STATE_LOGOFF       0x01
#define WLX_SHUTDOWN_STATE_POWER_OFF    0x02
#define WLX_SHUTDOWN_STATE_REBOOT       0x04
#define WLX_SHUTDOWN_STATE_SLEEP        0x10
#define WLX_SHUTDOWN_STATE_HIBERNATE    0x40

/* Which shutdown choices are allowed, per policy and power capabilities.
   shell32 runs in the user's context, so HKCU is the user's own hive. */
DWORD ShellGetAllowedShutdownOptions(VOID);

/* HKLM policy value trumps HKCU, under the same sub-key. */
LONG  ShellGetPolicyDWORD(PCWSTR pPolicyKey, PCWSTR ValueName, PDWORD pValue);

/* The classic "Shut Down Windows" dialog. Returns the chosen
   WLX_SHUTDOWN_STATE_* code (0x01/0x02/0x04/0x10/0x40), or 0 if cancelled or
   nothing is allowed. dwExcludeOptions masks choices out
   (e.g. ~(DWORD)WLX_SHUTDOWN_STATE_LOGOFF for a logoff-only prompt). */
DWORD ShellShutdownWindowsDialog(HWND hWndOwner, DWORD dwExcludeOptions);
