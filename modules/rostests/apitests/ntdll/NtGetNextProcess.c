/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     NtGetNextProcess walks the active process list both ways
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "precomp.h"

#ifndef PROCESS_GET_NEXT_FLAGS_PREVIOUS_PROCESS
#define PROCESS_GET_NEXT_FLAGS_PREVIOUS_PROCESS 0x00000001
#endif

static ULONG
WalkProcesses(ULONG Flags, PBOOLEAN FoundSelf)
{
    HANDLE Current = NULL, Next;
    ULONG Count = 0;
    NTSTATUS Status;

    *FoundSelf = FALSE;
    while (NT_SUCCESS(Status = NtGetNextProcess(Current, PROCESS_QUERY_LIMITED_INFORMATION, 0, Flags, &Next)))
    {
        if (GetProcessId(Next) == GetCurrentProcessId())
            *FoundSelf = TRUE;
        if (Current) NtClose(Current);
        Current = Next;
        if (++Count > 100000) break;
    }
    ok_hex(Status, STATUS_NO_MORE_ENTRIES);
    if (Current) NtClose(Current);
    return Count;
}

START_TEST(NtGetNextProcess)
{
    HANDLE Handle = (HANDLE)(ULONG_PTR)0xdeadbeef;
    BOOLEAN FoundSelf;
    ULONG Forward, Backward;
    NTSTATUS Status;

    Status = NtGetNextProcess(NULL, PROCESS_QUERY_LIMITED_INFORMATION, 0, 2, &Handle);
    ok_hex(Status, STATUS_INVALID_PARAMETER);
    ok(Handle == NULL, "Handle = %p, expected NULL\n", Handle);

    Status = NtGetNextProcess((HANDLE)(ULONG_PTR)0xdeadbeef, PROCESS_QUERY_LIMITED_INFORMATION, 0, 0, &Handle);
    ok_hex(Status, STATUS_INVALID_HANDLE);

    Forward = WalkProcesses(0, &FoundSelf);
    ok(Forward > 1, "Forward walk saw %lu processes\n", Forward);
    ok(FoundSelf, "Forward walk did not reach the current process\n");

    Backward = WalkProcesses(PROCESS_GET_NEXT_FLAGS_PREVIOUS_PROCESS, &FoundSelf);
    ok(Backward > 1, "Backward walk saw %lu processes\n", Backward);
    ok(FoundSelf, "Backward walk did not reach the current process\n");
}
