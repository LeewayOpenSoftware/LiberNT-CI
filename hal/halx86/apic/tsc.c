/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/apic/tsc.c
 * PURPOSE:         HAL Routines for TSC handling
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include "tsc.h"
#include "apicp.h"
#include "halstall.h"
#define NDEBUG
#include <debug.h>

LARGE_INTEGER HalpCpuClockFrequency = {{INITIAL_STALL_COUNT * 1000000}};

UCHAR TscCalibrationPhase;

/* The performance counter is the TSC shifted right by HalpQpcShift, plus
 * HalpQpcBias, at HalpQpcFrequency counts per second. Kernel and user mode
 * (through KUSER_SHARED_DATA) compute the same value from the same formula. */
static UCHAR HalpQpcShift;
static ULONG64 HalpQpcFrequency;
static ULONG64 HalpQpcBias;
static BOOLEAN HalpQpcReady;
/* Added to the scaled TSC before HalpQpcReady (HalCalibratePerformanceCounter) */
static ULONG64 HalpEarlyQpcBias;
/* Incremented by the last processor into HalCalibratePerformanceCounter */
static volatile LONG HalpCalibrateRelease;
ULONG64 TscCalibrationArray[NUM_SAMPLES];

#define HALP_PERFORMANCE_FREQUENCY 10000000ULL
#define RTC_MODE 6 /* Mode 6 is 1024 Hz */
#define SAMPLE_FREQUENCY ((32768 << 1) >> RTC_MODE)

#define PM_TIMER_FREQUENCY 3579545UL
#define PM_CALIBRATION_TICKS (PM_TIMER_FREQUENCY / 20) /* 50 ms */
#define PM_CALIBRATION_MIN_TICKS (PM_TIMER_FREQUENCY / 100) /* 10 ms */
#define PM_CALIBRATION_MAX_POLLS 1000000UL

/* PRIVATE FUNCTIONS *********************************************************/

static
ULONG64
HalpReadOrderedTsc(VOID)
{
    int CpuInfo[4];

    /* CPUID also works on pre-SSE2 processors supported by the x86 HAL. */
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("cpuid"
                         : "=a" (CpuInfo[0]), "=b" (CpuInfo[1]),
                           "=c" (CpuInfo[2]), "=d" (CpuInfo[3])
                         : "a" (0)
                         : "memory");
#else
    {
        volatile int SerializingValue;
        __cpuid(CpuInfo, 0);
        SerializingValue = CpuInfo[0];
        _ReadWriteBarrier();
    }
#endif
    return __rdtsc();
}

/* A Hyper-V compatible hypervisor that grants the frequency MSRs reports the
 * TSC and local APIC timer frequencies directly. Measuring them against its
 * emulated timers can be off when its virtual clock is catching up. */
BOOLEAN
HalpReadHypervisorFrequency(
    _In_ ULONG Msr,
    _Out_ PULONG64 Frequency)
{
    INT CpuInfo[4];

    *Frequency = 0;

    __cpuid(CpuInfo, 1);
    if (!(CpuInfo[2] & 0x80000000))
        return FALSE;

    __cpuid(CpuInfo, 0x40000000);
    if ((ULONG)CpuInfo[0] < 0x40000003)
        return FALSE;

    __cpuid(CpuInfo, 0x40000001);
    if ((ULONG)CpuInfo[0] != HV_CPUID_INTERFACE_HV1)
        return FALSE;

    __cpuid(CpuInfo, 0x40000003);
    if (!(CpuInfo[0] & HV_ACCESS_FREQUENCY_MSRS) || !(CpuInfo[3] & HV_FREQUENCY_MSRS_AVAILABLE))
        return FALSE;

    *Frequency = __readmsr(Msr);
    return *Frequency != 0;
}

static
BOOLEAN
HalpCalibrateTscWithPmTimer(OUT PULONG64 Frequency)
{
    ULONG Port, Mask, Start, Elapsed, Poll, Sample, Attempt;
    ULONG64 StartTsc, DeltaTsc, Samples[3], Swap;

    if (!HalpGetPmTimer(&Port, &Mask))
        return FALSE;

    Sample = 0;
    for (Attempt = 0; Attempt < 9 && Sample < RTL_NUMBER_OF(Samples); ++Attempt)
    {
        Start = READ_PORT_ULONG((PULONG)(ULONG_PTR)Port) & Mask;
        StartTsc = HalpReadOrderedTsc();

        /* Read the counter itself, not the number of delivered interrupts.
         * A bounded poll also handles absent or stopped firmware timers. */
        for (Poll = 0; Poll < PM_CALIBRATION_MAX_POLLS; ++Poll)
        {
            Elapsed = (READ_PORT_ULONG((PULONG)(ULONG_PTR)Port) - Start) & Mask;
            if (Elapsed >= PM_CALIBRATION_TICKS)
                break;
        }
        DeltaTsc = HalpReadOrderedTsc() - StartTsc;

        /* Reject backwards/glitching timers and heavily interrupted samples.
         * The mask above handles either 24-bit or 32-bit counter wraparound. */
        /* Fast port emulation can exhaust the polling budget before 50 ms.
         * A shorter, sufficiently long interval is still valid: the divisor
         * is the measured PM delta, not the requested duration or poll count. */
        if (Elapsed < PM_CALIBRATION_MIN_TICKS)
            return FALSE;

        if (Elapsed > PM_TIMER_FREQUENCY / 5 ||
            !DeltaTsc || DeltaTsc > MAXULONGLONG / PM_TIMER_FREQUENCY)
        {
            continue;
        }

        Samples[Sample] = DeltaTsc * PM_TIMER_FREQUENCY / Elapsed;
        if (Samples[Sample])
            ++Sample;
    }

    if (Sample != RTL_NUMBER_OF(Samples))
        return FALSE;

    /* Use the median so a single SMI or VM descheduling at an endpoint does
     * not determine the frequency used by every subsequent QPC call. */
    for (Sample = 0; Sample < 2; ++Sample)
    {
        for (Poll = Sample + 1; Poll < 3; ++Poll)
        {
            if (Samples[Poll] < Samples[Sample])
            {
                Swap = Samples[Sample];
                Samples[Sample] = Samples[Poll];
                Samples[Poll] = Swap;
            }
        }
    }
    *Frequency = Samples[1];
    return TRUE;
}

static
ULONG64
DoLinearRegression(
    ULONG XMax,
    ULONG64 *ArrayY)
{
    ULONG X, SumXX;
    ULONG64 SumXY;

    /* Calculate the sum of the squares of X */
    SumXX = (XMax * (XMax + 1) * (2*XMax + 1)) / 6;

    /* Calculate the sum of the differences to the first value
       weighted by x */
    for (SumXY = 0, X = 1; X <= XMax; X++)
    {
         SumXY += X * (ArrayY[X] - ArrayY[0]);
    }

    /* Account for sample frequency */
    SumXY *= SAMPLE_FREQUENCY;

    /* Return the quotient of the sums */
    return (SumXY + (SumXX/2)) / SumXX;
}

VOID
NTAPI
HalpInitializeTsc(VOID)
{
    ULONG_PTR Flags;
    PVOID PreviousHandler;
    UCHAR RegisterA, RegisterB;
    ULONG64 Frequency;

    /* Check if the CPU supports RDTSC */
    if (!(KeGetCurrentPrcb()->FeatureBits & KF_RDTSC))
    {
        KeBugCheck(HAL_INITIALIZATION_FAILED);
    }

    if (HalpReadHypervisorFrequency(HV_X64_MSR_TSC_FREQUENCY, &Frequency))
    {
        HalpCpuClockFrequency.QuadPart = Frequency;
        DPRINT1("TSC frequency from the hypervisor: %I64u Hz\n", Frequency);
        return;
    }

     /* Save flags and disable interrupts */
    Flags = __readeflags();
    _disable();

    if (HalpCalibrateTscWithPmTimer(&Frequency))
    {
        HalpCpuClockFrequency.QuadPart = Frequency;
        __writeeflags(Flags);
        return;
    }

    TscCalibrationPhase = 0;

    /* Enable the periodic interrupt in the CMOS */
    RegisterB = HalpReadCmos(RTC_REGISTER_B);
    HalpWriteCmos(RTC_REGISTER_B, RegisterB | RTC_REG_B_PI);

    /* Modify register A to RTC_MODE to get SAMPLE_FREQUENCY */
    RegisterA = HalpReadCmos(RTC_REGISTER_A);
    HalpWriteCmos(RTC_REGISTER_A, (RegisterA & 0xF0) | RTC_MODE);

    /* Save old IDT entry */
    PreviousHandler = KeQueryInterruptHandler(APIC_CLOCK_VECTOR);

    /* Set the calibration ISR */
    KeRegisterInterruptHandler(APIC_CLOCK_VECTOR, TscCalibrationISR);

    /* Enable the timer interrupt */
    HalEnableSystemInterrupt(APIC_CLOCK_VECTOR, CLOCK_LEVEL, Latched);

    /* Read register C, so that the next interrupt can happen */
    HalpReadCmos(RTC_REGISTER_C);

    /* Wait for completion */
    _enable();
    while (TscCalibrationPhase < NUM_SAMPLES) _ReadWriteBarrier();
    _disable();

    /* Disable the periodic interrupt in the CMOS */
    HalpWriteCmos(RTC_REGISTER_B, RegisterB & ~RTC_REG_B_PI);
    HalpWriteCmos(RTC_REGISTER_A, RegisterA);

    /* Disable the timer interrupt */
    HalDisableSystemInterrupt(APIC_CLOCK_VECTOR, CLOCK_LEVEL);

    /* Restore the previous handler */
    KeRegisterInterruptHandler(APIC_CLOCK_VECTOR, PreviousHandler);

    /* Calculate an average, using simplified linear regression */
    HalpCpuClockFrequency.QuadPart = DoLinearRegression(NUM_SAMPLES - 1,
                                                        TscCalibrationArray);

    /* Restore flags */
    __writeeflags(Flags);

}

CODE_SEG("INIT")
static
VOID
HalpInitializeQpc(VOID)
{
    ULONG64 Frequency = HalpCpuClockFrequency.QuadPart;
    ULONG64 Tsc, Current;
    UCHAR Shift = 0;

    /* Largest shift that keeps the counter at 10 MHz or faster, like the
     * fixed 10 MHz it replaces. */
    while (Shift < 32 && (Frequency >> (Shift + 1)) >= HALP_PERFORMANCE_FREQUENCY)
        Shift++;

    /* Continue from the value the 10 MHz counter has right now, so a time
     * stamp taken before this point never ends up ahead of a later one. */
    Tsc = __rdtsc();
    Current = (Tsc / Frequency) * HALP_PERFORMANCE_FREQUENCY +
              ((Tsc % Frequency) * HALP_PERFORMANCE_FREQUENCY) / Frequency +
              HalpEarlyQpcBias;

    HalpQpcShift = Shift;
    HalpQpcFrequency = Frequency >> Shift;
    HalpQpcBias = Current - (Tsc >> Shift);
    HalpQpcReady = TRUE;

#ifdef _M_AMD64
    {
        PKUSER_SHARED_DATA Data = (PKUSER_SHARED_DATA)KI_USER_SHARED_DATA;

        /* Publish the data first, the enable byte last: a reader that sees
         * it enabled sees a complete configuration. */
        Data->QpcFrequency = HalpQpcFrequency;
        Data->QpcBias = HalpQpcBias;
        Data->QpcShift = Shift;
        KeMemoryBarrier();
        Data->QpcBypassEnabled = 1;
    }
#else
    /* Nothing is published for user mode on x86. There, offset 0x300 of
     * KUSER_SHARED_DATA (QpcFrequency on x64) still holds the NT5
     * SystemCall stub pointer that ntdll calls through, and the x86 ntdll
     * has no user-mode counter. The bypass stays off, and
     * QueryPerformanceCounter keeps taking the system call, which returns
     * this same counter. */
#endif
}

CODE_SEG("INIT")
VOID
NTAPI
HalpCalibrateStallExecution(VOID)
{
    // Timer interrupt is now active

    HalpInitializeTsc();
    HalpInitializeQpc();

    HalpStallScaleFactor = (ULONG)(HalpCpuClockFrequency.QuadPart / 1000000);
    KeGetPcr()->StallScaleFactor = HalpStallScaleFactor;
}

/* PUBLIC FUNCTIONS ***********************************************************/

LARGE_INTEGER
NTAPI
KeQueryPerformanceCounter(
    OUT PLARGE_INTEGER PerformanceFrequency OPTIONAL)
{
    LARGE_INTEGER Result;
    ULONG64 Tsc, Seconds, Remainder, Frequency;

    if (HalpQpcReady)
    {
        if (PerformanceFrequency)
            PerformanceFrequency->QuadPart = HalpQpcFrequency;

        Result.QuadPart = (__rdtsc() >> HalpQpcShift) + HalpQpcBias;
        return Result;
    }

    /* Before the calibration is done: the initial estimate, scaled to 10 MHz */
    ASSERT(HalpCpuClockFrequency.QuadPart != 0);

    if (PerformanceFrequency)
        PerformanceFrequency->QuadPart = HALP_PERFORMANCE_FREQUENCY;

    Frequency = HalpCpuClockFrequency.QuadPart;
    Tsc = __rdtsc();
    Seconds = Tsc / Frequency;
    Remainder = Tsc % Frequency;
    Result.QuadPart = Seconds * HALP_PERFORMANCE_FREQUENCY + (Remainder * HALP_PERFORMANCE_FREQUENCY) / Frequency +
                      HalpEarlyQpcBias;
    return Result;
}

VOID
NTAPI
KeStallExecutionProcessor(ULONG MicroSeconds)
{
    ULONG64 StartTime, EndTime;

    /* Get the initial time */
    StartTime = __rdtsc();

    /* Calculate the ending time */
    EndTime = HalpComputeStallEndTime(StartTime,
                                      KeGetPcr()->StallScaleFactor,
                                      MicroSeconds);

    /* Loop until time is elapsed */
    while (__rdtsc() < EndTime);
}

/*
 * Called on every processor at the same time, with *Count initialised by the
 * caller to the number of processors taking part. The performance counter is
 * set to NewCount on all of them at one instant.
 *
 * The counter is (TSC >> HalpQpcShift) + HalpQpcBias on every processor and
 * in user mode (KUSER_SHARED_DATA), so it has one base for the whole system:
 * the bias. Like KeQueryPerformanceCounter, this relies on the processors'
 * TSCs being synchronised; the TSC itself is not written, because
 * KeStallExecutionProcessor and the user-mode counter read it directly.
 *
 * Each processor disables interrupts and checks in by decrementing *Count.
 * The last one in rebases the counter on its TSC while all others spin here
 * with interrupts disabled, so no processor (and no user-mode thread) reads
 * the counter while the bias changes. It then releases the others.
 */
VOID
NTAPI
HalCalibratePerformanceCounter(
    IN volatile PLONG Count,
    IN ULONGLONG NewCount)
{
    ULONG_PTR Flags;
    LONG Release;
    ULONG64 Tsc, Frequency;

    Flags = __readeflags();
    _disable();

    /* Read the release generation before checking in: it cannot change
     * until every processor, this one included, has decremented Count. */
    Release = HalpCalibrateRelease;
    _ReadWriteBarrier();

    if (_InterlockedDecrement(Count) == 0)
    {
        Tsc = __rdtsc();

        if (HalpQpcReady)
        {
            HalpQpcBias = NewCount - (Tsc >> HalpQpcShift);
#ifdef _M_AMD64
            /* One aligned 64-bit store; user mode cannot run on any
             * processor until they are released below. */
            ((PKUSER_SHARED_DATA)KI_USER_SHARED_DATA)->QpcBias = HalpQpcBias;
#endif
        }
        else
        {
            /* Before HalpInitializeQpc the counter is the TSC scaled to
             * HALP_PERFORMANCE_FREQUENCY, plus the early bias. */
            Frequency = HalpCpuClockFrequency.QuadPart;
            HalpEarlyQpcBias = NewCount -
                ((Tsc / Frequency) * HALP_PERFORMANCE_FREQUENCY +
                 ((Tsc % Frequency) * HALP_PERFORMANCE_FREQUENCY) / Frequency);
        }

        /* Publish the new base, then release the other processors */
        _InterlockedIncrement(&HalpCalibrateRelease);
    }
    else
    {
        while (HalpCalibrateRelease == Release)
            YieldProcessor();
    }

    __writeeflags(Flags);
}

