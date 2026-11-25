/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS NT Library
 * FILE:            dll/ntdll/dispatch/dispatch.c
 * PURPOSE:         User-Mode NT Dispatchers
 * PROGRAMERS:      Alex Ionescu (alex@relsoft.net)
 *                  David Welch <welch@cwcom.net>
 */

/* INCLUDES *****************************************************************/

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

typedef NTSTATUS (NTAPI *USER_CALL)(PVOID Argument, ULONG ArgumentLength);
typedef VOID (NTAPI *WOW64_PREPARE_FOR_EXCEPTION)(PEXCEPTION_RECORD, PCONTEXT);

/* AMD64 dispatches exceptions in assembly and defines this there. */
#if defined(_M_ARM64) || defined(_M_RISCV64)
PVOID LdrpWow64PrepareForException = NULL;
#endif

/* FUNCTIONS ****************************************************************/

/*
 * @implemented
 */
VOID
NTAPI
KiUserExceptionDispatcherWorker(PEXCEPTION_RECORD ExceptionRecord,
                                PCONTEXT Context)
{
    EXCEPTION_RECORD NestedExceptionRecord;
    NTSTATUS Status;

    /* Dispatch the exception and check the result */
#if defined(_M_ARM64) || defined(_M_RISCV64)
    if (LdrpWow64PrepareForException != NULL)
    {
        ((WOW64_PREPARE_FOR_EXCEPTION)LdrpWow64PrepareForException)(ExceptionRecord, Context);
    }
#endif

#if defined(_M_ARM64)
    if (ChpeIsChpeProcess() && ChpeDispatchException(ExceptionRecord, Context))
    {
        Status = NtContinue(Context, FALSE);
    }
    else
#endif
    if (RtlDispatchException(ExceptionRecord, Context))
    {
        /* Continue executing */
        Status = NtContinue(Context, FALSE);
    }
    else
    {
        /* Raise an exception */
        Status = NtRaiseException(ExceptionRecord, Context, FALSE);
    }

    /* Setup the Exception record */
    NestedExceptionRecord.ExceptionCode = Status;
    NestedExceptionRecord.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    NestedExceptionRecord.ExceptionRecord = ExceptionRecord;
    NestedExceptionRecord.NumberParameters = Status;

    /* Raise the exception */
    RtlRaiseException(&NestedExceptionRecord);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
KiRaiseUserExceptionDispatcher(VOID)
{
    EXCEPTION_RECORD ExceptionRecord;
    NTSTATUS ExceptionCode = ((PTEB)NtCurrentTeb())->ExceptionCode;

    /* Setup the exception record */
    ExceptionRecord.ExceptionCode = ExceptionCode;
    ExceptionRecord.ExceptionFlags = 0;
    ExceptionRecord.ExceptionRecord = NULL;
    ExceptionRecord.NumberParameters = 0;

    /* Raise the exception */
    RtlRaiseException(&ExceptionRecord);
    return ExceptionCode;
}

#ifdef _M_AMD64
extern PRTLP_UNHANDLED_EXCEPTION_FILTER RtlpUnhandledExceptionFilter;

/*
 * Exception handler of KiUserCallbackDispatcher (dispatch.S). The frames below the callout frame
 * belong to the caller of the syscall that called back, and the kernel entered the callback with the
 * nonvolatile registers cleared, so no walk can continue past this frame. An exception that nothing
 * inside the callback handled is unhandled here: the process's top-level filter runs, as at the end
 * of a thread in RtlpUserThreadStart, and the process ends with the exception code. An unwind that
 * targets this frame ends the callback, with the exception code as its status; one that only passes
 * through is left to the walk below, which has no registers to go on and fails by itself.
 */
EXCEPTION_DISPOSITION
NTAPI
KiUserCallbackExceptionHandler(
    _Inout_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID EstablisherFrame,
    _Inout_ PCONTEXT ContextRecord,
    _In_ PVOID DispatcherContext)
{
    EXCEPTION_POINTERS ExceptionPointers;
    PRTLP_UNHANDLED_EXCEPTION_FILTER Filter;
    LONG Result = EXCEPTION_CONTINUE_SEARCH;

    if (ExceptionRecord->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
    {
        if (ExceptionRecord->ExceptionFlags & EXCEPTION_TARGET_UNWIND)
            ZwCallbackReturn(NULL, 0, ExceptionRecord->ExceptionCode);
        return ExceptionContinueSearch;
    }

    ExceptionPointers.ExceptionRecord = ExceptionRecord;
    ExceptionPointers.ContextRecord = ContextRecord;
    Filter = RtlDecodePointer(RtlpUnhandledExceptionFilter);
    if (Filter)
        Result = Filter(&ExceptionPointers);
    if (Result == EXCEPTION_CONTINUE_EXECUTION)
        return ExceptionContinueExecution;
    if ((Filter && Result == EXCEPTION_CONTINUE_SEARCH) || NtCurrentPeb()->BeingDebugged)
    {
        /* A filter that left it, or a debugger: the second chance belongs to the debugger. Without one
           the kernel's exception port answers it as handled and the fault would repeat forever. */
        ZwRaiseException(ExceptionRecord, ContextRecord, FALSE);
    }
    NtTerminateProcess(NtCurrentProcess(), ExceptionRecord->ExceptionCode);
    return ExceptionContinueSearch;
}
#endif

/*
 * @implemented
 */
VOID
NTAPI
KiUserCallbackDispatcher(ULONG Index,
                         PVOID Argument,
                         ULONG ArgumentLength)
{
    /* Return with the result of the callback function */
    USER_CALL *KernelCallbackTable = NtCurrentPeb()->KernelCallbackTable;
    ZwCallbackReturn(NULL,
                     0,
                     KernelCallbackTable[Index](Argument, ArgumentLength));
}
