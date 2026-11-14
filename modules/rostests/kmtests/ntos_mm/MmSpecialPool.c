/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Special pool: placement, guard pages, delayed reuse, and deliberate faults for the lab
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>

#define BAIT_TAG        'tBpS'
#define OTHER_TAG       'tOpS'
#define BAIT_ALIGNMENT  16

#define END_OF(Block, Size) \
    ((ULONG_PTR)(Block) + (((Size) + BAIT_ALIGNMENT - 1) & ~(SIZE_T)(BAIT_ALIGNMENT - 1)))

static const SIZE_T ProbeSizes[] = { 24, 100, 1000 };

static
BOOLEAN
EveryTagIsSpecial(VOID)
{
    return (BOOLEAN)(MmIsDriverVerifying(KmtDriverObject) != FALSE);
}

static
BOOLEAN
EndsAtPageEnd(
    _In_ ULONG Tag)
{
    BOOLEAN AtEnd = TRUE;
    PVOID Block;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(ProbeSizes); i++)
    {
        Block = ExAllocatePoolWithTag(NonPagedPoolNx, ProbeSizes[i], Tag);
        if (Block == NULL)
            return FALSE;

        if (END_OF(Block, ProbeSizes[i]) & (PAGE_SIZE - 1))
            AtEnd = FALSE;

        ExFreePoolWithTag(Block, Tag);
    }

    return AtEnd;
}

static
BOOLEAN
BaitIsArmed(VOID)
{
    if (EndsAtPageEnd(BAIT_TAG))
        return TRUE;

    trace("Special pool is not active for tag SpBt. Set PoolTag (REG_DWORD) to 0x%08lx, or VerifyDrivers\n"
          "(REG_SZ) to kmtest_drv.sys, under\n"
          "HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management and restart.\n",
          (ULONG)BAIT_TAG);
    return FALSE;
}

static
VOID
TestPlacement(VOID)
{
    static const SIZE_T Sizes[] = { 1, 8, 15, 16, 17, 100, 1000, PAGE_SIZE - 16, PAGE_SIZE - 1 };
    static const POOL_TYPE Types[] = { NonPagedPoolNx, NonPagedPool, PagedPool };
    BOOLEAN QuotaCharged;
    PUCHAR Block, Page;
    SIZE_T Size, Queried, Byte, Wrong;
    ULONG i, t;

    for (t = 0; t < RTL_NUMBER_OF(Types); t++)
    {
        for (i = 0; i < RTL_NUMBER_OF(Sizes); i++)
        {
            Size = Sizes[i];
            Block = ExAllocatePoolWithTag(Types[t], Size, BAIT_TAG);
            ok(Block != NULL, "type %d size %Iu: no block\n", Types[t], Size);
            if (Block == NULL)
                continue;

            Page = (PUCHAR)PAGE_ALIGN(Block);
            ok(((ULONG_PTR)Block & (BAIT_ALIGNMENT - 1)) == 0, "type %d size %Iu: %p is not aligned\n",
               Types[t], Size, Block);
            ok((END_OF(Block, Size) & (PAGE_SIZE - 1)) == 0, "type %d size %Iu: %p does not end at the page end\n",
               Types[t], Size, Block);
            ok(MmIsAddressValid(Block), "type %d size %Iu: the block is not mapped\n", Types[t], Size);
            ok(!MmIsAddressValid(Page + PAGE_SIZE), "type %d size %Iu: the page after the block is mapped\n",
               Types[t], Size);
            ok(!MmIsAddressValid(Page - 1), "type %d size %Iu: the page before the block is mapped\n",
               Types[t], Size);

            RtlFillMemory(Block, Size, 0x5A);
            Wrong = 0;
            for (Byte = 0; Byte < Size; Byte++)
            {
                if (Block[Byte] != 0x5A)
                    Wrong++;
            }
            ok(Wrong == 0, "type %d size %Iu: %Iu bytes did not hold their value\n", Types[t], Size, Wrong);

            QuotaCharged = TRUE;
            Queried = ExQueryPoolBlockSize(Block, &QuotaCharged);
            ok(Queried == Size, "type %d size %Iu: ExQueryPoolBlockSize says %Iu\n", Types[t], Size, Queried);
            ok(!QuotaCharged, "type %d size %Iu: quota reported as charged\n", Types[t], Size);

            ExFreePoolWithTag(Block, BAIT_TAG);
            ok(!MmIsAddressValid(Block), "type %d size %Iu: the block is still mapped after the free\n",
               Types[t], Size);
        }
    }

    Block = ExAllocatePoolWithTag(NonPagedPoolNx, PAGE_SIZE, BAIT_TAG);
    ok(Block != NULL, "a whole page with the tag was refused\n");
    if (Block != NULL)
    {
        RtlFillMemory(Block, PAGE_SIZE, 0x33);
        ExFreePoolWithTag(Block, BAIT_TAG);
    }
}

static
VOID
TestSeparatePagesAndReuse(VOID)
{
    PUCHAR Held[32];
    PUCHAR First, Again;
    ULONG i, j, Shared = 0, Early = 0;

    RtlZeroMemory(Held, sizeof(Held));
    for (i = 0; i < RTL_NUMBER_OF(Held); i++)
    {
        Held[i] = ExAllocatePoolWithTag(NonPagedPoolNx, 64, BAIT_TAG);
        if (Held[i] != NULL)
            RtlFillMemory(Held[i], 64, (UCHAR)i);
    }

    for (i = 0; i < RTL_NUMBER_OF(Held); i++)
    {
        for (j = i + 1; j < RTL_NUMBER_OF(Held); j++)
        {
            if (Held[i] != NULL && Held[j] != NULL && PAGE_ALIGN(Held[i]) == PAGE_ALIGN(Held[j]))
                Shared++;
        }
    }
    ok(Shared == 0, "%lu pairs of blocks share a page\n", Shared);

    for (i = 0; i < RTL_NUMBER_OF(Held); i++)
    {
        if (Held[i] == NULL)
            continue;

        ok(Held[i][0] == (UCHAR)i && Held[i][63] == (UCHAR)i, "block %lu lost its content\n", i);
        ExFreePoolWithTag(Held[i], BAIT_TAG);
    }

    First = ExAllocatePoolWithTag(NonPagedPoolNx, 64, BAIT_TAG);
    ok(First != NULL, "no block\n");
    if (First == NULL)
        return;

    ExFreePoolWithTag(First, BAIT_TAG);
    for (i = 0; i < 256; i++)
    {
        Again = ExAllocatePoolWithTag(NonPagedPoolNx, 64, BAIT_TAG);
        if (Again == NULL)
            break;

        if (Again == First)
            Early++;

        ExFreePoolWithTag(Again, BAIT_TAG);
    }
    ok(i == 256, "only %lu of 256 allocations succeeded\n", i);
    ok(Early == 0, "a freed address came back within 256 allocations\n");
}

static
VOID
TestChurn(VOID)
{
    PUCHAR Block;
    ULONG Seed = 0x1234567, Round, Failed = 0, Wrong = 0, Misplaced = 0;
    SIZE_T Size;

    for (Round = 0; Round < 20000; Round++)
    {
        Seed = Seed * 1103515245 + 12345;
        Size = 1 + ((Seed >> 8) % (PAGE_SIZE - 1));
        Block = ExAllocatePoolWithTag((Round & 1) ? PagedPool : NonPagedPoolNx, Size, BAIT_TAG);
        if (Block == NULL)
        {
            Failed++;
            continue;
        }

        if (END_OF(Block, Size) & (PAGE_SIZE - 1))
            Misplaced++;

        Block[0] = (UCHAR)Round;
        Block[Size - 1] = (UCHAR)Round;
        if (Block[0] != (UCHAR)Round || Block[Size - 1] != (UCHAR)Round)
            Wrong++;

        ExFreePoolWithTag(Block, BAIT_TAG);
    }

    ok(Failed == 0, "%lu of 20000 allocations failed\n", Failed);
    ok(Misplaced == 0, "%lu of 20000 blocks were not placed at a page end\n", Misplaced);
    ok(Wrong == 0, "%lu of 20000 blocks did not hold their value\n", Wrong);
}

START_TEST(MmSpecialPool)
{
    if (!EveryTagIsSpecial())
        ok(!EndsAtPageEnd(OTHER_TAG), "blocks with another tag are placed like special pool blocks\n");

    if (skip(BaitIsArmed(), "Special pool is not active for the test tag\n"))
        return;

    TestPlacement();
    TestSeparatePagesAndReuse();
    TestChurn();
    if (!EveryTagIsSpecial())
        ok(!EndsAtPageEnd(OTHER_TAG), "blocks with another tag are placed like special pool blocks\n");
}

static
PUCHAR
BaitAllocate(
    _In_ SIZE_T Size)
{
    PUCHAR Block;

    if (!BaitIsArmed())
        return NULL;

    Block = ExAllocatePoolWithTag(NonPagedPoolNx, Size, BAIT_TAG);
    if (Block != NULL)
        RtlFillMemory(Block, Size, 0xA5);

    return Block;
}

static
VOID
BaitSurvived(
    _In_ PCSTR What)
{
    ok(FALSE, "%s was not detected: the system is still running\n", What);
}

START_TEST(MmSpecialPoolOverrunWrite)
{
    volatile UCHAR *Block = BaitAllocate(48);

    if (skip(Block != NULL, "not armed\n"))
        return;

    trace("Writing one byte past a 48 byte block at %p; expect bug check 0xD6\n", Block);
    Block[48] = 0x11;
    BaitSurvived("A write past the end of a block");
}

START_TEST(MmSpecialPoolOverrunRead)
{
    volatile UCHAR *Block = BaitAllocate(48);
    volatile UCHAR Value;

    if (skip(Block != NULL, "not armed\n"))
        return;

    trace("Reading one byte past a 48 byte block at %p; expect bug check 0xD6\n", Block);
    Value = Block[48];
    BaitSurvived("A read past the end of a block");
    (VOID)Value;
}

START_TEST(MmSpecialPoolUnderrun)
{
    volatile UCHAR *Block = BaitAllocate(48);

    if (skip(Block != NULL, "not armed\n"))
        return;

    trace("Writing one byte before a 48 byte block at %p, then freeing it; expect bug check 0xC1 (0x23),\n"
          "or 0xD6 at the write when PoolTagOverruns is 0\n", Block);
    Block[-1] = 0x11;
    ExFreePoolWithTag((PVOID)Block, BAIT_TAG);
    BaitSurvived("A write before the start of a block");
}

START_TEST(MmSpecialPoolSlackWrite)
{
    volatile UCHAR *Block = BaitAllocate(40);

    if (skip(Block != NULL, "not armed\n"))
        return;

    trace("Writing one byte past a 40 byte block at %p, inside its alignment padding, then freeing it;\n"
          "expect bug check 0xC1 (0x24)\n", Block);
    Block[40] = 0x11;
    ExFreePoolWithTag((PVOID)Block, BAIT_TAG);
    BaitSurvived("A write into the padding after a block");
}

START_TEST(MmSpecialPoolReadAfterFree)
{
    volatile UCHAR *Block = BaitAllocate(48);
    volatile UCHAR Value;

    if (skip(Block != NULL, "not armed\n"))
        return;

    ExFreePoolWithTag((PVOID)Block, BAIT_TAG);
    trace("Reading a freed block at %p; expect bug check 0xD5 (0xCC when the reader is the kernel itself)\n",
          Block);
    Value = Block[0];
    BaitSurvived("A read of a freed block");
    (VOID)Value;
}

START_TEST(MmSpecialPoolWriteAfterFree)
{
    volatile UCHAR *Block = BaitAllocate(48);

    if (skip(Block != NULL, "not armed\n"))
        return;

    ExFreePoolWithTag((PVOID)Block, BAIT_TAG);
    trace("Writing a freed block at %p; expect bug check 0xD5 (0xCC when the writer is the kernel itself)\n",
          Block);
    Block[0] = 0x11;
    BaitSurvived("A write to a freed block");
}

START_TEST(MmSpecialPoolDoubleFree)
{
    volatile UCHAR *Block = BaitAllocate(48);

    if (skip(Block != NULL, "not armed\n"))
        return;

    ExFreePoolWithTag((PVOID)Block, BAIT_TAG);
    trace("Freeing %p a second time; expect bug check 0xC1 (0x20)\n", Block);
    ExFreePoolWithTag((PVOID)Block, BAIT_TAG);
    BaitSurvived("A second free of a block");
}

START_TEST(MmSpecialPoolControl)
{
    volatile UCHAR *Block;
    volatile UCHAR Value;

    if (skip(!EveryTagIsSpecial(), "The test driver is verified: it has no ordinary blocks\n"))
        return;

    ok(!EndsAtPageEnd(OTHER_TAG), "blocks with another tag are placed like special pool blocks\n");

    Block = ExAllocatePoolWithTag(NonPagedPoolNx, 40, OTHER_TAG);
    ok(Block != NULL, "no block\n");
    if (Block == NULL)
        return;

    Block[39] = 0x11;
    Value = Block[39];
    ok(Value == 0x11, "the last byte of an ordinary block did not hold its value\n");
    ExFreePoolWithTag((PVOID)Block, OTHER_TAG);
    trace("An ordinary block was allocated, used and freed without a bug check\n");
}

START_TEST(MmDriverVerifier)
{
    ULONG Flags = 0xFFFFFFFF;
    NTSTATUS Status;
    PUCHAR Block;

    Status = MmIsVerifierEnabled(&Flags);
    if (!EveryTagIsSpecial())
    {
        ok(Status == STATUS_SUCCESS || Status == STATUS_NOT_SUPPORTED, "MmIsVerifierEnabled returned 0x%08lx\n", Status);
        if (Status == STATUS_NOT_SUPPORTED)
            ok(Flags == 0, "no verifier, but the flags are 0x%lx\n", Flags);

        skip(FALSE, "kmtest_drv.sys is not verified (VerifyDrivers does not name it)\n");
        return;
    }

    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Flags & DRIVER_VERIFIER_SPECIAL_POOLING, "the driver is verified but the flags 0x%lx do not say special pool\n", Flags);
    ok((Flags & ~(DRIVER_VERIFIER_SPECIAL_POOLING | DRIVER_VERIFIER_TRACK_POOL_ALLOCATIONS)) == 0,
       "the flags 0x%lx claim options that do not exist\n", Flags);
    ok(EndsAtPageEnd(OTHER_TAG), "blocks of a verified driver are not placed at a page end\n");

    Block = ExAllocatePool(NonPagedPoolNx, 40);
    ok(Block != NULL && (END_OF(Block, 40) & (PAGE_SIZE - 1)) == 0, "ExAllocatePool: %p\n", Block);
    if (Block != NULL)
        ExFreePool(Block);

    Block = ExAllocatePoolWithQuotaTag(NonPagedPoolNx | POOL_QUOTA_FAIL_INSTEAD_OF_RAISE, 40, OTHER_TAG);
    ok(Block != NULL && (END_OF(Block, 40) & (PAGE_SIZE - 1)) == 0, "ExAllocatePoolWithQuotaTag: %p\n", Block);
    if (Block != NULL)
        ExFreePoolWithTag(Block, OTHER_TAG);

    Block = ExAllocatePoolWithTagPriority(NonPagedPoolNx, 40, OTHER_TAG, NormalPoolPrioritySpecialPoolUnderrun);
    ok(Block != NULL && ((ULONG_PTR)Block & (PAGE_SIZE - 1)) == 0, "underrun placement: %p\n", Block);
    if (Block != NULL)
        ExFreePoolWithTag(Block, OTHER_TAG);

    Block = ExAllocatePoolWithTagPriority(NonPagedPoolNx, 40, OTHER_TAG, NormalPoolPrioritySpecialPoolOverrun);
    ok(Block != NULL && (END_OF(Block, 40) & (PAGE_SIZE - 1)) == 0, "overrun placement: %p\n", Block);
    if (Block != NULL)
        ExFreePoolWithTag(Block, OTHER_TAG);

    Block = ExAllocatePoolWithTag(NonPagedPoolNx, PAGE_SIZE * 2, OTHER_TAG);
    ok(Block != NULL, "a two page block was refused\n");
    if (Block != NULL)
    {
        RtlFillMemory(Block, PAGE_SIZE * 2, 0x44);
        ExFreePoolWithTag(Block, OTHER_TAG);
    }
}

START_TEST(MmDriverVerifierLeak)
{
    ULONG Flags = 0;
    PVOID Block;

    if (skip(EveryTagIsSpecial() && NT_SUCCESS(MmIsVerifierEnabled(&Flags)) &&
             (Flags & DRIVER_VERIFIER_TRACK_POOL_ALLOCATIONS),
             "kmtest_drv.sys is not verified with pool tracking\n"))
    {
        return;
    }

    Block = ExAllocatePoolWithTag(NonPagedPoolNx, 64, 'kLpS');
    ok(Block != NULL, "no block\n");
    trace("Left %p allocated on purpose; unloading kmtest_drv.sys must now end in bug check 0xC4 (0x62)\n", Block);
}
