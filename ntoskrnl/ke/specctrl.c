/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Speculation control: processor features and system policy
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

ULONG KiSpeculationFeatures;
ULONG64 KiArchCapabilities;

ULONG KiFeatureSettingsOverride;
ULONG KiFeatureSettingsOverrideMask;

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
KiDetectSpeculationFeatures(VOID)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    ULONG Features = 0;
    INT Regs[4];

    __cpuid(Regs, 0);
    if ((ULONG)Regs[0] >= 7)
    {
        ULONG Edx;

        __cpuidex(Regs, 7, 0);
        Edx = (ULONG)Regs[3];
        if (Edx & CPUID07_EDX_IBRS_IBPB) Features |= KSPEC_SPEC_CTRL | KSPEC_PRED_CMD | KSPEC_IBRS;
        if (Edx & CPUID07_EDX_STIBP) Features |= KSPEC_SPEC_CTRL | KSPEC_STIBP;
        if (Edx & CPUID07_EDX_SSBD) Features |= KSPEC_SPEC_CTRL | KSPEC_SSBD;
        if (Edx & CPUID07_EDX_MD_CLEAR) Features |= KSPEC_MD_CLEAR;
        if (Edx & CPUID07_EDX_L1D_FLUSH) Features |= KSPEC_L1D_FLUSH;
        if (Edx & CPUID07_EDX_ARCH_CAPABILITIES) Features |= KSPEC_ARCH_CAPABILITIES;
    }

    /* AMD reports the same controls in the extended leaf */
    __cpuid(Regs, 0x80000000);
    if ((ULONG)Regs[0] >= 0x80000008)
    {
        ULONG Ebx;

        __cpuid(Regs, 0x80000008);
        Ebx = (ULONG)Regs[1];
        if (Ebx & CPUID80000008_EBX_IBPB) Features |= KSPEC_PRED_CMD;
        if (Ebx & CPUID80000008_EBX_IBRS) Features |= KSPEC_SPEC_CTRL | KSPEC_IBRS;
        if (Ebx & CPUID80000008_EBX_STIBP) Features |= KSPEC_SPEC_CTRL | KSPEC_STIBP;
        if (Ebx & CPUID80000008_EBX_SSBD) Features |= KSPEC_SPEC_CTRL | KSPEC_SSBD;
        if (Ebx & CPUID80000008_EBX_VIRT_SSBD) Features |= KSPEC_VIRT_SSBD;
        if (Ebx & CPUID80000008_EBX_SSB_NO) Features |= KSPEC_SSB_NO;
    }

    if (Features & KSPEC_ARCH_CAPABILITIES)
    {
        KiArchCapabilities = __readmsr(MSR_IA32_ARCH_CAPABILITIES);
        if (KiArchCapabilities & ARCH_CAP_SSB_NO) Features |= KSPEC_SSB_NO;
        if (KiArchCapabilities & ARCH_CAP_MDS_NO) Features |= KSPEC_MDS_NO;
    }

    /* AMD documents its processors as not affected by MDS */
    if (Prcb->CpuVendor == CPU_AMD)
        Features |= KSPEC_MDS_NO;

    KiSpeculationFeatures = Features;
    DPRINT1("Speculation features 0x%lx, architecture capabilities 0x%I64x\n",
            Features, KiArchCapabilities);
}

NTSTATUS
NTAPI
KeQuerySpeculationControlInformation(
    _Out_writes_bytes_to_(Length, *ReturnLength) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength)
{
    SYSTEM_SPECULATION_CONTROL_INFORMATION Info;
    ULONG Features = KiSpeculationFeatures;
    ULONG Size;

    if (Length < sizeof(Info.Flags))
    {
        *ReturnLength = sizeof(Info.Flags);
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    RtlZeroMemory(&Info, sizeof(Info));

    /* No mitigation is active yet: report what the hardware offers */
    if (Features & (KSPEC_IBRS | KSPEC_PRED_CMD))
        Info.Flags |= SPECCTRL_BPB_DISABLED_SYSTEM_POLICY;
    else
        Info.Flags |= SPECCTRL_BPB_DISABLED_NO_HARDWARE;
    if (Features & KSPEC_SPEC_CTRL) Info.Flags |= SPECCTRL_SPEC_CTRL_ENUMERATED;
    if (Features & KSPEC_PRED_CMD) Info.Flags |= SPECCTRL_PRED_CMD_ENUMERATED;
    if (Features & KSPEC_IBRS) Info.Flags |= SPECCTRL_IBRS_PRESENT;
    if (Features & KSPEC_STIBP) Info.Flags |= SPECCTRL_STIBP_PRESENT;
    if (KeFeatureBits & KF_SMEP) Info.Flags |= SPECCTRL_SMEP_PRESENT;
    if (Features & (KSPEC_SSBD | KSPEC_VIRT_SSBD)) Info.Flags |= SPECCTRL_SSBD_SUPPORTED;
    if (!(Features & KSPEC_SSB_NO)) Info.Flags |= SPECCTRL_SSBD_REQUIRED;
    if (KiArchCapabilities & ARCH_CAP_IBRS_ALL) Info.Flags |= SPECCTRL_ENHANCED_IBRS_REPORTED;
    if (Features & KSPEC_MDS_NO) Info.Flags |= SPECCTRL_MDS_HARDWARE_PROTECTED;
    if (Features & KSPEC_MD_CLEAR) Info.Flags |= SPECCTRL_MB_CLEAR_REPORTED;

    Size = (Length >= sizeof(Info)) ? sizeof(Info) : sizeof(Info.Flags);
    RtlCopyMemory(Buffer, &Info, Size);
    *ReturnLength = Size;
    return STATUS_SUCCESS;
}
