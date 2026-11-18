/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The handle list of NtCreateUserProcess is read in the table of the parent and every entry counts
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

static UNICODE_STRING NtImagePath;

static
NTSTATUS
CreateSuspendedProcess(
    _In_opt_ HANDLE Parent,
    _In_reads_opt_(Count) PHANDLE Handles,
    _In_ ULONG Count,
    _In_ ULONG ProcessFlags,
    _Out_ PHANDLE Process)
{
    struct
    {
        SIZE_T TotalLength;
        PS_ATTRIBUTE Attributes[3];
    } List;
    PRTL_USER_PROCESS_PARAMETERS Parameters;
    PUNICODE_STRING ImagePath = &NtCurrentPeb()->ProcessParameters->ImagePathName;
    PS_CREATE_INFO CreateInfo;
    HANDLE Thread = NULL;
    NTSTATUS Status;
    ULONG Used = 0;

    *Process = NULL;
    Status = RtlCreateProcessParametersEx(&Parameters, ImagePath, NULL, NULL, ImagePath, NULL, NULL, NULL, NULL, NULL,
                                          RTL_USER_PROCESS_PARAMETERS_NORMALIZED);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&CreateInfo, sizeof(CreateInfo));
    CreateInfo.Size = sizeof(CreateInfo);
    CreateInfo.State = PsCreateInitialState;

    RtlZeroMemory(&List, sizeof(List));
    List.Attributes[Used].Attribute = PS_ATTRIBUTE_IMAGE_NAME;
    List.Attributes[Used].Size = NtImagePath.Length;
    List.Attributes[Used].ValuePtr = NtImagePath.Buffer;
    Used++;
    if (Parent)
    {
        List.Attributes[Used].Attribute = PS_ATTRIBUTE_PARENT_PROCESS;
        List.Attributes[Used].Size = sizeof(Parent);
        List.Attributes[Used].ValuePtr = Parent;
        Used++;
    }

    if (Handles)
    {
        List.Attributes[Used].Attribute = PS_ATTRIBUTE_HANDLE_LIST;
        List.Attributes[Used].Size = Count * sizeof(HANDLE);
        List.Attributes[Used].ValuePtr = Handles;
        Used++;
    }

    List.TotalLength = FIELD_OFFSET(PS_ATTRIBUTE_LIST, Attributes) + Used * sizeof(PS_ATTRIBUTE);
    Status = NtCreateUserProcess(Process, &Thread, PROCESS_ALL_ACCESS, THREAD_ALL_ACCESS, NULL, NULL, ProcessFlags,
                                 THREAD_CREATE_FLAGS_CREATE_SUSPENDED, Parameters, &CreateInfo, (PPS_ATTRIBUTE_LIST)&List);
    RtlDestroyProcessParameters(Parameters);
    if (!NT_SUCCESS(Status))
    {
        *Process = NULL;
        return Status;
    }

    NtClose(Thread);
    if (CreateInfo.State == PsCreateSuccess)
    {
        if (CreateInfo.SuccessState.FileHandle)
            NtClose(CreateInfo.SuccessState.FileHandle);
        if (CreateInfo.SuccessState.SectionHandle)
            NtClose(CreateInfo.SuccessState.SectionHandle);
    }

    return Status;
}

/* TRUE when Value is, in Process, a handle to the object behind Event */
static
BOOLEAN
ProcessHasEvent(
    _In_ HANDLE Process,
    _In_ HANDLE Value,
    _In_ HANDLE Event)
{
    LARGE_INTEGER NoWait;
    HANDLE Copy;
    NTSTATUS Status;

    Status = NtDuplicateObject(Process, Value, NtCurrentProcess(), &Copy, 0, 0, DUPLICATE_SAME_ACCESS);
    if (!NT_SUCCESS(Status))
        return FALSE;

    NoWait.QuadPart = 0;
    NtClearEvent(Event);
    NtSetEvent(Copy, NULL);
    Status = NtWaitForSingleObject(Event, FALSE, &NoWait);
    NtClearEvent(Event);
    NtClose(Copy);
    return Status == STATUS_WAIT_0;
}

static
VOID
TestList_(
    _In_opt_ HANDLE Parent,
    _In_ PHANDLE Handles,
    _In_ ULONG Count,
    _In_ NTSTATUS Expected,
    _In_ HANDLE Event,
    _In_opt_ HANDLE Present,
    _In_opt_ HANDLE Absent,
    _In_ int Line)
{
    HANDLE Process;
    NTSTATUS Status;

    Status = CreateSuspendedProcess(Parent, Handles, Count, PROCESS_CREATE_FLAGS_INHERIT_HANDLES, &Process);
    ok_(__FILE__, Line)(Status == Expected, "Status = 0x%08lx, expected 0x%08lx\n", Status, Expected);
    if (!NT_SUCCESS(Status))
        return;

    if (Present)
        ok_(__FILE__, Line)(ProcessHasEvent(Process, Present, Event), "The listed handle was not inherited\n");
    if (Absent)
        ok_(__FILE__, Line)(!ProcessHasEvent(Process, Absent, Event), "A handle that had to stay behind was inherited\n");
    NtTerminateProcess(Process, STATUS_SUCCESS);
    NtClose(Process);
}

#define TestList(Parent, Handles, Count, Expected, Event, Present, Absent) \
    TestList_(Parent, Handles, Count, Expected, Event, Present, Absent, __LINE__)

START_TEST(NtCreateUserProcess)
{
    HANDLE Event, Mine = NULL, Plain = NULL, Helper, Process, List[2];
    HANDLE InParent = NULL, Unlisted = NULL, NotInherited = NULL;
    NTSTATUS Status;

    if (!RtlDosPathNameToNtPathName_U(NtCurrentPeb()->ProcessParameters->ImagePathName.Buffer, &NtImagePath, NULL, NULL))
    {
        skip("No NT path for the image of the test\n");
        return;
    }

    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = NtDuplicateObject(NtCurrentProcess(), Event, NtCurrentProcess(), &Mine, 0, OBJ_INHERIT, DUPLICATE_SAME_ACCESS);
    ok_ntstatus(Status, STATUS_SUCCESS);
    Status = NtDuplicateObject(NtCurrentProcess(), Event, NtCurrentProcess(), &Plain, 0, 0, DUPLICATE_SAME_ACCESS);
    ok_ntstatus(Status, STATUS_SUCCESS);

    /* The caller is the parent */
    List[0] = Mine;
    TestList(NULL, List, 1, STATUS_SUCCESS, Event, Mine, NULL);
    List[0] = Plain;
    TestList(NULL, List, 1, STATUS_INVALID_PARAMETER, Event, NULL, NULL);
    List[0] = Mine;
    List[1] = Mine;
    TestList(NULL, List, 2, STATUS_INVALID_PARAMETER, Event, NULL, NULL);

    List[0] = Mine;
    Status = CreateSuspendedProcess(NULL, List, 1, 0, &Process);
    ok_ntstatus(Status, STATUS_INVALID_PARAMETER);
    if (NT_SUCCESS(Status))
    {
        NtTerminateProcess(Process, STATUS_SUCCESS);
        NtClose(Process);
    }

    /* Another process is the parent: it never runs, only its handle table is used */
    Status = CreateSuspendedProcess(NULL, NULL, 0, 0, &Helper);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = NtDuplicateObject(NtCurrentProcess(), Event, Helper, &InParent, 0, OBJ_INHERIT, DUPLICATE_SAME_ACCESS);
    ok_ntstatus(Status, STATUS_SUCCESS);
    Status = NtDuplicateObject(NtCurrentProcess(), Event, Helper, &Unlisted, 0, OBJ_INHERIT, DUPLICATE_SAME_ACCESS);
    ok_ntstatus(Status, STATUS_SUCCESS);
    Status = NtDuplicateObject(NtCurrentProcess(), Event, Helper, &NotInherited, 0, 0, DUPLICATE_SAME_ACCESS);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (InParent && Unlisted && NotInherited)
    {
        List[0] = InParent;
        TestList(Helper, List, 1, STATUS_SUCCESS, Event, InParent, Unlisted);
        List[0] = NotInherited;
        TestList(Helper, List, 1, STATUS_INVALID_PARAMETER, Event, NULL, NULL);
        List[0] = (HANDLE)(ULONG_PTR)0x7FFF0;
        TestList(Helper, List, 1, STATUS_INVALID_PARAMETER, Event, NULL, NULL);
        List[0] = (HANDLE)((ULONG_PTR)InParent | 1);
        TestList(Helper, List, 1, STATUS_INVALID_PARAMETER, Event, NULL, NULL);
        List[0] = NtCurrentProcess();
        TestList(Helper, List, 1, STATUS_INVALID_PARAMETER, Event, NULL, NULL);
        List[0] = InParent;
        List[1] = InParent;
        TestList(Helper, List, 2, STATUS_INVALID_PARAMETER, Event, NULL, NULL);
        List[1] = NotInherited;
        TestList(Helper, List, 2, STATUS_INVALID_PARAMETER, Event, NULL, NULL);
        List[1] = Unlisted;
        TestList(Helper, List, 2, STATUS_SUCCESS, Event, InParent, NotInherited);
        TestList(Helper, List, 2, STATUS_SUCCESS, Event, Unlisted, NULL);

        /* A NULL entry: the process starts and inherits nothing */
        List[1] = NULL;
        TestList(Helper, List, 2, STATUS_SUCCESS, Event, NULL, InParent);
        TestList(Helper, List, 2, STATUS_SUCCESS, Event, NULL, Unlisted);
        List[0] = NULL;
        List[1] = InParent;
        TestList(Helper, List, 2, STATUS_SUCCESS, Event, NULL, InParent);
    }

    NtTerminateProcess(Helper, STATUS_SUCCESS);
    NtClose(Helper);

Cleanup:
    if (Plain)
        NtClose(Plain);
    if (Mine)
        NtClose(Mine);
    NtClose(Event);
    RtlFreeUnicodeString(&NtImagePath);
}
