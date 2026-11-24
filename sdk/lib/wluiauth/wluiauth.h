/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Launch table and call authorizer for the wlui interface.
 *              Pure logic, no RPC: winlogon feeds it the caller's PID and SID.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#include <rpc.h>

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

/*
 * Per-launch state (wluistate.c): what winlogon keeps for one launched process.
 * Items in the command queue are opaque fixed-size blocks (the server stores
 * WLUI_COMMAND), so this library needs nothing from the IDL.
 */
#define WLUI_STATE_QUEUE_LENGTH 8

typedef struct _WLUI_LAUNCH_STATE
{
    LONG RefCount;
    BOOL Dead;
    GUID LaunchId;
    HANDLE ReadyEvent;
    HANDLE QueueSemaphore;
    CRITICAL_SECTION Lock;
    BOOL Ready;
    BOOL Exiting;
    BOOL Waiting;            /* one outstanding wait per launch */
    DWORD ExitReason;
    ULONGLONG LastHeartbeat;
    GUID LastFaultClsid;
    DWORD LastFaultReason;
    LONG SequenceCounter;
    SIZE_T ItemSize;
    ULONG Head;
    ULONG Count;
    PBYTE Queue;             /* WLUI_STATE_QUEUE_LENGTH * ItemSize bytes */
} WLUI_LAUNCH_STATE, *PWLUI_LAUNCH_STATE;

PWLUI_LAUNCH_STATE NTAPI WluiStateCreate(_In_ SIZE_T ItemSize);
VOID NTAPI WluiStateAddRef(_In_ PWLUI_LAUNCH_STATE State);
VOID NTAPI WluiStateRelease(_In_ PWLUI_LAUNCH_STATE State);
/* Marks the launch over and wakes a blocked wait; the caller drops its reference afterwards. */
VOID NTAPI WluiStateMarkDead(_In_ PWLUI_LAUNCH_STATE State);

BOOL NTAPI WluiStatePost(_In_ PWLUI_LAUNCH_STATE State, _In_reads_bytes_(Size) const VOID *Item, _In_ SIZE_T Size);
/*
 * Blocks up to TimeoutMs for an item. ERROR_SUCCESS with *Got FALSE on timeout,
 * ERROR_BUSY when another wait is outstanding, ERROR_OPERATION_ABORTED when the
 * launch died while waiting.
 */
DWORD NTAPI WluiStateWait(_In_ PWLUI_LAUNCH_STATE State, _In_ DWORD TimeoutMs,
                          _Out_writes_bytes_(Size) VOID *Item, _In_ SIZE_T Size, _Out_ PBOOL Got);

VOID NTAPI WluiStateSetReady(_In_ PWLUI_LAUNCH_STATE State);
BOOL NTAPI WluiStateWaitReady(_In_ PWLUI_LAUNCH_STATE State, _In_ DWORD TimeoutMs);
VOID NTAPI WluiStateTouch(_In_ PWLUI_LAUNCH_STATE State);
ULONGLONG NTAPI WluiStateLastHeartbeat(_In_ PWLUI_LAUNCH_STATE State);
VOID NTAPI WluiStateSetExiting(_In_ PWLUI_LAUNCH_STATE State, _In_ DWORD Reason);
BOOL NTAPI WluiStateExited(_In_ PWLUI_LAUNCH_STATE State, _Out_opt_ PDWORD Reason);
VOID NTAPI WluiStateRecordFault(_In_ PWLUI_LAUNCH_STATE State, _In_ const GUID *Clsid, _In_ DWORD Reason);

/* Interface callback for RpcServerRegisterIfEx: accepts a call only when it arrived over ncalrpc. */
RPC_STATUS RPC_ENTRY WluiServerIfCallback(_In_ RPC_IF_HANDLE Interface, _In_ void *Context);

#ifdef __cplusplus
}
#endif
