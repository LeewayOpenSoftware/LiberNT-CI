/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ex/timer.c
 * PURPOSE:         Executive Timer Implementation
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/* Timer Object Type */
POBJECT_TYPE ExTimerType = NULL;
/* Windows exports the timer object type under the public name ExTimerObjectType. */
POBJECT_TYPE ExTimerObjectType = NULL;

KSPIN_LOCK ExpWakeListLock;
LIST_ENTRY ExpWakeList;

/* Timer Mapping */
static GENERIC_MAPPING ExpTimerMapping =
{
    STANDARD_RIGHTS_READ    | TIMER_QUERY_STATE,
    STANDARD_RIGHTS_WRITE   | TIMER_MODIFY_STATE,
    STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
    TIMER_ALL_ACCESS
};

/* Timer Information Classes */
static const INFORMATION_CLASS_INFO ExTimerInfoClass[] =
{
    /* TimerBasicInformation */
    IQS_SAME(TIMER_BASIC_INFORMATION, ULONG, ICIF_QUERY),
};

static LIST_ENTRY ExpHighResolutionTimerList = { &ExpHighResolutionTimerList, &ExpHighResolutionTimerList };
static LIST_ENTRY ExpHighResolutionUserTimerList = { &ExpHighResolutionUserTimerList, &ExpHighResolutionUserTimerList };
static KSPIN_LOCK ExpHighResolutionTimerLock;

static VOID ExpUpdateHighResolutionDeadline(VOID);

/* PRIVATE FUNCTIONS *********************************************************/

static
VOID
ExpArmUserHighResolutionTimer(IN PETIMER Timer)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&ExpHighResolutionTimerLock, &OldIrql);
    if (!Timer->HighResolutionArmed)
    {
        InsertTailList(&ExpHighResolutionUserTimerList, &Timer->HighResolutionLink);
        Timer->HighResolutionArmed = TRUE;
    }
    ExpUpdateHighResolutionDeadline();
    KeReleaseSpinLock(&ExpHighResolutionTimerLock, OldIrql);
}

static
VOID
ExpDisarmUserHighResolutionTimer(IN PETIMER Timer,
                                 IN BOOLEAN OnlyIfExpired)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&ExpHighResolutionTimerLock, &OldIrql);
    if (Timer->HighResolutionArmed && (!OnlyIfExpired || !Timer->KeTimer.Header.Inserted))
    {
        RemoveEntryList(&Timer->HighResolutionLink);
        Timer->HighResolutionArmed = FALSE;
    }
    ExpUpdateHighResolutionDeadline();
    KeReleaseSpinLock(&ExpHighResolutionTimerLock, OldIrql);
}

VOID
NTAPI
ExTimerRundown(VOID)
{
    PETHREAD Thread = PsGetCurrentThread();
    KIRQL OldIrql;
    PLIST_ENTRY CurrentEntry;
    PETIMER Timer;
    ULONG DerefsToDo;

    /* Lock the Thread's Active Timer List and loop it */
    KeAcquireSpinLock(&Thread->ActiveTimerListLock, &OldIrql);
    CurrentEntry = Thread->ActiveTimerListHead.Flink;
    while (CurrentEntry != &Thread->ActiveTimerListHead)
    {
        /* Get the timer */
        Timer = CONTAINING_RECORD(CurrentEntry, ETIMER, ActiveTimerListEntry);

        /* Reference it */
        ObReferenceObject(Timer);
        DerefsToDo = 1;

        /* Unlock the list */
        KeReleaseSpinLock(&Thread->ActiveTimerListLock, OldIrql);

        /* Lock the Timer */
        KeAcquireSpinLock(&Timer->Lock, &OldIrql);

        /* Lock the list again */
        KeAcquireSpinLockAtDpcLevel(&Thread->ActiveTimerListLock);

        /* Make sure that the timer is valid */
        if ((Timer->ApcAssociated) && (&Thread->Tcb == Timer->TimerApc.Thread))
        {
            /* Remove it from the list */
            RemoveEntryList(&Timer->ActiveTimerListEntry);
            Timer->ApcAssociated = FALSE;

            /* Cancel the timer and remove its DPC and APC */
            KeCancelTimer(&Timer->KeTimer);
            if (Timer->HighResolution) ExpDisarmUserHighResolutionTimer(Timer, FALSE);
            KeRemoveQueueDpc(&Timer->TimerDpc);
            if (KeRemoveQueueApc(&Timer->TimerApc)) DerefsToDo++;

            /* Add another dereference to do */
            DerefsToDo++;
        }

        /* Unlock the list */
        KeReleaseSpinLockFromDpcLevel(&Thread->ActiveTimerListLock);

        /* Unlock the Timer */
        KeReleaseSpinLock(&Timer->Lock, OldIrql);

        /* Dereference it */
        ObDereferenceObjectEx(Timer, DerefsToDo);

        /* Loop again */
        KeAcquireSpinLock(&Thread->ActiveTimerListLock, &OldIrql);
        CurrentEntry = Thread->ActiveTimerListHead.Flink;
    }

    /* Release lock and return */
    KeReleaseSpinLock(&Thread->ActiveTimerListLock, OldIrql);
}

VOID
NTAPI
ExpDeleteTimer(IN PVOID ObjectBody)
{
    KIRQL OldIrql;
    PETIMER Timer = ObjectBody;

    /* Check if it has a Wait List */
    if (Timer->WakeTimerListEntry.Flink)
    {
        /* Lock the Wake List */
        KeAcquireSpinLock(&ExpWakeListLock, &OldIrql);

        /* Check again, since it might've changed before we locked */
        if (Timer->WakeTimerListEntry.Flink)
        {
            /* Remove it from the Wait List */
            RemoveEntryList(&Timer->WakeTimerListEntry);
            Timer->WakeTimerListEntry.Flink = NULL;
        }

        /* Release the Wake List */
        KeReleaseSpinLock(&ExpWakeListLock, OldIrql);
    }

    /* Tell the Kernel to cancel the Timer and flush all queued DPCs */
    KeCancelTimer(&Timer->KeTimer);
    if (Timer->HighResolution) ExpDisarmUserHighResolutionTimer(Timer, FALSE);
    KeFlushQueuedDpcs();
}

_Function_class_(KDEFERRED_ROUTINE)
VOID
NTAPI
ExpTimerDpcRoutine(IN PKDPC Dpc,
                   IN PVOID DeferredContext,
                   IN PVOID SystemArgument1,
                   IN PVOID SystemArgument2)
{
    PETIMER Timer = DeferredContext;
    BOOLEAN Inserted = FALSE;

    /* Reference the timer */
    if (!ObReferenceObjectSafe(Timer)) return;

    /* Lock the Timer */
    KeAcquireSpinLockAtDpcLevel(&Timer->Lock);

    if (Timer->HighResolution) ExpDisarmUserHighResolutionTimer(Timer, TRUE);

    /* Check if the timer is associated */
    if (Timer->ApcAssociated)
    {
        if ((Timer->ApcQueuedSequence != Timer->SetSequence) ||
            (Timer->ApcQueuedDueTime != Timer->KeTimer.DueTime.QuadPart))
        {
            Timer->ApcQueuedSequence = Timer->SetSequence;
            Timer->ApcQueuedDueTime = Timer->KeTimer.DueTime.QuadPart;
            Inserted = KeInsertQueueApc(&Timer->TimerApc, SystemArgument1, SystemArgument2, IO_NO_INCREMENT);
        }
    }

    /* Release the Timer */
    KeReleaseSpinLockFromDpcLevel(&Timer->Lock);

    /* Dereference it if we couldn't queue the APC */
    if (!Inserted) ObDereferenceObject(Timer);
}

VOID
NTAPI
ExpQueueTimerApcAfterWait(IN OUT PETIMER Timer)
{
    LARGE_INTEGER SystemTime;
    BOOLEAN Inserted = FALSE;
    KIRQL OldIrql;

    if (!ObReferenceObjectSafe(Timer)) return;

    KeQuerySystemTime(&SystemTime);
    KeAcquireSpinLock(&Timer->Lock, &OldIrql);
    if (Timer->ApcAssociated &&
        ((Timer->ApcQueuedSequence != Timer->SetSequence) ||
         (Timer->ApcQueuedDueTime != Timer->KeTimer.DueTime.QuadPart)))
    {
        Timer->ApcQueuedSequence = Timer->SetSequence;
        Timer->ApcQueuedDueTime = Timer->KeTimer.DueTime.QuadPart;
        Inserted = KeInsertQueueApc(&Timer->TimerApc, UlongToPtr(SystemTime.LowPart), UlongToPtr(SystemTime.HighPart), IO_NO_INCREMENT);
    }
    KeReleaseSpinLock(&Timer->Lock, OldIrql);

    if (!Inserted) ObDereferenceObject(Timer);
}

VOID
NTAPI
ExpTimerApcKernelRoutine(IN PKAPC Apc,
                         IN OUT PKNORMAL_ROUTINE* NormalRoutine,
                         IN OUT PVOID* NormalContext,
                         IN OUT PVOID* SystemArgument1,
                         IN OUT PVOID* SystemArguemnt2)
{
    PETIMER Timer;
    KIRQL OldIrql;
    ULONG DerefsToDo = 1;
    PETHREAD Thread = PsGetCurrentThread();

    /* We need to find out which Timer we are */
    Timer = CONTAINING_RECORD(Apc, ETIMER, TimerApc);

    /* Lock the Timer */
    KeAcquireSpinLock(&Timer->Lock, &OldIrql);

    /* Lock the Thread's Active Timer List*/
    KeAcquireSpinLockAtDpcLevel(&Thread->ActiveTimerListLock);

    /* Make sure that the Timer is valid, and that it belongs to this thread */
    if ((Timer->ApcAssociated) && (&Thread->Tcb == Timer->TimerApc.Thread))
    {
        /* Check if it's not periodic */
        if (!Timer->Period)
        {
            /* Remove it from the Active Timers List */
            RemoveEntryList(&Timer->ActiveTimerListEntry);

            /* Disable it */
            Timer->ApcAssociated = FALSE;
            DerefsToDo++;
        }
    }
    else
    {
        /* Clear the normal routine */
        *NormalRoutine = NULL;
    }

    /* Release locks */
    KeReleaseSpinLockFromDpcLevel(&Thread->ActiveTimerListLock);
    KeReleaseSpinLock(&Timer->Lock, OldIrql);

    /* Dereference as needed */
    ObDereferenceObjectEx(Timer, DerefsToDo);
}

CODE_SEG("INIT")
BOOLEAN
NTAPI
ExpInitializeTimerImplementation(VOID)
{
    OBJECT_TYPE_INITIALIZER ObjectTypeInitializer;
    UNICODE_STRING Name;
    NTSTATUS Status;

    /* Create the Timer Object Type */
    RtlZeroMemory(&ObjectTypeInitializer, sizeof(ObjectTypeInitializer));
    RtlInitUnicodeString(&Name, L"Timer");
    ObjectTypeInitializer.Length = sizeof(ObjectTypeInitializer);
    ObjectTypeInitializer.InvalidAttributes = OBJ_OPENLINK;
    ObjectTypeInitializer.DefaultNonPagedPoolCharge = 0x148;
    ObjectTypeInitializer.GenericMapping = ExpTimerMapping;
    ObjectTypeInitializer.PoolType = NonPagedPoolNx;
    ObjectTypeInitializer.ValidAccessMask = TIMER_ALL_ACCESS;
    ObjectTypeInitializer.DeleteProcedure = ExpDeleteTimer;
    Status = ObCreateObjectType(&Name, &ObjectTypeInitializer, NULL, &ExTimerType);
    if (!NT_SUCCESS(Status)) return FALSE;
    ExTimerObjectType = ExTimerType;

    /* Initialize the Wait List and Lock */
    KeInitializeSpinLock(&ExpWakeListLock);
    InitializeListHead(&ExpWakeList);
    return TRUE;
}

/* PUBLIC FUNCTIONS **********************************************************/

NTSTATUS
NTAPI
NtCancelTimer(IN HANDLE TimerHandle,
              OUT PBOOLEAN CurrentState OPTIONAL)
{
    PETIMER Timer;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    BOOLEAN State;
    KIRQL OldIrql;
    PETHREAD TimerThread;
    ULONG DerefsToDo = 1;
    NTSTATUS Status;
    PAGED_CODE();

    /* Check if we need to probe */
    if ((CurrentState) && (PreviousMode != KernelMode))
    {
        _SEH2_TRY
        {
            /* Make sure the pointer is valid */
            ProbeForWriteBoolean(CurrentState);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            /* Return the exception code */
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    /* Get the Timer Object */
    Status = ObReferenceObjectByHandle(TimerHandle,
                                       TIMER_MODIFY_STATE,
                                       ExTimerType,
                                       PreviousMode,
                                       (PVOID*)&Timer,
                                       NULL);
    if (NT_SUCCESS(Status))
    {
        /* Lock the Timer */
        KeAcquireSpinLock(&Timer->Lock, &OldIrql);

        /* Check if it's enabled */
        if (Timer->ApcAssociated)
        {
            /* Get the Thread. */
            TimerThread = CONTAINING_RECORD(Timer->TimerApc.Thread,
                                            ETHREAD,
                                            Tcb);

            /* Lock its active list */
            KeAcquireSpinLockAtDpcLevel(&TimerThread->ActiveTimerListLock);

            /* Remove it */
            RemoveEntryList(&Timer->ActiveTimerListEntry);
            Timer->ApcAssociated = FALSE;

            /* Unlock the list */
            KeReleaseSpinLockFromDpcLevel(&TimerThread->ActiveTimerListLock);

            /* Cancel the Timer */
            KeCancelTimer(&Timer->KeTimer);
            KeRemoveQueueDpc(&Timer->TimerDpc);
            if (KeRemoveQueueApc(&Timer->TimerApc)) DerefsToDo++;
            DerefsToDo++;
        }
        else
        {
            /* If timer was disabled, we still need to cancel it */
            KeCancelTimer(&Timer->KeTimer);
        }

        if (Timer->HighResolution) ExpDisarmUserHighResolutionTimer(Timer, FALSE);

        /* Handle a Wake Timer */
        if (Timer->WakeTimerListEntry.Flink)
        {
            /* Lock the Wake List */
            KeAcquireSpinLockAtDpcLevel(&ExpWakeListLock);

            /* Check again, since it might've changed before we locked */
            if (Timer->WakeTimerListEntry.Flink)
            {
                /* Remove it from the Wait List */
                RemoveEntryList(&Timer->WakeTimerListEntry);
                Timer->WakeTimerListEntry.Flink = NULL;
            }

            /* Release the Wake List */
            KeReleaseSpinLockFromDpcLevel(&ExpWakeListLock);
        }

        /* Unlock the Timer */
        KeReleaseSpinLock(&Timer->Lock, OldIrql);

        /* Read the old State */
        State = KeReadStateTimer(&Timer->KeTimer);

        /* Dereference the Object */
        ObDereferenceObjectEx(Timer, DerefsToDo);

        /* Check if caller wants the state */
        if (CurrentState)
        {
            _SEH2_TRY
            {
                /* Return the Timer State */
                *CurrentState = State;
            }
            _SEH2_EXCEPT(ExSystemExceptionFilter())
            {
                /* Do nothing */
                (void)0;
            }
            _SEH2_END;
        }
    }

    /* Return to Caller */
    return Status;
}

static
NTSTATUS
ExpCreateTimerObject(OUT PHANDLE TimerHandle,
                     IN ACCESS_MASK DesiredAccess,
                     IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
                     IN TIMER_TYPE TimerType,
                     IN BOOLEAN HighResolution)
{
    PETIMER Timer;
    HANDLE hTimer;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    NTSTATUS Status;
    PAGED_CODE();

    /* Check for correct timer type */
    if ((TimerType != NotificationTimer) &&
        (TimerType != SynchronizationTimer))
    {
        /* Fail */
        return STATUS_INVALID_PARAMETER_4;
    }

    /* Check if we need to probe */
    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            /* Make sure the pointer is valid */
            ProbeForWriteHandle(TimerHandle);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            /* Return the exception code */
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    /* Create the Object */
    Status = ObCreateObject(PreviousMode,
                            ExTimerType,
                            ObjectAttributes,
                            PreviousMode,
                            NULL,
                            sizeof(ETIMER),
                            0,
                            0,
                            (PVOID*)&Timer);
    if (NT_SUCCESS(Status))
    {
        /* Initialize the DPC */
        KeInitializeDpc(&Timer->TimerDpc, ExpTimerDpcRoutine, Timer);

        /* Initialize the Kernel Timer */
        KeInitializeTimerEx(&Timer->KeTimer, TimerType);

        /* Initialize the timer fields */
        KeInitializeSpinLock(&Timer->Lock);
        Timer->ApcAssociated = FALSE;
        Timer->WakeTimer = FALSE;
        Timer->WakeTimerListEntry.Flink = NULL;
        Timer->SetSequence = 0;
        Timer->ApcQueuedSequence = MAXULONG;
        Timer->ApcQueuedDueTime = 0;
        Timer->HighResolution = HighResolution;
        Timer->HighResolutionArmed = FALSE;

        /* Insert the Timer */
        Status = ObInsertObject((PVOID)Timer,
                                NULL,
                                DesiredAccess,
                                0,
                                NULL,
                                &hTimer);

        /* Check for success */
        if (NT_SUCCESS(Status))
        {
            /* Enter SEH */
            _SEH2_TRY
            {
                /* Return the Timer Handle */
                *TimerHandle = hTimer;
            }
            _SEH2_EXCEPT(ExSystemExceptionFilter())
            {
                /* Do nothing */
                (void)0;
            }
            _SEH2_END;
        }
    }

    /* Return to Caller */
    return Status;
}

NTSTATUS
NTAPI
NtCreateTimer(OUT PHANDLE TimerHandle,
              IN ACCESS_MASK DesiredAccess,
              IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
              IN TIMER_TYPE TimerType)
{
    return ExpCreateTimerObject(TimerHandle, DesiredAccess, ObjectAttributes, TimerType, FALSE);
}

/* TODO: NtSetTimer2 and NtCancelTimer2; NtSetTimer and NtCancelTimer serve these objects meanwhile */
NTSTATUS
NTAPI
NtCreateTimer2(OUT PHANDLE TimerHandle,
               IN PVOID Reserved1 OPTIONAL,
               IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
               IN ULONG Attributes,
               IN ACCESS_MASK DesiredAccess)
{
    UNREFERENCED_PARAMETER(Reserved1);

    if (Attributes & ~(EX_TIMER_HIGH_RESOLUTION | EX_TIMER_NO_WAKE | EX_TIMER_NOTIFICATION))
        return STATUS_INVALID_PARAMETER;
    return ExpCreateTimerObject(TimerHandle,
                                DesiredAccess,
                                ObjectAttributes,
                                (Attributes & EX_TIMER_NOTIFICATION) ? NotificationTimer : SynchronizationTimer,
                                (Attributes & EX_TIMER_HIGH_RESOLUTION) != 0);
}

NTSTATUS
NTAPI
NtOpenTimer(OUT PHANDLE TimerHandle,
            IN ACCESS_MASK DesiredAccess,
            IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    HANDLE hTimer;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    NTSTATUS Status;
    PAGED_CODE();

    /* Check Parameter Validity */
    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            /* Make sure the pointer is valid */
            ProbeForWriteHandle(TimerHandle);
        }
        _SEH2_EXCEPT(ExSystemExceptionFilter())
        {
            /* Return the exception code */
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    /* Open the Timer */
    Status = ObOpenObjectByName(ObjectAttributes,
                                ExTimerType,
                                PreviousMode,
                                NULL,
                                DesiredAccess,
                                NULL,
                                &hTimer);
    if (NT_SUCCESS(Status))
    {
        /* Enter SEH */
        _SEH2_TRY
        {
            /* Return the Timer Handle */
            *TimerHandle = hTimer;
        }
        _SEH2_EXCEPT(ExSystemExceptionFilter())
        {
            /* Do nothing */
            (void)0;
        }
        _SEH2_END;
    }

    /* Return to Caller */
    return Status;
}

NTSTATUS
NTAPI
NtQueryTimer(IN HANDLE TimerHandle,
             IN TIMER_INFORMATION_CLASS TimerInformationClass,
             OUT PVOID TimerInformation,
             IN ULONG TimerInformationLength,
             OUT PULONG ReturnLength OPTIONAL)
{
    PETIMER Timer;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    NTSTATUS Status;
    PTIMER_BASIC_INFORMATION BasicInfo = TimerInformation;
    PAGED_CODE();

    /* Check Validity */
    Status = DefaultQueryInfoBufferCheck(TimerInformationClass,
                                         ExTimerInfoClass,
                                         sizeof(ExTimerInfoClass) /
                                         sizeof(ExTimerInfoClass[0]),
                                         ICIF_PROBE_READ_WRITE,
                                         TimerInformation,
                                         TimerInformationLength,
                                         ReturnLength,
                                         NULL,
                                         PreviousMode);
    if (!NT_SUCCESS(Status)) return Status;

    /* Get the Timer Object */
    Status = ObReferenceObjectByHandle(TimerHandle,
                                       TIMER_QUERY_STATE,
                                       ExTimerType,
                                       PreviousMode,
                                       (PVOID*)&Timer,
                                       NULL);
    if (NT_SUCCESS(Status))
    {
        /* Return the Basic Information */
        _SEH2_TRY
        {
            /* Return the remaining time, corrected */
            BasicInfo->TimeRemaining.QuadPart = Timer->
                                                KeTimer.DueTime.QuadPart -
                                                KeQueryInterruptTime();

            /* Return the current state */
            BasicInfo->SignalState = KeReadStateTimer(&Timer->KeTimer);

            /* Return the buffer length if requested */
            if (ReturnLength) *ReturnLength = sizeof(TIMER_BASIC_INFORMATION);
        }
        _SEH2_EXCEPT(ExSystemExceptionFilter())
        {
            /* Get the exception code */
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;

        /* Dereference Object */
        ObDereferenceObject(Timer);
    }

    /* Return Status */
    return Status;
}

NTSTATUS
NTAPI
NtSetTimer(IN HANDLE TimerHandle,
           IN PLARGE_INTEGER DueTime,
           IN PTIMER_APC_ROUTINE TimerApcRoutine OPTIONAL,
           IN PVOID TimerContext OPTIONAL,
           IN BOOLEAN WakeTimer,
           IN LONG Period OPTIONAL,
           OUT PBOOLEAN PreviousState OPTIONAL)
{
    PETIMER Timer;
    KIRQL OldIrql;
    BOOLEAN State;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    PETHREAD Thread = PsGetCurrentThread();
    LARGE_INTEGER TimerDueTime;
    PETHREAD TimerThread;
    ULONG DerefsToDo = 1;
    NTSTATUS Status = STATUS_SUCCESS;
    PAGED_CODE();

    /* Check for a valid Period */
    if (Period < 0) return STATUS_INVALID_PARAMETER_6;

    /* Check if we need to probe */
    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            /* Probe and capture the due time */
            TimerDueTime = ProbeForReadLargeInteger(DueTime);

            /* Probe the state pointer if one was passed */
            if (PreviousState) ProbeForWriteBoolean(PreviousState);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            /* Return the exception code */
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }
    else
    {
        /* Capture the time directly */
        TimerDueTime = *DueTime;
    }

    /* Get the Timer Object */
    Status = ObReferenceObjectByHandle(TimerHandle,
                                       TIMER_MODIFY_STATE,
                                       ExTimerType,
                                       PreviousMode,
                                       (PVOID*)&Timer,
                                       NULL);

    /*
     * Tell the user we don't support Wake Timers...
     * when we have the ability to use/detect the Power Management
     * functionality required to support them, make this check dependent
     * on the actual PM capabilities
     */
    if (NT_SUCCESS(Status) && WakeTimer)
    {
        Status = STATUS_TIMER_RESUME_IGNORED;
    }

    /* Check status */
    if (NT_SUCCESS(Status))
    {
        /* Lock the Timer */
        KeAcquireSpinLock(&Timer->Lock, &OldIrql);

        /* Cancel Running Timer */
        if (Timer->ApcAssociated)
        {
            /* Get the Thread. */
            TimerThread = CONTAINING_RECORD(Timer->TimerApc.Thread,
                                            ETHREAD,
                                            Tcb);

            /* Lock its active list */
            KeAcquireSpinLockAtDpcLevel(&TimerThread->ActiveTimerListLock);

            /* Remove it */
            RemoveEntryList(&Timer->ActiveTimerListEntry);
            Timer->ApcAssociated = FALSE;

            /* Unlock the list */
            KeReleaseSpinLockFromDpcLevel(&TimerThread->ActiveTimerListLock);

            /* Cancel the Timer */
            KeCancelTimer(&Timer->KeTimer);
            KeRemoveQueueDpc(&Timer->TimerDpc);
            if (KeRemoveQueueApc(&Timer->TimerApc)) DerefsToDo++;
            DerefsToDo++;
        }
        else
        {
            /* If timer was disabled, we still need to cancel it */
            KeCancelTimer(&Timer->KeTimer);
        }

        /* Read the State */
        State = KeReadStateTimer(&Timer->KeTimer);

        /* Handle Wake Timers */
        Timer->WakeTimer = WakeTimer;
        KeAcquireSpinLockAtDpcLevel(&ExpWakeListLock);
        if ((WakeTimer) && !(Timer->WakeTimerListEntry.Flink))
        {
            /* Insert it into the list */
            InsertTailList(&ExpWakeList, &Timer->WakeTimerListEntry);
        }
        else if (!(WakeTimer) && (Timer->WakeTimerListEntry.Flink))
        {
            /* Remove it from the list */
            RemoveEntryList(&Timer->WakeTimerListEntry);
            Timer->WakeTimerListEntry.Flink = NULL;
        }
        KeReleaseSpinLockFromDpcLevel(&ExpWakeListLock);

        /* Set up the APC Routine if specified */
        Timer->Period = Period;
        Timer->SetSequence++;
        if (TimerApcRoutine)
        {
            /* Initialize the APC */
            KeInitializeApc(&Timer->TimerApc,
                            &Thread->Tcb,
                            CurrentApcEnvironment,
                            ExpTimerApcKernelRoutine,
                            (PKRUNDOWN_ROUTINE)NULL,
                            (PKNORMAL_ROUTINE)TimerApcRoutine,
                            PreviousMode,
                            TimerContext);

            /* Lock the Thread's Active List and Insert */
            KeAcquireSpinLockAtDpcLevel(&Thread->ActiveTimerListLock);
            InsertTailList(&Thread->ActiveTimerListHead,
                           &Timer->ActiveTimerListEntry);
            Timer->ApcAssociated = TRUE;
            KeReleaseSpinLockFromDpcLevel(&Thread->ActiveTimerListLock);

            /* One less dereference to do */
            DerefsToDo--;
         }

        if ((PreviousMode == UserMode) && (TimerDueTime.QuadPart < 0) &&
            !Timer->HighResolution &&
            !PsGetCurrentProcess()->SetTimerResolution)
        {
            ULONGLONG Now = KiQueryInterruptTimePrecise(NULL, FALSE);
            ULONGLONG Due = KiRoundToDefaultTimerResolution(Now - TimerDueTime.QuadPart);

            TimerDueTime.QuadPart = -(LONGLONG)(Due - Now);
        }

        /* Enable and Set the Timer */
        KeSetTimerEx(&Timer->KeTimer,
                     TimerDueTime,
                     Period,
                     (TimerApcRoutine || Timer->HighResolution) ? &Timer->TimerDpc : NULL);
        if (Timer->HighResolution) ExpArmUserHighResolutionTimer(Timer);

        /* Unlock the Timer */
        KeReleaseSpinLock(&Timer->Lock, OldIrql);

        /* Dereference if it was previously enabled */
        if (DerefsToDo) ObDereferenceObjectEx(Timer, DerefsToDo);

        /* Check if we need to return the State */
        if (PreviousState)
        {
            /* Enter SEH */
            _SEH2_TRY
            {
                /* Return the Timer State */
                *PreviousState = State;
            }
            _SEH2_EXCEPT(ExSystemExceptionFilter())
            {
                /* Do nothing */
                (void)0;
            }
            _SEH2_END;
        }
    }

    /* Return to Caller */
    return Status;
}

#if (NTDDI_VERSION >= NTDDI_WINBLUE)

/* EX_TIMER OBJECT FAMILY ****************************************************/

#define TAG_EX_TIMER 'RmiT'

typedef struct _EX_TIMER
{
    KTIMER KeTimer;
    KDPC TimerDpc;
    PEXT_CALLBACK Callback;
    PVOID CallbackContext;
    ULONG Attributes;
    LIST_ENTRY HighResolutionLink;
    ULONGLONG ScheduledTime;
    LONGLONG Period;
    BOOLEAN Armed;
    /* Deletion state (WDK, Deleting a System-Allocated Timer Object), under Lock */
    KSPIN_LOCK Lock;
    BOOLEAN Set;
    BOOLEAN Disabled;
    BOOLEAN DeleteAfterExpiry;
    BOOLEAN DeleteQueued;
    BOOLEAN ExpiredWhileDisabled;
    PEXT_DELETE_CALLBACK DeleteCallback;
    PVOID DeleteContext;
    WORK_QUEUE_ITEM DeleteWorkItem;
} EX_TIMER;


static
VOID
ExpUpdateHighResolutionDeadline(VOID)
{
    PLIST_ENTRY ListEntry;
    PEX_TIMER Timer;
    PETIMER UserTimer;
    ULONGLONG Deadline = 0;

    for (ListEntry = ExpHighResolutionTimerList.Flink;
         ListEntry != &ExpHighResolutionTimerList;
         ListEntry = ListEntry->Flink)
    {
        Timer = CONTAINING_RECORD(ListEntry, EX_TIMER, HighResolutionLink);
        if (Timer->KeTimer.Header.Inserted &&
            (!Deadline || (ULONGLONG)Timer->KeTimer.DueTime.QuadPart < Deadline))
        {
            Deadline = Timer->KeTimer.DueTime.QuadPart;
        }
    }

    for (ListEntry = ExpHighResolutionUserTimerList.Flink;
         ListEntry != &ExpHighResolutionUserTimerList;
         ListEntry = ListEntry->Flink)
    {
        UserTimer = CONTAINING_RECORD(ListEntry, ETIMER, HighResolutionLink);
        if (UserTimer->KeTimer.Header.Inserted &&
            (!Deadline || (ULONGLONG)UserTimer->KeTimer.DueTime.QuadPart < Deadline))
        {
            Deadline = UserTimer->KeTimer.DueTime.QuadPart;
        }
    }

    if (!KiSetClockDeadline(Deadline))
        ExpSetHighResolutionClock(!IsListEmpty(&ExpHighResolutionTimerList) ||
                                  !IsListEmpty(&ExpHighResolutionUserTimerList));
}

static
VOID
ExpDisarmHighResolutionTimer(IN PEX_TIMER Timer)
{
    if (Timer->Armed)
    {
        RemoveEntryList(&Timer->HighResolutionLink);
        Timer->Armed = FALSE;
    }
    ExpUpdateHighResolutionDeadline();
}

/* TRUE while an expiry can still come: on the table, or periodic and only off it for the clock's pass */
static
BOOLEAN
ExpTimerStillSet(IN PEX_TIMER Timer)
{
    return Timer->KeTimer.Header.Inserted ||
           (Timer->Set && ((Timer->KeTimer.Period != 0) || (Timer->Period != 0)));
}

/* The object goes first, then the driver hears about it (WDK, ExTimerDeleteCallback) */
static
VOID
ExpFinishTimerDelete(IN PEX_TIMER Timer)
{
    PEXT_DELETE_CALLBACK DeleteCallback = Timer->DeleteCallback;
    PVOID DeleteContext = Timer->DeleteContext;

    ASSERT(Timer->Disabled);
    ASSERT(!Timer->KeTimer.Header.Inserted && !Timer->Armed);

    ExFreePoolWithTag(Timer, TAG_EX_TIMER);
    if (DeleteCallback) DeleteCallback(DeleteContext);
}

_Function_class_(WORKER_THREAD_ROUTINE)
static
VOID
NTAPI
ExpDeleteTimerWorker(IN PVOID Context)
{
    PEX_TIMER Timer = Context;

    ASSERT(Timer->Disabled && Timer->DeleteQueued);

    /* An expiry the clock is still handing out, or the callback that asked for the deletion, finishes first */
    KeCancelTimer(&Timer->KeTimer);
    KeFlushQueuedDpcs();
    ExpFinishTimerDelete(Timer);
}

static
VOID
ExpQueueTimerDelete(IN PEX_TIMER Timer)
{
    ExInitializeWorkItem(&Timer->DeleteWorkItem, ExpDeleteTimerWorker, Timer);
    ExQueueWorkItem(&Timer->DeleteWorkItem, DelayedWorkQueue);
}

_Function_class_(KDEFERRED_ROUTINE)
static
VOID
NTAPI
ExpExtTimerDpcRoutine(IN PKDPC Dpc,
                      IN PVOID DeferredContext,
                      IN PVOID SystemArgument1,
                      IN PVOID SystemArgument2)
{
    PEX_TIMER Timer = DeferredContext;
    LARGE_INTEGER DueTime;
    ULONGLONG Now;
    BOOLEAN Call = TRUE;
    BOOLEAN Delete = FALSE;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    KeAcquireSpinLockAtDpcLevel(&Timer->Lock);
    if (Timer->Disabled)
    {
        /* A timer being deleted goes off at most once more; a one-shot back on the table was set before the delete */
        if ((Timer->KeTimer.Period != 0) || !Timer->KeTimer.Header.Inserted)
        {
            Call = !Timer->ExpiredWhileDisabled;
            Timer->ExpiredWhileDisabled = TRUE;
        }
    }
    else if (Timer->Attributes & EX_TIMER_HIGH_RESOLUTION)
    {
        KeAcquireSpinLockAtDpcLevel(&ExpHighResolutionTimerLock);
        if (Timer->Armed && !Timer->KeTimer.Header.Inserted)
        {
            if (Timer->Period)
            {
                Now = KiQueryInterruptTimePrecise(NULL, FALSE);
                Timer->ScheduledTime += Timer->Period;
                if (Timer->ScheduledTime <= Now)
                {
                    Timer->ScheduledTime += ((Now - Timer->ScheduledTime) / Timer->Period + 1) * Timer->Period;
                }
                DueTime.QuadPart = -(LONGLONG)(Timer->ScheduledTime - Now);
                KeSetTimerEx(&Timer->KeTimer, DueTime, 0, &Timer->TimerDpc);
                ExpUpdateHighResolutionDeadline();
            }
            else
            {
                ExpDisarmHighResolutionTimer(Timer);
            }
        }
        KeReleaseSpinLockFromDpcLevel(&ExpHighResolutionTimerLock);
    }
    KeReleaseSpinLockFromDpcLevel(&Timer->Lock);

    /* Invoke the client callback at DISPATCH_LEVEL */
    if (Call && Timer->Callback) Timer->Callback(Timer, Timer->CallbackContext);

    /* The last expiry of a deleted timer is over: the free waits for this DPC on a worker */
    KeAcquireSpinLockAtDpcLevel(&Timer->Lock);
    if (Timer->ExpiredWhileDisabled && Timer->DeleteAfterExpiry && !Timer->DeleteQueued)
    {
        KeCancelTimer(&Timer->KeTimer);
        if (Timer->Attributes & EX_TIMER_HIGH_RESOLUTION)
        {
            KeAcquireSpinLockAtDpcLevel(&ExpHighResolutionTimerLock);
            ExpDisarmHighResolutionTimer(Timer);
            KeReleaseSpinLockFromDpcLevel(&ExpHighResolutionTimerLock);
        }
        Timer->DeleteQueued = TRUE;
        Delete = TRUE;
    }
    KeReleaseSpinLockFromDpcLevel(&Timer->Lock);

    if (Delete) ExpQueueTimerDelete(Timer);
}

/*
 * @implemented
 */
PEX_TIMER
NTAPI
ExAllocateTimer(IN PEXT_CALLBACK Callback OPTIONAL,
                IN PVOID CallbackContext OPTIONAL,
                IN ULONG Attributes)
{
    PEX_TIMER Timer;
    TIMER_TYPE TimerType;

    /* Invalid attributes are a driver contract violation, not an allocation
     * failure. Windows reports TIMER_OR_DPC_INVALID with the supplied bits. */
    if (Attributes & ~(EX_TIMER_HIGH_RESOLUTION |
                       EX_TIMER_NO_WAKE |
                       EX_TIMER_NOTIFICATION))
    {
        KeBugCheckEx(TIMER_OR_DPC_INVALID, 9, 0, Attributes, 0);
    }

    /* Timer runs its DPC, so it must live in non-paged pool */
    Timer = ExAllocatePoolWithTag(NonPagedPool, sizeof(EX_TIMER), TAG_EX_TIMER);
    if (!Timer) return NULL;

    Timer->Callback = Callback;
    Timer->CallbackContext = CallbackContext;
    Timer->Attributes = Attributes;
    InitializeListHead(&Timer->HighResolutionLink);
    Timer->ScheduledTime = 0;
    Timer->Period = 0;
    Timer->Armed = FALSE;
    KeInitializeSpinLock(&Timer->Lock);
    Timer->Set = FALSE;
    Timer->Disabled = FALSE;
    Timer->DeleteAfterExpiry = FALSE;
    Timer->DeleteQueued = FALSE;
    Timer->ExpiredWhileDisabled = FALSE;
    Timer->DeleteCallback = NULL;
    Timer->DeleteContext = NULL;

    TimerType = (Attributes & EX_TIMER_NOTIFICATION) ? NotificationTimer
                                                     : SynchronizationTimer;
    KeInitializeTimerEx(&Timer->KeTimer, TimerType);

    /* The DPC also tracks the last expiry of a deleted timer, so every timer has one */
    KeInitializeDpc(&Timer->TimerDpc, ExpExtTimerDpcRoutine, Timer);

    return Timer;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
ExSetTimer(IN PEX_TIMER Timer,
           IN LONGLONG DueTime,
           IN LONGLONG Period,
           IN PEXT_SET_PARAMETERS Parameters OPTIONAL)
{
    LARGE_INTEGER TimerDueTime;
    BOOLEAN Inserted;
    KIRQL OldIrql;

    /* NoWakeTolerance-style coalescing is ignored on a single-node system */
    UNREFERENCED_PARAMETER(Parameters);

    TimerDueTime.QuadPart = DueTime;

    KeAcquireSpinLock(&Timer->Lock, &OldIrql);

    /* A timer being deleted takes no more settings */
    if (Timer->Disabled)
    {
        KeReleaseSpinLock(&Timer->Lock, OldIrql);
        return FALSE;
    }
    Timer->Set = TRUE;

    if (Timer->Attributes & EX_TIMER_HIGH_RESOLUTION)
    {
        KeAcquireSpinLockAtDpcLevel(&ExpHighResolutionTimerLock);
        Inserted = KeSetTimerEx(&Timer->KeTimer, TimerDueTime, 0, &Timer->TimerDpc);
        Timer->ScheduledTime = Timer->KeTimer.DueTime.QuadPart;
        Timer->Period = Period;
        if (!Timer->Armed)
        {
            InsertTailList(&ExpHighResolutionTimerList, &Timer->HighResolutionLink);
            Timer->Armed = TRUE;
        }
        ExpUpdateHighResolutionDeadline();
        KeReleaseSpinLockFromDpcLevel(&ExpHighResolutionTimerLock);
    }
    else
    {
        Inserted = KeSetTimerEx(&Timer->KeTimer,
                                TimerDueTime,
                                KiExtTimerPeriodToMilliseconds(Period),
                                &Timer->TimerDpc);
    }

    KeReleaseSpinLock(&Timer->Lock, OldIrql);
    return Inserted;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
ExCancelTimer(IN OUT PEX_TIMER Timer,
              IN PEXT_CANCEL_PARAMETERS Parameters OPTIONAL)
{
    BOOLEAN Cancelled;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(Parameters);

    KeAcquireSpinLock(&Timer->Lock, &OldIrql);

    if (Timer->Disabled)
    {
        KeReleaseSpinLock(&Timer->Lock, OldIrql);
        return FALSE;
    }
    Timer->Set = FALSE;

    /* Pull it out of the timer queue and drop any DPC still queued for it */
    Cancelled = KeCancelTimer(&Timer->KeTimer);
    KeRemoveQueueDpc(&Timer->TimerDpc);
    if (Timer->Attributes & EX_TIMER_HIGH_RESOLUTION)
    {
        KeAcquireSpinLockAtDpcLevel(&ExpHighResolutionTimerLock);
        ExpDisarmHighResolutionTimer(Timer);
        KeReleaseSpinLockFromDpcLevel(&ExpHighResolutionTimerLock);
    }

    KeReleaseSpinLock(&Timer->Lock, OldIrql);
    return Cancelled;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
ExDeleteTimer(IN PEX_TIMER Timer,
              IN BOOLEAN Cancel,
              IN BOOLEAN Wait,
              IN PEXT_DELETE_PARAMETERS Parameters OPTIONAL)
{
    BOOLEAN Cancelled = FALSE;
    KIRQL OldIrql;

    ASSERT(KeGetCurrentIrql() <= (Wait ? APC_LEVEL : DISPATCH_LEVEL));

    /* The WDK allows Wait only with Cancel: a wait is for a cancelled timer's last callback */
    if (Wait) Cancel = TRUE;

    KeAcquireSpinLock(&Timer->Lock, &OldIrql);

    /* A second delete (the callback of a deleted timer, say) is refused */
    if (Timer->Disabled)
    {
        KeReleaseSpinLock(&Timer->Lock, OldIrql);
        return FALSE;
    }
    Timer->Disabled = TRUE;
    if (Parameters)
    {
        Timer->DeleteCallback = Parameters->DeleteCallback;
        Timer->DeleteContext = Parameters->DeleteContext;
    }

    if (Cancel)
    {
        /* A cancelling delete lets no further callback start: an expiry already queued is dropped too */
        Timer->Set = FALSE;
        Cancelled = KeCancelTimer(&Timer->KeTimer);
        KeRemoveQueueDpc(&Timer->TimerDpc);
        Timer->ExpiredWhileDisabled = TRUE;
    }
    else if (ExpTimerStillSet(Timer))
    {
        /* Goes off once more; that expiry's DPC queues the free */
        Timer->DeleteAfterExpiry = TRUE;
        KeReleaseSpinLock(&Timer->Lock, OldIrql);
        return FALSE;
    }

    if (Timer->Attributes & EX_TIMER_HIGH_RESOLUTION)
    {
        KeAcquireSpinLockAtDpcLevel(&ExpHighResolutionTimerLock);
        ExpDisarmHighResolutionTimer(Timer);
        KeReleaseSpinLockFromDpcLevel(&ExpHighResolutionTimerLock);
    }
    Timer->DeleteQueued = !Wait;

    KeReleaseSpinLock(&Timer->Lock, OldIrql);

    if (!Wait)
    {
        ExpQueueTimerDelete(Timer);
        return Cancelled;
    }

    /* Drain an in-flight DPC so the callback can't touch freed memory */
    KeFlushQueuedDpcs();
    ExpFinishTimerDelete(Timer);
    return Cancelled;
}

#endif /* NTDDI_VERSION >= NTDDI_WINBLUE */

/* EOF */
