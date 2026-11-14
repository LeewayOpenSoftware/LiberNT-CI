/*
 * PROJECT:     ReactOS NT Virtual Memory Subsystem
 * FILE:        ntoskrnl/nvs/nt/ntdrvmgmt.c
 * PURPOSE:     Driver memory management interfaces
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#include <nvs/nt/mint.h>

extern KMUTANT MmSystemLoadLock;
extern UCHAR MmDisablePagingExecutive;
PLDR_DATA_TABLE_ENTRY NTAPI MiLookupDataTableEntry(IN PVOID Address);

MM_DRIVER_VERIFIER_DATA MmVerifierData;
LIST_ENTRY MiVerifierDriverAddedThunkListHead;
ULONG MiActiveVerifierThunks;
WCHAR MmVerifyDriverBuffer[512] = {0};
ULONG MmVerifyDriverBufferLength = sizeof(MmVerifyDriverBuffer);
ULONG MmVerifyDriverBufferType = REG_NONE;
ULONG MmVerifyDriverLevel = -1;
PVOID MmTriageActionTaken;
PVOID KernelVerifier;

#define MI_VERIFIER_DEFAULT_LEVEL (DRIVER_VERIFIER_SPECIAL_POOLING | DRIVER_VERIFIER_TRACK_POOL_ALLOCATIONS)
#define MI_VERIFIER_IMPLEMENTED_LEVEL \
    (MI_VERIFIER_DEFAULT_LEVEL | DRIVER_VERIFIER_IO_CHECKING | DRIVER_VERIFIER_DEADLOCK_DETECTION)

static BOOLEAN MiVerifierActive;

VOID
NTAPI
MmUnlockPageableImageSection(IN PVOID ImageSectionHandle)
{
    UNREFERENCED_PARAMETER(ImageSectionHandle);
    ASSERT(MmDisablePagingExecutive);
}

VOID
NTAPI
MmLockPageableSectionByHandle(IN PVOID ImageSectionHandle)
{
    UNREFERENCED_PARAMETER(ImageSectionHandle);
    ASSERT(MmDisablePagingExecutive);
}

PVOID
NTAPI
MmLockPageableDataSection(IN PVOID AddressWithinSection)
{
    ASSERT(MmDisablePagingExecutive);
    return AddressWithinSection;
}

ULONG
NTAPI
MmTrimAllSystemPageableMemory(IN ULONG PurgeTransitionList)
{
    UNIMPLEMENTED;
    return 0;
}

NTSTATUS
NTAPI
MmAddVerifierThunks(IN PVOID ThunkBuffer,
                    IN ULONG ThunkBufferSize)
{
    PDRIVER_VERIFIER_THUNK_PAIRS ThunkTable;
    ULONG ThunkCount;
    PDRIVER_SPECIFIED_VERIFIER_THUNKS DriverThunks;
    PLDR_DATA_TABLE_ENTRY LdrEntry;
    PVOID ModuleBase, ModuleEnd;
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;
    PAGED_CODE();

    if (!MiVerifierDriverAddedThunkListHead.Flink) return STATUS_NOT_SUPPORTED;

    ThunkCount = ThunkBufferSize / sizeof(DRIVER_VERIFIER_THUNK_PAIRS);
    if (!ThunkCount) return STATUS_INVALID_PARAMETER_1;

    DriverThunks = ExAllocatePoolWithTag(PagedPool,
                                         sizeof(*DriverThunks) +
                                         ThunkCount *
                                         sizeof(DRIVER_VERIFIER_THUNK_PAIRS),
                                         'tVmM');
    if (!DriverThunks) return STATUS_INSUFFICIENT_RESOURCES;

    ThunkTable = (PDRIVER_VERIFIER_THUNK_PAIRS)(DriverThunks + 1);
    RtlCopyMemory(ThunkTable,
                  ThunkBuffer,
                  ThunkCount * sizeof(DRIVER_VERIFIER_THUNK_PAIRS));

    KeEnterCriticalRegion();
    KeWaitForSingleObject(&MmSystemLoadLock,
                          WrVirtualMemory,
                          KernelMode,
                          FALSE,
                          NULL);

    LdrEntry = MiLookupDataTableEntry(ThunkTable->PristineRoutine);
    if (!LdrEntry)
    {
        Status = STATUS_INVALID_PARAMETER_2;
        goto Cleanup;
    }

    ModuleBase = LdrEntry->DllBase;
    ModuleEnd = (PVOID)((ULONG_PTR)LdrEntry->DllBase + LdrEntry->SizeOfImage);

    if (LdrEntry->LoadedImports == (PVOID)(ULONG_PTR)-1)
    {
        Status = STATUS_INVALID_PARAMETER_2;
        goto Cleanup;
    }

    for (i = 0; i < ThunkCount; i++)
    {
        if (((ULONG_PTR)ThunkTable[i].PristineRoutine < (ULONG_PTR)ModuleBase) ||
            ((ULONG_PTR)ThunkTable[i].PristineRoutine >= (ULONG_PTR)ModuleEnd))
        {
            Status = STATUS_INVALID_PARAMETER_2;
            goto Cleanup;
        }
    }

    DriverThunks->DataTableEntry = LdrEntry;
    DriverThunks->NumberOfThunks = ThunkCount;
    MiActiveVerifierThunks++;
    InsertTailList(&MiVerifierDriverAddedThunkListHead,
                   &DriverThunks->ListEntry);
    DriverThunks = NULL;

Cleanup:

    KeReleaseMutant(&MmSystemLoadLock, MUTANT_INCREMENT, FALSE, FALSE);
    KeLeaveCriticalRegion();

    if (DriverThunks) ExFreePoolWithTag(DriverThunks, 'tVmM');
    return Status;
}

LOGICAL
NTAPI
MmIsDriverVerifying(IN PDRIVER_OBJECT DriverObject)
{
    PLDR_DATA_TABLE_ENTRY LdrEntry;

    LdrEntry = (PLDR_DATA_TABLE_ENTRY)DriverObject->DriverSection;
    if (!LdrEntry) return FALSE;

    return (LdrEntry->Flags & LDRP_IMAGE_VERIFYING) ? TRUE: FALSE;
}

LOGICAL
NTAPI
MmIsDriverVerifyingByAddress(
    _In_ PVOID AddressWithinSection)
{
    PLDR_DATA_TABLE_ENTRY LdrEntry = MiLookupDataTableEntry(AddressWithinSection);

    return (LdrEntry != NULL && (LdrEntry->Flags & LDRP_IMAGE_VERIFYING)) ? TRUE : FALSE;
}

NTSTATUS
NTAPI
MmIsVerifierEnabled(OUT PULONG VerifierFlags)
{
    if (MiVerifierActive)
    {
        *VerifierFlags = MmVerifierData.Level;
        return STATUS_SUCCESS;
    }

    *VerifierFlags = 0;
    return STATUS_NOT_SUPPORTED;
}

VOID
NTAPI
MiInitializeDriverVerifier(VOID)
{
    ULONG Level;

    MmVerifyDriverBuffer[RTL_NUMBER_OF(MmVerifyDriverBuffer) - 1] = UNICODE_NULL;
    if (MmVerifyDriverBuffer[0] == UNICODE_NULL)
        return;

    Level = (MmVerifyDriverLevel == (ULONG)-1) ? MI_VERIFIER_DEFAULT_LEVEL : MmVerifyDriverLevel;
    Level &= MI_VERIFIER_IMPLEMENTED_LEVEL;
    if (!MiIsSpecialPoolEnabled())
        Level &= ~DRIVER_VERIFIER_SPECIAL_POOLING;

    if (Level == 0)
    {
        DbgPrint("Driver Verifier: not enabled, no requested option is available\n");
        return;
    }

    if (Level & DRIVER_VERIFIER_DEADLOCK_DETECTION)
        Level |= DRIVER_VERIFIER_TRACK_POOL_ALLOCATIONS;

    if (Level & DRIVER_VERIFIER_TRACK_POOL_ALLOCATIONS)
        VfInitializePoolTracking();

    if (Level & DRIVER_VERIFIER_IO_CHECKING)
        VfInitializeIrpTracking();

    if (Level & DRIVER_VERIFIER_DEADLOCK_DETECTION)
        VfInitializeDeadlockDetection();

    MmVerifierData.Level = Level;
    MiVerifierActive = TRUE;
    DbgPrint("Driver Verifier: level 0x%lx for \"%ws\"\n", Level, MmVerifyDriverBuffer);
}

static
BOOLEAN
MiIsVerifierSeparator(
    _In_ WCHAR Character)
{
    return (BOOLEAN)(Character == L' ' || Character == L'\t' || Character == L',' || Character == L';');
}

static
BOOLEAN
MiIsVerifiedDriverName(
    _In_ PCUNICODE_STRING BaseName)
{
    PWCHAR Cursor = MmVerifyDriverBuffer;
    UNICODE_STRING Name;

    for (;;)
    {
        while (MiIsVerifierSeparator(*Cursor))
            Cursor++;

        if (*Cursor == UNICODE_NULL)
            return FALSE;

        Name.Buffer = Cursor;
        while (*Cursor != UNICODE_NULL && !MiIsVerifierSeparator(*Cursor))
            Cursor++;

        Name.Length = (USHORT)((Cursor - Name.Buffer) * sizeof(WCHAR));
        Name.MaximumLength = Name.Length;
        if (Name.Length == sizeof(WCHAR) && Name.Buffer[0] == L'*')
            return TRUE;

        if (RtlEqualUnicodeString(&Name, BaseName, TRUE))
            return TRUE;
    }
}

static
BOOLEAN
MiRedirectVerifierThunk(
    _Inout_ PIMAGE_THUNK_DATA Thunk,
    _In_ const VF_THUNK *Table,
    _In_ ULONG Count)
{
    ULONG i;

    for (i = 0; i < Count; i++)
    {
        if (Thunk->u1.Function == Table[i].Pristine)
        {
            Thunk->u1.Function = Table[i].Replacement;
            return TRUE;
        }
    }

    return FALSE;
}

VOID
NTAPI
MiVerifyLoadedImage(
    _In_ PLDR_DATA_TABLE_ENTRY LdrEntry)
{
    PIMAGE_IMPORT_DESCRIPTOR Import;
    PIMAGE_THUNK_DATA Thunk;
    ULONG Redirected = 0;
    ULONG Size;

    if (!MiVerifierActive || !MiIsVerifiedDriverName(&LdrEntry->BaseDllName))
        return;

    Import = RtlImageDirectoryEntryToData(LdrEntry->DllBase, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &Size);
    if (Import != NULL)
    {
        for (; Import->Name != 0 && Import->FirstThunk != 0; Import++)
        {
            Thunk = (PIMAGE_THUNK_DATA)((ULONG_PTR)LdrEntry->DllBase + Import->FirstThunk);
            for (; Thunk->u1.Function != 0; Thunk++)
            {
                if (MiRedirectVerifierThunk(Thunk, VfPoolThunks, VfPoolThunkCount) ||
                    MiRedirectVerifierThunk(Thunk, VfIrqlThunks, VfIrqlThunkCount) ||
                    ((MmVerifierData.Level & DRIVER_VERIFIER_IO_CHECKING) &&
                     MiRedirectVerifierThunk(Thunk, VfIoThunks, VfIoThunkCount)))
                {
                    Redirected++;
                }
            }
        }
    }

    LdrEntry->Flags |= LDRP_IMAGE_VERIFYING;
    InterlockedIncrement((PLONG)&MmVerifierData.Loads);
    DbgPrint("Driver Verifier: %wZ is verified, %lu imports redirected\n", &LdrEntry->BaseDllName, Redirected);
}
