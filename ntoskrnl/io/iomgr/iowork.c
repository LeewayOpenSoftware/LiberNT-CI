/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/io/iomgr/iowork.c
 * PURPOSE:         I/O Wrappers for the Executive Work Item Functions
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 *                  Robert Dickenson (odin@pnc.com.au)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PRIVATE FUNCTIONS *********************************************************/

VOID
NTAPI
IopWorkItemCallback(IN PVOID Parameter)
{
    PIO_WORKITEM IoWorkItem = (PIO_WORKITEM)Parameter;
    PDEVICE_OBJECT DeviceObject = IoWorkItem->DeviceObject;
    PIO_WORKITEM_ROUTINE WorkerRoutine = IoWorkItem->WorkerRoutine;
    PIO_WORKITEM_ROUTINE_EX WorkerRoutineEx = IoWorkItem->WorkerRoutineEx;
    PVOID Context = IoWorkItem->Context;
    PAGED_CODE();

    /* The item is off the queue: from here on it may be queued again, or freed by its routine */
    InterlockedExchange(&IoWorkItem->Queued, 0);

    /* Call the work routine */
    if (WorkerRoutineEx != NULL)
        WorkerRoutineEx(DeviceObject, Context, IoWorkItem);
    else
        WorkerRoutine(DeviceObject, Context);

    /* Dereference the device object */
    ObDereferenceObject(DeviceObject);
}

/* PUBLIC FUNCTIONS **********************************************************/

ULONG
NTAPI
IoSizeofWorkItem(VOID)
{
#if defined(_M_ARM64)
    /* Windows 11 ARM64 exposes an 0x58-byte caller-owned work item. */
    return 0x58;
#else
    return sizeof(IO_WORKITEM);
#endif
}

VOID
NTAPI
IoInitializeWorkItem(
    _In_ PVOID IoObject,
    _Out_ PIO_WORKITEM IoWorkItem)
{
    RtlZeroMemory(IoWorkItem, sizeof(IO_WORKITEM));
    IoWorkItem->DeviceObject = (PDEVICE_OBJECT)IoObject;
    ExInitializeWorkItem(&IoWorkItem->Item, IopWorkItemCallback, IoWorkItem);
}

/*
 * @implemented
 */
VOID
NTAPI
IoUninitializeWorkItem(
    _Inout_ PIO_WORKITEM IoWorkItem)
{
    RtlZeroMemory(IoWorkItem, sizeof(IO_WORKITEM));
}

/*
 * @implemented
 */
VOID
NTAPI
IoQueueWorkItem(IN PIO_WORKITEM IoWorkItem,
                IN PIO_WORKITEM_ROUTINE WorkerRoutine,
                IN WORK_QUEUE_TYPE QueueType,
                IN PVOID Context)
{
    /* Make sure we're called at DISPATCH or lower */
    ASSERT_IRQL_LESS_OR_EQUAL(DISPATCH_LEVEL);

    /* Reference the device object */
    ObReferenceObject(IoWorkItem->DeviceObject);

    /* Setup the work item */
    IoWorkItem->WorkerRoutine = WorkerRoutine;
    IoWorkItem->WorkerRoutineEx = NULL;
    IoWorkItem->Context = Context;
    InterlockedExchange(&IoWorkItem->Queued, 1);

    /* Queue the work item */
    ExQueueWorkItem(&IoWorkItem->Item, QueueType);
}

/*
 * @implemented
 */
VOID
NTAPI
IoFreeWorkItem(IN PIO_WORKITEM IoWorkItem)
{
    /* Free the work item */
    ExFreePoolWithTag(IoWorkItem, TAG_IOWI);
}

/*
 * @implemented
 */
PIO_WORKITEM
NTAPI
IoAllocateWorkItem(IN PDEVICE_OBJECT DeviceObject)
{
    PIO_WORKITEM IoWorkItem;

    /* Allocate the work item */
    IoWorkItem = ExAllocatePoolWithTag(NonPagedPool,
                                       sizeof(IO_WORKITEM),
                                       TAG_IOWI);
    if (!IoWorkItem) return NULL;

    /* Initialize it */
    IoWorkItem->DeviceObject = DeviceObject;
    IoWorkItem->WorkerRoutineEx = NULL;
    IoWorkItem->Queued = 0;
    ExInitializeWorkItem(&IoWorkItem->Item, IopWorkItemCallback, IoWorkItem);

    /* Return it */
    return IoWorkItem;
}

/*
 * Queues the work item unless it is already waiting in a queue.
 * Returns TRUE if this call queued it.
 */
BOOLEAN
NTAPI
IoTryQueueWorkItem(
    _Inout_ PIO_WORKITEM IoWorkItem,
    _In_ PIO_WORKITEM_ROUTINE_EX WorkerRoutine,
    _In_ WORK_QUEUE_TYPE QueueType,
    _In_opt_ PVOID Context)
{
    ASSERT_IRQL_LESS_OR_EQUAL(DISPATCH_LEVEL);

    /* Only one caller takes the item from idle to queued */
    if (InterlockedCompareExchange(&IoWorkItem->Queued, 1, 0) != 0)
        return FALSE;

    ObReferenceObject(IoWorkItem->DeviceObject);

    IoWorkItem->WorkerRoutine = NULL;
    IoWorkItem->WorkerRoutineEx = WorkerRoutine;
    IoWorkItem->Context = Context;

    ExQueueWorkItem(&IoWorkItem->Item, QueueType);
    return TRUE;
}

/* EOF */
