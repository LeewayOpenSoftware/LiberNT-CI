/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Fourth batch of ntoskrnl exports that replaced spec stubs
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

#define TAG_TEST 'S4tK'

static VOID
TestStrings(VOID)
{
    static const WCHAR Mixed[] = { L'A', 0x00E9, 0x20AC };
    static const WCHAR Lone[] = { 0xD800 };
    UNICODE_STRING Unicode;
    UTF8_STRING Utf8;
    STRING String;
    CHAR Small[3];
    PCHAR Long;

    ok_eq_hex(RtlInitStringEx(&String, NULL), STATUS_SUCCESS);
    ok_eq_uint(String.Length, 0);
    ok_eq_uint(String.MaximumLength, 0);
    ok_eq_hex(RtlInitStringEx(&String, "abc"), STATUS_SUCCESS);
    ok_eq_uint(String.Length, 3);
    ok_eq_uint(String.MaximumLength, 4);
    ok_eq_hex(RtlInitUTF8StringEx(&Utf8, "\xC3\xA9"), STATUS_SUCCESS);
    ok_eq_uint(Utf8.Length, 2);

    Long = ExAllocatePoolWithTag(NonPagedPool, MAXUSHORT + 1, TAG_TEST);
    if (!skip(Long != NULL, "no memory\n"))
    {
        RtlFillMemory(Long, MAXUSHORT, 'a');
        Long[MAXUSHORT] = ANSI_NULL;
        ok_eq_hex(RtlInitStringEx(&String, Long), STATUS_NAME_TOO_LONG);
        ok_eq_hex(RtlInitUTF8StringEx(&Utf8, Long), STATUS_NAME_TOO_LONG);
        ExFreePoolWithTag(Long, TAG_TEST);
    }

    Unicode.Buffer = (PWSTR)Mixed;
    Unicode.Length = Unicode.MaximumLength = sizeof(Mixed);
    ok_eq_hex(RtlUnicodeStringToUTF8String(&Utf8, &Unicode, TRUE), STATUS_SUCCESS);
    ok_eq_uint(Utf8.Length, 6);
    if (Utf8.Buffer != NULL)
        ok(RtlCompareMemory(Utf8.Buffer, "A\xC3\xA9\xE2\x82\xAC", 6) == 6, "wrong UTF-8 bytes\n");
    RtlFreeUTF8String(&Utf8);
    ok(Utf8.Buffer == NULL, "buffer not released\n");

    /* A buffer too small keeps only the whole characters that fit */
    Utf8.Buffer = Small;
    Utf8.Length = 0;
    Utf8.MaximumLength = sizeof(Small);
    ok_eq_hex(RtlUnicodeStringToUTF8String(&Utf8, &Unicode, FALSE), STATUS_BUFFER_OVERFLOW);
    ok_eq_uint(Utf8.Length, 3);

    Unicode.Buffer = (PWSTR)Lone;
    Unicode.Length = Unicode.MaximumLength = sizeof(Lone);
    ok_eq_hex(RtlUnicodeStringToUTF8String(&Utf8, &Unicode, TRUE), STATUS_SOME_NOT_MAPPED);
    ok_eq_uint(Utf8.Length, 3);
    if (Utf8.Buffer != NULL)
        ok(RtlCompareMemory(Utf8.Buffer, "\xEF\xBF\xBD", 3) == 3, "no replacement character\n");
    RtlFreeUTF8String(&Utf8);

    Unicode.Length = 1;
    ok_eq_hex(RtlUnicodeStringToUTF8String(&Utf8, &Unicode, TRUE), STATUS_INVALID_PARAMETER);
    Unicode.Length = 0;
    ok_eq_hex(RtlUnicodeStringToUTF8String(&Utf8, &Unicode, TRUE), STATUS_SUCCESS);
    ok_eq_uint(Utf8.Length, 0);
    RtlFreeUTF8String(&Utf8);
}

static BOOLEAN
CheckBytes(PUCHAR Buffer, ULONG Size, ULONG Start, ULONG Count, UCHAR Inside, UCHAR Outside)
{
    ULONG Index;

    for (Index = 0; Index < Size; Index++)
    {
        if (Buffer[Index] != ((Index >= Start && Index < Start + Count) ? Inside : Outside))
            return FALSE;
    }
    return TRUE;
}

static VOID
TestNonVolatileMemory(VOID)
{
    PVOID DefaultToken = (PVOID)FLUSH_NV_MEMORY_DEFAULT_TOKEN;
    NV_MEMORY_RANGE Ranges[2];
    UCHAR Source[100];
    PUCHAR Buffer;
    PVOID Token;
    NTSTATUS Status;

    Buffer = ExAllocatePoolWithTag(NonPagedPool, 256, TAG_TEST);
    if (skip(Buffer != NULL, "no memory\n"))
        return;

    Status = RtlGetNonVolatileToken(Buffer, 256, &Token);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Buffer, TAG_TEST);
        return;
    }

    RtlFillMemory(Source, sizeof(Source), 0x5A);
    RtlFillMemory(Buffer, 256, 0x11);
    ok_eq_hex(RtlWriteNonVolatileMemory(Token, Buffer + 5, Source, 90, 0), STATUS_SUCCESS);
    ok(CheckBytes(Buffer, 256, 5, 90, 0x5A, 0x11), "plain write\n");

    RtlFillMemory(Buffer, 256, 0x11);
    ok_eq_hex(RtlWriteNonVolatileMemory(Token, Buffer + 3, Source, 97, WRITE_NV_MEMORY_FLAG_PERSIST), STATUS_SUCCESS);
    ok(CheckBytes(Buffer, 256, 3, 97, 0x5A, 0x11), "persisted write\n");

    RtlFillMemory(Buffer, 256, 0x11);
    ok_eq_hex(RtlWriteNonVolatileMemory(Token, Buffer + 1, Source, 31,
                                        WRITE_NV_MEMORY_FLAG_NON_TEMPORAL | WRITE_NV_MEMORY_FLAG_NO_DRAIN),
              STATUS_SUCCESS);
    ok(CheckBytes(Buffer, 256, 1, 31, 0x5A, 0x11), "non-temporal write\n");

    RtlFillMemory(Buffer, 256, 0x11);
    ok_eq_hex(RtlFillNonVolatileMemory(Token, Buffer + 7, 77, 0xC4,
                                       FILL_NV_MEMORY_FLAG_FLUSH | FILL_NV_MEMORY_FLAG_NO_DRAIN),
              STATUS_SUCCESS);
    ok(CheckBytes(Buffer, 256, 7, 77, 0xC4, 0x11), "flushed fill\n");

    RtlFillMemory(Buffer, 256, 0x11);
    ok_eq_hex(RtlFillNonVolatileMemory(Token, Buffer + 2, 200, 0x3C, FILL_NV_MEMORY_FLAG_PERSIST), STATUS_SUCCESS);
    ok(CheckBytes(Buffer, 256, 2, 200, 0x3C, 0x11), "persisted fill\n");

    ok_eq_hex(RtlWriteNonVolatileMemory(Token, Buffer, Source, 4, 0x4), STATUS_INVALID_PARAMETER);
    ok_eq_hex(RtlFillNonVolatileMemory(Token, Buffer, 4, 0, 0x200), STATUS_INVALID_PARAMETER);
    ok_eq_hex(RtlWriteNonVolatileMemory(NULL, Buffer, Source, 4, 0), STATUS_INVALID_PARAMETER);
    ok_eq_hex(RtlWriteNonVolatileMemory(Token, Buffer, NULL, 4, 0), STATUS_INVALID_PARAMETER);
    ok_eq_hex(RtlWriteNonVolatileMemory(Token, Buffer, Source, 0, 0), STATUS_SUCCESS);

    Ranges[0].BaseAddress = Buffer;
    Ranges[0].Length = 64;
    Ranges[1].BaseAddress = Buffer + 130;
    Ranges[1].Length = 100;
    ok_eq_hex(RtlFlushNonVolatileMemoryRanges(Token, Ranges, 2, 0), STATUS_SUCCESS);
    ok_eq_hex(RtlFlushNonVolatileMemoryRanges(Token, Ranges, 2, FLUSH_NV_MEMORY_IN_FLAG_NO_DRAIN), STATUS_SUCCESS);
    ok_eq_hex(RtlFlushNonVolatileMemoryRanges(Token, NULL, 1, 0), STATUS_INVALID_PARAMETER);
    ok_eq_hex(RtlFlushNonVolatileMemoryRanges(Token, Ranges, 2, 2), STATUS_INVALID_PARAMETER);
    ok_eq_hex(RtlDrainNonVolatileFlush(Token), STATUS_SUCCESS);
    ok_eq_hex(RtlDrainNonVolatileFlush(NULL), STATUS_INVALID_PARAMETER);

    /* The default token stands in for callers that know their region */
    ok_eq_hex(RtlFlushNonVolatileMemory(DefaultToken, Buffer, 256, 0), STATUS_SUCCESS);
    ok_eq_hex(RtlDrainNonVolatileFlush(DefaultToken), STATUS_SUCCESS);
    ok_eq_hex(RtlFreeNonVolatileToken(DefaultToken), STATUS_INVALID_PARAMETER);

    ok_eq_hex(RtlFreeNonVolatileToken(Token), STATUS_SUCCESS);
    ExFreePoolWithTag(Buffer, TAG_TEST);
}

static NTSTATUS
CreateVolatileKey(PCWSTR Path, PHANDLE Handle)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateKey(Handle, KEY_ALL_ACCESS, &Attributes, 0, NULL, REG_OPTION_VOLATILE, NULL);
}

static VOID
TestRegistryFallback(VOID)
{
    static UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"KmtValue");
    static UNICODE_STRING Missing = RTL_CONSTANT_STRING(L"KmtMissing");
    HANDLE Primary, Fallback;
    ULONG Data, Type, Result;
    USHORT Short;
    NTSTATUS Status;

    Status = CreateVolatileKey(L"\\Registry\\Machine\\Software\\KmtNtos4Primary", &Primary);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = CreateVolatileKey(L"\\Registry\\Machine\\Software\\KmtNtos4Fallback", &Fallback);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        ZwDeleteKey(Primary);
        ZwClose(Primary);
        return;
    }

    Data = 0x1234;
    ok_eq_hex(ZwSetValueKey(Fallback, &ValueName, 0, REG_DWORD, &Data, sizeof(Data)), STATUS_SUCCESS);

    Data = 0;
    Type = REG_NONE;
    ok_eq_hex(RtlQueryRegistryValueWithFallback(Primary, Fallback, &ValueName, sizeof(Data), &Type, &Data, &Result),
              STATUS_SUCCESS);
    ok_eq_hex(Data, 0x1234UL);
    ok_eq_ulong(Type, (ULONG)REG_DWORD);
    ok_eq_ulong(Result, (ULONG)sizeof(Data));

    Data = 0x5678;
    ok_eq_hex(ZwSetValueKey(Primary, &ValueName, 0, REG_DWORD, &Data, sizeof(Data)), STATUS_SUCCESS);
    Data = 0;
    ok_eq_hex(RtlQueryRegistryValueWithFallback(Primary, Fallback, &ValueName, sizeof(Data), NULL, &Data, &Result),
              STATUS_SUCCESS);
    ok_eq_hex(Data, 0x5678UL);

    ok_eq_hex(RtlQueryRegistryValueWithFallback(Primary, Fallback, &ValueName, sizeof(Short), NULL, &Short, &Result),
              STATUS_BUFFER_OVERFLOW);
    ok_eq_ulong(Result, (ULONG)sizeof(Data));

    ok_eq_hex(RtlQueryRegistryValueWithFallback(NULL, Fallback, &ValueName, sizeof(Data), NULL, &Data, &Result),
              STATUS_SUCCESS);
    ok_eq_hex(Data, 0x1234UL);
    ok_eq_hex(RtlQueryRegistryValueWithFallback(Primary, Fallback, &Missing, sizeof(Data), NULL, &Data, &Result),
              STATUS_OBJECT_NAME_NOT_FOUND);
    ok_eq_hex(RtlQueryRegistryValueWithFallback(NULL, NULL, &ValueName, sizeof(Data), NULL, &Data, &Result),
              STATUS_INVALID_PARAMETER);

    ZwDeleteKey(Fallback);
    ZwClose(Fallback);
    ZwDeleteKey(Primary);
    ZwClose(Primary);
}

static LONG CleanupCount;

static VOID
NTAPI
SiloContextCleanup(
    _In_ PVOID SiloContext)
{
    UNREFERENCED_PARAMETER(SiloContext);
    InterlockedIncrement(&CleanupCount);
}

static VOID
NTAPI
ExitingThread(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    PsTerminateSystemThread(STATUS_NOT_FOUND);
}

static VOID
TestPs(VOID)
{
    PVOID First, Second, Old, Current;
    PETHREAD Thread;
    HANDLE Handle;
    ULONG Slot;
    NTSTATUS Status;

    ok_eq_hex(PsGetThreadExitStatus(PsGetCurrentThread()), STATUS_PENDING);

    Status = PsCreateSystemThread(&Handle, THREAD_ALL_ACCESS, NULL, NULL, NULL, ExitingThread, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = ObReferenceObjectByHandle(Handle, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&Thread, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Handle);
        if (NT_SUCCESS(Status))
        {
            KeWaitForSingleObject(Thread, Executive, KernelMode, FALSE, NULL);
            ok_eq_hex(PsGetThreadExitStatus(Thread), STATUS_NOT_FOUND);
            ObDereferenceObject(Thread);
        }
    }

    Status = PsAllocSiloContextSlot(0, &Slot);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    CleanupCount = 0;
    First = Second = NULL;
    ok_eq_hex(PsCreateSiloContext(NULL, sizeof(ULONG), NonPagedPoolNx, SiloContextCleanup, &First), STATUS_SUCCESS);
    ok_eq_hex(PsCreateSiloContext(NULL, sizeof(ULONG), NonPagedPoolNx, SiloContextCleanup, &Second), STATUS_SUCCESS);
    if (First != NULL && Second != NULL)
    {
        ok_eq_hex(PsReplaceSiloContext(NULL, Slot, NULL, NULL), STATUS_INVALID_PARAMETER);

        /* An empty slot just receives the new context */
        Old = (PVOID)1;
        ok_eq_hex(PsReplaceSiloContext(NULL, Slot, First, &Old), STATUS_SUCCESS);
        ok(Old == NULL, "old context %p for an empty slot\n", Old);

        ok_eq_hex(PsReplaceSiloContext(NULL, Slot, Second, &Old), STATUS_SUCCESS);
        ok(Old == First, "old context %p, expected %p\n", Old, First);
        if (Old != NULL)
            PsDereferenceSiloContext(Old);

        Current = NULL;
        ok_eq_hex(PsGetSiloContext(NULL, Slot, &Current), STATUS_SUCCESS);
        ok(Current == Second, "slot holds %p, expected %p\n", Current, Second);
        if (Current != NULL)
            PsDereferenceSiloContext(Current);

        /* Without an output pointer the table's reference on the old context is dropped */
        ok_eq_hex(PsReplaceSiloContext(NULL, Slot, First, NULL), STATUS_SUCCESS);
        ok_eq_hex(PsRemoveSiloContext(NULL, Slot, NULL), STATUS_SUCCESS);
    }
    if (First != NULL)
        PsDereferenceSiloContext(First);
    if (Second != NULL)
        PsDereferenceSiloContext(Second);
    ok_eq_long(CleanupCount, 2L);

    ok_eq_hex(PsReplaceSiloContext(NULL, MAXULONG, (PVOID)1, NULL), STATUS_INVALID_PARAMETER);
    PsFreeSiloContextSlot(Slot);
}

static VOID
TestOb(VOID)
{
    PETHREAD Thread = PsGetCurrentThread();
    HANDLE Handle;
    NTSTATUS Status;

    ok_eq_hex(ObReferenceObjectByPointerWithTag(Thread, 0, *PsThreadType, KernelMode, TAG_TEST), STATUS_SUCCESS);
    ObDereferenceObjectDeferDeleteWithTag(Thread, TAG_TEST);
    ok_eq_hex(ObReferenceObjectByPointerWithTag(Thread, 0, *PsProcessType, UserMode, TAG_TEST),
              STATUS_OBJECT_TYPE_MISMATCH);

    Status = ObOpenObjectByPointerWithTag(PsGetCurrentProcess(), OBJ_KERNEL_HANDLE, NULL,
                                          PROCESS_QUERY_INFORMATION, *PsProcessType, KernelMode,
                                          TAG_TEST, &Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);
}

static VOID
TestStatusAndProtection(VOID)
{
    static ULONG Data;
    PVOID Pool;

    ok(IoIsValidIrpStatus(STATUS_SUCCESS), "STATUS_SUCCESS\n");
    ok(IoIsValidIrpStatus(STATUS_PENDING), "STATUS_PENDING\n");
    ok(IoIsValidIrpStatus(STATUS_BUFFER_OVERFLOW), "STATUS_BUFFER_OVERFLOW\n");
    ok(IoIsValidIrpStatus(STATUS_ACCESS_DENIED), "STATUS_ACCESS_DENIED\n");
    ok(IoIsValidIrpStatus((NTSTATUS)0xC0150001), "a status of facility 0x15\n");
    ok(IoIsValidIrpStatus((NTSTATUS)0xE0FF1234), "customer bit\n");
    ok(!IoIsValidIrpStatus((NTSTATUS)0xD0000001), "reserved bit\n");
    ok(!IoIsValidIrpStatus((NTSTATUS)0xCCCCCCCC), "fill pattern\n");
    ok(!IoIsValidIrpStatus((NTSTATUS)0xC0FF0001), "undefined facility\n");

    ok_eq_hex(MmProtectDriverSection(&Data, 1, 0), STATUS_INVALID_PARAMETER);
    ok_eq_hex(MmProtectDriverSection(&Data, 0, 2), STATUS_INVALID_PARAMETER);
    ok_eq_hex(MmProtectDriverSection(&Data, 0, MM_PROTECT_DRIVER_SECTION_ALLOW_UNLOAD), STATUS_INVALID_DEVICE_STATE);
    ok_eq_hex(MmProtectDriverSection((PVOID)(ULONG_PTR)TestOb, 0, 0), STATUS_INVALID_PAGE_PROTECTION);
    Pool = ExAllocatePoolWithTag(NonPagedPool, 16, TAG_TEST);
    if (Pool != NULL)
    {
        ok_eq_hex(MmProtectDriverSection(Pool, 0, 0), STATUS_INVALID_PARAMETER);
        ExFreePoolWithTag(Pool, TAG_TEST);
    }
}

static VOID
TestDeviceRoutines(VOID)
{
    static const UNICODE_STRING NullName = RTL_CONSTANT_STRING(L"\\Device\\Null");
    FILE_STANDARD_INFORMATION Standard;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT DeviceObject;
    ULONG_PTR Information = 5;
    ULONG Returned = 5;
    NTSTATUS Status;

    ok_eq_hex(FsRtlIssueDeviceIoControl(NULL, 0, 0, NULL, 0, NULL, 0, &Information), STATUS_INVALID_PARAMETER);
    ok_eq_hex(FsRtlQueryInformationFile(NULL, &Standard, sizeof(Standard), FileStandardInformation, &Returned),
              STATUS_INVALID_PARAMETER);
    ok(!FsRtlIsDaxVolume(NULL), "no file object, no DAX volume\n");

    Status = IoGetDeviceObjectPointer((PUNICODE_STRING)&NullName, FILE_READ_DATA, &FileObject, &DeviceObject);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    Information = 5;
    Status = FsRtlIssueDeviceIoControl(DeviceObject, CTL_CODE(FILE_DEVICE_NULL, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS),
                                       SL_OVERRIDE_VERIFY_VOLUME, NULL, 0, NULL, 0, &Information);
    ok(Status != STATUS_PENDING, "IOCTL returned pending\n");
    ok_eq_ulongptr(Information, 0);

    Returned = 5;
    Status = FsRtlQueryInformationFile(FileObject, &Standard, sizeof(Standard), FileStandardInformation, &Returned);
    ok(Status != STATUS_PENDING, "query returned pending\n");
    if (NT_ERROR(Status))
        ok_eq_ulong(Returned, 0UL);
    else
        ok(Returned <= sizeof(Standard), "returned %lu bytes\n", Returned);

    ok(!FsRtlIsDaxVolume(FileObject), "the null device is not a DAX volume\n");

    ObDereferenceObject(FileObject);
}

START_TEST(NtosStubExports4)
{
    TestStrings();
    TestNonVolatileMemory();
    TestRegistryFallback();
    TestPs();
    TestOb();
    TestStatusAndProtection();
    TestDeviceRoutines();
}
