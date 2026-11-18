/*
 * PROJECT:         LiberNT kernel-mode tests
 * LICENSE:         GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:         High-resolution EX_TIMER deadline: a 16.667 ms periodic
 *                  high-resolution timer must fire on time without raising
 *                  the periodic clock rate (KiSetClockDeadline / HAL one-shot).
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define HRD_PERIOD      166670LL    /* 16.667 ms in 100 ns units (60 Hz) */
#define HRD_FIRES       30

static KEVENT HrdEvent;
static volatile LONG HrdCount;
static ULONGLONG HrdTimes[HRD_FIRES];

static
VOID
NTAPI
HrdCallback(
    _In_ PEX_TIMER Timer,
    _In_opt_ PVOID Context)
{
    LONG Index;

    UNREFERENCED_PARAMETER(Timer);
    UNREFERENCED_PARAMETER(Context);

    Index = InterlockedIncrement(&HrdCount) - 1;
    if (Index < HRD_FIRES)
        HrdTimes[Index] = KeQueryInterruptTime();
    if (Index == HRD_FIRES - 1)
        KeSetEvent(&HrdEvent, IO_NO_INCREMENT, FALSE);
}

/* Number of clock interrupts (interrupt-time steps) seen in 200 ms */
static
ULONG
CountTicks(VOID)
{
    ULONGLONG Start, Last, Now;
    ULONG Ticks = 0;

    Start = Last = KeQueryInterruptTime();
    do
    {
        KeStallExecutionProcessor(20);
        Now = KeQueryInterruptTime();
        if (Now != Last)
        {
            Ticks++;
            Last = Now;
        }
    } while (Now - Start < 2000000);
    return Ticks;
}

START_TEST(ExTimerHighResDeadline)
{
    PEX_TIMER Timer;
    EXT_SET_PARAMETERS Parameters;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    ULONG MaxIncrement;
    ULONG TickBefore, TickDuring, Allowed;
    ULONGLONG Gap, MaxGap = 0, MinGap = ~0ULL;
    LONG Index;

    MaxIncrement = KeQueryTimeIncrement();
    TickBefore = CountTicks();
    trace("KeQueryTimeIncrement %lu, ticks/200ms before %lu\n", MaxIncrement, TickBefore);

    KeInitializeEvent(&HrdEvent, NotificationEvent, FALSE);
    HrdCount = 0;
    Timer = ExAllocateTimer(HrdCallback, NULL, EX_TIMER_HIGH_RESOLUTION);
    ok(Timer != NULL, "ExAllocateTimer(high resolution) failed\n");
    if (!Timer)
        return;

    ExInitializeSetTimerParameters(&Parameters);
    ExSetTimer(Timer, -HRD_PERIOD, HRD_PERIOD, &Parameters);

    /* Let the deadline / fallback settle, then sample the tick while armed */
    Timeout.QuadPart = -10LL * 1000 * 100;
    KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
    TickDuring = CountTicks();

    Timeout.QuadPart = -10LL * 1000 * 1000 * 5;
    Status = KeWaitForSingleObject(&HrdEvent, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);

    ExCancelTimer(Timer, NULL);
    ExDeleteTimer(Timer, TRUE, TRUE, NULL);

    for (Index = 1; Index < HRD_FIRES && Index < HrdCount; Index++)
    {
        Gap = HrdTimes[Index] - HrdTimes[Index - 1];
        if (Gap > MaxGap) MaxGap = Gap;
        if (Gap < MinGap) MinGap = Gap;
    }
    trace("fires %ld, gap min %I64u max %I64u, ticks/200ms during %lu\n",
          HrdCount, MinGap, MaxGap, TickDuring);

    /* On time: every period within 2 ms of 16.667 ms (a 15.625 ms periodic
     * clock alone would give 15.6/31.2 ms gaps). */
    ok(HrdCount >= HRD_FIRES, "only %ld fires\n", HrdCount);
    ok(MaxGap <= (ULONGLONG)HRD_PERIOD + 20000, "late period %I64u\n", MaxGap);
    ok(MinGap + 20000 >= (ULONGLONG)HRD_PERIOD, "early period %I64u\n", MinGap);

    /* The periodic clock must stay at its normal increment. Arming one 60 Hz
     * deadline timer may add at most about two clock interrupts per period
     * (the deadline tick and one re-aim); the fallback that runs the clock at
     * its finest period adds hundreds in 200 ms. Measured against the rate
     * before arming, so timers already armed elsewhere (softgpu vsync) count
     * in both. Skip if the clock already ran at 500 Hz or more before. */
    Allowed = TickBefore + 2 * (2000000 / (ULONG)HRD_PERIOD + 2);
    if (TickBefore > 100)
        skip(FALSE, "clock rate already raised before arming (%lu ticks/200ms)\n", TickBefore);
    else
        ok(TickDuring <= Allowed, "clock rate raised while armed: %lu ticks/200ms (before %lu, allowed %lu)\n",
           TickDuring, TickBefore, Allowed);
}
