/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     HAL Processor Routines
 * COPYRIGHT:   Copyright 2010 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>
#define NDEBUG
#include <debug.h>

KAFFINITY HalpActiveProcessors;
KAFFINITY HalpDefaultInterruptAffinity;

extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

/* PRIVATE FUNCTIONS *********************************************************/

ULONG
NTAPI
HalQueryMaximumProcessorCount(VOID)
{
    ULONG Count = HalpApicInfoTable.ProcessorCount;
    return (Count != 0) ? Count : 1;
}

BOOLEAN
NTAPI
HalIsHyperThreadingEnabled(VOID)
{
    return (KeGetCurrentPrcb()->LogicalProcessorsPerCore > 1);
}

VOID
NTAPI
HaliHaltSystem(VOID)
{
    /* Disable interrupts and halt the CPU */
    _disable();
    __halt();
}

/* FUNCTIONS *****************************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalAllProcessorsStarted(VOID)
{
    /* Do nothing */
    return TRUE;
}

/*
 * @implemented
 */
VOID
NTAPI
HalProcessorIdle(VOID)
{
#ifdef _M_AMD64
    /*
     * The interrupt priority lives in CR8 on amd64. A local APIC emulated
     * outside the processor only learns of it when the guest leaves, and can go
     * on holding back what is pending behind a priority lowered long ago. Give
     * the priority to the APIC itself before halting on it.
     */
    if (!HalpApicX2Mode)
        ApicWrite(APIC_TPR, IrqlToTpr(KeGetCurrentIrql()));
#endif

    /* Enable interrupts and halt the processor */
    _enable();
    __halt();
}

/* EOF */
