/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     FsRtlOplockKeysEqual and OPLOCK_FLAG_OPLOCK_KEY_CHECK_ONLY key attach semantics
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define OPLOCK_KEY_TAG 'PMTO'

static GUID KeyOne = { 0x11111111, 0x1111, 0x1111, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 } };
static GUID KeyTwo = { 0x22222222, 0x2222, 0x2222, { 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22 } };
static GUID ParentOne = { 0x33333333, 0x3333, 0x3333, { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33 } };
static GUID ParentTwo = { 0x66666666, 0x6666, 0x6666, { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66 } };
static GUID TargetOne = { 0x44444444, 0x4444, 0x4444, { 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44 } };
static GUID TargetTwo = { 0x55555555, 0x5555, 0x5555, { 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 } };

static
NTSTATUS
OpenFileObject(
    _Out_ PHANDLE Handle,
    _Out_ PFILE_OBJECT *FileObject)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\SystemRoot\\system32\\ntoskrnl.exe");
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    *FileObject = NULL;

    InitializeObjectAttributes(&Attributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwOpenFile(Handle,
                        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                        &Attributes,
                        &IoStatus,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status)) return Status;

    Status = ObReferenceObjectByHandle(*Handle, 0, *IoFileObjectType, KernelMode, (PVOID *)FileObject, NULL);
    if (!NT_SUCCESS(Status)) ZwClose(*Handle);

    return Status;
}

static
VOID
CloseFileObject(
    _In_ HANDLE Handle,
    _In_ PFILE_OBJECT FileObject)
{
    ObDereferenceObject(FileObject);
    ZwClose(Handle);
}

static
NTSTATUS
CheckOplockExOnFileObject(
    _In_ PFILE_OBJECT FileObject,
    _In_ UCHAR MajorFunction,
    _In_ ULONG Flags,
    _In_opt_ PECP_LIST EcpList)
{
    PVOID Oplock = NULL;
    PIRP Irp;
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status = STATUS_INSUFFICIENT_RESOURCES;

    Irp = IoAllocateIrp(1, FALSE);
    if (Irp == NULL) return Status;

    Stack = IoGetCurrentIrpStackLocation(Irp);
    Stack->MajorFunction = MajorFunction;
    Stack->FileObject = FileObject;

    if (EcpList != NULL)
    {
        Status = FsRtlSetEcpListIntoIrp(Irp, EcpList);
    }

    if (NT_SUCCESS(Status))
    {
        Status = FsRtlCheckOplockEx(&Oplock, Irp, Flags, NULL, NULL, NULL);
    }

    IoFreeIrp(Irp);
    return Status;
}

static
NTSTATUS
AttachSingleKey(
    _In_ PFILE_OBJECT FileObject,
    _In_ PCGUID Key)
{
    PECP_LIST EcpList = NULL;
    PVOID EcpContext;
    POPLOCK_KEY_ECP_CONTEXT KeyContext;
    NTSTATUS Status;

    Status = FsRtlAllocateExtraCreateParameterList(0, &EcpList);
    if (!NT_SUCCESS(Status)) return Status;

    Status = FsRtlAllocateExtraCreateParameter(&GUID_ECP_OPLOCK_KEY,
                                               sizeof(OPLOCK_KEY_ECP_CONTEXT),
                                               0,
                                               NULL,
                                               OPLOCK_KEY_TAG,
                                               &EcpContext);
    if (!NT_SUCCESS(Status))
    {
        FsRtlFreeExtraCreateParameterList(EcpList);
        return Status;
    }

    KeyContext = EcpContext;
    KeyContext->OplockKey = *Key;
    KeyContext->Reserved = 0;

    Status = FsRtlInsertExtraCreateParameter(EcpList, EcpContext);
    if (NT_SUCCESS(Status))
    {
        Status = CheckOplockExOnFileObject(FileObject, IRP_MJ_CREATE, OPLOCK_FLAG_OPLOCK_KEY_CHECK_ONLY, EcpList);
    }

    FsRtlFreeExtraCreateParameterList(EcpList);
    return Status;
}

static
NTSTATUS
AttachDualKey(
    _In_ PFILE_OBJECT FileObject,
    _In_opt_ PCGUID ParentKey,
    _In_opt_ PCGUID TargetKey)
{
    PECP_LIST EcpList = NULL;
    PVOID EcpContext;
    PDUAL_OPLOCK_KEY_ECP_CONTEXT DualContext;
    NTSTATUS Status;

    Status = FsRtlAllocateExtraCreateParameterList(0, &EcpList);
    if (!NT_SUCCESS(Status)) return Status;

    Status = FsRtlAllocateExtraCreateParameter(&GUID_ECP_DUAL_OPLOCK_KEY,
                                               sizeof(DUAL_OPLOCK_KEY_ECP_CONTEXT),
                                               0,
                                               NULL,
                                               OPLOCK_KEY_TAG,
                                               &EcpContext);
    if (!NT_SUCCESS(Status))
    {
        FsRtlFreeExtraCreateParameterList(EcpList);
        return Status;
    }

    DualContext = EcpContext;
    RtlZeroMemory(DualContext, sizeof(DUAL_OPLOCK_KEY_ECP_CONTEXT));
    if (ParentKey != NULL)
    {
        DualContext->ParentOplockKey = *ParentKey;
        DualContext->ParentOplockKeySet = TRUE;
    }
    if (TargetKey != NULL)
    {
        DualContext->TargetOplockKey = *TargetKey;
        DualContext->TargetOplockKeySet = TRUE;
    }

    Status = FsRtlInsertExtraCreateParameter(EcpList, EcpContext);
    if (NT_SUCCESS(Status))
    {
        Status = CheckOplockExOnFileObject(FileObject, IRP_MJ_CREATE, OPLOCK_FLAG_OPLOCK_KEY_CHECK_ONLY, EcpList);
    }

    FsRtlFreeExtraCreateParameterList(EcpList);
    return Status;
}

static
VOID
SendCleanup(
    _In_ PFILE_OBJECT FileObject)
{
    NTSTATUS Status;

    Status = CheckOplockExOnFileObject(FileObject, IRP_MJ_CLEANUP, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

START_TEST(OplockKeysEqualNoKeys)
{
    HANDLE HandleA, HandleB;
    PFILE_OBJECT FileA, FileB;
    NTSTATUS StatusA, StatusB;

    StatusA = OpenFileObject(&HandleA, &FileA);
    StatusB = OpenFileObject(&HandleB, &FileB);
    ok_eq_hex(StatusA, STATUS_SUCCESS);
    ok_eq_hex(StatusB, STATUS_SUCCESS);
    if (!NT_SUCCESS(StatusA) || !NT_SUCCESS(StatusB))
    {
        if (NT_SUCCESS(StatusA)) CloseFileObject(HandleA, FileA);
        if (NT_SUCCESS(StatusB)) CloseFileObject(HandleB, FileB);
        return;
    }

    ok_bool_true(FsRtlOplockKeysEqual(FileA, FileA), "same pointer matches");
    ok_bool_false(FsRtlOplockKeysEqual(FileA, FileB), "keyless opens do not match");
    ok_bool_false(FsRtlOplockKeysEqual(FileA, NULL), "one missing file object");
    ok_bool_false(FsRtlOplockKeysEqual(NULL, FileB), "other missing file object");
    ok_bool_false(FsRtlOplockKeysEqual(NULL, NULL), "both missing");

    CloseFileObject(HandleA, FileA);
    CloseFileObject(HandleB, FileB);
}

START_TEST(OplockKeyCheckOnlyAttach)
{
    HANDLE HandleA, HandleB, HandleC;
    PFILE_OBJECT FileA, FileB, FileC;
    NTSTATUS StatusA, StatusB, StatusC;

    StatusA = OpenFileObject(&HandleA, &FileA);
    StatusB = OpenFileObject(&HandleB, &FileB);
    StatusC = OpenFileObject(&HandleC, &FileC);
    ok_eq_hex(StatusA, STATUS_SUCCESS);
    ok_eq_hex(StatusB, STATUS_SUCCESS);
    ok_eq_hex(StatusC, STATUS_SUCCESS);
    if (!NT_SUCCESS(StatusA) || !NT_SUCCESS(StatusB) || !NT_SUCCESS(StatusC))
    {
        if (NT_SUCCESS(StatusA)) CloseFileObject(HandleA, FileA);
        if (NT_SUCCESS(StatusB)) CloseFileObject(HandleB, FileB);
        if (NT_SUCCESS(StatusC)) CloseFileObject(HandleC, FileC);
        return;
    }

    StatusA = AttachSingleKey(FileA, &KeyOne);
    StatusB = AttachSingleKey(FileB, &KeyOne);
    ok_eq_hex(StatusA, STATUS_SUCCESS);
    ok_eq_hex(StatusB, STATUS_SUCCESS);
    ok_bool_true(FsRtlOplockKeysEqual(FileA, FileB), "same key attached to both");

    ok_bool_false(FsRtlOplockKeysEqual(FileA, FileC), "keyed vs keyless");

    StatusB = AttachSingleKey(FileB, &KeyTwo);
    ok_eq_hex(StatusB, STATUS_SUCCESS);
    ok_bool_false(FsRtlOplockKeysEqual(FileA, FileB), "reattach overwrites key");

    SendCleanup(FileB);
    ok_bool_false(FsRtlOplockKeysEqual(FileA, FileB), "cleanup drops key");

    StatusA = CheckOplockExOnFileObject(FileA, IRP_MJ_CREATE, OPLOCK_FLAG_OPLOCK_KEY_CHECK_ONLY, NULL);
    ok_eq_hex(StatusA, STATUS_SUCCESS);
    ok_bool_false(FsRtlOplockKeysEqual(FileA, FileC), "no ECP list attaches nothing");

    CloseFileObject(HandleA, FileA);
    CloseFileObject(HandleB, FileB);
    CloseFileObject(HandleC, FileC);
}

START_TEST(OplockKeyDual)
{
    HANDLE HandleA, HandleB;
    PFILE_OBJECT FileA, FileB;
    NTSTATUS StatusA, StatusB;

    StatusA = OpenFileObject(&HandleA, &FileA);
    StatusB = OpenFileObject(&HandleB, &FileB);
    ok_eq_hex(StatusA, STATUS_SUCCESS);
    ok_eq_hex(StatusB, STATUS_SUCCESS);
    if (!NT_SUCCESS(StatusA) || !NT_SUCCESS(StatusB))
    {
        if (NT_SUCCESS(StatusA)) CloseFileObject(HandleA, FileA);
        if (NT_SUCCESS(StatusB)) CloseFileObject(HandleB, FileB);
        return;
    }

    StatusA = AttachDualKey(FileA, &ParentOne, &TargetOne);
    StatusB = AttachDualKey(FileB, &ParentTwo, &TargetOne);
    ok_eq_hex(StatusA, STATUS_SUCCESS);
    ok_eq_hex(StatusB, STATUS_SUCCESS);
    ok_bool_true(FsRtlOplockKeysEqual(FileA, FileB), "target keys compared, parents ignored");

    StatusB = AttachDualKey(FileB, &ParentTwo, &TargetTwo);
    ok_eq_hex(StatusB, STATUS_SUCCESS);
    ok_bool_false(FsRtlOplockKeysEqual(FileA, FileB), "differing target keys");

    StatusB = AttachDualKey(FileB, &ParentOne, NULL);
    ok_eq_hex(StatusB, STATUS_SUCCESS);
    ok_bool_false(FsRtlOplockKeysEqual(FileA, FileB), "target not set does not match");

    CloseFileObject(HandleA, FileA);
    CloseFileObject(HandleB, FileB);
}
