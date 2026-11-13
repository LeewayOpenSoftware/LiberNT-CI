/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ke/krnlinit.c
 * PURPOSE:         Portable part of kernel initialization
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>
#include <internal/napi.h>

/* GLOBALS *******************************************************************/

#define KI_DPC_TIME_LIMIT_TICKS 0x500
#define KI_DPC_WATCHDOG_PERIOD_TICKS 0x1E00

/* Portable CPU Features and Flags */
USHORT KeProcessorArchitecture;
USHORT KeProcessorLevel;
USHORT KeProcessorRevision;
ULONG64 KeFeatureBits;

/* System call count */
ULONG KiServiceLimit = NUMBER_OF_SYSCALLS;

/* ARC Loader Block */
PLOADER_PARAMETER_BLOCK KeLoaderBlock;

/* PRCB Array */
PKPRCB KiProcessorBlock[MAXIMUM_PROCESSORS];

/* NUMA Node Support */
KNODE KiNode0;
PKNODE KeNodeBlock[KI_MAX_NUMA_NODES] = { &KiNode0 };
UCHAR KeNumberNodes = 1;
UCHAR KeProcessNodeSeed;

/* Initial Process and Thread */
ETHREAD KiInitialThread;
EPROCESS KiInitialProcess;

/* System-defined Spinlocks */
/* Still backs the LockQueueDispatcherLock slot used by KeConnectInterrupt/
 * KeDisconnectInterrupt (amd64/i386) to serialize IDT changes. The object-lock
 * rework moved dispatcher wait/signal paths off this lock, but those interrupt
 * routines still acquire it, so the slot must point at a real lock (not NULL). */
KSPIN_LOCK KiDispatcherLock;
KSPIN_LOCK MmPfnLock;
KSPIN_LOCK MmSystemSpaceLock;
KSPIN_LOCK CcBcbSpinLock;
KSPIN_LOCK CcMasterSpinLock;
KSPIN_LOCK CcVacbSpinLock;
KSPIN_LOCK CcWorkQueueSpinLock;
KSPIN_LOCK NonPagedPoolLock;
KSPIN_LOCK MmNonPagedPoolLock;
KSPIN_LOCK IopCancelSpinLock;
KSPIN_LOCK IopVpbSpinLock;
KSPIN_LOCK IopDatabaseLock;
KSPIN_LOCK IopCompletionLock;
KSPIN_LOCK NtfsStructLock;
KSPIN_LOCK AfdWorkQueueSpinLock;
KSPIN_LOCK KiTimerTableLock[LOCK_QUEUE_TIMER_TABLE_LOCKS];
KSPIN_LOCK KiReverseStallIpiLock;

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
KiInitSystem(VOID)
{
    ULONG i;

    /* Initialize Bugcheck Callback data */
    InitializeListHead(&KeBugcheckCallbackListHead);
    InitializeListHead(&KeBugcheckReasonCallbackListHead);
    KeInitializeSpinLock(&BugCheckCallbackLock);

    /* Initialize the Timer Expiration DPC */
    KeInitializeDpc(&KiTimerExpireDpc, KiTimerExpiration, NULL);
    KeSetTargetProcessorDpc(&KiTimerExpireDpc, 0);

    /* Initialize Profiling data */
    KeInitializeSpinLock(&KiProfileLock);
    InitializeListHead(&KiProfileListHead);
    InitializeListHead(&KiProfileSourceListHead);

    /* Loop the timer table */
    for (i = 0; i < TIMER_TABLE_SIZE; i++)
    {
        /* Initialize the list and entries */
        InitializeListHead(&KiTimerTableListHead[i].Entry);
        KiTimerTableListHead[i].Time.HighPart = 0xFFFFFFFF;
        KiTimerTableListHead[i].Time.LowPart = 0;
    }

    /* Initialize the Swap event and all swap lists */
    KeInitializeEvent(&KiSwapEvent, SynchronizationEvent, FALSE);
    InitializeListHead(&KiProcessInSwapListHead);
    InitializeListHead(&KiProcessOutSwapListHead);
    InitializeListHead(&KiStackInSwapListHead);

    /* Initialize the mutex for generic DPC calls */
    ExInitializeFastMutex(&KiGenericCallDpcMutex);

    /* Initialize the syscall table */
    KeServiceDescriptorTable[0].Base = MainSSDT;
    KeServiceDescriptorTable[0].Count = NULL;
    KeServiceDescriptorTable[0].Limit = KiServiceLimit;
    KeServiceDescriptorTable[1].Limit = 0;
    KeServiceDescriptorTable[0].Number = MainSSPT;

    /* Copy the the current table into the shadow table for win32k */
    RtlCopyMemory(KeServiceDescriptorTableShadow,
                  KeServiceDescriptorTable,
                  sizeof(KeServiceDescriptorTable));
}

CODE_SEG("INIT")
LARGE_INTEGER
NTAPI
KiComputeReciprocal(IN LONG Divisor,
                    OUT PUCHAR Shift)
{
    LARGE_INTEGER Reciprocal = {{0, 0}};
    LONG BitCount = 0, Remainder = 1;

    /* Start by calculating the remainder */
    while (Reciprocal.HighPart >= 0)
    {
        /* Increase the loop (bit) count */
        BitCount++;

        /* Calculate the current fraction */
        Reciprocal.HighPart = (Reciprocal.HighPart << 1) |
                              (Reciprocal.LowPart >> 31);
        Reciprocal.LowPart <<= 1;

        /* Double the remainder and see if we went past the divisor */
        Remainder <<= 1;
        if (Remainder >= Divisor)
        {
            /* Set the low-bit and calculate the new remainder */
            Remainder -= Divisor;
            Reciprocal.LowPart |= 1;
        }
    }

    /* Check if we have a remainder */
    if (Remainder)
    {
        /* Check if the current fraction value is too large */
        if ((Reciprocal.LowPart == 0xFFFFFFFF) &&
            (Reciprocal.HighPart == (LONG)0xFFFFFFFF))
        {
            /* Set the high bit and reduce the bit count */
            Reciprocal.LowPart = 0;
            Reciprocal.HighPart = 0x80000000;
            BitCount--;
        }
        else
        {
            /* Check if only the lowest bits got too large */
            if (Reciprocal.LowPart == 0xFFFFFFFF)
            {
                /* Reset them and increase the high bits instead */
                Reciprocal.LowPart = 0;
                Reciprocal.HighPart++;
            }
            else
            {
                /* All is well, increase the low bits */
                Reciprocal.LowPart++;
            }
        }
    }

    /* Now calculate the actual shift and return the reciprocal */
    *Shift = (UCHAR)BitCount - 64;
    return Reciprocal;
}

CODE_SEG("INIT")
VOID
NTAPI
KiInitSpinLocks(IN PKPRCB Prcb,
                IN CCHAR Number)
{
    ULONG i;

    /* Initialize Dispatcher Fields */
    Prcb->QueueIndex = 1;
    Prcb->ReadySummary = 0;
    Prcb->DeferredReadyListHead.Next = NULL;
    for (i = 0; i < MAXIMUM_PRIORITY; i++)
    {
        /* Initialize the ready list */
        InitializeListHead(&Prcb->DispatcherReadyListHead[i]);
    }

    /* Initialize DPC Fields */
#if (NTDDI_VERSION >= NTDDI_LONGHORN)
    Prcb->DpcData[DPC_NORMAL].DpcList.ListHead.Next = NULL;
    Prcb->DpcData[DPC_NORMAL].DpcList.LastEntry = NULL;
#else
    InitializeListHead(&Prcb->DpcData[DPC_NORMAL].DpcListHead);
#endif
    KeInitializeSpinLock(&Prcb->DpcData[DPC_NORMAL].DpcLock);
    Prcb->DpcData[DPC_NORMAL].DpcQueueDepth = 0;
    Prcb->DpcData[DPC_NORMAL].DpcCount = 0;
    Prcb->DpcRoutineActive = FALSE;
    Prcb->MaximumDpcQueueDepth = KiMaximumDpcQueueDepth;
    Prcb->MinimumDpcRate = KiMinimumDpcRate;
    Prcb->AdjustDpcThreshold = KiAdjustDpcThreshold;
    KiPrcbDpcTimeLimit(Prcb) = KI_DPC_TIME_LIMIT_TICKS;
    KiPrcbDpcWatchdogPeriod(Prcb) = KI_DPC_WATCHDOG_PERIOD_TICKS;
    KeInitializeDpc(&Prcb->CallDpc, NULL, NULL);
    KeSetTargetProcessorDpc(&Prcb->CallDpc, Number);
    KeSetImportanceDpc(&Prcb->CallDpc, HighImportance);

    /* Initialize the Wait List Head */
    InitializeListHead(&Prcb->WaitListHead);

    /* Initialize Queued Spinlocks */
    Prcb->LockQueue[LockQueueDispatcherLock].Next = NULL;
    Prcb->LockQueue[LockQueueDispatcherLock].Lock = &KiDispatcherLock;
    Prcb->LockQueue[LockQueueExpansionLock].Next = NULL;
    Prcb->LockQueue[LockQueueExpansionLock].Lock = NULL;
    Prcb->LockQueue[LockQueuePfnLock].Next = NULL;
    Prcb->LockQueue[LockQueuePfnLock].Lock = &MmPfnLock;
    Prcb->LockQueue[LockQueueSystemSpaceLock].Next = NULL;
    Prcb->LockQueue[LockQueueSystemSpaceLock].Lock = &MmSystemSpaceLock;
    Prcb->LockQueue[LockQueueBcbLock].Next = NULL;
    Prcb->LockQueue[LockQueueBcbLock].Lock = &CcBcbSpinLock;
    Prcb->LockQueue[LockQueueMasterLock].Next = NULL;
    Prcb->LockQueue[LockQueueMasterLock].Lock = &CcMasterSpinLock;
    Prcb->LockQueue[LockQueueVacbLock].Next = NULL;
    Prcb->LockQueue[LockQueueVacbLock].Lock = &CcVacbSpinLock;
    Prcb->LockQueue[LockQueueWorkQueueLock].Next = NULL;
    Prcb->LockQueue[LockQueueWorkQueueLock].Lock = &CcWorkQueueSpinLock;
    Prcb->LockQueue[LockQueueNonPagedPoolLock].Next = NULL;
    Prcb->LockQueue[LockQueueNonPagedPoolLock].Lock = &NonPagedPoolLock;
    Prcb->LockQueue[LockQueueMmNonPagedPoolLock].Next = NULL;
    Prcb->LockQueue[LockQueueMmNonPagedPoolLock].Lock = &MmNonPagedPoolLock;
    Prcb->LockQueue[LockQueueIoCancelLock].Next = NULL;
    Prcb->LockQueue[LockQueueIoCancelLock].Lock = &IopCancelSpinLock;
    Prcb->LockQueue[LockQueueIoVpbLock].Next = NULL;
    Prcb->LockQueue[LockQueueIoVpbLock].Lock = &IopVpbSpinLock;
    Prcb->LockQueue[LockQueueIoDatabaseLock].Next = NULL;
    Prcb->LockQueue[LockQueueIoDatabaseLock].Lock = &IopDatabaseLock;
    Prcb->LockQueue[LockQueueIoCompletionLock].Next = NULL;
    Prcb->LockQueue[LockQueueIoCompletionLock].Lock = &IopCompletionLock;
    Prcb->LockQueue[LockQueueNtfsStructLock].Next = NULL;
    Prcb->LockQueue[LockQueueNtfsStructLock].Lock = &NtfsStructLock;
    Prcb->LockQueue[LockQueueAfdWorkQueueLock].Next = NULL;
    Prcb->LockQueue[LockQueueAfdWorkQueueLock].Lock = &AfdWorkQueueSpinLock;
    Prcb->LockQueue[LockQueueUnusedSpare16].Next = NULL;
    Prcb->LockQueue[LockQueueUnusedSpare16].Lock = NULL;

    /* Loop timer locks (shared amongst all CPUs) */
    for (i = 0; i < LOCK_QUEUE_TIMER_TABLE_LOCKS; i++)
    {
        /* Setup the Queued Spinlock (done only once by the boot CPU) */
        if (!Number)
            KeInitializeSpinLock(&KiTimerTableLock[i]);

        /* Initialize the lock */
        KiPrcbTimerTableLockQueue(Prcb, i)->Next = NULL;
        KiPrcbTimerTableLockQueue(Prcb, i)->Lock = &KiTimerTableLock[i];
    }

    /* Initialize the PRCB lock */
    KeInitializeSpinLock(&Prcb->PrcbLock);

    /* Check if this is the boot CPU */
    if (!Number)
    {
        /* Initialize the lock themselves */
        KeInitializeSpinLock(&KiDispatcherLock);
        KeInitializeSpinLock(&KiReverseStallIpiLock);
        KeInitializeSpinLock(&MmPfnLock);
        KeInitializeSpinLock(&MmSystemSpaceLock);
        KeInitializeSpinLock(&CcBcbSpinLock);
        KeInitializeSpinLock(&CcMasterSpinLock);
        KeInitializeSpinLock(&CcVacbSpinLock);
        KeInitializeSpinLock(&CcWorkQueueSpinLock);
        KeInitializeSpinLock(&IopCancelSpinLock);
        KeInitializeSpinLock(&IopCompletionLock);
        KeInitializeSpinLock(&IopDatabaseLock);
        KeInitializeSpinLock(&IopVpbSpinLock);
        KeInitializeSpinLock(&NonPagedPoolLock);
        KeInitializeSpinLock(&MmNonPagedPoolLock);
        KeInitializeSpinLock(&NtfsStructLock);
        KeInitializeSpinLock(&AfdWorkQueueSpinLock);
    }
}

CODE_SEG("INIT")
BOOLEAN
NTAPI
KeInitSystem(VOID)
{
    /* Check if Threaded DPCs are enabled */
    if (KeThreadDpcEnable)
        KiStartDpcThreads();

    /* Initialize non-portable parts of the kernel */
    KiInitMachineDependent();
    return TRUE;
}

#ifdef _M_AMD64

/* NT: an expansion may not exceed the large stack minus the normal one */
#define KI_STACK_EXPANSION_MAXIMUM (KERNEL_LARGE_STACK_SIZE - KERNEL_STACK_SIZE)
/* Headroom kept beyond the requested size: the callout's own frames around the deep part */
#define KI_STACK_EXPANSION_SLACK PAGE_SIZE

/*
 * @implemented (amd64)
 *
 * Runs Callout(Parameter) with at least Size bytes of stack: on the current stack when
 * that much is left, otherwise on a freshly committed expansion stack (KiSwitchStackAndCallout
 * in trap.S switches rsp and the thread's StackBase/StackLimit around the call; the trap
 * frame, InitialStack and the state save area stay on the original stack). Expansions nest.
 * Context is reserved (NT passes NULL).
 */
NTSTATUS
NTAPI
KeExpandKernelStackAndCalloutEx(
    _In_ PEXPAND_STACK_CALLOUT Callout,
    _In_opt_ PVOID Parameter,
    _In_ SIZE_T Size,
    _In_ BOOLEAN Wait,
    _In_opt_ PVOID Context)
{
    PKTHREAD Thread = KeGetCurrentThread();
    PKSTACK_CONTROL Control;
    PVOID StackTop;
    ULONG_PTR StackLimit, StackPointer;
    KIRQL Irql;
    UCHAR WasActive;
    volatile UCHAR Here = 0;

    UNREFERENCED_PARAMETER(Context);

    if (Size > KI_STACK_EXPANSION_MAXIMUM)
        return STATUS_INVALID_PARAMETER_3;

    /* Above DISPATCH_LEVEL no stack can be committed, and nothing may run on one */
    Irql = KeGetCurrentIrql();
    if (Irql > DISPATCH_LEVEL)
        return STATUS_INVALID_PARAMETER;

    /* Fast path: enough of the current stack is left */
    StackPointer = (ULONG_PTR)&Here;
    if (StackPointer > Thread->StackLimit &&
        StackPointer - Thread->StackLimit >= Size + KI_STACK_EXPANSION_SLACK)
    {
        Callout(Parameter);
        return STATUS_SUCCESS;
    }

    /* Committed up front: the callout never grows it from a fault. The control block
     * that records where we came from sits at the top of the new stack. */
    StackTop = MmCreateExpansionStack(Size + KI_STACK_EXPANSION_SLACK + sizeof(KSTACK_CONTROL),
                                      Wait && (Irql < DISPATCH_LEVEL),
                                      &StackLimit);
    if (StackTop == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Control = ALIGN_DOWN_POINTER_BY((PUCHAR)StackTop - sizeof(KSTACK_CONTROL), 16);
    RtlZeroMemory(Control, sizeof(*Control));
    Control->StackBase = (ULONG_PTR)StackTop;
    Control->ActualLimit = StackLimit;
    Control->Previous.StackBase = (ULONG_PTR)Thread->StackBase;
    Control->Previous.StackLimit = Thread->StackLimit;
    Control->Previous.InitialStack = (ULONG_PTR)Thread->InitialStack;
    /* Previous.KernelStack: the rsp we leave, filled by the switch */

    /* No stack swapping while a callout runs on a stack the swapper does not know */
    WasActive = Thread->CalloutActive;
    Thread->CalloutActive = TRUE;

    KiSwitchStackAndCallout(Parameter, Callout, Control);

    Thread->CalloutActive = WasActive;
    ASSERT(Thread->StackBase == (PVOID)Control->Previous.StackBase);
    ASSERT(Thread->StackLimit == Control->Previous.StackLimit);

    MmDeleteExpansionStack(StackTop);
    return STATUS_SUCCESS;
}

#define KI_STACK_CHAIN_MAXIMUM 32

BOOLEAN
NTAPI
KiFindStackLimits(
    _In_ PKTHREAD Thread,
    _In_ ULONG_PTR Address,
    _Out_ PULONG_PTR LowLimit,
    _Out_ PULONG_PTR HighLimit)
{
    ULONG_PTR Low = Thread->StackLimit;
    ULONG_PTR High = (ULONG_PTR)Thread->StackBase;
    ULONG_PTR Initial = (ULONG_PTR)Thread->InitialStack;
    PKSTACK_CONTROL Control;
    ULONG i;

    for (i = 0; i < KI_STACK_CHAIN_MAXIMUM; i++)
    {
        if (Address >= Low && Address < High)
        {
            *LowLimit = Low;
            *HighLimit = High;
            return TRUE;
        }

        /* The stack that holds InitialStack is the one the thread started on: it has no control block */
        if (!Thread->CalloutActive || (Initial > Low && Initial <= High))
            break;

        Control = ALIGN_DOWN_POINTER_BY((PUCHAR)High - sizeof(KSTACK_CONTROL), 16);
        Low = Control->Previous.StackLimit;
        High = Control->Previous.StackBase;
        if (Low >= High)
            break;
    }

    return FALSE;
}

#else /* i386, arm, arm64, ...: the callout runs on the current stack (no expansion yet) */

NTSTATUS
NTAPI
KeExpandKernelStackAndCalloutEx(
    _In_ PEXPAND_STACK_CALLOUT Callout,
    _In_opt_ PVOID Parameter,
    _In_ SIZE_T Size,
    _In_ BOOLEAN Wait,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(Context);

    Callout(Parameter);
    return STATUS_SUCCESS;
}

#endif

NTSTATUS
NTAPI
KeExpandKernelStackAndCallout(
    _In_ PEXPAND_STACK_CALLOUT Callout,
    _In_opt_ PVOID Parameter,
    _In_ SIZE_T Size)
{
    return KeExpandKernelStackAndCalloutEx(Callout, Parameter, Size, TRUE, NULL);
}
