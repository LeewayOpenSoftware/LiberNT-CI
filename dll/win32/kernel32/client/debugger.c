/*
 * PROJECT:         ReactOS Win32 Base API
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            dll/win32/kernel32/client/debugger.c
 * PURPOSE:         Wrappers for the NT Debug Implementation
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <k32.h>

#include <ndk/dbgkfuncs.h>

#define NDEBUG
#include <debug.h>


/* PRIVATE FUNCTIONS *********************************************************/


/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
DebugBreakProcess(IN HANDLE Process)
{
    NTSTATUS Status;

    /* Send the breakin request */
    Status = DbgUiIssueRemoteBreakin(Process);
    if (!NT_SUCCESS(Status))
    {
        /* Failure */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Success */
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
DebugSetProcessKillOnExit(IN BOOL KillOnExit)
{
    HANDLE Handle;
    NTSTATUS Status;
    ULONG State;

    /* Get the debug object */
    Handle = DbgUiGetThreadDebugObject();
    if (!Handle)
    {
        /* Fail */
        BaseSetLastNTError(STATUS_INVALID_HANDLE);
        return FALSE;
    }

    /* Now set the kill-on-exit state */
    State = KillOnExit != 0;
    Status = NtSetInformationDebugObject(Handle,
                                         DebugObjectKillProcessOnExitInformation,
                                         &State,
                                         sizeof(State),
                                         NULL);
    if (!NT_SUCCESS(Status))
    {
        /* Fail */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Success */
    return TRUE;
}

/*
 * @implemented
 */

/*
 * @implemented
 */


/*
 * @implemented
 */

/*
 * @implemented
 */

/* EOF */
