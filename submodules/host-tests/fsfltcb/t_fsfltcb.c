/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     File system filter callbacks around the FsRtl acquire/release
 *              entry points (ntoskrnl/fsrtl/fsfltcb.c, built unchanged): every
 *              filter of the stack is called, Pre top-down before the file
 *              system lock and Post bottom-up after it with its status, contexts
 *              start at NULL, a failing Pre unwinds, a filter can complete the
 *              operation itself, success statuses from Pre never skip the lock,
 *              the CcFlush and ModWrite pairs are dispatched, a short
 *              registration is never read past its size.
 *              One case per process: `t_fsfltcb <case>`; no argument runs all.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include <ntoskrnl.h>
#include <stdio.h>
#include <stdlib.h>

static int Failures;
#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            Failures++;                                                         \
        }                                                                       \
    } while (0)

#define CHECK_LOG(expected)                                                     \
    do {                                                                        \
        if (strcmp(Log, (expected)) != 0) {                                     \
            fprintf(stderr, "  FAIL %s:%d: log\n    got:  %s\n    want: %s\n",  \
                    __FILE__, __LINE__, Log, (expected));                       \
            Failures++;                                                         \
        }                                                                       \
    } while (0)

static void Fatal(const char *What)
{
    fprintf(stderr, "FATAL (kernel contract): %s\n", What);
    fflush(stderr);
    abort();
}

/* Call log --------------------------------------------------------------------- */
static char Log[4096];

static void Note(const char *Token)
{
    if (Log[0] != '\0')
        strncat(Log, " ", sizeof(Log) - strlen(Log) - 1);
    strncat(Log, Token, sizeof(Log) - strlen(Log) - 1);
}

/* Resources, critical region, pool ------------------------------------------- */
static ERESOURCE MainResource, PagingResource;
static int CriticalRegion;

static const char *ResName(PERESOURCE Resource)
{
    return Resource == &MainResource ? "R" : Resource == &PagingResource ? "P" : "?";
}

BOOLEAN ExAcquireResourceExclusiveLite(PERESOURCE Resource, BOOLEAN Wait)
{
    char T[8];
    (void)Wait;
    if (Resource->Exclusive || Resource->Shared)
        Fatal("exclusive acquire of a held resource would block forever");
    Resource->Exclusive = 1;
    snprintf(T, sizeof(T), "%s+x", ResName(Resource));
    Note(T);
    return TRUE;
}

BOOLEAN ExAcquireResourceSharedLite(PERESOURCE Resource, BOOLEAN Wait)
{
    char T[8];
    (void)Wait;
    if (Resource->Exclusive)
        Resource->Exclusive++;
    else
        Resource->Shared++;
    snprintf(T, sizeof(T), "%s+s", ResName(Resource));
    Note(T);
    return TRUE;
}

BOOLEAN ExAcquireSharedWaitForExclusive(PERESOURCE Resource, BOOLEAN Wait)
{
    return ExAcquireResourceSharedLite(Resource, Wait);
}

BOOLEAN ExIsResourceAcquiredSharedLite(PERESOURCE Resource)
{
    return (BOOLEAN)(Resource->Exclusive || Resource->Shared);
}

void ExConvertExclusiveToSharedLite(PERESOURCE Resource)
{
    if (Resource->Exclusive != 1)
        Fatal("convert of a resource not held exclusive");
    Resource->Exclusive = 0;
    Resource->Shared = 1;
}

void ExReleaseResourceLite(PERESOURCE Resource)
{
    char T[8];
    if (Resource->Exclusive)
        Resource->Exclusive--;
    else if (Resource->Shared)
        Resource->Shared--;
    else
        Fatal("release of a resource that is not held");
    snprintf(T, sizeof(T), "%s-", ResName(Resource));
    Note(T);
}

void FsRtlEnterFileSystem(void)
{
    CriticalRegion++;
}

void FsRtlExitFileSystem(void)
{
    if (CriticalRegion == 0)
        Fatal("FsRtlExitFileSystem without FsRtlEnterFileSystem");
    CriticalRegion--;
}

/* Allocations carry a tail of poison function pointers: a read of a callback
 * slot past the size a filter registered calls one and faults. */
#define POOL_TAIL 256
static void *Allocations[64];
static int AllocationCount;

static NTSTATUS NTAPI PoisonCallback(void)
{
    Fatal("callback read past the registered SizeOfFsFilterCallbacks");
    return STATUS_UNSUCCESSFUL;
}

PVOID ExAllocatePoolWithTag(int PoolType, size_t Size, ULONG Tag)
{
    unsigned char *P;
    size_t i;
    (void)PoolType;
    (void)Tag;
    P = malloc(Size + POOL_TAIL);
    if (P == NULL || AllocationCount == 64)
        Fatal("out of host memory");
    for (i = 0; i + sizeof(void *) <= POOL_TAIL; i += sizeof(void *))
    {
        void (*Poison)(void) = (void (*)(void))PoisonCallback;
        memcpy(P + Size + i, &Poison, sizeof(Poison));
    }
    Allocations[AllocationCount++] = P;
    return P;
}

static void FreeAllocations(void)
{
    while (AllocationCount)
        free(Allocations[--AllocationCount]);
}

/* Device stack: A (top filter) -> B (filter) -> FS (file system) ------------- */
static DRIVER_OBJECT DriverA, DriverB, DriverFs;
static DEVICE_OBJECT DevA, DevB, DevFs;
static FSRTL_COMMON_FCB_HEADER Fcb;
static FILE_OBJECT File;

PDEVICE_OBJECT IoGetRelatedDeviceObject(PFILE_OBJECT FileObject)
{
    return FileObject->TopDevice;
}

PDEVICE_OBJECT IoGetLowerDeviceObject(PDEVICE_OBJECT DeviceObject)
{
    if (DeviceObject->AttachedTo)
        ObReferenceObject(DeviceObject->AttachedTo);
    return DeviceObject->AttachedTo;
}

PDEVICE_OBJECT IoGetBaseFileSystemDeviceObject(PFILE_OBJECT FileObject)
{
    return FileObject->BaseDevice;
}

void ObReferenceObject(PVOID Object)
{
    ((PDEVICE_OBJECT)Object)->RefCount++;
}

void ObDereferenceObject(PVOID Object)
{
    if (((PDEVICE_OBJECT)Object)->RefCount <= 0)
        Fatal("device object dereferenced below zero");
    ((PDEVICE_OBJECT)Object)->RefCount--;
}

/* Filter behaviour, per device ------------------------------------------------- */
typedef struct _FILTER_CONFIG
{
    NTSTATUS PreStatus;
    PVOID Context;
} FILTER_CONFIG;
static FILTER_CONFIG ConfigA, ConfigB;
static PVOID PostContextA, PostContextB;
static FS_FILTER_PARAMETERS PreParametersA;

static FILTER_CONFIG *ConfigOf(PDEVICE_OBJECT Device)
{
    return Device == &DevA ? &ConfigA : &ConfigB;
}

static char OpLetter(UCHAR Operation)
{
    switch (Operation)
    {
        case FS_FILTER_ACQUIRE_FOR_SECTION_SYNCHRONIZATION: return 's';
        case FS_FILTER_RELEASE_FOR_SECTION_SYNCHRONIZATION: return 'S';
        case FS_FILTER_ACQUIRE_FOR_CC_FLUSH: return 'c';
        case FS_FILTER_RELEASE_FOR_CC_FLUSH: return 'C';
        case FS_FILTER_ACQUIRE_FOR_MOD_WRITE: return 'm';
        case FS_FILTER_RELEASE_FOR_MOD_WRITE: return 'M';
        default: return '?';
    }
}

static NTSTATUS NTAPI Pre(PFS_FILTER_CALLBACK_DATA Data, PVOID *CompletionContext)
{
    FILTER_CONFIG *Config = ConfigOf(Data->DeviceObject);
    char T[32];

    snprintf(T, sizeof(T), "%s.pre%c", Data->DeviceObject->Name, OpLetter(Data->Operation));
    Note(T);
    if (Data->DeviceObject == &DevA)
        PreParametersA = Data->Parameters;
    if (Config->Context != NULL)
        *CompletionContext = Config->Context;
    return Config->PreStatus;
}

static void NTAPI Post(PFS_FILTER_CALLBACK_DATA Data, NTSTATUS OperationStatus, PVOID CompletionContext)
{
    char T[48];

    snprintf(T, sizeof(T), "%s.post%c(%lx)", Data->DeviceObject->Name, OpLetter(Data->Operation),
             (unsigned long)(ULONG)OperationStatus);
    Note(T);
    if (Data->DeviceObject == &DevA)
        PostContextA = CompletionContext;
    else
        PostContextB = CompletionContext;
}

static FS_FILTER_CALLBACKS FullCallbacks = {
    sizeof(FS_FILTER_CALLBACKS), 0,
    Pre, Post, Pre, Post, Pre, Post, Pre, Post, Pre, Post, Pre, Post, NULL, NULL
};

static void Setup(BOOLEAN WithFilters)
{
    memset(Log, 0, sizeof(Log));
    memset(&MainResource, 0, sizeof(MainResource));
    memset(&PagingResource, 0, sizeof(PagingResource));
    CriticalRegion = 0;
    memset(&ConfigA, 0, sizeof(ConfigA));
    memset(&ConfigB, 0, sizeof(ConfigB));
    PostContextA = PostContextB = (PVOID)(uintptr_t)0xDEADBEEF;
    memset(&PreParametersA, 0, sizeof(PreParametersA));

    memset(&DriverA, 0, sizeof(DriverA));
    memset(&DriverB, 0, sizeof(DriverB));
    memset(&DriverFs, 0, sizeof(DriverFs));
    DevA = (DEVICE_OBJECT){ &DriverA, &DevB, 0, "A" };
    DevB = (DEVICE_OBJECT){ &DriverB, &DevFs, 0, "B" };
    DevFs = (DEVICE_OBJECT){ &DriverFs, NULL, 0, "FS" };

    memset(&Fcb, 0, sizeof(Fcb));
    Fcb.Resource = &MainResource;
    Fcb.PagingIoResource = &PagingResource;
    File = (FILE_OBJECT){ &Fcb, &DevA, &DevFs };

    if (WithFilters)
    {
        CHECK(FsRtlRegisterFileSystemFilterCallbacks(&DriverA, &FullCallbacks) == STATUS_SUCCESS);
        CHECK(FsRtlRegisterFileSystemFilterCallbacks(&DriverB, &FullCallbacks) == STATUS_SUCCESS);
    }
}

static void Balanced(void)
{
    CHECK(CriticalRegion == 0);
    CHECK(MainResource.Exclusive == 0 && MainResource.Shared == 0);
    CHECK(PagingResource.Exclusive == 0 && PagingResource.Shared == 0);
    CHECK(DevA.RefCount == 0 && DevB.RefCount == 0 && DevFs.RefCount == 0);
    FreeAllocations();
}

/* ------------------------------------------------------------------------ */

/* Every filter, Pre top-down, the lock, Post bottom-up with the status */
static void section_sync_calls_every_filter_in_order(void)
{
    Setup(TRUE);
    CHECK(FsRtlAcquireToCreateMappedSection(&File, 4) == STATUS_SUCCESS);
    CHECK_LOG("A.pres B.pres R+x B.posts(0) A.posts(0)");
    CHECK(CriticalRegion == 1);
    CHECK(PreParametersA.AcquireForSectionSynchronization.SyncType == SyncTypeCreateSection);
    CHECK(PreParametersA.AcquireForSectionSynchronization.PageProtection == 4);

    Log[0] = '\0';
    FsRtlReleaseFile(&File);
    CHECK_LOG("A.preS B.preS R- B.postS(0) A.postS(0)");
    Balanced();
}

/* A Pre that sets no context gets NULL in its Post, not stack garbage */
static void contexts_start_null(void)
{
    Setup(TRUE);
    ConfigA.Context = (PVOID)(uintptr_t)0xA1;
    CHECK(FsRtlAcquireToCreateMappedSection(&File, 4) == STATUS_SUCCESS);
    CHECK(PostContextA == (PVOID)(uintptr_t)0xA1);
    CHECK(PostContextB == NULL);
    FsRtlReleaseFile(&File);
    Balanced();
}

/* A failing Pre stops the acquire: the filters above see the failure, the
 * filter that failed and everything below are not called, nothing is held */
static void failing_pre_unwinds(void)
{
    Setup(TRUE);
    ConfigB.PreStatus = STATUS_ACCESS_DENIED;
    CHECK(FsRtlAcquireToCreateMappedSection(&File, 4) == STATUS_ACCESS_DENIED);
    CHECK_LOG("A.pres B.pres A.posts(c0000022)");
    Balanced();
}

/* A filter that performs the acquire itself: lower filters and the FS skipped */
static void filter_completes_operation(void)
{
    Setup(TRUE);
    ConfigA.PreStatus = STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY;
    CHECK(FsRtlAcquireToCreateMappedSection(&File, 4) == STATUS_SUCCESS);
    CHECK_LOG("A.pres A.posts(0)");
    CHECK(CriticalRegion == 1);
    CriticalRegion = 0;
    CHECK(DevA.RefCount == 0 && DevB.RefCount == 0);
    FreeAllocations();
}

/* A success status such as FILE_LOCKED_WITH_WRITERS from a Pre must not skip
 * the FS acquire: the caller will release the file afterwards */
static void informational_pre_status_still_locks(void)
{
    Setup(TRUE);
    ConfigA.PreStatus = STATUS_FILE_LOCKED_WITH_WRITERS;
    CHECK(FsRtlAcquireToCreateMappedSection(&File, 4) == STATUS_FILE_LOCKED_WITH_WRITERS);
    CHECK(MainResource.Exclusive == 1);
    FsRtlReleaseFile(&File);
    Balanced();
}

static void cc_flush_pair_is_dispatched(void)
{
    Setup(TRUE);
    CHECK(FsRtlAcquireFileForCcFlushEx(&File) == STATUS_SUCCESS);
    CHECK_LOG("A.prec B.prec R+x P+s B.postc(0) A.postc(0)");
    CHECK(CriticalRegion == 1);

    Log[0] = '\0';
    FsRtlReleaseFileForCcFlush(&File);
    CHECK_LOG("A.preC B.preC P- R- B.postC(0) A.postC(0)");
    Balanced();
}

static void cc_flush_failing_pre_fails_the_acquire(void)
{
    Setup(TRUE);
    ConfigB.PreStatus = STATUS_CANT_WAIT;
    CHECK(FsRtlAcquireFileForCcFlushEx(&File) == STATUS_CANT_WAIT);
    CHECK_LOG("A.prec B.prec A.postc(c00000d8)");
    Balanced();
}

static void mod_write_pair_is_dispatched(void)
{
    LARGE_INTEGER End = { 4096 };
    PERESOURCE ToRelease = NULL;

    Setup(TRUE);
    CHECK(FsRtlAcquireFileForModWriteEx(&File, &End, &ToRelease) == STATUS_SUCCESS);
    CHECK_LOG("A.prem B.prem P+s B.postm(0) A.postm(0)");
    CHECK(ToRelease == &PagingResource);
    CHECK(PreParametersA.AcquireForModifiedPageWriter.EndingOffset == &End);
    CHECK(PreParametersA.AcquireForModifiedPageWriter.ResourceToRelease == &ToRelease);

    Log[0] = '\0';
    FsRtlReleaseFileForModWrite(&File, ToRelease);
    CHECK_LOG("A.preM B.preM P- B.postM(0) A.postM(0)");
    CHECK(PreParametersA.ReleaseForModifiedPageWriter.ResourceToRelease == &PagingResource);
    Balanced();
}

/* A release cannot fail: a failing Pre is ignored and the file is released */
static void failing_release_pre_still_releases(void)
{
    Setup(TRUE);
    CHECK(FsRtlAcquireToCreateMappedSection(&File, 4) == STATUS_SUCCESS);
    ConfigA.PreStatus = STATUS_ACCESS_DENIED;
    FsRtlReleaseFile(&File);
    Balanced();
}

/* A filter that registered only the section synchronization callbacks: the
 * CcFlush and ModWrite slots past its size are not read */
static void short_registration_is_not_read_past(void)
{
    FS_FILTER_CALLBACKS Short = FullCallbacks;

    Setup(FALSE);
    Short.SizeOfFsFilterCallbacks = FIELD_OFFSET(FS_FILTER_CALLBACKS, PreAcquireForCcFlush);
    CHECK(FsRtlRegisterFileSystemFilterCallbacks(&DriverA, &Short) == STATUS_SUCCESS);
    CHECK(FsRtlAcquireFileForCcFlushEx(&File) == STATUS_SUCCESS);
    FsRtlReleaseFileForCcFlush(&File);
    CHECK(FsRtlAcquireToCreateMappedSection(&File, 4) == STATUS_SUCCESS);
    FsRtlReleaseFile(&File);
    CHECK_LOG("R+x P+s P- R- A.pres R+x A.posts(0) A.preS R- A.postS(0)");
    Balanced();
}

static void registration_rejects_a_size_without_callbacks(void)
{
    FS_FILTER_CALLBACKS Tiny = FullCallbacks;

    Setup(FALSE);
    Tiny.SizeOfFsFilterCallbacks = 4;
    CHECK(FsRtlRegisterFileSystemFilterCallbacks(&DriverA, &Tiny) == STATUS_INVALID_PARAMETER);
    CHECK(DriverA.Extension.FsFilterCallbacks == NULL);
    Balanced();
}

/* No filters: the plain FS behaviour is unchanged */
static void no_filters_plain_locking(void)
{
    LARGE_INTEGER End = { 4096 };
    PERESOURCE ToRelease = NULL;

    Setup(FALSE);
    CHECK(FsRtlAcquireToCreateMappedSection(&File, 4) == STATUS_SUCCESS);
    FsRtlReleaseFile(&File);
    CHECK(FsRtlAcquireFileForCcFlushEx(&File) == STATUS_SUCCESS);
    FsRtlReleaseFileForCcFlush(&File);
    CHECK(FsRtlAcquireFileForModWriteEx(&File, &End, &ToRelease) == STATUS_SUCCESS);
    FsRtlReleaseFileForModWrite(&File, ToRelease);
    CHECK_LOG("R+x R- R+x P+s P- R- P+s P-");
    Balanced();
}

/* ------------------------------------------------------------------------ */
static const struct
{
    const char *Name;
    void (*Run)(void);
} Cases[] = {
    { "section_sync_calls_every_filter_in_order", section_sync_calls_every_filter_in_order },
    { "contexts_start_null", contexts_start_null },
    { "failing_pre_unwinds", failing_pre_unwinds },
    { "filter_completes_operation", filter_completes_operation },
    { "informational_pre_status_still_locks", informational_pre_status_still_locks },
    { "cc_flush_pair_is_dispatched", cc_flush_pair_is_dispatched },
    { "cc_flush_failing_pre_fails_the_acquire", cc_flush_failing_pre_fails_the_acquire },
    { "mod_write_pair_is_dispatched", mod_write_pair_is_dispatched },
    { "failing_release_pre_still_releases", failing_release_pre_still_releases },
    { "short_registration_is_not_read_past", short_registration_is_not_read_past },
    { "registration_rejects_a_size_without_callbacks", registration_rejects_a_size_without_callbacks },
    { "no_filters_plain_locking", no_filters_plain_locking },
};

int main(int argc, char **argv)
{
    size_t i;
    int Ran = 0, Failed = 0;

    for (i = 0; i < sizeof(Cases) / sizeof(Cases[0]); i++)
    {
        int Before = Failures;
        if (argc > 1 && strcmp(argv[1], Cases[i].Name) != 0)
            continue;
        Cases[i].Run();
        Ran++;
        if (Failures != Before)
            Failed++;
        printf("%s %s\n", Failures != Before ? "FAIL" : "ok  ", Cases[i].Name);
    }

    if (Ran == 0)
    {
        fprintf(stderr, "unknown case %s\n", argc > 1 ? argv[1] : "");
        return 2;
    }
    printf("%d case(s), %d failed\n", Ran, Failed);
    return Failed ? 1 : 0;
}
