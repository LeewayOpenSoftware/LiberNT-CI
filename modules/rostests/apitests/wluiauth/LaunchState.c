/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the wlui per-launch state (queue, single wait, death)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <wluiauth.h>

typedef struct { BYTE Bytes[16]; } ITEM;

static PWLUI_LAUNCH_STATE g_Shared;
static DWORD g_ThreadResult;
static BOOL g_ThreadGot;

static DWORD WINAPI
WaiterThread(LPVOID Param)
{
    ITEM Item;
    g_ThreadResult = WluiStateWait(g_Shared, (DWORD)(ULONG_PTR)Param, &Item, sizeof(Item), &g_ThreadGot);
    return 0;
}

START_TEST(LaunchState)
{
    PWLUI_LAUNCH_STATE S;
    ITEM In, Out;
    BOOL Got;
    HANDLE Thread;
    ULONG i;

    ok(WluiStateCreate(0) == NULL, "zero item size accepted\n");
    S = WluiStateCreate(sizeof(ITEM));
    ok(S != NULL, "create\n");
    if (S == NULL)
        return;

    FillMemory(&In, sizeof(In), 0xA5);

    /* Empty queue: a short wait times out with success and nothing delivered. */
    ok(WluiStateWait(S, 0, &Out, sizeof(Out), &Got) == ERROR_SUCCESS && !Got, "empty wait\n");
    ok(WluiStateWait(S, 0, &Out, sizeof(Out) - 1, &Got) == ERROR_INVALID_PARAMETER, "wrong size\n");
    ok(!WluiStatePost(S, &In, sizeof(In) + 1), "post wrong size\n");

    /* FIFO delivery, item bytes intact, slot wiped after delivery. */
    ok(WluiStatePost(S, &In, sizeof(In)), "post\n");
    ok(WluiStateWait(S, 0, &Out, sizeof(Out), &Got) == ERROR_SUCCESS && Got, "wait after post\n");
    ok(memcmp(&In, &Out, sizeof(In)) == 0, "item bytes\n");
    ok(S->Count == 0 && S->Queue[(S->Head + WLUI_STATE_QUEUE_LENGTH - 1) % WLUI_STATE_QUEUE_LENGTH * sizeof(ITEM)] == 0,
       "slot not wiped\n");

    /* Bounded: exactly WLUI_STATE_QUEUE_LENGTH items fit. */
    for (i = 0; i < WLUI_STATE_QUEUE_LENGTH; i++)
        ok(WluiStatePost(S, &In, sizeof(In)), "post %lu\n", i);
    ok(!WluiStatePost(S, &In, sizeof(In)), "queue not bounded\n");
    for (i = 0; i < WLUI_STATE_QUEUE_LENGTH; i++)
        ok(WluiStateWait(S, 0, &Out, sizeof(Out), &Got) == ERROR_SUCCESS && Got, "drain %lu\n", i);
    ok(WluiStateWait(S, 0, &Out, sizeof(Out), &Got) == ERROR_SUCCESS && !Got, "drained\n");

    /* R2: one outstanding wait per launch; a concurrent second one gets ERROR_BUSY. */
    g_Shared = S;
    Thread = CreateThread(NULL, 0, WaiterThread, (LPVOID)(ULONG_PTR)5000, 0, NULL);
    ok(Thread != NULL, "thread\n");
    Sleep(200);
    ok(WluiStateWait(S, 0, &Out, sizeof(Out), &Got) == ERROR_BUSY, "second wait not busy\n");
    ok(WluiStatePost(S, &In, sizeof(In)), "post to waiter\n");
    ok(WaitForSingleObject(Thread, 5000) == WAIT_OBJECT_0, "waiter did not return\n");
    ok(g_ThreadResult == ERROR_SUCCESS && g_ThreadGot, "waiter result %lu got %d\n", g_ThreadResult, g_ThreadGot);
    CloseHandle(Thread);
    ok(WluiStateWait(S, 0, &Out, sizeof(Out), &Got) == ERROR_SUCCESS && !Got, "wait free again\n");

    /* Death wakes a blocked wait with ERROR_OPERATION_ABORTED and ends posting. */
    Thread = CreateThread(NULL, 0, WaiterThread, (LPVOID)(ULONG_PTR)5000, 0, NULL);
    Sleep(200);
    WluiStateMarkDead(S);
    ok(WaitForSingleObject(Thread, 5000) == WAIT_OBJECT_0, "dead waiter did not return\n");
    ok(g_ThreadResult == ERROR_OPERATION_ABORTED, "dead waiter result %lu\n", g_ThreadResult);
    CloseHandle(Thread);
    ok(!WluiStatePost(S, &In, sizeof(In)), "post after death\n");
    ok(WluiStateWait(S, 0, &Out, sizeof(Out), &Got) == ERROR_OPERATION_ABORTED, "wait after death\n");
    ok(!WluiStateWaitReady(S, 0), "ready after death\n");

    /* Ready, heartbeat, exit and fault bookkeeping. */
    WluiStateRelease(S);
    S = WluiStateCreate(sizeof(ITEM));
    ok(!WluiStateWaitReady(S, 0), "ready before Ready\n");
    WluiStateSetReady(S);
    ok(WluiStateWaitReady(S, 0), "ready after Ready\n");
    {
        ULONGLONG Before = WluiStateLastHeartbeat(S);
        Sleep(20);
        WluiStateTouch(S);
        ok(WluiStateLastHeartbeat(S) >= Before, "heartbeat went backwards\n");
    }
    ok(!WluiStateExited(S, NULL), "exited before\n");
    WluiStateSetExiting(S, 2);
    {
        DWORD Reason = 0;
        ok(WluiStateExited(S, &Reason) && Reason == 2, "exit reason %lu\n", Reason);
    }
    WluiStateAddRef(S);
    WluiStateRelease(S);
    WluiStateRelease(S);
}
