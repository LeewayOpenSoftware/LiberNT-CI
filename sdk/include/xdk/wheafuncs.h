/******************************************************************************
 *                    Hardware Error Architecture Functions                   *
 ******************************************************************************/
$if (_NTDDK_)

#if (NTDDI_VERSION >= NTDDI_WIN7)

NTKERNELAPI
NTSTATUS
NTAPI
WheaAddErrorSource(
  _Inout_ PWHEA_ERROR_SOURCE_DESCRIPTOR ErrorSource,
  _In_opt_ PVOID Context);

NTKERNELAPI
VOID
NTAPI
WheaRemoveErrorSource(
  _In_ ULONG ErrorSourceId);

NTKERNELAPI
NTSTATUS
NTAPI
WheaReportHwError(
  _In_ PWHEA_ERROR_PACKET ErrorPacket);

#endif /* (NTDDI_VERSION >= NTDDI_WIN7) */

$endif (_NTDDK_)
