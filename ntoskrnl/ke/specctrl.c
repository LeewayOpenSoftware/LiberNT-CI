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

ULONG KiSpeculationHardening = KSPEC_HARDEN_DEFAULTS;
ULONG KiSpeculationBtiMode;
ULONG KiSpeculationActive;
ULONG64 KiSpecCtrlKernel;
ULONG64 KiSpecCtrlUser;
/* What each processor runs with; the entry and exit stubs read these */
ULONG64 KiSpecCtrlKernelCpu[MAXIMUM_PROCESSORS];
ULONG64 KiSpecCtrlUserCpu[MAXIMUM_PROCESSORS];
ULONG KiSpeculationSsbdMode;
#ifdef _M_AMD64
/* Legacy IBRS on a core with a sibling thread: drop IBRS while this thread idles */
BOOLEAN KiSpeculationIdleClear;
#endif
#ifdef _M_AMD64
/* SSBD state per processor when it lives in VIRT_SPEC_CTRL; SPEC_CTRL must never carry it then */
static UCHAR KiVirtSsbdCpu[MAXIMUM_PROCESSORS];
#endif
BOOLEAN KiSpeculationSsbdPerProcess;
static BOOLEAN KiSpeculationSsbdSystemWide;
BOOLEAN KiSpeculationIbpbOnSwitch;
ULONG_PTR KiSpeculationIndexMaskOff = (ULONG_PTR)-1;
#ifdef _M_IX86
BOOLEAN KiSpeculationFenceEnabled;
#endif

#ifdef _M_AMD64
/* Patch site: a 7-byte nop whose first six bytes mark it and whose last byte says what it becomes */
static const UCHAR KiSiteMarker[6] = { 0x0F, 0x1F, 0x80, 0x4C, 0x4E, 0x54 };
#define KSPEC_SITE_FENCE                0x46
#define KSPEC_SITE_VERW                 0x4D
#define KSPEC_SITE_ENTER                0x49
#define KSPEC_SITE_EXIT                 0x58
#define KSPEC_SITE_RSB                  0x52
#define KSPEC_SITE_ENTER_PARANOID       0x50
#define KSPEC_SITE_EXIT_PARANOID        0x70

/* Internal kinds: IBRS written on kernel entry and exit; return stack refill on context switch */
#define KSPEC_PATCH_IBRS                0x80000000
#define KSPEC_PATCH_RSB                 0x40000000

/* lfence followed by a 4-byte nop */
static const UCHAR KiFenceSiteLfence[7] = { 0x0F, 0xAE, 0xE8, 0x0F, 0x1F, 0x40, 0x00 };
/* verw operand: a writable data segment selector */
static USHORT KiVerwSelector = KGDT64_R0_DATA;
/* Sites patched per kind over all boot images */
static ULONG KiPatchedClear, KiPatchedEnter, KiPatchedExit;

VOID KiSpecCtrlEnterKernel(VOID);
VOID KiSpecCtrlExitKernel(VOID);
VOID KiSpecFillReturnStack(VOID);
VOID KiSpecCtrlEnterParanoid(VOID);
VOID KiSpecCtrlExitParanoid(VOID);
#endif

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
KiDetectSpeculationFeatures(VOID)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    ULONG Features = 0;
    ULONG MaxExtended;
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
    MaxExtended = (ULONG)Regs[0];
    if (MaxExtended >= 0x80000008 && MaxExtended < 0x80010000)
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
    if (MaxExtended >= 0x80000021 && MaxExtended < 0x80010000)
    {
        __cpuid(Regs, 0x80000021);
        if ((ULONG)Regs[0] & CPUID80000021_EAX_AUTO_IBRS) Features |= KSPEC_AUTO_IBRS;
    }

    if (Features & KSPEC_ARCH_CAPABILITIES)
    {
        KiArchCapabilities = __readmsr(MSR_IA32_ARCH_CAPABILITIES);
        if (KiArchCapabilities & ARCH_CAP_SSB_NO) Features |= KSPEC_SSB_NO;
        if (KiArchCapabilities & ARCH_CAP_MDS_NO) Features |= KSPEC_MDS_NO;
        if (KiArchCapabilities & ARCH_CAP_IBRS_ALL) Features |= KSPEC_EIBRS;
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
    if (KiSpeculationBtiMode != KSPEC_BTI_NONE)
    {
        Info.Flags |= SPECCTRL_BPB_ENABLED;
        if (KiSpeculationBtiMode != KSPEC_BTI_LEGACY_IBRS)
            Info.Flags |= SPECCTRL_ENHANCED_IBRS;
    }
    else if (Features & (KSPEC_IBRS | KSPEC_PRED_CMD))
        Info.Flags |= SPECCTRL_BPB_DISABLED_SYSTEM_POLICY;
    else
        Info.Flags |= SPECCTRL_BPB_DISABLED_NO_HARDWARE;
    if (Features & KSPEC_SPEC_CTRL) Info.Flags |= SPECCTRL_SPEC_CTRL_ENUMERATED;
    if (Features & KSPEC_PRED_CMD) Info.Flags |= SPECCTRL_PRED_CMD_ENUMERATED;
    if (Features & KSPEC_IBRS) Info.Flags |= SPECCTRL_IBRS_PRESENT;
    if (Features & KSPEC_STIBP) Info.Flags |= SPECCTRL_STIBP_PRESENT;
    if (KeFeatureBits & KF_SMEP) Info.Flags |= SPECCTRL_SMEP_PRESENT;
    if (Features & (KSPEC_SSBD | KSPEC_VIRT_SSBD)) Info.Flags |= SPECCTRL_SSBD_SUPPORTED;
    if (KiSpeculationSsbdMode != KSPEC_SSBD_NONE) Info.Flags |= SPECCTRL_SSBD_AVAILABLE;
    if (KiSpeculationSsbdSystemWide) Info.Flags |= SPECCTRL_SSBD_SYSTEM_WIDE | SPECCTRL_SSBD_KERNEL;
    if (!(Features & KSPEC_SSB_NO)) Info.Flags |= SPECCTRL_SSBD_REQUIRED;
    if (Features & (KSPEC_EIBRS | KSPEC_AUTO_IBRS)) Info.Flags |= SPECCTRL_ENHANCED_IBRS_REPORTED;
    if (Features & KSPEC_MDS_NO) Info.Flags |= SPECCTRL_MDS_HARDWARE_PROTECTED;
    if (Features & KSPEC_MD_CLEAR) Info.Flags |= SPECCTRL_MB_CLEAR_REPORTED;
    if (KiSpeculationActive & KSPEC_HARDEN_MDS) Info.Flags |= SPECCTRL_MB_CLEAR_ENABLED;

    Size = (Length >= sizeof(Info)) ? sizeof(Info) : sizeof(Info.Flags);
    RtlCopyMemory(Buffer, &Info, Size);
    *ReturnLength = Size;
    return STATUS_SUCCESS;
}

#ifdef _M_AMD64
/*
 * An NMI can run a site while it is rewritten, so each step leaves a valid instruction:
 * first a short jump over the site, then the tail, then the head. One store instruction
 * per step is enough on the single processor running at this point.
 */
CODE_SEG("INIT")
static
VOID
KiWriteSite(
    _Inout_updates_(7) PUCHAR Code,
    _In_reads_(7) const UCHAR *NewCode)
{
    ULONG i;

    *(volatile USHORT UNALIGNED *)Code = 0x05EB;
    for (i = 2; i < 7; i++)
        ((volatile UCHAR *)Code)[i] = NewCode[i];
    *(volatile USHORT UNALIGNED *)Code = *(const USHORT UNALIGNED *)NewCode;
}

/* Encode a 7-byte site as an instruction with a rel32 operand followed by padding */
CODE_SEG("INIT")
static
VOID
KiWriteRel32Site(
    _Inout_updates_(7) PUCHAR Code,
    _In_ ULONG OpcodeLength,
    _In_reads_(OpcodeLength) const UCHAR *Opcode,
    _In_ PVOID Target)
{
    LONG Disp = (LONG)((LONG_PTR)Target - (LONG_PTR)(Code + OpcodeLength + sizeof(LONG)));
    UCHAR NewCode[7];
    ULONG i;

    RtlCopyMemory(NewCode, Opcode, OpcodeLength);
    RtlCopyMemory(NewCode + OpcodeLength, &Disp, sizeof(Disp));
    for (i = OpcodeLength + sizeof(LONG); i < 7; i++)
        NewCode[i] = 0x90;
    KiWriteSite(Code, NewCode);
}

/* Find the .spsites table that the SPEC_SITE macro and KiSpeculationBarrier fill */
CODE_SEG("INIT")
static
PULONG
KiFindSiteTable(
    _In_ PVOID ImageBase,
    _In_ PIMAGE_NT_HEADERS NtHeaders,
    _Out_ PULONG Count)
{
    PIMAGE_SECTION_HEADER Section = IMAGE_FIRST_SECTION(NtHeaders);
    ULONG i;

    for (i = 0; i < NtHeaders->FileHeader.NumberOfSections; i++, Section++)
    {
        if (RtlEqualMemory(Section->Name, ".spsites", IMAGE_SIZEOF_SHORT_NAME))
        {
            *Count = min(Section->Misc.VirtualSize, Section->SizeOfRawData) / sizeof(ULONG);
            return (PULONG)((PUCHAR)ImageBase + Section->VirtualAddress);
        }
    }
    *Count = 0;
    return NULL;
}

CODE_SEG("INIT")
static
ULONG
KiPatchImageSites(
    _In_ PVOID ImageBase,
    _In_ ULONG Kinds)
{
    static const UCHAR Verw[3] = { 0x0F, 0x00, 0x2D };
    static const UCHAR Call[1] = { 0xE8 };
    PIMAGE_NT_HEADERS NtHeaders = RtlImageNtHeader(ImageBase);
    ULONG i, Entries, Sites = 0, Listed = 0, Stale = 0;
    ULONG Fence = 0, Clear = 0, Enter = 0, Exit = 0, Refill = 0, Paranoid = 0;
    PULONG Table;
    PUCHAR Code;

    if (!NtHeaders)
        return 0;

    Table = KiFindSiteTable(ImageBase, NtHeaders, &Entries);
    for (i = 0; i < Entries; i++)
    {
        /* Alignment padding between object contributions is zero */
        if (Table[i] == 0 || Table[i] > NtHeaders->OptionalHeader.SizeOfImage - 7)
            continue;

        Listed++;
        Code = (PUCHAR)ImageBase + Table[i];
        if (!RtlEqualMemory(Code, KiSiteMarker, sizeof(KiSiteMarker)))
        {
            Stale++;
            continue;
        }

        switch (Code[6])
        {
            case KSPEC_SITE_FENCE:
                if (!(Kinds & KSPEC_HARDEN_BOUNDS)) continue;
                KiWriteSite(Code, KiFenceSiteLfence);
                Fence++;
                break;

            case KSPEC_SITE_VERW:
                if (!(Kinds & KSPEC_HARDEN_MDS)) continue;
                KiWriteRel32Site(Code, sizeof(Verw), Verw, &KiVerwSelector);
                Clear++;
                break;

            case KSPEC_SITE_ENTER:
                if (!(Kinds & KSPEC_PATCH_IBRS)) continue;
                KiWriteRel32Site(Code, sizeof(Call), Call, (PVOID)KiSpecCtrlEnterKernel);
                Enter++;
                break;

            case KSPEC_SITE_EXIT:
                if (!(Kinds & KSPEC_PATCH_IBRS)) continue;
                KiWriteRel32Site(Code, sizeof(Call), Call, (PVOID)KiSpecCtrlExitKernel);
                Exit++;
                break;

            case KSPEC_SITE_ENTER_PARANOID:
            case KSPEC_SITE_EXIT_PARANOID:
                if (!(Kinds & KSPEC_PATCH_IBRS)) continue;
                KiWriteRel32Site(Code, sizeof(Call), Call,
                                 (Code[6] == KSPEC_SITE_ENTER_PARANOID) ? (PVOID)KiSpecCtrlEnterParanoid :
                                                                          (PVOID)KiSpecCtrlExitParanoid);
                Paranoid++;
                break;

            case KSPEC_SITE_RSB:
                if (!(Kinds & KSPEC_PATCH_RSB)) continue;
                KiWriteRel32Site(Code, sizeof(Call), Call, (PVOID)KiSpecFillReturnStack);
                Refill++;
                break;

            default:
                Stale++;
                continue;
        }
        Sites++;
    }

    /* Listed is every site in the sources; a smoke compares it with the patched counts per kind */
    DPRINT1("Speculation sites at %p: listed %lu, stale %lu, fence %lu, clear %lu, entry %lu, exit %lu, refill %lu, paranoid %lu\n",
            ImageBase, Listed, Stale, Fence, Clear, Enter, Exit, Refill, Paranoid);
    KiPatchedClear += Clear;
    KiPatchedEnter += Enter;
    KiPatchedExit += Exit;
    return Sites;
}

/* The kernel and the HAL carry the trap entry and exit code; they are the first two boot images */
CODE_SEG("INIT")
static
ULONG
KiPatchBootImages(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _Inout_ PULONG Kinds)
{
    PLIST_ENTRY Entry;
    PLDR_DATA_TABLE_ENTRY Images[2] = { NULL, NULL };
    ULONG_PTR Cr0, Flags, Low, High;
    ULONG i, Sites = 0;

    for (i = 0, Entry = LoaderBlock->LoadOrderListHead.Flink;
         i < 2 && Entry != &LoaderBlock->LoadOrderListHead;
         i++, Entry = Entry->Flink)
    {
        Images[i] = CONTAINING_RECORD(Entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
    }

    /* rel32 sites reach the kernel; all of it must stay within 2 GB of every patched image */
    if (Images[0] && Images[1])
    {
        Low = min((ULONG_PTR)Images[0]->DllBase, (ULONG_PTR)Images[1]->DllBase);
        High = max((ULONG_PTR)Images[0]->DllBase + Images[0]->SizeOfImage,
                   (ULONG_PTR)Images[1]->DllBase + Images[1]->SizeOfImage);
        if (High - Low >= 0x7FFF0000)
        {
            DPRINT1("Speculation hardening: kernel and HAL too far apart, rel32 sites not patched\n");
            *Kinds &= ~(KSPEC_HARDEN_MDS | KSPEC_PATCH_IBRS | KSPEC_PATCH_RSB);
        }
    }
    if (!*Kinds)
        return 0;

    /* Boot processor only, before the other processors start; the images may be write-protected */
    Flags = __readeflags();
    _disable();
    Cr0 = __readcr0();
    __writecr0(Cr0 & ~CR0_WP);

    for (i = 0; i < 2 && Images[i]; i++)
    {
        ULONG ImageSites = KiPatchImageSites(Images[i]->DllBase, *Kinds);

        DPRINT1("Speculation sites patched in %wZ: %lu\n", &Images[i]->BaseDllName, ImageSites);
        Sites += ImageSites;
    }

    __writecr0(Cr0);
    __writeeflags(Flags);

    /* A kind without patched sites (for example a lost site table) is not active */
    if (KiPatchedClear == 0)
        *Kinds &= ~KSPEC_HARDEN_MDS;
    if (KiPatchedEnter == 0 || KiPatchedExit == 0)
        *Kinds &= ~KSPEC_PATCH_IBRS;

    /* Serialize so this processor does not run stale instructions */
    {
        INT Regs[4];
        __cpuid(Regs, 0);
    }
    return Sites;
}
#endif

#ifdef _M_AMD64
/* L1 terminal fault: Intel processors without RDCL_NO; AMD is not affected */
CODE_SEG("INIT")
static
BOOLEAN
KiApplyL1tfPolicy(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PMEMORY_ALLOCATION_DESCRIPTOR Descriptor;
    PLIST_ENTRY Entry;
    ULONG64 TopPage = 0, SlotLimit;
    ULONG PhysicalBits = 36;
    BOOLEAN Guest, Enabled;
    INT Regs[4];

    if (KeGetCurrentPrcb()->CpuVendor != CPU_INTEL || (KiArchCapabilities & ARCH_CAP_RDCL_NO))
        return FALSE;

    __cpuid(Regs, 1);
    Guest = ((ULONG)Regs[2] & (1UL << 31)) != 0;
    __cpuid(Regs, 0x80000000);
    if ((ULONG)Regs[0] >= 0x80000008)
    {
        __cpuid(Regs, 0x80000008);
        PhysicalBits = (ULONG)Regs[0] & 0xFF;
    }
    if (PhysicalBits < 36 || PhysicalBits > 52)
        PhysicalBits = 36;

    for (Entry = LoaderBlock->MemoryDescriptorListHead.Flink;
         Entry != &LoaderBlock->MemoryDescriptorListHead;
         Entry = Entry->Flink)
    {
        Descriptor = CONTAINING_RECORD(Entry, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
        if (Descriptor->MemoryType != LoaderBad)
            TopPage = max(TopPage, (ULONG64)Descriptor->BasePage + Descriptor->PageCount);
    }

    /* An inverted value lands at or above 2^(MAXPHYADDR-1) while it stays below 2^(MAXPHYADDR-13) */
    SlotLimit = 1ULL << (PhysicalBits - 13);
    Enabled = MiEnableSoftPteInversion(SlotLimit);
    DPRINT1("L1TF inversion %s: guest %u, MAXPHYADDR %lu, RAM top 0x%I64x, %s, page file limit %I64u MB\n",
            Enabled ? "on" : "not set, entries already made", Guest, PhysicalBits, TopPage << PAGE_SHIFT,
            (TopPage < SlotLimit) ? "complete" : "incomplete, memory reaches 2^(MAXPHYADDR-1)",
            SlotLimit >> (20 - PAGE_SHIFT));
    return Enabled;
}
#endif

#ifdef _M_AMD64
/* The idle path needs a sibling to give the core to, and a wait that wakes on interrupts while they are masked */
CODE_SEG("INIT")
static
BOOLEAN
KiCanClearIbrsWhenIdle(
    _Out_ PCSTR *Reason)
{
    INT Regs[4];
    ULONG MaxLeaf;

    __cpuid(Regs, 0);
    MaxLeaf = (ULONG)Regs[0];
    if (MaxLeaf < 0xB)
    {
        *Reason = "no topology leaf";
        return FALSE;
    }
    __cpuidex(Regs, 0xB, 0);
    if ((((ULONG)Regs[2] >> 8) & 0xFF) != 1 || ((ULONG)Regs[1] & 0xFFFF) < 2)
    {
        *Reason = "no SMT";
        return FALSE;
    }
    __cpuid(Regs, 1);
    if (!((ULONG)Regs[2] & (1UL << 3)))
    {
        *Reason = "no MONITOR/MWAIT";
        return FALSE;
    }
    __cpuid(Regs, 5);
    if (((ULONG)Regs[2] & 3) != 3)
    {
        *Reason = "MWAIT cannot wake on masked interrupts";
        return FALSE;
    }
    *Reason = "SMT, MWAIT";
    return TRUE;
}

/* Called by the idle loop with interrupts disabled; returns with them enabled */
VOID
FASTCALL
KiSpeculationIdle(
    _In_ PKPRCB Prcb)
{
    ULONG Number = Prcb->Number;
    volatile VOID *Line = &Prcb->NextThread;

    /* Interrupts stay masked until IBRS is back, so no handler runs without it; NMIs take the paranoid path */
    __writemsr(MSR_IA32_SPEC_CTRL, KiSpecCtrlUserCpu[Number]);
#if defined(__GNUC__) || defined(__clang__)
    /* The "memory" clobber keeps the re-check below from being hoisted above MONITOR */
    __asm__ __volatile__ ("monitor" :: "a"(Line), "c"(0), "d"(0) : "memory");
    if (Prcb->NextThread == NULL && Prcb->DpcData[DPC_NORMAL].DpcQueueDepth == 0)
        __asm__ __volatile__ ("mwait" :: "a"(0), "c"(1) : "memory");
#else
    _mm_monitor((const void *)Line, 0, 0);
    _ReadWriteBarrier();
    if (Prcb->NextThread == NULL && Prcb->DpcData[DPC_NORMAL].DpcQueueDepth == 0)
        _mm_mwait(1, 0);
#endif
    __writemsr(MSR_IA32_SPEC_CTRL, KiSpecCtrlKernelCpu[Number]);
    _enable();
}
#endif

CODE_SEG("INIT")
VOID
NTAPI
KiApplySpeculationPolicy(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    ULONG Kinds = 0, Sites = 0;
    ULONG64 Base = 0;
    BOOLEAN DefaultPolicy = (KiSpeculationHardening & KSPEC_HARDEN_DEFAULT) != 0;

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

    if ((KiSpeculationHardening & KSPEC_HARDEN_STIBP) && (KiSpeculationFeatures & KSPEC_STIBP))
        Base |= SPEC_CTRL_STIBP;
    if ((KiSpeculationHardening & KSPEC_HARDEN_IBPB) && (KiSpeculationFeatures & KSPEC_PRED_CMD))
        KiSpeculationIbpbOnSwitch = TRUE;

    /* Branch target injection: FeatureSettingsOverride bit 0 always wins */
    if ((KiSpeculationHardening & KSPEC_HARDEN_BTI) &&
        !(KiFeatureSettingsOverride & KiFeatureSettingsOverrideMask & FSO_DISABLE_BTI))
    {
        /* AMD user-to-kernel protection is opt-in on Windows client; the default policy turns AutoIBRS on */
        BOOLEAN Allowed = DefaultPolicy || (KeGetCurrentPrcb()->CpuVendor != CPU_AMD) ||
                          (KiFeatureSettingsOverride & FSO_AMD_USER_TO_KERNEL);

        if (KiSpeculationFeatures & KSPEC_EIBRS)
        {
            KiSpeculationBtiMode = KSPEC_BTI_EIBRS;
        }
#ifdef _M_AMD64
        else if (Allowed && (KiSpeculationFeatures & KSPEC_AUTO_IBRS))
        {
            KiSpeculationBtiMode = KSPEC_BTI_AUTO_IBRS;
        }
        else if (Allowed && !DefaultPolicy && (KiSpeculationFeatures & KSPEC_IBRS))
        {
            KiSpeculationBtiMode = KSPEC_BTI_LEGACY_IBRS;
            Kinds |= KSPEC_PATCH_IBRS;
        }
#else
        UNREFERENCED_PARAMETER(Allowed);
#endif
    }

#ifdef _M_AMD64
    /* Refill the return stack on context switch in every mode, as other kernels do */
    if (KiSpeculationBtiMode != KSPEC_BTI_NONE)
        Kinds |= KSPEC_PATCH_RSB;
    if (Kinds)
        Sites = KiPatchBootImages(LoaderBlock, &Kinds);
    if ((KiSpeculationBtiMode == KSPEC_BTI_LEGACY_IBRS) && !(Kinds & KSPEC_PATCH_IBRS))
        KiSpeculationBtiMode = KSPEC_BTI_NONE;
    Kinds &= ~(KSPEC_PATCH_IBRS | KSPEC_PATCH_RSB);

    /* Before nvs phase 0 writes its first not-present entry */
    if ((KiSpeculationHardening & KSPEC_HARDEN_L1TF) && KiApplyL1tfPolicy(LoaderBlock))
        Kinds |= KSPEC_HARDEN_L1TF;
#else
    UNREFERENCED_PARAMETER(LoaderBlock);
    /* No buffer clearing sites on this architecture yet */
    Kinds &= ~KSPEC_HARDEN_MDS;
#endif

    /* Speculative store bypass: off unless the system (FeatureSettingsOverride bit 3) or a process opts in */
    if (KiSpeculationHardening & KSPEC_HARDEN_SSBD)
    {
        if (KiSpeculationFeatures & KSPEC_SSBD)
            KiSpeculationSsbdMode = KSPEC_SSBD_SPEC_CTRL;
#ifdef _M_AMD64
        else if (KiSpeculationFeatures & KSPEC_VIRT_SSBD)
            KiSpeculationSsbdMode = KSPEC_SSBD_VIRT_SPEC_CTRL;
#endif
        if (KiSpeculationSsbdMode != KSPEC_SSBD_NONE)
        {
            if (KiFeatureSettingsOverride & FSO_ENABLE_SSBD)
                KiSpeculationSsbdSystemWide = TRUE;
            else
                KiSpeculationSsbdPerProcess = TRUE;
        }
        if (KiSpeculationSsbdSystemWide && KiSpeculationSsbdMode == KSPEC_SSBD_SPEC_CTRL)
            Base |= SPEC_CTRL_SSBD;
    }

    /* The user value is what runs outside the kernel; with IBRS on entry the kernel adds IBRS */
    KiSpecCtrlUser = Base;
    KiSpecCtrlKernel = Base;
    if (KiSpeculationBtiMode == KSPEC_BTI_EIBRS)
    {
        KiSpecCtrlUser |= SPEC_CTRL_IBRS;
        KiSpecCtrlKernel |= SPEC_CTRL_IBRS;
    }
    else if (KiSpeculationBtiMode == KSPEC_BTI_LEGACY_IBRS)
    {
        KiSpecCtrlKernel |= SPEC_CTRL_IBRS;
    }

    /* The other processors apply it when they start */
    KiApplySpeculationControlProcessor();

#ifdef _M_AMD64
    if (KiSpeculationBtiMode == KSPEC_BTI_LEGACY_IBRS)
    {
        PCSTR Reason;

        KiSpeculationIdleClear = KiCanClearIbrsWhenIdle(&Reason);
        DPRINT1("Speculation idle: IBRS %s while idle (%s)\n",
                KiSpeculationIdleClear ? "cleared" : "kept", Reason);
    }
#endif

    if (KiSpeculationHardening & KSPEC_HARDEN_BTI)
        Kinds |= (KiSpeculationBtiMode != KSPEC_BTI_NONE) ? KSPEC_HARDEN_BTI : 0;
    if (KiSpeculationSsbdMode != KSPEC_SSBD_NONE)
        Kinds |= KSPEC_HARDEN_SSBD;
    KiSpeculationActive = Kinds;
    DPRINT1("Speculation hardening 0x%lx: active 0x%lx, %lu sites, BTI mode %lu, SPEC_CTRL 0x%I64x/0x%I64x, IBPB %u, SSBD mode %lu %s\n",
            KiSpeculationHardening, Kinds, Sites, KiSpeculationBtiMode,
            KiSpecCtrlKernel, KiSpecCtrlUser, KiSpeculationIbpbOnSwitch, KiSpeculationSsbdMode,
            KiSpeculationSsbdSystemWide ? "system" : (KiSpeculationSsbdPerProcess ? "per process" : "off"));
    DPRINT1("Speculation BTI %s, %s policy%s\n",
            KiSpeculationBtiMode == KSPEC_BTI_EIBRS ? "eIBRS" :
            KiSpeculationBtiMode == KSPEC_BTI_AUTO_IBRS ? "AutoIBRS" :
            KiSpeculationBtiMode == KSPEC_BTI_LEGACY_IBRS ? "legacy IBRS" : "off",
            DefaultPolicy ? "default" : "registry",
            (KiFeatureSettingsOverride & KiFeatureSettingsOverrideMask & FSO_DISABLE_BTI) ? ", disabled by FeatureSettingsOverride" :
            (DefaultPolicy && KiSpeculationBtiMode == KSPEC_BTI_NONE && (KiSpeculationFeatures & KSPEC_IBRS)) ?
                ", legacy IBRS needs SpeculationHardening bit 2" : "");
}

VOID
NTAPI
KiApplySpeculationControlProcessor(VOID)
{
    ULONG Number = KeGetCurrentPrcb()->Number;

    KiSpecCtrlKernelCpu[Number] = KiSpecCtrlKernel;
    KiSpecCtrlUserCpu[Number] = KiSpecCtrlUser;

    /* Leave the MSRs alone unless a policy asks for something; the processor is in the kernel here */
    if (KiSpecCtrlKernel != 0)
        __writemsr(MSR_IA32_SPEC_CTRL, KiSpecCtrlKernel);
#ifdef _M_AMD64
    if (KiSpeculationBtiMode == KSPEC_BTI_AUTO_IBRS)
        __writemsr(MSR_EFER, __readmsr(MSR_EFER) | EFER_AUTOIBRS);
    if (KiSpeculationSsbdSystemWide && KiSpeculationSsbdMode == KSPEC_SSBD_VIRT_SPEC_CTRL)
        __writemsr(MSR_AMD64_VIRT_SPEC_CTRL, SPEC_CTRL_SSBD);
#endif
}

/* Called on process switch when processes may opt in to SSBD */
VOID
NTAPI
KiSpeculationSwitchSsbd(
    _In_ PKPROCESS NewProcess)
{
    ULONG Number = KeGetCurrentPrcb()->Number;
    ULONG64 Want = ((PEPROCESS)NewProcess)->MitigationFlags2Values.SpeculativeStoreBypassDisable ?
                   SPEC_CTRL_SSBD : 0;

#ifdef _M_AMD64
    if (KiSpeculationSsbdMode == KSPEC_SSBD_VIRT_SPEC_CTRL)
    {
        if (KiVirtSsbdCpu[Number] == (Want != 0))
            return;
        KiVirtSsbdCpu[Number] = (Want != 0);
        __writemsr(MSR_AMD64_VIRT_SPEC_CTRL, Want);
        return;
    }
#endif

    if ((KiSpecCtrlUserCpu[Number] & SPEC_CTRL_SSBD) == Want)
        return;

    KiSpecCtrlUserCpu[Number] = (KiSpecCtrlUserCpu[Number] & ~SPEC_CTRL_SSBD) | Want;
    KiSpecCtrlKernelCpu[Number] = (KiSpecCtrlKernelCpu[Number] & ~SPEC_CTRL_SSBD) | Want;
    __writemsr(MSR_IA32_SPEC_CTRL, KiSpecCtrlKernelCpu[Number]);
}
