/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtQuerySystemInformation
 * COPYRIGHT:   Copyright 2019 Thomas Faber (thomas.faber@reactos.org)
 */

#include "precomp.h"

static
VOID
TestProcessImageName(VOID)
{
    PUNICODE_STRING ImagePath = &NtCurrentPeb()->ProcessParameters->ImagePathName;
    PSYSTEM_PROCESS_INFORMATION Info, Buffer;
    UNICODE_STRING BaseName;
    ULONG Length = 0x40000;
    USHORT Index;
    NTSTATUS Status;

    Index = ImagePath->Length / sizeof(WCHAR);
    while (Index != 0 && ImagePath->Buffer[Index - 1] != L'\\')
        Index--;
    BaseName.Buffer = &ImagePath->Buffer[Index];
    BaseName.Length = BaseName.MaximumLength = ImagePath->Length - Index * sizeof(WCHAR);

    Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length);
    if (!Buffer)
    {
        skip("Could not allocate process information\n");
        return;
    }
    Status = NtQuerySystemInformation(SystemProcessInformation, Buffer, Length, &Length);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Info = Buffer;
        while (Info->UniqueProcessId != NtCurrentTeb()->ClientId.UniqueProcess && Info->NextEntryOffset != 0)
            Info = (PSYSTEM_PROCESS_INFORMATION)((PUCHAR)Info + Info->NextEntryOffset);
        ok(Info->UniqueProcessId == NtCurrentTeb()->ClientId.UniqueProcess, "Current process not listed\n");
        ok(RtlEqualUnicodeString(&Info->ImageName, &BaseName, TRUE),
           "Image name %.*ls, expected %.*ls\n",
           Info->ImageName.Length / (int)sizeof(WCHAR), Info->ImageName.Buffer,
           BaseName.Length / (int)sizeof(WCHAR), BaseName.Buffer);
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
}

static
VOID
TestHandleCount(VOID)
{
    SYSTEM_HANDLE_INFORMATION Handles;
    PSYSTEM_HANDLE_INFORMATION Buffer;
    ULONG Length = 0;
    ULONG Count;
    NTSTATUS Status;

    RtlFillMemory(&Handles, sizeof(Handles), 0xCC);
    Status = NtQuerySystemInformation(SystemHandleInformation, &Handles, sizeof(Handles), NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(Handles.NumberOfHandles != 0xCCCCCCCC, "Handle count not written on a short buffer\n");
    Count = Handles.NumberOfHandles;

    RtlFillMemory(&Handles, sizeof(Handles), 0xCC);
    Status = NtQuerySystemInformation(SystemHandleInformation, &Handles, sizeof(Handles), &Length);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(Handles.NumberOfHandles != 0xCCCCCCCC, "Handle count not written on a short buffer\n");
    ok(Length > sizeof(Handles), "Handle information length %lu\n", Length);
    trace("Handle count %lu, length %lu\n", Handles.NumberOfHandles, Length);

    Length += 0x10000;
    Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length);
    if (!Buffer)
    {
        skip("Could not allocate handle information\n");
        return;
    }
    Status = NtQuerySystemInformation(SystemHandleInformation, Buffer, Length, &Length);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(Buffer->NumberOfHandles > 1, "Handle count %lu\n", Buffer->NumberOfHandles);
        ok(Length == FIELD_OFFSET(SYSTEM_HANDLE_INFORMATION, Handles) + Buffer->NumberOfHandles * sizeof(Buffer->Handles[0]),
           "Length %lu for %lu handles\n", Length, Buffer->NumberOfHandles);
        ok(Count <= Buffer->NumberOfHandles + 64 && Count + 64 >= Buffer->NumberOfHandles,
           "Short-buffer count %lu, full count %lu\n", Count, Buffer->NumberOfHandles);
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
}

/* The zero-size probe is how callers size the pool tag buffer */
static
VOID
TestPoolTagCount(VOID)
{
    SYSTEM_POOLTAG_INFORMATION Header;
    PSYSTEM_POOLTAG_INFORMATION Buffer;
    ULONG Length = 0;
    ULONG Count;
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemPoolTagInformation, NULL, 0, &Length);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(Length >= FIELD_OFFSET(SYSTEM_POOLTAG_INFORMATION, TagInfo) + sizeof(SYSTEM_POOLTAG),
       "Pool tag information length %lu on the zero-size probe\n", Length);
    trace("Pool tag information length %lu\n", Length);

    RtlFillMemory(&Header, sizeof(Header), 0xCC);
    Status = NtQuerySystemInformation(SystemPoolTagInformation, &Header, FIELD_OFFSET(SYSTEM_POOLTAG_INFORMATION, TagInfo), &Length);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(Header.Count != 0xCCCCCCCC, "Tag count not written on a header-only buffer\n");
    ok(Header.Count == 0, "Tag count %lu on a header-only buffer\n", Header.Count);
    ok(Length >= FIELD_OFFSET(SYSTEM_POOLTAG_INFORMATION, TagInfo) + sizeof(SYSTEM_POOLTAG),
       "Pool tag information length %lu on a header-only buffer\n", Length);

    Length += 64 * sizeof(SYSTEM_POOLTAG);
    Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length);
    if (!Buffer)
    {
        skip("Could not allocate pool tag information\n");
        return;
    }
    Status = NtQuerySystemInformation(SystemPoolTagInformation, Buffer, Length, &Length);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Count = Buffer->Count;
        ok(Count != 0, "No pool tags reported\n");
        ok(Length == FIELD_OFFSET(SYSTEM_POOLTAG_INFORMATION, TagInfo) + Count * sizeof(SYSTEM_POOLTAG),
           "Length %lu for %lu tags\n", Length, Count);
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
}

START_TEST(NtQuerySystemInformation)
{
    NTSTATUS Status;
    ULONG Length = 0;
    PRTL_PROCESS_MODULES Modules;
    SYSTEM_BASIC_INFORMATION Basic;

    Status = NtQuerySystemInformation(0, NULL, 0, NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    Status = NtQuerySystemInformation(0x80000000, NULL, 0, NULL);
    ok_hex(Status, STATUS_INVALID_INFO_CLASS);

    RtlZeroMemory(&Basic, sizeof(Basic));
    Status = NtQuerySystemInformation(SystemBasicInformation, &Basic, sizeof(Basic), &Length);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(Basic.NumberOfPhysicalPages != 0, "No physical memory reported\n");
        ok(SharedUserData->NumberOfPhysicalPages == Basic.NumberOfPhysicalPages,
           "Shared physical pages %lu, system information %lu\n",
           SharedUserData->NumberOfPhysicalPages, Basic.NumberOfPhysicalPages);
        trace("Physical pages: shared %lu, queried %lu\n",
              SharedUserData->NumberOfPhysicalPages, Basic.NumberOfPhysicalPages);
    }

    TestProcessImageName();
    TestHandleCount();
    TestPoolTagCount();

    /* Both loaded-image lists must be initialized even when no user images
     * have been registered with the kernel's module list. */
    Status = NtQuerySystemInformation(SystemModuleInformation, NULL, 0, &Length);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(Length >= sizeof(*Modules), "Module information length %lu\n", Length);
    Modules = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length + 4096);
    if (!Modules)
    {
        skip("Could not allocate module information\n");
        return;
    }
    Status = NtQuerySystemInformation(SystemModuleInformation, Modules, Length + 4096, &Length);
    ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ok(Modules->NumberOfModules != 0, "No kernel modules reported\n");
    RtlFreeHeap(RtlGetProcessHeap(), 0, Modules);
}
