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

ULONG KiSpeculationHardening;
ULONG_PTR KiSpeculationIndexMaskOff = (ULONG_PTR)-1;
#ifdef _M_IX86
BOOLEAN KiSpeculationFenceEnabled;
#endif

#ifdef _M_AMD64
static const UCHAR KiFenceSiteMarker[7] = { KSPEC_FENCE_SITE_BYTES };
/* lfence followed by a 4-byte nop */
static const UCHAR KiFenceSiteLfence[7] = { 0x0F, 0xAE, 0xE8, 0x0F, 0x1F, 0x40, 0x00 };
static const UCHAR KiVerwSiteMarker[7] = { KSPEC_VERW_SITE_BYTES };
/* verw operand: a writable data segment selector */
static USHORT KiVerwSelector = KGDT64_R0_DATA;
#endif

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

#ifdef _M_AMD64
CODE_SEG("INIT")
static
ULONG
KiPatchImageSites(
    _In_ PVOID ImageBase,
    _In_ ULONG Kinds)
{
    PIMAGE_NT_HEADERS NtHeaders = RtlImageNtHeader(ImageBase);
    PIMAGE_SECTION_HEADER Section;
    ULONG i, Sites = 0;
    PUCHAR Code, End;

    if (!NtHeaders)
        return 0;

    Section = IMAGE_FIRST_SECTION(NtHeaders);
    for (i = 0; i < NtHeaders->FileHeader.NumberOfSections; i++, Section++)
    {
        if (!(Section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;

        Code = (PUCHAR)ImageBase + Section->VirtualAddress;
        End = Code + min(Section->Misc.VirtualSize, Section->SizeOfRawData);
        for (; Code + sizeof(KiFenceSiteMarker) <= End; Code++)
        {
            if (Code[0] != 0x0F)
                continue;

            if ((Kinds & KSPEC_HARDEN_BOUNDS) &&
                RtlEqualMemory(Code, KiFenceSiteMarker, sizeof(KiFenceSiteMarker)))
            {
                RtlCopyMemory(Code, KiFenceSiteLfence, sizeof(KiFenceSiteLfence));
                Code += sizeof(KiFenceSiteMarker) - 1;
                Sites++;
            }
            else if ((Kinds & KSPEC_HARDEN_MDS) &&
                     RtlEqualMemory(Code, KiVerwSiteMarker, sizeof(KiVerwSiteMarker)))
            {
                /* verw word ptr [rip + disp32] */
                LONG64 Disp = (LONG64)((ULONG_PTR)&KiVerwSelector - (ULONG_PTR)(Code + 7));
                LONG Disp32 = (LONG)Disp;

                if (Disp32 != Disp)
                    continue;

                Code[0] = 0x0F;
                Code[1] = 0x00;
                Code[2] = 0x2D;
                RtlCopyMemory(&Code[3], &Disp32, sizeof(Disp32));
                Code += sizeof(KiVerwSiteMarker) - 1;
                Sites++;
            }
        }
    }
    return Sites;
}

/* The kernel and the HAL carry the trap entry and exit code; they are the first two boot images */
CODE_SEG("INIT")
static
ULONG
KiPatchBootImages(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ ULONG Kinds)
{
    PLIST_ENTRY Entry = LoaderBlock->LoadOrderListHead.Flink;
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    ULONG_PTR Cr0, Flags;
    ULONG i, Sites = 0;

    /* Boot processor only, before the other processors start; the images may be write-protected */
    Flags = __readeflags();
    _disable();
    Cr0 = __readcr0();
    __writecr0(Cr0 & ~CR0_WP);

    for (i = 0; i < 2 && Entry != &LoaderBlock->LoadOrderListHead; i++, Entry = Entry->Flink)
    {
        ULONG ImageSites;

        LdrEntry = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        ImageSites = KiPatchImageSites(LdrEntry->DllBase, Kinds);
        DPRINT1("Speculation sites patched in %wZ: %lu\n", &LdrEntry->BaseDllName, ImageSites);
        Sites += ImageSites;
    }

    __writecr0(Cr0);
    __writeeflags(Flags);

    /* Serialize so this processor does not run stale instructions */
    {
        INT Regs[4];
        __cpuid(Regs, 0);
    }
    return Sites;
}
#endif

CODE_SEG("INIT")
VOID
NTAPI
KiApplySpeculationPolicy(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    ULONG Kinds = 0, Sites = 0;

    if (KiSpeculationHardening & KSPEC_HARDEN_BOUNDS)
    {
        Kinds |= KSPEC_HARDEN_BOUNDS;
        KiSpeculationIndexMaskOff = 0;
#ifdef _M_IX86
        /* lfence needs SSE2 */
        if (KeFeatureBits & KF_XMMI64)
            KiSpeculationFenceEnabled = TRUE;
#endif
    }

    /* verw clears the buffers only where MD_CLEAR is enumerated, and is needed only where MDS applies */
    if ((KiSpeculationHardening & KSPEC_HARDEN_MDS) &&
        (KiSpeculationFeatures & KSPEC_MD_CLEAR) &&
        !(KiSpeculationFeatures & KSPEC_MDS_NO))
    {
        Kinds |= KSPEC_HARDEN_MDS;
    }

#ifdef _M_AMD64
    if (Kinds)
        Sites = KiPatchBootImages(LoaderBlock, Kinds);
#else
    UNREFERENCED_PARAMETER(LoaderBlock);
#endif
    DPRINT1("Speculation hardening 0x%lx: active 0x%lx, %lu sites patched\n",
            KiSpeculationHardening, Kinds, Sites);
}
