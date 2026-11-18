/******************************************************************************
 *                    Hardware Error Architecture Functions                   *
 ******************************************************************************/
$if (_NTDDK_)

#if (NTDDI_VERSION >= NTDDI_WIN7)

_IRQL_requires_max_(DISPATCH_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
WheaAddErrorSource(
  _Inout_ PWHEA_ERROR_SOURCE_DESCRIPTOR ErrorSource,
  _In_opt_ PVOID Context);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTKERNELAPI
VOID
NTAPI
WheaRemoveErrorSource(
  _In_ ULONG ErrorSourceId);

/* The packet must be in memory that cannot be paged out: it is read with interrupts off */
_IRQL_requires_max_(HIGH_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
WheaReportHwError(
  _In_ PWHEA_ERROR_PACKET ErrorPacket);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
WheaInitializeRecordHeader(
  _Out_ PWHEA_ERROR_RECORD_HEADER Header);

_IRQL_requires_max_(PASSIVE_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
WheaAddErrorSourceDeviceDriver(
  _Inout_opt_ PVOID Context,
  _In_ PWHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER Configuration,
  _In_ ULONG NumberPreallocatedErrorReports);

_IRQL_requires_max_(PASSIVE_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
WheaAddErrorSourceDeviceDriverV1(
  _Inout_opt_ PVOID Context,
  _In_ PWHEA_ERROR_SOURCE_CONFIGURATION_DEVICE_DRIVER Configuration,
  _In_ ULONG NumBuffersToPreallocate,
  _In_ ULONG MaxDataLength);

_IRQL_requires_max_(PASSIVE_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
WheaRemoveErrorSourceDeviceDriver(
  _In_ ULONG ErrorSourceId);

/* Above PASSIVE_LEVEL the source needs a preallocated report, and the data and the name must not be pageable */
_IRQL_requires_max_(HIGH_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
WheaReportHwErrorDeviceDriver(
  _In_ ULONG ErrorSourceId,
  _In_opt_ PDEVICE_OBJECT DeviceObject,
  _In_reads_bytes_(ErrorDataLength) PUCHAR ErrorData,
  _In_ ULONG ErrorDataLength,
  _In_ LPGUID SectionTypeGuid,
  _In_ WHEA_ERROR_SEVERITY ErrorSeverity,
  _In_opt_ LPSTR DeviceFriendlyName);

_IRQL_requires_max_(HIGH_LEVEL)
NTKERNELAPI
WHEA_ERROR_SOURCE_STATE
NTAPI
WheaErrorSourceGetState(
  _In_ ULONG ErrorSourceId);

/* Above PASSIVE_LEVEL the source needs a preallocated report */
_IRQL_requires_max_(HIGH_LEVEL)
NTKERNELAPI
WHEA_ERROR_HANDLE
NTAPI
WheaCreateHwErrorReportDeviceDriver(
  _In_ ULONG ErrorSourceId,
  _In_opt_ PDEVICE_OBJECT DeviceObject);

_IRQL_requires_max_(HIGH_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
WheaAddHwErrorReportSectionDeviceDriver(
  _In_ WHEA_ERROR_HANDLE ErrorHandle,
  _In_ ULONG SectionDataLength,
  _Out_ PWHEA_DRIVER_BUFFER_SET BufferSet);

_IRQL_requires_max_(HIGH_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
WheaHwErrorReportAbandonDeviceDriver(
  _In_ WHEA_ERROR_HANDLE ErrorHandle);

_IRQL_requires_max_(HIGH_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
WheaHwErrorReportSubmitDeviceDriver(
  _In_ WHEA_ERROR_HANDLE ErrorHandle);

_IRQL_requires_max_(HIGH_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
WheaHwErrorReportSetSeverityDeviceDriver(
  _In_ WHEA_ERROR_HANDLE ErrorHandle,
  _In_ WHEA_ERROR_SEVERITY ErrorSeverity);

_IRQL_requires_max_(HIGH_LEVEL)
NTKERNELAPI
NTSTATUS
NTAPI
WheaHwErrorReportSetSectionNameDeviceDriver(
  _In_ PWHEA_DRIVER_BUFFER_SET BufferSet,
  _In_range_(0, WHEA_GENERIC_ENTRY_TEXT_LEN) ULONG NameLength,
  _In_reads_bytes_(NameLength) PUCHAR Name);

typedef
NTSTATUS
(NTAPI _WHEA_ERROR_SOURCE_CORRECT)(
  _Inout_ PWHEA_ERROR_SOURCE_DESCRIPTOR ErrorSource,
  _Out_ PULONG MaximumSectionLength);
typedef _WHEA_ERROR_SOURCE_CORRECT *WHEA_ERROR_SOURCE_CORRECT;

typedef
NTSTATUS
(NTAPI _WHEA_ERROR_SOURCE_INITIALIZE)(
  _In_ ULONG Phase,
  _Inout_ PWHEA_ERROR_SOURCE_DESCRIPTOR ErrorSource,
  _Inout_opt_ PVOID Context);
typedef _WHEA_ERROR_SOURCE_INITIALIZE *WHEA_ERROR_SOURCE_INITIALIZE;

typedef
NTSTATUS
(NTAPI _WHEA_ERROR_SOURCE_CREATE_RECORD)(
  _Inout_ PWHEA_ERROR_SOURCE_DESCRIPTOR ErrorSource,
  _Inout_ PWHEA_ERROR_PACKET ErrorPacket,
  _Out_writes_bytes_(BufferSize) PWHEA_ERROR_RECORD ErrorRecord,
  _In_ ULONG BufferSize,
  _Inout_opt_ PVOID Context);
typedef _WHEA_ERROR_SOURCE_CREATE_RECORD *WHEA_ERROR_SOURCE_CREATE_RECORD;

typedef
NTSTATUS
(NTAPI _WHEA_ERROR_SOURCE_RECOVER)(
  _Inout_ PVOID RecoveryContext,
  _Inout_ PWHEA_ERROR_SEVERITY Severity);
typedef _WHEA_ERROR_SOURCE_RECOVER *WHEA_ERROR_SOURCE_RECOVER;

typedef
VOID
(NTAPI _WHEA_ERROR_SOURCE_UNINITIALIZE)(
  _Inout_opt_ PVOID Context);
typedef _WHEA_ERROR_SOURCE_UNINITIALIZE *WHEA_ERROR_SOURCE_UNINITIALIZE;

typedef struct _WHEA_ERROR_SOURCE_CONFIGURATION {
  ULONG Flags;
  WHEA_ERROR_SOURCE_CORRECT Correct;
  WHEA_ERROR_SOURCE_INITIALIZE Initialize;
  WHEA_ERROR_SOURCE_CREATE_RECORD CreateRecord;
  WHEA_ERROR_SOURCE_RECOVER Recover;
  WHEA_ERROR_SOURCE_UNINITIALIZE Uninitialize;
  PVOID Reserved;
} WHEA_ERROR_SOURCE_CONFIGURATION, *PWHEA_ERROR_SOURCE_CONFIGURATION;

NTKERNELAPI
NTSTATUS
NTAPI
WheaConfigureErrorSource(
  _In_ WHEA_ERROR_SOURCE_TYPE SourceType,
  _In_ PWHEA_ERROR_SOURCE_CONFIGURATION Configuration);

NTKERNELAPI
NTSTATUS
NTAPI
WheaUnconfigureErrorSource(
  _In_ WHEA_ERROR_SOURCE_TYPE SourceType);

#endif /* (NTDDI_VERSION >= NTDDI_WIN7) */

$endif (_NTDDK_)
