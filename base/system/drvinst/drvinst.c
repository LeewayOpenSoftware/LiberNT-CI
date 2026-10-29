/*
 * PROJECT:     LiberNT Plug and Play Manager
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Worker process that installs the driver of one device
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/* INCLUDES *****************************************************************/

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <winreg.h>
#include <winuser.h>
#include <cfgmgr32.h>
#include <umpnpmgr/drvinst.h>

/* The service starts one worker per install so that a hang or a crash of an
 * installer cannot take the service down, and so that it can enforce a time
 * limit. This is the entry point exported by newdev.dll for that purpose. */
BOOL
WINAPI
DevInstallW(
    IN HWND hWndParent,
    IN HINSTANCE hInstance,
    IN LPCWSTR InstanceId,
    IN INT Show);

/* From <newdev.h>: install flags of UpdateDriverForPlugAndPlayDevices. */
#define INSTALLFLAG_FORCE           0x00000001
#define INSTALLFLAG_READONLY        0x00000002
#define INSTALLFLAG_NONINTERACTIVE  0x00000004

/* LiberNT private: install one specific INF on one device instance. The
 * hardware and compatible ids of the device all take part in the match. */
BOOL
WINAPI
DevInstallInfW(
    IN LPCWSTR InstanceId,
    IN LPCWSTR FullInfPath,
    IN DWORD InstallFlags,
    OUT PBOOL bRebootRequired OPTIONAL);

/* FUNCTIONS ****************************************************************/

static BOOL
ParseHex(
    _In_ PCWSTR String,
    _Out_ ULONG_PTR *Value)
{
    ULONG_PTR Result = 0;
    ULONG Digits = 0;

    for (; *String; String++, Digits++)
    {
        WCHAR c = *String;

        if (c >= L'0' && c <= L'9')
            Result = (Result << 4) | (c - L'0');
        else if (c >= L'a' && c <= L'f')
            Result = (Result << 4) | (c - L'a' + 10);
        else if (c >= L'A' && c <= L'F')
            Result = (Result << 4) | (c - L'A' + 10);
        else
            return FALSE;
    }

    *Value = Result;
    return Digits != 0;
}

static VOID
QueryDevnode(
    _In_ PCWSTR DeviceInstanceId,
    _Inout_ PDRVINST_RESULT Result)
{
    DEVINST DevInst;
    ULONG Status = 0;
    ULONG Problem = 0;

    Result->Cr = CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)DeviceInstanceId, CM_LOCATE_DEVNODE_NORMAL);
    if (Result->Cr != CR_SUCCESS)
        return;

    Result->Cr = CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0);
    if (Result->Cr != CR_SUCCESS)
        return;

    Result->DevStatus = Status;
    Result->Problem = (Status & DN_HAS_PROBLEM) ? Problem : 0;
    if (Status & DN_STARTED)
        Result->Flags |= DRVINST_RF_STARTED;
    if (Status & DN_NEED_RESTART)
        Result->Flags |= DRVINST_RF_NEEDREBOOT;
}

static PCWSTR
StateName(
    _In_ ULONG State)
{
    switch (State)
    {
        case DrvInstJobQueued: return L"queued";
        case DrvInstJobRunning: return L"running";
        case DrvInstJobSucceeded: return L"succeeded";
        case DrvInstJobFailed: return L"failed";
        case DrvInstJobTimedOut: return L"timed out";
        case DrvInstJobCanceled: return L"canceled";
        case DrvInstJobRolledBack: return L"rolled back";
    }
    return L"unknown";
}

static VOID
PrintJob(
    _In_ const DRVINST_JOB_INFO *Job)
{
    wprintf(L"job %I64u  %-9ls  %ls\n", Job->JobId, StateName(Job->State), Job->DeviceInstanceId);
    if (Job->State >= DrvInstJobSucceeded)
    {
        wprintf(L"    error %lu  problem %lu  started %ls  need-restart %ls\n",
                Job->Result.Win32, Job->Result.Problem,
                (Job->Result.Flags & DRVINST_RF_STARTED) ? L"yes" : L"no",
                (Job->Result.Flags & DRVINST_RF_NEEDREBOOT) ? L"yes" : L"no");
    }
}

/* drvinst /list | drvinst /job <id> | drvinst /submit <device instance id> */
static int
ClientMain(
    _In_ int argc,
    _In_ WCHAR **argv)
{
    DRVINST_JOB_INFO Job;
    DRVINST_JOB_ID Ids[64];
    DWORD Count, Error, i;

    if (!lstrcmpiW(argv[1], L"/list"))
    {
        Count = RTL_NUMBER_OF(Ids);
        Error = DrvInstEnumJobs(Ids, &Count);
        if (Error != ERROR_SUCCESS)
        {
            wprintf(L"Cannot list the jobs (error %lu)\n", Error);
            return (int)Error;
        }

        for (i = 0; i < Count; i++)
        {
            if (DrvInstGetJob(Ids[i], &Job) == ERROR_SUCCESS)
                PrintJob(&Job);
        }
        wprintf(L"%lu job(s)\n", Count);
        return 0;
    }

    if (!lstrcmpiW(argv[1], L"/job") && argc > 2)
    {
        Error = DrvInstGetJob(_wtoi64(argv[2]), &Job);
        if (Error != ERROR_SUCCESS)
        {
            wprintf(L"Cannot read the job (error %lu)\n", Error);
            return (int)Error;
        }
        PrintJob(&Job);
        return 0;
    }

    if (!lstrcmpiW(argv[1], L"/submit") && argc > 2)
    {
        DRVINST_JOB_ID JobId;

        Error = DrvInstSubmitDevice(argv[2], DRVINST_F_SILENT, &JobId);
        if (Error != ERROR_SUCCESS)
        {
            wprintf(L"Cannot submit the job (error %lu)\n", Error);
            return (int)Error;
        }

        wprintf(L"submitted job %I64u\n", JobId);
        Error = DrvInstWait(JobId, 600000, &Job);
        if (Error == ERROR_SUCCESS || Error == ERROR_TIMEOUT)
            PrintJob(&Job);
        return (int)(Error ? Error : Job.Result.Win32);
    }

    if (!lstrcmpiW(argv[1], L"/submitinf") && argc > 3)
    {
        DRVINST_JOB_ID JobId;

        Error = DrvInstSubmitInf(argv[2], argv[3], DRVINST_F_SILENT, &JobId);
        if (Error != ERROR_SUCCESS)
        {
            wprintf(L"Cannot submit the INF job (error %lu)\n", Error);
            return (int)Error;
        }

        wprintf(L"submitted job %I64u\n", JobId);
        Error = DrvInstWait(JobId, 600000, &Job);
        if (Error == ERROR_SUCCESS || Error == ERROR_TIMEOUT)
            PrintJob(&Job);
        return (int)(Error ? Error : Job.Result.Win32);
    }

    if (!lstrcmpiW(argv[1], L"/rollback") && argc > 2)
    {
        Error = DrvInstRollback(argv[2]);
        if (Error != ERROR_SUCCESS)
        {
            wprintf(L"Cannot roll back (error %lu)\n", Error);
            return (int)Error;
        }
        wprintf(L"Rolled back %ls\n", argv[2]);
        return 0;
    }

    wprintf(L"usage: drvinst /list | /job <id> | /submit <device instance id> | /rollback <device instance id>\n");
    return ERROR_INVALID_PARAMETER;
}

int
wmain(
    int argc,
    WCHAR **argv)
{
    DRVINST_RESULT Result;
    PCWSTR DeviceInstanceId = NULL;
    PCWSTR InfPath = NULL;
    HANDLE ResultHandle = NULL;
    BOOL ShowWizard = FALSE;
    DWORD Written;
    int i;

    /* Started by a person: talk to the service instead of installing */
    if (argc > 1 && (!lstrcmpiW(argv[1], L"/list") || !lstrcmpiW(argv[1], L"/job") ||
                     !lstrcmpiW(argv[1], L"/submit") || !lstrcmpiW(argv[1], L"/submitinf") ||
                     !lstrcmpiW(argv[1], L"/rollback") || !lstrcmpiW(argv[1], L"/?")))
    {
        return ClientMain(argc, argv);
    }

    ZeroMemory(&Result, sizeof(Result));
    Result.Size = sizeof(Result);

    for (i = 1; i < argc; i++)
    {
        if (!lstrcmpiW(argv[i], L"/i") && i + 1 < argc)
        {
            DeviceInstanceId = argv[++i];
        }
        else if (!lstrcmpiW(argv[i], L"/f") && i + 1 < argc)
        {
            InfPath = argv[++i];
        }
        else if (!lstrcmpiW(argv[i], L"/h") && i + 1 < argc)
        {
            ULONG_PTR Value;

            if (ParseHex(argv[++i], &Value))
                ResultHandle = (HANDLE)Value;
        }
        else if (!lstrcmpiW(argv[i], L"/w"))
        {
            ShowWizard = TRUE;
        }
    }

    if (!DeviceInstanceId || !DeviceInstanceId[0])
    {
        Result.Win32 = ERROR_INVALID_PARAMETER;
    }
    else
    {
        if (InfPath)
        {
            BOOL NeedReboot = FALSE;

            SetLastError(ERROR_SUCCESS);
            if (!DevInstallInfW(DeviceInstanceId, InfPath,
                                INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE,
                                &NeedReboot))
            {
                Result.Win32 = GetLastError();
                if (Result.Win32 == ERROR_SUCCESS)
                    Result.Win32 = ERROR_GEN_FAILURE;
            }
            if (NeedReboot)
                Result.Flags |= DRVINST_RF_NEEDREBOOT;
        }
        else
        {
            SetLastError(ERROR_SUCCESS);
            if (!DevInstallW(NULL, NULL, DeviceInstanceId, ShowWizard ? SW_SHOWNOACTIVATE : SW_HIDE))
            {
                Result.Win32 = GetLastError();
                if (Result.Win32 == ERROR_SUCCESS)
                    Result.Win32 = ERROR_GEN_FAILURE;
            }
        }

        /* The state of the devnode tells whether the driver also started */
        QueryDevnode(DeviceInstanceId, &Result);
        if (InfPath && Result.Win32 == ERROR_SUCCESS && Result.Problem != 0 &&
            Result.Problem != CM_PROB_NEED_RESTART &&
            Result.Problem != CM_PROB_DISABLED &&
            Result.Problem != CM_PROB_HARDWARE_DISABLED)
            Result.Win32 = ERROR_GEN_FAILURE;
    }

    if (ResultHandle && ResultHandle != INVALID_HANDLE_VALUE)
    {
        WriteFile(ResultHandle, &Result, sizeof(Result), &Written, NULL);
        CloseHandle(ResultHandle);
    }

    return (int)Result.Win32;
}
