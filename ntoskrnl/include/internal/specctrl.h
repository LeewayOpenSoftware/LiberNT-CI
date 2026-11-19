/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Speculation control: processor features and system policy
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#if defined(_M_IX86) || defined(_M_AMD64)

/* Architectural MSRs (Intel SDM volume 4, AMD APM volume 2) */
#define MSR_IA32_SPEC_CTRL              0x00000048
#define MSR_IA32_PRED_CMD               0x00000049
#define MSR_IA32_ARCH_CAPABILITIES      0x0000010A
#define MSR_IA32_FLUSH_CMD              0x0000010B

/* CPUID.(EAX=07h,ECX=0):EDX */
#define CPUID07_EDX_MD_CLEAR            (1UL << 10)
#define CPUID07_EDX_IBRS_IBPB           (1UL << 26)
#define CPUID07_EDX_STIBP               (1UL << 27)
#define CPUID07_EDX_L1D_FLUSH           (1UL << 28)
#define CPUID07_EDX_ARCH_CAPABILITIES   (1UL << 29)
#define CPUID07_EDX_SSBD                (1UL << 31)

/* CPUID.80000008h:EBX (AMD) */
#define CPUID80000008_EBX_IBPB          (1UL << 12)
#define CPUID80000008_EBX_IBRS          (1UL << 14)
#define CPUID80000008_EBX_STIBP         (1UL << 15)
#define CPUID80000008_EBX_SSBD          (1UL << 24)
#define CPUID80000008_EBX_VIRT_SSBD     (1UL << 25)
#define CPUID80000008_EBX_SSB_NO        (1UL << 26)

/* IA32_ARCH_CAPABILITIES */
#define ARCH_CAP_RDCL_NO                (1ULL << 0)
#define ARCH_CAP_IBRS_ALL               (1ULL << 1)
#define ARCH_CAP_RSBA                   (1ULL << 2)
#define ARCH_CAP_SKIP_L1DFL_VMENTRY     (1ULL << 3)
#define ARCH_CAP_SSB_NO                 (1ULL << 4)
#define ARCH_CAP_MDS_NO                 (1ULL << 5)
#define ARCH_CAP_TAA_NO                 (1ULL << 8)

/* What the processor offers, collected once on the boot processor */
#define KSPEC_SPEC_CTRL                 0x00000001  /* IA32_SPEC_CTRL exists */
#define KSPEC_PRED_CMD                  0x00000002  /* IA32_PRED_CMD (IBPB) exists */
#define KSPEC_IBRS                      0x00000004
#define KSPEC_STIBP                     0x00000008
#define KSPEC_SSBD                      0x00000010
#define KSPEC_VIRT_SSBD                 0x00000020
#define KSPEC_MD_CLEAR                  0x00000040
#define KSPEC_L1D_FLUSH                 0x00000080
#define KSPEC_ARCH_CAPABILITIES         0x00000100
#define KSPEC_SSB_NO                    0x00000200  /* Not affected by speculative store bypass */
#define KSPEC_MDS_NO                    0x00000400  /* Not affected by MDS */

extern ULONG KiSpeculationFeatures;
extern ULONG64 KiArchCapabilities;

/* Session Manager\Memory Management values, read by CmGetSystemControlValues */
extern ULONG KiFeatureSettingsOverride;
extern ULONG KiFeatureSettingsOverrideMask;

CODE_SEG("INIT")
VOID
NTAPI
KiDetectSpeculationFeatures(VOID);

NTSTATUS
NTAPI
KeQuerySpeculationControlInformation(
    _Out_writes_bytes_to_(Length, *ReturnLength) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength);

#endif /* _M_IX86 || _M_AMD64 */
