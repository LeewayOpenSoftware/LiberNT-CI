/*
 * PROJECT:     LiberNT Plug and Play Manager
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver install jobs: table, worker process and results
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/* INCLUDES *****************************************************************/

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

/* Finished jobs stay visible until their slot is reused */
#define DRVINST_MAX_JOBS    64

static CRITICAL_SECTION JobLock;
static DRVINST_JOB_INFO JobTable[DRVINST_MAX_JOBS];
static ULONG JobCount;                  /* entries used in JobTable */
static DRVINST_JOB_ID NextJobId = 1;
static HANDLE hRunMutex;                /* one install at a time */
static BOOL JobsInitialized;

/* TABLE ********************************************************************/

VOID
DrvInstInitialize(VOID)
{
    InitializeCriticalSection(&JobLock);
    hRunMutex = CreateMutexW(NULL, FALSE, NULL);
    JobsInitialized = (hRunMutex != NULL);
}

/* Serializes rollback with running install jobs; the RPC path uses these. */
DWORD
DrvInstAcquireInstallLock(
    _In_ DWORD TimeoutMs)
{
    if (!JobsInitialized)
        return ERROR_SERVICE_NOT_ACTIVE;
    {
        DWORD Wait = WaitForSingleObject(hRunMutex, TimeoutMs);
        return (Wait == WAIT_OBJECT_0 || Wait == WAIT_ABANDONED) ? ERROR_SUCCESS : ERROR_BUSY;
    }
}

VOID
DrvInstReleaseInstallLock(VOID)
{
    ReleaseMutex(hRunMutex);
}

static PDRVINST_JOB_INFO
FindJob(
    _In_ DRVINST_JOB_ID JobId)
{
    ULONG i;

    for (i = 0; i < JobCount; i++)
    {
        if (JobTable[i].JobId == JobId)
            return &JobTable[i];
    }

    return NULL;
}

/* The caller holds JobLock. Reuse the oldest finished slot when the table is full. */
static PDRVINST_JOB_INFO
AllocateJob(VOID)
{
    PDRVINST_JOB_INFO Oldest = NULL;
    ULONG i;

    if (JobCount < DRVINST_MAX_JOBS)
        return &JobTable[JobCount++];

    for (i = 0; i < JobCount; i++)
    {
        if (JobTable[i].State < DrvInstJobSucceeded)
            continue;
        if (!Oldest || JobTable[i].JobId < Oldest->JobId)
            Oldest = &JobTable[i];
    }

    return Oldest;
}

BOOL
IsValidDeviceInstanceId(
    _In_ PCWSTR DeviceInstanceId)
{
    static const WCHAR Allowed[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789\\&_.#{}()-+";
    PCWSTR p;

    for (; *DeviceInstanceId; DeviceInstanceId++)
    {
        for (p = Allowed; *p; p++)
        {
            if (*p == *DeviceInstanceId)
                break;
        }
        if (!*p)
            return FALSE;
    }
    return TRUE;
}

BOOL
IsValidInfPath(
    _In_ PCWSTR InfPath)
{
    if (InfPath[0] == L'\\' || InfPath[0] == L'"')
        return FALSE;
    if (!((InfPath[0] >= L'A' && InfPath[0] <= L'Z') ||
          (InfPath[0] >= L'a' && InfPath[0] <= L'z')))
        return FALSE;
    if (InfPath[1] != L':' || InfPath[2] != L'\\')
        return FALSE;
    if (wcschr(InfPath, L'"'))
        return FALSE;
    return TRUE;
}

static DRVINST_JOB_ID
CreateJob(
    _In_ PCWSTR DeviceInstanceId,
    _In_ ULONG Source,
    _In_ ULONG Flags)
{
    PDRVINST_JOB_INFO Job;
    DRVINST_JOB_ID JobId = 0;

    if (!DeviceInstanceId[0] || !IsValidDeviceInstanceId(DeviceInstanceId))
    {
        DPRINT1("Rejecting driver install job for invalid device instance id %ls\n", DeviceInstanceId);
        return 0;
    }

    EnterCriticalSection(&JobLock);

    Job = AllocateJob();
    if (Job)
    {
        ZeroMemory(Job, sizeof(*Job));
        Job->Size = sizeof(*Job);
        Job->JobId = JobId = NextJobId++;
        Job->State = DrvInstJobQueued;
        Job->Source = Source;
        Job->Flags = Flags;
        Job->SubmitTick = GetTickCount();
        lstrcpynW(Job->DeviceInstanceId, DeviceInstanceId, DRVINST_DEVICE_ID_CCH);
    }

    LeaveCriticalSection(&JobLock);

    return JobId;
}

static VOID
SetJobState(
    _In_ DRVINST_JOB_ID JobId,
    _In_ ULONG State,
    _In_opt_ const DRVINST_RESULT *Result)
{
    PDRVINST_JOB_INFO Job;
    ULONG Tick;

    EnterCriticalSection(&JobLock);

    Job = FindJob(JobId);
    if (Job)
    {
        Job->State = State;
        if (State >= DrvInstJobSucceeded)
        {
            Tick = GetTickCount();
            Job->EndTick = Tick ? Tick : 1;
        }
        if (Result)
            Job->Result = *Result;
    }

    LeaveCriticalSection(&JobLock);
}

BOOL
DrvInstGetJobInfo(
    _In_ DRVINST_JOB_ID JobId,
    _Out_ PDRVINST_JOB_INFO Info)
{
    PDRVINST_JOB_INFO Job;
    BOOL Found = FALSE;

    if (!JobsInitialized)
        return FALSE;

    EnterCriticalSection(&JobLock);

    Job = FindJob(JobId);
    if (Job)
    {
        *Info = *Job;
        Found = TRUE;
    }

    LeaveCriticalSection(&JobLock);

    return Found;
}

/* Copies up to Max jobs, newest first, and returns how many were copied */
ULONG
DrvInstpEnumJobs(
    _Out_writes_to_(Max, return) PDRVINST_JOB_INFO Info,
    _In_ ULONG Max)
{
    ULONG Count = 0;
    DRVINST_JOB_ID Wanted;

    if (!JobsInitialized)
        return 0;

    EnterCriticalSection(&JobLock);

    for (Wanted = NextJobId - 1; Wanted > 0 && Count < Max; Wanted--)
    {
        PDRVINST_JOB_INFO Job = FindJob(Wanted);

        if (Job)
            Info[Count++] = *Job;
    }

    LeaveCriticalSection(&JobLock);

    return Count;
}

/* WORKER *******************************************************************/

static DWORD
QueryDrvInstValue(
    _In_ PCWSTR ValueName,
    _In_ DWORD Default)
{
    HKEY hKey;
    DWORD Value = Default, Size = sizeof(Value), Type;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, DRVINST_REG_KEY, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return Default;

    if (RegQueryValueExW(hKey, ValueName, NULL, &Type, (PBYTE)&Value, &Size) != ERROR_SUCCESS ||
        Type != REG_DWORD)
    {
        Value = Default;
    }

    RegCloseKey(hKey);
    return Value;
}

BOOL
DrvInstUseWorker(VOID)
{
    return JobsInitialized && QueryDrvInstValue(DRVINST_REG_USEWORKER, 0) != 0;
}

static DWORD
GetWorkerTimeout(VOID)
{
    DWORD Value = QueryDrvInstValue(DRVINST_REG_TIMEOUT, DRVINST_DEFAULT_TIMEOUT_MS);

    return Value ? Value : DRVINST_DEFAULT_TIMEOUT_MS;
}

/*
 * Start drvinst.exe for one device and wait for it, at most the time limit.
 * Fills Result; the return value is the state the job ends in.
 * InfPath, when given, makes the worker install that package instead of
 * searching for the best one.
 */
static ULONG
RunWorker(
    _In_ PCWSTR DeviceInstanceId,
    _In_opt_ PCWSTR InfPath,
    _In_ BOOL ShowWizard,
    _Out_ PDRVINST_RESULT Result)
{
    SECURITY_ATTRIBUTES Sa;
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    HANDLE hRead = NULL, hWrite = NULL;
    LPVOID Environment = NULL;
    WCHAR SystemDir[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + DRVINST_DEVICE_ID_CCH + MAX_PATH + 64];
    DWORD WaitResult, Read, ExitCode = 0;
    ULONG State = DrvInstJobFailed;
    BOOL Created = FALSE;

    ZeroMemory(Result, sizeof(*Result));
    Result->Size = sizeof(*Result);
    ZeroMemory(&ProcessInfo, sizeof(ProcessInfo));

    /* The worker inherits only the write end of the result pipe */
    Sa.nLength = sizeof(Sa);
    Sa.lpSecurityDescriptor = NULL;
    Sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hRead, &hWrite, &Sa, sizeof(*Result)))
    {
        Result->Win32 = GetLastError();
        return State;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    if (!GetWindowsDirectoryW(SystemDir, RTL_NUMBER_OF(SystemDir)))
    {
        Result->Win32 = GetLastError();
        goto Cleanup;
    }

    if (InfPath)
    {
        _snwprintf(CommandLine, RTL_NUMBER_OF(CommandLine), L"\"%ls\\system32\\%ls\" /i \"%ls\" /f \"%ls\" /h %lx%ls",
                   SystemDir, DRVINST_WORKER_NAME, DeviceInstanceId, InfPath, (ULONG)(ULONG_PTR)hWrite,
                   ShowWizard ? L" /w" : L"");
    }
    else
    {
        _snwprintf(CommandLine, RTL_NUMBER_OF(CommandLine), L"\"%ls\\system32\\%ls\" /i \"%ls\" /h %lx%ls",
                   SystemDir, DRVINST_WORKER_NAME, DeviceInstanceId, (ULONG)(ULONG_PTR)hWrite,
                   ShowWizard ? L" /w" : L"");
    }

    CommandLine[RTL_NUMBER_OF(CommandLine) - 1] = UNICODE_NULL;

    ZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);

    if (hUserToken)
    {
        /* Like the installer before, run under the environment of the current user */
        if (!CreateEnvironmentBlock(&Environment, hUserToken, FALSE))
        {
            Result->Win32 = GetLastError();
            goto Cleanup;
        }

        Created = CreateProcessAsUserW(hUserToken, NULL, CommandLine, NULL, NULL, TRUE,
                                       CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                       Environment, NULL, &StartupInfo, &ProcessInfo);
    }
    else
    {
        Created = CreateProcessW(NULL, CommandLine, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                                 NULL, NULL, &StartupInfo, &ProcessInfo);
    }

    /* The service must not keep the write end open, or a read could never end */
    CloseHandle(hWrite);
    hWrite = NULL;

    if (!Created)
    {
        Result->Win32 = GetLastError();
        DPRINT1("Cannot start %ls for %ls (error %lu)\n", DRVINST_WORKER_NAME, DeviceInstanceId, Result->Win32);
        goto Cleanup;
    }

    WaitResult = WaitForSingleObject(ProcessInfo.hProcess, GetWorkerTimeout());
    if (WaitResult == WAIT_TIMEOUT)
    {
        DPRINT1("Install of %ls took too long, stopping the worker\n", DeviceInstanceId);
        TerminateProcess(ProcessInfo.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(ProcessInfo.hProcess, 5000);
        Result->Win32 = ERROR_TIMEOUT;
        State = DrvInstJobTimedOut;
        goto Cleanup;
    }

    GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);

    if (ReadFile(hRead, Result, sizeof(*Result), &Read, NULL) && Read == sizeof(*Result) &&
        Result->Size == sizeof(*Result))
    {
        State = (Result->Win32 == ERROR_SUCCESS) ? DrvInstJobSucceeded : DrvInstJobFailed;
    }
    else
    {
        /* The worker ended without reporting: it crashed or was stopped */
        ZeroMemory(Result, sizeof(*Result));
        Result->Size = sizeof(*Result);
        Result->Win32 = ExitCode ? ExitCode : ERROR_PROCESS_ABORTED;
        DPRINT1("%ls gave no result for %ls (exit code 0x%lx)\n", DRVINST_WORKER_NAME, DeviceInstanceId, ExitCode);
    }

Cleanup:
    if (Environment)
        DestroyEnvironmentBlock(Environment);
    if (ProcessInfo.hThread)
        CloseHandle(ProcessInfo.hThread);
    if (ProcessInfo.hProcess)
        CloseHandle(ProcessInfo.hProcess);
    if (hWrite)
        CloseHandle(hWrite);
    if (hRead)
        CloseHandle(hRead);

    return State;
}

/* Run a created job to its end in the calling thread. Installs take turns:
 * setupapi and the class installers are not meant to run twice at once. */
static BOOL
ExecuteJobEx(
    _In_ DRVINST_JOB_ID JobId,
    _In_ PCWSTR DeviceInstanceId,
    _In_opt_ PCWSTR InfPath,
    _In_ ULONG Flags)
{
    DRVINST_RESULT Result;
    ULONG State;
    DWORD SnapshotSeq = 0;
    DWORD RollbackError;

    WaitForSingleObject(hRunMutex, INFINITE);

    SetJobState(JobId, DrvInstJobRunning, NULL);
    if (DrvInstSnapshotDevice(DeviceInstanceId, &SnapshotSeq) != ERROR_SUCCESS)
        SnapshotSeq = 0;
    State = RunWorker(DeviceInstanceId, InfPath, !(Flags & DRVINST_F_SILENT), &Result);
    if ((State == DrvInstJobFailed || State == DrvInstJobTimedOut) &&
        Result.Problem != 0 &&
        SnapshotSeq != 0 &&
        QueryDrvInstValue(DRVINST_REG_AUTOROLLBACK, 1) != 0)
    {
        /* Only the snapshot of this job: an old one may not describe the device. */
        RollbackError = DrvInstRollbackDeviceToSeq(DeviceInstanceId, SnapshotSeq);
        if (RollbackError == ERROR_SUCCESS)
        {
            Result.Flags |= DRVINST_RF_ROLLEDBACK;
            State = DrvInstJobRolledBack;
        }
        else if (RollbackError == ERROR_INSTALL_FAILURE)
        {
            /* The registry is restored but the device still reports a problem. */
            Result.Flags |= DRVINST_RF_ROLLEDBACK;
        }
    }
    SetJobState(JobId, State, &Result);

    ReleaseMutex(hRunMutex);

    DPRINT("Job %I64u for %ls ended: state %lu, error %lu, problem %lu\n",
           JobId, DeviceInstanceId, State, Result.Win32, Result.Problem);

    return State == DrvInstJobSucceeded;
}

/* Run an install job for a device and wait for it. Returns TRUE when the driver was installed. */
static BOOL
RunDeviceJobEx(
    _In_ PCWSTR DeviceInstanceId,
    _In_opt_ PCWSTR InfPath,
    _In_ ULONG Source,
    _In_ ULONG Flags,
    _Out_opt_ PDRVINST_JOB_ID OutJobId)
{
    DRVINST_JOB_ID JobId;

    if (OutJobId)
        *OutJobId = 0;

    if (!JobsInitialized)
        return FALSE;

    JobId = CreateJob(DeviceInstanceId, Source, Flags);
    if (OutJobId)
        *OutJobId = JobId;
    if (!JobId)
        return FALSE;

    return ExecuteJobEx(JobId, DeviceInstanceId, InfPath, Flags);
}

BOOL
DrvInstRunDeviceJob(
    _In_ PCWSTR DeviceInstanceId,
    _In_ ULONG Source,
    _In_ ULONG Flags,
    _Out_opt_ PDRVINST_JOB_ID OutJobId)
{
    return RunDeviceJobEx(DeviceInstanceId, NULL, Source, Flags, OutJobId);
}

BOOL
DrvInstRunInfJob(
    _In_ PCWSTR DeviceInstanceId,
    _In_ PCWSTR InfPath,
    _In_ ULONG Source,
    _In_ ULONG Flags,
    _Out_opt_ PDRVINST_JOB_ID OutJobId)
{
    return RunDeviceJobEx(DeviceInstanceId, InfPath, Source, Flags, OutJobId);
}

typedef struct _ASYNC_JOB
{
    DRVINST_JOB_ID JobId;
    ULONG Flags;
    WCHAR DeviceInstanceId[DRVINST_DEVICE_ID_CCH];
    WCHAR InfPath[MAX_PATH];
} ASYNC_JOB, *PASYNC_JOB;

static DWORD WINAPI
AsyncJobThread(LPVOID Parameter)
{
    PASYNC_JOB Async = Parameter;

    ExecuteJobEx(Async->JobId, Async->DeviceInstanceId,
                 Async->InfPath[0] ? Async->InfPath : NULL, Async->Flags);
    HeapFree(GetProcessHeap(), 0, Async);

    return 0;
}

/* Queue an install job and return at once; the caller follows it with its id. */
static DWORD
SubmitDeviceJobEx(
    _In_ PCWSTR DeviceInstanceId,
    _In_opt_ PCWSTR InfPath,
    _In_ ULONG Source,
    _In_ ULONG Flags,
    _Out_ PDRVINST_JOB_ID OutJobId)
{
    PASYNC_JOB Async;
    DRVINST_JOB_ID JobId;
    HANDLE hThread;
    DWORD Error;

    *OutJobId = 0;

    if (!JobsInitialized)
        return ERROR_SERVICE_NOT_ACTIVE;
    if (!DeviceInstanceId[0] || lstrlenW(DeviceInstanceId) >= DRVINST_DEVICE_ID_CCH ||
        !IsValidDeviceInstanceId(DeviceInstanceId))
        return ERROR_INVALID_PARAMETER;
    if (InfPath && (!InfPath[0] || lstrlenW(InfPath) >= MAX_PATH || !IsValidInfPath(InfPath)))
        return ERROR_INVALID_PARAMETER;

    Async = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Async));
    if (!Async)
        return ERROR_NOT_ENOUGH_MEMORY;

    Async->JobId = JobId = CreateJob(DeviceInstanceId, Source, Flags);
    if (!JobId)
    {
        HeapFree(GetProcessHeap(), 0, Async);
        return ERROR_NO_SYSTEM_RESOURCES;
    }
    Async->Flags = Flags;
    lstrcpynW(Async->DeviceInstanceId, DeviceInstanceId, DRVINST_DEVICE_ID_CCH);
    if (InfPath)
        lstrcpynW(Async->InfPath, InfPath, MAX_PATH);

    hThread = CreateThread(NULL, 0, AsyncJobThread, Async, 0, NULL);
    if (!hThread)
    {
        DRVINST_RESULT FailedResult;

        Error = GetLastError();
        ZeroMemory(&FailedResult, sizeof(FailedResult));
        FailedResult.Size = sizeof(FailedResult);
        FailedResult.Win32 = Error ? Error : ERROR_GEN_FAILURE;
        SetJobState(JobId, DrvInstJobFailed, &FailedResult);
        HeapFree(GetProcessHeap(), 0, Async);
        return FailedResult.Win32;
    }

    *OutJobId = JobId;
    CloseHandle(hThread);

    return ERROR_SUCCESS;
}

DWORD
DrvInstSubmitDeviceJob(
    _In_ PCWSTR DeviceInstanceId,
    _In_ ULONG Source,
    _In_ ULONG Flags,
    _Out_ PDRVINST_JOB_ID OutJobId)
{
    return SubmitDeviceJobEx(DeviceInstanceId, NULL, Source, Flags, OutJobId);
}

DWORD
DrvInstSubmitInfJob(
    _In_ PCWSTR DeviceInstanceId,
    _In_ PCWSTR InfPath,
    _In_ ULONG Source,
    _In_ ULONG Flags,
    _Out_ PDRVINST_JOB_ID OutJobId)
{
    return SubmitDeviceJobEx(DeviceInstanceId, InfPath, Source, Flags, OutJobId);
}
