/*
 * PROJECT:     LiberNT NT Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     RISC-V64 hooks for processes that run under an emulation host
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <ntdll.h>
#include <reactos/emulation.h>
#include <reactos/emuhost.h>

PCWSTR
LdrpArchGetEmulationHost(
    _In_ USHORT Machine)
{
    return Machine == IMAGE_FILE_MACHINE_AMD64 ? EMULATION_HOST_IMAGE_AMD64 : NULL;
}

VOID
LdrpArchEnterEmulationHost(
    _Inout_ PCONTEXT Context,
    _In_ PTEB HostTeb,
    _In_ PVOID Stack,
    _In_ PVOID Entry,
    _In_ PVOID Argument)
{
    Context->Pc = (ULONG_PTR)Entry;
    Context->Ra = 0;
    Context->Sp = (ULONG_PTR)Stack;
    Context->Tp = (ULONG_PTR)HostTeb;
    Context->A0 = (ULONG_PTR)Argument;
}

ULONG64
__rv64x_call_x64(
    _In_ PVOID Target,
    _In_ ULONG64 DescriptorLow,
    _In_ ULONG64 DescriptorHigh,
    _In_ const ULONG64 *Arguments)
{
    static const EMU_HOST_INTERFACE *Interface;
    UNICODE_STRING Library = RTL_CONSTANT_STRING(EMU_HOST_LIBRARY);
    ANSI_STRING Export = RTL_CONSTANT_STRING(EMU_HOST_INTERFACE_EXPORT);
    const EMU_HOST_INTERFACE *(NTAPI *GetInterface)(VOID);
    PVOID Base;

    if (!Interface)
    {
        if (!NT_SUCCESS(LdrGetDllHandle(NULL, NULL, &Library, &Base)) ||
            !NT_SUCCESS(LdrGetProcedureAddress(Base, &Export, 0, (PVOID *)&GetInterface)))
        {
            RtlRaiseStatus(STATUS_ACCESS_VIOLATION);
        }
        Interface = GetInterface();
    }
    return Interface->CallDescribed((ULONG64)(ULONG_PTR)Target, DescriptorLow, DescriptorHigh, Arguments);
}
