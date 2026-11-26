/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     JobObjectEndOfJobTimeInformation (query/set) and the JobObjectAssociateCompletionPortInformation set
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include "precomp.h"

typedef NTSTATUS (NTAPI *PNT_QUERY_INFORMATION_JOB_OBJECT)(HANDLE, JOBOBJECTINFOCLASS, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI *PNT_SET_INFORMATION_JOB_OBJECT)(HANDLE, JOBOBJECTINFOCLASS, PVOID, ULONG);

static PNT_QUERY_INFORMATION_JOB_OBJECT pNtQueryInformationJobObject;
static PNT_SET_INFORMATION_JOB_OBJECT pNtSetInformationJobObject;

static VOID
TestLengthAndAccessRows(HANDLE Job)
{
    JOBOBJECT_END_OF_JOB_TIME_INFORMATION Info;
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT Associate;
    HANDLE TerminateOnlyJob;
    NTSTATUS Status;

    /* Fixed-length classes reject anything but the exact size */
    RtlZeroMemory(&Info, sizeof(Info));
    Status = pNtQueryInformationJobObject(Job,
                                          JobObjectEndOfJobTimeInformation,
                                          &Info,
                                          sizeof(Info) - 1,
                                          NULL);
    ok_ntstatus(Status, STATUS_INFO_LENGTH_MISMATCH);
    Status = pNtQueryInformationJobObject(Job,
                                          JobObjectEndOfJobTimeInformation,
                                          &Info,
                                          sizeof(Info) + 1,
                                          NULL);
    ok_ntstatus(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* Win11 measured: the associate-port query is rejected with
       STATUS_INVALID_INFO_CLASS; a short buffer hits the length gate first */
    RtlZeroMemory(&Associate, sizeof(Associate));
    Status = pNtQueryInformationJobObject(Job,
                                          JobObjectAssociateCompletionPortInformation,
                                          &Associate,
                                          sizeof(Associate) - sizeof(ULONG),
                                          NULL);
    ok_ntstatus(Status, STATUS_INFO_LENGTH_MISMATCH);
    Status = pNtQueryInformationJobObject(Job,
                                          JobObjectAssociateCompletionPortInformation,
                                          &Associate,
                                          sizeof(Associate),
                                          NULL);
    ok_ntstatus(Status, STATUS_INVALID_INFO_CLASS);

    /* A handle without JOB_OBJECT_QUERY cannot query */
    ok(DuplicateHandle(GetCurrentProcess(),
                       Job,
                       GetCurrentProcess(),
                       &TerminateOnlyJob,
                       JOB_OBJECT_TERMINATE,
                       FALSE,
                       0),
       "DuplicateHandle failed: %lu\n", GetLastError());
    if (TerminateOnlyJob)
    {
        Status = pNtQueryInformationJobObject(TerminateOnlyJob,
                                              JobObjectEndOfJobTimeInformation,
                                              &Info,
                                              sizeof(Info),
                                              NULL);
        ok_ntstatus(Status, STATUS_ACCESS_DENIED);

        SetLastError(0xdeadbeef);
        ok(!QueryInformationJobObject(TerminateOnlyJob,
                                      JobObjectEndOfJobTimeInformation,
                                      &Info,
                                      sizeof(Info),
                                      NULL),
           "QueryInformationJobObject succeeded without JOB_OBJECT_QUERY\n");
        ok_int(GetLastError(), ERROR_ACCESS_DENIED);
        CloseHandle(TerminateOnlyJob);
    }
}

static VOID
TestEndOfJobTimeWithoutPort(HANDLE Job)
{
    JOBOBJECT_END_OF_JOB_TIME_INFORMATION Info;
    ULONG ReturnLength;

    /* Win11 baseline: the initial action is 0 (no action) */
    RtlZeroMemory(&Info, sizeof(Info));
    ok(QueryInformationJobObject(Job, JobObjectEndOfJobTimeInformation, &Info, sizeof(Info), &ReturnLength),
       "QueryInformationJobObject failed: %lu\n", GetLastError());
    ok_hex(Info.EndOfJobTimeAction, 0);

    /* Action 0 is accepted and stored (Win11 measured) */
    Info.EndOfJobTimeAction = 0;
    ok(SetInformationJobObject(Job, JobObjectEndOfJobTimeInformation, &Info, sizeof(Info)),
       "SetInformationJobObject(0) failed: %lu\n", GetLastError());
    RtlZeroMemory(&Info, sizeof(Info));
    ok(QueryInformationJobObject(Job, JobObjectEndOfJobTimeInformation, &Info, sizeof(Info), &ReturnLength),
       "QueryInformationJobObject failed: %lu\n", GetLastError());
    ok_hex(Info.EndOfJobTimeAction, 0);

    /* TERMINATE needs no port and round-trips */
    Info.EndOfJobTimeAction = JOB_OBJECT_TERMINATE_AT_END_OF_JOB;
    ok(SetInformationJobObject(Job, JobObjectEndOfJobTimeInformation, &Info, sizeof(Info)),
       "SetInformationJobObject(TERMINATE) failed: %lu\n", GetLastError());
    RtlZeroMemory(&Info, sizeof(Info));
    ok(QueryInformationJobObject(Job, JobObjectEndOfJobTimeInformation, &Info, sizeof(Info), &ReturnLength),
       "QueryInformationJobObject failed: %lu\n", GetLastError());
    ok_hex(Info.EndOfJobTimeAction, JOB_OBJECT_TERMINATE_AT_END_OF_JOB);

    /* Win11 measured: POST without an associated completion port is rejected
       and leaves the stored action untouched */
    Info.EndOfJobTimeAction = JOB_OBJECT_POST_AT_END_OF_JOB;
    ok(!SetInformationJobObject(Job, JobObjectEndOfJobTimeInformation, &Info, sizeof(Info)),
       "SetInformationJobObject(POST) succeeded without a completion port\n");
    ok_int(GetLastError(), ERROR_INVALID_PARAMETER);
    RtlZeroMemory(&Info, sizeof(Info));
    ok(QueryInformationJobObject(Job, JobObjectEndOfJobTimeInformation, &Info, sizeof(Info), &ReturnLength),
       "QueryInformationJobObject failed: %lu\n", GetLastError());
    ok_hex(Info.EndOfJobTimeAction, JOB_OBJECT_TERMINATE_AT_END_OF_JOB);

    /* Unknown actions are rejected */
    Info.EndOfJobTimeAction = 3;
    ok(!SetInformationJobObject(Job, JobObjectEndOfJobTimeInformation, &Info, sizeof(Info)),
       "SetInformationJobObject(3) succeeded\n");
    Info.EndOfJobTimeAction = 0xFFFFFFFF;
    ok(!SetInformationJobObject(Job, JobObjectEndOfJobTimeInformation, &Info, sizeof(Info)),
       "SetInformationJobObject(0xFFFFFFFF) succeeded\n");
}

static VOID
TestAssociateCompletionPortAndPost(HANDLE Job)
{
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT Associate;
    JOBOBJECT_END_OF_JOB_TIME_INFORMATION Info;
    ULONG ReturnLength;
    NTSTATUS Status;

    /* Win11 measured: the associate-port class is set-only; the query answers
       STATUS_INVALID_INFO_CLASS */
    RtlZeroMemory(&Associate, sizeof(Associate));
    Status = pNtQueryInformationJobObject(Job,
                                          JobObjectAssociateCompletionPortInformation,
                                          &Associate,
                                          sizeof(Associate),
                                          NULL);
    ok_ntstatus(Status, STATUS_INVALID_INFO_CLASS);

    /* Associate a port */
    Associate.CompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    ok(Associate.CompletionPort != NULL, "CreateIoCompletionPort failed: %lu\n", GetLastError());
    if (Associate.CompletionPort == NULL) return;

    Associate.CompletionKey = (PVOID)(ULONG_PTR)0x1234;
    ok(SetInformationJobObject(Job, JobObjectAssociateCompletionPortInformation, &Associate, sizeof(Associate)),
       "SetInformationJobObject(AssociateCompletionPort) failed: %lu\n", GetLastError());

    /* The association is one-shot: a second attempt is rejected with
       STATUS_INVALID_PARAMETER (Win11 measured) */
    {
        HANDLE SecondPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);

        if (SecondPort != NULL)
        {
            Associate.CompletionPort = SecondPort;
            Status = pNtSetInformationJobObject(Job,
                                                JobObjectAssociateCompletionPortInformation,
                                                &Associate,
                                                sizeof(Associate));
            ok_ntstatus(Status, STATUS_INVALID_PARAMETER);
            CloseHandle(SecondPort);
        }
    }

    /* Win11 measured: POST (2) is rejected even with a port associated; only
       0 and TERMINATE are accepted for the end-of-job-time action */
    Info.EndOfJobTimeAction = JOB_OBJECT_POST_AT_END_OF_JOB;
    ok(!SetInformationJobObject(Job, JobObjectEndOfJobTimeInformation, &Info, sizeof(Info)),
       "SetInformationJobObject(POST) succeeded\n");
    ok_int(GetLastError(), ERROR_INVALID_PARAMETER);
    RtlZeroMemory(&Info, sizeof(Info));
    ok(QueryInformationJobObject(Job, JobObjectEndOfJobTimeInformation, &Info, sizeof(Info), &ReturnLength),
       "QueryInformationJobObject failed: %lu\n", GetLastError());
    ok_hex(Info.EndOfJobTimeAction, JOB_OBJECT_TERMINATE_AT_END_OF_JOB);

    CloseHandle(Associate.CompletionPort);
}

START_TEST(JobQueryInfo)
{
    HMODULE Ntdll;
    HANDLE Job;

    Ntdll = GetModuleHandleW(L"ntdll.dll");
    pNtQueryInformationJobObject = (PNT_QUERY_INFORMATION_JOB_OBJECT)GetProcAddress(Ntdll, "NtQueryInformationJobObject");
    pNtSetInformationJobObject = (PNT_SET_INFORMATION_JOB_OBJECT)GetProcAddress(Ntdll, "NtSetInformationJobObject");
    if (!pNtQueryInformationJobObject || !pNtSetInformationJobObject)
    {
        skip("Nt job information routines unavailable\n");
        return;
    }

    Job = CreateJobObjectW(NULL, NULL);
    ok(Job != NULL, "CreateJobObjectW failed: %lu\n", GetLastError());
    if (Job == NULL) return;

    TestLengthAndAccessRows(Job);
    TestEndOfJobTimeWithoutPort(Job);
    TestAssociateCompletionPortAndPost(Job);

    CloseHandle(Job);
}
