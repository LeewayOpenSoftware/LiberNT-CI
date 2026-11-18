/*
 * PROJECT:     Local Security Authority Server DLL
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        dll/win32/lsasrv/session.c
 * PURPOSE:     Logon session management routines
 * COPYRIGHT:   Copyright 2013 Eric Kohl
 */

#include "lsasrv.h"

typedef struct _LSAP_LOGON_SESSION
{
    LIST_ENTRY Entry;
    LUID LogonId;
    ULONG LogonType;
    ULONG Session;
    LARGE_INTEGER LogonTime;
    PSID Sid;
    UNICODE_STRING UserName;
    UNICODE_STRING LogonDomain;
    UNICODE_STRING AuthenticationPackage;
    UNICODE_STRING LogonServer;
    UNICODE_STRING DnsDomainName;
    UNICODE_STRING Upn;
    LIST_ENTRY CredentialListHead;   /* LSAP_CREDENTIAL entries; guarded by LogonSessionLock */
} LSAP_LOGON_SESSION, *PLSAP_LOGON_SESSION;

/*
 * A credential an authentication package attached to a logon session through
 * LsaAddCredential, keyed by (package id, PrimaryKeyValue). The value is secret
 * material (e.g. the DPAPI pre-key msv1_0 derives from the logon password) and
 * is wiped before its storage is released.
 */
typedef struct _LSAP_CREDENTIAL
{
    LIST_ENTRY Entry;
    ULONG AuthenticationPackage;
    STRING PrimaryKeyValue;
    STRING Credential;
} LSAP_CREDENTIAL, *PLSAP_CREDENTIAL;


typedef struct _LSAP_LOGON_SESSION_DATA32
{
    ULONG Size;
    LUID LogonId;
    UNICODE_STRING32 UserName;
    UNICODE_STRING32 LogonDomain;
    UNICODE_STRING32 AuthenticationPackage;
    ULONG LogonType;
    ULONG Session;
    ULONG Sid;
    LARGE_INTEGER LogonTime;
    UNICODE_STRING32 LogonServer;
    UNICODE_STRING32 DnsDomainName;
    UNICODE_STRING32 Upn;
#if (_WIN32_WINNT >= 0x0600)
    ULONG UserFlags;
    LSA_LAST_INTER_LOGON_INFO LastLogonInfo;
    UNICODE_STRING32 LogonScript;
    UNICODE_STRING32 ProfilePath;
    UNICODE_STRING32 HomeDirectory;
    UNICODE_STRING32 HomeDirectoryDrive;
    LARGE_INTEGER LogoffTime;
    LARGE_INTEGER KickOffTime;
    LARGE_INTEGER PasswordLastSet;
    LARGE_INTEGER PasswordCanChange;
    LARGE_INTEGER PasswordMustChange;
#endif
} LSAP_LOGON_SESSION_DATA32;

static VOID
LsapCopyString32(PUNICODE_STRING32 Destination, const UNICODE_STRING *Source)
{
    Destination->Length = Source->Length;
    Destination->MaximumLength = Source->MaximumLength;
    Destination->Buffer = (ULONG)(ULONG_PTR)Source->Buffer;
}

/* GLOBALS *****************************************************************/

LIST_ENTRY SessionListHead;
ULONG SessionCount;

static RTL_SRWLOCK LogonSessionLock;

/* FUNCTIONS ***************************************************************/

VOID
LsapInitLogonSessions(VOID)
{
    InitializeListHead(&SessionListHead);
    SessionCount = 0;
}


/* Credential helpers (caller holds LogonSessionLock) ***********************/

static
VOID
LsapFreeCredential(
    _In_ PLSAP_CREDENTIAL Credential)
{
    if (Credential->Credential.Buffer != NULL)
    {
        RtlSecureZeroMemory(Credential->Credential.Buffer,
                            Credential->Credential.MaximumLength);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Credential->Credential.Buffer);
    }

    if (Credential->PrimaryKeyValue.Buffer != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Credential->PrimaryKeyValue.Buffer);

    RtlFreeHeap(RtlGetProcessHeap(), 0, Credential);
}

static
VOID
LsapFreeSessionCredentials(
    _In_ PLSAP_LOGON_SESSION Session)
{
    while (!IsListEmpty(&Session->CredentialListHead))
    {
        PLIST_ENTRY Entry = RemoveHeadList(&Session->CredentialListHead);

        LsapFreeCredential(CONTAINING_RECORD(Entry, LSAP_CREDENTIAL, Entry));
    }
}

/*
 * Copies an LSA_STRING into process-heap storage (the LSA heap). One extra
 * zero byte is appended so text keys can also be read as C strings.
 */
static
NTSTATUS
LsapCopyLsaString(
    _Out_ PSTRING Destination,
    _In_opt_ PLSA_STRING Source)
{
    Destination->Length = 0;
    Destination->MaximumLength = 0;
    Destination->Buffer = NULL;

    if (Source == NULL || Source->Buffer == NULL || Source->Length == 0)
        return STATUS_SUCCESS;

    if (Source->Length == MAXUSHORT)
        return STATUS_INVALID_PARAMETER;

    Destination->Buffer = RtlAllocateHeap(RtlGetProcessHeap(),
                                          HEAP_ZERO_MEMORY,
                                          (SIZE_T)Source->Length + sizeof(CHAR));
    if (Destination->Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Destination->Buffer, Source->Buffer, Source->Length);
    Destination->Length = Source->Length;
    Destination->MaximumLength = Source->Length + sizeof(CHAR);

    return STATUS_SUCCESS;
}

static
PLSAP_CREDENTIAL
LsapFindCredential(
    _In_ PLSAP_LOGON_SESSION Session,
    _In_ ULONG AuthenticationPackage,
    _In_ PLSA_STRING PrimaryKeyValue)
{
    PLIST_ENTRY Entry;

    for (Entry = Session->CredentialListHead.Flink;
         Entry != &Session->CredentialListHead;
         Entry = Entry->Flink)
    {
        PLSAP_CREDENTIAL Credential = CONTAINING_RECORD(Entry, LSAP_CREDENTIAL, Entry);

        if (Credential->AuthenticationPackage != AuthenticationPackage)
            continue;

        if (Credential->PrimaryKeyValue.Length != PrimaryKeyValue->Length)
            continue;

        if (PrimaryKeyValue->Length != 0 &&
            RtlCompareMemory(Credential->PrimaryKeyValue.Buffer,
                             PrimaryKeyValue->Buffer,
                             PrimaryKeyValue->Length) != PrimaryKeyValue->Length)
        {
            continue;
        }

        return Credential;
    }

    return NULL;
}

VOID
LsapLockLogonSessions(VOID)
{
    RtlAcquireSRWLockExclusive(&LogonSessionLock);
}

VOID
LsapUnlockLogonSessions(VOID)
{
    RtlReleaseSRWLockExclusive(&LogonSessionLock);
}


static
PLSAP_LOGON_SESSION
LsapGetLogonSession(IN PLUID LogonId)
{
    PLIST_ENTRY SessionEntry;
    PLSAP_LOGON_SESSION CurrentSession;

    SessionEntry = SessionListHead.Flink;
    while (SessionEntry != &SessionListHead)
    {
        CurrentSession = CONTAINING_RECORD(SessionEntry,
                                           LSAP_LOGON_SESSION,
                                           Entry);
        if (RtlEqualLuid(&CurrentSession->LogonId, LogonId))
            return CurrentSession;

        SessionEntry = SessionEntry->Flink;
    }

    return NULL;
}


NTSTATUS
LsapSetLogonSessionData(
    _In_ PLUID LogonId,
    _In_ ULONG LogonType,
    _In_ PUNICODE_STRING UserName,
    _In_ PUNICODE_STRING LogonDomain,
    _In_ PSID Sid,
    _In_ ULONG SessionId)
{
    NTSTATUS Status;
    PLSAP_LOGON_SESSION Session;
    ULONG Length;

    TRACE("LsapSetLogonSessionData(%p)\n", LogonId);

    Session = LsapGetLogonSession(LogonId);
    if (Session == NULL)
        return STATUS_NO_SUCH_LOGON_SESSION;

    TRACE("LogonType %lu\n", LogonType);
    Session->LogonType = LogonType;
    Session->Session = SessionId;
    NtQuerySystemTime(&Session->LogonTime);

    Status = RtlValidateUnicodeString(0, UserName);
    if (!NT_SUCCESS(Status) || UserName == NULL)
        return STATUS_INVALID_PARAMETER;

    /* UserName is mandatory and cannot be an empty string */
    TRACE("UserName %wZ\n", UserName);
    Session->UserName.Buffer = RtlAllocateHeap(RtlGetProcessHeap(),
                                               HEAP_ZERO_MEMORY,
                                               UserName->MaximumLength);
    if (Session->UserName.Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Session->UserName.Length = UserName->Length;
    Session->UserName.MaximumLength = UserName->MaximumLength;
    RtlCopyMemory(Session->UserName.Buffer, UserName->Buffer, UserName->MaximumLength);

    Status = RtlValidateUnicodeString(0, LogonDomain);
    if (!NT_SUCCESS(Status))
    {
        /* Cleanup and fail */
        if (Session->UserName.Buffer != NULL)
            RtlFreeHeap(RtlGetProcessHeap(), 0, Session->UserName.Buffer);

        return STATUS_INVALID_PARAMETER;
    }

    /* LogonDomain is optional and can be an empty string */
    TRACE("LogonDomain %wZ\n", LogonDomain);
    if (LogonDomain != NULL && LogonDomain->Length)
    {
        Session->LogonDomain.Buffer = RtlAllocateHeap(RtlGetProcessHeap(),
                                                      HEAP_ZERO_MEMORY,
                                                      LogonDomain->MaximumLength);
        if (Session->LogonDomain.Buffer == NULL)
        {
            /* Cleanup and fail */
            if (Session->UserName.Buffer != NULL)
                RtlFreeHeap(RtlGetProcessHeap(), 0, Session->UserName.Buffer);

            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Session->LogonDomain.Length = LogonDomain->Length;
        Session->LogonDomain.MaximumLength = LogonDomain->MaximumLength;
        RtlCopyMemory(Session->LogonDomain.Buffer, LogonDomain->Buffer, LogonDomain->MaximumLength);
    }
    else
    {
        RtlInitEmptyUnicodeString(&Session->LogonDomain, NULL, 0);
    }

    Length = RtlLengthSid(Sid);
    Session->Sid = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Length);
    if (Session->Sid == NULL)
    {
        /* Cleanup and fail */
        if (Session->LogonDomain.Buffer != NULL)
            RtlFreeHeap(RtlGetProcessHeap(), 0, Session->LogonDomain.Buffer);
        if (Session->UserName.Buffer != NULL)
            RtlFreeHeap(RtlGetProcessHeap(), 0, Session->UserName.Buffer);

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(Session->Sid, Sid, Length);

    return STATUS_SUCCESS;
}


NTSTATUS
NTAPI
LsapCreateLogonSession(IN PLUID LogonId)
{
    PLSAP_LOGON_SESSION Session;
    NTSTATUS Status;

    TRACE("LsapCreateLogonSession(%p)\n", LogonId);

    /* Fail, if a session already exists */
    if (LsapGetLogonSession(LogonId) != NULL)
        return STATUS_LOGON_SESSION_COLLISION;

    /* Allocate a new session entry */
    Session = RtlAllocateHeap(RtlGetProcessHeap(),
                              HEAP_ZERO_MEMORY,
                              sizeof(LSAP_LOGON_SESSION));
    if (Session == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Initialize the session entry */
    RtlCopyLuid(&Session->LogonId, LogonId);

    TRACE("LsapCreateLogonSession(<0x%lx,0x%lx>)\n",
          LogonId->HighPart, LogonId->LowPart);

    /* Tell ntoskrnl to create a new logon session */
    Status = LsapRmCreateLogonSession(LogonId);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Session);
        return Status;
    }

    InitializeListHead(&Session->CredentialListHead);

    /* Insert the new session into the session list */
    InsertHeadList(&SessionListHead, &Session->Entry);
    SessionCount++;

    return STATUS_SUCCESS;
}


NTSTATUS
NTAPI
LsapDeleteLogonSession(IN PLUID LogonId)
{
    PLSAP_LOGON_SESSION Session;
    NTSTATUS Status;

    TRACE("LsapDeleteLogonSession(%p)\n", LogonId);

    /* Fail, if the session does not exist */
    Session = LsapGetLogonSession(LogonId);
    if (Session == NULL)
        return STATUS_NO_SUCH_LOGON_SESSION;

    TRACE("LsapDeleteLogonSession(0x%08lx%08lx)\n",
          LogonId->HighPart, LogonId->LowPart);

    /* Tell ntoskrnl to delete the logon session */
    Status = LsapRmDeleteLogonSession(LogonId);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Notify the authentication packages */
    LsapTerminateLogon(LogonId);

    /* Remove the session entry from the list */
    RemoveEntryList(&Session->Entry);
    SessionCount--;

    /* Wipe and release the credentials the packages attached to this session */
    LsapFreeSessionCredentials(Session);

    /* Free the session data */
    if (Session->Sid != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Session->Sid);

    if (Session->UserName.Buffer != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Session->UserName.Buffer);

    if (Session->LogonDomain.Buffer != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Session->LogonDomain.Buffer);

    if (Session->AuthenticationPackage.Buffer != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Session->AuthenticationPackage.Buffer);

    if (Session->LogonServer.Buffer != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Session->LogonServer.Buffer);

    if (Session->DnsDomainName.Buffer != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Session->DnsDomainName.Buffer);

    if (Session->Upn.Buffer != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Session->Upn.Buffer);

    /* Free the session entry */
    RtlFreeHeap(RtlGetProcessHeap(), 0, Session);

    return STATUS_SUCCESS;
}


NTSTATUS
NTAPI
LsapAddCredential(
    _In_ PLUID LogonId,
    _In_ ULONG AuthenticationPackage,
    _In_ PLSA_STRING PrimaryKeyValue,
    _In_ PLSA_STRING Credential)
{
    PLSAP_LOGON_SESSION Session;
    PLSAP_CREDENTIAL Entry = NULL;
    STRING NewValue;
    NTSTATUS Status;

    TRACE("LsapAddCredential(%p %lu %p %p)\n",
          LogonId, AuthenticationPackage, PrimaryKeyValue, Credential);

    /*
     * LogonSessionLock is held by the caller: the authentication port
     * dispatcher (authport.c) acquires it around every package call, and
     * lsasrv-internal callers take it themselves before calling in here.
     */
    if (LogonId == NULL || PrimaryKeyValue == NULL || Credential == NULL)
        return STATUS_INVALID_PARAMETER;

    Session = LsapGetLogonSession(LogonId);
    if (Session == NULL)
        return STATUS_NO_SUCH_LOGON_SESSION;

    /* Copy the new value first so a failure leaves an existing credential intact */
    Status = LsapCopyLsaString(&NewValue, Credential);
    if (!NT_SUCCESS(Status))
        return Status;

    Entry = LsapFindCredential(Session, AuthenticationPackage, PrimaryKeyValue);
    if (Entry != NULL)
    {
        /* Replace the value of the existing credential */
        if (Entry->Credential.Buffer != NULL)
        {
            RtlSecureZeroMemory(Entry->Credential.Buffer, Entry->Credential.MaximumLength);
            RtlFreeHeap(RtlGetProcessHeap(), 0, Entry->Credential.Buffer);
        }
        Entry->Credential = NewValue;
        return STATUS_SUCCESS;
    }

    Entry = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LSAP_CREDENTIAL));
    if (Entry == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }

    Status = LsapCopyLsaString(&Entry->PrimaryKeyValue, PrimaryKeyValue);
    if (!NT_SUCCESS(Status))
        goto fail;

    Entry->AuthenticationPackage = AuthenticationPackage;
    Entry->Credential = NewValue;
    InsertTailList(&Session->CredentialListHead, &Entry->Entry);

    return STATUS_SUCCESS;

fail:
    if (Entry != NULL)
    {
        if (Entry->PrimaryKeyValue.Buffer != NULL)
            RtlFreeHeap(RtlGetProcessHeap(), 0, Entry->PrimaryKeyValue.Buffer);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Entry);
    }

    if (NewValue.Buffer != NULL)
    {
        RtlSecureZeroMemory(NewValue.Buffer, NewValue.MaximumLength);
        RtlFreeHeap(RtlGetProcessHeap(), 0, NewValue.Buffer);
    }

    return Status;
}


NTSTATUS
NTAPI
LsapGetCredentials(
    _In_ PLUID LogonId,
    _In_ ULONG AuthenticationPackage,
    _Inout_ PULONG QueryContext,
    _In_ BOOLEAN RetrieveAllCredentials,
    _Inout_ PLSA_STRING PrimaryKeyValue,
    _Out_ PULONG PrimaryKeyLength,
    _Out_ PLSA_STRING Credentials)
{
    PLSAP_LOGON_SESSION Session;
    PLSAP_CREDENTIAL Found = NULL;
    NTSTATUS Status;

    TRACE("LsapGetCredentials(%p %lu %p %u %p %p %p)\n",
          LogonId, AuthenticationPackage, QueryContext, RetrieveAllCredentials,
          PrimaryKeyValue, PrimaryKeyLength, Credentials);

    /* LogonSessionLock is held by the caller (see LsapAddCredential) */
    if (LogonId == NULL || QueryContext == NULL || PrimaryKeyValue == NULL ||
        PrimaryKeyLength == NULL || Credentials == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Credentials->Length = 0;
    Credentials->MaximumLength = 0;
    Credentials->Buffer = NULL;
    *PrimaryKeyLength = 0;

    Session = LsapGetLogonSession(LogonId);
    if (Session == NULL)
        return STATUS_NO_SUCH_LOGON_SESSION;

    if (RetrieveAllCredentials)
    {
        /* *QueryContext enumerates this package's credentials in list order */
        PLIST_ENTRY Entry;
        ULONG Index = 0;

        for (Entry = Session->CredentialListHead.Flink;
             Entry != &Session->CredentialListHead;
             Entry = Entry->Flink)
        {
            PLSAP_CREDENTIAL Credential = CONTAINING_RECORD(Entry, LSAP_CREDENTIAL, Entry);

            if (Credential->AuthenticationPackage != AuthenticationPackage)
                continue;

            if (Index++ < *QueryContext)
                continue;

            Found = Credential;
            break;
        }

        if (Found == NULL)
            return STATUS_NO_MORE_ENTRIES;

        /* Hand the key back in the caller's buffer */
        *PrimaryKeyLength = Found->PrimaryKeyValue.Length;
        if (PrimaryKeyValue->MaximumLength < Found->PrimaryKeyValue.Length ||
            (PrimaryKeyValue->Buffer == NULL && Found->PrimaryKeyValue.Length != 0))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        if (Found->PrimaryKeyValue.Length != 0)
        {
            RtlCopyMemory(PrimaryKeyValue->Buffer,
                          Found->PrimaryKeyValue.Buffer,
                          Found->PrimaryKeyValue.Length);
        }
        PrimaryKeyValue->Length = Found->PrimaryKeyValue.Length;
    }
    else
    {
        Found = LsapFindCredential(Session, AuthenticationPackage, PrimaryKeyValue);
        if (Found == NULL)
            return STATUS_NOT_FOUND;

        *PrimaryKeyLength = Found->PrimaryKeyValue.Length;
    }

    /* The caller owns the returned copy (LSA heap == process heap) */
    Status = LsapCopyLsaString(Credentials, &Found->Credential);
    if (NT_SUCCESS(Status) && RetrieveAllCredentials)
        (*QueryContext)++;

    return Status;
}


NTSTATUS
NTAPI
LsapDeleteCredential(
    _In_ PLUID LogonId,
    _In_ ULONG AuthenticationPackage,
    _In_ PLSA_STRING PrimaryKeyValue)
{
    PLSAP_LOGON_SESSION Session;
    PLSAP_CREDENTIAL Credential;

    TRACE("LsapDeleteCredential(%p %lu %p)\n",
          LogonId, AuthenticationPackage, PrimaryKeyValue);

    /* LogonSessionLock is held by the caller (see LsapAddCredential) */
    if (LogonId == NULL || PrimaryKeyValue == NULL)
        return STATUS_INVALID_PARAMETER;

    Session = LsapGetLogonSession(LogonId);
    if (Session == NULL)
        return STATUS_NO_SUCH_LOGON_SESSION;

    Credential = LsapFindCredential(Session, AuthenticationPackage, PrimaryKeyValue);
    if (Credential == NULL)
        return STATUS_NOT_FOUND;

    RemoveEntryList(&Credential->Entry);
    LsapFreeCredential(Credential);

    return STATUS_SUCCESS;
}


NTSTATUS
NTAPI
LsapGetDpapiPrekey(
    _In_ PLUID LogonId,
    _Out_ PLSA_STRING Prekey)
{
    PLSAP_LOGON_SESSION Session;
    PLIST_ENTRY Entry;
    NTSTATUS Status = STATUS_NOT_FOUND;

    Prekey->Length = 0;
    Prekey->MaximumLength = 0;
    Prekey->Buffer = NULL;

    RtlAcquireSRWLockExclusive(&LogonSessionLock);

    Session = LsapGetLogonSession(LogonId);
    if (Session != NULL)
    {
        for (Entry = Session->CredentialListHead.Flink;
             Entry != &Session->CredentialListHead;
             Entry = Entry->Flink)
        {
            PLSAP_CREDENTIAL Credential = CONTAINING_RECORD(Entry, LSAP_CREDENTIAL, Entry);

            if (Credential->PrimaryKeyValue.Length == sizeof("Primary") - 1 &&
                RtlCompareMemory(Credential->PrimaryKeyValue.Buffer, "Primary",
                                 sizeof("Primary") - 1) == sizeof("Primary") - 1 &&
                Credential->Credential.Length != 0)
            {
                Prekey->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0,
                                                 Credential->Credential.Length);
                if (Prekey->Buffer == NULL)
                {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    break;
                }

                RtlCopyMemory(Prekey->Buffer, Credential->Credential.Buffer,
                              Credential->Credential.Length);
                Prekey->Length = Credential->Credential.Length;
                Prekey->MaximumLength = Credential->Credential.Length;
                Status = STATUS_SUCCESS;
                break;
            }
        }
    }

    RtlReleaseSRWLockExclusive(&LogonSessionLock);
    return Status;
}


NTSTATUS
LsapEnumLogonSessions(IN OUT PLSA_API_MSG RequestMsg)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE ProcessHandle = NULL;
    PLIST_ENTRY SessionEntry;
    PLSAP_LOGON_SESSION CurrentSession;
    PLUID SessionList;
    ULONG i, Length;
    SIZE_T MemSize;
    PVOID ClientBaseAddress = NULL;
    ULONG PointerSize = RequestMsg->EnumLogonSessions.Request.PointerSize;
    NTSTATUS Status;

    TRACE("LsapEnumLogonSessions(%p)\n", RequestMsg);

    if (RequestMsg->EnumLogonSessions.Request.PointerSize != sizeof(ULONG) &&
        RequestMsg->EnumLogonSessions.Request.PointerSize != sizeof(ULONGLONG))
        return STATUS_INVALID_PARAMETER;

    if (SessionCount == 0)
    {
        RequestMsg->EnumLogonSessions.Reply.LogonSessionCount = 0;
        RequestMsg->EnumLogonSessions.Reply.Reserved = 0;
        RequestMsg->EnumLogonSessions.Reply.LogonSessionBuffer = 0;
        return STATUS_SUCCESS;
    }

    Length = SessionCount * sizeof(LUID);
    SessionList = RtlAllocateHeap(RtlGetProcessHeap(),
                                  HEAP_ZERO_MEMORY,
                                  Length);
    if (SessionList == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    i = 0;
    SessionEntry = SessionListHead.Flink;
    while (SessionEntry != &SessionListHead)
    {
        CurrentSession = CONTAINING_RECORD(SessionEntry,
                                           LSAP_LOGON_SESSION,
                                           Entry);

        RtlCopyLuid(&SessionList[i],
                    &CurrentSession->LogonId);

        SessionEntry = SessionEntry->Flink;
        i++;
    }

    InitializeObjectAttributes(&ObjectAttributes,
                               NULL,
                               0,
                               NULL,
                               NULL);

    Status = NtOpenProcess(&ProcessHandle,
                           PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
                           &ObjectAttributes,
                           &RequestMsg->h.ClientId);
    if (!NT_SUCCESS(Status))
    {
        TRACE("NtOpenProcess() failed (Status %lx)\n", Status);
        goto done;
    }

    TRACE("Length: %lu\n", Length);

    MemSize = Length;
    Status = NtAllocateVirtualMemory(ProcessHandle,
                                     &ClientBaseAddress,
                                     PointerSize == sizeof(ULONG) && sizeof(PVOID) > sizeof(ULONG) ? MAXULONG : 0,
                                     &MemSize,
                                     MEM_COMMIT,
                                     PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
    {
        TRACE("NtAllocateVirtualMemory() failed (Status %lx)\n", Status);
        goto done;
    }

    TRACE("MemSize: %lu\n", MemSize);
    TRACE("ClientBaseAddress: %p\n", ClientBaseAddress);

    Status = NtWriteVirtualMemory(ProcessHandle,
                                  ClientBaseAddress,
                                  SessionList,
                                  Length,
                                  NULL);
    if (!NT_SUCCESS(Status))
    {
        TRACE("NtWriteVirtualMemory() failed (Status %lx)\n", Status);
        goto done;
    }

    RequestMsg->EnumLogonSessions.Reply.LogonSessionCount = SessionCount;
    RequestMsg->EnumLogonSessions.Reply.Reserved = 0;
    RequestMsg->EnumLogonSessions.Reply.LogonSessionBuffer = (ULONGLONG)(ULONG_PTR)ClientBaseAddress;

done:
    if (ProcessHandle != NULL)
        NtClose(ProcessHandle);

    if (SessionList != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, SessionList);

    return Status;
}


NTSTATUS
LsapGetLogonSessionData(IN OUT PLSA_API_MSG RequestMsg)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE ProcessHandle = NULL;
    PLSAP_LOGON_SESSION Session;
    PSECURITY_LOGON_SESSION_DATA LocalSessionData;
    LSAP_LOGON_SESSION_DATA32 Data32;
    PVOID ClientBaseAddress = NULL;
    ULONG PointerSize = RequestMsg->GetLogonSessionData.Request.PointerSize;
    ULONG TotalLength, SidLength = 0;
    SIZE_T MemSize;
    PUCHAR Ptr;
    NTSTATUS Status;

    TRACE("LsapGetLogonSessionData(%p)\n", RequestMsg);

    if (PointerSize != sizeof(ULONG) && PointerSize != sizeof(ULONGLONG))
        return STATUS_INVALID_PARAMETER;

    TRACE("LogonId: %lx\n", RequestMsg->GetLogonSessionData.Request.LogonId.LowPart);
    Session = LsapGetLogonSession(&RequestMsg->GetLogonSessionData.Request.LogonId);
    if (Session == NULL)
        return STATUS_NO_SUCH_LOGON_SESSION;

    /* Calculate the required buffer size */
    TotalLength = sizeof(SECURITY_LOGON_SESSION_DATA) +
                  Session->UserName.MaximumLength +
                  Session->LogonDomain.MaximumLength +
                  Session->AuthenticationPackage.MaximumLength +
                  Session->LogonServer.MaximumLength +
                  Session->DnsDomainName.MaximumLength +
                  Session->Upn.MaximumLength;
    if (Session->Sid != NULL)
    {
        SidLength = RtlLengthSid(Session->Sid);
        TotalLength += SidLength;
    }
    TRACE("TotalLength: %lu\n", TotalLength);

    /* Allocate the buffer */
    LocalSessionData = RtlAllocateHeap(RtlGetProcessHeap(),
                                       HEAP_ZERO_MEMORY,
                                       TotalLength);
    if (LocalSessionData == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Ptr = (PUCHAR)((ULONG_PTR)LocalSessionData + sizeof(SECURITY_LOGON_SESSION_DATA));
    TRACE("LocalSessionData: %p  Ptr: %p\n", LocalSessionData, Ptr);

    LocalSessionData->Size = sizeof(SECURITY_LOGON_SESSION_DATA);

    /* Copy the LogonId */
    RtlCopyLuid(&LocalSessionData->LogonId,
                &RequestMsg->GetLogonSessionData.Request.LogonId);

    /* Copy the UserName string */
    LocalSessionData->UserName.Length = Session->UserName.Length;
    LocalSessionData->UserName.MaximumLength = Session->UserName.MaximumLength;
    if (Session->UserName.MaximumLength != 0)
    {
        RtlCopyMemory(Ptr, Session->UserName.Buffer, Session->UserName.MaximumLength);
        LocalSessionData->UserName.Buffer = (PWSTR)((ULONG_PTR)Ptr - (ULONG_PTR)LocalSessionData);

        Ptr = (PUCHAR)((ULONG_PTR)Ptr + Session->UserName.MaximumLength);
    }

    /* Copy the LogonDomain string */
    LocalSessionData->LogonDomain.Length = Session->LogonDomain.Length;
    LocalSessionData->LogonDomain.MaximumLength = Session->LogonDomain.MaximumLength;
    if (Session->LogonDomain.MaximumLength != 0)
    {
        RtlCopyMemory(Ptr, Session->LogonDomain.Buffer, Session->LogonDomain.MaximumLength);
        LocalSessionData->LogonDomain.Buffer = (PWSTR)((ULONG_PTR)Ptr - (ULONG_PTR)LocalSessionData);

        Ptr = (PUCHAR)((ULONG_PTR)Ptr + Session->LogonDomain.MaximumLength);
    }

    /* Copy the AuthenticationPackage string */
    LocalSessionData->AuthenticationPackage.Length = Session->AuthenticationPackage.Length;
    LocalSessionData->AuthenticationPackage.MaximumLength = Session->AuthenticationPackage.MaximumLength;
    if (Session->AuthenticationPackage.MaximumLength != 0)
    {
        RtlCopyMemory(Ptr, Session->AuthenticationPackage.Buffer, Session->AuthenticationPackage.MaximumLength);
        LocalSessionData->AuthenticationPackage.Buffer = (PWSTR)((ULONG_PTR)Ptr - (ULONG_PTR)LocalSessionData);

        Ptr = (PUCHAR)((ULONG_PTR)Ptr + Session->AuthenticationPackage.MaximumLength);
    }

    LocalSessionData->LogonType = Session->LogonType;
    LocalSessionData->Session = Session->Session;

    /* Sid */
    if (Session->Sid != NULL)
    {
        RtlCopyMemory(Ptr, Session->Sid, SidLength);
        LocalSessionData->Sid = (PSID)((ULONG_PTR)Ptr - (ULONG_PTR)LocalSessionData);

        Ptr = (PUCHAR)((ULONG_PTR)Ptr + SidLength);
    }

    /* LogonTime */
    LocalSessionData->LogonTime.QuadPart = Session->LogonTime.QuadPart;

    /* Copy the LogonServer string */
    LocalSessionData->LogonServer.Length = Session->LogonServer.Length;
    LocalSessionData->LogonServer.MaximumLength = Session->LogonServer.MaximumLength;
    if (Session->LogonServer.MaximumLength != 0)
    {
        RtlCopyMemory(Ptr, Session->LogonServer.Buffer, Session->LogonServer.MaximumLength);
        LocalSessionData->LogonServer.Buffer = (PWSTR)((ULONG_PTR)Ptr - (ULONG_PTR)LocalSessionData);

        Ptr = (PUCHAR)((ULONG_PTR)Ptr + Session->LogonServer.MaximumLength);
    }

    /* Copy the DnsDomainName string */
    LocalSessionData->DnsDomainName.Length = Session->DnsDomainName.Length;
    LocalSessionData->DnsDomainName.MaximumLength = Session->DnsDomainName.MaximumLength;
    if (Session->DnsDomainName.MaximumLength != 0)
    {
        RtlCopyMemory(Ptr, Session->DnsDomainName.Buffer, Session->DnsDomainName.MaximumLength);
        LocalSessionData->DnsDomainName.Buffer = (PWSTR)((ULONG_PTR)Ptr - (ULONG_PTR)LocalSessionData);

        Ptr = (PUCHAR)((ULONG_PTR)Ptr + Session->DnsDomainName.MaximumLength);
    }

    /* Copy the Upn string */
    LocalSessionData->Upn.Length = Session->Upn.Length;
    LocalSessionData->Upn.MaximumLength = Session->Upn.MaximumLength;
    if (Session->Upn.MaximumLength != 0)
    {
        RtlCopyMemory(Ptr, Session->Upn.Buffer, Session->Upn.MaximumLength);
        LocalSessionData->Upn.Buffer = (PWSTR)((ULONG_PTR)Ptr - (ULONG_PTR)LocalSessionData);

        Ptr = (PUCHAR)((ULONG_PTR)Ptr + Session->Upn.MaximumLength);
    }

    if (PointerSize == sizeof(ULONG) && sizeof(PVOID) > sizeof(ULONG))
    {
        RtlZeroMemory(&Data32, sizeof(Data32));
        Data32.Size = sizeof(Data32);
        Data32.LogonId = LocalSessionData->LogonId;
        LsapCopyString32(&Data32.UserName, &LocalSessionData->UserName);
        LsapCopyString32(&Data32.LogonDomain, &LocalSessionData->LogonDomain);
        LsapCopyString32(&Data32.AuthenticationPackage, &LocalSessionData->AuthenticationPackage);
        Data32.LogonType = LocalSessionData->LogonType;
        Data32.Session = LocalSessionData->Session;
        Data32.Sid = (ULONG)(ULONG_PTR)LocalSessionData->Sid;
        Data32.LogonTime = LocalSessionData->LogonTime;
        LsapCopyString32(&Data32.LogonServer, &LocalSessionData->LogonServer);
        LsapCopyString32(&Data32.DnsDomainName, &LocalSessionData->DnsDomainName);
        LsapCopyString32(&Data32.Upn, &LocalSessionData->Upn);
        RtlCopyMemory(LocalSessionData, &Data32, sizeof(Data32));
    }

    InitializeObjectAttributes(&ObjectAttributes,
                               NULL,
                               0,
                               NULL,
                               NULL);

    Status = NtOpenProcess(&ProcessHandle,
                           PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
                           &ObjectAttributes,
                           &RequestMsg->h.ClientId);
    if (!NT_SUCCESS(Status))
    {
        TRACE("NtOpenProcess() failed (Status %lx)\n", Status);
        goto done;
    }

    MemSize = TotalLength;
    Status = NtAllocateVirtualMemory(ProcessHandle,
                                     &ClientBaseAddress,
                                     PointerSize == sizeof(ULONG) && sizeof(PVOID) > sizeof(ULONG) ? MAXULONG : 0,
                                     &MemSize,
                                     MEM_COMMIT,
                                     PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
    {
        TRACE("NtAllocateVirtualMemory() failed (Status %lx)\n", Status);
        goto done;
    }

    TRACE("MemSize: %lu\n", MemSize);
    TRACE("ClientBaseAddress: %p\n", ClientBaseAddress);

    Status = NtWriteVirtualMemory(ProcessHandle,
                                  ClientBaseAddress,
                                  LocalSessionData,
                                  TotalLength,
                                  NULL);
    if (!NT_SUCCESS(Status))
    {
        TRACE("NtWriteVirtualMemory() failed (Status %lx)\n", Status);
        goto done;
    }

    RequestMsg->GetLogonSessionData.Reply.SessionDataBuffer = ClientBaseAddress;

done:
    if (ProcessHandle != NULL)
        NtClose(ProcessHandle);

    if (LocalSessionData != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, LocalSessionData);

    return Status;
}

/* EOF */
