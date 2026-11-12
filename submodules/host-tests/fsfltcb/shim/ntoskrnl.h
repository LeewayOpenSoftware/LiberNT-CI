/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Minimal kernel surface that ntoskrnl/fsrtl/fsfltcb.c compiles
 *              against on a host, found through -I ahead of any real header so
 *              the real file is built unchanged. ERESOURCEs track ownership and
 *              abort on a release of a resource that is not held; the FsRtl
 *              critical region is counted; device stacks are plain arrays.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#ifndef FSFLTCB_HOST_NTOSKRNL_H
#define FSFLTCB_HOST_NTOSKRNL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#define IN
#define OUT
#define OPTIONAL
#define NTAPI
#define VOID void
#define _In_
#define _Out_
#define _Check_return_
#define _Outptr_result_maybenull_
#define POINTER_ALIGNMENT
typedef void *PVOID;
typedef unsigned char UCHAR, BOOLEAN, *PUCHAR;
typedef uint32_t ULONG, *PULONG;
typedef int32_t LONG, NTSTATUS;
typedef uintptr_t ULONG_PTR;
typedef union _LARGE_INTEGER { int64_t QuadPart; } LARGE_INTEGER, *PLARGE_INTEGER;
#define TRUE 1
#define FALSE 0

#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
#define FIELD_OFFSET(type, field) ((ULONG)offsetof(type, field))
#define FlagOn(F, SF) ((F) & (SF))
#define UNREFERENCED_PARAMETER(P) ((void)(P))
#define PAGED_CODE() do { } while (0)
#define ASSERT(x) assert(x)
#define RtlZeroMemory(Dst, Len) memset((Dst), 0, (Len))
#define RtlCopyMemory(Dst, Src, Len) memcpy((Dst), (Src), (Len))

#define STATUS_SUCCESS                            ((NTSTATUS)0x00000000)
#define STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY ((NTSTATUS)0x00000126)
#define STATUS_FILE_LOCKED_WITH_ONLY_READERS      ((NTSTATUS)0x0000012A)
#define STATUS_FILE_LOCKED_WITH_WRITERS           ((NTSTATUS)0x0000012B)
#define STATUS_UNSUCCESSFUL                       ((NTSTATUS)0xC0000001)
#define STATUS_INVALID_PARAMETER                  ((NTSTATUS)0xC000000D)
#define STATUS_ACCESS_DENIED                      ((NTSTATUS)0xC0000022)
#define STATUS_INSUFFICIENT_RESOURCES             ((NTSTATUS)0xC000009A)
#define STATUS_CANT_WAIT                          ((NTSTATUS)0xC00000D8)
#define STATUS_INVALID_DEVICE_REQUEST             ((NTSTATUS)0xC0000010)

/* Resources (ownership-checked) ------------------------------------------- */
typedef struct _ERESOURCE
{
    int Exclusive;
    int Shared;
} ERESOURCE, *PERESOURCE;

BOOLEAN ExAcquireResourceExclusiveLite(PERESOURCE Resource, BOOLEAN Wait);
BOOLEAN ExAcquireResourceSharedLite(PERESOURCE Resource, BOOLEAN Wait);
BOOLEAN ExAcquireSharedWaitForExclusive(PERESOURCE Resource, BOOLEAN Wait);
BOOLEAN ExIsResourceAcquiredSharedLite(PERESOURCE Resource);
void ExConvertExclusiveToSharedLite(PERESOURCE Resource);
void ExReleaseResourceLite(PERESOURCE Resource);

void FsRtlEnterFileSystem(void);
void FsRtlExitFileSystem(void);

typedef enum _POOL_TYPE { NonPagedPool = 0, PagedPool = 1 } POOL_TYPE;
PVOID ExAllocatePoolWithTag(int PoolType, size_t Size, ULONG Tag);

/* FCB header ---------------------------------------------------------------- */
#define FSRTL_FLAG_ACQUIRE_MAIN_RSRC_EX 0x08
#define FSRTL_FLAG_ACQUIRE_MAIN_RSRC_SH 0x10
typedef struct _FSRTL_COMMON_FCB_HEADER
{
    UCHAR Flags;
    PERESOURCE Resource;
    PERESOURCE PagingIoResource;
    LARGE_INTEGER FileSize;
    LARGE_INTEGER ValidDataLength;
} FSRTL_COMMON_FCB_HEADER, *PFSRTL_COMMON_FCB_HEADER;

/* Objects -------------------------------------------------------------------- */
struct _DEVICE_OBJECT;
struct _FILE_OBJECT;
struct _FS_FILTER_CALLBACKS;

typedef struct _FAST_IO_DISPATCH
{
    void (NTAPI *AcquireFileForNtCreateSection)(struct _FILE_OBJECT *FileObject);
    void (NTAPI *ReleaseFileForNtCreateSection)(struct _FILE_OBJECT *FileObject);
    NTSTATUS (NTAPI *AcquireForModWrite)(struct _FILE_OBJECT *, PLARGE_INTEGER, PERESOURCE *, struct _DEVICE_OBJECT *);
    NTSTATUS (NTAPI *ReleaseForModWrite)(struct _FILE_OBJECT *, PERESOURCE, struct _DEVICE_OBJECT *);
    NTSTATUS (NTAPI *AcquireForCcFlush)(struct _FILE_OBJECT *, struct _DEVICE_OBJECT *);
    NTSTATUS (NTAPI *ReleaseForCcFlush)(struct _FILE_OBJECT *, struct _DEVICE_OBJECT *);
} FAST_IO_DISPATCH, *PFAST_IO_DISPATCH;

typedef struct _EXTENDED_DRIVER_EXTENSION
{
    struct _FS_FILTER_CALLBACKS *FsFilterCallbacks;
} EXTENDED_DRIVER_EXTENSION, *PEXTENDED_DRIVER_EXTENSION;

typedef struct _DRIVER_OBJECT
{
    PFAST_IO_DISPATCH FastIoDispatch;
    EXTENDED_DRIVER_EXTENSION Extension;   /* host: what IoGetDrvObjExtension returns */
} DRIVER_OBJECT, *PDRIVER_OBJECT;

typedef struct _DEVICE_OBJECT
{
    PDRIVER_OBJECT DriverObject;
    struct _DEVICE_OBJECT *AttachedTo;     /* host: the device below */
    LONG RefCount;                         /* host: Ob reference balance */
    const char *Name;                      /* host: for the call log */
} DEVICE_OBJECT, *PDEVICE_OBJECT;

typedef struct _FILE_OBJECT
{
    PVOID FsContext;
    PDEVICE_OBJECT TopDevice;              /* host: IoGetRelatedDeviceObject */
    PDEVICE_OBJECT BaseDevice;             /* host: IoGetBaseFileSystemDeviceObject */
} FILE_OBJECT, *PFILE_OBJECT;

#define IoGetDrvObjExtension(DriverObject) (&(DriverObject)->Extension)
PDEVICE_OBJECT IoGetRelatedDeviceObject(PFILE_OBJECT FileObject);
PDEVICE_OBJECT IoGetLowerDeviceObject(PDEVICE_OBJECT DeviceObject);
PDEVICE_OBJECT IoGetBaseFileSystemDeviceObject(PFILE_OBJECT FileObject);
void ObReferenceObject(PVOID Object);
void ObDereferenceObject(PVOID Object);

/* FS filter callbacks (as ntifs.h / iotypes.h define them) ------------------ */
typedef enum _FS_FILTER_SECTION_SYNC_TYPE { SyncTypeOther = 0, SyncTypeCreateSection } FS_FILTER_SECTION_SYNC_TYPE;
typedef enum _FS_FILTER_STREAM_FO_NOTIFICATION_TYPE { NotifyTypeCreate = 0, NotifyTypeRetired } FS_FILTER_STREAM_FO_NOTIFICATION_TYPE;
typedef int FILE_INFORMATION_CLASS;
typedef struct _IRP *PIRP;
typedef struct _FS_FILTER_SECTION_SYNC_OUTPUT *PFS_FILTER_SECTION_SYNC_OUTPUT;

typedef union _FS_FILTER_PARAMETERS
{
    struct { PLARGE_INTEGER EndingOffset; PERESOURCE *ResourceToRelease; } AcquireForModifiedPageWriter;
    struct { PERESOURCE ResourceToRelease; } ReleaseForModifiedPageWriter;
    struct { FS_FILTER_SECTION_SYNC_TYPE SyncType; ULONG PageProtection; PFS_FILTER_SECTION_SYNC_OUTPUT OutputInformation; } AcquireForSectionSynchronization;
    struct { FS_FILTER_STREAM_FO_NOTIFICATION_TYPE NotificationType; BOOLEAN POINTER_ALIGNMENT SafeToRecurse; } NotifyStreamFileObject;
    struct { PIRP Irp; PVOID FileInformation; PULONG Length; FILE_INFORMATION_CLASS FileInformationClass; NTSTATUS CompletionStatus; } QueryOpen;
    struct { PVOID Argument1, Argument2, Argument3, Argument4, Argument5; } Others;
} FS_FILTER_PARAMETERS, *PFS_FILTER_PARAMETERS;

#define FS_FILTER_ACQUIRE_FOR_SECTION_SYNCHRONIZATION (UCHAR)-1
#define FS_FILTER_RELEASE_FOR_SECTION_SYNCHRONIZATION (UCHAR)-2
#define FS_FILTER_ACQUIRE_FOR_MOD_WRITE               (UCHAR)-3
#define FS_FILTER_RELEASE_FOR_MOD_WRITE               (UCHAR)-4
#define FS_FILTER_ACQUIRE_FOR_CC_FLUSH                (UCHAR)-5
#define FS_FILTER_RELEASE_FOR_CC_FLUSH                (UCHAR)-6

typedef struct _FS_FILTER_CALLBACK_DATA
{
    ULONG SizeOfFsFilterCallbackData;
    UCHAR Operation;
    UCHAR Reserved;
    struct _DEVICE_OBJECT *DeviceObject;
    struct _FILE_OBJECT *FileObject;
    FS_FILTER_PARAMETERS Parameters;
} FS_FILTER_CALLBACK_DATA, *PFS_FILTER_CALLBACK_DATA;

typedef NTSTATUS (NTAPI *PFS_FILTER_CALLBACK)(PFS_FILTER_CALLBACK_DATA Data, PVOID *CompletionContext);
typedef void (NTAPI *PFS_FILTER_COMPLETION_CALLBACK)(PFS_FILTER_CALLBACK_DATA Data, NTSTATUS OperationStatus, PVOID CompletionContext);

typedef struct _FS_FILTER_CALLBACKS
{
    ULONG SizeOfFsFilterCallbacks;
    ULONG Reserved;
    PFS_FILTER_CALLBACK PreAcquireForSectionSynchronization;
    PFS_FILTER_COMPLETION_CALLBACK PostAcquireForSectionSynchronization;
    PFS_FILTER_CALLBACK PreReleaseForSectionSynchronization;
    PFS_FILTER_COMPLETION_CALLBACK PostReleaseForSectionSynchronization;
    PFS_FILTER_CALLBACK PreAcquireForCcFlush;
    PFS_FILTER_COMPLETION_CALLBACK PostAcquireForCcFlush;
    PFS_FILTER_CALLBACK PreReleaseForCcFlush;
    PFS_FILTER_COMPLETION_CALLBACK PostReleaseForCcFlush;
    PFS_FILTER_CALLBACK PreAcquireForModifiedPageWriter;
    PFS_FILTER_COMPLETION_CALLBACK PostAcquireForModifiedPageWriter;
    PFS_FILTER_CALLBACK PreReleaseForModifiedPageWriter;
    PFS_FILTER_COMPLETION_CALLBACK PostReleaseForModifiedPageWriter;
    PFS_FILTER_CALLBACK PreQueryOpen;
    PFS_FILTER_COMPLETION_CALLBACK PostQueryOpen;
} FS_FILTER_CALLBACKS, *PFS_FILTER_CALLBACKS;

/* The FsRtl API under test --------------------------------------------------- */
NTSTATUS NTAPI FsRtlAcquireFileExclusiveCommon(PFILE_OBJECT FileObject, FS_FILTER_SECTION_SYNC_TYPE SyncType, ULONG Reserved);
void NTAPI FsRtlAcquireFileExclusive(PFILE_OBJECT FileObject);
NTSTATUS NTAPI FsRtlAcquireToCreateMappedSection(PFILE_OBJECT FileObject, ULONG SectionPageProtection);
void NTAPI FsRtlReleaseFile(PFILE_OBJECT FileObject);
NTSTATUS NTAPI FsRtlAcquireFileForCcFlushEx(PFILE_OBJECT FileObject);
void NTAPI FsRtlAcquireFileForCcFlush(PFILE_OBJECT FileObject);
void NTAPI FsRtlReleaseFileForCcFlush(PFILE_OBJECT FileObject);
NTSTATUS NTAPI FsRtlAcquireFileForModWriteEx(PFILE_OBJECT FileObject, PLARGE_INTEGER EndingOffset, PERESOURCE *ResourceToRelease);
void NTAPI FsRtlReleaseFileForModWrite(PFILE_OBJECT FileObject, PERESOURCE ResourceToRelease);
NTSTATUS NTAPI FsRtlRegisterFileSystemFilterCallbacks(PDRIVER_OBJECT FilterDriverObject, PFS_FILTER_CALLBACKS Callbacks);

#endif /* FSFLTCB_HOST_NTOSKRNL_H */
