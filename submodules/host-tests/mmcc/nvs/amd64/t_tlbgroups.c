/*
 * PROJECT:     LiberNT host-native tests
 * FILE:        submodules/host-tests/mmcc/nvs/amd64/t_tlbgroups.c
 * PURPOSE:     AMD64 TLB shootdown across two processor groups
 *
 * SPDX-FileCopyrightText: 2026 LiberNT contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "tlbshim.h"
#include <nvs/arch/amd64/tlb.h>
#include <nvs/arch/amd64/hardware.h>

#define TEST_CPUS MI_AMD64_TLB_MAXIMUM_PROCESSORS
#define TEST_GROUPS ((TEST_CPUS + 63) / 64)

typedef struct _TEST_CPU
{
    ULONG64 Flushes, PageFlushes;
    ULONG64 Sources[TEST_GROUPS];
    BOOLEAN Enabled;
    KIRQL Irql;
} TEST_CPU;

static TEST_CPU Cpus[TEST_CPUS];
static ULONG CurrentCpu;
static ULONG64 Active[TEST_GROUPS];
static ULONG64 Ipis[TEST_GROUPS];
static ULONG Checks, Failures, Polls;

#define CHECK(e) do { Checks++; if (!(e)) { if (Failures++ < 20) fprintf(stderr, "cpu%u %u: %s\n", CurrentCpu, __LINE__, #e); } } while (0)

ULONG MiAmd64TlbCpu(VOID) { return CurrentCpu; }
ULONG64 MiAmd64TlbActiveCpus(VOID) { return Active[0]; }
ULONG MiAmd64TlbGroupCount(VOID) { return TEST_GROUPS; }
ULONG64 MiAmd64TlbActiveGroup(ULONG Group) { CHECK(Group < TEST_GROUPS); return Active[Group]; }
ULONG64 MiAmd64TlbReadCr3(VOID) { return 0x1000; }
ULONG64 MiAmd64TlbReadCr4(VOID) { return 0; }
VOID MiAmd64TlbWriteCr3(ULONG64 Value) { UNREFERENCED_PARAMETER(Value); Cpus[CurrentCpu].Flushes++; }
VOID MiAmd64TlbWriteCr4(ULONG64 Value) { UNREFERENCED_PARAMETER(Value); CHECK(FALSE); }
VOID MiAmd64TlbPage(ULONG64 Address) { UNREFERENCED_PARAMETER(Address); Cpus[CurrentCpu].PageFlushes++; }
VOID MiAmd64TlbCpuid(int Registers[4], int Leaf) { UNREFERENCED_PARAMETER(Leaf); memset(Registers, 0, 4 * sizeof(int)); }
VOID MiAmd64TlbPause(VOID) { }
BOOLEAN MiAmd64TlbWatchdogEnabled(VOID) { return FALSE; }
BOOLEAN MiAmd64TlbWatchdogFreezeActive(VOID) { return FALSE; }
ULONG64 MiAmd64TlbWatchdogEpoch(VOID) { return 0; }
ULONG64 MiAmd64TlbWatchdogNow(PULONG64 Frequency) { *Frequency = 1; return 0; }
VOID MiAmd64TlbWatchdogBugcheck(ULONG64 Frequency, ULONG64 Now, ULONG64 Baseline)
{
    UNREFERENCED_PARAMETER(Frequency);
    UNREFERENCED_PARAMETER(Now);
    UNREFERENCED_PARAMETER(Baseline);
    CHECK(FALSE);
}
VOID MiAmd64TlbInvpcid(VOID) { CHECK(FALSE); }
VOID MiAmd64TlbInvpcidNonGlobal(VOID) { CHECK(FALSE); }

BOOLEAN MiAmd64TlbDisableInterrupts(VOID)
{
    BOOLEAN Previous = Cpus[CurrentCpu].Enabled;
    Cpus[CurrentCpu].Enabled = FALSE;
    return Previous;
}

VOID MiAmd64TlbRestoreInterrupts(BOOLEAN Enabled) { Cpus[CurrentCpu].Enabled = Enabled; }

KIRQL MiAmd64TlbRaiseIrql(VOID)
{
    KIRQL Previous = Cpus[CurrentCpu].Irql;
    if (Previous < 12)
        Cpus[CurrentCpu].Irql = 12;
    return Previous;
}

VOID MiAmd64TlbLowerIrql(KIRQL Previous) { Cpus[CurrentCpu].Irql = Previous; }

VOID KiSendMemoryIpi(ULONG64 Targets) { KiSendMemoryIpiGroup(0, Targets); }

/* Queues the sender in every target's mailbox, as the kernel does */
VOID KiSendMemoryIpiGroup(ULONG Group, ULONG64 Targets)
{
    ULONG Bit, Target;

    CHECK(Group < TEST_GROUPS);
    CHECK((Targets & ~Active[Group]) == 0);
    CHECK(Group != CurrentCpu / 64 || !(Targets & (1ULL << (CurrentCpu % 64))));
    Ipis[Group] |= Targets;
    for (; Targets != 0; Targets &= Targets - 1)
    {
        Bit = (ULONG)__builtin_ctzll(Targets);
        Target = Group * 64 + Bit;
        Cpus[Target].Sources[CurrentCpu / 64] |= 1ULL << (CurrentCpu % 64);
    }
}

/* Every interrupted processor runs its mailbox */
VOID MiAmd64TlbPoll(VOID)
{
    ULONG Sender = CurrentCpu, Target, Group;
    ULONG64 Sources;

    /* A request no target was asked for would be waited on forever */
    if (++Polls > 1000)
    {
        fprintf(stderr, "cpu%u: TLB request never acknowledged\n", Sender);
        exit(1);
    }
    for (Target = 0; Target < TEST_CPUS; Target++)
    {
        for (Group = 0; Group < TEST_GROUPS; Group++)
        {
            Sources = Cpus[Target].Sources[Group];
            Cpus[Target].Sources[Group] = 0;
            CurrentCpu = Target;
            for (; Sources != 0; Sources &= Sources - 1)
                MiAmd64ProcessTlbRequest(Group * 64 + (ULONG)__builtin_ctzll(Sources));
        }
    }
    CurrentCpu = Sender;
}

static VOID Reset(ULONG Cpu)
{
    memset(Cpus, 0, sizeof(Cpus));
    memset(Ipis, 0, sizeof(Ipis));
    Polls = 0;
    CurrentCpu = Cpu;
}

static BOOLEAN IsActive(ULONG Cpu) { return (Active[Cpu / 64] & (1ULL << (Cpu % 64))) != 0; }

int main(VOID)
{
    ULONG Cpu;

    C_ASSERT(TEST_GROUPS == 2);
    Active[0] = ~(1ULL << 5);
    Active[1] = ((1ULL << 40) - 1) & ~(1ULL << 10);

    /* All processors from group 1: every active processor of both groups */
    Reset(70);
    MiArchInvalidateTlbAll(MiTlbAllProcessors);
    CHECK(Ipis[0] == Active[0]);
    CHECK(Ipis[1] == (Active[1] & ~(1ULL << 6)));
    for (Cpu = 0; Cpu < TEST_CPUS; Cpu++)
        CHECK(Cpus[Cpu].Flushes == (IsActive(Cpu) ? 1u : 0u) && Cpus[Cpu].PageFlushes == 0);

    /* A range for all processors from the last active one */
    Reset(103);
    MiArchInvalidateTlbRange((PVOID)0x5000, 2 * PAGE_SIZE, MiTlbAllProcessors);
    for (Cpu = 0; Cpu < TEST_CPUS; Cpu++)
        CHECK(Cpus[Cpu].PageFlushes == (IsActive(Cpu) ? 2u : 0u) && Cpus[Cpu].Flushes == 0);

    /* Local scope stays local */
    Reset(100);
    MiArchInvalidateTlbAll(MiTlbLocal);
    CHECK(Ipis[0] == 0 && Ipis[1] == 0);
    for (Cpu = 0; Cpu < TEST_CPUS; Cpu++)
        CHECK(Cpus[Cpu].Flushes == (Cpu == 100 ? 1u : 0u));

    /* A group 0 processor set plus the current processor, from group 0 */
    Reset(3);
    MiArchTlbInvalidateProcessors(0x1000, 1, (1ULL << 1) | (1ULL << 5) | (1ULL << 9));
    CHECK(Ipis[0] == ((1ULL << 1) | (1ULL << 9)) && Ipis[1] == 0);
    for (Cpu = 0; Cpu < TEST_CPUS; Cpu++)
        CHECK(Cpus[Cpu].PageFlushes == ((Cpu == 1 || Cpu == 3 || Cpu == 9) ? 1u : 0u));

    /* The same from group 1: the current processor is still flushed */
    Reset(65);
    MiArchTlbInvalidateProcessors(0x1000, 1, 1ULL << 2);
    CHECK(Ipis[0] == (1ULL << 2) && Ipis[1] == 0);
    for (Cpu = 0; Cpu < TEST_CPUS; Cpu++)
        CHECK(Cpus[Cpu].PageFlushes == ((Cpu == 2 || Cpu == 65) ? 1u : 0u));

    /* A group 0 mask from group 1 does not name the current processor */
    Reset(64);
    MiAmd64FlushTargets(~0ULL, 0x2000, 1);
    CHECK(Ipis[0] == Active[0] && Ipis[1] == 0);
    for (Cpu = 0; Cpu < TEST_CPUS; Cpu++)
        CHECK(Cpus[Cpu].PageFlushes == ((Cpu < 64 && IsActive(Cpu)) ? 1u : 0u));

    /* Spurious mailbox runs do nothing */
    Reset(80);
    for (Cpu = 0; Cpu < TEST_CPUS; Cpu++)
        if (IsActive(Cpu))
        {
            CurrentCpu = Cpu;
            MiAmd64ProcessTlbRequest(80);
            MiAmd64ProcessTlbRequest(0);
        }
    for (Cpu = 0; Cpu < TEST_CPUS; Cpu++)
        CHECK(Cpus[Cpu].Flushes == 0 && Cpus[Cpu].PageFlushes == 0);

    printf("AMD64 TLB groups: %u checks, %u failures\n", Checks, Failures);
    return Failures != 0;
}
