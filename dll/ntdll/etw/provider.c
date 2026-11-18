/*
 * PROJECT:     LiberNT Native API DLL
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Event Tracing for Windows user-mode provider registration
 * COPYRIGHT:   Copyright 2026 Ahmed Arif <arif.ing@outlook.com>
 */

#include <ntdll.h>

#include <evntprov.h>
#include <ntwmi.h>
#include <ndk/sefuncs.h>

typedef struct _ETW_USER_PROVIDER
{
    LIST_ENTRY Entry;
    HANDLE RegistrationHandle;
    GUID ProviderId;
    PENABLECALLBACK EnableCallback;
    PVOID CallbackContext;
    PVOID ProviderTraits;
    ULONG ProviderTraitsLength;
    BOOLEAN TrackBinary;
    BOOLEAN UseDescriptorType;
} ETW_USER_PROVIDER, *PETW_USER_PROVIDER;

static RTL_SRWLOCK EtwpProviderLock = RTL_SRWLOCK_INIT;
static LIST_ENTRY EtwpProviderList = {&EtwpProviderList, &EtwpProviderList};

static
PETW_USER_PROVIDER
EtwpFindProvider(
    _In_ REGHANDLE RegHandle)
{
    PLIST_ENTRY Link;
    PETW_USER_PROVIDER Provider;

    for (Link = EtwpProviderList.Flink; Link != &EtwpProviderList; Link = Link->Flink)
    {
        Provider = CONTAINING_RECORD(Link, ETW_USER_PROVIDER, Entry);
        if ((REGHANDLE)(ULONG_PTR)Provider == RegHandle) return Provider;
    }

    return NULL;
}

/* The registration handle and GUID of a provider, or FALSE for an unknown REGHANDLE */
static
BOOLEAN
EtwpLookupProvider(
    _In_ REGHANDLE RegHandle,
    _Out_opt_ HANDLE *RegistrationHandle,
    _Out_opt_ GUID *ProviderId)
{
    PETW_USER_PROVIDER Provider;

    RtlAcquireSRWLockShared(&EtwpProviderLock);
    Provider = EtwpFindProvider(RegHandle);
    if (Provider)
    {
        if (RegistrationHandle) *RegistrationHandle = Provider->RegistrationHandle;
        if (ProviderId) *ProviderId = Provider->ProviderId;
    }
    RtlReleaseSRWLockShared(&EtwpProviderLock);
    return Provider != NULL;
}

/* Would an event of this level and keyword reach a session? (asks the kernel) */
static
BOOLEAN
EtwpQueryEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    ETW_QUERY_ENABLE Query;
    HANDLE RegistrationHandle;
    ULONG Enabled = 0, ReturnSize;

    RtlZeroMemory(&Query, sizeof(Query));
    if (!EtwpLookupProvider(RegHandle, &RegistrationHandle, NULL)) return FALSE;
    Query.RegistrationHandle = (ULONG64)(ULONG_PTR)RegistrationHandle;
    Query.Level = Level;
    Query.Keyword = Keyword;
    if (!NT_SUCCESS(NtTraceControl(EtwQueryEnableCode, &Query, sizeof(Query), &Enabled, sizeof(Enabled), &ReturnSize)))
        return FALSE;
    return Enabled != 0;
}

/*
 * Hands the event to the kernel, which delivers it to every session the
 * provider is enabled for (and drops it when there is none). The data stays
 * in place: the kernel copies it from the descriptors.
 */
static
ULONG
EtwpWriteEvent(
    _In_ REGHANDLE RegHandle,
    _In_opt_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    struct
    {
        ETW_USER_EVENT_PACKET Header;
        EVENT_DATA_DESCRIPTOR More[ETW_MAX_EVENT_DATA_DESCRIPTORS - 1];
    } Packet;
    HANDLE RegistrationHandle;
    static const GUID NullGuid;
    NTSTATUS Status;

    if (!EtwpLookupProvider(RegHandle, &RegistrationHandle, NULL)) return ERROR_INVALID_HANDLE;
    if (!EventDescriptor || UserDataCount > ETW_MAX_EVENT_DATA_DESCRIPTORS || (UserDataCount && !UserData))
        return ERROR_INVALID_PARAMETER;

    RtlZeroMemory(&Packet.Header, FIELD_OFFSET(ETW_USER_EVENT_PACKET, UserData));
    Packet.Header.Descriptor = *EventDescriptor;
    /* Without an explicit one, the thread's activity id goes with the event */
    if (!ActivityId && RtlCompareMemory(&NtCurrentTeb()->ActivityId, &NullGuid, sizeof(GUID)) != sizeof(GUID))
        ActivityId = &NtCurrentTeb()->ActivityId;
    if (ActivityId)
    {
        Packet.Header.ActivityId = *ActivityId;
        Packet.Header.Flags |= ETW_USER_EVENT_ACTIVITY_ID;
    }
    if (RelatedActivityId)
    {
        Packet.Header.RelatedActivityId = *RelatedActivityId;
        Packet.Header.Flags |= ETW_USER_EVENT_RELATED_ACTIVITY_ID;
    }
    Packet.Header.UserDataCount = UserDataCount;
    if (UserDataCount)
        RtlCopyMemory(Packet.Header.UserData, UserData, UserDataCount * sizeof(EVENT_DATA_DESCRIPTOR));

    Status = NtTraceEvent(RegistrationHandle, ETW_NT_FLAGS_TRACE_EVENT,
                          FIELD_OFFSET(ETW_USER_EVENT_PACKET, UserData) + UserDataCount * sizeof(EVENT_DATA_DESCRIPTOR),
                          (PEVENT_TRACE_HEADER)&Packet);
    return NT_SUCCESS(Status) ? ERROR_SUCCESS : RtlNtStatusToDosError(Status);
}

static
ULONG
EtwpCreateActivityId(
    _Out_ GUID *ActivityId)
{
    ULARGE_INTEGER Time;
    ULONG Range;
    ULONG Sequence;
    UCHAR Seed[6];
    NTSTATUS Status;

    Status = NtAllocateUuids(&Time, &Range, &Sequence, Seed);
    if (!NT_SUCCESS(Status)) return RtlNtStatusToDosError(Status);

    ActivityId->Data1 = Time.LowPart;
    ActivityId->Data2 = (USHORT)Time.HighPart;
    ActivityId->Data3 = (USHORT)(((Time.HighPart >> 16) & 0x0fff) | 0x1000);
    ActivityId->Data4[0] = (UCHAR)(((Sequence >> 8) & 0x3f) | 0x80);
    ActivityId->Data4[1] = (UCHAR)Sequence;
    RtlCopyMemory(&ActivityId->Data4[2], Seed, sizeof(Seed));
    return ERROR_SUCCESS;
}

ULONG
WINAPI
EtwEventActivityIdControl(
    _In_ ULONG ControlCode,
    _Inout_ LPGUID ActivityId)
{
    GUID NewActivityId;
    GUID OldActivityId;
    ULONG Error;

    if (!ActivityId) return ERROR_INVALID_PARAMETER;

    switch (ControlCode)
    {
        case EVENT_ACTIVITY_CTRL_GET_ID:
            *ActivityId = NtCurrentTeb()->ActivityId;
            return ERROR_SUCCESS;

        case EVENT_ACTIVITY_CTRL_SET_ID:
            NtCurrentTeb()->ActivityId = *ActivityId;
            return ERROR_SUCCESS;

        case EVENT_ACTIVITY_CTRL_CREATE_ID:
            return EtwpCreateActivityId(ActivityId);

        case EVENT_ACTIVITY_CTRL_GET_SET_ID:
            OldActivityId = NtCurrentTeb()->ActivityId;
            NtCurrentTeb()->ActivityId = *ActivityId;
            *ActivityId = OldActivityId;
            return ERROR_SUCCESS;

        case EVENT_ACTIVITY_CTRL_CREATE_SET_ID:
            Error = EtwpCreateActivityId(&NewActivityId);
            if (Error != ERROR_SUCCESS) return Error;
            OldActivityId = NtCurrentTeb()->ActivityId;
            NtCurrentTeb()->ActivityId = NewActivityId;
            *ActivityId = OldActivityId;
            return ERROR_SUCCESS;

        default:
            return ERROR_INVALID_PARAMETER;
    }
}

BOOLEAN
WINAPI
EtwEventEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor)
{
    if (!EventDescriptor) return FALSE;
    return EtwpQueryEnabled(RegHandle, EventDescriptor->Level, EventDescriptor->Keyword);
}

BOOLEAN
WINAPI
EtwEventProviderEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    return EtwpQueryEnabled(RegHandle, Level, Keyword);
}

ULONG
WINAPI
EtwEventRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    PETW_USER_PROVIDER Provider;
    ETW_REGISTER_GUID_INFO Info;
    ULONG64 Handle;
    ULONG ReturnSize;
    NTSTATUS Status;

    if (!ProviderId || !RegHandle || (!EnableCallback && CallbackContext)) return ERROR_INVALID_PARAMETER;

    Provider = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Provider));
    if (!Provider) return ERROR_NOT_ENOUGH_MEMORY;

    RtlZeroMemory(&Info, sizeof(Info));
    Info.Guid = *ProviderId;
    Status = NtTraceControl(EtwRegisterGuidsCode, &Info, sizeof(Info), &Handle, sizeof(Handle), &ReturnSize);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Provider);
        return RtlNtStatusToDosError(Status);
    }

    Provider->RegistrationHandle = (HANDLE)(ULONG_PTR)Handle;
    Provider->ProviderId = *ProviderId;
    Provider->EnableCallback = EnableCallback;
    Provider->CallbackContext = CallbackContext;
    RtlAcquireSRWLockExclusive(&EtwpProviderLock);
    InsertTailList(&EtwpProviderList, &Provider->Entry);
    RtlReleaseSRWLockExclusive(&EtwpProviderLock);
    *RegHandle = (REGHANDLE)(ULONG_PTR)Provider;
    return ERROR_SUCCESS;
}

ULONG
WINAPI
EtwEventSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ EVENT_INFO_CLASS InformationClass,
    _In_reads_bytes_opt_(InformationLength) PVOID Information,
    _In_ ULONG InformationLength)
{
    PETW_USER_PROVIDER Provider;
    PVOID ProviderTraits = NULL;
    USHORT ProviderTraitsLength;
    ULONG NameIndex;
    ULONG Error = ERROR_SUCCESS;

    if (InformationClass >= MaxEventInfo) return ERROR_INVALID_PARAMETER;
    if (InformationClass == EventProviderSetReserved1) return ERROR_NOT_SUPPORTED;

    switch (InformationClass)
    {
        case EventProviderBinaryTrackInfo:
            if (Information || InformationLength) return ERROR_INVALID_PARAMETER;
            break;

        case EventProviderSetTraits:
            if (!Information || InformationLength < sizeof(USHORT) + 2) return ERROR_INVALID_PARAMETER;
            RtlCopyMemory(&ProviderTraitsLength, Information, sizeof(ProviderTraitsLength));
            if (ProviderTraitsLength != InformationLength) return ERROR_FILE_CORRUPT;
            for (NameIndex = sizeof(USHORT); NameIndex < InformationLength && ((PUCHAR)Information)[NameIndex]; NameIndex++) { }
            if (NameIndex == sizeof(USHORT) || NameIndex == InformationLength) return ERROR_FILE_CORRUPT;
            ProviderTraits = RtlAllocateHeap(RtlGetProcessHeap(), 0, InformationLength);
            if (!ProviderTraits) return ERROR_NOT_ENOUGH_MEMORY;
            RtlCopyMemory(ProviderTraits, Information, InformationLength);
            break;

        case EventProviderUseDescriptorType:
            if (!Information || InformationLength != sizeof(BOOLEAN)) return ERROR_INVALID_PARAMETER;
            break;

        default:
            return ERROR_INVALID_PARAMETER;
    }

    RtlAcquireSRWLockExclusive(&EtwpProviderLock);
    Provider = EtwpFindProvider(RegHandle);
    if (!Provider)
    {
        Error = ERROR_INVALID_HANDLE;
    }
    else if (InformationClass == EventProviderBinaryTrackInfo)
    {
        Provider->TrackBinary = TRUE;
    }
    else if (InformationClass == EventProviderSetTraits)
    {
        if (Provider->ProviderTraits) RtlFreeHeap(RtlGetProcessHeap(), 0, Provider->ProviderTraits);
        Provider->ProviderTraits = ProviderTraits;
        Provider->ProviderTraitsLength = InformationLength;
        ProviderTraits = NULL;
    }
    else
    {
        Provider->UseDescriptorType = *(PBOOLEAN)Information;
    }
    RtlReleaseSRWLockExclusive(&EtwpProviderLock);
    if (ProviderTraits) RtlFreeHeap(RtlGetProcessHeap(), 0, ProviderTraits);
    return Error;
}

ULONG
WINAPI
EtwEventUnregister(
    _In_ REGHANDLE RegHandle)
{
    PETW_USER_PROVIDER Provider;

    RtlAcquireSRWLockExclusive(&EtwpProviderLock);
    Provider = EtwpFindProvider(RegHandle);
    if (!Provider)
    {
        RtlReleaseSRWLockExclusive(&EtwpProviderLock);
        return ERROR_INVALID_HANDLE;
    }

    RemoveEntryList(&Provider->Entry);
    RtlReleaseSRWLockExclusive(&EtwpProviderLock);
    NtClose(Provider->RegistrationHandle);
    if (Provider->ProviderTraits) RtlFreeHeap(RtlGetProcessHeap(), 0, Provider->ProviderTraits);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Provider);
    return ERROR_SUCCESS;
}

ULONG
WINAPI
EtwEventWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    return EtwpWriteEvent(RegHandle, EventDescriptor, NULL, NULL, UserDataCount, UserData);
}

ULONG
WINAPI
EtwEventWriteEx(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG64 Filter,
    _In_ ULONG Flags,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(Flags);
    return EtwpWriteEvent(RegHandle, EventDescriptor, ActivityId, RelatedActivityId, UserDataCount, UserData);
}

ULONG
WINAPI
EtwEventWriteString(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_ PCWSTR String)
{
    EVENT_DESCRIPTOR Descriptor;
    EVENT_DATA_DESCRIPTOR Data;
    SIZE_T Length;

    if (!EtwpLookupProvider(RegHandle, NULL, NULL)) return ERROR_INVALID_HANDLE;
    if (!String) return ERROR_INVALID_PARAMETER;
    Length = (wcslen(String) + 1) * sizeof(WCHAR);
    if (Length > MAXUSHORT) return ERROR_ARITHMETIC_OVERFLOW;

    /* a string event carries only level and keyword in its descriptor */
    RtlZeroMemory(&Descriptor, sizeof(Descriptor));
    Descriptor.Level = Level;
    Descriptor.Keyword = Keyword;
    Data.Ptr = (ULONGLONG)(ULONG_PTR)String;
    Data.Size = (ULONG)Length;
    Data.Reserved = 0;
    return EtwpWriteEvent(RegHandle, &Descriptor, NULL, NULL, 1, &Data);
}

ULONG
WINAPI
EtwEventWriteTransfer(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    return EtwpWriteEvent(RegHandle, EventDescriptor, ActivityId, RelatedActivityId, UserDataCount, UserData);
}

BOOLEAN
NTAPI
EtwCheckCoverage(
    _Inout_ PVOID CoveragePoint)
{
    UNREFERENCED_PARAMETER(CoveragePoint);

    return FALSE;
}
