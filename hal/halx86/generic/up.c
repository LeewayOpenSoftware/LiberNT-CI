/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Core source file for Uniprocessor (UP) alternative functions
 * COPYRIGHT:   Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

#ifdef _M_AMD64
VOID
NTAPI
HalRequestIpi(
    _In_ ULONG Reserved,
    _In_ PKAFFINITY_EX TargetSet)
#else
VOID
NTAPI
HalRequestIpi(
    _In_ KAFFINITY TargetProcessors)
#endif
{
    /* This should never be called in UP mode */
    __debugbreak();
}

BOOLEAN
NTAPI
HalStartNextProcessor(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ PKPROCESSOR_STATE ProcessorState)
{
    /* Always return false on UP systems */
    return FALSE;
}

VOID
HalpSetupProcessorsTable(
    _In_ UINT32 NTProcessorNumber)
{
    NOTHING;
}

VOID
FASTCALL
HalpBroadcastClockIpi(
    _In_ UCHAR Vector)
{
    NOTHING;
}

#ifdef _M_AMD64

VOID
NTAPI
HalSendNMI(
    _In_ KAFFINITY TargetSet)
{
    NOTHING;
}

VOID
NTAPI
HalSendSoftwareInterrupt(
    _In_ ULONG ProcessorIndex,
    _In_ KIRQL Irql)
{
    NOTHING;
}

#endif // _M_AMD64

BOOLEAN
HalpRestartProcessor(
    _In_ ULONG NTProcessorNumber,
    _In_ PKPROCESSOR_STATE ProcessorState,
    _In_ PVOID Entry)
{
    return FALSE;
}
