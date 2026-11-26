/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS NT User-Mode DLL
 * FILE:            lib/ntdll/rtl/uilist.c
 * PURPOSE:         RTL UI to API network computers list conversion.
 *                  Helper for NETAPI32.DLL
 * PROGRAMMERS:     Hermes Belusca-Maito
 */

/* INCLUDES *****************************************************************/

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ***************************************************************/

/*
 * @unimplemented
 *
 * Legacy-only export: Windows 11 no longer exports it, so there is no modern
 * reference to verify a conversion against, and no in-tree caller uses it.
 * It stays exported so old binaries still load, but reports the missing
 * functionality instead of a fake success. The output is still set to an
 * empty string for callers that read it regardless of the status.
 */
NTSTATUS
NTAPI
RtlConvertUiListToApiList(
    IN PUNICODE_STRING UiList,
    OUT PUNICODE_STRING ApiList,
    IN BOOLEAN SpaceAsSeparator)
{
    UNREFERENCED_PARAMETER(UiList);
    UNREFERENCED_PARAMETER(SpaceAsSeparator);

    RtlInitEmptyUnicodeString(ApiList, NULL, 0);
    return STATUS_NOT_IMPLEMENTED;
}

/* EOF */
