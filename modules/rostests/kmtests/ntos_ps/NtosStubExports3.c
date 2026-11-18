/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Third batch of ntoskrnl exports that replaced spec stubs
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <kmt_test.h>

#define TAG_TEST 'S3tK'

static VOID
TestCorrelationVectors(VOID)
{
    static const GUID Zero;
    GUID Ones;
    CORRELATION_VECTOR Cv;
    ULONG Index;
    NTSTATUS Status;

    ok_eq_hex(RtlInitializeCorrelationVector(&Cv, RTL_CORRELATION_VECTOR_VERSION_2, NULL), STATUS_INVALID_PARAMETER);
    ok_eq_hex(RtlInitializeCorrelationVector(&Cv, 3, &Zero), STATUS_INVALID_PARAMETER);

    ok_eq_hex(RtlInitializeCorrelationVector(&Cv, RTL_CORRELATION_VECTOR_VERSION_2, &Zero), STATUS_SUCCESS);
    ok_eq_str(Cv.Vector, "AAAAAAAAAAAAAAAAAAAAAA.0");
    ok_eq_hex(RtlValidateCorrelationVector(&Cv), STATUS_SUCCESS);

    RtlFillMemory(&Ones, sizeof(Ones), 0xFF);
    ok_eq_hex(RtlInitializeCorrelationVector(&Cv, RTL_CORRELATION_VECTOR_VERSION_2, &Ones), STATUS_SUCCESS);
    ok_eq_str(Cv.Vector, "/////////////////////w.0");

    ok_eq_hex(RtlInitializeCorrelationVector(&Cv, RTL_CORRELATION_VECTOR_VERSION_1, &Ones), STATUS_SUCCESS);
    ok_eq_str(Cv.Vector, "////////////////.0");
    ok_eq_hex(RtlValidateCorrelationVector(&Cv), STATUS_SUCCESS);

    ok_eq_hex(RtlInitializeCorrelationVector(&Cv, RTL_CORRELATION_VECTOR_VERSION_2, &Zero), STATUS_SUCCESS);
    ok_eq_hex(RtlIncrementCorrelationVector(&Cv), STATUS_SUCCESS);
    ok_eq_str(Cv.Vector, "AAAAAAAAAAAAAAAAAAAAAA.1");
    ok_eq_hex(RtlExtendCorrelationVector(&Cv), STATUS_SUCCESS);
    ok_eq_str(Cv.Vector, "AAAAAAAAAAAAAAAAAAAAAA.1.0");
    for (Index = 0; Index < 10; Index++)
        RtlIncrementCorrelationVector(&Cv);
    ok_eq_str(Cv.Vector, "AAAAAAAAAAAAAAAAAAAAAA.1.10");

    /* Extending stops at 127 characters */
    for (Index = 0; Index < 60; Index++)
    {
        Status = RtlExtendCorrelationVector(&Cv);
        if (!NT_SUCCESS(Status))
            break;
    }
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok(strlen(Cv.Vector) <= 127, "vector grew to %Iu characters\n", strlen(Cv.Vector));
    ok_eq_hex(RtlValidateCorrelationVector(&Cv), STATUS_SUCCESS);

    RtlCopyMemory(Cv.Vector, "AAAAAAAAAAAAAAAAAAAAAA.", sizeof("AAAAAAAAAAAAAAAAAAAAAA."));
    ok_eq_hex(RtlValidateCorrelationVector(&Cv), STATUS_INVALID_PARAMETER);
    RtlCopyMemory(Cv.Vector, "AAAAAAAAAAAAAAAAAAAAAB.0", sizeof("AAAAAAAAAAAAAAAAAAAAAB.0"));
    ok_eq_hex(RtlValidateCorrelationVector(&Cv), STATUS_INVALID_PARAMETER);
    RtlCopyMemory(Cv.Vector, "AAAAAAAAAAAAAAAAAAAAA-.0", sizeof("AAAAAAAAAAAAAAAAAAAAA-.0"));
    ok_eq_hex(RtlValidateCorrelationVector(&Cv), STATUS_INVALID_PARAMETER);
    ok_eq_hex(RtlIncrementCorrelationVector(&Cv), STATUS_INVALID_PARAMETER);

    RtlCopyMemory(Cv.Vector, "AAAAAAAAAAAAAAAAAAAAAA.4294967295", sizeof("AAAAAAAAAAAAAAAAAAAAAA.4294967295"));
    ok_eq_hex(RtlIncrementCorrelationVector(&Cv), STATUS_BUFFER_OVERFLOW);
}

static VOID
TestFillAndPlaceholder(VOID)
{
    UCHAR Buffer[64];
    ULONG Index;
    BOOLEAN Good = TRUE;

    RtlFillMemory(Buffer, sizeof(Buffer), 0x11);
    RtlFillMemoryNonTemporal(Buffer + 3, 37, 0xC3);
    for (Index = 0; Index < sizeof(Buffer); Index++)
        Good &= (Buffer[Index] == ((Index >= 3 && Index < 40) ? 0xC3 : 0x11));
    ok(Good, "non-temporal fill wrote the wrong bytes\n");

    RtlFillMemoryNonTemporal(Buffer, 5, 0x22);
    ok(Buffer[4] == 0x22 && Buffer[5] == 0xC3, "short fill\n");

    ok(RtlIsCloudFilesPlaceholder(FILE_ATTRIBUTE_REPARSE_POINT, 0x9000001A), "IO_REPARSE_TAG_CLOUD\n");
    ok(RtlIsCloudFilesPlaceholder(FILE_ATTRIBUTE_REPARSE_POINT, 0x9000F01A), "IO_REPARSE_TAG_CLOUD_F\n");
    ok(!RtlIsCloudFilesPlaceholder(0, 0x9000001A), "no reparse point, the value is an EA size\n");
    ok(!RtlIsCloudFilesPlaceholder(FILE_ATTRIBUTE_REPARSE_POINT, IO_REPARSE_TAG_SYMLINK), "symlink\n");
}

static VOID
TestDecompressFragmentEx(VOID)
{
    PUCHAR Source, Compressed, Output;
    PVOID WorkSpace = NULL, FragmentSpace = NULL;
    ULONG WorkSize, FragmentSize, CompressedSize, Final, Index;
    NTSTATUS Status;

    Source = ExAllocatePoolWithTag(PagedPool, 3 * 8192, TAG_TEST);
    if (Source == NULL) return;
    Compressed = Source + 8192;
    Output = Compressed + 8192;
    for (Index = 0; Index < 8192; Index++)
        Source[Index] = (UCHAR)((Index / 7) % 23);

    Status = RtlGetCompressionWorkSpaceSize(COMPRESSION_FORMAT_LZNT1, &WorkSize, &FragmentSize);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        WorkSpace = ExAllocatePoolWithTag(PagedPool, max(WorkSize, 1), TAG_TEST);
        FragmentSpace = ExAllocatePoolWithTag(PagedPool, max(FragmentSize, 4096), TAG_TEST);
    }
    if (WorkSpace != NULL && FragmentSpace != NULL)
    {
        Status = RtlCompressBuffer(COMPRESSION_FORMAT_LZNT1, Source, 8192, Compressed, 8192, 4096,
                                   &CompressedSize, WorkSpace);
        ok_eq_hex(Status, STATUS_SUCCESS);

        Status = RtlDecompressFragmentEx(COMPRESSION_FORMAT_LZNT1, Output, 1000, Compressed, CompressedSize,
                                         5000, 4096, &Final, FragmentSpace);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(Final, 1000UL);
        ok(RtlEqualMemory(Output, Source + 5000, 1000), "fragment at 5000 differs\n");

        Status = RtlDecompressFragmentEx(COMPRESSION_FORMAT_LZNT1, Output, 1000, Compressed, CompressedSize,
                                         0, 1000, &Final, FragmentSpace);
        ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
        Status = RtlDecompressFragmentEx(COMPRESSION_FORMAT_NONE, Output, 1000, Compressed, CompressedSize,
                                         0, 4096, &Final, FragmentSpace);
        ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    }

    if (FragmentSpace) ExFreePoolWithTag(FragmentSpace, TAG_TEST);
    if (WorkSpace) ExFreePoolWithTag(WorkSpace, TAG_TEST);
    ExFreePoolWithTag(Source, TAG_TEST);
}

static VOID
NTAPI
SiloContextCleanup(
    _In_ PVOID SiloContext)
{
    UNREFERENCED_PARAMETER(SiloContext);
}

static VOID
NTAPI
ImageNotify(
    _In_opt_ PUNICODE_STRING FullImageName,
    _In_ HANDLE ProcessId,
    _In_ PIMAGE_INFO ImageInfo)
{
    UNREFERENCED_PARAMETER(FullImageName);
    UNREFERENCED_PARAMETER(ProcessId);
    UNREFERENCED_PARAMETER(ImageInfo);
}

static VOID
TestPs(VOID)
{
    PVOID Context, Removed = NULL;
    ULONG Slot;
    NTSTATUS Status;

    Status = PsAllocSiloContextSlot(0, &Slot);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = PsCreateSiloContext(NULL, sizeof(ULONG), NonPagedPoolNx, SiloContextCleanup, &Context);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok_eq_hex(PsRemoveSiloContext(NULL, Slot, NULL), STATUS_NOT_FOUND);
            ok_eq_hex(PsInsertSiloContext(NULL, Slot, Context), STATUS_SUCCESS);
            ok_eq_hex(PsRemoveSiloContext(NULL, Slot, &Removed), STATUS_SUCCESS);
            ok(Removed == Context, "removed %p, inserted %p\n", Removed, Context);
            PsDereferenceSiloContext(Removed);
            ok_eq_hex(PsRemoveSiloContext(NULL, Slot, &Removed), STATUS_NOT_FOUND);

            /* Without an output pointer the table's reference is dropped here */
            ok_eq_hex(PsInsertSiloContext(NULL, Slot, Context), STATUS_SUCCESS);
            ok_eq_hex(PsRemoveSiloContext(NULL, Slot, NULL), STATUS_SUCCESS);
            PsDereferenceSiloContext(Context);
        }
        ok_eq_hex(PsRemoveSiloContext(NULL, MAXULONG, NULL), STATUS_INVALID_PARAMETER);
        PsFreeSiloContextSlot(Slot);
    }

    ok_eq_hex(PsSetLoadImageNotifyRoutineEx(ImageNotify, 2), STATUS_INVALID_PARAMETER_2);
    Status = PsSetLoadImageNotifyRoutineEx(ImageNotify, PS_IMAGE_NOTIFY_CONFLICTING_ARCHITECTURE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ok_eq_hex(PsRemoveLoadImageNotifyRoutine(ImageNotify), STATUS_SUCCESS);
}

typedef struct _MAP_CONTEXT
{
    PVOID Expected;
    PVOID Seen;
    ULONG Calls;
} MAP_CONTEXT, *PMAP_CONTEXT;

static VOID
NTAPI
MapRoutine(
    _In_opt_ PVOID DriverContext,
    _In_ PVOID MappedVa)
{
    PMAP_CONTEXT Context = DriverContext;

    Context->Calls++;
    Context->Seen = MappedVa;
    RtlFillMemory(MappedVa, 16, 0x5A);
}

static VOID
TestMapMdl(VOID)
{
    PHYSICAL_ADDRESS Low, High, Skip;
    MAP_CONTEXT Context;
    PUCHAR Buffer, Va;
    PMDL Mdl;

    Buffer = ExAllocatePoolWithTag(NonPagedPoolNx, 64, TAG_TEST);
    if (Buffer == NULL) return;
    Mdl = IoAllocateMdl(Buffer, 64, FALSE, FALSE, NULL);
    ok(Mdl != NULL, "no MDL\n");
    if (Mdl != NULL)
    {
        MmBuildMdlForNonPagedPool(Mdl);
        ok_eq_hex(MmMapMdl(Mdl, PAGE_READWRITE, NULL, NULL), STATUS_INVALID_PARAMETER);

        RtlZeroMemory(&Context, sizeof(Context));
        ok_eq_hex(MmMapMdl(Mdl, PAGE_READWRITE, MapRoutine, &Context), STATUS_SUCCESS);
        ok_eq_ulong(Context.Calls, 1UL);
        ok(Context.Seen == Buffer, "nonpaged MDL mapped at %p, buffer %p\n", Context.Seen, Buffer);
        ok(Buffer[15] == 0x5A, "routine wrote through the mapping\n");
        IoFreeMdl(Mdl);
    }
    ExFreePoolWithTag(Buffer, TAG_TEST);

    Low.QuadPart = 0;
    High.QuadPart = -1;
    Skip.QuadPart = 0;
    Mdl = MmAllocatePagesForMdlEx(Low, High, Skip, PAGE_SIZE, MmCached, 0);
    ok(Mdl != NULL, "page allocation failed\n");
    if (Mdl == NULL) return;

    RtlZeroMemory(&Context, sizeof(Context));
    ok_eq_hex(MmMapMdl(Mdl, PAGE_READWRITE, MapRoutine, &Context), STATUS_SUCCESS);
    ok_eq_ulong(Context.Calls, 1UL);
    ok(!(Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA), "the temporary mapping is released\n");

    Va = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
    if (Va != NULL)
    {
        ok(Va[0] == 0x5A && Va[15] == 0x5A, "pages keep what the routine wrote\n");
        MmUnmapLockedPages(Va, Mdl);
    }
    MmFreePagesFromMdl(Mdl);
    ExFreePool(Mdl);
}

static VOID
TestCacheQueries(VOID)
{
    SECTION_OBJECT_POINTERS Pointers;
    VPB Vpb;
    ULONG Pages = 7;
    LONGLONG Vdl;

    RtlZeroMemory(&Pointers, sizeof(Pointers));
    ok(CcGetFileObjectFromSectionPtrsRef(&Pointers) == NULL, "no cached file behind empty pointers\n");

    RtlZeroMemory(&Vpb, sizeof(Vpb));
    ok(!CcIsThereDirtyDataEx(&Vpb, &Pages), "an unknown volume has no dirty data\n");
    ok_eq_ulong(Pages, 0UL);
    ok(!CcIsThereDirtyDataEx(&Vpb, NULL), "count is optional\n");

    ok_eq_hex(FsRtlQueryCachedVdl(NULL, &Vdl), STATUS_INVALID_PARAMETER);
}

static VOID
TestDeviceRoutines(VOID)
{
    static const UNICODE_STRING NullName = RTL_CONSTANT_STRING(L"\\Device\\Null");
    IO_CREATE_STREAM_FILE_OPTIONS Options;
    PFILE_OBJECT FileObject, Stream;
    PDEVICE_OBJECT DeviceObject;
    HANDLE Handle;
    ULONG Returned = 5;
    NTSTATUS Status;

    Status = IoGetDeviceObjectPointer((PUNICODE_STRING)&NullName, FILE_READ_DATA, &FileObject, &DeviceObject);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    /* Null does not handle FSCTLs; the request still makes the round trip synchronously */
    ok_eq_hex(FsRtlKernelFsControlFile(NULL, FSCTL_IS_VOLUME_MOUNTED, NULL, 0, NULL, 0, &Returned),
              STATUS_INVALID_PARAMETER);
    Status = FsRtlKernelFsControlFile(FileObject, FSCTL_IS_VOLUME_MOUNTED, NULL, 0, NULL, 0, &Returned);
    ok(Status != STATUS_PENDING, "kernel FSCTL returned pending\n");
    ok_eq_ulong(Returned, 0UL);

    RtlZeroMemory(&Options, sizeof(Options));
    ok_eq_hex(IoCreateStreamFileObjectEx2(&Options, NULL, DeviceObject, &Stream, NULL), STATUS_INVALID_PARAMETER);

    Options.Size = sizeof(Options);
    Options.Flags = IO_CREATE_STREAM_FILE_LITE;
    ok_eq_hex(IoCreateStreamFileObjectEx2(&Options, NULL, DeviceObject, &Stream, &Handle), STATUS_INVALID_PARAMETER);
    Options.Flags = 0x80;
    ok_eq_hex(IoCreateStreamFileObjectEx2(&Options, NULL, DeviceObject, &Stream, NULL), STATUS_INVALID_PARAMETER);

    Options.Flags = IO_CREATE_STREAM_FILE_LITE;
    Status = IoCreateStreamFileObjectEx2(&Options, FileObject, NULL, &Stream, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(Stream->Flags & FO_STREAM_FILE, "stream flag\n");
        ok(Stream->DeviceObject == DeviceObject, "device %p\n", Stream->DeviceObject);
        ObDereferenceObject(Stream);
    }

    Options.Flags = 0;
    Options.TargetDeviceObject = DeviceObject;
    Status = IoCreateStreamFileObjectEx2(&Options, NULL, DeviceObject, &Stream, &Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(Stream->Flags & FO_FILE_OBJECT_HAS_EXTENSION, "target stored in the file object extension\n");
        ok(IoGetRelatedDeviceObject(Stream) == DeviceObject, "related device\n");
        ZwClose(Handle);
    }

    ObDereferenceObject(FileObject);
}

START_TEST(NtosStubExports3)
{
    TestCorrelationVectors();
    TestFillAndPlaceholder();
    TestDecompressFragmentEx();
    TestPs();
    TestMapMdl();
    TestCacheQueries();
    TestDeviceRoutines();
}
