/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Launch table and call authorizer for the wlui interface.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windef.h>
#include <winbase.h>
#include <rpc.h>

#include "wluiauth.h"

/* Role matrix: which operations each role may perform. Everything else is denied. */
static const BOOL Allowed[3][WluiOpCount] =
{
    /* (unused)  */ { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE },
    /* Host      */ { TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE  },
    /* Recovery  */ { TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  FALSE, TRUE,  FALSE }
};

VOID
NTAPI
WluiLaunchTableInit(
    _Out_ PWLUI_LAUNCH_TABLE Table)
{
    ZeroMemory(Table, sizeof(*Table));
    InitializeCriticalSection(&Table->Lock);
}

VOID
NTAPI
WluiLaunchTableDelete(
    _Inout_ PWLUI_LAUNCH_TABLE Table)
{
    DeleteCriticalSection(&Table->Lock);
    SecureZeroMemory(Table, sizeof(*Table));
}

static PWLUI_LAUNCH_ENTRY
WluiFindLocked(
    _In_ PWLUI_LAUNCH_TABLE Table,
    _In_ const GUID *LaunchId)
{
    ULONG i;

    for (i = 0; i < WLUI_MAX_LAUNCHES; i++)
    {
        if (Table->Entries[i].InUse && IsEqualGUID(&Table->Entries[i].LaunchId, LaunchId))
            return &Table->Entries[i];
    }
    return NULL;
}

BOOL
NTAPI
WluiLaunchTableAdd(
    _Inout_ PWLUI_LAUNCH_TABLE Table,
    _In_ DWORD Pid,
    _In_ PSID Sid,
    _In_ WLUI_AUTH_ROLE Role,
    _In_opt_ PVOID Context,
    _In_opt_ const GUID *RequestedId,
    _Out_ GUID *LaunchId)
{
    PWLUI_LAUNCH_ENTRY Entry = NULL;
    static const GUID Null = { 0 };
    DWORD SidLength;
    ULONG i;

    ZeroMemory(LaunchId, sizeof(*LaunchId));

    if (Pid == 0 || Sid == NULL || !IsValidSid(Sid) ||
        (Role != WluiAuthRoleHost && Role != WluiAuthRoleRecovery))
    {
        return FALSE;
    }

    SidLength = GetLengthSid(Sid);
    if (SidLength > SECURITY_MAX_SID_SIZE)
        return FALSE;

    if (RequestedId != NULL)
    {
        if (IsEqualGUID(RequestedId, &Null))
            return FALSE;
        *LaunchId = *RequestedId;
    }
    /* rpcrt4, not ole32: winlogon already links it and the id only needs to be unique locally. */
    else if (UuidCreate(LaunchId) != RPC_S_OK)
        return FALSE;

    EnterCriticalSection(&Table->Lock);
    /* A requested id must not collide with a live entry. */
    if (WluiFindLocked(Table, LaunchId) != NULL)
    {
        LeaveCriticalSection(&Table->Lock);
        ZeroMemory(LaunchId, sizeof(*LaunchId));
        return FALSE;
    }
    for (i = 0; i < WLUI_MAX_LAUNCHES; i++)
    {
        if (!Table->Entries[i].InUse)
        {
            Entry = &Table->Entries[i];
            break;
        }
    }
    if (Entry != NULL)
    {
        ZeroMemory(Entry, sizeof(*Entry));
        Entry->LaunchId = *LaunchId;
        Entry->Pid = Pid;
        Entry->Role = Role;
        Entry->Context = Context;
        CopyMemory(Entry->Sid, Sid, SidLength);
        Entry->InUse = TRUE;
    }
    LeaveCriticalSection(&Table->Lock);

    if (Entry == NULL)
        ZeroMemory(LaunchId, sizeof(*LaunchId));
    return Entry != NULL;
}

BOOL
NTAPI
WluiLaunchTableRemove(
    _Inout_ PWLUI_LAUNCH_TABLE Table,
    _In_ const GUID *LaunchId)
{
    PWLUI_LAUNCH_ENTRY Entry;

    EnterCriticalSection(&Table->Lock);
    Entry = WluiFindLocked(Table, LaunchId);
    if (Entry != NULL)
        SecureZeroMemory(Entry, sizeof(*Entry));
    LeaveCriticalSection(&Table->Lock);

    return Entry != NULL;
}

WLUI_AUTH_RESULT
NTAPI
WluiAuthorize(
    _Inout_ PWLUI_LAUNCH_TABLE Table,
    _In_ DWORD CallerPid,
    _In_ PSID CallerSid,
    _In_ const GUID *LaunchId,
    _In_ WLUI_AUTH_OP Op,
    _Out_opt_ WLUI_AUTH_ROLE *Role,
    _Out_opt_ PVOID *Context)
{
    PWLUI_LAUNCH_ENTRY Entry;
    WLUI_AUTH_RESULT Result;

    if (Role) *Role = 0;
    if (Context) *Context = NULL;

    if (CallerSid == NULL || !IsValidSid(CallerSid) || LaunchId == NULL || Op >= WluiOpCount)
        return WluiAuthUnknownLaunch;

    EnterCriticalSection(&Table->Lock);
    Entry = WluiFindLocked(Table, LaunchId);
    if (Entry == NULL)
        Result = WluiAuthUnknownLaunch;
    else if (Entry->Pid != CallerPid)
        Result = WluiAuthPidMismatch;
    else if (!EqualSid((PSID)Entry->Sid, CallerSid))
        Result = WluiAuthSidMismatch;
    else if (!Allowed[Entry->Role][Op])
        Result = WluiAuthRoleDenied;
    else
    {
        Result = WluiAuthOk;
        if (Role) *Role = Entry->Role;
        if (Context) *Context = Entry->Context;
    }
    LeaveCriticalSection(&Table->Lock);

    return Result;
}

PCWSTR
NTAPI
WluiAuthResultName(
    _In_ WLUI_AUTH_RESULT Result)
{
    switch (Result)
    {
        case WluiAuthOk:            return L"ok";
        case WluiAuthUnknownLaunch: return L"unknown-launch";
        case WluiAuthPidMismatch:   return L"pid-mismatch";
        case WluiAuthSidMismatch:   return L"sid-mismatch";
        case WluiAuthRoleDenied:    return L"role-denied";
        default:                    return L"invalid";
    }
}
