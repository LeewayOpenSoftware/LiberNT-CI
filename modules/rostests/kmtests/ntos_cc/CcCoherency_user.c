/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite Cc purge coherency test, user-mode part
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>

START_TEST(CcCoherency)
{
    HANDLE Handle;
    NTSTATUS Status;
    LARGE_INTEGER ByteOffset;
    IO_STATUS_BLOCK IoStatusBlock;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING File = RTL_CONSTANT_STRING(L"\\Device\\Kmtest-CcCoherency\\CoherencyTest");
    UCHAR Byte = 0;
    DWORD Error;

    Error = KmtLoadAndOpenDriver(L"CcCoherency", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    InitializeObjectAttributes(&ObjectAttributes, &File, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Handle, FILE_ALL_ACCESS, &ObjectAttributes, &IoStatusBlock, 0,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        /* One cached write runs the driver-side tests */
        ByteOffset.QuadPart = 0;
        Status = NtWriteFile(Handle, NULL, NULL, NULL, &IoStatusBlock, &Byte, sizeof(Byte), &ByteOffset, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        NtClose(Handle);
    }

    KmtCloseDriver();
    KmtUnloadDriver();
}
