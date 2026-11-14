/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     amd64 interrupt service routines run on the per-processor ISR stack, so an
 *              interrupt needs only a trap frame on the interrupted thread's stack
 */

#include <kmt_test.h>

#ifdef _M_AMD64

#define ISR_TEST_CPUS 64
#define ISR_TEST_FRAME 0x200
#define ISR_TEST_LEFT 0x600
#define ISR_TEST_SPIN_MS 300

typedef struct _ISR_RECORD
{
    ULONG_PTR Sp[ISR_TEST_CPUS];
    ULONG_PTR IsrTop[ISR_TEST_CPUS];
    LONG Calls;
} ISR_RECORD, *PISR_RECORD;

typedef struct _ISR_STRESS
{
    ISR_RECORD Record;
    volatile LONG Stop;
    LONG Rounds;
    SIZE_T Left;
    ULONG Depth;
    ULONG Corrupt;
    KIRQL OldIrql;
    LARGE_INTEGER TicksBefore;
    LARGE_INTEGER TicksAfter;
} ISR_STRESS, *PISR_STRESS;

static
ULONG_PTR
NTAPI
IsrIpiWorker(
    _In_ ULONG_PTR Argument)
{
    PISR_RECORD Record = (PISR_RECORD)Argument;
    ULONG Cpu = KeGetCurrentProcessorNumber();
    volatile UCHAR Here = 0;

    if (Cpu < ISR_TEST_CPUS)
    {
        Record->Sp[Cpu] = (ULONG_PTR)&Here;
        Record->IsrTop[Cpu] = (ULONG_PTR)KeGetCurrentPrcb()->IsrStack;
    }
    InterlockedIncrement(&Record->Calls);
    return 0;
}

static
VOID
NTAPI
IsrIpiThread(
    _In_ PVOID Context)
{
    PISR_STRESS Stress = Context;

    while (!Stress->Stop)
    {
        KeIpiGenericCall(IsrIpiWorker, (ULONG_PTR)&Stress->Record);
        InterlockedIncrement(&Stress->Rounds);
    }
}

/* Recurse until about ISR_TEST_LEFT bytes of the thread stack are left, then take interrupts there */
static
ULONG
IsrDeepSpin(
    _In_ PISR_STRESS Stress,
    _In_ ULONG Level)
{
    volatile UCHAR Frame[ISR_TEST_FRAME];
    ULONG_PTR Low, High;
    ULONG Result, i;

    Frame[0] = (UCHAR)Level;
    Frame[ISR_TEST_FRAME - 1] = (UCHAR)~Level;
    IoGetStackLimits(&Low, &High);
    if ((ULONG_PTR)Frame - Low >= ISR_TEST_LEFT + 2 * ISR_TEST_FRAME)
    {
        Result = IsrDeepSpin(Stress, Level + 1);
    }
    else
    {
        Stress->Left = (ULONG_PTR)Frame - Low;
        KeQueryTickCount(&Stress->TicksBefore);

        /* No DPC, APC or thread switch may use what is left: the caller lowers the IRQL again */
        KeRaiseIrql(DISPATCH_LEVEL, &Stress->OldIrql);
        for (i = 0; i < ISR_TEST_SPIN_MS * 10; i++)
            KeStallExecutionProcessor(100);
        KeQueryTickCount(&Stress->TicksAfter);
        Result = Level;
    }

    if (Frame[0] != (UCHAR)Level || Frame[ISR_TEST_FRAME - 1] != (UCHAR)~Level)
        Stress->Corrupt++;
    return Result;
}

START_TEST(KeIsrStack)
{
    static ISR_RECORD Record;
    ULONG_PTR Low, High;
    ULONG Cpu, Self, Remote = 0;

    RtlZeroMemory(&Record, sizeof(Record));
    KeSetSystemAffinityThread(1);
    Self = KeGetCurrentProcessorNumber();
    IoGetStackLimits(&Low, &High);
    KeIpiGenericCall(IsrIpiWorker, (ULONG_PTR)&Record);
    KeRevertToUserAffinityThread();

    ok_eq_long(Record.Calls, (LONG)KeNumberProcessors);
    for (Cpu = 0; Cpu < (ULONG)KeNumberProcessors && Cpu < ISR_TEST_CPUS; Cpu++)
    {
        ok(Record.IsrTop[Cpu] != 0, "processor %lu has no ISR stack\n", Cpu);
        if (Cpu == Self)
            continue;

        /* The other processors got here through the IPI interrupt */
        Remote++;
        ok(Record.IsrTop[Cpu] - Record.Sp[Cpu] <= ISR_STACK_SIZE,
           "processor %lu ran the IPI at %p, ISR stack top %p\n",
           Cpu, (PVOID)Record.Sp[Cpu], (PVOID)Record.IsrTop[Cpu]);
    }
    ok(Record.Sp[Self] >= Low && Record.Sp[Self] < High, "the calling processor left the thread stack\n");
    if (Remote == 0)
        skip(FALSE, "one processor: no IPI interrupt to look at\n");
}

START_TEST(KeIsrStackStress)
{
    static ISR_STRESS Stress;
    PKTHREAD Thread;

    RtlZeroMemory(&Stress, sizeof(Stress));
    Thread = KmtStartThread(IsrIpiThread, &Stress);
    Stress.Depth = IsrDeepSpin(&Stress, 0);
    KeLowerIrql(Stress.OldIrql);
    Stress.Stop = 1;
    KmtFinishThread(Thread, NULL);

    trace("depth %lu, %Iu bytes left, %ld IPI rounds, ticks %I64d -> %I64d\n",
          Stress.Depth, Stress.Left, Stress.Rounds, Stress.TicksBefore.QuadPart, Stress.TicksAfter.QuadPart);
    ok(Stress.Left < ISR_TEST_LEFT + 2 * ISR_TEST_FRAME, "only got down to %Iu bytes left\n", Stress.Left);
    ok(Stress.TicksAfter.QuadPart > Stress.TicksBefore.QuadPart + 1, "no clock interrupts during the spin\n");
    ok_eq_ulong(Stress.Corrupt, 0UL);
}

#else

START_TEST(KeIsrStack)
{
    skip(FALSE, "amd64 only\n");
}

START_TEST(KeIsrStackStress)
{
    skip(FALSE, "amd64 only\n");
}

#endif
