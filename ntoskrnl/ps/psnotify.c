/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ps/psnotify.c
 * PURPOSE:         Process Manager: Callbacks to Registered Clients (Drivers)
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 *                  Thomas Weidenmueller (w3seek@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

BOOLEAN PsImageNotifyEnabled = FALSE;
ULONG PspThreadNotifyRoutineCount, PspProcessNotifyRoutineCount;
ULONG PspLoadImageNotifyRoutineCount;
EX_CALLBACK PspThreadNotifyRoutine[PSP_MAX_CREATE_THREAD_NOTIFY];
EX_CALLBACK PspNonSystemThreadNotifyRoutine[PSP_MAX_CREATE_THREAD_NOTIFY];
ULONG PspNonSystemThreadNotifyRoutineCount;
EX_CALLBACK PspProcessNotifyRoutine[PSP_MAX_CREATE_PROCESS_NOTIFY];
EX_CALLBACK PspLoadImageNotifyRoutine[PSP_MAX_LOAD_IMAGE_NOTIFY];
PLEGO_NOTIFY_ROUTINE PspLegoNotifyRoutine;

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * PspSetCreateProcessNotifyRoutine
 *
 * Internal worker shared by PsSetCreateProcessNotifyRoutine (legacy) and
 * PsSetCreateProcessNotifyRoutineEx.
 */
static
NTSTATUS
PspSetCreateProcessNotifyRoutine(IN PVOID NotifyRoutine,
                                 IN BOOLEAN Extended,
                                 IN BOOLEAN Remove)
{
    ULONG i;
    PEX_CALLBACK_ROUTINE_BLOCK CallBack;
    PAGED_CODE();

    /* Check if we're removing */
    if (Remove)
    {
        /* Loop all the routines */
        for (i = 0; i < PSP_MAX_CREATE_PROCESS_NOTIFY; i++)
        {
            /* Reference the callback block */
            CallBack = ExReferenceCallBackBlock(&PspProcessNotifyRoutine[i]);
            if (!CallBack) continue;

            /* Check it this is a matching block */
            if ((ExGetCallBackBlockRoutine(CallBack) == (PVOID)NotifyRoutine) &&
                ((ExGetCallBackBlockContext(CallBack) != NULL) == Extended))
            {
                /* Try removing it if it matches */
                if (ExCompareExchangeCallBack(&PspProcessNotifyRoutine[i],
                                              NULL,
                                              CallBack))
                {
                    /* Decrement the number of routines */
                    InterlockedDecrement((PLONG)&PspProcessNotifyRoutineCount);

                    /* Dereference the block */
                    ExDereferenceCallBackBlock(&PspProcessNotifyRoutine[i],
                                               CallBack);

                    /* Wait for active callbacks */
                    ExWaitForCallBacks(CallBack);

                    /* Free the callback and exit */
                    ExFreeCallBack(CallBack);
                    return STATUS_SUCCESS;
                }

                /* Dereference the block */
                ExDereferenceCallBackBlock(&PspProcessNotifyRoutine[i],
                                           CallBack);
            }
        }

        /* We didn't find any matching block */
        return STATUS_PROCEDURE_NOT_FOUND;
    }
    else
    {
        if (!NotifyRoutine) return STATUS_ACCESS_DENIED;

        /* Allocate a callback */
        CallBack = ExAllocateCallBack((PVOID)NotifyRoutine, (PVOID)(ULONG_PTR)Extended);
        if (!CallBack) return STATUS_INSUFFICIENT_RESOURCES;

        /* Loop all callbacks */
        for (i = 0; i < PSP_MAX_CREATE_PROCESS_NOTIFY; i++)
        {
            /* Add this routine if it's an empty slot */
            if (ExCompareExchangeCallBack(&PspProcessNotifyRoutine[i],
                                          CallBack,
                                          NULL))
            {
                /* Found and inserted into an empty slot, return */
                InterlockedIncrement((PLONG)&PspProcessNotifyRoutineCount);
                return STATUS_SUCCESS;
            }
        }

        /* We didn't find a free slot, free the callback and fail */
        ExFreeCallBack(CallBack);
        return STATUS_INVALID_PARAMETER;
    }
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsSetCreateProcessNotifyRoutine(IN PCREATE_PROCESS_NOTIFY_ROUTINE NotifyRoutine,
                                IN BOOLEAN Remove)
{
    return PspSetCreateProcessNotifyRoutine((PVOID)NotifyRoutine, FALSE, Remove);
}

/*
 * @implemented
 *
 * Extended process create/exit notification. The callback uses the richer
 * PCREATE_PROCESS_NOTIFY_ROUTINE_EX signature and receives a PS_CREATE_NOTIFY_INFO
 * on create (NULL on exit).
 */
NTSTATUS
NTAPI
PsSetCreateProcessNotifyRoutineEx(IN PCREATE_PROCESS_NOTIFY_ROUTINE_EX NotifyRoutine,
                                  IN BOOLEAN Remove)
{
    PAGED_CODE();

    return PspSetCreateProcessNotifyRoutine((PVOID)NotifyRoutine, TRUE, Remove);
}

/*
 * @implemented
 *
 * Windows currently defines one Ex2 notification class. It uses the extended
 * callback signature and includes processes from every subsystem. ReactOS has
 * no non-Win32 process subsystem to distinguish here, so it shares the Ex
 * callback registry and dispatcher.
 */
#if (NTDDI_VERSION >= NTDDI_WIN10_RS2)
NTSTATUS
NTAPI
PsSetCreateProcessNotifyRoutineEx2(
    IN PSCREATEPROCESSNOTIFYTYPE NotifyType,
    IN PVOID NotifyInformation,
    IN BOOLEAN Remove)
{
    PAGED_CODE();

    if (NotifyType != PsCreateProcessNotifySubsystems)
        return STATUS_INVALID_PARAMETER;

    return PsSetCreateProcessNotifyRoutineEx(
        (PCREATE_PROCESS_NOTIFY_ROUTINE_EX)NotifyInformation,
        Remove);
}
#endif

/*
 * @implemented
 */
ULONG
NTAPI
PsSetLegoNotifyRoutine(PVOID LegoNotifyRoutine)
{
    /* Set the System-Wide Lego Routine */
    PspLegoNotifyRoutine = LegoNotifyRoutine;

    /* Return the location to the Lego Data */
    return FIELD_OFFSET(KTHREAD, LegoData);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsRemoveLoadImageNotifyRoutine(IN PLOAD_IMAGE_NOTIFY_ROUTINE NotifyRoutine)
{
    ULONG i;
    PEX_CALLBACK_ROUTINE_BLOCK CallBack;
    PAGED_CODE();

    /* Loop all callbacks */
    for (i = 0; i < PSP_MAX_LOAD_IMAGE_NOTIFY; i++)
    {
        /* Reference this slot */
        CallBack = ExReferenceCallBackBlock(&PspLoadImageNotifyRoutine[i]);
        if (CallBack)
        {
            /* Check for a match */
            if (ExGetCallBackBlockRoutine(CallBack) == (PVOID)NotifyRoutine)
            {
                /* Try removing it if it matches */
                if (ExCompareExchangeCallBack(&PspLoadImageNotifyRoutine[i],
                                              NULL,
                                              CallBack))
                {
                    /* We removed it, now dereference the block */
                    InterlockedDecrement((PLONG)&PspLoadImageNotifyRoutineCount);
                    ExDereferenceCallBackBlock(&PspLoadImageNotifyRoutine[i],
                                               CallBack);

                    /* Wait for active callbacks */
                    ExWaitForCallBacks(CallBack);

                    /* Free the callback and return */
                    ExFreeCallBack(CallBack);
                    return STATUS_SUCCESS;
                }
            }

            /* Dereference the callback */
            ExDereferenceCallBackBlock(&PspLoadImageNotifyRoutine[i], CallBack);
        }
    }

    /* Nothing found to remove */
    return STATUS_PROCEDURE_NOT_FOUND;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsSetLoadImageNotifyRoutine(IN PLOAD_IMAGE_NOTIFY_ROUTINE NotifyRoutine)
{
    ULONG i;
    PEX_CALLBACK_ROUTINE_BLOCK CallBack;
    PAGED_CODE();

    /* Allocate a callback */
    CallBack = ExAllocateCallBack((PVOID)NotifyRoutine, NULL);
    if (!CallBack) return STATUS_INSUFFICIENT_RESOURCES;

    /* Loop callbacks */
    for (i = 0; i < PSP_MAX_LOAD_IMAGE_NOTIFY; i++)
    {
        /* Add this entry if the slot is empty */
        if (ExCompareExchangeCallBack(&PspLoadImageNotifyRoutine[i],
                                      CallBack,
                                      NULL))
        {
            /* Return success */
            InterlockedIncrement((PLONG)&PspLoadImageNotifyRoutineCount);
            PsImageNotifyEnabled = TRUE;
            return STATUS_SUCCESS;
        }
    }

    /* No free space found, fail */
    ExFreeCallBack(CallBack);
    return STATUS_INSUFFICIENT_RESOURCES;
}

static
NTSTATUS
PspRemoveNonSystemThreadNotifyRoutine(IN PCREATE_THREAD_NOTIFY_ROUTINE NotifyRoutine)
{
    ULONG i;
    PEX_CALLBACK_ROUTINE_BLOCK CallBack;

    for (i = 0; i < PSP_MAX_CREATE_THREAD_NOTIFY; i++)
    {
        CallBack = ExReferenceCallBackBlock(&PspNonSystemThreadNotifyRoutine[i]);
        if (!CallBack) continue;

        if ((ExGetCallBackBlockRoutine(CallBack) == (PVOID)NotifyRoutine) &&
            ExCompareExchangeCallBack(&PspNonSystemThreadNotifyRoutine[i], NULL, CallBack))
        {
            InterlockedDecrement((PLONG)&PspNonSystemThreadNotifyRoutineCount);
            ExDereferenceCallBackBlock(&PspNonSystemThreadNotifyRoutine[i], CallBack);

            /* Wait for active callbacks */
            ExWaitForCallBacks(CallBack);
            ExFreeCallBack(CallBack);
            return STATUS_SUCCESS;
        }

        ExDereferenceCallBackBlock(&PspNonSystemThreadNotifyRoutine[i], CallBack);
    }

    return STATUS_PROCEDURE_NOT_FOUND;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsRemoveCreateThreadNotifyRoutine(IN PCREATE_THREAD_NOTIFY_ROUTINE NotifyRoutine)
{
    ULONG i;
    PEX_CALLBACK_ROUTINE_BLOCK CallBack;
    PAGED_CODE();

    /* Loop all callbacks */
    for (i = 0; i < PSP_MAX_CREATE_THREAD_NOTIFY; i++)
    {
        /* Reference this slot */
        CallBack = ExReferenceCallBackBlock(&PspThreadNotifyRoutine[i]);
        if (CallBack)
        {
            /* Check for a match */
            if (ExGetCallBackBlockRoutine(CallBack) == (PVOID)NotifyRoutine)
            {
                /* Try removing it if it matches */
                if (ExCompareExchangeCallBack(&PspThreadNotifyRoutine[i],
                                              NULL,
                                              CallBack))
                {
                    /* We removed it, now dereference the block */
                    InterlockedDecrement((PLONG)&PspThreadNotifyRoutineCount);
                    ExDereferenceCallBackBlock(&PspThreadNotifyRoutine[i],
                                               CallBack);

                    /* Wait for active callbacks */
                    ExWaitForCallBacks(CallBack);

                    /* Free the callback and return */
                    ExFreeCallBack(CallBack);
                    return STATUS_SUCCESS;
                }
            }

            /* Dereference the callback */
            ExDereferenceCallBackBlock(&PspThreadNotifyRoutine[i], CallBack);
        }
    }

    /* The routine may have been registered with PsCreateThreadNotifyNonSystem */
    return PspRemoveNonSystemThreadNotifyRoutine(NotifyRoutine);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsSetCreateThreadNotifyRoutine(IN PCREATE_THREAD_NOTIFY_ROUTINE NotifyRoutine)
{
    ULONG i;
    PEX_CALLBACK_ROUTINE_BLOCK CallBack;
    PAGED_CODE();

    /* Allocate a callback */
    CallBack = ExAllocateCallBack((PVOID)NotifyRoutine, NULL);
    if (!CallBack) return STATUS_INSUFFICIENT_RESOURCES;

    /* Loop callbacks */
    for (i = 0; i < PSP_MAX_CREATE_THREAD_NOTIFY; i++)
    {
        /* Add this entry if the slot is empty */
        if (ExCompareExchangeCallBack(&PspThreadNotifyRoutine[i],
                                      CallBack,
                                      NULL))
        {
            /* Return success */
            InterlockedIncrement((PLONG)&PspThreadNotifyRoutineCount);
            return STATUS_SUCCESS;
        }
    }

    /* No free space found, fail */
    ExFreeCallBack(CallBack);
    return STATUS_INSUFFICIENT_RESOURCES;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsSetCreateThreadNotifyRoutineEx(
    _In_ PSCREATETHREADNOTIFYTYPE NotifyType,
    _In_ PVOID NotifyInformation)
{
    ULONG i;
    PEX_CALLBACK_ROUTINE_BLOCK CallBack;
    PAGED_CODE();

    if (NotifyInformation == NULL)
        return STATUS_INVALID_PARAMETER;

    /* The routines of PsSetCreateThreadNotifyRoutine already see the threads of all subsystems */
    if (NotifyType == PsCreateThreadNotifySubsystems)
        return PsSetCreateThreadNotifyRoutine((PCREATE_THREAD_NOTIFY_ROUTINE)NotifyInformation);

    if (NotifyType != PsCreateThreadNotifyNonSystem)
        return STATUS_INVALID_PARAMETER;

    CallBack = ExAllocateCallBack(NotifyInformation, NULL);
    if (!CallBack) return STATUS_INSUFFICIENT_RESOURCES;

    for (i = 0; i < PSP_MAX_CREATE_THREAD_NOTIFY; i++)
    {
        if (ExCompareExchangeCallBack(&PspNonSystemThreadNotifyRoutine[i], CallBack, NULL))
        {
            InterlockedIncrement((PLONG)&PspNonSystemThreadNotifyRoutineCount);
            return STATUS_SUCCESS;
        }
    }

    ExFreeCallBack(CallBack);
    return STATUS_INSUFFICIENT_RESOURCES;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
PsSetLoadImageNotifyRoutineEx(
    _In_ PLOAD_IMAGE_NOTIFY_ROUTINE NotifyRoutine,
    _In_ ULONG_PTR Flags)
{
    PAGED_CODE();

    if (Flags & ~PS_IMAGE_NOTIFY_CONFLICTING_ARCHITECTURE)
        return STATUS_INVALID_PARAMETER_2;

    /* Image notifications carry no architecture filter here, so every
       routine already sees what PS_IMAGE_NOTIFY_CONFLICTING_ARCHITECTURE asks for */
    return PsSetLoadImageNotifyRoutine(NotifyRoutine);
}

/* EOF */
