/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver Verifier: fault injection for tests, failed writes to a storage stack
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/*
 * For tests only. Nothing here runs unless the value VerifierFaultInjection
 * under "Session Manager\Memory Management" was not zero when the system
 * started, and a driver is verified with I/O verification.
 *
 * With that, a test asks for failed writes while the system runs. It sets two
 * values under the same key:
 *
 *   VerifierFailWritesDevice (REG_SZ)    name of a device object, for
 *                                        example \Device\HarddiskVolume2
 *   VerifierFailWrites       (REG_DWORD) how many writes fail; 0 withdraws a
 *                                        request that is not used up
 *
 * The key is read once a second. A request is taken once: the count value is
 * deleted when it is read. From then on, the next writes that a VERIFIED
 * driver sends with IoCallDriver to any device of the stack of the named
 * device are not sent. They are completed with STATUS_IO_DEVICE_ERROR, the
 * way a lower driver that fails the request at once would complete them. A
 * write sent by a driver that is not verified is not seen and not failed.
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define VF_FAULT_POLL_MILLISECONDS  1000
#define VF_FAULT_NAME_CHARS         260

ULONG VfFaultInjection;

static KSPIN_LOCK VfpFaultLock;
static PDEVICE_OBJECT VfpFaultBase;
static volatile LONG VfpFaultRemaining;
static LONG VfpFaultStarted;
static LONG VfpFaultPollQueued;
static KTIMER VfpFaultTimer;
static KDPC VfpFaultDpc;
static WORK_QUEUE_ITEM VfpFaultWorkItem;

static
VOID
VfpFaultArm(
    _In_opt_ PDEVICE_OBJECT Base,
    _In_ ULONG Count)
{
    PDEVICE_OBJECT Old;
    KIRQL OldIrql;

    KeAcquireSpinLock(&VfpFaultLock, &OldIrql);
    Old = VfpFaultBase;
    VfpFaultBase = Base;
    VfpFaultRemaining = (Base != NULL) ? (LONG)Count : 0;
    KeReleaseSpinLock(&VfpFaultLock, OldIrql);

    if (Old != NULL)
        ObDereferenceObject(Old);
}

static
VOID
VfpFaultTakeRequest(
    _In_ HANDLE Key)
{
    static UNICODE_STRING CountName = RTL_CONSTANT_STRING(L"VerifierFailWrites");
    static UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(L"VerifierFailWritesDevice");
    union
    {
        KEY_VALUE_PARTIAL_INFORMATION Info;
        UCHAR Bytes[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + VF_FAULT_NAME_CHARS * sizeof(WCHAR)];
    } Value;
    PDEVICE_OBJECT Device, Base;
    UNICODE_STRING Name;
    ULONG Length, Count;
    NTSTATUS Status;

    Status = ZwQueryValueKey(Key, &CountName, KeyValuePartialInformation, &Value, sizeof(Value), &Length);
    if (!NT_SUCCESS(Status))
        return;

    /* A request is taken once, whatever it holds */
    ZwDeleteValueKey(Key, &CountName);

    if (Value.Info.Type != REG_DWORD || Value.Info.DataLength != sizeof(ULONG))
    {
        DbgPrint("Driver Verifier (fault injection): VerifierFailWrites is not a REG_DWORD, ignored\n");
        return;
    }

    RtlCopyMemory(&Count, Value.Info.Data, sizeof(ULONG));
    if (Count == 0 || Count > MAXLONG)
    {
        VfpFaultArm(NULL, 0);
        DbgPrint("Driver Verifier (fault injection): no write fails any more\n");
        return;
    }

    Status = ZwQueryValueKey(Key, &DeviceName, KeyValuePartialInformation, &Value, sizeof(Value), &Length);
    if (!NT_SUCCESS(Status) || Value.Info.Type != REG_SZ || Value.Info.DataLength < 2 * sizeof(WCHAR))
    {
        DbgPrint("Driver Verifier (fault injection): VerifierFailWritesDevice is missing, too long or not a "
                 "REG_SZ (status 0x%08lx), request ignored\n", Status);
        return;
    }

    Name.Buffer = (PWSTR)Value.Info.Data;
    Name.Length = (USHORT)(Value.Info.DataLength & ~(sizeof(WCHAR) - 1));
    Name.MaximumLength = Name.Length;
    while (Name.Length != 0 && Name.Buffer[Name.Length / sizeof(WCHAR) - 1] == UNICODE_NULL)
        Name.Length -= sizeof(WCHAR);

    Status = ObReferenceObjectByName(&Name,
                                     OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                     NULL,
                                     0,
                                     IoDeviceObjectType,
                                     KernelMode,
                                     NULL,
                                     (PVOID *)&Device);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("Driver Verifier (fault injection): no device object \"%wZ\" (status 0x%08lx), request ignored\n",
                 &Name, Status);
        return;
    }

    /* The stack is known by its lowest device, which keeps a reference while
     * the request lasts */
    Base = IoGetDeviceAttachmentBaseRef(Device);
    ObDereferenceObject(Device);

    DbgPrint("Driver Verifier (fault injection): the next %lu write(s) that a verified driver sends to the stack "
             "of \"%wZ\" (lowest device %p) fail\n", Count, &Name, Base);
    VfpFaultArm(Base, Count);
}

static
VOID
NTAPI
VfpFaultPollWorker(
    _In_ PVOID Context)
{
    static UNICODE_STRING KeyName = RTL_CONSTANT_STRING(
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Session Manager\\Memory Management");
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Key;

    UNREFERENCED_PARAMETER(Context);

    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    if (NT_SUCCESS(ZwOpenKey(&Key, KEY_QUERY_VALUE | KEY_SET_VALUE, &ObjectAttributes)))
    {
        VfpFaultTakeRequest(Key);
        ZwClose(Key);
    }

    InterlockedExchange(&VfpFaultPollQueued, 0);
}

static
VOID
NTAPI
VfpFaultTimerDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /* The item is queued again only when the worker is done with it */
    if (InterlockedExchange(&VfpFaultPollQueued, 1) == 0)
        ExQueueWorkItem(&VfpFaultWorkItem, DelayedWorkQueue);
}

static
VOID
VfpFaultStart(VOID)
{
    LARGE_INTEGER DueTime;

    KeInitializeSpinLock(&VfpFaultLock);
    ExInitializeWorkItem(&VfpFaultWorkItem, VfpFaultPollWorker, NULL);
    KeInitializeDpc(&VfpFaultDpc, VfpFaultTimerDpc, NULL);
    KeInitializeTimerEx(&VfpFaultTimer, NotificationTimer);

    DueTime.QuadPart = -10000LL * VF_FAULT_POLL_MILLISECONDS;
    KeSetTimerEx(&VfpFaultTimer, DueTime, VF_FAULT_POLL_MILLISECONDS, &VfpFaultDpc);
    DbgPrint("Driver Verifier (fault injection): enabled, waiting for requests\n");
}

/*
 * Called for every request that a verified driver sends, before it is sent.
 * Returns TRUE if the request was a write that had to fail: it is completed
 * then, and *CallStatus is what the sender gets from IoCallDriver.
 */
BOOLEAN
NTAPI
VfFaultFailWrite(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ PVOID Caller,
    _Out_ PNTSTATUS CallStatus)
{
    PDEVICE_OBJECT Base, Release = NULL;
    PIO_STACK_LOCATION Stack;
    LARGE_INTEGER Offset;
    ULONG Length;
    LONG Left = 0;
    BOOLEAN Hit = FALSE;
    KIRQL OldIrql;

    /* The first request of a verified driver starts the reading of the key.
     * The lock is valid before anyone can find a count that is not zero. */
    if (VfpFaultStarted != 2)
    {
        if (InterlockedCompareExchange(&VfpFaultStarted, 1, 0) == 0)
        {
            VfpFaultStart();
            InterlockedExchange(&VfpFaultStarted, 2);
        }

        return FALSE;
    }

    if (VfpFaultRemaining == 0)
        return FALSE;

    /* The sender must have a location left for the device it sends to: the
     * call itself stops the system otherwise, and it must do so here too */
    Stack = IoGetNextIrpStackLocation(Irp);
    if (Stack->MajorFunction != IRP_MJ_WRITE || Irp->CurrentLocation <= 1)
        return FALSE;

    Base = IoGetDeviceAttachmentBaseRef(DeviceObject);

    KeAcquireSpinLock(&VfpFaultLock, &OldIrql);
    if (Base == VfpFaultBase && VfpFaultRemaining > 0)
    {
        Left = --VfpFaultRemaining;
        Hit = TRUE;
        if (Left == 0)
        {
            Release = VfpFaultBase;
            VfpFaultBase = NULL;
        }
    }
    KeReleaseSpinLock(&VfpFaultLock, OldIrql);

    ObDereferenceObject(Base);
    if (Release != NULL)
        ObDereferenceObject(Release);

    if (!Hit)
        return FALSE;

    Offset = Stack->Parameters.Write.ByteOffset;
    Length = Stack->Parameters.Write.Length;

    /* What IoCallDriver does before it calls the driver below */
    IoSetNextIrpStackLocation(Irp);
    Stack->DeviceObject = DeviceObject;

    DbgPrint("Driver Verifier (fault injection): write failed, device %p, offset 0x%I64x, length 0x%lx, IRP %p, "
             "caller %p, %ld more to fail\n", DeviceObject, Offset.QuadPart, Length, Irp, Caller, Left);

    /* What a driver below does when it fails the request at once */
    Irp->IoStatus.Status = STATUS_IO_DEVICE_ERROR;
    Irp->IoStatus.Information = 0;
    IofCompleteRequest(Irp, IO_NO_INCREMENT);

    *CallStatus = STATUS_IO_DEVICE_ERROR;
    return TRUE;
}
