/*
 * PROJECT:     LiberNT NetIO driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     WFP enforcement mode (M3 stage 0). Off by default; the shim forces
 *              permit while Off, so production behaviour is unchanged. Reading
 *              classification still runs in both modes, so per-filter counters and
 *              the event ring are produced for the Off/On A/B measurement on one
 *              image. The mode is the enforcement master control: it is set only by
 *              LocalSystem and every attempt is audited (pid + image).
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "wfp.h"
#include <drivers/wfp/wfpioctl.h>

/* Off (0) until an authorized SYSTEM caller sets it. No stage commits a default of
 * On (M3-WFP-BFE-DESIGN s8 row 0, D-MODE). */
volatile LONG g_WfpEnforcementMode = WfpModeOff;

#define WFP_PARAMETERS_KEY \
    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\Netio\\Parameters"
#define WFP_ENFORCEMENT_VALUE L"WfpEnforcement"

BOOLEAN
WfpModeEnforcing(VOID)
{
    return (BOOLEAN)(ReadAcquire(&g_WfpEnforcementMode) == WfpModeOn);
}

ULONG
WfpCurrentMode(VOID)
{
    return (ULONG)ReadAcquire(&g_WfpEnforcementMode);
}

/* Missing, wrong-typed or out-of-range value means Off: the registry default is a
 * boot-time administrator-writable input, so fail to the shipped-off default. */
VOID
WfpInitializeMode(VOID)
{
    UNICODE_STRING KeyName = RTL_CONSTANT_STRING(WFP_PARAMETERS_KEY);
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(WFP_ENFORCEMENT_VALUE);
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key = NULL;
    NTSTATUS Status;
    ULONG Mode = WfpModeOff;
    ULONG ResultLength = 0;
    UCHAR Storage[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Storage;

    InitializeObjectAttributes(&Attributes, &KeyName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwOpenKey(&Key, KEY_QUERY_VALUE, &Attributes);
    if (NT_SUCCESS(Status))
    {
        Status = ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation,
                                 Info, sizeof(Storage), &ResultLength);
        if (NT_SUCCESS(Status) && Info->Type == REG_DWORD &&
            Info->DataLength == sizeof(ULONG))
        {
            Mode = *(PULONG)Info->Data;
        }
        ZwClose(Key);
    }

    if (Mode != WfpModeOn)
    {
        Mode = WfpModeOff;
    }
    WriteRelease(&g_WfpEnforcementMode, (LONG)Mode);
}

/* The effective (impersonation-aware) token must be LocalSystem. The device ACL is
 * SYS_ALL_ADM_ALL so an administrator can open the handle; this SID check keeps the
 * mode switch itself SYSTEM-only (M3 D-AUTH). ProcessId is returned for the audit. */
static
BOOLEAN
WfpCallerIsSystem(
    _Out_ PHANDLE ProcessId)
{
    SECURITY_SUBJECT_CONTEXT Subject;
    PACCESS_TOKEN Token;
    PTOKEN_USER UserInfo = NULL;
    BOOLEAN IsSystem = FALSE;

    *ProcessId = PsGetCurrentProcessId();

    SeCaptureSubjectContext(&Subject);
    SeLockSubjectContext(&Subject);
    Token = SeQuerySubjectContextToken(&Subject);
    if (Token != NULL &&
        NT_SUCCESS(SeQueryInformationToken(Token, TokenUser, (PVOID *)&UserInfo)))
    {
        IsSystem = (BOOLEAN)RtlEqualSid(UserInfo->User.Sid, SeExports->SeLocalSystemSid);
        ExFreePool(UserInfo);
    }
    SeUnlockSubjectContext(&Subject);
    SeReleaseSubjectContext(&Subject);
    return IsSystem;
}

/* IOCTL_WFP_SET_MODE. No WFP session is required: this is a device-level control for
 * the policy owner (bfe.exe/the mode helper). LocalSystem only; audited either way. */
NTSTATUS
WfpSetEnforcementMode(
    _In_reads_bytes_(InputLength) PVOID Buffer,
    _In_ ULONG InputLength)
{
    PWFP_IOCTL_SET_MODE Request = Buffer;
    HANDLE ProcessId = NULL;
    ULONG NewMode, OldMode;

    if (InputLength < sizeof(WFP_IOCTL_SET_MODE))
    {
        return STATUS_INVALID_PARAMETER;
    }

    NewMode = Request->Mode;
    if (NewMode != WfpModeOff && NewMode != WfpModeOn)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!WfpCallerIsSystem(&ProcessId))
    {
        OldMode = (ULONG)ReadAcquire(&g_WfpEnforcementMode);
        WfpEmitModeEvent(OldMode, OldMode, ProcessId, STATUS_ACCESS_DENIED);
        return STATUS_ACCESS_DENIED;
    }

    OldMode = (ULONG)ReadAcquire(&g_WfpEnforcementMode);
    WriteRelease(&g_WfpEnforcementMode, (LONG)NewMode);
    WfpEmitModeEvent(OldMode, NewMode, ProcessId, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}
