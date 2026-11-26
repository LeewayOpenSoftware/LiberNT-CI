/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for RtlQueryEnvironmentVariable_U
 * COPYRIGHT:   Copyright 2026 Timo Kreuzer <timo.kreuzer@reactos.org>
 *              Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

/* Rows measured on Windows 11 26100: a value of UNICODE_STRING_MAX_CHARS or more gives STATUS_NO_MEMORY */
static
VOID
TestLongValue(ULONG Chars, NTSTATUS ExpectedTooSmall)
{
    UNICODE_STRING NameString, ValueString;
    PWSTR Environment, Value;
    NTSTATUS Status;
    ULONG Index;

    Environment = RtlAllocateHeap(RtlGetProcessHeap(), 0, (Chars + 4) * sizeof(WCHAR));
    Value = RtlAllocateHeap(RtlGetProcessHeap(), 0, MAXUSHORT + 1);
    if (!Environment || !Value)
    {
        skip("no memory\n");
        goto Cleanup;
    }
    Environment[0] = L'L';
    Environment[1] = L'=';
    for (Index = 0; Index < Chars; Index++)
        Environment[2 + Index] = L'y';
    Environment[2 + Chars] = Environment[3 + Chars] = UNICODE_NULL;
    RtlInitUnicodeString(&NameString, L"L");

    RtlInitEmptyUnicodeString(&ValueString, NULL, 0);
    ValueString.Length = 0x5555;
    Status = RtlQueryEnvironmentVariable_U(Environment, &NameString, &ValueString);
    ok(Status == ExpectedTooSmall, "%lu chars, empty buffer: 0x%lx\n", Chars, Status);
    ok(ValueString.Length == (Status == STATUS_NO_MEMORY ? 0x5555 : Chars * sizeof(WCHAR)),
       "%lu chars: Length %u\n", Chars, ValueString.Length);

    Value[0] = Value[1] = 0x2323;
    RtlInitEmptyUnicodeString(&ValueString, Value, sizeof(WCHAR));
    Status = RtlQueryEnvironmentVariable_U(Environment, &NameString, &ValueString);
    ok(Status == ExpectedTooSmall, "%lu chars, 1-char buffer: 0x%lx\n", Chars, Status);
    ok(Value[0] == UNICODE_NULL, "%lu chars: Value[0] 0x%x\n", Chars, Value[0]);
    ok(Value[1] == 0x2323, "%lu chars: Value[1] 0x%x\n", Chars, Value[1]);

    RtlInitEmptyUnicodeString(&ValueString, Value, MAXUSHORT - 1);
    ValueString.Length = 0x5555;
    Status = RtlQueryEnvironmentVariable_U(Environment, &NameString, &ValueString);
    ok(Status == (ExpectedTooSmall == STATUS_NO_MEMORY ? STATUS_NO_MEMORY : STATUS_SUCCESS),
       "%lu chars, full buffer: 0x%lx\n", Chars, Status);
    ok(ValueString.Length == (Status == STATUS_NO_MEMORY ? 0x5555 : Chars * sizeof(WCHAR)),
       "%lu chars: Length %u\n", Chars, ValueString.Length);

Cleanup:
    if (Environment) RtlFreeHeap(RtlGetProcessHeap(), 0, Environment);
    if (Value) RtlFreeHeap(RtlGetProcessHeap(), 0, Value);
}

START_TEST(RtlQueryEnvironmentVariable_U)
{
    static WCHAR TestEnvironment[] = L"TestVar=TestVal\0Foo=4\0EmptyVar\0Bar=8\0\0";
    WCHAR Buffer[32];
    UNICODE_STRING NameString, ValueString;
    NTSTATUS Status;
    PVOID SavedEnvironment;

    /* Query the TestVar environment variable length */
    RtlInitUnicodeString(&NameString, L"TestVar");
    RtlInitEmptyUnicodeString(&ValueString, NULL, 0);
    Status = RtlQueryEnvironmentVariable_U(TestEnvironment, &NameString, &ValueString);
    ok_ntstatus(Status, STATUS_BUFFER_TOO_SMALL);
    ok_eq_pointer(ValueString.Buffer, NULL);
    ok_eq_ulong(ValueString.Length, 7 * sizeof(WCHAR));
    ok_eq_ulong(ValueString.MaximumLength, 0);

    /* Test passing an empty sized buffer */
    memset(Buffer, 0xAB, sizeof(Buffer));
    RtlInitEmptyUnicodeString(&ValueString, Buffer, 0);
    Status = RtlQueryEnvironmentVariable_U(TestEnvironment, &NameString, &ValueString);
    ok_ntstatus(Status, STATUS_BUFFER_TOO_SMALL);
    ok_eq_pointer(ValueString.Buffer, Buffer);
    ok_eq_ulong(ValueString.Length, 7 * sizeof(WCHAR));
    ok_eq_ulong(ValueString.MaximumLength, 0);
    ok_eq_wchar(Buffer[0], 0xABAB);

    /* Test a buffer size that is too small */
    memset(Buffer, 0xAB, sizeof(Buffer));
    RtlInitEmptyUnicodeString(&ValueString, Buffer, 2 * sizeof(WCHAR));
    Status = RtlQueryEnvironmentVariable_U(TestEnvironment, &NameString, &ValueString);
    ok_ntstatus(Status, STATUS_BUFFER_TOO_SMALL);
    ok_eq_pointer(ValueString.Buffer, Buffer);
    ok_eq_ulong(ValueString.Length, 7 * sizeof(WCHAR));
    ok_eq_ulong(ValueString.MaximumLength, 2 * sizeof(WCHAR));
    ok(Buffer[0] == UNICODE_NULL || broken(/* Windows 2003 */ Buffer[0] == 0xABAB), "Buffer[0] = 0x%x\n", Buffer[0]);
    ok_eq_wchar(Buffer[1], 0xABAB);

    /* Test a buffer size that doesn't fit the terminating NULL char */
    memset(Buffer, 0xAB, sizeof(Buffer));
    RtlInitEmptyUnicodeString(&ValueString, Buffer, 7 * sizeof(WCHAR));
    Status = RtlQueryEnvironmentVariable_U(TestEnvironment, &NameString, &ValueString);
    ok(Status == STATUS_BUFFER_TOO_SMALL || broken(/* Windows 2003 */ Status == STATUS_SUCCESS), "Status = 0x%lx\n", Status);
    ok_eq_pointer(ValueString.Buffer, Buffer);
    ok_eq_ulong(ValueString.Length, 7 * sizeof(WCHAR));
    ok_eq_ulong(ValueString.MaximumLength, 7 * sizeof(WCHAR));
    if (Status == STATUS_BUFFER_TOO_SMALL)
    {
        ok_eq_wchar(Buffer[0], UNICODE_NULL);
        ok_eq_wchar(Buffer[1], 0xABAB);
    }

    /* Test a buffer size that is just large enough */
    memset(Buffer, 0xAB, sizeof(Buffer));
    RtlInitEmptyUnicodeString(&ValueString, Buffer, 8 * sizeof(WCHAR));
    Status = RtlQueryEnvironmentVariable_U(TestEnvironment, &NameString, &ValueString);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_eq_pointer(ValueString.Buffer, Buffer);
    ok_eq_ulong(ValueString.Length, 7 * sizeof(WCHAR));
    ok_eq_ulong(ValueString.MaximumLength, 8 * sizeof(WCHAR));
    ok_eq_wstr(Buffer, L"TestVal");
    ok_eq_wchar(Buffer[7], UNICODE_NULL);
    ok_eq_wchar(Buffer[8], 0xABAB);

    /* Test a buffer size that is larger than needed */
    memset(Buffer, 0xAB, sizeof(Buffer));
    RtlInitEmptyUnicodeString(&ValueString, Buffer, sizeof(Buffer));
    Status = RtlQueryEnvironmentVariable_U(TestEnvironment, &NameString, &ValueString);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_eq_pointer(ValueString.Buffer, Buffer);
    ok_eq_ulong(ValueString.Length, 7 * sizeof(WCHAR));
    ok_eq_ulong(ValueString.MaximumLength, sizeof(Buffer));
    ok_eq_wstr(Buffer, L"TestVal");
    ok_eq_wchar(Buffer[7], UNICODE_NULL);
    ok_eq_wchar(Buffer[8], 0xABAB);

    /* Test a too big variable Length */
    memset(Buffer, 0xAB, sizeof(Buffer));
    NameString.Length += sizeof(WCHAR);
    RtlInitEmptyUnicodeString(&ValueString, Buffer, sizeof(Buffer));
    Status = RtlQueryEnvironmentVariable_U(TestEnvironment, &NameString, &ValueString);
    ok_ntstatus(Status, STATUS_VARIABLE_NOT_FOUND);
    ok_eq_wchar(Buffer[0], 0xABAB);

    /* Query the EmptyVar environment variable */
    memset(Buffer, 0xAB, sizeof(Buffer));
    RtlInitUnicodeString(&NameString, L"EmptyVar");
    RtlInitEmptyUnicodeString(&ValueString, Buffer, sizeof(Buffer));
    Status = RtlQueryEnvironmentVariable_U(TestEnvironment, &NameString, &ValueString);
    ok_ntstatus(Status, STATUS_VARIABLE_NOT_FOUND);
    ok_eq_pointer(ValueString.Buffer, Buffer);
    ok_eq_ulong(ValueString.Length, 0);
    ok_eq_ulong(ValueString.MaximumLength, sizeof(Buffer));
    ok_eq_wchar(Buffer[0], 0xABAB);

    /* Query the Bar environment variable */
    memset(Buffer, 0xAB, sizeof(Buffer));
    RtlInitUnicodeString(&NameString, L"Bar");
    RtlInitEmptyUnicodeString(&ValueString, Buffer, sizeof(Buffer));
    Status = RtlQueryEnvironmentVariable_U(TestEnvironment, &NameString, &ValueString);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_eq_pointer(ValueString.Buffer, Buffer);
    ok_eq_ulong(ValueString.Length, 1 * sizeof(WCHAR));
    ok_eq_ulong(ValueString.MaximumLength, sizeof(Buffer));
    ok_eq_wstr(Buffer, L"8");
    ok_eq_wchar(Buffer[1], UNICODE_NULL);
    ok_eq_wchar(Buffer[2], 0xABAB);

    /* Test NULL Variable name */
    memset(Buffer, 0xAB, sizeof(Buffer));
    RtlInitEmptyUnicodeString(&NameString, NULL, 0);
    RtlInitEmptyUnicodeString(&ValueString, Buffer, sizeof(Buffer));
    Status = RtlQueryEnvironmentVariable_U(TestEnvironment, &NameString, &ValueString);
    ok_ntstatus(Status, STATUS_VARIABLE_NOT_FOUND);
    ok_eq_pointer(ValueString.Buffer, Buffer);
    ok_eq_ulong(ValueString.Length, 0);
    ok_eq_ulong(ValueString.MaximumLength, sizeof(Buffer));
    ok_eq_wchar(Buffer[0], 0xABAB);

    /* No environment block at all */
    RtlAcquirePebLock();
    SavedEnvironment = NtCurrentPeb()->ProcessParameters->Environment;
    NtCurrentPeb()->ProcessParameters->Environment = NULL;
    memset(Buffer, 0xAB, sizeof(Buffer));
    RtlInitUnicodeString(&NameString, L"Bar");
    RtlInitEmptyUnicodeString(&ValueString, Buffer, sizeof(Buffer));
    Status = RtlQueryEnvironmentVariable_U(NULL, &NameString, &ValueString);
    NtCurrentPeb()->ProcessParameters->Environment = SavedEnvironment;
    RtlReleasePebLock();
    ok_ntstatus(Status, STATUS_VARIABLE_NOT_FOUND);
    ok_eq_ulong(ValueString.Length, 0);
    ok_eq_wchar(Buffer[0], 0xABAB);

    TestLongValue(32766, STATUS_BUFFER_TOO_SMALL);
    TestLongValue(32767, STATUS_NO_MEMORY);
    TestLongValue(40000, STATUS_NO_MEMORY);
}
