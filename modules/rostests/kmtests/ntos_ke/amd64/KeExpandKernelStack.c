/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     KeExpandKernelStackAndCallout(Ex): fast path, expansion stack with a deep
 *              recursion, SEH inside the callout, unwind across the switch, parameter and
 *              IRQL checks, DISPATCH_LEVEL without waiting, nesting
 */

#include <kmt_test.h>
#include <ndk/ketypes.h>
#include <ndk/rtlfuncs.h>

NTSYSAPI
PVOID
NTAPI
RtlPcToFileHeader(
    _In_ PVOID PcValue,
    _Out_ PVOID *BaseOfImage);

/* NT: an expansion may not exceed the large stack minus the normal one (0xC000 on amd64) */
#define EXPANSION_MAXIMUM (KERNEL_LARGE_STACK_SIZE - KERNEL_STACK_SIZE)
/* The deep part consumes more than a whole 24 KiB thread stack */
#define DEEP_BYTES 0x7000
#define FRAME_BYTES 1024
#define NESTED_SIZE 0x4000

typedef struct _EXPAND_CONTEXT
{
    /* The caller's view */
    ULONG_PTR CallerLow;
    ULONG_PTR CallerHigh;
    SIZE_T Size;
    BOOLEAN Deep;
    BOOLEAN DoSeh;
    BOOLEAN DoUnwind;
    BOOLEAN Nest;

    /* Filled by the callout */
    ULONG Calls;
    PVOID Parameter;
    KIRQL Irql;
    ULONG_PTR Low;
    ULONG_PTR High;
    ULONG_PTR Sp;
    BOOLEAN CalloutActive;
    BOOLEAN Switched;
    ULONG Depth;
    ULONG OutOfLimits;
    ULONG Corrupt;
    ULONG Caught;
    ULONG CaughtDeep;
    ULONG UnwindFrames;
    ULONG UnwindNoEntry;
    BOOLEAN UnwindCrossed;
    BOOLEAN UnwindSameImage;
    ULONG WalkFrames;
    BOOLEAN WalkCrossed;
    NTSTATUS NestStatus;
    ULONG NestCalls;
    BOOLEAN NestSwitched;
    BOOLEAN NestRestored;
} EXPAND_CONTEXT, *PEXPAND_CONTEXT;

/* Where the stack is right now: the address of a local, handed out through a volatile so the
 * compiler does not treat it as a returned stack address (it is only ever compared with limits) */
static
ULONG_PTR
GetSp(VOID)
{
    volatile UCHAR Here = 0;
    volatile ULONG_PTR Sp = (ULONG_PTR)&Here;
    return Sp;
}

static
VOID
InitContext(
    _Out_ PEXPAND_CONTEXT Context,
    _In_ ULONG_PTR Low,
    _In_ ULONG_PTR High,
    _In_ SIZE_T Size)
{
    RtlZeroMemory(Context, sizeof(*Context));
    Context->CallerLow = Low;
    Context->CallerHigh = High;
    Context->Size = Size;
}

/* 1 KiB per level, every frame checked against the limits the kernel reports */
static
ULONG
Recurse(
    _In_ PEXPAND_CONTEXT Context,
    _In_ ULONG_PTR Top,
    _In_ ULONG Level,
    _In_ BOOLEAN RaiseAtBottom)
{
    volatile UCHAR Frame[FRAME_BYTES];
    ULONG_PTR Low, High;
    ULONG Result;

    Frame[0] = (UCHAR)Level;
    Frame[FRAME_BYTES - 1] = (UCHAR)~Level;
    IoGetStackLimits(&Low, &High);
    if ((ULONG_PTR)Frame < Low + 2 * FRAME_BYTES || (ULONG_PTR)Frame >= High)
    {
        /* The next frame would leave the stack we were given: stop instead of crashing */
        Context->OutOfLimits++;
        return Level;
    }
    if (Top - (ULONG_PTR)Frame >= DEEP_BYTES)
    {
        if (RaiseAtBottom)
            ExRaiseStatus(STATUS_UNSUCCESSFUL);
        return Level;
    }
    Result = Recurse(Context, Top, Level + 1, RaiseAtBottom);
    if (Frame[0] != (UCHAR)Level || Frame[FRAME_BYTES - 1] != (UCHAR)~Level)
        Context->Corrupt++;
    return Result;
}

/* Walk back from the callout over the switch onto the caller's stack, with the real
 * unwinder and with the debugger-style frame walk */
static
VOID
UnwindAcrossSwitch(
    _In_ PEXPAND_CONTEXT Context)
{
    CONTEXT Ctx;
    PRUNTIME_FUNCTION Entry;
    ULONG64 ImageBase, MyImageBase, Establisher;
    PVOID HandlerData;
    PVOID Callers[32];
    PVOID Base;
    ULONG i, Count;
    BOOLEAN SeenOtherImage;

    MyImageBase = 0;
    Entry = RtlLookupFunctionEntry((ULONG64)(ULONG_PTR)UnwindAcrossSwitch, &MyImageBase, NULL);
    ok(Entry != NULL, "no unwind data for the test itself\n");

    RtlCaptureContext(&Ctx);
    for (i = 0; i < 64; i++)
    {
        Entry = RtlLookupFunctionEntry(Ctx.Rip, &ImageBase, NULL);
        if (Entry == NULL)
        {
            /* Only a leaf may lack unwind data, and only as the first frame */
            if (i != 0)
            {
                Context->UnwindNoEntry++;
                break;
            }
            Ctx.Rip = *(PULONG64)(ULONG_PTR)Ctx.Rsp;
            Ctx.Rsp += 8;
        }
        else
        {
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ImageBase, Ctx.Rip, Entry, &Ctx, &HandlerData, &Establisher, NULL);
        }
        Context->UnwindFrames++;
        if (Ctx.Rip == 0)
            break;
        if (Ctx.Rsp >= Context->CallerLow && Ctx.Rsp < Context->CallerHigh)
        {
            /* Back on the caller's stack: the frame we landed in must be this driver's again */
            Context->UnwindCrossed = TRUE;
            Entry = RtlLookupFunctionEntry(Ctx.Rip, &ImageBase, NULL);
            Context->UnwindSameImage = (Entry != NULL && ImageBase == MyImageBase);
            break;
        }
    }

    /* driver frames -> kernel frames (the switch, the API) -> driver frames again */
    Count = RtlWalkFrameChain(Callers, RTL_NUMBER_OF(Callers), 0);
    Context->WalkFrames = Count;
    SeenOtherImage = FALSE;
    for (i = 0; i < Count; i++)
    {
        Base = NULL;
        RtlPcToFileHeader(Callers[i], &Base);
        if ((ULONG64)(ULONG_PTR)Base != MyImageBase)
            SeenOtherImage = TRUE;
        else if (SeenOtherImage)
            Context->WalkCrossed = TRUE;
    }
}

static
VOID
NTAPI
ExpandCallout(
    _In_opt_ PVOID Parameter)
{
    PEXPAND_CONTEXT Context = Parameter;
    PKTHREAD Thread = KeGetCurrentThread();
    EXPAND_CONTEXT Inner;
    ULONG_PTR Low, High;

    Context->Calls++;
    Context->Parameter = Parameter;
    Context->Irql = KeGetCurrentIrql();
    Context->Sp = GetSp();
    IoGetStackLimits(&Context->Low, &Context->High);
    Context->CalloutActive = Thread->CalloutActive != 0;
    /* Only use the room when we really got another stack with it: on a kernel that runs
     * the callout on the caller's stack the deep part would overflow, so it is skipped
     * and the checks below report it */
    Context->Switched = (Context->Low != Context->CallerLow || Context->High != Context->CallerHigh) &&
                        Context->High - Context->Low >= Context->Size;

    if (Context->Deep && Context->Switched)
        Context->Depth = Recurse(Context, Context->Sp, 0, FALSE);

    if (Context->DoSeh)
    {
        _SEH2_TRY
        {
            ExRaiseStatus(STATUS_UNSUCCESSFUL);
        }
        _SEH2_EXCEPT(_SEH2_GetExceptionCode() == STATUS_UNSUCCESSFUL ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            Context->Caught++;
        }
        _SEH2_END;

        if (Context->Switched)
        {
            _SEH2_TRY
            {
                Recurse(Context, Context->Sp, 0, TRUE);
            }
            _SEH2_EXCEPT(_SEH2_GetExceptionCode() == STATUS_UNSUCCESSFUL ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
            {
                Context->CaughtDeep++;
            }
            _SEH2_END;
        }
    }

    if (Context->DoUnwind && Context->Switched)
        UnwindAcrossSwitch(Context);

    if (Context->Nest)
    {
        InitContext(&Inner, Context->Low, Context->High, NESTED_SIZE);
        Context->NestStatus = KeExpandKernelStackAndCallout(ExpandCallout, &Inner, NESTED_SIZE);
        Context->NestCalls = Inner.Calls;
        Context->NestSwitched = Inner.Switched;
        IoGetStackLimits(&Low, &High);
        Context->NestRestored = (Low == Context->Low && High == Context->High);
    }
}

START_TEST(KeExpandKernelStack)
{
    EXPAND_CONTEXT C;
    NTSTATUS Status;
    ULONG_PTR Low, High, AfterLow, AfterHigh;
    KIRQL OldIrql;
    PKTHREAD Thread = KeGetCurrentThread();

    IoGetStackLimits(&Low, &High);
    ok_irql(PASSIVE_LEVEL);
    ok(Thread->CalloutActive == 0, "CalloutActive set before the test\n");
    trace("thread stack %p-%p (%Iu bytes), expansion maximum 0x%Ix\n",
          (PVOID)Low, (PVOID)High, (SIZE_T)(High - Low), (SIZE_T)EXPANSION_MAXIMUM);

    /* 1. Fast path: a small request runs on the same stack */
    InitContext(&C, Low, High, 0x100);
    Status = KeExpandKernelStackAndCallout(ExpandCallout, &C, 0x100);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(C.Calls, 1UL);
    ok_eq_pointer(C.Parameter, &C);
    ok(C.Low == Low && C.High == High, "fast path moved the stack: %p-%p, caller %p-%p\n",
       (PVOID)C.Low, (PVOID)C.High, (PVOID)Low, (PVOID)High);
    ok(C.Sp >= Low && C.Sp < High, "fast path sp %p outside %p-%p\n", (PVOID)C.Sp, (PVOID)Low, (PVOID)High);
    ok_eq_uint(C.Irql, PASSIVE_LEVEL);
    IoGetStackLimits(&AfterLow, &AfterHigh);
    ok(AfterLow == Low && AfterHigh == High, "limits changed after the fast path\n");
    ok_irql(PASSIVE_LEVEL);

    /* 2. Forced switch: the maximum size, a deep recursion inside, unwind across the
     *    switch with the real unwinder and the frame walk, and a nested expansion */
    InitContext(&C, Low, High, EXPANSION_MAXIMUM);
    C.Deep = TRUE;
    C.DoUnwind = TRUE;
    C.Nest = TRUE;
    Status = KeExpandKernelStackAndCalloutEx(ExpandCallout, &C, EXPANSION_MAXIMUM, TRUE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(C.Calls, 1UL);
    ok_eq_pointer(C.Parameter, &C);
    ok(C.Switched, "callout ran on the caller's stack: %p-%p, caller %p-%p\n",
       (PVOID)C.Low, (PVOID)C.High, (PVOID)Low, (PVOID)High);
    ok(C.Sp >= C.Low && C.Sp < C.High, "sp %p outside the reported stack %p-%p\n",
       (PVOID)C.Sp, (PVOID)C.Low, (PVOID)C.High);
    ok(C.High - C.Low >= EXPANSION_MAXIMUM, "expansion stack has only %Iu bytes\n", (SIZE_T)(C.High - C.Low));
    ok(C.CalloutActive, "CalloutActive not set inside the callout\n");
    ok_eq_uint(C.Irql, PASSIVE_LEVEL);
    ok((SIZE_T)C.Depth * FRAME_BYTES >= DEEP_BYTES, "recursion stopped at depth %lu\n", C.Depth);
    ok_eq_ulong(C.OutOfLimits, 0UL);
    ok_eq_ulong(C.Corrupt, 0UL);
    ok(C.UnwindCrossed, "RtlVirtualUnwind did not reach the caller's stack (%lu frames, %lu without unwind data)\n",
       C.UnwindFrames, C.UnwindNoEntry);
    ok(C.UnwindSameImage, "the first frame back on the caller's stack is not this driver's\n");
    ok_eq_ulong(C.UnwindNoEntry, 0UL);
    ok(C.WalkCrossed, "RtlWalkFrameChain (%lu frames) did not cross the switch\n", C.WalkFrames);
    ok_eq_hex(C.NestStatus, STATUS_SUCCESS);
    ok_eq_ulong(C.NestCalls, 1UL);
    ok(C.NestSwitched, "nested callout ran on the outer expansion stack\n");
    ok(C.NestRestored, "limits not restored after the nested callout\n");
    IoGetStackLimits(&AfterLow, &AfterHigh);
    ok(AfterLow == Low && AfterHigh == High, "limits not restored after the switch: %p-%p\n",
       (PVOID)AfterLow, (PVOID)AfterHigh);
    ok(Thread->CalloutActive == 0, "CalloutActive still set after the callout\n");
    ok_irql(PASSIVE_LEVEL);

    /* 3. SEH inside the callout: raised at the top, and raised at the bottom of the deep
     *    part and caught at the top (the dispatcher and the unwinder work on the new stack) */
    InitContext(&C, Low, High, 0x8000);
    C.DoSeh = TRUE;
    Status = KeExpandKernelStackAndCallout(ExpandCallout, &C, 0x8000);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(C.Calls, 1UL);
    ok(C.Switched, "SEH case: callout ran on the caller's stack\n");
    ok_eq_ulong(C.Caught, 1UL);
    ok_eq_ulong(C.CaughtDeep, 1UL);
    IoGetStackLimits(&AfterLow, &AfterHigh);
    ok(AfterLow == Low && AfterHigh == High, "limits not restored after the SEH case\n");
    ok_irql(PASSIVE_LEVEL);

    /* 4. Size above the maximum is refused without calling out */
    InitContext(&C, Low, High, EXPANSION_MAXIMUM + 1);
    Status = KeExpandKernelStackAndCallout(ExpandCallout, &C, EXPANSION_MAXIMUM + 1);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER_3);
    ok_eq_ulong(C.Calls, 0UL);

    /* 5. Above DISPATCH_LEVEL there is no stack to give: refused, no callout */
    InitContext(&C, Low, High, 0x8000);
    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    Status = KeExpandKernelStackAndCalloutEx(ExpandCallout, &C, 0x8000, FALSE, NULL);
    KeLowerIrql(OldIrql);
    ok(!NT_SUCCESS(Status), "accepted at HIGH_LEVEL: 0x%lx\n", Status);
    ok_eq_ulong(C.Calls, 0UL);
    ok_irql(PASSIVE_LEVEL);

    /* 6. DISPATCH_LEVEL without waiting: either a stack or STATUS_INSUFFICIENT_RESOURCES,
     *    never a fault */
    InitContext(&C, Low, High, 0x8000);
    C.Deep = TRUE;
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Status = KeExpandKernelStackAndCalloutEx(ExpandCallout, &C, 0x8000, FALSE, NULL);
    ok_irql(DISPATCH_LEVEL);
    IoGetStackLimits(&AfterLow, &AfterHigh);
    KeLowerIrql(OldIrql);
    ok(Status == STATUS_SUCCESS || Status == STATUS_INSUFFICIENT_RESOURCES,
       "DISPATCH_LEVEL, no wait: 0x%lx\n", Status);
    if (Status == STATUS_SUCCESS)
    {
        ok_eq_ulong(C.Calls, 1UL);
        ok_eq_uint(C.Irql, DISPATCH_LEVEL);
        ok(C.Switched, "DISPATCH_LEVEL case: callout ran on the caller's stack\n");
        ok((SIZE_T)C.Depth * FRAME_BYTES >= DEEP_BYTES, "DISPATCH_LEVEL recursion stopped at depth %lu\n", C.Depth);
        ok_eq_ulong(C.OutOfLimits, 0UL);
        ok_eq_ulong(C.Corrupt, 0UL);
    }
    else
    {
        ok_eq_ulong(C.Calls, 0UL);
    }
    ok(AfterLow == Low && AfterHigh == High, "limits not restored after the DISPATCH_LEVEL case\n");
    ok(Thread->CalloutActive == 0, "CalloutActive still set at the end\n");
    ok_irql(PASSIVE_LEVEL);
}
