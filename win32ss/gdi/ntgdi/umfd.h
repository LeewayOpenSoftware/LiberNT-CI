#pragma once

#include <reactos/umfd.h>

VOID UmfdInitialize(VOID);
ULONG UmfdGetMode(VOID);
VOID UmfdProcessCleanup(_In_ PEPROCESS Process);

INT
UmfdExtEscape(
    _In_opt_ HDC hDC,
    _In_opt_ PWCHAR pDriver,
    _In_ INT Escape,
    _In_ INT InSize,
    _In_opt_ LPSTR UnsafeIn,
    _In_ INT OutSize,
    _Out_opt_ LPSTR UnsafeOut);

/* Sends one request and waits for the host's reply; the caller must hold no USER or font lock */
NTSTATUS
UmfdCallHost(
    _In_ USHORT Op,
    _In_ ULONG FontId,
    _In_reads_bytes_opt_(cbIn) const VOID *pIn,
    _In_ ULONG cbIn,
    _Outptr_result_maybenull_ PUMFD_HEADER *ppReply);

VOID UmfdFreeReply(_In_ PUMFD_HEADER Reply);
