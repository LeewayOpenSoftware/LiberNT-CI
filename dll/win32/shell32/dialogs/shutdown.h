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
 * Shutdown option / selection codes. These match the WLX_SHUTDOWN_STATE_*
 * bits (winwlx.h) and the 0x01/0x02/0x04/0x10/0x40 result values that
 * ExitWindowsDialog() already acts on, so no caller mapping changes.
 */

/* Which shutdown choices are allowed, per policy and power capabilities.
   shell32 runs in the user's context, so HKCU is the user's own hive. */
DWORD ShellGetAllowedShutdownOptions(VOID);

/* Remembered selection (WLX_SHUTDOWN_STATE_* value) in HKCU Explorer. */
DWORD ShellLoadShutdownSelState(VOID);
VOID  ShellSaveShutdownSelState(DWORD State);

/* HKLM policy value trumps HKCU, under the same sub-key. */
LONG  ShellGetPolicyDWORD(PCWSTR pPolicyKey, PCWSTR ValueName, PDWORD pValue);
