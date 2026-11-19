/*
 * PROJECT:     LiberNT COM security
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Access, launch and activation checks shared by combase and RpcSs
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#include <windef.h>
#include <winbase.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HKLM\Software\LiberNT\COM, value Enforcement (REG_DWORD) */
#define COMSEC_OFF      0   /* no check (the old path) */
#define COMSEC_AUDIT    1   /* compute the decision, log a denial, allow the call */
#define COMSEC_ENFORCE  2   /* compute the decision, log a denial, refuse the call */

/* Reads the switch; anything missing, of the wrong type or out of range is COMSEC_OFF */
DWORD comsec_read_mode(void);

/* Reads a self-relative security descriptor stored as REG_BINARY and returns a
 * normalized copy (see comsec_normalize), or NULL when the value is absent or invalid.
 * The key is opened in the 64-bit view. Free the result with LocalFree. */
PSECURITY_DESCRIPTOR comsec_read_sd(HKEY root, const WCHAR *subkey, const WCHAR *value);

/* Copies the DACL of a descriptor into a new self-relative descriptor that AccessCheck
 * accepts (owner and group set to Local System). An ACE whose mask is exactly
 * COM_RIGHTS_EXECUTE, as written before Windows XP SP2, is widened to every COM right.
 * Returns NULL on failure. Free the result with LocalFree. */
PSECURITY_DESCRIPTOR comsec_normalize(PSECURITY_DESCRIPTOR sd, DWORD length);

/* Documented defaults when the registry has no value:
 * launch and activation: Local System, Administrators and INTERACTIVE, every right;
 * access: the server's own user and Local System. */
PSECURITY_DESCRIPTOR comsec_default_launch_sd(void);
PSECURITY_DESCRIPTOR comsec_default_access_sd(PSID self);

/* TRUE when the token is granted COM_RIGHTS_EXECUTE plus the given rights. A NULL
 * descriptor grants everything. The token must be an impersonation token. */
BOOL comsec_check(HANDLE token, PSECURITY_DESCRIPTOR sd, DWORD rights);

/* Logs a denial: Application event log, source "LiberNT-COM". Event 1: refused
 * (enforce), event 2: would be refused (audit). At most 64 events per minute and process. */
void comsec_report_denial(DWORD mode, const WCHAR *what, const GUID *clsid, HANDLE token, DWORD pid);

/* Token helpers; the SID is allocated with LocalAlloc */
PSID comsec_token_user(HANDLE token);
DWORD comsec_token_integrity(HANDLE token);
BOOL comsec_token_logon(HANDLE token, LUID *logon);

#ifdef __cplusplus
}
#endif
