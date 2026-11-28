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

typedef struct _DBGSS_THREAD_DATA
{
    struct _DBGSS_THREAD_DATA *Next;
    HANDLE ThreadHandle;
    HANDLE ProcessHandle;
    DWORD ProcessId;
    DWORD ThreadId;
    BOOLEAN HandleMarked;
} DBGSS_THREAD_DATA, *PDBGSS_THREAD_DATA;

#define DbgSsSetThreadData(d) \
    NtCurrentTeb()->DbgSsReserved[0] = d

#define DbgSsGetThreadData() \
    ((PDBGSS_THREAD_DATA)NtCurrentTeb()->DbgSsReserved[0])

/* PRIVATE FUNCTIONS *********************************************************/

static HANDLE
K32CreateDBMonMutex(VOID)
{
    static SID_IDENTIFIER_AUTHORITY siaNTAuth = {SECURITY_NT_AUTHORITY};
    static SID_IDENTIFIER_AUTHORITY siaWorldAuth = {SECURITY_WORLD_SID_AUTHORITY};
    HANDLE hMutex;
    NTSTATUS Status;

    /* SIDs to be used in the DACL */
    PSID psidSystem = NULL;
    PSID psidAdministrators = NULL;
    PSID psidEveryone = NULL;

    /* Buffer for the DACL */
    PVOID pDaclBuf = NULL;

    /* Minimum size of the DACL: an ACL descriptor and three ACCESS_ALLOWED_ACE
     * headers. We will add the size of SIDs when they are known. */
    SIZE_T nDaclBufSize =
        sizeof(ACL) + 3 * FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart);

    /* Security descriptor and attributes of the mutex */
    SECURITY_DESCRIPTOR sdMutexSecurity;
    SECURITY_ATTRIBUTES saMutexAttribs = {sizeof(saMutexAttribs),
                                          &sdMutexSecurity,
                                          TRUE};

    /* Try to open the mutex */
    hMutex = OpenMutexW(SYNCHRONIZE | READ_CONTROL | MUTANT_QUERY_STATE,
                        TRUE,
                        L"DBWinMutex");
    if (hMutex != NULL)
    {
        /* Success */
        return hMutex;
    }
    /* Error other than the mutex not being found */
    else if (GetLastError() != ERROR_FILE_NOT_FOUND)
    {
        /* Failure */
        return NULL;
    }

    /* If the mutex does not exist, set up its security, then create it */

    /* Allocate the NT AUTHORITY\SYSTEM SID */
    Status = RtlAllocateAndInitializeSid(&siaNTAuth,
                                         1,
                                         SECURITY_LOCAL_SYSTEM_RID,
                                         0, 0, 0, 0, 0, 0, 0,
                                         &psidSystem);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Allocate the BUILTIN\Administrators SID */
    Status = RtlAllocateAndInitializeSid(&siaNTAuth,
                                         2,
                                         SECURITY_BUILTIN_DOMAIN_RID,
                                         DOMAIN_ALIAS_RID_ADMINS,
                                         0, 0, 0, 0, 0, 0,
                                         &psidAdministrators);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Allocate the Everyone SID */
    Status = RtlAllocateAndInitializeSid(&siaWorldAuth,
                                         1,
                                         SECURITY_WORLD_RID,
                                         0, 0, 0, 0, 0, 0, 0,
                                         &psidEveryone);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Allocate space for the SIDs too */
    nDaclBufSize += RtlLengthSid(psidSystem);
    nDaclBufSize += RtlLengthSid(psidAdministrators);
    nDaclBufSize += RtlLengthSid(psidEveryone);

    /* Allocate the buffer for the DACL */
    pDaclBuf = GlobalAlloc(GMEM_FIXED, nDaclBufSize);
    if (pDaclBuf == NULL)
        goto Cleanup;

    /* Create the DACL */
    Status = RtlCreateAcl(pDaclBuf, nDaclBufSize, ACL_REVISION);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Grant the minimum required access to Everyone */
    Status = RtlAddAccessAllowedAce(pDaclBuf,
                                    ACL_REVISION,
                                    SYNCHRONIZE |
                                    READ_CONTROL |
                                    MUTANT_QUERY_STATE,
                                    psidEveryone);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Grant full access to BUILTIN\Administrators */
    Status = RtlAddAccessAllowedAce(pDaclBuf,
                                    ACL_REVISION,
                                    MUTANT_ALL_ACCESS,
                                    psidAdministrators);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Grant full access to NT AUTHORITY\SYSTEM */
    Status = RtlAddAccessAllowedAce(pDaclBuf,
                                    ACL_REVISION,
                                    MUTANT_ALL_ACCESS,
                                    psidSystem);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Create the security descriptor */
    Status = RtlCreateSecurityDescriptor(&sdMutexSecurity,
                                         SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Set the descriptor's DACL to the created ACL */
    Status = RtlSetDaclSecurityDescriptor(&sdMutexSecurity,
                                          TRUE,
                                          pDaclBuf,
                                          FALSE);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Create the mutex */
    hMutex = CreateMutexW(&saMutexAttribs, FALSE, L"DBWinMutex");

Cleanup:
    /* Free the buffers */
    if (pDaclBuf) GlobalFree(pDaclBuf);
    if (psidEveryone) RtlFreeSid(psidEveryone);
    if (psidAdministrators) RtlFreeSid(psidAdministrators);
    if (psidSystem) RtlFreeSid(psidSystem);

    return hMutex;
}

VOID
WINAPI
SaveThreadHandle(IN DWORD dwProcessId,
                 IN DWORD dwThreadId,
                 IN HANDLE hThread)
{
    PDBGSS_THREAD_DATA ThreadData;

    /* Allocate a thread structure */
    ThreadData = RtlAllocateHeap(RtlGetProcessHeap(),
                                 0,
                                 sizeof(DBGSS_THREAD_DATA));
    if (!ThreadData) return;

    /* Fill it out */
    ThreadData->ThreadHandle = hThread;
    ThreadData->ProcessId = dwProcessId;
    ThreadData->ThreadId = dwThreadId;
    ThreadData->ProcessHandle = NULL;
    ThreadData->HandleMarked = FALSE;

    /* Link it */
    ThreadData->Next = DbgSsGetThreadData();
    DbgSsSetThreadData(ThreadData);
}

VOID
WINAPI
SaveProcessHandle(IN DWORD dwProcessId,
                  IN HANDLE hProcess)
{
    PDBGSS_THREAD_DATA ThreadData;

    /* Allocate a thread structure */
    ThreadData = RtlAllocateHeap(RtlGetProcessHeap(),
                                 0,
                                 sizeof(DBGSS_THREAD_DATA));
    if (!ThreadData) return;

    /* Fill it out */
    ThreadData->ProcessHandle = hProcess;
    ThreadData->ProcessId = dwProcessId;
    ThreadData->ThreadId = 0;
    ThreadData->ThreadHandle = NULL;
    ThreadData->HandleMarked = FALSE;

    /* Link it */
    ThreadData->Next = DbgSsGetThreadData();
    DbgSsSetThreadData(ThreadData);
}

VOID
WINAPI
MarkThreadHandle(IN DWORD dwThreadId)
{
    PDBGSS_THREAD_DATA ThreadData;

    /* Loop all thread data events */
    for (ThreadData = DbgSsGetThreadData(); ThreadData; ThreadData = ThreadData->Next)
    {
        /* Check if this one matches */
        if (ThreadData->ThreadId == dwThreadId)
        {
            /* Mark the structure and break out */
            ThreadData->HandleMarked = TRUE;
            break;
        }
    }
}

VOID
WINAPI
MarkProcessHandle(IN DWORD dwProcessId)
{
    PDBGSS_THREAD_DATA ThreadData;

    /* Loop all thread data events */
    for (ThreadData = DbgSsGetThreadData(); ThreadData; ThreadData = ThreadData->Next)
    {
        /* Check if this one matches */
        if ((ThreadData->ProcessId == dwProcessId) && !(ThreadData->ThreadId))
        {
            /* Mark the structure and break out */
            ThreadData->HandleMarked = TRUE;
            break;
        }
    }
}

VOID
WINAPI
RemoveHandles(IN DWORD dwProcessId,
              IN DWORD dwThreadId)
{
    PDBGSS_THREAD_DATA *ThreadData;
    PDBGSS_THREAD_DATA ThisData;

    /* Loop all thread data events */
    ThreadData = (PDBGSS_THREAD_DATA*)NtCurrentTeb()->DbgSsReserved;
    ThisData = *ThreadData;
    while(ThisData)
    {
        /* Check if this one matches */
        if ((ThisData->HandleMarked) &&
            ((ThisData->ProcessId == dwProcessId) || (ThisData->ThreadId == dwThreadId)))
        {
            /* Close open handles */
            if (ThisData->ThreadHandle) CloseHandle(ThisData->ThreadHandle);
            if (ThisData->ProcessHandle) CloseHandle(ThisData->ProcessHandle);

            /* Unlink the thread data */
            *ThreadData = ThisData->Next;

            /* Free it*/
            RtlFreeHeap(RtlGetProcessHeap(), 0, ThisData);
        }
        else
        {
            /* Move to the next one */
            ThreadData = &ThisData->Next;
        }
        ThisData = *ThreadData;
    }
}


HANDLE

/* PUBLIC FUNCTIONS **********************************************************/

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
