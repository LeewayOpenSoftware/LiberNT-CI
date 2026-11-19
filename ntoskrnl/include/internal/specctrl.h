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
#define MSR_AMD64_VIRT_SPEC_CTRL        0xC001011F

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

/* IA32_SPEC_CTRL */
#define SPEC_CTRL_IBRS                  (1ULL << 0)
#define SPEC_CTRL_STIBP                 (1ULL << 1)
#define SPEC_CTRL_SSBD                  (1ULL << 2)

/* CPUID.80000021h:EAX (AMD) and EFER.AIBRSE */
#define CPUID80000021_EAX_AUTO_IBRS     (1UL << 8)
#define EFER_AUTOIBRS                   (1ULL << 21)

/* FeatureSettingsOverride bits documented in KB4073119 */
#define FSO_DISABLE_BTI                 0x00000001
#define FSO_ENABLE_SSBD                 0x00000008
#define FSO_AMD_USER_TO_KERNEL          0x00000040

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
#define KSPEC_EIBRS                     0x00000800  /* IBRS_ALL: IBRS may stay set */
#define KSPEC_AUTO_IBRS                 0x00001000  /* AMD automatic IBRS */

extern ULONG KiSpeculationFeatures;
extern ULONG64 KiArchCapabilities;

/* Session Manager\Memory Management values, read by CmGetSystemControlValues */
extern ULONG KiFeatureSettingsOverride;
extern ULONG KiFeatureSettingsOverrideMask;

/* Session Manager\Kernel SpeculationHardening, our own switch until each class has a default */
#define KSPEC_HARDEN_BOUNDS             0x00000001  /* Index masking and fences after bound checks */
#define KSPEC_HARDEN_MDS                0x00000002  /* Clear CPU buffers on return to user mode */
#define KSPEC_HARDEN_BTI                0x00000004  /* Kernel protection against branch target injection */
#define KSPEC_HARDEN_IBPB               0x00000008  /* Barrier on process switch (opt-in) */
#define KSPEC_HARDEN_STIBP              0x00000010  /* Single-thread indirect branch predictors (opt-in) */
#define KSPEC_HARDEN_SSBD               0x00000020  /* Honour the SSBD system and process opt-ins */

/* How speculative store bypass is disabled */
#define KSPEC_SSBD_NONE                 0
#define KSPEC_SSBD_SPEC_CTRL            1
#define KSPEC_SSBD_VIRT_SPEC_CTRL       2

/* How branch target injection is mitigated */
#define KSPEC_BTI_NONE                  0
#define KSPEC_BTI_EIBRS                 1
#define KSPEC_BTI_AUTO_IBRS             2
#define KSPEC_BTI_LEGACY_IBRS           3  /* IBRS written on every kernel entry and exit */

extern ULONG KiSpeculationBtiMode;
extern ULONG KiSpeculationActive;
extern ULONG64 KiSpecCtrlKernel;
extern ULONG64 KiSpecCtrlUser;
extern BOOLEAN KiSpeculationIbpbOnSwitch;
extern ULONG KiSpeculationSsbdMode;
extern BOOLEAN KiSpeculationSsbdPerProcess;

VOID
NTAPI
KiSpeculationSwitchSsbd(
    _In_ PKPROCESS NewProcess);

/* Speculation state that follows the process: predictor barrier and store bypass */
FORCEINLINE
VOID
KiSpeculationProcessSwitch(
    _In_ PKPROCESS NewProcess)
{
    if (KiSpeculationIbpbOnSwitch)
        __writemsr(MSR_IA32_PRED_CMD, 1);
    if (KiSpeculationSsbdPerProcess)
        KiSpeculationSwitchSsbd(NewProcess);
}

VOID
NTAPI
KiApplySpeculationControlProcessor(VOID);

extern ULONG KiSpeculationHardening;
extern ULONG_PTR KiSpeculationIndexMaskOff;
#ifdef _M_IX86
extern BOOLEAN KiSpeculationFenceEnabled;
#endif

/* All ones when Index < Size, zero otherwise, without a branch the processor can mispredict */
FORCEINLINE
ULONG_PTR
KiSpeculationIndexMask(
    _In_ ULONG_PTR Index,
    _In_ ULONG_PTR Size)
{
    ULONG_PTR Mask;
#if defined(__GNUC__) || defined(__clang__)
    __asm__ ("cmp %1, %2; sbb %0, %0" : "=r"(Mask) : "rm"(Size), "r"(Index) : "cc");
#else
    Mask = (ULONG_PTR)(~(LONG_PTR)(Index | (Size - 1 - Index)) >> (sizeof(ULONG_PTR) * 8 - 1));
#endif
    return Mask | KiSpeculationIndexMaskOff;
}

/* KiSpeculationBarrier emits a marked 7-byte nop that KiApplySpeculationPolicy turns into lfence */
FORCEINLINE
VOID
KiSpeculationBarrier(VOID)
{
#if defined(_M_AMD64) && (defined(__GNUC__) || defined(__clang__))
    __asm__ __volatile__ (".byte 0x0F, 0x1F, 0x80, 0x4C, 0x4E, 0x54, 0x46" ::: "memory");
#elif defined(_M_AMD64)
    if (KiSpeculationIndexMaskOff == 0) _mm_lfence();
#else
    if (KiSpeculationFenceEnabled) _mm_lfence();
#endif
}

CODE_SEG("INIT")
VOID
NTAPI
KiDetectSpeculationFeatures(VOID);

CODE_SEG("INIT")
VOID
NTAPI
KiApplySpeculationPolicy(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock);

NTSTATUS
NTAPI
KeQuerySpeculationControlInformation(
    _Out_writes_bytes_to_(Length, *ReturnLength) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength);

#else

#define KiSpeculationBarrier() ((void)0)
#define KiSpeculationIndexMask(Index, Size) ((ULONG_PTR)-1)

#endif /* _M_IX86 || _M_AMD64 */
