/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite processor group API on a single group
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

static
VOID
TestGroupCounts(
    _Out_ PULONG ActiveCount)
{
    USHORT ActiveGroups;
    USHORT MaximumGroups;
    ULONG AllGroups;
    ULONG Group0;
    ULONG Maximum0;

    ActiveGroups = KeQueryActiveGroupCount();
    MaximumGroups = KeQueryMaximumGroupCount();
    ok_eq_uint(ActiveGroups, 1);
    ok(MaximumGroups >= 1, "KeQueryMaximumGroupCount returned %u\n", MaximumGroups);

    AllGroups = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    Group0 = KeQueryActiveProcessorCountEx(0);
    ok_eq_ulong(AllGroups, Group0);
    ok_eq_ulong(Group0, (ULONG)KeNumberProcessors);

    Maximum0 = KeQueryMaximumProcessorCountEx(0);
    ok(Maximum0 >= Group0,
       "KeQueryMaximumProcessorCountEx(0) %lu < active count %lu\n", Maximum0, Group0);
    ok(Maximum0 <= sizeof(KAFFINITY) * 8,
       "KeQueryMaximumProcessorCountEx(0) %lu exceeds one KAFFINITY\n", Maximum0);

    *ActiveCount = Group0;
}

static
VOID
TestIndexNumberConversion(
    _In_ ULONG ActiveCount)
{
    PROCESSOR_NUMBER Number;
    NTSTATUS Status;
    ULONG Index;

    for (Index = 0; Index < ActiveCount; Index++)
    {
        RtlZeroMemory(&Number, sizeof(Number));
        Number.Group = 0xFFFF;
        Status = KeGetProcessorNumberFromIndex(Index, &Number);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            continue;

        ok_eq_uint(Number.Group, 0);
        ok_eq_ulong(KeGetProcessorIndexFromNumber(&Number), Index);
    }

    /* A group that does not exist has no processor index */
    RtlZeroMemory(&Number, sizeof(Number));
    Number.Group = 1;
    Number.Number = 0;
    ok_eq_ulong(KeGetProcessorIndexFromNumber(&Number), (ULONG)INVALID_PROCESSOR_INDEX);
}

static
VOID
TestGroupAffinity(VOID)
{
    KAFFINITY GroupAffinity;
    KAFFINITY ActiveAffinity;

    GroupAffinity = KeQueryGroupAffinity(0);
    ActiveAffinity = KeQueryActiveProcessors();
    ok_eq_ulongptr(GroupAffinity, ActiveAffinity);
}

static
VOID
TestCurrentProcessor(
    _In_ ULONG ActiveCount)
{
    PROCESSOR_NUMBER Number;
    KIRQL OldIrql;
    ULONG IndexEx;
    ULONG Current;

    RtlZeroMemory(&Number, sizeof(Number));
    Number.Group = 0xFFFF;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    IndexEx = KeGetCurrentProcessorNumberEx(&Number);
    Current = KeGetCurrentProcessorNumber();
    KeLowerIrql(OldIrql);

    ok(IndexEx < ActiveCount,
       "KeGetCurrentProcessorNumberEx returned %lu, active count %lu\n", IndexEx, ActiveCount);
    ok_eq_uint(Number.Group, 0);
    ok_eq_ulong(IndexEx, Current);
}

START_TEST(KeGroups)
{
    ULONG ActiveCount = 0;

    TestGroupCounts(&ActiveCount);
    TestIndexNumberConversion(ActiveCount);
    TestGroupAffinity();
    TestCurrentProcessor(ActiveCount);
}
