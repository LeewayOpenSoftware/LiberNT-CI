/*
 * PROJECT:         LiberNT Kernel
 * LICENSE:         GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:         Persisted state location resolution
 * COPYRIGHT:       Copyright 2026 LeewayOpenSoftware
 */

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI
RtlGetPersistedStateLocation(
    _In_ PCWSTR SourceID,
    _In_opt_ PCWSTR CustomValue,
    _In_opt_ PCWSTR DefaultPath,
    _In_ STATE_LOCATION_TYPE StateLocationType,
    _Out_ PWCHAR TargetPath,
    _In_ ULONG BufferLengthIn,
    _Out_opt_ PULONG BufferLengthOut)
{
    PCWSTR Result;
    ULONG Required;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(CustomValue);
    UNREFERENCED_PARAMETER(StateLocationType);

    if (SourceID == NULL || TargetPath == NULL)
        return STATUS_INVALID_PARAMETER;

    Result = DefaultPath != NULL ? DefaultPath : L"";
    Required = (ULONG)((wcslen(Result) + 1) * sizeof(WCHAR));

    if (BufferLengthOut != NULL)
        *BufferLengthOut = Required;

    if (BufferLengthIn < Required)
        return STATUS_BUFFER_OVERFLOW;

    RtlCopyMemory(TargetPath, Result, Required);
    return STATUS_SUCCESS;
}
