/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Corrected machine checks of x86 processors, polled and reported as hardware errors
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntoskrnl.h>
#include "../wheap.h"
#define NDEBUG
#include <debug.h>

#define WHEAP_MCA_POLL_SECONDS 60
#define WHEAP_MCA_RECORDS 16
#define WHEAP_MCA_CPUID_MCA 0x00004000

typedef struct _WHEAP_MCA_PACKET
{
    WHEA_ERROR_PACKET_V2 Header;
    WHEA_XPF_MCA_SECTION Data;
} WHEAP_MCA_PACKET, *PWHEAP_MCA_PACKET;

typedef struct _WHEAP_MCA_RECORD
{
    WHEA_ERROR_RECORD_HEADER Header;
    WHEA_ERROR_RECORD_SECTION_DESCRIPTOR Section;
    WHEA_XPF_MCA_SECTION Data;
} WHEAP_MCA_RECORD, *PWHEAP_MCA_RECORD;

C_ASSERT(sizeof(WHEAP_MCA_RECORD) ==
         sizeof(WHEA_ERROR_RECORD_HEADER) + sizeof(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR) + sizeof(WHEA_XPF_MCA_SECTION));

static const GUID WheapCmcNotifyType = { 0x2DCE8BB1, 0xBDD7, 0x450E, { 0xB9, 0xAD, 0x9C, 0xF4, 0xEB, 0xD4, 0xF8, 0x90 } };
static const GUID WheapMceNotifyType = { 0xE8F56FFE, 0x919C, 0x4CC5, { 0xBA, 0x88, 0x65, 0xAB, 0xE1, 0x49, 0x13, 0xBB } };
static const GUID WheapMcaSectionType = { 0x8A1E1D01, 0x42F9, 0x4557, { 0x9C, 0x33, 0x56, 0x5E, 0x5C, 0xC3, 0xF7, 0xE8 } };

static KTIMER WheapMcaTimer;
static KDPC WheapMcaTimerDpc;
static KDPC WheapMcaProcessorDpc[MAXIMUM_PROCESSORS];
static ULONG WheapMcaProcessorCount;
static ULONG WheapMcaSourceId;

static
VOID
WheapMcaScanProcessor(VOID)
{
    WHEAP_MCA_PACKET Packet;
    PKPRCB Prcb = KeGetCurrentPrcb();
    LARGE_INTEGER SystemTime;
    MCG_STATUS GlobalStatus;
    MCG_CAP Capability;
    MCI_STATUS Status;
    ULONG Bank;

    GlobalStatus.QuadPart = __readmsr(MSR_MCG_STATUS);
    if (GlobalStatus.MachineCheckInProgress)
        return;

    Capability.QuadPart = __readmsr(MSR_MCG_CAP);
    for (Bank = 0; Bank < Capability.CountField; Bank++)
    {
        /* TODO: an uncorrected error found here, left from before the boot or signalled without an exception, is not reported */
        Status.QuadPart = __readmsr(MSR_MC0_STATUS + 4 * Bank);
        if (!Status.CommonBits.Valid || Status.CommonBits.UncorrectedError)
            continue;

        RtlZeroMemory(&Packet, sizeof(Packet));
        Packet.Header.Signature = WHEA_ERROR_PACKET_V2_SIGNATURE;
        Packet.Header.Version = WHEA_ERROR_PACKET_V2_VERSION;
        Packet.Header.Length = sizeof(Packet);
        Packet.Header.ErrorType = WheaErrTypeProcessor;
        Packet.Header.ErrorSeverity = WheaErrSevCorrected;
        Packet.Header.ErrorSourceId = WheapMcaSourceId;
        Packet.Header.ErrorSourceType = WheaErrSrcTypeCMC;
        Packet.Header.NotifyType = WheapCmcNotifyType;
        Packet.Header.DataFormat = WheaDataFormatXPFMCA;
        Packet.Header.DataOffset = FIELD_OFFSET(WHEAP_MCA_PACKET, Data);
        Packet.Header.DataLength = sizeof(Packet.Data);

        KeQuerySystemTime(&SystemTime);
        Packet.Data.VersionNumber = WHEA_XPF_MCA_SECTION_VERSION;
        Packet.Data.CpuVendor = Prcb->CpuVendor == CPU_INTEL ? WheaCpuVendorIntel :
                                Prcb->CpuVendor == CPU_AMD ? WheaCpuVendorAmd : WheaCpuVendorOther;
        Packet.Data.Timestamp = SystemTime;
        Packet.Data.ProcessorNumber = Prcb->Number;
        Packet.Data.GlobalStatus = GlobalStatus;
        Packet.Data.BankNumber = Bank;
        Packet.Data.Status = Status;
        if (Status.CommonBits.AddressValid)
            Packet.Data.Address = __readmsr(MSR_MC0_ADDR + 4 * Bank);
        if (Status.CommonBits.MiscValid)
            Packet.Data.Misc = __readmsr(MSR_MC0_MISC + 4 * Bank);
        Packet.Data.ApicId = Prcb->InitialApicId;
        Packet.Data.GlobalCapability = Capability;

        if (NT_SUCCESS(WheaReportHwError(&Packet.Header)))
            __writemsr(MSR_MC0_STATUS + 4 * Bank, 0);
    }
}

ULONG
NTAPI
WheaMcaBuildFatalRecord(
    _Out_writes_bytes_(BufferLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _In_ ULONG64 InstructionPointer,
    _Out_ PULONG ReportedBank,
    _Out_ PULONG64 ReportedStatus)
{
    PWHEAP_MCA_RECORD Record = Buffer;
    PKPRCB Prcb = KeGetCurrentPrcb();
    LARGE_INTEGER SystemTime;
    MCI_STATUS Status, Chosen;
    MCG_CAP Capability;
    ULONG Bank, ChosenBank = 0;

    if (!Buffer || BufferLength < sizeof(*Record))
        return 0;

    Chosen.QuadPart = 0;
    Capability.QuadPart = __readmsr(MSR_MCG_CAP);
    for (Bank = 0; Bank < Capability.CountField; Bank++)
    {
        Status.QuadPart = __readmsr(MSR_MC0_STATUS + 4 * Bank);
        if (!Status.CommonBits.Valid)
            continue;

        if (!Chosen.CommonBits.Valid || (Status.CommonBits.UncorrectedError && !Chosen.CommonBits.UncorrectedError))
        {
            Chosen = Status;
            ChosenBank = Bank;
        }
    }

    if (!Chosen.CommonBits.Valid)
        return 0;

    RtlZeroMemory(Record, sizeof(*Record));
    WheaInitializeRecordHeader(&Record->Header);
    Record->Header.SectionCount = 1;
    Record->Header.Severity = WheaErrSevFatal;
    Record->Header.Length = sizeof(*Record);
    Record->Header.NotifyType = WheapMceNotifyType;

    Record->Section.SectionOffset = FIELD_OFFSET(WHEAP_MCA_RECORD, Data);
    Record->Section.SectionLength = sizeof(Record->Data);
    Record->Section.Revision.AsUSHORT = WHEAP_SECTION_REVISION;
    Record->Section.SectionType = WheapMcaSectionType;
    Record->Section.SectionSeverity = WheaErrSevFatal;

    KeQuerySystemTime(&SystemTime);
    Record->Data.VersionNumber = WHEA_XPF_MCA_SECTION_VERSION;
    Record->Data.CpuVendor = Prcb->CpuVendor == CPU_INTEL ? WheaCpuVendorIntel :
                             Prcb->CpuVendor == CPU_AMD ? WheaCpuVendorAmd : WheaCpuVendorOther;
    Record->Data.Timestamp = SystemTime;
    Record->Data.ProcessorNumber = Prcb->Number;
    Record->Data.GlobalStatus.QuadPart = __readmsr(MSR_MCG_STATUS);
    Record->Data.InstructionPointer = InstructionPointer;
    Record->Data.BankNumber = ChosenBank;
    Record->Data.Status = Chosen;
    if (Chosen.CommonBits.AddressValid)
        Record->Data.Address = __readmsr(MSR_MC0_ADDR + 4 * ChosenBank);
    if (Chosen.CommonBits.MiscValid)
        Record->Data.Misc = __readmsr(MSR_MC0_MISC + 4 * ChosenBank);
    Record->Data.ApicId = Prcb->InitialApicId;
    Record->Data.GlobalCapability = Capability;

    *ReportedBank = ChosenBank;
    *ReportedStatus = Chosen.QuadPart;
    return sizeof(*Record);
}

static
VOID
NTAPI
WheapMcaProcessorDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    WheapMcaScanProcessor();
}

static
VOID
NTAPI
WheapMcaTimerDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    ULONG Index;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    for (Index = 0; Index < WheapMcaProcessorCount; Index++)
        KeInsertQueueDpc(&WheapMcaProcessorDpc[Index], NULL, NULL);
}

CODE_SEG("INIT")
VOID
NTAPI
WheaMcaInitialize(VOID)
{
    WHEAP_DESCRIPTOR_HEADER Descriptor;
    PROCESSOR_NUMBER Processor;
    LARGE_INTEGER DueTime;
    CPU_INFO CpuInfo;
    MCG_CAP Capability;
    ULONG Count, Index;
    PKDPC Dpc;

    KiCpuId(&CpuInfo, 1);
    if (!(CpuInfo.Edx & X86_FEATURE_MCE) || !(CpuInfo.Edx & WHEAP_MCA_CPUID_MCA))
        return;

    Capability.QuadPart = __readmsr(MSR_MCG_CAP);
    if (Capability.CountField == 0)
        return;

    RtlZeroMemory(&Descriptor, sizeof(Descriptor));
    Descriptor.Length = sizeof(Descriptor);
    Descriptor.Version = WHEA_ERROR_SOURCE_DESCRIPTOR_VERSION_10;
    Descriptor.Type = WheaErrSrcTypeCMC;
    Descriptor.State = WheaErrSrcStateStarted;
    Descriptor.MaxRawDataLength = sizeof(WHEA_XPF_MCA_SECTION);
    Descriptor.NumRecordsToPreallocate = WHEAP_MCA_RECORDS;
    Descriptor.MaxSectionsPerRecord = 1;
    if (!NT_SUCCESS(WheaAddErrorSource((PWHEA_ERROR_SOURCE_DESCRIPTOR)&Descriptor, NULL)))
        return;

    WheapMcaSourceId = Descriptor.ErrorSourceId;
    Count = min(KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS), MAXIMUM_PROCESSORS);
    for (Index = 0; Index < Count; Index++)
    {
        Dpc = &WheapMcaProcessorDpc[WheapMcaProcessorCount];
        KeInitializeDpc(Dpc, WheapMcaProcessorDpcRoutine, NULL);
        if (NT_SUCCESS(KeGetProcessorNumberFromIndex(Index, &Processor)) &&
            NT_SUCCESS(KeSetTargetProcessorDpcEx(Dpc, &Processor)))
        {
            WheapMcaProcessorCount++;
        }
    }

    KeInitializeTimer(&WheapMcaTimer);
    KeInitializeDpc(&WheapMcaTimerDpc, WheapMcaTimerDpcRoutine, NULL);
    DueTime.QuadPart = -(LONGLONG)WHEAP_MCA_POLL_SECONDS * 10000000;
    KeSetTimerEx(&WheapMcaTimer, DueTime, WHEAP_MCA_POLL_SECONDS * 1000, &WheapMcaTimerDpc);
}
