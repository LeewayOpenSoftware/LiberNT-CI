/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GNU GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/apic/rtctimer.c
 * PURPOSE:         HAL APIC Management and Control Code
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 * REFERENCES:      https://wiki.osdev.org/RTC
 *                  https://forum.osdev.org/viewtopic.php?f=13&t=20825&start=0
 *                  https://web.archive.org/web/20240119203005/http://www.bioscentral.com/misc/cmosmap.htm
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

static const UCHAR RtcMinimumClockRate = 6;  /* Minimum rate  6: 1024 Hz / 0.97 ms */
static const UCHAR RtcMaximumClockRate = 10; /* Maximum rate 10: 64 Hz / 15.6 ms */
static UCHAR HalpCurrentClockRate = 10;  /* Initial rate  10: 64 Hz / 15.6 ms */
static ULONG HalpCurrentTimeIncrement;
static ULONG HalpMinimumTimeIncrement;
static ULONG HalpMaximumTimeIncrement;
static ULONG HalpCurrentFractionalIncrement;
static ULONG HalpRunningFraction;
static BOOLEAN HalpSetClockRate;
static UCHAR HalpNextClockRate;
static ULONG HalpNextTimeIncrement;

static LONG HalpClockIpiTickOffset;

/*
 * The local APIC timer clock counts to one tick at a time. The count it runs
 * with, how long that count lasts, and how long before it started the previous
 * tick was, give the time the tick it ends in is due after the one before.
 * A deadline from the kernel makes that tick come sooner than the base period.
 * All of it belongs to processor 0, whose timer is the clock.
 */
#define HALP_CLOCK_SOONEST          100
#define HALP_CLOCK_DEADLINE_SLACK   500
#define HALP_CLOCK_RECHECK          5000
static ULONGLONG HalpClockDeadline;
static ULONG HalpClockCount;
static ULONG HalpClockCountTime;
static ULONG HalpClockCountBase;
static ULONG HalpClockPendingIncrement;

BOOLEAN HalpApicTimerClock = FALSE;
static ULONG64 HalpApicTimerFrequency;

extern LARGE_INTEGER HalpCpuClockFrequency;
extern BOOLEAN HalIsProfiling;

static
BOOLEAN
HalpCanUseApicTimerClock(VOID)
{
    INT CpuInfo[4];

    __cpuid(CpuInfo, 1);

    if (CpuInfo[2] & (1UL << 31))
        return TRUE;

    __cpuid(CpuInfo, 0);
    if (CpuInfo[0] < 6)
        return FALSE;
    __cpuid(CpuInfo, 6);
    return (CpuInfo[0] & (1UL << 2)) != 0;
}

static
VOID
HalpCalibrateApicTimer(VOID)
{
    LVT_REGISTER LvtEntry;
    ULONG64 Start, Ticks;
    ULONG Elapsed;

    /* The timer runs with divide-by-1, so the reported frequency is its rate. */
    if (HalpReadHypervisorFrequency(HV_X64_MSR_APIC_FREQUENCY, &HalpApicTimerFrequency))
    {
        DPRINT1("Local APIC timer clock from the hypervisor: %I64u Hz\n", HalpApicTimerFrequency);
        return;
    }

    LvtEntry.Long = 0;
    LvtEntry.Vector = APIC_CLOCK_VECTOR;
    LvtEntry.Mask = 1;
    ApicWrite(APIC_TMRLVTR, LvtEntry.Long);
    ApicWrite(APIC_TDCR, TIMER_DV_DivideBy1);
    ApicWrite(APIC_TICR, MAXULONG);

    Ticks = HalpCpuClockFrequency.QuadPart / 100;
    Start = __rdtsc();
    while (__rdtsc() - Start < Ticks)
        YieldProcessor();
    Elapsed = MAXULONG - ApicRead(APIC_TCCR);

    ApicWrite(APIC_TICR, 0);
    HalpApicTimerFrequency = (ULONG64)Elapsed * 100;
    DPRINT1("Local APIC timer clock: %I64u Hz\n", HalpApicTimerFrequency);
}

/*
 * A clock tick reads and writes the local APIC several times: its own EOI, the
 * self interrupt that requests the DPC (status poll, command and IRR poll),
 * the EOI of that interrupt and, at the base rate, the clock IPI. Where each
 * access leaves the guest to be emulated in user mode, a tick costs about a
 * millisecond, and a clock asked to tick every millisecond then leaves the
 * processor nothing else. The finest period reported is therefore tied to
 * what one access costs here. A tick makes fewer than ten of them, so at 200
 * accesses to the period it stays under a twentieth of it. This is a bound
 * taken once at startup, not a promise: on such a machine the cost of an
 * access moves with what the emulator is doing, and was seen to differ by a
 * factor of four from one start to the next.
 */
#define HALP_CLOCK_PERIOD_PER_APIC_ACCESS   200
#define HALP_APIC_ACCESS_ROUNDS             15
#define HALP_APIC_ACCESS_SAMPLES            4

static
ULONG
HalpFinestApicTimerIncrement(VOID)
{
    ULONG64 Rounds[HALP_APIC_ACCESS_ROUNDS], Start, Ticks, Increment;
    ULONG Round, Sample, Slot;

    /* Time the rounds and keep them in order */
    for (Round = 0; Round < HALP_APIC_ACCESS_ROUNDS; Round++)
    {
        Start = __rdtsc();
        for (Sample = 0; Sample < HALP_APIC_ACCESS_SAMPLES; Sample++)
            (VOID)ApicRead(APIC_VER);
        Ticks = __rdtsc() - Start;

        for (Slot = Round; Slot > 0 && Rounds[Slot - 1] > Ticks; Slot--)
            Rounds[Slot] = Rounds[Slot - 1];
        Rounds[Slot] = Ticks;
    }

    /* Go by the middle one: a round the host preempted says nothing about the
     * cost, and neither does the one round that happened to go through fast.
     * Its time in 100 ns units, then the period it allows. */
    Increment = Rounds[HALP_APIC_ACCESS_ROUNDS / 2] * 10000000ULL / HalpCpuClockFrequency.QuadPart;
    Increment = Increment * HALP_CLOCK_PERIOD_PER_APIC_ACCESS / HALP_APIC_ACCESS_SAMPLES;

    /* A millisecond is the finest period offered, the base rate the coarsest */
    Increment = max(Increment, 10000);
    return (ULONG)min(Increment, HalpMaximumTimeIncrement);
}

/*!
    \brief Converts the CMOS RTC rate into the time increment in 0.1ns intervals.

    Rate Frequency Interval (ms) Precise increment (0.1ns)
    ------------------------------------------------------
     0   disabled
     1   32768      0.03052            305,175
     2   16384      0.06103            610,351
     3    8192      0.12207          1,220,703
     4    4096      0.24414          2,441,406
     5    2048      0.48828          4,882,812
     6    1024      0.97656          9,765,625 <- minimum
     7     512      1.95313         19,531,250
     8     256      3.90625         39,062,500
     9     128      7.8125          78,125,000
    10      64     15.6250         156,250,000 <- maximum / default
    11      32     31.25           312,500,000
    12      16     62.5            625,000,000
    13       8    125            1,250,000,000
    14       4    250            2,500,000,000
    15       2    500            5,000,000,000

*/
FORCEINLINE
ULONG
RtcClockRateToPreciseIncrement(UCHAR Rate)
{
    /* Calculate frequency */
    ULONG Frequency = 32768 >> (Rate - 1);

    /* Calculate interval in 0.1ns interval: Interval = (1 / Frequency) * 10,000,000,000 */
    return 10000000000ULL / Frequency;
}

/*
 * Starts the timer over on a count that lasts Time. Base is how long ago the
 * previous tick was, so the tick this count ends in comes Base + Time after it.
 */
static
VOID
HalpRestartApicTimerClock(ULONG Base, ULONG Time)
{
    ULONG64 Count = HalpApicTimerFrequency * Time / 10000000ULL;

    HalpClockCount = (ULONG)min(max(Count, 1), MAXULONG);
    HalpClockCountTime = Time;
    HalpClockCountBase = Base;
    ApicWrite(APIC_TICR, HalpClockCount);
}

static
VOID
HalpProgramApicTimerClock(ULONG Increment)
{
    LVT_REGISTER LvtEntry;

    LvtEntry.Long = 0;
    LvtEntry.Vector = APIC_CLOCK_VECTOR;
    LvtEntry.TimerMode = 1;
    LvtEntry.Mask = 0;
    ApicWrite(APIC_TDCR, TIMER_DV_DivideBy1);
    ApicWrite(APIC_TMRLVTR, LvtEntry.Long);
    HalpRestartApicTimerClock(0, Increment);
}

/* The local APIC timer runs at any period, so it is given the exact increment asked for */
static
VOID
HalpSetApicTimerIncrement(ULONG Increment)
{
    HalpCurrentTimeIncrement = Increment;
    HalpCurrentFractionalIncrement = 0;
    HalpProgramApicTimerClock(Increment);
}

/*
 * When the tick after the one at Now is due: a base period later, or at the
 * deadline if that comes first, but never closer to Now than the finest period.
 */
static
ULONG
HalpNextClockTick(ULONGLONG Now)
{
    ULONGLONG Deadline = HalpClockDeadline, Delta;
    ULONG Target = HalpCurrentTimeIncrement;

    if (Deadline == 0)
        return Target;

    /* Reached, as far as can be told from here. The kernel keeps time by the
     * performance counter and may not be there yet: tick again shortly, and
     * let it withdraw the deadline once what waited on it has gone off. */
    if (Deadline <= Now)
        return HalpMinimumTimeIncrement;

    /* Aim a little past it: a tick that comes early finds nothing due */
    Delta = Deadline - Now;
    Delta += max(Delta >> 8, HALP_CLOCK_DEADLINE_SLACK);
    if (Delta < Target)
        Target = max((ULONG)Delta, HalpMinimumTimeIncrement);

    return Target;
}

/* How long ago the previous tick was, read off the count the timer is at */
static
ULONG
HalpClockTickElapsed(VOID)
{
    ULONG Current = ApicRead(APIC_TCCR);
    ULONG Elapsed = HalpClockCountBase;

    if (Current < HalpClockCount)
        Elapsed += (ULONG)((ULONG64)(HalpClockCount - Current) * 10000000ULL / HalpApicTimerFrequency);

    return Elapsed;
}

/*
 * Has the timer count to the tick that is due after the one at Now, and leaves
 * it alone when it already does. A deadline is counted to exactly, and one
 * already behind makes the tick come at once. With none due, a tick starts a
 * whole base period from where the timer is, which the timer then keeps to by
 * itself; between ticks the tick is put back a base period after the previous
 * one, so that withdrawing a deadline does not make the clock skip a beat.
 * Between ticks, a tick already pending is left to aim the next one.
 */
static
VOID
HalpAimClockTick(ULONGLONG Now, BOOLEAN BetweenTicks)
{
    ULONG Target = HalpNextClockTick(Now);
    ULONG Irr = APIC_IRR + 0x10 * (APIC_CLOCK_VECTOR / 32);
    ULONG Elapsed, Previous;

    if (Target < HalpCurrentTimeIncrement)
    {
        if (Target == HalpClockCountBase + HalpClockCountTime)
            return;
    }
    else if (HalpClockCountTime == HalpCurrentTimeIncrement ||
             (BetweenTicks && HalpClockCountBase + HalpClockCountTime == HalpCurrentTimeIncrement))
    {
        return;
    }

    Elapsed = HalpClockTickElapsed();
    if (BetweenTicks && (ApicRead(Irr) & (1UL << (APIC_CLOCK_VECTOR % 32))))
        return;

    Previous = HalpClockCountBase + HalpClockCountTime;

    if (Target >= HalpCurrentTimeIncrement && !BetweenTicks)
        HalpRestartApicTimerClock(Elapsed, HalpCurrentTimeIncrement);
    else if (Target > Elapsed)
        HalpRestartApicTimerClock(Elapsed, Target - Elapsed);
    else
        HalpRestartApicTimerClock(Elapsed, HALP_CLOCK_SOONEST);

    /*
     * The count can run out between the look at the pending tick above and the
     * write that replaced it. The tick then pending ended the count before
     * this one, and has to report that count and not one that has only begun.
     * A count too short to tell apart from one that ran out by itself is left.
     */
    if (BetweenTicks && HalpClockCountTime >= HALP_CLOCK_RECHECK &&
        (ApicRead(Irr) & (1UL << (APIC_CLOCK_VECTOR % 32))))
    {
        HalpClockPendingIncrement = Previous;
    }
}

VOID
RtcSetClockRate(UCHAR ClockRate)
{
    UCHAR RegisterA;
    ULONG PreciseIncrement;

    /* Update the global values */
    HalpCurrentClockRate = ClockRate;
    PreciseIncrement = RtcClockRateToPreciseIncrement(ClockRate);
    HalpCurrentTimeIncrement = PreciseIncrement / 1000;
    HalpCurrentFractionalIncrement = PreciseIncrement % 1000;

    if (HalpApicTimerClock)
    {
        HalpProgramApicTimerClock(HalpCurrentTimeIncrement);
        return;
    }

    /* Acquire CMOS lock */
    HalpAcquireCmosSpinLock();

    // TODO: disable NMI

    /* Read value of register A */
    RegisterA = HalpReadCmos(RTC_REGISTER_A);

    /* Change lower 4 bits to new rate */
    RegisterA &= 0xF0;
    RegisterA |= ClockRate;

    /* Write the new value */
    HalpWriteCmos(RTC_REGISTER_A, RegisterA);

    /* Release CMOS lock */
    HalpReleaseCmosSpinLock();
}

CODE_SEG("INIT")
VOID
NTAPI
HalpInitializeClock(VOID)
{
    ULONG_PTR EFlags;
    UCHAR RegisterB;

    /* Save EFlags and disable interrupts */
    EFlags = __readeflags();
    _disable();

    if (HalpCanUseApicTimerClock())
    {
        HalpApicTimerClock = TRUE;
        KeSetProfileIrql(max(PROFILE_LEVEL, CLOCK_LEVEL));
        HalpCalibrateApicTimer();

        RtcSetClockRate(HalpCurrentClockRate);

        /* The timer gives any period exactly; the finest one is a millisecond
         * unless the local APIC is too slow to reach for a tick that short */
        HalpMaximumTimeIncrement = RtcClockRateToPreciseIncrement(RtcMaximumClockRate) / 1000;
        HalpMinimumTimeIncrement = HalpFinestApicTimerIncrement();
        KeSetTimeIncrement(HalpMaximumTimeIncrement, HalpMinimumTimeIncrement);

        __writeeflags(EFlags);
        DPRINT1("Clock initialized (local APIC timer), finest period %lu00 ns\n",
                HalpMinimumTimeIncrement);
        return;
    }

    // TODO: disable NMI

    /* Acquire CMOS lock */
    HalpAcquireCmosSpinLock();

    /* Enable the periodic interrupt in the CMOS */
    RegisterB = HalpReadCmos(RTC_REGISTER_B);
    HalpWriteCmos(RTC_REGISTER_B, RegisterB | RTC_REG_B_PI);

    /* Release CMOS lock */
    HalpReleaseCmosSpinLock();

    /* Set initial rate */
    RtcSetClockRate(HalpCurrentClockRate);

    /* Calculate minumum and maximum increment */
    HalpMinimumTimeIncrement = RtcClockRateToPreciseIncrement(RtcMinimumClockRate) / 1000;
    HalpMaximumTimeIncrement = RtcClockRateToPreciseIncrement(RtcMaximumClockRate) / 1000;

    /* Notify the kernel about the maximum and minimum increment */
    KeSetTimeIncrement(HalpMaximumTimeIncrement, HalpMinimumTimeIncrement);

    /* Enable the timer interrupt */
    HalEnableSystemInterrupt(APIC_CLOCK_VECTOR, CLOCK_LEVEL, Latched);

    /*
     * Firmware can leave an RTC interrupt pending, and calibrating the TSC
     * against the PM timer does not acknowledge it. Clear register C after
     * unmasking the IOAPIC route so the next periodic interrupt produces a
     * fresh edge. Keep CPU interrupts disabled until the RTC is rearmed.
     */
    HalpAcquireCmosSpinLock();
    HalpReadCmos(RTC_REGISTER_C);
    HalpReleaseCmosSpinLock();

    /* Restore interrupt state */
    __writeeflags(EFlags);

    DPRINT1("Clock initialized\n");
}

VOID
HalpRestoreClock(VOID)
{
    UCHAR RegisterB;

    if (HalpApicTimerClock)
    {
        HalpSetApicTimerIncrement(HalpCurrentTimeIncrement);
        return;
    }

    HalpAcquireCmosSpinLock();
    RegisterB = HalpReadCmos(RTC_REGISTER_B);
    HalpWriteCmos(RTC_REGISTER_B, RegisterB | RTC_REG_B_PI);
    HalpReleaseCmosSpinLock();

    RtcSetClockRate(HalpCurrentClockRate);

    HalpAcquireCmosSpinLock();
    HalpReadCmos(RTC_REGISTER_C);
    HalpReleaseCmosSpinLock();
}

VOID
FASTCALL
HalpClockInterruptHandler(IN PKTRAP_FRAME TrapFrame)
{
    ULONG LastIncrement;
    KIRQL Irql;

    /* Enter trap */
    KiEnterInterruptTrap(TrapFrame);
#ifdef _M_AMD64
    /* This is for debugging */
    TrapFrame->ErrorCode = 0xc10c4;
#endif

    /* Start the interrupt */
    if (!HalBeginSystemInterrupt(CLOCK_LEVEL, APIC_CLOCK_VECTOR, &Irql))
    {
        /* Spurious, just end the interrupt */
#ifdef _M_IX86
        KiEoiHelper(TrapFrame);
#endif
        return;
    }

    /* Read register C, so that the next interrupt can happen */
    if (!HalpApicTimerClock)
    {
        HalpAcquireCmosSpinLock();
        HalpReadCmos(RTC_REGISTER_C);
        HalpReleaseCmosSpinLock();
    }

    /* Save increment */
    LastIncrement = HalpCurrentTimeIncrement;
    if (HalpApicTimerClock)
    {
        /* The count that ran out says how long this tick took. The timer has
         * started it over, so nothing of the next tick is spent yet. */
        LastIncrement = HalpClockCountBase + HalpClockCountTime;
        if (HalpClockPendingIncrement != 0)
        {
            /* This tick ended a count that was replaced just as it ran out */
            LastIncrement = HalpClockPendingIncrement;
            HalpClockPendingIncrement = 0;
        }
        HalpClockCountBase = 0;
    }

    /* Check if the running fraction has accounted for 100 ns */
    HalpRunningFraction += HalpCurrentFractionalIncrement;
    if (HalpRunningFraction >= 1000)
    {
        LastIncrement++;
        HalpRunningFraction -= 1000;
    }

    /* Check if someone changed the time rate */
    if (HalpSetClockRate)
    {
        /* Set new clock rate */
        if (HalpApicTimerClock)
        {
            /* The timer takes it up below, with the tick it is due at */
            HalpCurrentTimeIncrement = HalpNextTimeIncrement;
            HalpCurrentFractionalIncrement = 0;
        }
        else
        {
            RtcSetClockRate(HalpNextClockRate);
        }

        /* We're done */
        HalpSetClockRate = FALSE;
    }

    /* Count to the base period or to the deadline, whichever is due first */
    if (HalpApicTimerClock)
        HalpAimClockTick(KeQueryInterruptTime() + LastIncrement, FALSE);

    /* Update AP runtime counters at the same cadence as the boot CPU. */
    HalpClockIpiTickOffset += (LONG)LastIncrement;
    if (HalpClockIpiTickOffset >= (LONG)HalpMaximumTimeIncrement)
    {
        HalpClockIpiTickOffset -= (LONG)HalpMaximumTimeIncrement;
        HalpBroadcastClockIpi(CLOCK_IPI_VECTOR);
    }

    if (HalpApicTimerClock && HalIsProfiling)
        KeProfileInterruptWithSource(TrapFrame, ProfileTime);

    /* Update the system time -- on x86 the kernel will exit this trap  */
    KeUpdateSystemTime(TrapFrame, LastIncrement, Irql);

    /* End the interrupt */
    KiEndInterrupt(Irql, TrapFrame);
}

VOID
FASTCALL
HalpClockIpiHandler(IN PKTRAP_FRAME TrapFrame)
{
    KIRQL Irql;

    /* Enter trap */
    KiEnterInterruptTrap(TrapFrame);
#ifdef _M_AMD64
    /* This is for debugging */
    TrapFrame->ErrorCode = 0xc10c4;
#endif

    /* Start the interrupt */
    if (!HalBeginSystemInterrupt(CLOCK_LEVEL, CLOCK_IPI_VECTOR, &Irql))
    {
        /* Spurious, just end the interrupt */
#ifdef _M_IX86
        KiEoiHelper(TrapFrame);
#endif
        return;
    }

    if (HalpApicTimerClock && HalIsProfiling)
        KeProfileInterruptWithSource(TrapFrame, ProfileTime);

    /* Call the kernel to update runtimes */
    KeUpdateRunTime(TrapFrame, Irql);

    /* End the interrupt */
    KiEndInterrupt(Irql, TrapFrame);
}

ULONG
NTAPI
HalSetTimeIncrement(IN ULONG Increment)
{
    UCHAR Rate;
    ULONG NextIncrement;

    if (HalpApicTimerClock)
    {
        Increment = min(max(Increment, HalpMinimumTimeIncrement), HalpMaximumTimeIncrement);
        HalpNextTimeIncrement = Increment;
        HalpSetClockRate = TRUE;
        return Increment;
    }

    /* Lookup largest value below given Increment */
    for (Rate = RtcMinimumClockRate; Rate < RtcMaximumClockRate; Rate++)
    {
        /* Check if this is the largest rate possible */
        NextIncrement = RtcClockRateToPreciseIncrement(Rate + 1) / 1000;
        if (NextIncrement > Increment)
            break;
    }

    /* Set the rate and tell HAL we want to change it */
    HalpNextClockRate = Rate;
    HalpSetClockRate = TRUE;

    /* Return the real increment */
    return RtcClockRateToPreciseIncrement(Rate) / 1000;
}

/*
 * Has the clock tick at an interrupt time of the kernel's choosing, so that
 * what waits on that time need not have the clock run at its finest period.
 * Zero withdraws the deadline. On processor 0, whose timer is the clock, the
 * deadline takes effect here. On any other the caller hands it on to processor
 * 0, and the result only tells whether the clock takes a deadline at all.
 */
BOOLEAN
NTAPI
HalSetClockDeadline(_In_ ULONGLONG InterruptTime)
{
    ULONG_PTR Flags;

    /* The RTC only ticks at its own rates */
    if (!HalpApicTimerClock)
        return FALSE;

    if (KeGetCurrentPrcb()->Number != 0)
        return TRUE;

    Flags = __readeflags();
    _disable();

    HalpClockDeadline = InterruptTime;
    HalpAimClockTick(KeQueryInterruptTime(), TRUE);

    __writeeflags(Flags);
    return TRUE;
}
