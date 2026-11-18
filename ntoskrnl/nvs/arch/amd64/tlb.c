/*
 * PROJECT:     LiberNT NT Virtual Memory Subsystem
 * FILE:        ntoskrnl/nvs/arch/amd64/tlb.c
 * PURPOSE:     AMD64 translation lookaside buffer maintenance
 *
 * SPDX-FileCopyrightText: 2026 Ahmed ARIF
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifdef MM_HOST_TEST
#include <mmcc/nvs/amd64/tlbshim.h>
#else
#include <nvs/nt/mint.h>
#endif
#include "hardware.h"
#include "tlb.h"
#include "tlbops.h"

#define MI_AMD64_TLB_WATCHDOG_SECONDS 10

/*
 * One message per sending processor. Each pending word holds the request
 * sequence in its high half and 32 target processors in its low half; a
 * target acknowledges by clearing its bit while the sequence is unchanged,
 * so a stale acknowledgement cannot complete a newer request. The sequence
 * is 32 bits: a stale acknowledgement could only match again after 2^32
 * further requests from the same sender while its handler stays interrupted.
 * Processors are numbered by index: group Index / 64, number Index % 64.
 */
#define MI_AMD64_TLB_TARGETS_PER_WORD 32
#define MI_AMD64_TLB_GROUP_SIZE 64
#define MI_AMD64_TLB_GROUPS \
    ((MI_AMD64_TLB_MAXIMUM_PROCESSORS + MI_AMD64_TLB_GROUP_SIZE - 1) / MI_AMD64_TLB_GROUP_SIZE)
#define MI_AMD64_TLB_TARGET_WORDS \
    ((MI_AMD64_TLB_MAXIMUM_PROCESSORS + MI_AMD64_TLB_TARGETS_PER_WORD - 1) / MI_AMD64_TLB_TARGETS_PER_WORD)
#define MI_AMD64_TLB_TARGET_BITS(Word) ((Word) & 0xFFFFFFFFULL)
#define MI_AMD64_TLB_SEQUENCE(Word) ((Word) >> 32)

typedef struct MI_CACHE_ALIGNED _MI_AMD64_TLB_MESSAGE
{
    ULONG64 Address;
    ULONG64 Pages;
    ULONG64 Sequence;
    ULONG64 Pending[MI_AMD64_TLB_TARGET_WORDS];
} MI_AMD64_TLB_MESSAGE;

/* A target set: one 64-processor mask per group */
typedef struct _MI_AMD64_TLB_SET
{
    ULONG64 Group[MI_AMD64_TLB_GROUPS];
} MI_AMD64_TLB_SET;

static MI_AMD64_TLB_MESSAGE MiAmd64TlbMessages[MI_AMD64_TLB_MAXIMUM_PROCESSORS];
static ULONG MiAmd64TlbFeatures[MI_AMD64_TLB_MAXIMUM_PROCESSORS];
volatile LONG KiTbFlushTimeStamp;
typedef struct _MI_AMD64_TLB_WATCHDOG
{
    ULONG64 Baseline;
    ULONG64 Epoch;
    ULONG64 Remaining;
    ULONG Samples;
    BOOLEAN Valid;
} MI_AMD64_TLB_WATCHDOG;

static
BOOLEAN
MiAmd64TlbWatchdogExpired(MI_AMD64_TLB_WATCHDOG *Watchdog, ULONG64 Remaining,
                          PULONG64 Frequency, PULONG64 Now)
{
    ULONG64 Before, After;

    if (Watchdog->Remaining != Remaining)
    {
        Watchdog->Remaining = Remaining;
        Watchdog->Valid = FALSE;
    }

    if (!MiAmd64TlbWatchdogEnabled())
    {
        Watchdog->Valid = FALSE;
        return FALSE;
    }

    if (Watchdog->Valid && ++Watchdog->Samples < 1024)
        return FALSE;
    Watchdog->Samples = 0;

    Before = MiAmd64TlbWatchdogEpoch();
    *Now = MiAmd64TlbWatchdogNow(Frequency);
    After = MiAmd64TlbWatchdogEpoch();
    if (Before != After || MiAmd64TlbWatchdogFreezeActive() ||
        !MiAmd64TlbWatchdogEnabled() || *Frequency == 0)
    {
        Watchdog->Valid = FALSE;
        return FALSE;
    }

    if (!Watchdog->Valid || Watchdog->Epoch != After || *Now < Watchdog->Baseline)
    {
        Watchdog->Baseline = *Now;
        Watchdog->Epoch = After;
        Watchdog->Valid = TRUE;
        return FALSE;
    }

    return (*Now - Watchdog->Baseline) / *Frequency >= MI_AMD64_TLB_WATCHDOG_SECONDS;
}

ULONG
MiAmd64TlbCapabilities(VOID)
{
    BOOLEAN Enabled = MiAmd64TlbDisableInterrupts();
    ULONG Cpu = MiAmd64TlbCpu();
    ULONG Features;
    int Registers[4];

    MI_ASSERT(Cpu < MI_AMD64_TLB_MAXIMUM_PROCESSORS);
    Features = MI_AMD64_READ_ACQUIRE32(&MiAmd64TlbFeatures[Cpu]);
    if (Features == 0)
    {
        Features = 1;
        MiAmd64TlbCpuid(Registers, 0);
        if ((ULONG)Registers[0] >= 7)
        {
            MiAmd64TlbCpuid(Registers, 7);
            if (Registers[1] & (1 << 10))
                Features |= MI_AMD64_TLB_INVPCID;
        }
        MI_AMD64_WRITE_RELEASE32(&MiAmd64TlbFeatures[Cpu], Features);
    }
    MiAmd64TlbRestoreInterrupts(Enabled);
    return Features & ~1u;
}

static
VOID
MiAmd64FlushLocal(ULONG64 Address, ULONG64 Pages)
{
    BOOLEAN Enabled = MiAmd64TlbDisableInterrupts();

    if (Pages == MI_AMD64_TLB_NON_GLOBAL && (MiAmd64TlbCapabilities() & MI_AMD64_TLB_INVPCID))
    {
        MiAmd64TlbInvpcidNonGlobal();
    }
    else if (Pages == MI_AMD64_TLB_NON_GLOBAL && !(MiAmd64TlbReadCr4() & (1ULL << 17)))
    {
        MiAmd64TlbWriteCr3(MiAmd64TlbReadCr3() & ~(1ULL << 63));
    }
    else if (Pages != 0 && Pages != MI_AMD64_TLB_NON_GLOBAL)
    {
        do
        {
            MiAmd64TlbPage(Address);
            Address += PAGE_SIZE;
        } while (--Pages != 0);
    }
    else if (MiAmd64TlbCapabilities() & MI_AMD64_TLB_INVPCID)
    {
        MiAmd64TlbInvpcid();
    }
    else
    {
        ULONG64 Cr4 = MiAmd64TlbReadCr4();
        if (Cr4 & ((1ULL << 7) | (1ULL << 17)))
        {
            MiAmd64TlbWriteCr4(Cr4 ^ (1ULL << 7));
            MiAmd64TlbWriteCr4(Cr4);
        }
        else
        {
            MiAmd64TlbWriteCr3(MiAmd64TlbReadCr3() & ~(1ULL << 63));
        }
    }
    MiAmd64TlbRestoreInterrupts(Enabled);
}

static
BOOLEAN
MiAmd64TlbTargetPending(ULONG64 Word, ULONG64 Sequence, ULONG64 Bit)
{
    return MI_AMD64_TLB_SEQUENCE(Word) == Sequence && (Word & Bit) != 0;
}

VOID
MiAmd64ProcessTlbRequest(ULONG Source)
{
    BOOLEAN Enabled = MiAmd64TlbDisableInterrupts();
    ULONG Cpu = MiAmd64TlbCpu();
    MI_AMD64_TLB_MESSAGE *Message;
    ULONG64 *Pending;
    ULONG64 Word, Sequence, Bit, Address, Pages;

    MI_ASSERT(Cpu < MI_AMD64_TLB_MAXIMUM_PROCESSORS && Source < MI_AMD64_TLB_MAXIMUM_PROCESSORS);
    Message = &MiAmd64TlbMessages[Source];
    Pending = &Message->Pending[Cpu / MI_AMD64_TLB_TARGETS_PER_WORD];
    Bit = 1ULL << (Cpu % MI_AMD64_TLB_TARGETS_PER_WORD);
    Word = MI_AMD64_READ_ACQUIRE64(Pending);
    Sequence = MI_AMD64_TLB_SEQUENCE(Word);
    if (Word & Bit)
    {
        /* Acquire loads keep the payload ahead of the recheck below */
        Address = MI_AMD64_READ_ACQUIRE64(&Message->Address);
        Pages = MI_AMD64_READ_ACQUIRE64(&Message->Pages);
        Word = MI_AMD64_READ_ACQUIRE64(Pending);
        if (MiAmd64TlbTargetPending(Word, Sequence, Bit))
        {
            MiAmd64FlushLocal(Address, Pages);
            for (;;)
            {
                ULONG64 Expected = MI_AMD64_READ_ACQUIRE64(Pending);
                if (!MiAmd64TlbTargetPending(Expected, Sequence, Bit) ||
                    MI_AMD64_COMPARE_EXCHANGE64(Pending, Expected, Expected & ~Bit))
                    break;
            }
        }
    }
    MiAmd64TlbRestoreInterrupts(Enabled);
}

static
ULONG
MiAmd64TlbPendingTargets(MI_AMD64_TLB_MESSAGE *Message)
{
    ULONG Count = 0, Index;
    ULONG64 Bits;

    for (Index = 0; Index < MI_AMD64_TLB_TARGET_WORDS; Index++)
    {
        Bits = MI_AMD64_TLB_TARGET_BITS(MI_AMD64_READ_ACQUIRE64(&Message->Pending[Index]));
        for (; Bits != 0; Bits &= Bits - 1)
            Count++;
    }
    return Count;
}

static
VOID
MiAmd64TlbAddSelf(MI_AMD64_TLB_SET *Targets)
{
    ULONG Cpu = MiAmd64TlbCpu();

    MI_ASSERT(Cpu < MI_AMD64_TLB_MAXIMUM_PROCESSORS);
    Targets->Group[Cpu / MI_AMD64_TLB_GROUP_SIZE] |= 1ULL << (Cpu % MI_AMD64_TLB_GROUP_SIZE);
}

static
VOID
MiAmd64TlbAddActive(MI_AMD64_TLB_SET *Targets)
{
    ULONG Group, Groups = MiAmd64TlbGroupCount();

    for (Group = 0; Group < Groups && Group < MI_AMD64_TLB_GROUPS; Group++)
        Targets->Group[Group] |= MiAmd64TlbActiveGroup(Group);
}

static
VOID
MiAmd64FlushTargetSet(const MI_AMD64_TLB_SET *Targets, ULONG64 Address, ULONG64 Pages)
{
    KIRQL Previous = MiAmd64TlbRaiseIrql();
    ULONG Cpu = MiAmd64TlbCpu(), Group, Groups, Index, Remaining;
    ULONG64 Self, Sequence, Frequency = 0, Now = 0;
    ULONG64 Remote[MI_AMD64_TLB_GROUPS], AnyRemote = 0;
    BOOLEAN Local;
    MI_AMD64_TLB_MESSAGE *Message;
    MI_AMD64_TLB_WATCHDOG Watchdog = {0};

    MI_ASSERT(Cpu < MI_AMD64_TLB_MAXIMUM_PROCESSORS);
    Self = 1ULL << (Cpu % MI_AMD64_TLB_GROUP_SIZE);
    Local = (Targets->Group[Cpu / MI_AMD64_TLB_GROUP_SIZE] & Self) != 0;
    Groups = MiAmd64TlbGroupCount();
    for (Group = 0; Group < MI_AMD64_TLB_GROUPS; Group++)
    {
        Remote[Group] = (Group < Groups) ? (Targets->Group[Group] & MiAmd64TlbActiveGroup(Group)) : 0;
        if (Group == Cpu / MI_AMD64_TLB_GROUP_SIZE)
            Remote[Group] &= ~Self;
        AnyRemote |= Remote[Group];
    }
    Address &= ~((ULONG64)PAGE_SIZE - 1);
    if (Pages > MI_AMD64_TLB_USER_RANGE_LIMIT && Address < MI_AMD64_USER_END &&
        Pages - 1 < ((MI_AMD64_USER_END - Address) >> PAGE_SHIFT))
        Pages = MI_AMD64_TLB_NON_GLOBAL;
    else if (Pages > MI_AMD64_TLB_RANGE_LIMIT ||
             !MiAmd64CanonicalAddress(Address) ||
             (Pages != 0 && (Pages - 1 > (~Address >> PAGE_SHIFT) ||
                            !MiAmd64CanonicalAddress(Address + ((Pages - 1) << PAGE_SHIFT)))))
        Pages = 0;
    Message = &MiAmd64TlbMessages[Cpu];
    MI_ASSERT(MiAmd64TlbPendingTargets(Message) == 0);
    if (AnyRemote != 0)
    {
        Sequence = (MI_AMD64_READ_NOFENCE64(&Message->Sequence) + 1) & 0xFFFFFFFFULL;
        MI_AMD64_WRITE_RELEASE64(&Message->Sequence, Sequence);
        MI_AMD64_WRITE_RELEASE64(&Message->Address, Address);
        MI_AMD64_WRITE_RELEASE64(&Message->Pages, Pages);
        for (Index = 0; Index < MI_AMD64_TLB_TARGET_WORDS; Index++)
        {
            ULONG64 Bits = Remote[Index / 2] >> ((Index % 2) * MI_AMD64_TLB_TARGETS_PER_WORD);
            Bits &= 0xFFFFFFFFULL;
            if (Bits != 0)
                MI_AMD64_WRITE_RELEASE64(&Message->Pending[Index], (Sequence << 32) | Bits);
        }
        for (Group = 0; Group < MI_AMD64_TLB_GROUPS; Group++)
        {
            if (Remote[Group] != 0)
                KiSendMemoryIpiGroup(Group, Remote[Group]);
        }
    }
    if (Local)
        MiAmd64FlushLocal(Address, Pages);
    Remaining = (AnyRemote != 0) ? MiAmd64TlbPendingTargets(Message) : 0;
    while (Remaining != 0)
    {
        Remaining = MiAmd64TlbPendingTargets(Message);
        if (Remaining != 0)
        {
            if (MiAmd64TlbWatchdogExpired(&Watchdog, Remaining, &Frequency, &Now))
            {
                Remaining = MiAmd64TlbPendingTargets(Message);
                if (Remaining == Watchdog.Remaining &&
                    MiAmd64TlbWatchdogEnabled() &&
                    !MiAmd64TlbWatchdogFreezeActive() &&
                    MiAmd64TlbWatchdogEpoch() == Watchdog.Epoch)
                    MiAmd64TlbWatchdogBugcheck(Frequency, Now, Watchdog.Baseline);
                else
                    Watchdog.Valid = FALSE;
            }
            MiAmd64TlbPoll();
            MiAmd64TlbPause();
        }
    }
    MiAmd64TlbLowerIrql(Previous);
}

/* Targets is a group 0 mask */
VOID
MiAmd64FlushTargets(ULONG64 Targets, ULONG64 Address, ULONG64 Pages)
{
    MI_AMD64_TLB_SET Set = {{0}};

    Set.Group[0] = Targets;
    MiAmd64FlushTargetSet(&Set, Address, Pages);
}

VOID
MiArchInvalidateTlbRange(PVOID BaseAddress, SIZE_T Size, MI_TLB_SCOPE Scope)
{
    ULONG64 Address = (ULONG_PTR)BaseAddress;
    ULONG64 Pages;
    MI_AMD64_TLB_SET Targets = {{0}};
    KIRQL Previous;

    if (Size == 0)
        return;
    Pages = ((Size - 1) >> PAGE_SHIFT) + 1;
    Pages += (((Size - 1) & (PAGE_SIZE - 1)) + (Address & (PAGE_SIZE - 1))) >> PAGE_SHIFT;
    Previous = MiAmd64TlbRaiseIrql();
    MiAmd64TlbAddSelf(&Targets);
    if (Scope == MiTlbAllProcessors)
        MiAmd64TlbAddActive(&Targets);
    MiAmd64FlushTargetSet(&Targets, Address, Pages);
    MiAmd64TlbLowerIrql(Previous);
}

VOID
MiArchInvalidateTlbSingle(PVOID Address, MI_TLB_SCOPE Scope)
{
    MiArchInvalidateTlbRange(Address, 1, Scope);
}

VOID
MiArchInvalidateTlbAll(MI_TLB_SCOPE Scope)
{
    KIRQL Previous = MiAmd64TlbRaiseIrql();
    MI_AMD64_TLB_SET Targets = {{0}};

    MiAmd64TlbAddSelf(&Targets);
    if (Scope == MiTlbAllProcessors)
        MiAmd64TlbAddActive(&Targets);
    MiAmd64FlushTargetSet(&Targets, 0, 0);
    MiAmd64TlbLowerIrql(Previous);
}

VOID
NTAPI
KeFlushCurrentTb(VOID)
{
    MiAmd64FlushLocal(0, 0);
}

VOID
NTAPI
KeFlushEntireTb(BOOLEAN Invalid, BOOLEAN AllProcessors)
{
    UNREFERENCED_PARAMETER(Invalid);
    MiArchInvalidateTlbAll(AllProcessors ? MiTlbAllProcessors : MiTlbLocal);
    MI_AMD64_INCREMENT_NOFENCE(&KiTbFlushTimeStamp);
}

VOID
MiArchTlbInvalidate(ULONG64 VirtualAddress, ULONG64 PageCount, BOOLEAN AllProcessors)
{
    MI_TLB_SCOPE Scope = AllProcessors ? MiTlbAllProcessors : MiTlbLocal;
    if (PageCount > ((SIZE_T)-1 >> PAGE_SHIFT))
        MiArchInvalidateTlbAll(Scope);
    else
        MiArchInvalidateTlbRange((PVOID)(ULONG_PTR)VirtualAddress,
                                 (SIZE_T)(PageCount << PAGE_SHIFT), Scope);
}

/* Processors is a group 0 mask; the current processor is always flushed */
VOID
MiArchTlbInvalidateProcessors(ULONG64 VirtualAddress, ULONG64 PageCount, ULONG64 Processors)
{
    MI_AMD64_TLB_SET Targets = {{0}};
    KIRQL Previous;

    if (PageCount == 0)
        return;
    if (PageCount > ((SIZE_T)-1 >> PAGE_SHIFT))
        PageCount = 0;
    Previous = MiAmd64TlbRaiseIrql();
    Targets.Group[0] = Processors;
    MiAmd64TlbAddSelf(&Targets);
    MiAmd64FlushTargetSet(&Targets, VirtualAddress, PageCount);
    MiAmd64TlbLowerIrql(Previous);
}

VOID
MiArchTlbInvalidateAll(BOOLEAN AllProcessors)
{
    MiArchInvalidateTlbAll(AllProcessors ? MiTlbAllProcessors : MiTlbLocal);
}
