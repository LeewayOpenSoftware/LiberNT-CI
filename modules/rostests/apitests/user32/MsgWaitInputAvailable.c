/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test that MsgWaitForMultipleObjects wakes only for new input, and MWMO_INPUTAVAILABLE for queued input
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#define TEST_MESSAGE (WM_APP + 0x51)

/* Remove every queued message of this thread; returns how many TEST_MESSAGEs were removed */
static UINT DrainQueue(VOID)
{
    MSG Msg;
    UINT Count = 0;

    while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
    {
        if (Msg.message == TEST_MESSAGE)
            Count++;
    }
    return Count;
}

START_TEST(MsgWaitInputAvailable)
{
    MSG Msg;
    DWORD Result;
    HANDLE Event;

    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(Event != NULL, "CreateEventW failed, error %lu\n", GetLastError());
    if (!Event)
        return;

    /* Make sure this thread has a message queue, and that it is empty */
    PeekMessageW(&Msg, NULL, 0, 0, PM_NOREMOVE);
    DrainQueue();

    /* New input: a message posted after the last look at the queue wakes the wait */
    ok(PostThreadMessageW(GetCurrentThreadId(), TEST_MESSAGE, 0, 0), "PostThreadMessageW failed, error %lu\n", GetLastError());
    Result = MsgWaitForMultipleObjects(1, &Event, FALSE, 0, QS_ALLINPUT);
    ok(Result == WAIT_OBJECT_0 + 1, "new input: Result = %lu, expected %lu\n", Result, (DWORD)(WAIT_OBJECT_0 + 1));

    /* Old input: once the thread has looked at the queue (PM_NOREMOVE leaves the message there),
     * a plain wait no longer wakes for it */
    ok(PeekMessageW(&Msg, NULL, 0, 0, PM_NOREMOVE), "the posted message is gone\n");
    Result = MsgWaitForMultipleObjects(1, &Event, FALSE, 0, QS_ALLINPUT);
    ok(Result == WAIT_TIMEOUT, "old input, plain wait: Result = %lu, expected WAIT_TIMEOUT\n", Result);

    /* MWMO_INPUTAVAILABLE wakes for input that is in the queue, old or new */
    Result = MsgWaitForMultipleObjectsEx(1, &Event, 0, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    ok(Result == WAIT_OBJECT_0 + 1, "old input, MWMO_INPUTAVAILABLE: Result = %lu, expected %lu\n",
       Result, (DWORD)(WAIT_OBJECT_0 + 1));

    /* The same holds when the message was already queued before a message was retrieved:
     * retrieving the first one marks the second as seen */
    ok(PostThreadMessageW(GetCurrentThreadId(), TEST_MESSAGE, 0, 0), "PostThreadMessageW failed, error %lu\n", GetLastError());
    ok(PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE) && Msg.message == TEST_MESSAGE, "first message not retrieved\n");
    Result = MsgWaitForMultipleObjects(1, &Event, FALSE, 0, QS_ALLINPUT);
    ok(Result == WAIT_TIMEOUT, "second queued message, plain wait: Result = %lu, expected WAIT_TIMEOUT\n", Result);
    Result = MsgWaitForMultipleObjectsEx(1, &Event, 0, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    ok(Result == WAIT_OBJECT_0 + 1, "second queued message, MWMO_INPUTAVAILABLE: Result = %lu, expected %lu\n",
       Result, (DWORD)(WAIT_OBJECT_0 + 1));
    ok(DrainQueue() == 1, "expected the second message still queued\n");

    /* An empty queue does not wake it. With input queued and the object signalled, MWMO_INPUTAVAILABLE
     * reports the input (as Windows 11 26100 does), so a caller drains its queue before it sees the object */
    Result = MsgWaitForMultipleObjectsEx(1, &Event, 0, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    ok(Result == WAIT_TIMEOUT, "empty queue, MWMO_INPUTAVAILABLE: Result = %lu, expected WAIT_TIMEOUT\n", Result);
    ok(PostThreadMessageW(GetCurrentThreadId(), TEST_MESSAGE, 0, 0), "PostThreadMessageW failed, error %lu\n", GetLastError());
    SetEvent(Event);
    Result = MsgWaitForMultipleObjectsEx(1, &Event, 0, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    ok(Result == WAIT_OBJECT_0 + 1, "signalled object and queued input: Result = %lu, expected %lu\n",
       Result, (DWORD)(WAIT_OBJECT_0 + 1));
    ok(DrainQueue() == 1, "expected one queued message\n");
    Result = MsgWaitForMultipleObjectsEx(1, &Event, 0, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    ok(Result == WAIT_OBJECT_0, "signalled object, empty queue: Result = %lu, expected WAIT_OBJECT_0\n", Result);

    CloseHandle(Event);
}
