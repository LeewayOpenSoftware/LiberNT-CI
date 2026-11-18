/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     SMP specific APIC code
 * COPYRIGHT:   Copyright 2021 Timo Kreuzer <timo.kreuzer@reactos.org>
 *              Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>

#define NDEBUG
#include <debug.h>


extern PPROCESSOR_IDENTITY HalpProcessorIdentity;
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

/* INTERNAL FUNCTIONS *********************************************************/

/*!
    \param Vector - Specifies the interrupt vector to be delivered.

    \param MessageType - Specifies the message type sent to the CPU core
        interrupt handler. This can be one of the following values:
        APIC_MT_Fixed - Delivers an interrupt to the target local APIC
            specified in Destination field.
        APIC_MT_LowestPriority - Delivers an interrupt to the local APIC
            executing at the lowest priority of all local APICs.
        APIC_MT_SMI - Delivers an SMI interrupt to target local APIC(s).
        APIC_MT_RemoteRead - Delivers a read request to read an APIC register
            in the target local APIC specified in Destination field.
        APIC_MT_NMI - Delivers a non-maskable interrupt to the target local
            APIC specified in the Destination field. Vector is ignored.
        APIC_MT_INIT - Delivers an INIT request to the target local APIC(s)
            specified in the Destination field. TriggerMode must be
            APIC_TGM_Edge, Vector must be 0.
        APIC_MT_Startup - Delivers a start-up request (SIPI) to the target
            local APIC(s) specified in Destination field. Vector specifies
            the startup address.
        APIC_MT_ExtInt - Delivers an external interrupt to the target local
            APIC specified in Destination field.

    \param TriggerMode - The trigger mode of the interrupt. Can be:
        APIC_TGM_Edge - The interrupt is edge triggered.
        APIC_TGM_Level - The interrupt is level triggered.

    \param DestinationShortHand - Specifies where to send the interrupt.
        APIC_DSH_Destination
        APIC_DSH_Self
        APIC_DSH_AllIncludingSelf
        APIC_DSH_AllExcludingSelf

    \see "AMD64 Architecture Programmer's Manual Volume 2 System Programming"
        Chapter 16 "Advanced Programmable Interrupt Controller (APIC)"
        16.5 "Interprocessor Interrupts (IPI)"

 */
FORCEINLINE
VOID
ApicRequestGlobalInterruptLevel(
    _In_ ULONG DestinationProcessor,
    _In_ UCHAR Vector,
    _In_ APIC_MT MessageType,
    _In_ APIC_TGM TriggerMode,
    _In_ APIC_DSH DestinationShortHand,
    _In_ ULONG Level)
{
    ULONG Flags;
    APIC_INTERRUPT_COMMAND_REGISTER Icr;
    APIC_INTERRUPT_COMMAND_REGISTER IcrStatus;

    /* Setup the command register */
    Icr.LongLong = 0;
    Icr.Vector = Vector;
    Icr.MessageType = MessageType;
    Icr.DestinationMode = APIC_DM_Physical;
    Icr.DeliveryStatus = 0;
    Icr.Level = Level;
    Icr.TriggerMode = TriggerMode;
    Icr.RemoteReadStatus = 0;
    Icr.DestinationShortHand = DestinationShortHand;

    /* x2APIC: one MSR write with a 32-bit destination, no delivery status */
    if (HalpApicX2Mode)
    {
        ApicWriteIcr(Icr.Long0, DestinationProcessor);
        return;
    }

    ASSERT(DestinationProcessor <= 0xFF);
    Icr.Destination = (UCHAR)DestinationProcessor;

    if (HalpApicHvMsrs && MessageType == APIC_MT_Fixed)
    {
        __writemsr(HV_X64_MSR_ICR, Icr.LongLong);
        return;
    }

    /* Disable interrupts so that we can change IRR without being interrupted */
    Flags = __readeflags();
    _disable();

    do
    {
        IcrStatus.Long0 = ApicRead(APIC_ICR0);
    } while (IcrStatus.DeliveryStatus);

    /* A shorthand names the destinations itself, so the destination register
     * is not read and writing it only costs a local APIC access. */
    if (DestinationShortHand == APIC_DSH_Destination)
        ApicWrite(APIC_ICR1, Icr.Long1);

    /* Write the low dword last to send the interrupt */
    ApicWrite(APIC_ICR0, Icr.Long0);

    /* An interrupt is left to the local APIC to deliver: the next sender
     * waits for the delivery status above. Startup and init hand-offs are
     * sequenced by the caller, so those are waited for here. */
    if (MessageType != APIC_MT_Fixed)
    {
        do
        {
            IcrStatus.Long0 = ApicRead(APIC_ICR0);
        } while (IcrStatus.DeliveryStatus);
    }

    /* Finally, restore the original interrupt state */
    if (Flags & EFLAGS_INTERRUPT_MASK)
    {
        _enable();
    }
}

FORCEINLINE
VOID
ApicRequestGlobalInterrupt(
    _In_ ULONG DestinationProcessor,
    _In_ UCHAR Vector,
    _In_ APIC_MT MessageType,
    _In_ APIC_TGM TriggerMode,
    _In_ APIC_DSH DestinationShortHand)
{
    ApicRequestGlobalInterruptLevel(DestinationProcessor,
                                    Vector,
                                    MessageType,
                                    TriggerMode,
                                    DestinationShortHand,
                                    1);
}


/* SMP SUPPORT FUNCTIONS ******************************************************/

VOID
ApicStartApplicationProcessor(
    _In_ ULONG NTProcessorNumber,
    _In_ PHYSICAL_ADDRESS StartupLoc)
{
    ULONG LapicId;
    UCHAR StartupVector;

    ASSERT(StartupLoc.HighPart == 0);
    ASSERT((StartupLoc.QuadPart & 0xFFF) == 0);
    ASSERT((StartupLoc.QuadPart & 0xFFF00FFF) == 0);

    /* Intel MP spec 1.4 B.4: INIT (level, deassert), 10 ms, STARTUP twice */
    LapicId = HalpProcessorIdentity[NTProcessorNumber].LapicId;
    StartupVector = (UCHAR)((StartupLoc.LowPart) >> 12);

    ApicRequestGlobalInterruptLevel(LapicId, 0,
        APIC_MT_INIT, APIC_TGM_Level, APIC_DSH_Destination, 1);

    KeStallExecutionProcessor(10000);

    /* x2APIC processors do not support the INIT level de-assert */
    if (!HalpApicX2Mode)
    {
        ApicRequestGlobalInterruptLevel(LapicId, 0,
            APIC_MT_INIT, APIC_TGM_Level, APIC_DSH_Destination, 0);

        KeStallExecutionProcessor(200);
    }

    ApicRequestGlobalInterrupt(LapicId, StartupVector,
        APIC_MT_Startup, APIC_TGM_Edge, APIC_DSH_Destination);

    KeStallExecutionProcessor(200);

    ApicRequestGlobalInterrupt(LapicId, StartupVector,
        APIC_MT_Startup, APIC_TGM_Edge, APIC_DSH_Destination);
}

/* HAL IPI FUNCTIONS **********************************************************/

/*!
 *  \brief Broadcasts an IPI with a specified vector to all processors.
 *
 *  \param Vector - Specifies the interrupt vector to be delivered.
 *  \param IncludeSelf - Specifies whether to include the current processor.
 */
VOID
NTAPI
HalpBroadcastIpiSpecifyVector(
    _In_ UCHAR Vector,
    _In_ BOOLEAN IncludeSelf)
{
    APIC_DSH DestinationShortHand = IncludeSelf ?
        APIC_DSH_AllIncludingSelf : APIC_DSH_AllExcludingSelf;

    /* Request the interrupt targeted at all processors */
    ApicRequestGlobalInterrupt(0, // Ignored
                               Vector,
                               APIC_MT_Fixed,
                               APIC_TGM_Edge,
                               DestinationShortHand);
}

/*!
 *  \brief Requests an IPI with a specified vector on the specified processors.
 *
 *  \param TargetSet - Specifies the set of processors to send the IPI to.
 *  \param Vector - Specifies the interrupt vector to be delivered.
 *
 *  \remarks This function is exported on Windows 10.
 */
VOID
NTAPI
HalpRequestIpiSpecifyVector(
    _In_ KAFFINITY TargetSet,
    _In_ UCHAR Vector)
{
    KAFFINITY ActiveProcessors = HalpActiveProcessors;
    KAFFINITY RemainingSet, SetMember;
    ULONG ProcessorIndex;
    ULONG LApicId;

    /* Sanitize the target set */
    TargetSet &= ActiveProcessors;

    /* Check if all processors are requested */
    if (TargetSet == ActiveProcessors)
    {
        /* Send an IPI to all processors, including this processor */
        HalpBroadcastIpiSpecifyVector(Vector, TRUE);
        return;
    }

    /* Check if all processors except the current one are requested */
    if (TargetSet == (ActiveProcessors & ~KeGetCurrentPrcb()->SetMember))
    {
        /* Send an IPI to all processors, excluding this processor */
        HalpBroadcastIpiSpecifyVector(Vector, FALSE);
        return;
    }

    /* Loop while we have more processors */
    RemainingSet = TargetSet;
    while (RemainingSet != 0)
    {
        NT_VERIFY(BitScanForwardAffinity(&ProcessorIndex, RemainingSet) != 0);
        ASSERT(ProcessorIndex < KeNumberProcessors);
        SetMember = AFFINITY_MASK(ProcessorIndex);
        RemainingSet &= ~SetMember;

        /* Send the interrupt to the target processor */
        LApicId = HalpProcessorIdentity[ProcessorIndex].LapicId;
        ApicRequestGlobalInterrupt(LApicId,
                                   Vector,
                                   APIC_MT_Fixed,
                                   APIC_TGM_Edge,
                                   APIC_DSH_Destination);
    }
}

/*!
 *  \brief Requests an IPI interrupt on the specified processors.
 *
 *  \param TargetSet - Specifies the set of processors to send the IPI to.
 */
VOID
NTAPI
HalpRequestIpi(
    _In_ KAFFINITY TargetSet)
{
    /* Request the IPI vector */
    HalpRequestIpiSpecifyVector(TargetSet, APIC_IPI_VECTOR);
}

#ifdef _M_AMD64

/*!
 *  \brief Requests a software interrupt on the specified processors.
 *
 *  \param TargetSet - Specifies the set of processors to send the IPI to.
 *  \param Irql - Specifies the IRQL of the software interrupt.
 */
VOID
NTAPI
HalpSendSoftwareInterrupt(
    _In_ KAFFINITY TargetSet,
    _In_ KIRQL Irql)
{
    UCHAR Vector;

    /* Get the vector for the requested IRQL */
    if (Irql == APC_LEVEL)
    {
        Vector = APC_VECTOR;
    }
    else if (Irql == DISPATCH_LEVEL)
    {
        Vector = DISPATCH_VECTOR;
    }
    else
    {
        ASSERT(FALSE);
        return;
    }

    /* Request the IPI with the specified vector */
    HalpRequestIpiSpecifyVector(TargetSet, Vector);
}

/*!
 *  \brief Requests an interrupt with a specified vector on one processor.
 *
 *  \param ProcessorIndex - The processor index (group * 64 + number).
 *  \param Vector - Specifies the interrupt vector to be delivered.
 */
static
VOID
HalpRequestIpiProcessor(
    _In_ ULONG ProcessorIndex,
    _In_ UCHAR Vector)
{
    /* Group 0 keeps the KAFFINITY path and its broadcast shortcuts */
    if (ProcessorIndex < sizeof(KAFFINITY) * 8)
    {
        HalpRequestIpiSpecifyVector(AFFINITY_MASK(ProcessorIndex), Vector);
        return;
    }

    /* Only processors this HAL has initialized. A request for one that is
     * not up yet is dropped on purpose: the kernel queued it in that
     * processor's mailbox, which is read on its first IPI. */
    if ((ProcessorIndex >= HalpApicInfoTable.ProcessorCount) ||
        (HalpProcessorIdentity[ProcessorIndex].ProcessorPrcb == NULL))
    {
        return;
    }

    ApicRequestGlobalInterrupt(HalpProcessorIdentity[ProcessorIndex].LapicId,
                               Vector,
                               APIC_MT_Fixed,
                               APIC_TGM_Edge,
                               APIC_DSH_Destination);
}

/*!
 *  \brief Requests an IPI on the processors of a multi-group target set.
 *
 *  \param TargetSet - Specifies the processors, in every group.
 */
VOID
NTAPI
HalpRequestIpiEx(
    _In_ PKAFFINITY_EX TargetSet)
{
    USHORT Groups = min(TargetSet->Count, TargetSet->Size);
    USHORT Group;
    KAFFINITY Mask;
    ULONG Bit;

    if ((Groups != 0) && (TargetSet->Bitmap[0] != 0))
        HalpRequestIpiSpecifyVector(TargetSet->Bitmap[0], APIC_IPI_VECTOR);

    for (Group = 1; Group < Groups; Group++)
    {
        Mask = TargetSet->Bitmap[Group];
        while (Mask != 0)
        {
            NT_VERIFY(BitScanForwardAffinity(&Bit, Mask) != 0);
            Mask &= ~AFFINITY_MASK(Bit);
            HalpRequestIpiProcessor(((ULONG)Group * sizeof(KAFFINITY) * 8) + Bit,
                                    APIC_IPI_VECTOR);
        }
    }
}

/*!
 *  \brief Requests a software interrupt on one processor.
 *
 *  \param ProcessorIndex - The processor index.
 *  \param Irql - Specifies the IRQL of the software interrupt.
 */
VOID
NTAPI
HalpSendSoftwareInterruptProcessor(
    _In_ ULONG ProcessorIndex,
    _In_ KIRQL Irql)
{
    if (Irql == APC_LEVEL)
    {
        HalpRequestIpiProcessor(ProcessorIndex, APC_VECTOR);
    }
    else if (Irql == DISPATCH_LEVEL)
    {
        HalpRequestIpiProcessor(ProcessorIndex, DISPATCH_VECTOR);
    }
    else
    {
        ASSERT(FALSE);
    }
}

/*!
 *  \brief Requests an NMI interrupt on the specified processors.
 *
 *  \param TargetSet - Specifies the set of processors to send the IPI to.
 */
VOID
NTAPI
HalpSendNMI(
    _In_ KAFFINITY TargetSet)
{
    KAFFINITY RemainingSet, SetMember;
    ULONG ProcessorIndex;
    ULONG LApicId;

    /* Make sure we do not send an NMI to ourselves */
    ASSERT((TargetSet & KeGetCurrentPrcb()->SetMember) == 0);

    /* Loop while we have more processors */
    RemainingSet = TargetSet;
    while (RemainingSet != 0)
    {
        NT_VERIFY(BitScanForwardAffinity(&ProcessorIndex, RemainingSet) != 0);
        ASSERT(ProcessorIndex < KeNumberProcessors);
        SetMember = AFFINITY_MASK(ProcessorIndex);
        RemainingSet &= ~SetMember;

        /* Send and NMI to the target processor */
        LApicId = HalpProcessorIdentity[ProcessorIndex].LapicId;
        ApicRequestGlobalInterrupt(LApicId,
                                   0,
                                   APIC_MT_NMI,
                                   APIC_TGM_Edge,
                                   APIC_DSH_Destination);
    }
}

#endif // _M_AMD64
