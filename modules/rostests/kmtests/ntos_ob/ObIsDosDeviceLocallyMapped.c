/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for ObIsDosDeviceLocallyMapped
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>

NTSTATUS
NTAPI
ObIsDosDeviceLocallyMapped(
    _In_ ULONG Index,
    _Out_ PUCHAR DosDeviceState);

static
BOOLEAN
GlobalDriveLinkExists(
    _In_ ULONG Index)
{
    WCHAR NameBuffer[] = L"\\GLOBAL??\\A:";
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle;
    NTSTATUS Status;

    NameBuffer[10] = (WCHAR)(L'A' + Index - 1);
    RtlInitUnicodeString(&Name, NameBuffer);
    InitializeObjectAttributes(&ObjectAttributes,
                               &Name,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);
    Status = ZwOpenSymbolicLinkObject(&Handle, SYMBOLIC_LINK_QUERY, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return FALSE;

    ZwClose(Handle);
    return TRUE;
}

START_TEST(ObIsDosDeviceLocallyMapped)
{
    NTSTATUS Status;
    UCHAR State;
    ULONG Index;
    ULONG Mapped = 0;

    ok_irql(PASSIVE_LEVEL);

    /* Only the letters A to Z are valid, numbered from 1 */
    State = 0x55;
    Status = ObIsDosDeviceLocallyMapped(0, &State);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok_eq_uint(State, 0x55);

    State = 0x55;
    Status = ObIsDosDeviceLocallyMapped(27, &State);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok_eq_uint(State, 0x55);

    for (Index = 1; Index <= 26; Index++)
    {
        State = 0x55;
        Status = ObIsDosDeviceLocallyMapped(Index, &State);
        ok(Status == STATUS_SUCCESS, "Index %lu: status 0x%08lx\n", Index, Status);
        ok(State == 0 || State == 1, "Index %lu: state %u\n", Index, State);

        if (NT_SUCCESS(Status) && State != 0)
        {
            Mapped++;

            /* A letter the system device map knows has its link in the global directory */
            ok(GlobalDriveLinkExists(Index), "Index %lu (%c:) is mapped but has no global link\n",
               Index, 'A' + (int)Index - 1);
        }
    }

    /* The system was started from a drive that has a letter */
    ok(Mapped != 0, "No drive letter is mapped\n");
}
