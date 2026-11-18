/******************************************************************************
 *                 Windows Hardware Error Architecture Types                  *
 ******************************************************************************/
$if (_NTDDK_)
#pragma pack(push, 1)

typedef union _WHEA_ERROR_RECORD_SECTION_DESCRIPTOR_FLAGS
{
    struct {
        ULONG Primary : 1;
        ULONG ContainmentWarning : 1;
        ULONG Reset : 1;
        ULONG ThresholdExceeded : 1;
        ULONG ResourceNotAvailable : 1;
        ULONG LatentError : 1;
        ULONG Reserved : 26;
    } DUMMYSTRUCTNAME;
    ULONG AsULONG;
} WHEA_ERROR_RECORD_SECTION_DESCRIPTOR_FLAGS, *PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR_FLAGS;

typedef union _WHEA_ERROR_RECORD_SECTION_DESCRIPTOR_VALIDBITS
{
    struct {
        UCHAR FRUId : 1;
        UCHAR FRUText : 1;
        UCHAR Reserved : 6;
    } DUMMYSTRUCTNAME;
    UCHAR AsUCHAR;
} WHEA_ERROR_RECORD_SECTION_DESCRIPTOR_VALIDBITS, *PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR_VALIDBITS;

typedef union _WHEA_REVISION
{
    struct {
        UCHAR MinorRevision;
        UCHAR MajorRevision;
    } DUMMYSTRUCTNAME;
    USHORT AsUSHORT;
} WHEA_REVISION, *PWHEA_REVISION;

typedef enum _WHEA_ERROR_SEVERITY
{
    WheaErrSevRecoverable   = 0,
    WheaErrSevFatal         = 1,
    WheaErrSevCorrected     = 2,
    WheaErrSevInformational = 3
} WHEA_ERROR_SEVERITY, *PWHEA_ERROR_SEVERITY;

typedef struct _WHEA_ERROR_RECORD_SECTION_DESCRIPTOR
{
    ULONG SectionOffset;
    ULONG SectionLength;
    WHEA_REVISION Revision;
    WHEA_ERROR_RECORD_SECTION_DESCRIPTOR_VALIDBITS ValidBits;
    UCHAR Reserved;
    WHEA_ERROR_RECORD_SECTION_DESCRIPTOR_FLAGS Flags;
    GUID SectionType;
    GUID FRUId;
    WHEA_ERROR_SEVERITY SectionSeverity;
    CCHAR FRUText[20];
} WHEA_ERROR_RECORD_SECTION_DESCRIPTOR, *PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR;

typedef union _WHEA_PROCESSOR_GENERIC_ERROR_SECTION_VALIDBITS
{
    struct {
        ULONGLONG ProcessorType : 1;
        ULONGLONG InstructionSet : 1;
        ULONGLONG ErrorType : 1;
        ULONGLONG Operation : 1;
        ULONGLONG Flags : 1;
        ULONGLONG Level : 1;
        ULONGLONG CPUVersion : 1;
        ULONGLONG CPUBrandString : 1;
        ULONGLONG ProcessorId : 1;
        ULONGLONG TargetAddress : 1;
        ULONGLONG RequesterId : 1;
        ULONGLONG ResponderId : 1;
        ULONGLONG InstructionPointer : 1;
        ULONGLONG Reserved : 51;
    } DUMMYSTRUCTNAME;
    ULONGLONG ValidBits;
} WHEA_PROCESSOR_GENERIC_ERROR_SECTION_VALIDBITS, *PWHEA_PROCESSOR_GENERIC_ERROR_SECTION_VALIDBITS;

typedef struct _WHEA_PROCESSOR_GENERIC_ERROR_SECTION
{
    WHEA_PROCESSOR_GENERIC_ERROR_SECTION_VALIDBITS ValidBits;
    UCHAR ProcessorType;
    UCHAR InstructionSet;
    UCHAR ErrorType;
    UCHAR Operation;
    UCHAR Flags;
    UCHAR Level;
    USHORT Reserved;
    ULONGLONG CPUVersion;
    UCHAR CPUBrandString[128];
    ULONGLONG ProcessorId;
    ULONGLONG TargetAddress;
    ULONGLONG RequesterId;
    ULONGLONG ResponderId;
    ULONGLONG InstructionPointer;
} WHEA_PROCESSOR_GENERIC_ERROR_SECTION, *PWHEA_PROCESSOR_GENERIC_ERROR_SECTION;

#pragma pack(pop)

typedef struct _WHEA_RECOVERY_CONTEXT
{
    union {
        struct {
            ULONG_PTR Address;
            BOOLEAN Consumed;
            UINT16 ErrorCode;
            BOOLEAN ErrorIpValid;
            BOOLEAN RestartIpValid;
        } MemoryError;
    };
    UINT64 PartitionId;
    UINT32 VpIndex;
} WHEA_RECOVERY_CONTEXT, *PWHEA_RECOVERY_CONTEXT;

#pragma pack(push, 1)

typedef enum _WHEA_ERROR_TYPE {
    WheaErrTypeProcessor = 0,
    WheaErrTypeMemory,
    WheaErrTypePCIExpress,
    WheaErrTypeNMI,
    WheaErrTypePCIXBus,
    WheaErrTypePCIXDevice,
    WheaErrTypeGeneric,
    WheaErrTypePmem
} WHEA_ERROR_TYPE, *PWHEA_ERROR_TYPE;

#pragma pack(pop)

typedef enum _WHEA_ERROR_SOURCE_TYPE {
    WheaErrSrcTypeMCE = 0,
    WheaErrSrcTypeCMC,
    WheaErrSrcTypeCPE,
    WheaErrSrcTypeNMI,
    WheaErrSrcTypePCIe,
    WheaErrSrcTypeGeneric,
    WheaErrSrcTypeINIT,
    WheaErrSrcTypeBOOT,
    WheaErrSrcTypeSCIGeneric,
    WheaErrSrcTypeIPFMCA,
    WheaErrSrcTypeIPFCMC,
    WheaErrSrcTypeIPFCPE,
    WheaErrSrcTypeGenericV2,
    WheaErrSrcTypeSCIGenericV2,
    WheaErrSrcTypeBMC,
    WheaErrSrcTypePMEM,
    WheaErrSrcTypeDeviceDriver,
    WheaErrSrcTypeSea,
    WheaErrSrcTypeSei,
    WheaErrSrcTypeMax
} WHEA_ERROR_SOURCE_TYPE, *PWHEA_ERROR_SOURCE_TYPE;

typedef enum _WHEA_ERROR_SOURCE_STATE {
    WheaErrSrcStateStopped = 1,
    WheaErrSrcStateStarted = 2,
    WheaErrSrcStateRemoved = 3,
    WheaErrSrcStateRemovePending = 4
} WHEA_ERROR_SOURCE_STATE, *PWHEA_ERROR_SOURCE_STATE;

#define WHEA_ERROR_SOURCE_DESCRIPTOR_VERSION_10 10

#pragma pack(push, 1)

typedef union _WHEA_ERROR_RECORD_HEADER_VALIDBITS {
    struct {
        ULONG PlatformId : 1;
        ULONG Timestamp : 1;
        ULONG PartitionId : 1;
        ULONG Reserved : 29;
    } DUMMYSTRUCTNAME;
    ULONG AsULONG;
} WHEA_ERROR_RECORD_HEADER_VALIDBITS, *PWHEA_ERROR_RECORD_HEADER_VALIDBITS;

typedef union _WHEA_ERROR_RECORD_HEADER_FLAGS {
    struct {
        ULONG Recovered : 1;
        ULONG PreviousError : 1;
        ULONG Simulated : 1;
        ULONG DeviceDriver : 1;
        ULONG CriticalEvent : 1;
        ULONG PersistPfn : 1;
        ULONG SectionsTruncated : 1;
        ULONG RecoveryInProgress : 1;
        ULONG Throttle : 1;
        ULONG Reserved : 23;
    } DUMMYSTRUCTNAME;
    ULONG AsULONG;
} WHEA_ERROR_RECORD_HEADER_FLAGS, *PWHEA_ERROR_RECORD_HEADER_FLAGS;

typedef union _WHEA_TIMESTAMP {
    struct {
        ULONGLONG Seconds : 8;
        ULONGLONG Minutes : 8;
        ULONGLONG Hours : 8;
        ULONGLONG Precise : 1;
        ULONGLONG Reserved : 7;
        ULONGLONG Day : 8;
        ULONGLONG Month : 8;
        ULONGLONG Year : 8;
        ULONGLONG Century : 8;
    } DUMMYSTRUCTNAME;
    LARGE_INTEGER AsLARGE_INTEGER;
} WHEA_TIMESTAMP, *PWHEA_TIMESTAMP;

typedef union _WHEA_PERSISTENCE_INFO {
    struct {
        ULONGLONG Signature : 16;
        ULONGLONG Length : 24;
        ULONGLONG Identifier : 16;
        ULONGLONG Attributes : 2;
        ULONGLONG DoNotLog : 1;
        ULONGLONG Reserved : 5;
    } DUMMYSTRUCTNAME;
    ULONGLONG AsULONGLONG;
} WHEA_PERSISTENCE_INFO, *PWHEA_PERSISTENCE_INFO;

#define WHEA_ERROR_RECORD_SIGNATURE     'REPC'
#define WHEA_ERROR_RECORD_SIGNATURE_END 0xFFFFFFFF

typedef struct _WHEA_ERROR_RECORD_HEADER {
    ULONG Signature;
    WHEA_REVISION Revision;
    ULONG SignatureEnd;
    USHORT SectionCount;
    WHEA_ERROR_SEVERITY Severity;
    WHEA_ERROR_RECORD_HEADER_VALIDBITS ValidBits;
    ULONG Length;
    WHEA_TIMESTAMP Timestamp;
    GUID PlatformId;
    GUID PartitionId;
    GUID CreatorId;
    GUID NotifyType;
    ULONGLONG RecordId;
    WHEA_ERROR_RECORD_HEADER_FLAGS Flags;
    WHEA_PERSISTENCE_INFO PersistenceInfo;
    union {
        struct {
            ULONG OsBuildNumber;
            UCHAR Reserved2[8];
        } DUMMYSTRUCTNAME;
        UCHAR Reserved[12];
    } DUMMYUNIONNAME;
} WHEA_ERROR_RECORD_HEADER, *PWHEA_ERROR_RECORD_HEADER;

typedef struct _WHEA_ERROR_RECORD {
    WHEA_ERROR_RECORD_HEADER Header;
    WHEA_ERROR_RECORD_SECTION_DESCRIPTOR SectionDescriptor[ANYSIZE_ARRAY];
} WHEA_ERROR_RECORD, *PWHEA_ERROR_RECORD;

#define WHEA_INVALID_ERR_SRC_ID 0

#define WHEA_DEVICE_DRIVER_CONFIG_V1 1
#define WHEA_DEVICE_DRIVER_CONFIG_V2 2

typedef
NTSTATUS
(NTAPI _WHEA_ERROR_SOURCE_INITIALIZE_DEVICE_DRIVER)(
    _Inout_opt_ PVOID Context,
    _In_ ULONG ErrorSourceId);
typedef _WHEA_ERROR_SOURCE_INITIALIZE_DEVICE_DRIVER *WHEA_ERROR_SOURCE_INITIALIZE_DEVICE_DRIVER;

typedef
VOID
(NTAPI _WHEA_ERROR_SOURCE_UNINITIALIZE_DEVICE_DRIVER)(
    _Inout_opt_ PVOID Context);
typedef _WHEA_ERROR_SOURCE_UNINITIALIZE_DEVICE_DRIVER *WHEA_ERROR_SOURCE_UNINITIALIZE_DEVICE_DRIVER;

typedef struct _WHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER_V1 {
    ULONG Version;
    GUID SourceGuid;
    USHORT LogTag;
    UCHAR Reserved[6];
    WHEA_ERROR_SOURCE_INITIALIZE_DEVICE_DRIVER Initialize;
    WHEA_ERROR_SOURCE_UNINITIALIZE_DEVICE_DRIVER Uninitialize;
} WHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER_V1, *PWHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER_V1;

typedef struct _WHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER {
    ULONG Version;
    GUID SourceGuid;
    USHORT LogTag;
    UCHAR Reserved[6];
    WHEA_ERROR_SOURCE_INITIALIZE_DEVICE_DRIVER Initialize;
    WHEA_ERROR_SOURCE_UNINITIALIZE_DEVICE_DRIVER Uninitialize;
    ULONG MaxSectionDataLength;
    ULONG MaxSectionsPerReport;
    GUID CreatorId;
    GUID PartitionId;
} WHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER, *PWHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER;

typedef PVOID WHEA_ERROR_HANDLE;
#define WHEA_ERROR_HANDLE_INVALID NULL

#define WHEA_GENERIC_ENTRY_TEXT_LEN 20

#define WHEA_DEVICE_DRIVER_BUFFER_SET_V1 1

typedef struct _WHEA_DRIVER_BUFFER_SET {
    ULONG Version;
    PUCHAR Data;
    ULONG DataSize;
    LPGUID SectionTypeGuid;
    PUCHAR SectionFriendlyName;
    PUCHAR Flags;
} WHEA_DRIVER_BUFFER_SET, *PWHEA_DRIVER_BUFFER_SET;

#pragma pack(pop)

#pragma pack(push, 1)

typedef union _WHEA_ERROR_PACKET_FLAGS {
    struct {
        ULONG PreviousError : 1;
        ULONG Reserved1 : 1;
        ULONG HypervisorError : 1;
        ULONG Simulated : 1;
        ULONG PlatformPfaControl : 1;
        ULONG PlatformDirectedOffline : 1;
        ULONG Reserved2 : 26;
    } DUMMYSTRUCTNAME;
    ULONG AsULONG;
} WHEA_ERROR_PACKET_FLAGS, *PWHEA_ERROR_PACKET_FLAGS;

typedef enum _WHEA_ERROR_PACKET_DATA_FORMAT {
    WheaDataFormatIPFSalRecord = 0,
    WheaDataFormatXPFMCA,
    WheaDataFormatMemory,
    WheaDataFormatPCIExpress,
    WheaDataFormatNMIPort,
    WheaDataFormatPCIXBus,
    WheaDataFormatPCIXDevice,
    WheaDataFormatGeneric,
    WheaDataFormatMax
} WHEA_ERROR_PACKET_DATA_FORMAT, *PWHEA_ERROR_PACKET_DATA_FORMAT;

#if (NTDDI_VERSION >= NTDDI_WIN7)

#define WHEA_ERROR_PACKET_V2_SIGNATURE 'AEHW'
#define WHEA_ERROR_PACKET_V2_VERSION   3

typedef struct _WHEA_ERROR_PACKET_V2 {
    ULONG Signature;
    ULONG Version;
    ULONG Length;
    WHEA_ERROR_PACKET_FLAGS Flags;
    WHEA_ERROR_TYPE ErrorType;
    WHEA_ERROR_SEVERITY ErrorSeverity;
    ULONG ErrorSourceId;
    WHEA_ERROR_SOURCE_TYPE ErrorSourceType;
    GUID NotifyType;
    ULONGLONG Context;
    WHEA_ERROR_PACKET_DATA_FORMAT DataFormat;
    ULONG Reserved1;
    ULONG DataOffset;
    ULONG DataLength;
    ULONG PshedDataOffset;
    ULONG PshedDataLength;
} WHEA_ERROR_PACKET_V2, *PWHEA_ERROR_PACKET_V2;

#define WHEA_ERROR_PACKET_SIGNATURE WHEA_ERROR_PACKET_V2_SIGNATURE
#define WHEA_ERROR_PACKET_VERSION   WHEA_ERROR_PACKET_V2_VERSION
typedef WHEA_ERROR_PACKET_V2 WHEA_ERROR_PACKET;
typedef WHEA_ERROR_PACKET_V2 *PWHEA_ERROR_PACKET;

#endif /* (NTDDI_VERSION >= NTDDI_WIN7) */

#pragma pack(pop)

#pragma pack(push, 1)

typedef union _MCG_CAP {
    struct {
        ULONG64 CountField : 8;
        ULONG64 ControlMsrPresent : 1;
        ULONG64 ExtendedMsrsPresent : 1;
        ULONG64 SignalingExtensionPresent : 1;
        ULONG64 ThresholdErrorStatusPresent : 1;
        ULONG64 Reserved : 4;
        ULONG64 ExtendedRegisterCount : 8;
        ULONG64 SoftwareErrorRecoverySupported : 1;
        ULONG64 EnhancedMachineCheckCapability : 1;
        ULONG64 ExtendedErrorLogging : 1;
        ULONG64 LocalMachineCheckException : 1;
    } DUMMYSTRUCTNAME;
    ULONG64 QuadPart;
} MCG_CAP, *PMCG_CAP;

typedef union _MCG_STATUS {
    struct {
        ULONG RestartIpValid : 1;
        ULONG ErrorIpValid : 1;
        ULONG MachineCheckInProgress : 1;
        ULONG LocalMceValid : 1;
        ULONG Reserved1 : 28;
        ULONG Reserved2;
    } DUMMYSTRUCTNAME;
    ULONGLONG QuadPart;
} MCG_STATUS, *PMCG_STATUS;

typedef struct _MCI_STATUS_BITS_COMMON {
    ULONG64 McaErrorCode : 16;
    ULONG64 ModelErrorCode : 16;
    ULONG64 Reserved : 25;
    ULONG64 ContextCorrupt : 1;
    ULONG64 AddressValid : 1;
    ULONG64 MiscValid : 1;
    ULONG64 ErrorEnabled : 1;
    ULONG64 UncorrectedError : 1;
    ULONG64 StatusOverFlow : 1;
    ULONG64 Valid : 1;
} MCI_STATUS_BITS_COMMON, *PMCI_STATUS_BITS_COMMON;

typedef struct _MCI_STATUS_AMD_BITS {
    ULONG64 McaErrorCode : 16;
    ULONG64 ModelErrorCode : 16;
    ULONG64 ImplementationSpecific2 : 11;
    ULONG64 Poison : 1;
    ULONG64 Deferred : 1;
    ULONG64 ImplementationSpecific1 : 12;
    ULONG64 ContextCorrupt : 1;
    ULONG64 AddressValid : 1;
    ULONG64 MiscValid : 1;
    ULONG64 ErrorEnabled : 1;
    ULONG64 UncorrectedError : 1;
    ULONG64 StatusOverFlow : 1;
    ULONG64 Valid : 1;
} MCI_STATUS_AMD_BITS, *PMCI_STATUS_AMD_BITS;

typedef struct _MCI_STATUS_INTEL_BITS {
    ULONG64 McaErrorCode : 16;
    ULONG64 ModelErrorCode : 16;
    ULONG64 OtherInfo : 5;
    ULONG64 FirmwareUpdateError : 1;
    ULONG64 CorrectedErrorCount : 15;
    ULONG64 ThresholdErrorStatus : 2;
    ULONG64 ActionRequired : 1;
    ULONG64 Signalling : 1;
    ULONG64 ContextCorrupt : 1;
    ULONG64 AddressValid : 1;
    ULONG64 MiscValid : 1;
    ULONG64 ErrorEnabled : 1;
    ULONG64 UncorrectedError : 1;
    ULONG64 StatusOverFlow : 1;
    ULONG64 Valid : 1;
} MCI_STATUS_INTEL_BITS, *PMCI_STATUS_INTEL_BITS;

typedef union _MCI_STATUS {
    MCI_STATUS_BITS_COMMON CommonBits;
    MCI_STATUS_AMD_BITS AmdBits;
    MCI_STATUS_INTEL_BITS IntelBits;
    ULONG64 QuadPart;
} MCI_STATUS, *PMCI_STATUS;

typedef enum _WHEA_CPU_VENDOR {
    WheaCpuVendorOther = 0,
    WheaCpuVendorIntel,
    WheaCpuVendorAmd
} WHEA_CPU_VENDOR, *PWHEA_CPU_VENDOR;

#define WHEA_XPF_MCA_EXTREG_MAX_COUNT 24
#define WHEA_XPF_MCA_SECTION_VERSION_2 2
#define WHEA_XPF_MCA_SECTION_VERSION_3 3
#define WHEA_XPF_MCA_SECTION_VERSION_4 4
#define WHEA_XPF_MCA_SECTION_VERSION WHEA_XPF_MCA_SECTION_VERSION_4
#define WHEA_AMD_EXT_REG_NUM 10
#define WHEA_XPF_MCA_EXBANK_COUNT 32

typedef struct _WHEA_AMD_EXTENDED_REGISTERS {
    ULONGLONG IPID;
    ULONGLONG SYND;
    ULONGLONG CONFIG;
    ULONGLONG DESTAT;
    ULONGLONG DEADDR;
    ULONGLONG MISC1;
    ULONGLONG MISC2;
    ULONGLONG MISC3;
    ULONGLONG MISC4;
    ULONGLONG RasCap;
    ULONGLONG Reserved[WHEA_XPF_MCA_EXTREG_MAX_COUNT - WHEA_AMD_EXT_REG_NUM];
} WHEA_AMD_EXTENDED_REGISTERS, *PWHEA_AMD_EXTENDED_REGISTERS;

typedef struct _XPF_RECOVERY_INFO {
    struct {
        UINT32 NotSupported : 1;
        UINT32 Overflow : 1;
        UINT32 ContextCorrupt : 1;
        UINT32 RestartIpErrorIpNotValid : 1;
        UINT32 NoRecoveryContext : 1;
        UINT32 MiscOrAddrNotValid : 1;
        UINT32 InvalidAddressMode : 1;
        UINT32 HighIrql : 1;
        UINT32 InterruptsDisabled : 1;
        UINT32 SwapBusy : 1;
        UINT32 StackOverflow : 1;
        UINT32 Reserved : 21;
    } FailureReason;
    struct {
        UINT32 RecoveryAttempted : 1;
        UINT32 HvHandled : 1;
        UINT32 Reserved : 30;
    } Action;
    BOOLEAN ActionRequired;
    BOOLEAN RecoverySucceeded;
    BOOLEAN RecoveryKernel;
    UINT8 Reserved;
    UINT16 Reserved2;
    UINT16 Reserved3;
    UINT32 Reserved4;
} XPF_RECOVERY_INFO, *PXPF_RECOVERY_INFO;

typedef struct _WHEA_XPF_MCA_SECTION {
    ULONG VersionNumber;
    WHEA_CPU_VENDOR CpuVendor;
    LARGE_INTEGER Timestamp;
    ULONG ProcessorNumber;
    MCG_STATUS GlobalStatus;
    ULONGLONG InstructionPointer;
    ULONG BankNumber;
    MCI_STATUS Status;
    ULONGLONG Address;
    ULONGLONG Misc;
    ULONG ExtendedRegisterCount;
    ULONG ApicId;
    _ANONYMOUS_UNION union {
        ULONGLONG ExtendedRegisters[WHEA_XPF_MCA_EXTREG_MAX_COUNT];
        WHEA_AMD_EXTENDED_REGISTERS AMDExtendedRegisters;
    } DUMMYUNIONNAME;
    MCG_CAP GlobalCapability;
    XPF_RECOVERY_INFO RecoveryInfo;
    ULONG ExBankCount;
    ULONG BankNumberEx[WHEA_XPF_MCA_EXBANK_COUNT];
    MCI_STATUS StatusEx[WHEA_XPF_MCA_EXBANK_COUNT];
    ULONGLONG AddressEx[WHEA_XPF_MCA_EXBANK_COUNT];
    ULONGLONG MiscEx[WHEA_XPF_MCA_EXBANK_COUNT];
} WHEA_XPF_MCA_SECTION, *PWHEA_XPF_MCA_SECTION;

#pragma pack(pop)

$endif (_NTDDK_)
