/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Launch table and call authorizer for the wlui interface.
 *              Pure logic, no RPC: winlogon feeds it the caller's PID and SID.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _WLUI_AUTH_ROLE
{
    WluiAuthRoleHost = 1,
    WluiAuthRoleRecovery = 2
} WLUI_AUTH_ROLE;

/* One per interface function; the role matrix below says who may call what. */
typedef enum _WLUI_AUTH_OP
{
    WluiOpReady = 0,
    WluiOpHeartbeat,
    WluiOpWaitForCommand,
    WluiOpSubmitSerialization,
    WluiOpRequestAction,
    WluiOpReportProviderFault,
    WluiOpExiting,
    WluiOpCount
} WLUI_AUTH_OP;

typedef enum _WLUI_AUTH_RESULT
{
    WluiAuthOk = 0,
    WluiAuthUnknownLaunch,   /* no live entry has this LaunchId */
    WluiAuthPidMismatch,     /* the caller is not the process winlogon launched */
    WluiAuthSidMismatch,     /* the caller does not run as the user winlogon launched it as */
    WluiAuthRoleDenied       /* the entry's role may not perform this operation */
} WLUI_AUTH_RESULT;

#define WLUI_MAX_LAUNCHES 4

typedef struct _WLUI_LAUNCH_ENTRY
{
    BOOL InUse;
    GUID LaunchId;
    DWORD Pid;
    WLUI_AUTH_ROLE Role;
    BYTE Sid[SECURITY_MAX_SID_SIZE];
    /* Owned by winlogon's server; the table only stores them. */
    PVOID Context;
} WLUI_LAUNCH_ENTRY, *PWLUI_LAUNCH_ENTRY;

typedef struct _WLUI_LAUNCH_TABLE
{
    CRITICAL_SECTION Lock;
    WLUI_LAUNCH_ENTRY Entries[WLUI_MAX_LAUNCHES];
} WLUI_LAUNCH_TABLE, *PWLUI_LAUNCH_TABLE;

VOID NTAPI WluiLaunchTableInit(_Out_ PWLUI_LAUNCH_TABLE Table);
VOID NTAPI WluiLaunchTableDelete(_Inout_ PWLUI_LAUNCH_TABLE Table);

/* Records a launch winlogon just made; LaunchId is generated here and handed to the child. */
BOOL
NTAPI
WluiLaunchTableAdd(
    _Inout_ PWLUI_LAUNCH_TABLE Table,
    _In_ DWORD Pid,
    _In_ PSID Sid,
    _In_ WLUI_AUTH_ROLE Role,
    _In_opt_ PVOID Context,
    _Out_ GUID *LaunchId);

BOOL
NTAPI
WluiLaunchTableRemove(
    _Inout_ PWLUI_LAUNCH_TABLE Table,
    _In_ const GUID *LaunchId);

/*
 * Authorizes one call. On WluiAuthOk, *Entry points at the live entry (valid while
 * the table lock is not needed by the caller: entries are never moved, only cleared)
 * and *Role is the role winlogon assigned at launch.
 */
WLUI_AUTH_RESULT
NTAPI
WluiAuthorize(
    _Inout_ PWLUI_LAUNCH_TABLE Table,
    _In_ DWORD CallerPid,
    _In_ PSID CallerSid,
    _In_ const GUID *LaunchId,
    _In_ WLUI_AUTH_OP Op,
    _Out_opt_ WLUI_AUTH_ROLE *Role,
    _Out_opt_ PVOID *Context);

PCWSTR NTAPI WluiAuthResultName(_In_ WLUI_AUTH_RESULT Result);

#ifdef __cplusplus
}
#endif
