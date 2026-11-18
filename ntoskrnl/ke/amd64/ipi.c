/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     IPI code for x64
 * COPYRIGHT:   Copyright 2023 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#include <reactos/smpdbg.h>
#define NDEBUG
#include <debug.h>

#define KI_IPI_MEMORY       0x20
#define KI_IPI_GENERIC_CALL 0x40

typedef struct DECLSPEC_CACHEALIGN _KI_GENERIC_CALL_PACKET
{
    PKIPI_BROADCAST_WORKER Function;
    ULONG_PTR Argument;
    volatile LONG TargetsRemaining;
    volatile LONG Release;
    volatile KAFFINITY OutstandingDone;
} KI_GENERIC_CALL_PACKET;

static KI_GENERIC_CALL_PACKET KiGenericCallPacket;
static volatile LONG KiGenericCallOwner = -1;

/*
 * Per-target IPI mailbox, indexed by processor index. Requests holds the
 * requests that do not depend on the sender (IPI_DPC, IPI_FREEZE,
 * KI_IPI_GENERIC_CALL). MemorySenders holds, per group, the senders whose
 * TLB request (KI_IPI_MEMORY) the target has not processed yet. This is
 * linear in the processor count, where the per-sender 64-slot
 * KPRCB.RequestMailbox it replaces is quadratic.
 */
typedef struct DECLSPEC_CACHEALIGN _KI_IPI_MAILBOX
{
    volatile LONG64 Requests;
    volatile KAFFINITY MemorySenders[KI_MAXIMUM_GROUPS];
} KI_IPI_MAILBOX, *PKI_IPI_MAILBOX;

static KI_IPI_MAILBOX KiIpiMailbox[KI_MAXIMUM_PROCESSORS];

/* FUNCTIONS *****************************************************************/

/* HalRequestIpi takes a KAFFINITY_EX; Size 1 describes group 0 only */
static
VOID
KiHalRequestIpiGroup0(
    _In_ KAFFINITY Targets)
{
    KAFFINITY_EX TargetSet;

    TargetSet.Count = 1;
    TargetSet.Size = 1;
    TargetSet.Reserved = 0;
    TargetSet.Bitmap[0] = Targets;
    HalRequestIpi(0, &TargetSet);
}

static
VOID
KiIpiQueueMailbox(
    _In_ ULONG Target,
    _In_ PPROCESSOR_NUMBER Source,
    _In_ ULONG IpiRequest)
{
    PKI_IPI_MAILBOX Mailbox = &KiIpiMailbox[Target];

    ASSERT(KiProcessorBlock[Target] != NULL);
    if (IpiRequest == KI_IPI_MEMORY)
    {
        InterlockedOr64((PLONG64)&Mailbox->MemorySenders[Source->Group],
                        (LONG64)KiAffinityForProcessorIndex(Source->Number));
    }
    else
    {
        InterlockedOr64(&Mailbox->Requests, IpiRequest);
    }
}

static
KAFFINITY
KiIpiQueueRequest(
    _In_ KAFFINITY TargetSet,
    _In_ ULONG IpiRequest)
{
    KAFFINITY QueuedTargets;
    PKPRCB SourcePrcb = KeGetCurrentPrcb();
    PROCESSOR_NUMBER Source;
    ULONG Processor;

    ASSERT(SourcePrcb->Number < KI_MAXIMUM_PROCESSORS);
    KiProcessorNumberFromIndex(SourcePrcb->Number, &Source);
    TargetSet &= (KAFFINITY)KeActiveProcessors;
    QueuedTargets = TargetSet;

    while (BitScanForwardAffinity(&Processor, TargetSet))
    {
        KiIpiQueueMailbox(Processor, &Source, IpiRequest);
        TargetSet &= ~AFFINITY_MASK(Processor);
    }

    return QueuedTargets;
}

VOID
FASTCALL
KiIpiSend(
    _In_ KAFFINITY TargetSet,
    _In_ ULONG IpiRequest)
{
    ULONG Processor;

    /* Check if we can send the IPI directly */
    if (IpiRequest == IPI_APC)
    {
        while (BitScanForwardAffinity(&Processor, TargetSet))
        {
            HalSendSoftwareInterrupt(Processor, APC_LEVEL);
            TargetSet &= ~AFFINITY_MASK(Processor);
        }
    }
    else if (IpiRequest == IPI_DPC)
    {
        TargetSet = KiIpiQueueRequest(TargetSet, IPI_DPC);

        if (TargetSet != 0)
            KiHalRequestIpiGroup0(TargetSet);
    }
    else if (IpiRequest == IPI_FREEZE)
    {
        /* On x64 the freeze IPI is an NMI */
        HalSendNMI(TargetSet);

        /* Also queue the request behind a normal IPI. Some hypervisors can
         * lose an NMI (VirtualBox on Hyper-V strands it in TRPM forever);
         * KiIpiProcessRequests then completes the freeze through
         * KiFreezeIfRequested. Whichever path claims TARGET_FREEZE first
         * wins, the other one finds nothing to do. */
        TargetSet = KiIpiQueueRequest(TargetSet, IPI_FREEZE);

        if (TargetSet != 0)
            KiHalRequestIpiGroup0(TargetSet);
    }
    else
    {
        ASSERT(FALSE);
    }
}

VOID
KiSendMemoryIpi(ULONG64 Targets)
{
    KAFFINITY Queued = KiIpiQueueRequest((KAFFINITY)Targets, KI_IPI_MEMORY);
    if (Queued != 0)
        KiHalRequestIpiGroup0(Queued);
}

/* Sends a TLB request to active processors of one group */
VOID
KiSendMemoryIpiGroup(ULONG Group, ULONG64 Targets)
{
    PROCESSOR_NUMBER Source;
    KAFFINITY_EX TargetSet;
    KAFFINITY Queued;
    ULONG Bit, Index;

    if (Group == 0)
    {
        KiSendMemoryIpi(Targets);
        return;
    }

    ASSERT(Group < KI_MAXIMUM_GROUPS);
    KiProcessorNumberFromIndex(KeGetCurrentPrcb()->Number, &Source);
    Queued = (KAFFINITY)Targets & KiQueryActiveGroupAffinity((USHORT)Group);
    if (Queued == 0)
        return;

    for (Targets = Queued; BitScanForwardAffinity(&Bit, (KAFFINITY)Targets); Targets &= ~AFFINITY_MASK(Bit))
        KiIpiQueueMailbox(KiProcessorIndexFromNumber((USHORT)Group, (UCHAR)Bit), &Source, KI_IPI_MEMORY);

    TargetSet.Count = (USHORT)(Group + 1);
    TargetSet.Size = (USHORT)(Group + 1);
    TargetSet.Reserved = 0;
    for (Index = 0; Index <= Group; Index++)
        TargetSet.StaticBitmap[Index] = 0;
    TargetSet.StaticBitmap[Group] = Queued;
    HalRequestIpi(0, &TargetSet);
}

VOID
FASTCALL
KiIpiSendProcessor(
    _In_ ULONG Index,
    _In_ ULONG IpiRequest)
{
    PROCESSOR_NUMBER Source;
    PROCESSOR_NUMBER Target;
    KAFFINITY_EX TargetSet;
    USHORT Group;

    ASSERT(Index < KiNumberProcessors);
    if (IpiRequest == IPI_APC)
    {
        HalSendSoftwareInterrupt(Index, APC_LEVEL);
        return;
    }

    ASSERT(IpiRequest == IPI_DPC);
    KiProcessorNumberFromIndex(KeGetCurrentPrcb()->Number, &Source);
    KiIpiQueueMailbox(Index, &Source, IpiRequest);

    KiProcessorNumberFromIndex(Index, &Target);
    TargetSet.Count = Target.Group + 1;
    TargetSet.Size = Target.Group + 1;
    TargetSet.Reserved = 0;
    for (Group = 0; Group <= Target.Group; Group++)
        TargetSet.StaticBitmap[Group] = 0;
    TargetSet.StaticBitmap[Target.Group] = KiAffinityForProcessorIndex(Target.Number);
    HalRequestIpi(0, &TargetSet);
}

static
VOID
KiIpiParticipateInGenericCall(
    _In_ PKPRCB Prcb)
{
    PKIPI_BROADCAST_WORKER Function = KiGenericCallPacket.Function;
    ULONG_PTR Argument = KiGenericCallPacket.Argument;
    KIRQL OldIrql;
    LONG Remaining;
    BOOLEAN WasOutstanding;

    ASSERT(Function != NULL);
    ASSERT(KiGenericCallOwner >= 0 && KiGenericCallOwner != (LONG)Prcb->Number);
    ASSERT(KiGenericCallPacket.OutstandingDone & Prcb->SetMember);

    Remaining = InterlockedDecrement(&KiGenericCallPacket.TargetsRemaining);
    UNREFERENCED_PARAMETER(Remaining);
    ASSERT(Remaining >= 0);
    while (!KiGenericCallPacket.Release)
    {
        YieldProcessor();
    }

    ASSERT(KiGenericCallPacket.Release != 0);
    OldIrql = KfRaiseIrql(IPI_LEVEL);
    Function(Argument);
    KeLowerIrql(OldIrql);

    WasOutstanding = InterlockedBitTestAndResetAffinity(&KiGenericCallPacket.OutstandingDone,
                                                        Prcb->Number);
    UNREFERENCED_PARAMETER(WasOutstanding);
    ASSERT(WasOutstanding);
}

VOID
NTAPI
KiIpiProcessRequests(VOID)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    PKI_IPI_MAILBOX Mailbox;
    ULONG64 Requests;
    KAFFINITY Senders;
    USHORT Groups;
    USHORT Group;
    ULONG Bit;

    ASSERT(Prcb->Number < KI_MAXIMUM_PROCESSORS);
    Mailbox = &KiIpiMailbox[Prcb->Number];

    /* TLB requests first, from every sender that queued one */
    Groups = KiActiveProcessorsEx.Count;
    for (Group = 0; Group < Groups; Group++)
    {
        Senders = (KAFFINITY)InterlockedExchange64((PLONG64)&Mailbox->MemorySenders[Group], 0);
        while (BitScanForwardAffinity(&Bit, Senders))
        {
            if (SmpDbgEnabled)
                SmpDbgTbFlushIpi(Prcb->Number);
            MiAmd64ProcessTlbRequest(KiProcessorIndexFromNumber(Group, (UCHAR)Bit));
            Senders &= ~AFFINITY_MASK(Bit);
        }
    }

    Requests = (ULONG64)InterlockedExchange64(&Mailbox->Requests, 0);

    if (Requests & KI_IPI_GENERIC_CALL)
    {
        if (SmpDbgEnabled)
            SmpDbgGenericCallIpi(Prcb->Number);
        KiIpiParticipateInGenericCall(Prcb);
    }

    ASSERT((Requests & ~(KI_IPI_GENERIC_CALL |
                         IPI_DPC |
                         IPI_FREEZE)) == 0);

    if (Requests & IPI_DPC)
    {
        if (SmpDbgEnabled)
            SmpDbgRemoteDpc(Prcb->Number, SMPDBG_SOURCE_UNKNOWN);
        KiRequestDispatchInterrupt(Prcb);
    }

    /* Complete a freeze whose NMI has not arrived. This also covers the
     * spin loops that poll here with interrupts disabled (TLB shootdown,
     * generic call), where a lost NMI would otherwise deadlock the owner. */
    KiFreezeIfRequested();
}

ULONG_PTR
NTAPI
KeIpiGenericCall(
    _In_ PKIPI_BROADCAST_WORKER Function,
    _In_ ULONG_PTR Argument)
{
    PKPRCB Prcb;
    KAFFINITY Targets, IpiTargets, RemainingSet;
    BOOLEAN InterruptsEnabled;
    KIRQL OldIrql, ExecuteIrql;
    ULONG_PTR Status;
    ULONG Processor;
    LONG TargetCount;

    ASSERT(Function != NULL);
    InterruptsEnabled = (__readeflags() & EFLAGS_INTERRUPT_MASK) != 0;

    OldIrql = KeGetCurrentIrql();
    if (OldIrql < SYNCH_LEVEL)
        KeRaiseIrql(SYNCH_LEVEL, &OldIrql);

    Prcb = KeGetCurrentPrcb();

    while (InterlockedCompareExchange(&KiGenericCallOwner,
                                      (LONG)Prcb->Number,
                                      -1) != -1)
    {
        KiIpiProcessRequests();
        YieldProcessor();
    }

    ASSERT(KiGenericCallPacket.TargetsRemaining == 0);
    ASSERT(KiGenericCallPacket.OutstandingDone == 0);
    Targets = (KAFFINITY)KeActiveProcessors & ~Prcb->SetMember;

    if (Targets != 0)
    {
        TargetCount = 0;
        RemainingSet = Targets;
        while (BitScanForwardAffinity(&Processor, RemainingSet))
        {
            TargetCount++;
            RemainingSet &= ~AFFINITY_MASK(Processor);
        }

        ASSERT(TargetCount > 0);
        ASSERT((Targets & Prcb->SetMember) == 0);
        KiGenericCallPacket.Function = Function;
        KiGenericCallPacket.Argument = Argument;
        KiGenericCallPacket.Release = 0;
        KiGenericCallPacket.OutstandingDone = Targets;
        KiGenericCallPacket.TargetsRemaining = TargetCount;
        KeMemoryBarrier();

        IpiTargets = KiIpiQueueRequest(Targets, KI_IPI_GENERIC_CALL);
        ASSERT(IpiTargets == Targets);
        if (IpiTargets != 0)
            KiHalRequestIpiGroup0(IpiTargets);

        while (KiGenericCallPacket.TargetsRemaining != 0)
        {
            if (!InterruptsEnabled)
                KiIpiProcessRequests();

            YieldProcessor();
        }
    }

    ASSERT(Targets == 0 || KiGenericCallPacket.OutstandingDone == Targets);
    KeRaiseIrql(IPI_LEVEL, &ExecuteIrql);
    if (Targets != 0)
        KiGenericCallPacket.Release = 1;

    Status = Function(Argument);

    while (KiGenericCallPacket.OutstandingDone != 0)
    {
        if (!InterruptsEnabled)
            KiIpiProcessRequests();

        YieldProcessor();
    }

    KeLowerIrql(ExecuteIrql);
    InterlockedExchange(&KiGenericCallOwner, -1);
    KeLowerIrql(OldIrql);
    return Status;
}
