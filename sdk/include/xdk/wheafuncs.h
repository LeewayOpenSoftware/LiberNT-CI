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

#endif /* (NTDDI_VERSION >= NTDDI_WIN7) */

$endif (_NTDDK_)
