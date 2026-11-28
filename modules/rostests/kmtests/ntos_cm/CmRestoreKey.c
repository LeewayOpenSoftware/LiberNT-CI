/*
 * PROJECT:         LiberNT kernel-mode tests
 * LICENSE:         GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:         Kernel-Mode Test Suite ZwRestoreKey with ZwSaveKey round trips
 * COPYRIGHT:       Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define SRC_KEY  L"\\Registry\\Machine\\SOFTWARE\\KmtRestoreSrc"
#define DST_KEY  L"\\Registry\\Machine\\SOFTWARE\\KmtRestoreDst"
#define VDST_KEY L"\\Registry\\Machine\\SOFTWARE\\KmtRestoreVolatile"

static UNICODE_STRING HiveFile = RTL_CONSTANT_STRING(L"\\SystemRoot\\KmtRestore.hiv");
static UNICODE_STRING BadFile = RTL_CONSTANT_STRING(L"\\SystemRoot\\KmtRestoreBad.hiv");

static
NTSTATUS
KeyCreate(
    _Out_ PHANDLE KeyHandle,
    _In_opt_ HANDLE Root,
    _In_ PCWSTR Name,
    _In_ ULONG Options)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING KeyName;

    RtlInitUnicodeString(&KeyName, Name);
    InitializeObjectAttributes(&ObjectAttributes, &KeyName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, Root, NULL);
    return ZwCreateKey(KeyHandle, KEY_ALL_ACCESS, &ObjectAttributes, 0, NULL, Options, NULL);
}

static
NTSTATUS
KeyOpen(
    _Out_ PHANDLE KeyHandle,
    _In_opt_ HANDLE Root,
    _In_ PCWSTR Name)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING KeyName;

    RtlInitUnicodeString(&KeyName, Name);
    InitializeObjectAttributes(&ObjectAttributes, &KeyName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, Root, NULL);
    return ZwOpenKey(KeyHandle, KEY_ALL_ACCESS, &ObjectAttributes);
}

static
NTSTATUS
SetValue(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _In_ ULONG Type,
    _In_ PVOID Data,
    _In_ ULONG Size)
{
    UNICODE_STRING ValueName;

    RtlInitUnicodeString(&ValueName, Name);
    return ZwSetValueKey(Key, &ValueName, 0, Type, Data, Size);
}

static
NTSTATUS
QueryValue(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _Out_ PULONG Type)
{
    UCHAR Buffer[128];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    UNICODE_STRING ValueName;
    ULONG Length;
    NTSTATUS Status;

    RtlInitUnicodeString(&ValueName, Name);
    Status = ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Info,
                             sizeof(Buffer), &Length);
    *Type = NT_SUCCESS(Status) ? Info->Type : REG_NONE;
    return Status;
}

static
VOID
DeleteTree(
    _In_ HANDLE Key)
{
    UCHAR Buffer[sizeof(KEY_BASIC_INFORMATION) + 256 * sizeof(WCHAR)];
    PKEY_BASIC_INFORMATION Info = (PKEY_BASIC_INFORMATION)Buffer;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING Name;
    HANDLE Sub;
    ULONG Length;

    while (NT_SUCCESS(ZwEnumerateKey(Key, 0, KeyBasicInformation, Info,
                                     sizeof(Buffer) - sizeof(WCHAR), &Length)))
    {
        Name.Buffer = Info->Name;
        Name.Length = Name.MaximumLength = (USHORT)Info->NameLength;
        InitializeObjectAttributes(&ObjectAttributes, &Name,
                                   OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, Key, NULL);
        if (!NT_SUCCESS(ZwOpenKey(&Sub, KEY_ALL_ACCESS, &ObjectAttributes)))
            break;
        DeleteTree(Sub);
        ZwClose(Sub);
    }
    ZwDeleteKey(Key);
}

static
VOID
DeleteKeyByName(
    _In_ PCWSTR Name)
{
    HANDLE Key;

    if (NT_SUCCESS(KeyOpen(&Key, NULL, Name)))
    {
        DeleteTree(Key);
        ZwClose(Key);
    }
}

static
NTSTATUS
OpenFile(
    _Out_ PHANDLE File,
    _In_ PUNICODE_STRING FileName,
    _In_ BOOLEAN Write)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;

    InitializeObjectAttributes(&ObjectAttributes, FileName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateFile(File,
                        (Write ? GENERIC_WRITE : GENERIC_READ) | SYNCHRONIZE,
                        &ObjectAttributes, &IoStatus, NULL, FILE_ATTRIBUTE_NORMAL,
                        Write ? 0 : FILE_SHARE_READ,
                        Write ? FILE_OVERWRITE_IF : FILE_OPEN,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                        NULL, 0);
}

static
VOID
DeleteFileByName(
    _In_ PUNICODE_STRING FileName)
{
    OBJECT_ATTRIBUTES ObjectAttributes;

    InitializeObjectAttributes(&ObjectAttributes, FileName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    ZwDeleteFile(&ObjectAttributes);
}

static
BOOLEAN
SaveSource(VOID)
{
    HANDLE Src, Sub, Deep, File;
    ULONG Dword = 0x1234;
    NTSTATUS Status;

    DeleteKeyByName(SRC_KEY);
    Status = KeyCreate(&Src, NULL, SRC_KEY, REG_OPTION_NON_VOLATILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return FALSE;

    SetValue(Src, L"A", REG_SZ, L"restored", sizeof(L"restored"));
    SetValue(Src, L"B", REG_DWORD, &Dword, sizeof(Dword));
    Status = KeyCreate(&Sub, Src, L"Sub", REG_OPTION_NON_VOLATILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        SetValue(Sub, L"C", REG_DWORD, &Dword, sizeof(Dword));
        if (NT_SUCCESS(KeyCreate(&Deep, Sub, L"Deep", REG_OPTION_NON_VOLATILE)))
            ZwClose(Deep);
        ZwClose(Sub);
    }

    Status = OpenFile(&File, &HiveFile, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = ZwSaveKey(Src, File);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(File);
    }

    DeleteTree(Src);
    ZwClose(Src);
    return NT_SUCCESS(Status);
}

static
NTSTATUS
Restore(
    _In_ HANDLE Key,
    _In_ PUNICODE_STRING FileName,
    _In_ ULONG Flags)
{
    HANDLE File;
    NTSTATUS Status;

    Status = OpenFile(&File, FileName, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return Status;

    Status = ZwRestoreKey(Key, File, Flags);
    ZwClose(File);
    return Status;
}

static
VOID
CheckRestored(
    _In_ HANDLE Key)
{
    UCHAR Buffer[sizeof(KEY_FULL_INFORMATION) + 64];
    PKEY_FULL_INFORMATION Full = (PKEY_FULL_INFORMATION)Buffer;
    HANDLE Sub;
    ULONG Type, Length;
    NTSTATUS Status;

    ok_eq_hex(QueryValue(Key, L"Old", &Type), STATUS_OBJECT_NAME_NOT_FOUND);
    ok_eq_hex(QueryValue(Key, L"A", &Type), STATUS_SUCCESS);
    ok_eq_ulong(Type, (ULONG)REG_SZ);
    ok_eq_hex(QueryValue(Key, L"B", &Type), STATUS_SUCCESS);
    ok_eq_ulong(Type, (ULONG)REG_DWORD);

    ok_eq_hex(KeyOpen(&Sub, Key, L"OldSub"), STATUS_OBJECT_NAME_NOT_FOUND);
    Status = KeyOpen(&Sub, Key, L"Sub\\Deep");
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) ZwClose(Sub);

    Status = ZwQueryKey(Key, KeyFullInformation, Full, sizeof(Buffer), &Length);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_ulong(Full->Values, 2UL);
        ok_eq_ulong(Full->SubKeys, 1UL);
        ok(Full->MaxNameLen >= 3 * sizeof(WCHAR), "MaxNameLen %lu\n", Full->MaxNameLen);
    }
}

static
VOID
TestRoundTrip(VOID)
{
    HANDLE Dst, Old, OpenSub;
    ULONG Dword = 1;
    NTSTATUS Status;

    DeleteKeyByName(DST_KEY);
    Status = KeyCreate(&Dst, NULL, DST_KEY, REG_OPTION_NON_VOLATILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    SetValue(Dst, L"Old", REG_DWORD, &Dword, sizeof(Dword));
    if (NT_SUCCESS(KeyCreate(&Old, Dst, L"OldSub\\Inner", REG_OPTION_NON_VOLATILE)))
        ZwClose(Old);

    ok_eq_hex(Restore(Dst, &HiveFile, 0), STATUS_SUCCESS);
    CheckRestored(Dst);

    ok_eq_hex(Restore(Dst, &HiveFile, 0x10), STATUS_INVALID_PARAMETER);
    ok_eq_hex(Restore(Dst, &HiveFile, REG_WHOLE_HIVE_VOLATILE), STATUS_NOT_IMPLEMENTED);

    Status = KeyOpen(&OpenSub, Dst, L"Sub");
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_hex(Restore(Dst, &HiveFile, 0), STATUS_CANNOT_DELETE);
        ok_eq_hex(Restore(Dst, &HiveFile, REG_FORCE_RESTORE), STATUS_SUCCESS);
        ok_eq_hex(SetValue(OpenSub, L"X", REG_DWORD, &Dword, sizeof(Dword)), STATUS_KEY_DELETED);
        ZwClose(OpenSub);
        CheckRestored(Dst);
    }

    DeleteTree(Dst);
    ZwClose(Dst);
}

static
VOID
TestVolatileTarget(VOID)
{
    HANDLE Dst;
    NTSTATUS Status;

    DeleteKeyByName(VDST_KEY);
    Status = KeyCreate(&Dst, NULL, VDST_KEY, REG_OPTION_VOLATILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    ok_eq_hex(Restore(Dst, &HiveFile, 0), STATUS_SUCCESS);
    CheckRestored(Dst);

    DeleteTree(Dst);
    ZwClose(Dst);
}

static
VOID
TestBadFiles(VOID)
{
    static UCHAR Zeros[2 * 4096];
    IO_STATUS_BLOCK IoStatus;
    HANDLE Dst, File;
    NTSTATUS Status;

    Status = KeyCreate(&Dst, NULL, DST_KEY, REG_OPTION_NON_VOLATILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    Status = OpenFile(&File, &BadFile, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ZwWriteFile(File, NULL, NULL, NULL, &IoStatus, Zeros, sizeof(Zeros), NULL, NULL);
        ZwClose(File);
        ok_eq_hex(Restore(Dst, &BadFile, 0), STATUS_REGISTRY_CORRUPT);
    }

    Status = OpenFile(&File, &BadFile, TRUE);
    if (NT_SUCCESS(Status))
    {
        ZwWriteFile(File, NULL, NULL, NULL, &IoStatus, Zeros, 512, NULL, NULL);
        ZwClose(File);
        ok_eq_hex(Restore(Dst, &BadFile, 0), STATUS_REGISTRY_CORRUPT);
    }

    DeleteFileByName(&BadFile);
    DeleteTree(Dst);
    ZwClose(Dst);
}

START_TEST(CmRestoreKey)
{
    if (!SaveSource())
    {
        skip(FALSE, "Could not save the source key\n");
        return;
    }

    TestRoundTrip();
    TestVolatileTarget();
    TestBadFiles();

    DeleteFileByName(&HiveFile);
}
