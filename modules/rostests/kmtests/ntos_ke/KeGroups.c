/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite processor group API consistency
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define PROCESSORS_PER_GROUP ((ULONG)(sizeof(KAFFINITY) * 8))

static
VOID
TestGroupCounts(
    _Out_ PULONG ActiveCount,
    _Out_ PUSHORT ActiveGroups)
{
    USHORT MaximumGroups;
    USHORT Group;
    ULONG AllGroups;
    ULONG Sum;
    ULONG MaximumAll;
    ULONG MaximumSum;
    ULONG Maximum;

    *ActiveGroups = KeQueryActiveGroupCount();
    MaximumGroups = KeQueryMaximumGroupCount();
    ok(*ActiveGroups >= 1, "KeQueryActiveGroupCount returned %u\n", *ActiveGroups);
    ok(MaximumGroups >= *ActiveGroups,
       "maximum group count %u < active group count %u\n", MaximumGroups, *ActiveGroups);

    /* The active processors of every group add up to the system count */
    AllGroups = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    ok_eq_ulong(AllGroups, (ULONG)KeNumberProcessors);
    Sum = 0;
    for (Group = 0; Group < *ActiveGroups; Group++)
    {
        ULONG Count = KeQueryActiveProcessorCountEx(Group);

        ok(Count >= 1 && Count <= PROCESSORS_PER_GROUP,
           "group %u has %lu active processors\n", Group, Count);
        Sum += Count;
    }
    ok_eq_ulong(Sum, AllGroups);
    ok_eq_ulong(KeQueryActiveProcessorCountEx(MaximumGroups), 0UL);

    /* The legacy maximum describes group 0 and fits in one KAFFINITY */
    Maximum = KeQueryMaximumProcessorCount();
    ok_eq_ulong(Maximum, KeQueryMaximumProcessorCountEx(0));
    ok(Maximum <= PROCESSORS_PER_GROUP,
       "KeQueryMaximumProcessorCount %lu exceeds one KAFFINITY\n", Maximum);

    MaximumAll = KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS);
    ok(MaximumAll >= AllGroups,
       "maximum processor count %lu < active count %lu\n", MaximumAll, AllGroups);
    MaximumSum = 0;
    for (Group = 0; Group < MaximumGroups; Group++)
    {
        ULONG Count = KeQueryMaximumProcessorCountEx(Group);

        ok(Count <= PROCESSORS_PER_GROUP,
           "group %u maximum %lu exceeds one KAFFINITY\n", Group, Count);
        ok(Count >= KeQueryActiveProcessorCountEx(Group),
           "group %u maximum %lu < active count\n", Group, Count);
        MaximumSum += Count;
    }
    ok_eq_ulong(MaximumSum, MaximumAll);
    ok_eq_ulong(KeQueryMaximumProcessorCountEx(MaximumGroups), 0UL);

    *ActiveCount = AllGroups;
}

static
VOID
TestIndexNumberConversion(
    _In_ ULONG ActiveCount,
    _In_ USHORT ActiveGroups)
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

        ok(Number.Group < ActiveGroups, "index %lu is in group %u of %u\n",
           Index, Number.Group, ActiveGroups);
        ok(Number.Number < PROCESSORS_PER_GROUP, "index %lu has number %u\n",
           Index, Number.Number);
        ok_eq_uint(Number.Reserved, 0);
        ok(KeQueryGroupAffinity(Number.Group) & ((KAFFINITY)1 << Number.Number),
           "index %lu (%u:%u) is not in its group affinity\n",
           Index, Number.Group, Number.Number);
        ok_eq_ulong(KeGetProcessorIndexFromNumber(&Number), Index);
    }

    ok_eq_hex(KeGetProcessorNumberFromIndex(ActiveCount, &Number), STATUS_INVALID_PARAMETER);

    /* A group that is not active has no processor index */
    RtlZeroMemory(&Number, sizeof(Number));
    Number.Group = ActiveGroups;
    Number.Number = 0;
    ok_eq_ulong(KeGetProcessorIndexFromNumber(&Number), (ULONG)INVALID_PROCESSOR_INDEX);

    /* Nor does a number beyond one KAFFINITY */
    Number.Group = 0;
    Number.Number = (UCHAR)PROCESSORS_PER_GROUP;
    ok_eq_ulong(KeGetProcessorIndexFromNumber(&Number), (ULONG)INVALID_PROCESSOR_INDEX);
}

static
VOID
TestGroupAffinity(
    _In_ USHORT ActiveGroups)
{
    USHORT Group;

    ok_eq_ulongptr(KeQueryGroupAffinity(0), KeQueryActiveProcessors());
    ok_eq_ulongptr(KeQueryGroupAffinity(ActiveGroups), (KAFFINITY)0);

    /* Every active group has a mask that matches its active count */
    for (Group = 0; Group < ActiveGroups; Group++)
    {
        KAFFINITY Mask = KeQueryGroupAffinity(Group);
        ULONG Bits = 0;

        for (; Mask != 0; Mask &= Mask - 1)
            Bits++;
        ok(Bits != 0, "active group %u has an empty affinity\n", Group);
        ok_eq_ulong(Bits, KeQueryActiveProcessorCountEx(Group));
    }
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
    ULONG FromNumber;

    RtlZeroMemory(&Number, sizeof(Number));
    Number.Group = 0xFFFF;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    IndexEx = KeGetCurrentProcessorNumberEx(&Number);
    Current = KeGetCurrentProcessorNumber();
    FromNumber = KeGetProcessorIndexFromNumber(&Number);
    KeLowerIrql(OldIrql);

    ok(IndexEx < ActiveCount,
       "KeGetCurrentProcessorNumberEx returned %lu, active count %lu\n", IndexEx, ActiveCount);
    ok_eq_ulong(IndexEx, Current);
    ok_eq_ulong(FromNumber, IndexEx);
}

START_TEST(KeGroups)
{
    ULONG ActiveCount = 0;
    USHORT ActiveGroups = 1;

    TestGroupCounts(&ActiveCount, &ActiveGroups);
    TestIndexNumberConversion(ActiveCount, ActiveGroups);
    TestGroupAffinity(ActiveGroups);
    TestCurrentProcessor(ActiveCount);
}
