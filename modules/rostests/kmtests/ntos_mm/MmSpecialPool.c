/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Special pool: placement, guard pages, delayed reuse, and deliberate faults for the lab
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>

#define VF_TEST_DEADLOCK_DETECTION 0x0020

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
    ok((Flags & ~(DRIVER_VERIFIER_SPECIAL_POOLING | DRIVER_VERIFIER_TRACK_POOL_ALLOCATIONS |
                  DRIVER_VERIFIER_IO_CHECKING | VF_TEST_DEADLOCK_DETECTION)) == 0,
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

    {
        LARGE_INTEGER NoWait = { { 0, 0 } };
        KEVENT Event;
        KIRQL OldIrql;

        KeInitializeEvent(&Event, NotificationEvent, FALSE);
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        Status = KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, &NoWait);
        KeLowerIrql(OldIrql);
        ok_eq_hex(Status, STATUS_TIMEOUT);
    }

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

START_TEST(MmDriverVerifierSpinLockAtPassive)
{
    KSPIN_LOCK Lock;

    if (skip(EveryTagIsSpecial(), "kmtest_drv.sys is not verified\n"))
        return;

    KeInitializeSpinLock(&Lock);
    trace("KeAcquireSpinLockAtDpcLevel below DISPATCH_LEVEL; expect bug check 0xC4 (0x40)\n");
    KeAcquireSpinLockAtDpcLevel(&Lock);
    ok(FALSE, "the call was not detected\n");
    KeReleaseSpinLockFromDpcLevel(&Lock);
}

START_TEST(MmDriverVerifierPagedAtDispatch)
{
    KIRQL OldIrql;
    PVOID Block;

    if (skip(EveryTagIsSpecial(), "kmtest_drv.sys is not verified\n"))
        return;

    trace("Paged pool allocation at DISPATCH_LEVEL; expect bug check 0xC4 (0x01)\n");
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Block = ExAllocatePoolWithTag(PagedPool, 64, OTHER_TAG);
    KeLowerIrql(OldIrql);
    ok(FALSE, "the allocation was not detected\n");
    if (Block != NULL)
        ExFreePoolWithTag(Block, OTHER_TAG);
}

START_TEST(MmDriverVerifierWaitAtDispatch)
{
    KEVENT Event;
    KIRQL OldIrql;

    if (skip(EveryTagIsSpecial(), "kmtest_drv.sys is not verified\n"))
        return;

    KeInitializeEvent(&Event, NotificationEvent, TRUE);
    trace("KeWaitForSingleObject without a zero timeout at DISPATCH_LEVEL; expect bug check 0xC4 (0x3B)\n");
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    KeLowerIrql(OldIrql);
    ok(FALSE, "the wait was not detected\n");
}

START_TEST(MmDriverVerifierCompletePending)
{
    ULONG Flags = 0;
    PIRP Irp;

    if (skip(EveryTagIsSpecial() && NT_SUCCESS(MmIsVerifierEnabled(&Flags)) && (Flags & DRIVER_VERIFIER_IO_CHECKING),
             "kmtest_drv.sys is not verified with I/O verification (VerifyDriverLevel 0x19)\n"))
    {
        return;
    }

    Irp = IoAllocateIrp(1, FALSE);
    ok(Irp != NULL, "no IRP\n");
    if (Irp == NULL)
        return;

    Irp->IoStatus.Status = STATUS_PENDING;
    trace("Completing IRP %p with STATUS_PENDING; expect bug check 0xC9 (0x06)\n", Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    ok(FALSE, "the completion was not detected\n");
}

static PDEVICE_OBJECT VfLowerDevice;
static PDRIVER_DISPATCH VfSavedDispatch;
static PIRP VfHeldIrp;
static BOOLEAN VfLowerIsShared;

static
VOID
NTAPI
VfNeverCancel(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    IoReleaseCancelSpinLock(Irp->CancelIrql);
}

static
NTSTATUS
NTAPI
VfLowerDispatch(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    if (DeviceObject != VfLowerDevice)
        return VfSavedDispatch(DeviceObject, Irp);

    IoSetCancelRoutine(Irp, NULL);
    IoMarkIrpPending(Irp);
    if (!VfLowerIsShared)
        VfHeldIrp = Irp;

    return STATUS_PENDING;
}

static
NTSTATUS
NTAPI
VfStopCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(Context);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
VOID
VfSendIrpToHold(
    _In_ PIRP Irp,
    _In_ BOOLEAN WithCancelRoutine)
{
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;

    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoSetCompletionRoutine(Irp, VfStopCompletion, NULL, TRUE, TRUE, TRUE);
    if (WithCancelRoutine)
        IoSetCancelRoutine(Irp, VfNeverCancel);

    VfHeldIrp = NULL;
    Status = IoCallDriver(VfLowerDevice, Irp);
    ok_eq_hex(Status, STATUS_PENDING);
    ok(VfHeldIrp == Irp, "the lower device holds %p, sent %p\n", VfHeldIrp, Irp);
}

static
PIRP
VfSendHeldIrp(
    _In_ BOOLEAN WithCancelRoutine)
{
    PIRP Irp;

    Irp = IoAllocateIrp(VfLowerDevice->StackSize, FALSE);
    ok(Irp != NULL, "no IRP\n");
    if (Irp == NULL)
        return NULL;

    VfSendIrpToHold(Irp, WithCancelRoutine);
    return Irp;
}

static
VOID
VfCompleteHeldIrp(
    _In_ PIRP Irp)
{
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoFreeIrp(Irp);
}

START_TEST(MmDriverVerifierIrpTracking)
{
    PVOID *Slot = (PVOID *)&KmtDriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL];
    ULONG Flags = 0;
    NTSTATUS Status;
    PIRP Irp;

    if (skip(EveryTagIsSpecial() && NT_SUCCESS(MmIsVerifierEnabled(&Flags)) && (Flags & DRIVER_VERIFIER_IO_CHECKING),
             "kmtest_drv.sys is not verified with I/O verification (VerifyDriverLevel 0x19)\n"))
    {
        return;
    }

    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &VfLowerDevice);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    VfSavedDispatch = InterlockedExchangePointer(Slot, (PVOID)VfLowerDispatch);

    trace("Case 1: an IRP pends below and its owner completes it; expect NO verifier line\n");
    Irp = VfSendHeldIrp(FALSE);
    if (Irp != NULL)
        VfCompleteHeldIrp(Irp);

    trace("Case 2: an IRP is freed while it pends below; expect one log-only line, I/O rule 0x20b\n");
    Irp = VfSendHeldIrp(FALSE);
    if (Irp != NULL)
        IoFreeIrp(Irp);

    trace("Case 3: an IRP is reused while it pends below; expect one log-only line, I/O rule 0x310\n");
    Irp = VfSendHeldIrp(FALSE);
    if (Irp != NULL)
    {
        IoReuseIrp(Irp, STATUS_SUCCESS);
        IoFreeIrp(Irp);
    }

    trace("Case 4: an IRP is sent with its cancel routine set; expect one log-only line, I/O rule 0x203\n");
    Irp = VfSendHeldIrp(TRUE);
    if (Irp != NULL)
        VfCompleteHeldIrp(Irp);

    trace("Case 5: an IRP is completed, reused and sent again by its owner; expect NO verifier line\n");
    Irp = VfSendHeldIrp(FALSE);
    if (Irp != NULL)
    {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReuseIrp(Irp, STATUS_SUCCESS);
        VfSendIrpToHold(Irp, FALSE);
        VfCompleteHeldIrp(Irp);
    }

    InterlockedExchangePointer(Slot, (PVOID)VfSavedDispatch);
    IoDeleteDevice(VfLowerDevice);
    VfLowerDevice = NULL;
    trace("Done: the debug log must hold exactly three log-only lines (0x20b, 0x310, 0x203) and no bug check\n");
}

#define VF_STRESS_THREADS 4
#define VF_STRESS_ROUNDS 2000

static LONG VfStressRounds;

static
NTSTATUS
VfStressSendAndComplete(
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;

    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoSetCompletionRoutine(Irp, VfStopCompletion, NULL, TRUE, TRUE, TRUE);
    Status = IoCallDriver(VfLowerDevice, Irp);
    if (Status != STATUS_PENDING)
        return STATUS_UNSUCCESSFUL;

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
VfIrpStressThread(
    _In_ PVOID Context)
{
    NTSTATUS Status;
    PIRP Irp;
    ULONG i;

    UNREFERENCED_PARAMETER(Context);

    for (i = 0; i < VF_STRESS_ROUNDS; i++)
    {
        Irp = IoAllocateIrp(VfLowerDevice->StackSize, FALSE);
        if (Irp == NULL)
            break;

        Status = VfStressSendAndComplete(Irp);
        if (NT_SUCCESS(Status) && (i & 1) != 0)
        {
            IoReuseIrp(Irp, STATUS_SUCCESS);
            Status = VfStressSendAndComplete(Irp);
        }

        IoFreeIrp(Irp);
        if (!NT_SUCCESS(Status))
            break;

        InterlockedIncrement(&VfStressRounds);
    }
}

START_TEST(MmDriverVerifierIrpStress)
{
    PVOID *Slot = (PVOID *)&KmtDriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL];
    PKTHREAD Threads[VF_STRESS_THREADS];
    ULONG Flags = 0;
    NTSTATUS Status;
    ULONG i;

    if (skip(EveryTagIsSpecial() && NT_SUCCESS(MmIsVerifierEnabled(&Flags)) && (Flags & DRIVER_VERIFIER_IO_CHECKING),
             "kmtest_drv.sys is not verified with I/O verification (VerifyDriverLevel 0x19)\n"))
    {
        return;
    }

    Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &VfLowerDevice);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    VfLowerIsShared = TRUE;
    VfStressRounds = 0;
    VfSavedDispatch = InterlockedExchangePointer(Slot, (PVOID)VfLowerDispatch);

    trace("%d threads send, complete, reuse and free IRPs correctly; expect NO verifier line\n", VF_STRESS_THREADS);
    for (i = 0; i < VF_STRESS_THREADS; i++)
        Threads[i] = KmtStartThread(VfIrpStressThread, NULL);

    for (i = 0; i < VF_STRESS_THREADS; i++)
        KmtFinishThread(Threads[i], NULL);

    ok_eq_long(VfStressRounds, VF_STRESS_THREADS * VF_STRESS_ROUNDS);

    InterlockedExchangePointer(Slot, (PVOID)VfSavedDispatch);
    VfLowerIsShared = FALSE;
    IoDeleteDevice(VfLowerDevice);
    VfLowerDevice = NULL;
    trace("Done: the debug log must hold no Driver Verifier line and no bug check\n");
}

static FAST_MUTEX VfFirstMutex;
static FAST_MUTEX VfSecondMutex;

START_TEST(MmDriverVerifierLockOrder)
{
    ULONG Flags = 0;

    if (skip(EveryTagIsSpecial() && NT_SUCCESS(MmIsVerifierEnabled(&Flags)) && (Flags & VF_TEST_DEADLOCK_DETECTION),
             "kmtest_drv.sys is not verified with deadlock detection (VerifyDriverLevel 0x29)\n"))
    {
        return;
    }

    ExInitializeFastMutex(&VfFirstMutex);
    ExInitializeFastMutex(&VfSecondMutex);

    trace("Case 1: first, then second; expect NO verifier line\n");
    ExAcquireFastMutex(&VfFirstMutex);
    ExAcquireFastMutex(&VfSecondMutex);
    ExReleaseFastMutex(&VfSecondMutex);
    ExReleaseFastMutex(&VfFirstMutex);

    trace("Case 2: the same order again; expect NO verifier line\n");
    ExAcquireFastMutex(&VfFirstMutex);
    ExAcquireFastMutex(&VfSecondMutex);
    ExReleaseFastMutex(&VfSecondMutex);
    ExReleaseFastMutex(&VfFirstMutex);

    trace("Case 3: second, then first; expect one log-only line, deadlock rule 0x1001, lock %p\n", &VfFirstMutex);
    ExAcquireFastMutex(&VfSecondMutex);
    ExAcquireFastMutex(&VfFirstMutex);
    ExReleaseFastMutex(&VfFirstMutex);
    ExReleaseFastMutex(&VfSecondMutex);

    ok_eq_uint(KeGetCurrentIrql(), PASSIVE_LEVEL);
    trace("Done: the debug log must hold exactly one log-only line (0x1001) and no bug check\n");
}

static
BOOLEAN
VfDeadlockDetectionIsOn(VOID)
{
    ULONG Flags = 0;

    return EveryTagIsSpecial() && NT_SUCCESS(MmIsVerifierEnabled(&Flags)) && (Flags & VF_TEST_DEADLOCK_DETECTION);
}

static
VOID
NTAPI
VfTakeAndLeaveThread(
    _In_ PVOID Context)
{
    PFAST_MUTEX Mutex = Context;

    ExAcquireFastMutex(Mutex);
    KeLowerIrql(PASSIVE_LEVEL);
}

START_TEST(MmDriverVerifierLockOwner)
{
    PKTHREAD Thread;
    KIRQL OldIrql;

    if (skip(VfDeadlockDetectionIsOn(),
             "kmtest_drv.sys is not verified with deadlock detection (VerifyDriverLevel 0x29)\n"))
    {
        return;
    }

    ExInitializeFastMutex(&VfFirstMutex);

    trace("Case 1: a thread takes and releases its own lock; expect NO verifier line\n");
    ExAcquireFastMutex(&VfFirstMutex);
    ExReleaseFastMutex(&VfFirstMutex);

    trace("Case 2: another thread takes lock %p, this one releases it; expect one log-only line, deadlock rule "
          "0x1004\n", &VfFirstMutex);
    Thread = KmtStartThread(VfTakeAndLeaveThread, &VfFirstMutex);
    if (Thread == NULL)
        return;

    KmtFinishThread(Thread, NULL);
    KeRaiseIrql(APC_LEVEL, &OldIrql);
    ExReleaseFastMutex(&VfFirstMutex);
    KeLowerIrql(OldIrql);

    trace("Case 3: the lock is taken and released again by one thread; expect NO verifier line\n");
    ExAcquireFastMutex(&VfFirstMutex);
    ExReleaseFastMutex(&VfFirstMutex);

    ok_eq_uint(KeGetCurrentIrql(), PASSIVE_LEVEL);
    trace("Done: the debug log must hold exactly one log-only line (0x1004) and no bug check\n");
}

START_TEST(MmDriverVerifierLockFreed)
{
    PFAST_MUTEX Mutex;

    if (skip(VfDeadlockDetectionIsOn(),
             "kmtest_drv.sys is not verified with deadlock detection (VerifyDriverLevel 0x29)\n"))
    {
        return;
    }

    trace("Case 1: a block is freed after its lock was released; expect NO verifier line\n");
    Mutex = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Mutex), 'kLpS');
    ok(Mutex != NULL, "no block\n");
    if (Mutex == NULL)
        return;

    ExInitializeFastMutex(Mutex);
    ExAcquireFastMutex(Mutex);
    ExReleaseFastMutex(Mutex);
    ExFreePoolWithTag(Mutex, 'kLpS');

    Mutex = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Mutex), 'kLpS');
    ok(Mutex != NULL, "no block\n");
    if (Mutex == NULL)
        return;

    trace("Case 2: block %p is freed while its lock is held; expect one log-only line, deadlock rule 0x100b\n",
          Mutex);
    ExInitializeFastMutex(Mutex);
    ExAcquireFastMutex(Mutex);
    ExFreePoolWithTag(Mutex, 'kLpS');
    KeLowerIrql(PASSIVE_LEVEL);

    ok_eq_uint(KeGetCurrentIrql(), PASSIVE_LEVEL);
    trace("Done: the debug log must hold exactly one log-only line (0x100b) and no bug check\n");
}

static
VOID
NTAPI
VfLockStressThread(
    _In_ PVOID Context)
{
    PFAST_MUTEX Pair;
    ULONG First, i;

    UNREFERENCED_PARAMETER(Context);

    for (i = 0; i < VF_STRESS_ROUNDS; i++)
    {
        Pair = ExAllocatePoolWithTag(NonPagedPool, 2 * sizeof(*Pair), 'kLpS');
        if (Pair == NULL)
            break;

        ExInitializeFastMutex(&Pair[0]);
        ExInitializeFastMutex(&Pair[1]);
        First = i & 1;

        ExAcquireFastMutex(&VfFirstMutex);
        ExAcquireFastMutex(&VfSecondMutex);
        ExAcquireFastMutex(&Pair[First]);
        ExAcquireFastMutex(&Pair[First ^ 1]);
        ExReleaseFastMutex(&Pair[First ^ 1]);
        ExReleaseFastMutex(&Pair[First]);
        ExReleaseFastMutex(&VfSecondMutex);
        ExReleaseFastMutex(&VfFirstMutex);

        ExFreePoolWithTag(Pair, 'kLpS');
        InterlockedIncrement(&VfStressRounds);
    }
}

START_TEST(MmDriverVerifierLockStress)
{
    PKTHREAD Threads[VF_STRESS_THREADS];
    ULONG i;

    if (skip(VfDeadlockDetectionIsOn(),
             "kmtest_drv.sys is not verified with deadlock detection (VerifyDriverLevel 0x29)\n"))
    {
        return;
    }

    ExInitializeFastMutex(&VfFirstMutex);
    ExInitializeFastMutex(&VfSecondMutex);
    VfStressRounds = 0;

    trace("%d threads take two fixed locks in one order, then two locks of a fresh block in alternating order, and "
          "free the block; expect NO verifier line\n", VF_STRESS_THREADS);
    for (i = 0; i < VF_STRESS_THREADS; i++)
        Threads[i] = KmtStartThread(VfLockStressThread, NULL);

    for (i = 0; i < VF_STRESS_THREADS; i++)
        KmtFinishThread(Threads[i], NULL);

    ok_eq_long(VfStressRounds, VF_STRESS_THREADS * VF_STRESS_ROUNDS);
    ok_eq_uint(KeGetCurrentIrql(), PASSIVE_LEVEL);
    trace("Done: the debug log must hold no Driver Verifier line and no bug check\n");
}
