/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Per-launch state for the wlui server (see wluiauth.h).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windef.h>
#include <winbase.h>
#include <rpc.h>

#include "wluiauth.h"

PWLUI_LAUNCH_STATE
NTAPI
WluiStateCreate(
    _In_ SIZE_T ItemSize)
{
    PWLUI_LAUNCH_STATE State;

    if (ItemSize == 0 || ItemSize > 65536)
        return NULL;

    State = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*State));
    if (State == NULL)
        return NULL;

    State->RefCount = 1;
    State->ItemSize = ItemSize;
    State->LastHeartbeat = GetTickCount64();
    InitializeCriticalSection(&State->Lock);
    State->Queue = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ItemSize * WLUI_STATE_QUEUE_LENGTH);
    State->ReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    State->ShownEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    /* One extra count so MarkDead can always wake a waiter even with a full queue. */
    State->QueueSemaphore = CreateSemaphoreW(NULL, 0, WLUI_STATE_QUEUE_LENGTH + 1, NULL);
    if (State->Queue == NULL || State->ReadyEvent == NULL || State->ShownEvent == NULL || State->QueueSemaphore == NULL)
    {
        WluiStateRelease(State);
        return NULL;
    }
    return State;
}

VOID
NTAPI
WluiStateAddRef(
    _In_ PWLUI_LAUNCH_STATE State)
{
    InterlockedIncrement(&State->RefCount);
}

VOID
NTAPI
WluiStateRelease(
    _In_ PWLUI_LAUNCH_STATE State)
{
    if (InterlockedDecrement(&State->RefCount) != 0)
        return;

    DeleteCriticalSection(&State->Lock);
    if (State->ReadyEvent) CloseHandle(State->ReadyEvent);
    if (State->ShownEvent) CloseHandle(State->ShownEvent);
    if (State->QueueSemaphore) CloseHandle(State->QueueSemaphore);
    if (State->Queue)
    {
        SecureZeroMemory(State->Queue, State->ItemSize * WLUI_STATE_QUEUE_LENGTH);
        HeapFree(GetProcessHeap(), 0, State->Queue);
    }
    SecureZeroMemory(State, sizeof(*State));
    HeapFree(GetProcessHeap(), 0, State);
}

VOID
NTAPI
WluiStateMarkDead(
    _In_ PWLUI_LAUNCH_STATE State)
{
    EnterCriticalSection(&State->Lock);
    State->Dead = TRUE;
    LeaveCriticalSection(&State->Lock);
    ReleaseSemaphore(State->QueueSemaphore, 1, NULL);
    SetEvent(State->ReadyEvent);
    SetEvent(State->ShownEvent);
}

BOOL
NTAPI
WluiStatePost(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_reads_bytes_(Size) const VOID *Item,
    _In_ SIZE_T Size)
{
    BOOL Queued = FALSE;

    if (Item == NULL || Size != State->ItemSize)
        return FALSE;

    EnterCriticalSection(&State->Lock);
    if (!State->Dead && State->Count < WLUI_STATE_QUEUE_LENGTH)
    {
        ULONG Slot = (State->Head + State->Count) % WLUI_STATE_QUEUE_LENGTH;
        CopyMemory(State->Queue + Slot * State->ItemSize, Item, Size);
        State->Count++;
        Queued = TRUE;
    }
    LeaveCriticalSection(&State->Lock);

    if (Queued)
        ReleaseSemaphore(State->QueueSemaphore, 1, NULL);
    return Queued;
}

DWORD
NTAPI
WluiStateWait(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ DWORD TimeoutMs,
    _Out_writes_bytes_(Size) VOID *Item,
    _In_ SIZE_T Size,
    _Out_ PBOOL Got)
{
    DWORD Error = ERROR_SUCCESS;

    *Got = FALSE;
    if (Item == NULL || Size != State->ItemSize)
        return ERROR_INVALID_PARAMETER;
    ZeroMemory(Item, Size);

    EnterCriticalSection(&State->Lock);
    if (State->Dead)
        Error = ERROR_OPERATION_ABORTED;
    else if (State->Waiting)
        Error = ERROR_BUSY;
    else
        State->Waiting = TRUE;
    LeaveCriticalSection(&State->Lock);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (WaitForSingleObject(State->QueueSemaphore, TimeoutMs) == WAIT_OBJECT_0)
    {
        EnterCriticalSection(&State->Lock);
        if (State->Dead)
        {
            Error = ERROR_OPERATION_ABORTED;
        }
        else if (State->Count > 0)
        {
            PBYTE Slot = State->Queue + State->Head * State->ItemSize;
            CopyMemory(Item, Slot, Size);
            SecureZeroMemory(Slot, Size);
            State->Head = (State->Head + 1) % WLUI_STATE_QUEUE_LENGTH;
            State->Count--;
            *Got = TRUE;
        }
        State->Waiting = FALSE;
        LeaveCriticalSection(&State->Lock);
    }
    else
    {
        EnterCriticalSection(&State->Lock);
        State->Waiting = FALSE;
        LeaveCriticalSection(&State->Lock);
    }
    return Error;
}

VOID
NTAPI
WluiStateSetReady(
    _In_ PWLUI_LAUNCH_STATE State)
{
    EnterCriticalSection(&State->Lock);
    State->Ready = TRUE;
    State->LastHeartbeat = GetTickCount64();
    LeaveCriticalSection(&State->Lock);
    SetEvent(State->ReadyEvent);
}

BOOL
NTAPI
WluiStateSetShown(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ DWORD Sequence)
{
    if (Sequence == 0 || Sequence > (DWORD)InterlockedCompareExchange(&State->SequenceCounter, 0, 0))
        return FALSE;
    EnterCriticalSection(&State->Lock);
    if (Sequence > State->ShownSequence)
        State->ShownSequence = Sequence;
    LeaveCriticalSection(&State->Lock);
    SetEvent(State->ShownEvent);
    return TRUE;
}

BOOL
NTAPI
WluiStateWaitShown(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ DWORD Sequence,
    _In_ DWORD TimeoutMs)
{
    ULONGLONG End = GetTickCount64() + TimeoutMs;
    BOOL Shown, Dead;

    for (;;)
    {
        ULONGLONG Now = GetTickCount64();

        EnterCriticalSection(&State->Lock);
        Shown = State->ShownSequence >= Sequence;
        Dead = State->Dead;
        LeaveCriticalSection(&State->Lock);
        if (Shown || Dead || Now >= End)
            return Shown && !Dead;
        WaitForSingleObject(State->ShownEvent, (DWORD)(End - Now));
    }
}

BOOL
NTAPI
WluiStateWaitReady(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ DWORD TimeoutMs)
{
    BOOL Ready;

    if (WaitForSingleObject(State->ReadyEvent, TimeoutMs) != WAIT_OBJECT_0)
        return FALSE;
    EnterCriticalSection(&State->Lock);
    Ready = State->Ready && !State->Dead;
    LeaveCriticalSection(&State->Lock);
    return Ready;
}

VOID
NTAPI
WluiStateTouch(
    _In_ PWLUI_LAUNCH_STATE State)
{
    EnterCriticalSection(&State->Lock);
    State->LastHeartbeat = GetTickCount64();
    LeaveCriticalSection(&State->Lock);
}

ULONGLONG
NTAPI
WluiStateLastHeartbeat(
    _In_ PWLUI_LAUNCH_STATE State)
{
    ULONGLONG Last;
    EnterCriticalSection(&State->Lock);
    Last = State->LastHeartbeat;
    LeaveCriticalSection(&State->Lock);
    return Last;
}

VOID
NTAPI
WluiStateSetExiting(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ DWORD Reason)
{
    EnterCriticalSection(&State->Lock);
    State->Exiting = TRUE;
    State->ExitReason = Reason;
    LeaveCriticalSection(&State->Lock);
}

BOOL
NTAPI
WluiStateExited(
    _In_ PWLUI_LAUNCH_STATE State,
    _Out_opt_ PDWORD Reason)
{
    BOOL Exiting;
    EnterCriticalSection(&State->Lock);
    Exiting = State->Exiting;
    if (Reason) *Reason = State->ExitReason;
    LeaveCriticalSection(&State->Lock);
    return Exiting;
}

VOID
NTAPI
WluiStateRecordFault(
    _In_ PWLUI_LAUNCH_STATE State,
    _In_ const GUID *Clsid,
    _In_ DWORD Reason)
{
    EnterCriticalSection(&State->Lock);
    State->LastFaultClsid = *Clsid;
    State->LastFaultReason = Reason;
    LeaveCriticalSection(&State->Lock);
}

/* An interface is callable on every protocol sequence the process listens on;
 * wlui must stay on the local, SYSTEM-only endpoint, so refuse any other transport. */
RPC_STATUS
RPC_ENTRY
WluiServerIfCallback(
    _In_ RPC_IF_HANDLE Interface,
    _In_ void *Context)
{
    RPC_WSTR Binding = NULL;
    BOOL Local = FALSE;

    UNREFERENCED_PARAMETER(Interface);

    if (RpcBindingToStringBindingW((RPC_BINDING_HANDLE)Context, &Binding) == RPC_S_OK && Binding != NULL)
    {
        Local = (_wcsnicmp((PCWSTR)Binding, L"ncalrpc:", 8) == 0);
        RpcStringFreeW(&Binding);
    }
    return Local ? RPC_S_OK : RPC_S_ACCESS_DENIED;
}
