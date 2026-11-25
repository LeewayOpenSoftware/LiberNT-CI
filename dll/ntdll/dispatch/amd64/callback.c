/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS NT Library
 * FILE:            dll/ntdll/dispatch/amd64/callback.c
 * PURPOSE:         Exception handler of the user-mode callback dispatcher (dispatch.S)
 */

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

extern PRTLP_UNHANDLED_EXCEPTION_FILTER RtlpUnhandledExceptionFilter;

/*
 * Exception handler of KiUserCallbackDispatcher (dispatch.S). The frames below the callout frame
 * belong to the caller of the syscall that called back, and the kernel entered the callback with the
 * nonvolatile registers cleared, so no walk can continue past this frame. An exception that nothing
 * inside the callback handled is unhandled here: the process's top-level filter runs, as at the end
 * of a thread in RtlpUserThreadStart, and the process ends with STATUS_FATAL_USER_CALLBACK_EXCEPTION,
 * the exit code the Win11 lab VM shows for such a process. An unwind that
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
    NtTerminateProcess(NtCurrentProcess(), STATUS_FATAL_USER_CALLBACK_EXCEPTION);
    return ExceptionContinueSearch;
}
