/*
 * PROJECT:     LiberNT Local Security Authority (LSA) Server
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     DPAPI (Data Protection API) server
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "lsasrv.h"
#include "dpapi_s.h"
#include <ndk/iofuncs.h>
#include <bcrypt.h>

#define DPAPI_BLOB_MAGIC            0x3150444C
#define DPAPI_BLOB_MAGIC_V2         0x3250444C  /* 'LDP2': blob names its master key (Q8/D1) */
#define DPAPI_PBKDF2_ITERATIONS     8000
#define DPAPI_SALT_SIZE             16
#define DPAPI_IV_SIZE               16
#define DPAPI_AES_KEY_SIZE          32
#define DPAPI_HMAC_KEY_SIZE         64
#define DPAPI_MAC_SIZE              64
#define DPAPI_DERIVED_SIZE          (DPAPI_AES_KEY_SIZE + DPAPI_HMAC_KEY_SIZE)
#define DPAPI_HEADER_SIZE           (4 + 4 + DPAPI_SALT_SIZE + DPAPI_IV_SIZE + 4)
#define DPAPI_V2_HEADER_SIZE        (4 + 4 + 16 + DPAPI_SALT_SIZE + DPAPI_IV_SIZE + 4)
#define DPAPI_MIN_BLOB_SIZE         (DPAPI_HEADER_SIZE + DPAPI_MAC_SIZE)
#define DPAPI_MIN_BLOB_SIZE_V2      (DPAPI_V2_HEADER_SIZE + DPAPI_MAC_SIZE)
#define DPAPI_MAX_DATA_SIZE         (16 * 1024 * 1024)
#define DPAPI_MAX_ENTROPY_SIZE      (64 * 1024)

/* Derivation scope tags: 0/1 blob user/machine, 2 user key file, 3 credhist entry. */
#define DPAPI_SCOPE_BLOB_USER       0
#define DPAPI_SCOPE_BLOB_MACHINE    1
#define DPAPI_SCOPE_USER_KEYFILE    2
#define DPAPI_SCOPE_CREDHIST        3

#define DPAPI_MASTER_KEY_SIZE       64
#define DPAPI_DIGEST_SIZE           20
#define DPAPI_KEY_MAGIC             0x4B4D444C  /* 'LDMK' */
#define DPAPI_KEY_VERSION           1
#define DPAPI_KEY_KDF_PBKDF2_SHA512 1
#define DPAPI_KEY_ITERATIONS        DPAPI_PBKDF2_ITERATIONS
#define DPAPI_KEY_CIPHER_SIZE       (DPAPI_MASTER_KEY_SIZE + 16)  /* CBC + PKCS7: one padding block */
#define DPAPI_KEYFILE_SIZE          (4 + 4 + 4 + DPAPI_SALT_SIZE + DPAPI_IV_SIZE + \
                                     DPAPI_KEY_CIPHER_SIZE + DPAPI_MAC_SIZE)
#define DPAPI_CREDHIST_MAGIC        0x4843444C  /* 'LDCH' */
#define DPAPI_CREDHIST_VERSION      1
#define DPAPI_CREDHIST_ENTRY_SIZE   (DPAPI_SALT_SIZE + DPAPI_IV_SIZE + 32 + DPAPI_MAC_SIZE)

#ifndef CRYPTPROTECT_LOCAL_MACHINE
#define CRYPTPROTECT_LOCAL_MACHINE  0x0004
#endif

#define DPAPI_MACHINE_KEY_SIZE      64

/* LiberNT-private LSA secret holding the machine-wide DPAPI key. The "L$" prefix marks a local
 * (non-replicated) secret. No Windows master-key blob-format compatibility yet (gap). */
#define DPAPI_MACHINE_SECRET_NAME   L"L$LDW_DPAPI_MACHINE"

static RTL_CRITICAL_SECTION DpapiMachineKeyLock;
static BYTE DpapiMachineKey[DPAPI_MACHINE_KEY_SIZE];
static BOOLEAN DpapiMachineKeyLoaded = FALSE;

/* Q8 per-SID serialization (D3): one lock per user SID, guarding first-use
 * creation, Preferred writes, re-keys and lazy re-keys for that SID. */
typedef struct _DPAPI_SID_LOCK
{
    LIST_ENTRY Entry;
    PSID Sid;
    RTL_CRITICAL_SECTION Lock;
} DPAPI_SID_LOCK;

static LIST_ENTRY DpapiSidLockList;
static RTL_CRITICAL_SECTION DpapiSidLockListLock;

VOID
DpapiServerInitialize(VOID)
{
    RtlInitializeCriticalSection(&DpapiMachineKeyLock);
    RtlInitializeCriticalSection(&DpapiSidLockListLock);
    InitializeListHead(&DpapiSidLockList);
}

static NTSTATUS DpapiGetCallerLogonId(_Out_ PLUID LogonId);
static NTSTATUS DpapiAppendCallerSid(_Inout_ PLSA_STRING Key);

static
NTSTATUS
DpapiCreateMachineKeySd(
    _Outptr_ PSECURITY_DESCRIPTOR *Sd,
    _Out_ PULONG SdSize)
{
    SECURITY_DESCRIPTOR AbsoluteSd;
    PSECURITY_DESCRIPTOR RelativeSd = NULL;
    ULONG RelativeSdSize = 0;
    PSID SystemSid = NULL;
    PACL Dacl = NULL;
    ULONG DaclSize;
    NTSTATUS Status;

    *Sd = NULL;
    *SdSize = 0;

    Status = RtlCreateSecurityDescriptor(&AbsoluteSd, SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = RtlAllocateAndInitializeSid(&NtAuthority, 1, SECURITY_LOCAL_SYSTEM_RID,
                                         0, 0, 0, 0, 0, 0, 0, &SystemSid);
    if (!NT_SUCCESS(Status))
        goto done;

    DaclSize = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(ULONG) + RtlLengthSid(SystemSid);
    Dacl = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, DaclSize);
    if (Dacl == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    Status = RtlCreateAcl(Dacl, DaclSize, ACL_REVISION);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = RtlAddAccessAllowedAce(Dacl, ACL_REVISION, SECRET_ALL_ACCESS, SystemSid);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = RtlSetDaclSecurityDescriptor(&AbsoluteSd, TRUE, Dacl, FALSE);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = RtlSetGroupSecurityDescriptor(&AbsoluteSd, SystemSid, FALSE);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = RtlSetOwnerSecurityDescriptor(&AbsoluteSd, SystemSid, FALSE);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = RtlAbsoluteToSelfRelativeSD(&AbsoluteSd, RelativeSd, &RelativeSdSize);
    if (Status != STATUS_BUFFER_TOO_SMALL)
        goto done;

    RelativeSd = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, RelativeSdSize);
    if (RelativeSd == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    Status = RtlAbsoluteToSelfRelativeSD(&AbsoluteSd, RelativeSd, &RelativeSdSize);
    if (!NT_SUCCESS(Status))
        goto done;

    *Sd = RelativeSd;
    *SdSize = RelativeSdSize;

done:
    if (Dacl != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Dacl);
    if (SystemSid != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, SystemSid);
    if (!NT_SUCCESS(Status) && RelativeSd != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, RelativeSd);
    return Status;
}

static
NTSTATUS
DpapiCheckLocalCaller(VOID)
{
    HANDLE Token = NULL;
    PTOKEN_GROUPS Groups = NULL;
    PSID NetworkSid = NULL;
    PSID AnonymousSid = NULL;
    ULONG Length = 0;
    ULONG Index;
    BOOLEAN Denied = FALSE;
    NTSTATUS Status;

    if (RpcImpersonateClient(NULL) != RPC_S_OK)
        return STATUS_CANNOT_IMPERSONATE;

    Status = NtOpenThreadToken(NtCurrentThread(), TOKEN_QUERY, TRUE, &Token);
    if (!NT_SUCCESS(Status))
    {
        RpcRevertToSelf();
        return Status;
    }

    NtQueryInformationToken(Token, TokenGroups, NULL, 0, &Length);
    if (Length != 0)
        Groups = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length);
    if (Groups == NULL)
        Status = STATUS_INSUFFICIENT_RESOURCES;
    else
        Status = NtQueryInformationToken(Token, TokenGroups, Groups, Length, &Length);

    if (NT_SUCCESS(Status))
        Status = RtlAllocateAndInitializeSid(&NtAuthority, 1, SECURITY_NETWORK_RID,
                                             0, 0, 0, 0, 0, 0, 0, &NetworkSid);
    if (NT_SUCCESS(Status))
        Status = RtlAllocateAndInitializeSid(&NtAuthority, 1, SECURITY_ANONYMOUS_LOGON_RID,
                                             0, 0, 0, 0, 0, 0, 0, &AnonymousSid);

    if (NT_SUCCESS(Status))
    {
        for (Index = 0; Index < Groups->GroupCount; Index++)
        {
            if (RtlEqualSid(Groups->Groups[Index].Sid, NetworkSid) ||
                RtlEqualSid(Groups->Groups[Index].Sid, AnonymousSid))
            {
                Denied = TRUE;
                break;
            }
        }
    }

    if (NetworkSid != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, NetworkSid);
    if (AnonymousSid != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, AnonymousSid);
    if (Groups != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Groups);
    NtClose(Token);
    RpcRevertToSelf();

    if (!NT_SUCCESS(Status))
        return Status;
    return Denied ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
}

static
NTSTATUS
DpapiReadMachineSecret(
    _In_ PLSA_DB_OBJECT PolicyObject,
    _Out_writes_bytes_(DPAPI_MACHINE_KEY_SIZE) PBYTE Key)
{
    PLSA_DB_OBJECT SecretObject = NULL;
    ULONG Size = 0;
    NTSTATUS Status;

    Status = LsapOpenDbObject(PolicyObject,
                              L"Secrets",
                              DPAPI_MACHINE_SECRET_NAME,
                              LsaDbSecretObject,
                              0,
                              TRUE,
                              &SecretObject);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Query the size first so an absent CurrentValue is reported as NOT_FOUND (regenerate), while a
     * present but wrong-sized value is a hard error (never regenerate -- that would overwrite a key
     * in use). The output buffer is written only on a fully successful read. */
    Status = LsapGetObjectAttribute(SecretObject, L"CurrentValue", NULL, &Size);
    if (NT_SUCCESS(Status))
    {
        if (Size != DPAPI_MACHINE_KEY_SIZE)
        {
            Status = STATUS_UNSUCCESSFUL;
        }
        else
        {
            Size = DPAPI_MACHINE_KEY_SIZE;
            Status = LsapGetObjectAttribute(SecretObject, L"CurrentValue", Key, &Size);
        }
    }

    LsapCloseDbObject(SecretObject);
    return Status;
}

static
NTSTATUS
DpapiCreateMachineSecret(
    _In_ PLSA_DB_OBJECT PolicyObject,
    _In_reads_bytes_(DPAPI_MACHINE_KEY_SIZE) PBYTE Key)
{
    PLSA_DB_OBJECT SecretObject = NULL;
    PSECURITY_DESCRIPTOR SecretSd = NULL;
    ULONG SecretSdSize = 0;
    LARGE_INTEGER Time;
    NTSTATUS Status;

    Status = DpapiCreateMachineKeySd(&SecretSd, &SecretSdSize);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = LsapCreateDbObject(PolicyObject,
                                L"Secrets",
                                DPAPI_MACHINE_SECRET_NAME,
                                LsaDbSecretObject,
                                0,
                                TRUE,
                                &SecretObject);
    if (!NT_SUCCESS(Status))
        goto done;

    NtQuerySystemTime(&Time);
    Status = LsapSetObjectAttribute(SecretObject, L"CurrentTime", &Time, sizeof(LARGE_INTEGER));
    if (NT_SUCCESS(Status))
        Status = LsapSetObjectAttribute(SecretObject, L"OldTime", &Time, sizeof(LARGE_INTEGER));
    if (NT_SUCCESS(Status))
        Status = LsapSetObjectAttribute(SecretObject, L"SecDesc", SecretSd, SecretSdSize);
    if (NT_SUCCESS(Status))
        Status = LsapSetObjectAttribute(SecretObject, L"CurrentValue", Key, DPAPI_MACHINE_KEY_SIZE);

    if (!NT_SUCCESS(Status))
    {
        /* Do not leave a half-made secret (e.g. without an SD) behind. */
        LsapDeleteDbObject(SecretObject);
        SecretObject = NULL;
    }

done:
    if (SecretObject != NULL)
        LsapCloseDbObject(SecretObject);
    if (SecretSd != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, SecretSd);
    return Status;
}

static
NTSTATUS
DpapiLoadMachineKey(
    _Out_writes_bytes_(DPAPI_MACHINE_KEY_SIZE) PBYTE Key)
{
    PLSA_DB_OBJECT PolicyObject = NULL;
    NTSTATUS Status;

    RtlEnterCriticalSection(&DpapiMachineKeyLock);

    if (DpapiMachineKeyLoaded)
    {
        RtlCopyMemory(Key, DpapiMachineKey, DPAPI_MACHINE_KEY_SIZE);
        RtlLeaveCriticalSection(&DpapiMachineKeyLock);
        return STATUS_SUCCESS;
    }

    Status = LsapOpenDbObject(NULL,
                              NULL,
                              L"Policy",
                              LsaDbPolicyObject,
                              0,
                              TRUE,
                              &PolicyObject);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = DpapiReadMachineSecret(PolicyObject, DpapiMachineKey);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND || Status == STATUS_OBJECT_PATH_NOT_FOUND)
    {
        /* Genuinely absent: generate and store. A read that failed for any other reason (including a
         * present value of the wrong size) must NOT regenerate -- that would overwrite a key still in
         * use and make every machine blob undecryptable. */
        Status = BCryptGenRandom(NULL, DpapiMachineKey, DPAPI_MACHINE_KEY_SIZE,
                                 BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (NT_SUCCESS(Status))
            Status = DpapiCreateMachineSecret(PolicyObject, DpapiMachineKey);

        if (!NT_SUCCESS(Status))
        {
            RtlSecureZeroMemory(DpapiMachineKey, DPAPI_MACHINE_KEY_SIZE);
            goto done;
        }
    }
    else if (!NT_SUCCESS(Status))
    {
        RtlSecureZeroMemory(DpapiMachineKey, DPAPI_MACHINE_KEY_SIZE);
        goto done;
    }

    DpapiMachineKeyLoaded = TRUE;
    RtlCopyMemory(Key, DpapiMachineKey, DPAPI_MACHINE_KEY_SIZE);

done:
    if (PolicyObject != NULL)
        LsapCloseDbObject(PolicyObject);
    RtlLeaveCriticalSection(&DpapiMachineKeyLock);
    return Status;
}

/*
 * Produce the key material PBKDF2 derives from. CRYPTPROTECT_LOCAL_MACHINE uses the machine key so
 * any user on the machine can unprotect; otherwise the caller's per-logon pre-key is used and the
 * data is bound to that logon identity. The returned Buffer is always heap-allocated and the caller
 * frees it (and zeroes it) the same way for both paths.
 */
static
NTSTATUS
DpapiAcquireKey(
    _In_ ULONG Flags,
    _Out_ PLSA_STRING Key)
{
    LUID LogonId;
    NTSTATUS Status;

    Key->Length = 0;
    Key->MaximumLength = 0;
    Key->Buffer = NULL;

    if (Flags & CRYPTPROTECT_LOCAL_MACHINE)
    {
        BYTE MachineKey[DPAPI_MACHINE_KEY_SIZE];

        /* LOCAL_MACHINE means any LOCAL user, not anyone who can reach the pipe: reject anonymous and
         * network callers so a machine blob copied off this disk cannot be decrypted from elsewhere. */
        Status = DpapiCheckLocalCaller();
        if (!NT_SUCCESS(Status))
            return Status;

        Status = DpapiLoadMachineKey(MachineKey);
        if (!NT_SUCCESS(Status))
            return Status;

        Key->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, DPAPI_MACHINE_KEY_SIZE);
        if (Key->Buffer == NULL)
        {
            RtlSecureZeroMemory(MachineKey, sizeof(MachineKey));
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(Key->Buffer, MachineKey, DPAPI_MACHINE_KEY_SIZE);
        Key->Length = DPAPI_MACHINE_KEY_SIZE;
        Key->MaximumLength = DPAPI_MACHINE_KEY_SIZE;
        RtlSecureZeroMemory(MachineKey, sizeof(MachineKey));
        return STATUS_SUCCESS;
    }

    Status = DpapiGetCallerLogonId(&LogonId);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = LsapGetDpapiPrekey(&LogonId, Key);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Bind the caller's account identity into the user-scope key so two accounts with the same
     * password do not derive the same key (and a blob cannot be moved between hives). */
    return DpapiAppendCallerSid(Key);
}

/* Query the caller's user SID (heap copy) for the master-key directory. */
static
NTSTATUS
DpapiGetCallerSid(
    _Outptr_ PSID *Sid)
{
    HANDLE Token = NULL;
    PTOKEN_USER UserInfo = NULL;
    ULONG Length = 0;
    NTSTATUS Status;

    *Sid = NULL;

    if (RpcImpersonateClient(NULL) != RPC_S_OK)
        return STATUS_CANNOT_IMPERSONATE;

    Status = NtOpenThreadToken(NtCurrentThread(), TOKEN_QUERY, TRUE, &Token);
    if (NT_SUCCESS(Status))
    {
        NtQueryInformationToken(Token, TokenUser, NULL, 0, &Length);
        if (Length == 0)
            Status = STATUS_UNSUCCESSFUL;
        else if (!(UserInfo = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length)))
            Status = STATUS_INSUFFICIENT_RESOURCES;
        else
            Status = NtQueryInformationToken(Token, TokenUser, UserInfo, Length, &Length);

        NtClose(Token);
    }
    RpcRevertToSelf();

    if (NT_SUCCESS(Status))
    {
        ULONG SidLength = RtlLengthSid(UserInfo->User.Sid);

        *Sid = RtlAllocateHeap(RtlGetProcessHeap(), 0, SidLength);
        if (*Sid == NULL)
            Status = STATUS_INSUFFICIENT_RESOURCES;
        else
            RtlCopyMemory(*Sid, UserInfo->User.Sid, SidLength);
    }

    if (UserInfo != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, UserInfo);
    if (!NT_SUCCESS(Status) && *Sid != NULL)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, *Sid);
        *Sid = NULL;
    }
    return Status;
}

static
NTSTATUS
DpapiGetCallerLogonId(
    _Out_ PLUID LogonId)
{
    HANDLE TokenHandle;
    TOKEN_STATISTICS Statistics;
    ULONG Length;
    NTSTATUS Status;

    if (RpcImpersonateClient(NULL) != RPC_S_OK)
        return STATUS_CANNOT_IMPERSONATE;

    Status = NtOpenThreadToken(NtCurrentThread(), TOKEN_QUERY, TRUE, &TokenHandle);
    if (NT_SUCCESS(Status))
    {
        Status = NtQueryInformationToken(TokenHandle, TokenStatistics,
                                         &Statistics, sizeof(Statistics), &Length);
        if (NT_SUCCESS(Status))
            *LogonId = Statistics.AuthenticationId;

        NtClose(TokenHandle);
    }

    RpcRevertToSelf();
    return Status;
}

static
NTSTATUS
DpapiAppendCallerSid(
    _Inout_ PLSA_STRING Key)
{
    HANDLE Token = NULL;
    PTOKEN_USER UserInfo = NULL;
    ULONG Length = 0;
    ULONG SidLength;
    PBYTE NewBuffer;
    NTSTATUS Status;

    if (RpcImpersonateClient(NULL) != RPC_S_OK)
    {
        Status = STATUS_CANNOT_IMPERSONATE;
    }
    else
    {
        Status = NtOpenThreadToken(NtCurrentThread(), TOKEN_QUERY, TRUE, &Token);
        if (NT_SUCCESS(Status))
        {
            NtQueryInformationToken(Token, TokenUser, NULL, 0, &Length);
            if (Length == 0)
                Status = STATUS_UNSUCCESSFUL;
            else if (!(UserInfo = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length)))
                Status = STATUS_INSUFFICIENT_RESOURCES;
            else
                Status = NtQueryInformationToken(Token, TokenUser, UserInfo, Length, &Length);

            NtClose(Token);
        }
        RpcRevertToSelf();
    }

    if (NT_SUCCESS(Status))
    {
        SidLength = RtlLengthSid(UserInfo->User.Sid);
        NewBuffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Key->Length + SidLength);
        if (NewBuffer == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            RtlCopyMemory(NewBuffer, Key->Buffer, Key->Length);
            RtlCopyMemory(NewBuffer + Key->Length, UserInfo->User.Sid, SidLength);
            RtlSecureZeroMemory(Key->Buffer, Key->Length);
            RtlFreeHeap(RtlGetProcessHeap(), 0, Key->Buffer);
            Key->Buffer = (PCHAR)NewBuffer;
            Key->Length = Key->MaximumLength = (USHORT)(Key->Length + SidLength);
        }
    }

    if (UserInfo != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, UserInfo);

    /* On failure free the pre-key buffer so the caller is not left with a half-formed key to free. */
    if (!NT_SUCCESS(Status) && Key->Buffer != NULL)
    {
        RtlSecureZeroMemory(Key->Buffer, Key->Length);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Key->Buffer);
        Key->Buffer = NULL;
        Key->Length = 0;
        Key->MaximumLength = 0;
    }

    return Status;
}

static
NTSTATUS
DpapiDeriveKeys(
    _In_ ULONG Scope,
    _In_ PLSA_STRING Prekey,
    _In_reads_bytes_(EntropyLength) PBYTE Entropy,
    _In_ ULONG EntropyLength,
    _In_reads_bytes_(DPAPI_SALT_SIZE) PBYTE Salt,
    _Out_writes_bytes_(DPAPI_DERIVED_SIZE) PBYTE Derived)
{
    BCRYPT_ALG_HANDLE Algorithm = NULL;
    PBYTE Password = NULL;
    ULONG PasswordLength;
    NTSTATUS Status;

    /* Prefix a scope tag so the user and machine scopes never derive the same key from the same key
     * material: PBKDF2 password = Scope(4 bytes) || Prekey || Entropy. */
    PasswordLength = sizeof(ULONG) + Prekey->Length + EntropyLength;

    Password = RtlAllocateHeap(RtlGetProcessHeap(), 0, PasswordLength);
    if (Password == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Password, &Scope, sizeof(ULONG));
    RtlCopyMemory(Password + sizeof(ULONG), Prekey->Buffer, Prekey->Length);
    if (EntropyLength != 0)
        RtlCopyMemory(Password + sizeof(ULONG) + Prekey->Length, Entropy, EntropyLength);

    Status = BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA512_ALGORITHM, NULL,
                                         BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (NT_SUCCESS(Status))
    {
        Status = BCryptDeriveKeyPBKDF2(Algorithm, Password, PasswordLength,
                                       Salt, DPAPI_SALT_SIZE, DPAPI_PBKDF2_ITERATIONS,
                                       Derived, DPAPI_DERIVED_SIZE, 0);
        BCryptCloseAlgorithmProvider(Algorithm, 0);
    }

    RtlSecureZeroMemory(Password, PasswordLength);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Password);
    return Status;
}

static
NTSTATUS
DpapiAesCrypt(
    _In_ BOOLEAN Encrypt,
    _In_reads_bytes_(DPAPI_AES_KEY_SIZE) PBYTE Key,
    _In_reads_bytes_(DPAPI_IV_SIZE) PBYTE Iv,
    _In_reads_bytes_(InputLength) PBYTE Input,
    _In_ ULONG InputLength,
    _Outptr_result_bytebuffer_(*OutputLength) PBYTE *Output,
    _Out_ PULONG OutputLength)
{
    BCRYPT_ALG_HANDLE Algorithm = NULL;
    BCRYPT_KEY_HANDLE KeyHandle = NULL;
    PBYTE KeyObject = NULL;
    PBYTE Buffer = NULL;
    BYTE IvCopy[DPAPI_IV_SIZE];
    ULONG KeyObjectLength = 0;
    ULONG ResultLength = 0;
    ULONG Length;
    NTSTATUS Status;

    *Output = NULL;
    *OutputLength = 0;

    Status = BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = BCryptSetProperty(Algorithm, BCRYPT_CHAINING_MODE,
                               (PBYTE)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = BCryptGetProperty(Algorithm, BCRYPT_OBJECT_LENGTH,
                               (PBYTE)&KeyObjectLength, sizeof(KeyObjectLength), &Length, 0);
    if (!NT_SUCCESS(Status))
        goto done;

    KeyObject = RtlAllocateHeap(RtlGetProcessHeap(), 0, KeyObjectLength);
    if (KeyObject == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    Status = BCryptGenerateSymmetricKey(Algorithm, &KeyHandle, KeyObject, KeyObjectLength,
                                        Key, DPAPI_AES_KEY_SIZE, 0);
    if (!NT_SUCCESS(Status))
        goto done;

    RtlCopyMemory(IvCopy, Iv, DPAPI_IV_SIZE);

    if (Encrypt)
        Status = BCryptEncrypt(KeyHandle, Input, InputLength, NULL, IvCopy, DPAPI_IV_SIZE,
                               NULL, 0, &ResultLength, BCRYPT_BLOCK_PADDING);
    else
        Status = BCryptDecrypt(KeyHandle, Input, InputLength, NULL, IvCopy, DPAPI_IV_SIZE,
                               NULL, 0, &ResultLength, BCRYPT_BLOCK_PADDING);
    if (!NT_SUCCESS(Status))
        goto done;

    Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, ResultLength != 0 ? ResultLength : 1);
    if (Buffer == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    RtlCopyMemory(IvCopy, Iv, DPAPI_IV_SIZE);

    if (Encrypt)
        Status = BCryptEncrypt(KeyHandle, Input, InputLength, NULL, IvCopy, DPAPI_IV_SIZE,
                               Buffer, ResultLength, &ResultLength, BCRYPT_BLOCK_PADDING);
    else
        Status = BCryptDecrypt(KeyHandle, Input, InputLength, NULL, IvCopy, DPAPI_IV_SIZE,
                               Buffer, ResultLength, &ResultLength, BCRYPT_BLOCK_PADDING);
    if (!NT_SUCCESS(Status))
        goto done;

    *Output = Buffer;
    *OutputLength = ResultLength;
    Buffer = NULL;

done:
    if (Buffer != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
    if (KeyHandle != NULL)
        BCryptDestroyKey(KeyHandle);
    if (KeyObject != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, KeyObject);
    BCryptCloseAlgorithmProvider(Algorithm, 0);
    RtlSecureZeroMemory(IvCopy, sizeof(IvCopy));
    return Status;
}

static
NTSTATUS
DpapiMac(
    _In_reads_bytes_(DPAPI_HMAC_KEY_SIZE) PBYTE Key,
    _In_reads_bytes_(DataLength) PBYTE Data,
    _In_ ULONG DataLength,
    _Out_writes_bytes_(DPAPI_MAC_SIZE) PBYTE Mac)
{
    BCRYPT_ALG_HANDLE Algorithm = NULL;
    NTSTATUS Status;

    Status = BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA512_ALGORITHM, NULL,
                                         BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = BCryptHash(Algorithm, Key, DPAPI_HMAC_KEY_SIZE, Data, DataLength, Mac, DPAPI_MAC_SIZE);
    BCryptCloseAlgorithmProvider(Algorithm, 0);
    return Status;
}

static
BOOLEAN
DpapiEqualMac(
    _In_reads_bytes_(DPAPI_MAC_SIZE) PBYTE A,
    _In_reads_bytes_(DPAPI_MAC_SIZE) PBYTE B)
{
    BYTE Diff = 0;
    ULONG Index;

    for (Index = 0; Index < DPAPI_MAC_SIZE; Index++)
        Diff |= (BYTE)(A[Index] ^ B[Index]);

    return Diff == 0;
}

/* ---- Q8: per-user master keys, credhist, password-change re-key ---- */

typedef enum _DPAPI_IO_MODE
{
    DpapiIoImpersonated, /* an RPC caller is impersonated for all file I/O (D2) */
    DpapiIoSystem        /* no caller: reparse-refusing, handle-relative I/O (D2) */
} DPAPI_IO_MODE;

static
NTSTATUS
DpapiAcquireSidLock(
    _In_ PSID Sid)
{
    PLIST_ENTRY Entry;
    DPAPI_SID_LOCK *SidLock;
    ULONG SidLength;
    NTSTATUS Status;

    RtlEnterCriticalSection(&DpapiSidLockListLock);

    for (Entry = DpapiSidLockList.Flink; Entry != &DpapiSidLockList; Entry = Entry->Flink)
    {
        SidLock = CONTAINING_RECORD(Entry, DPAPI_SID_LOCK, Entry);
        if (RtlEqualSid(SidLock->Sid, Sid))
        {
            RtlLeaveCriticalSection(&DpapiSidLockListLock);
            RtlEnterCriticalSection(&SidLock->Lock);
            return STATUS_SUCCESS;
        }
    }

    SidLength = RtlLengthSid(Sid);
    SidLock = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY,
                              sizeof(*SidLock) + SidLength);
    if (SidLock == NULL)
    {
        RtlLeaveCriticalSection(&DpapiSidLockListLock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    SidLock->Sid = (PSID)(SidLock + 1);
    RtlCopyMemory(SidLock->Sid, Sid, SidLength);
    Status = RtlInitializeCriticalSectionEx(&SidLock->Lock, 0, 0);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, SidLock);
        RtlLeaveCriticalSection(&DpapiSidLockListLock);
        return Status;
    }

    InsertTailList(&DpapiSidLockList, &SidLock->Entry);
    RtlLeaveCriticalSection(&DpapiSidLockListLock);
    RtlEnterCriticalSection(&SidLock->Lock);
    return STATUS_SUCCESS;
}

static
VOID
DpapiReleaseSidLock(
    _In_ PSID Sid)
{
    PLIST_ENTRY Entry;
    DPAPI_SID_LOCK *SidLock;

    RtlEnterCriticalSection(&DpapiSidLockListLock);
    for (Entry = DpapiSidLockList.Flink; Entry != &DpapiSidLockList; Entry = Entry->Flink)
    {
        SidLock = CONTAINING_RECORD(Entry, DPAPI_SID_LOCK, Entry);
        if (RtlEqualSid(SidLock->Sid, Sid))
        {
            RtlLeaveCriticalSection(&DpapiSidLockListLock);
            RtlLeaveCriticalSection(&SidLock->Lock);
            return;
        }
    }
    RtlLeaveCriticalSection(&DpapiSidLockListLock);
}

/* The profile directory of a SID from ProfileList, read as SYSTEM (never under the caller's
 * token) and expanded: ProfileImagePath is REG_EXPAND_SZ ("%SystemDrive%\\Users\\name"). */
static
NTSTATUS
DpapiReadProfileImagePath(
    _In_ PCUNICODE_STRING SidUnicode,
    _Out_writes_(PathChars) PWCHAR Path,
    _In_ ULONG PathChars)
{
    WCHAR KeyPath[256];
    WCHAR RawPath[MAX_PATH];
    DWORD RawSize = sizeof(RawPath), Expanded;
    LONG Length;
    LSTATUS Result;
    HKEY Key;

    Length = _snwprintf(KeyPath, ARRAYSIZE(KeyPath),
                        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\%wZ",
                        SidUnicode);
    if (Length < 0 || Length >= ARRAYSIZE(KeyPath))
        return STATUS_NAME_TOO_LONG;

    Result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyPath, 0, KEY_READ, &Key);
    if (Result != ERROR_SUCCESS)
    {
        ERR("ProfileList key %s: error %lu\n", debugstr_w(KeyPath), Result);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    Result = RegGetValueW(Key, NULL, L"ProfileImagePath",
                          RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                          NULL, RawPath, &RawSize);
    RegCloseKey(Key);
    if (Result != ERROR_SUCCESS)
    {
        ERR("ProfileImagePath under %s: error %lu\n", debugstr_w(KeyPath), Result);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    Expanded = ExpandEnvironmentStringsW(RawPath, Path, PathChars);
    if (Expanded == 0)
    {
        ERR("expanding %s: error %lu\n", debugstr_w(RawPath), GetLastError());
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (Expanded > PathChars)
        return STATUS_NAME_TOO_LONG;
    return STATUS_SUCCESS;
}

static
NTSTATUS
DpapiGetUserProtectPath(
    _In_ PSID Sid,
    _In_ BOOLEAN WithSidDir,
    _Out_writes_bytes_(DirLength) PWCHAR Dir,
    _In_ ULONG DirLength)
{
    WCHAR SidString[64];
    UNICODE_STRING SidUnicode;
    WCHAR ProfilePath[MAX_PATH];
    LONG Length;
    NTSTATUS Status;

    RtlInitEmptyUnicodeString(&SidUnicode, SidString, sizeof(SidString));
    if (!NT_SUCCESS(RtlConvertSidToUnicodeString(&SidUnicode, Sid, FALSE)))
        return STATUS_UNSUCCESSFUL;

    Status = DpapiReadProfileImagePath(&SidUnicode, ProfilePath, ARRAYSIZE(ProfilePath));
    if (!NT_SUCCESS(Status))
        return Status;

    if (WithSidDir)
    {
        Length = _snwprintf(Dir, DirLength / sizeof(WCHAR),
                            L"%s\\AppData\\Roaming\\Microsoft\\Protect\\%wZ",
                            ProfilePath, &SidUnicode);
    }
    else
    {
        Length = _snwprintf(Dir, DirLength / sizeof(WCHAR),
                            L"%s\\AppData\\Roaming\\Microsoft\\Protect",
                            ProfilePath);
    }
    if (Length < 0 || (ULONG)(Length + 1) * sizeof(WCHAR) > DirLength)
        return STATUS_NAME_TOO_LONG;

    return STATUS_SUCCESS;
}

/* Build <profile>\AppData\Roaming\Microsoft\Protect\<SID> from ProfileList. */
static
NTSTATUS
DpapiGetUserProtectDir(
    _In_ PSID Sid,
    _Out_writes_bytes_(DirLength) PWCHAR Dir,
    _In_ ULONG DirLength)
{
    return DpapiGetUserProtectPath(Sid, TRUE, Dir, DirLength);
}

/* The credhist lives at the Protect root (Win11 layout, C3). */
static
NTSTATUS
DpapiGetUserProtectRoot(
    _In_ PSID Sid,
    _Out_writes_bytes_(DirLength) PWCHAR Dir,
    _In_ ULONG DirLength)
{
    return DpapiGetUserProtectPath(Sid, FALSE, Dir, DirLength);
}

/* DACL for a per-user key file, the credhist or the SID dir: SYSTEM,
 * Administrators and the owning user get Full, nobody else (Win11 parity). */
static
NTSTATUS
DpapiCreateUserKeySd(
    _In_ PSID OwnerSid,
    _Outptr_ PSECURITY_DESCRIPTOR *Sd,
    _Out_ PULONG SdSize)
{
    SECURITY_DESCRIPTOR AbsoluteSd;
    PSECURITY_DESCRIPTOR RelativeSd = NULL;
    ULONG RelativeSdSize = 0;
    PSID SystemSid = NULL, AdminsSid = NULL;
    PACL Dacl = NULL;
    ULONG DaclSize;
    NTSTATUS Status;

    *Sd = NULL;
    *SdSize = 0;

    Status = RtlCreateSecurityDescriptor(&AbsoluteSd, SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = RtlAllocateAndInitializeSid(&NtAuthority, 1, SECURITY_LOCAL_SYSTEM_RID,
                                         0, 0, 0, 0, 0, 0, 0, &SystemSid);
    if (NT_SUCCESS(Status))
        Status = RtlAllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                             DOMAIN_ALIAS_RID_ADMINS,
                                             0, 0, 0, 0, 0, 0, &AdminsSid);
    if (!NT_SUCCESS(Status))
        goto done;

    DaclSize = sizeof(ACL) +
               3 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(ULONG)) +
               RtlLengthSid(SystemSid) + RtlLengthSid(AdminsSid) + RtlLengthSid(OwnerSid);
    Dacl = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, DaclSize);
    if (Dacl == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    Status = RtlCreateAcl(Dacl, DaclSize, ACL_REVISION);
    if (NT_SUCCESS(Status))
        Status = RtlAddAccessAllowedAce(Dacl, ACL_REVISION, FILE_ALL_ACCESS, SystemSid);
    if (NT_SUCCESS(Status))
        Status = RtlAddAccessAllowedAce(Dacl, ACL_REVISION, FILE_ALL_ACCESS, AdminsSid);
    if (NT_SUCCESS(Status))
        Status = RtlAddAccessAllowedAce(Dacl, ACL_REVISION, FILE_ALL_ACCESS, OwnerSid);
    if (NT_SUCCESS(Status))
        Status = RtlSetDaclSecurityDescriptor(&AbsoluteSd, TRUE, Dacl, FALSE);
    if (NT_SUCCESS(Status))
        Status = RtlSetOwnerSecurityDescriptor(&AbsoluteSd, SystemSid, FALSE);
    if (NT_SUCCESS(Status))
        Status = RtlSetGroupSecurityDescriptor(&AbsoluteSd, SystemSid, FALSE);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = RtlAbsoluteToSelfRelativeSD(&AbsoluteSd, RelativeSd, &RelativeSdSize);
    if (Status != STATUS_BUFFER_TOO_SMALL)
        goto done;

    RelativeSd = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, RelativeSdSize);
    if (RelativeSd == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    Status = RtlAbsoluteToSelfRelativeSD(&AbsoluteSd, RelativeSd, &RelativeSdSize);
    if (NT_SUCCESS(Status))
    {
        *Sd = RelativeSd;
        *SdSize = RelativeSdSize;
        RelativeSd = NULL;
    }

done:
    if (Dacl != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Dacl);
    if (SystemSid != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, SystemSid);
    if (AdminsSid != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, AdminsSid);
    if (RelativeSd != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, RelativeSd);
    return Status;
}

/* Create the Protect\<SID> directory chain; the leaf gets the user SD. */
static
NTSTATUS
DpapiEnsureProtectDir(
    _In_ PCWSTR Dir,
    _In_ PSID OwnerSid)
{
    WCHAR Path[MAX_PATH * 2];
    PWCHAR Cursor;
    PSECURITY_DESCRIPTOR Sd = NULL;
    ULONG SdSize;
    NTSTATUS Status;

    if (GetFileAttributesW(Dir) != INVALID_FILE_ATTRIBUTES)
        return STATUS_SUCCESS;

    if (wcslen(Dir) >= ARRAYSIZE(Path))
        return STATUS_NAME_TOO_LONG;
    wcscpy(Path, Dir);

    /* Create each missing level with default ACLs; the leaf gets the user SD. */
    for (Cursor = Path; *Cursor != UNICODE_NULL; Cursor++)
    {
        if (*Cursor == L'\\' && Cursor != Path && Cursor[-1] != L':')
        {
            *Cursor = UNICODE_NULL;
            if (GetFileAttributesW(Path) == INVALID_FILE_ATTRIBUTES &&
                !CreateDirectoryW(Path, NULL) &&
                GetLastError() != ERROR_ALREADY_EXISTS)
            {
                return STATUS_UNSUCCESSFUL;
            }
            *Cursor = L'\\';
        }
    }

    Status = DpapiCreateUserKeySd(OwnerSid, &Sd, &SdSize);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!CreateDirectoryW(Path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        Status = STATUS_UNSUCCESSFUL;

    if (NT_SUCCESS(Status) && !SetFileSecurityW(Path, DACL_SECURITY_INFORMATION, Sd))
        Status = RtlGetLastNtStatus();

    RtlFreeHeap(RtlGetProcessHeap(), 0, Sd);
    return Status;
}

/* Open a path relative to a directory handle without following reparse points
 * (D2, SYSTEM mode). */
static
NTSTATUS
DpapiOpenNoReparse(
    _In_opt_ HANDLE DirHandle,
    _In_ PCWSTR Name,
    _In_ ULONG DesiredAccess,
    _In_ ULONG CreateDisposition,
    _In_ ULONG CreateOptions,
    _Out_ PHANDLE FileHandle)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING NameUnicode;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_ATTRIBUTE_TAG_INFORMATION TagInfo;
    NTSTATUS Status;

    RtlInitUnicodeString(&NameUnicode, Name);
    InitializeObjectAttributes(&ObjectAttributes, &NameUnicode,
                               OBJ_CASE_INSENSITIVE, DirHandle, NULL);
    Status = NtCreateFile(FileHandle, DesiredAccess | SYNCHRONIZE | FILE_READ_ATTRIBUTES,
                          &ObjectAttributes, &IoStatusBlock, NULL,
                          FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          CreateDisposition,
                          CreateOptions | FILE_SYNCHRONOUS_IO_NONALERT |
                          FILE_OPEN_REPARSE_POINT,
                          NULL, 0);
    if (!NT_SUCCESS(Status))
        return Status;

    /* FILE_OPEN_REPARSE_POINT opens the link itself; refuse it (D2). */
    Status = NtQueryInformationFile(*FileHandle, &IoStatusBlock, &TagInfo,
                                    sizeof(TagInfo), FileAttributeTagInformation);
    if (NT_SUCCESS(Status) &&
        (TagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        NtClose(*FileHandle);
        *FileHandle = NULL;
        return STATUS_IO_REPARSE_TAG_MISMATCH;
    }

    return Status;
}

/* Open a directory handle for the rename-by-handle target (D2/D3). */
static
NTSTATUS
DpapiOpenDirHandle(
    _In_ PCWSTR Dir,
    _In_ DPAPI_IO_MODE IoMode,
    _Out_ PHANDLE DirHandle)
{
    NTSTATUS Status;

    if (IoMode == DpapiIoSystem)
    {
        return DpapiOpenNoReparse(NULL, Dir, FILE_LIST_DIRECTORY,
                                  FILE_OPEN, FILE_DIRECTORY_FILE, DirHandle);
    }

    *DirHandle = CreateFileW(Dir, FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (*DirHandle == INVALID_HANDLE_VALUE)
    {
        *DirHandle = NULL;
        Status = RtlGetLastNtStatus();
        return Status;
    }
    return STATUS_SUCCESS;
}

/* Write FileName in Dir by temp file plus rename-by-handle relative to the
 * directory handle; in SYSTEM mode that handle came from DpapiOpenNoReparse,
 * so neither the temp nor the rename can be redirected (D2/D3). */
static
NTSTATUS
DpapiSecureWriteFile(
    _In_ PCWSTR Dir,
    _In_ PCWSTR FileName,
    _In_reads_bytes_(DataLength) PBYTE Data,
    _In_ ULONG DataLength,
    _In_ DPAPI_IO_MODE IoMode,
    _In_opt_ PSECURITY_DESCRIPTOR Sd)
{
    WCHAR TempName[64];
    WCHAR TempPath[MAX_PATH * 2];
    HANDLE DirHandle = NULL, FileHandle = NULL;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_RENAME_INFORMATION *RenameInfo = NULL;
    ULONG RenameInfoSize;
    NTSTATUS Status;

    /* SYSTEM-mode writes go through the verified handle tree (DpapiSysWrite);
     * a path-based SYSTEM write cannot be made reparse-safe (C1). */
    if (IoMode != DpapiIoImpersonated)
        return STATUS_INVALID_PARAMETER;

    _snwprintf(TempName, ARRAYSIZE(TempName), L"tmp%lx%lx",
               GetCurrentProcessId(), GetTickCount64() & 0xffffffff);

    Status = DpapiOpenDirHandle(Dir, IoMode, &DirHandle);
    if (!NT_SUCCESS(Status))
        return Status;

    _snwprintf(TempPath, ARRAYSIZE(TempPath), L"%s\\%s", Dir, TempName);
    FileHandle = CreateFileW(TempPath, FILE_GENERIC_WRITE | DELETE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (FileHandle == INVALID_HANDLE_VALUE)
    {
        FileHandle = NULL;
        Status = RtlGetLastNtStatus();
    }
    if (!NT_SUCCESS(Status))
        goto done;

    if (Sd != NULL)
        NtSetSecurityObject(FileHandle, DACL_SECURITY_INFORMATION, Sd);

    Status = NtWriteFile(FileHandle, NULL, NULL, NULL, &IoStatusBlock,
                         (PVOID)Data, DataLength, NULL, NULL);
    if (NT_SUCCESS(Status) && IoStatusBlock.Information != DataLength)
        Status = STATUS_UNEXPECTED_IO_ERROR;
    if (!NT_SUCCESS(Status))
        goto done;

    RenameInfoSize = sizeof(FILE_RENAME_INFORMATION) +
                     (ULONG)wcslen(FileName) * sizeof(WCHAR);
    RenameInfo = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, RenameInfoSize);
    if (RenameInfo == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    RenameInfo->ReplaceIfExists = TRUE;
    RenameInfo->RootDirectory = DirHandle;
    RenameInfo->FileNameLength = (ULONG)wcslen(FileName) * sizeof(WCHAR);
    RtlCopyMemory(RenameInfo->FileName, FileName, RenameInfo->FileNameLength);

    Status = NtSetInformationFile(FileHandle, &IoStatusBlock, RenameInfo,
                                  RenameInfoSize, FileRenameInformation);

done:
    if (RenameInfo != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, RenameInfo);
    if (FileHandle != NULL)
        NtClose(FileHandle);
    if (DirHandle != NULL)
        NtClose(DirHandle);
    return Status;
}

#define DPAPI_CREDHIST_MAX_ENTRIES 4096
#define DPAPI_CREDHIST_MAX_SIZE    (12 + DPAPI_CREDHIST_MAX_ENTRIES * \
                                    DPAPI_CREDHIST_ENTRY_SIZE)

/* Overflow-safe credhist file validation: exact layout, capped count (C5). */
static
NTSTATUS
DpapiCredhistValidate(
    _In_reads_bytes_(Length) PBYTE Data,
    _In_ ULONG Length,
    _Out_ PULONG Count)
{
    ULONG Magic, Version, Entries;
    ULONGLONG Expected;

    if (Length < 12)
        return STATUS_UNSUCCESSFUL;

    RtlCopyMemory(&Magic, Data, 4);
    RtlCopyMemory(&Version, Data + 4, 4);
    RtlCopyMemory(&Entries, Data + 8, 4);
    if (Magic != DPAPI_CREDHIST_MAGIC || Version != DPAPI_CREDHIST_VERSION ||
        Entries > DPAPI_CREDHIST_MAX_ENTRIES)
    {
        return STATUS_UNSUCCESSFUL;
    }

    Expected = 12ULL + (ULONGLONG)Entries * DPAPI_CREDHIST_ENTRY_SIZE;
    if (Expected != Length)
        return STATUS_UNSUCCESSFUL;

    *Count = Entries;
    return STATUS_SUCCESS;
}

/* Read a whole file with size checks (C5): refuse HighPart, cap at MaxSize,
 * require exactly ExpectedSize when it is nonzero, and verify the read
 * length. Used in the impersonated mode; the SYSTEM mode reads through the
 * verified handles instead (DpapiSysRead). */
static
NTSTATUS
DpapiSecureReadFile(
    _In_ PCWSTR Path,
    _In_ DPAPI_IO_MODE IoMode,
    _In_ ULONG MaxSize,
    _In_ ULONG ExpectedSize,
    _Outptr_result_bytebuffer_(*DataLength) PBYTE *Data,
    _Out_ PULONG DataLength)
{
    HANDLE FileHandle;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_STANDARD_INFORMATION StandardInfo;
    PBYTE Buffer = NULL;
    NTSTATUS Status;

    *Data = NULL;
    *DataLength = 0;

    if (IoMode == DpapiIoSystem)
        return STATUS_INVALID_PARAMETER;

    FileHandle = CreateFileW(Path, FILE_GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (FileHandle == INVALID_HANDLE_VALUE)
    {
        FileHandle = NULL;
        return RtlGetLastNtStatus();
    }

    Status = NtQueryInformationFile(FileHandle, &IoStatusBlock, &StandardInfo,
                                    sizeof(StandardInfo), FileStandardInformation);
    if (!NT_SUCCESS(Status))
        goto done;

    if (StandardInfo.EndOfFile.HighPart != 0 ||
        StandardInfo.EndOfFile.LowPart > MaxSize ||
        (ExpectedSize != 0 && StandardInfo.EndOfFile.LowPart != ExpectedSize))
    {
        Status = STATUS_FILE_TOO_LARGE;
        goto done;
    }

    Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0,
                             StandardInfo.EndOfFile.LowPart != 0
                             ? StandardInfo.EndOfFile.LowPart : 1);
    if (Buffer == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    Status = NtReadFile(FileHandle, NULL, NULL, NULL, &IoStatusBlock,
                        Buffer, StandardInfo.EndOfFile.LowPart, NULL, NULL);
    if (NT_SUCCESS(Status) && IoStatusBlock.Information != StandardInfo.EndOfFile.LowPart)
        Status = STATUS_UNEXPECTED_IO_ERROR;
    if (!NT_SUCCESS(Status))
        goto done;

    *Data = Buffer;
    *DataLength = StandardInfo.EndOfFile.LowPart;
    Buffer = NULL;

done:
    if (Buffer != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
    if (FileHandle != NULL)
        NtClose(FileHandle);
    return Status;
}

/* SYSTEM-mode I/O works exclusively through this verified handle tree (C1):
 * the profile root is opened as an NT path, then every component down to
 * Protect\<SID> is opened relative to the previous handle with a reparse
 * refusal, so no path string after the root and no junction can redirect a
 * create, read or rename. */
typedef struct _DPAPI_PROTECT_TREE
{
    HANDLE RootHandle;
    HANDLE ProtectHandle;
    HANDLE SidHandle;
} DPAPI_PROTECT_TREE;

static
VOID
DpapiCloseProtectTree(
    _Inout_ DPAPI_PROTECT_TREE *Tree);

static
NTSTATUS
DpapiOpenTreeStep(
    _In_ HANDLE Parent,
    _In_ PCWSTR Name,
    _Out_ PHANDLE Next)
{
    return DpapiOpenNoReparse(Parent, Name, FILE_LIST_DIRECTORY,
                              FILE_OPEN, FILE_DIRECTORY_FILE, Next);
}

static
NTSTATUS
DpapiOpenProtectTree(
    _In_ PSID Sid,
    _Out_ DPAPI_PROTECT_TREE *Tree)
{
    WCHAR SidString[64];
    UNICODE_STRING SidUnicode;
    WCHAR ProfilePath[MAX_PATH];
    WCHAR NtPath[MAX_PATH + 8];
    NTSTATUS Status;

    Tree->RootHandle = NULL;
    Tree->ProtectHandle = NULL;
    Tree->SidHandle = NULL;

    RtlInitEmptyUnicodeString(&SidUnicode, SidString, sizeof(SidString));
    if (!NT_SUCCESS(RtlConvertSidToUnicodeString(&SidUnicode, Sid, FALSE)))
        return STATUS_UNSUCCESSFUL;

    Status = DpapiReadProfileImagePath(&SidUnicode, ProfilePath, ARRAYSIZE(ProfilePath));
    if (!NT_SUCCESS(Status))
        return Status;

    _snwprintf(NtPath, ARRAYSIZE(NtPath), L"\\??\\%s", ProfilePath);
    Status = DpapiOpenNoReparse(NULL, NtPath, FILE_LIST_DIRECTORY,
                                FILE_OPEN, FILE_DIRECTORY_FILE, &Tree->RootHandle);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = DpapiOpenTreeStep(Tree->RootHandle, L"AppData", &Tree->ProtectHandle);
    if (NT_SUCCESS(Status))
    {
        HANDLE Next = NULL;

        Status = DpapiOpenTreeStep(Tree->ProtectHandle, L"Roaming", &Next);
        if (NT_SUCCESS(Status))
        {
            NtClose(Tree->ProtectHandle);
            Tree->ProtectHandle = Next;
            Status = DpapiOpenTreeStep(Tree->ProtectHandle, L"Microsoft", &Next);
        }
        if (NT_SUCCESS(Status))
        {
            NtClose(Tree->ProtectHandle);
            Tree->ProtectHandle = Next;
            Status = DpapiOpenTreeStep(Tree->ProtectHandle, L"Protect", &Next);
        }
        if (NT_SUCCESS(Status))
        {
            NtClose(Tree->ProtectHandle);
            Tree->ProtectHandle = Next;
            Status = DpapiOpenTreeStep(Tree->ProtectHandle, SidString, &Tree->SidHandle);
        }
    }
    if (!NT_SUCCESS(Status))
    {
        DpapiCloseProtectTree(Tree);
        return Status;
    }
    return STATUS_SUCCESS;
}

static
VOID
DpapiCloseProtectTree(
    _Inout_ DPAPI_PROTECT_TREE *Tree)
{
    if (Tree->SidHandle != NULL)
        NtClose(Tree->SidHandle);
    if (Tree->ProtectHandle != NULL)
        NtClose(Tree->ProtectHandle);
    if (Tree->RootHandle != NULL)
        NtClose(Tree->RootHandle);
    Tree->RootHandle = NULL;
    Tree->ProtectHandle = NULL;
    Tree->SidHandle = NULL;
}

/* Read Name relative to a verified directory handle with the C5 size checks. */
static
NTSTATUS
DpapiSysRead(
    _In_ HANDLE DirHandle,
    _In_ PCWSTR Name,
    _In_ ULONG MaxSize,
    _In_ ULONG ExpectedSize,
    _Outptr_result_bytebuffer_(*DataLength) PBYTE *Data,
    _Out_ PULONG DataLength)
{
    HANDLE FileHandle;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_STANDARD_INFORMATION StandardInfo;
    PBYTE Buffer = NULL;
    NTSTATUS Status;

    *Data = NULL;
    *DataLength = 0;

    Status = DpapiOpenNoReparse(DirHandle, Name, FILE_GENERIC_READ,
                                FILE_OPEN, FILE_NON_DIRECTORY_FILE, &FileHandle);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtQueryInformationFile(FileHandle, &IoStatusBlock, &StandardInfo,
                                    sizeof(StandardInfo), FileStandardInformation);
    if (!NT_SUCCESS(Status))
        goto done;

    if (StandardInfo.EndOfFile.HighPart != 0 ||
        StandardInfo.EndOfFile.LowPart > MaxSize ||
        (ExpectedSize != 0 && StandardInfo.EndOfFile.LowPart != ExpectedSize))
    {
        Status = STATUS_FILE_TOO_LARGE;
        goto done;
    }

    Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0,
                             StandardInfo.EndOfFile.LowPart != 0
                             ? StandardInfo.EndOfFile.LowPart : 1);
    if (Buffer == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    Status = NtReadFile(FileHandle, NULL, NULL, NULL, &IoStatusBlock,
                        Buffer, StandardInfo.EndOfFile.LowPart, NULL, NULL);
    if (NT_SUCCESS(Status) && IoStatusBlock.Information != StandardInfo.EndOfFile.LowPart)
        Status = STATUS_UNEXPECTED_IO_ERROR;
    if (!NT_SUCCESS(Status))
        goto done;

    *Data = Buffer;
    *DataLength = StandardInfo.EndOfFile.LowPart;
    Buffer = NULL;

done:
    if (Buffer != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
    NtClose(FileHandle);
    return Status;
}

/* Write Name relative to a verified directory handle by temp plus rename by
 * handle, re-applying the SD (the rename would otherwise drop it). */
static
NTSTATUS
DpapiSysWrite(
    _In_ HANDLE DirHandle,
    _In_ PCWSTR Name,
    _In_reads_bytes_(DataLength) PBYTE Data,
    _In_ ULONG DataLength,
    _In_opt_ PSECURITY_DESCRIPTOR Sd)
{
    WCHAR TempName[64];
    HANDLE FileHandle = NULL;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_RENAME_INFORMATION *RenameInfo = NULL;
    ULONG RenameInfoSize;
    NTSTATUS Status;

    _snwprintf(TempName, ARRAYSIZE(TempName), L"tmp%lx%lx",
               GetCurrentProcessId(), GetTickCount64() & 0xffffffff);

    Status = DpapiOpenNoReparse(DirHandle, TempName,
                                FILE_GENERIC_WRITE | DELETE,
                                FILE_CREATE, FILE_NON_DIRECTORY_FILE, &FileHandle);
    if (!NT_SUCCESS(Status))
        return Status;

    /* R3: a key file or credhist without the explicit DACL is the D2
     * violation; fail the write rather than leave the inherited DACL. */
    if (Sd != NULL)
    {
        Status = NtSetSecurityObject(FileHandle, DACL_SECURITY_INFORMATION, Sd);
        if (!NT_SUCCESS(Status))
            goto done;
    }

    Status = NtWriteFile(FileHandle, NULL, NULL, NULL, &IoStatusBlock,
                         (PVOID)Data, DataLength, NULL, NULL);
    if (NT_SUCCESS(Status) && IoStatusBlock.Information != DataLength)
        Status = STATUS_UNEXPECTED_IO_ERROR;
    if (!NT_SUCCESS(Status))
        goto done;

    RenameInfoSize = sizeof(FILE_RENAME_INFORMATION) +
                     (ULONG)wcslen(Name) * sizeof(WCHAR);
    RenameInfo = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, RenameInfoSize);
    if (RenameInfo == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    RenameInfo->ReplaceIfExists = TRUE;
    RenameInfo->RootDirectory = DirHandle;
    RenameInfo->FileNameLength = (ULONG)wcslen(Name) * sizeof(WCHAR);
    RtlCopyMemory(RenameInfo->FileName, Name, RenameInfo->FileNameLength);

    Status = NtSetInformationFile(FileHandle, &IoStatusBlock, RenameInfo,
                                  RenameInfoSize, FileRenameInformation);
    if (NT_SUCCESS(Status))
        TempName[0] = UNICODE_NULL;

done:
    /* R3: leave no tmp residue in the user's directory on failure. */
    if (!NT_SUCCESS(Status) && TempName[0] != UNICODE_NULL)
    {
        FILE_DISPOSITION_INFORMATION Disposition;
        IO_STATUS_BLOCK DeleteIosb;

        Disposition.DeleteFile = TRUE;
        NtSetInformationFile(FileHandle, &DeleteIosb, &Disposition,
                             sizeof(Disposition), FileDispositionInformation);
    }
    if (RenameInfo != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, RenameInfo);
    if (FileHandle != NULL)
        NtClose(FileHandle);
    return Status;
}

/* K_user for a password digest: digest || SID, matching the logon pre-key
 * shape produced by msv1_0 plus DpapiAppendCallerSid. */
static
NTSTATUS
DpapiBuildUserPrekey(
    _In_reads_bytes_(DPAPI_DIGEST_SIZE) const UCHAR *Digest,
    _In_ PSID Sid,
    _Out_ PLSA_STRING Prekey)
{
    ULONG SidLength = RtlLengthSid(Sid);

    Prekey->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0,
                                     DPAPI_DIGEST_SIZE + SidLength);
    if (Prekey->Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Prekey->Buffer, Digest, DPAPI_DIGEST_SIZE);
    RtlCopyMemory(Prekey->Buffer + DPAPI_DIGEST_SIZE, Sid, SidLength);
    Prekey->Length = DPAPI_DIGEST_SIZE + SidLength;
    Prekey->MaximumLength = Prekey->Length;
    return STATUS_SUCCESS;
}

static
VOID
DpapiFormatKeyGuid(
    _In_ const GUID *Guid,
    _Out_writes_(37) PWCHAR Name)
{
    _snwprintf(Name, 37, L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
               Guid->Data1, Guid->Data2, Guid->Data3,
               Guid->Data4[0], Guid->Data4[1], Guid->Data4[2], Guid->Data4[3],
               Guid->Data4[4], Guid->Data4[5], Guid->Data4[6], Guid->Data4[7]);
}

static
NTSTATUS
DpapiEncryptKeyFile(
    _In_ PLSA_STRING UserPrekey,
    _In_reads_bytes_(DPAPI_MASTER_KEY_SIZE) PBYTE MasterKey,
    _Out_writes_bytes_(DPAPI_KEYFILE_SIZE) PBYTE KeyFile)
{
    BYTE Derived[DPAPI_DERIVED_SIZE];
    BYTE Salt[DPAPI_SALT_SIZE];
    BYTE Iv[DPAPI_IV_SIZE];
    PBYTE Cipher = NULL;
    ULONG CipherLength = 0;
    ULONG Version = DPAPI_KEY_VERSION;
    ULONG KdfId = DPAPI_KEY_KDF_PBKDF2_SHA512;
    ULONG Iterations = DPAPI_KEY_ITERATIONS;
    NTSTATUS Status;

    Status = BCryptGenRandom(NULL, Salt, DPAPI_SALT_SIZE, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (NT_SUCCESS(Status))
        Status = BCryptGenRandom(NULL, Iv, DPAPI_IV_SIZE, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = DpapiDeriveKeys(DPAPI_SCOPE_USER_KEYFILE, UserPrekey, NULL, 0, Salt, Derived);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = DpapiAesCrypt(TRUE, Derived, Iv, MasterKey, DPAPI_MASTER_KEY_SIZE,
                           &Cipher, &CipherLength);
    if (!NT_SUCCESS(Status))
        goto done;
    if (CipherLength != DPAPI_KEY_CIPHER_SIZE)
    {
        Status = STATUS_UNSUCCESSFUL;
        goto done;
    }

    RtlCopyMemory(KeyFile, &Version, 4);
    RtlCopyMemory(KeyFile + 4, &KdfId, 4);
    RtlCopyMemory(KeyFile + 8, &Iterations, 4);
    RtlCopyMemory(KeyFile + 12, Salt, DPAPI_SALT_SIZE);
    RtlCopyMemory(KeyFile + 28, Iv, DPAPI_IV_SIZE);
    RtlCopyMemory(KeyFile + 44, Cipher, DPAPI_KEY_CIPHER_SIZE);

    Status = DpapiMac(Derived + DPAPI_AES_KEY_SIZE, KeyFile, 44 + DPAPI_KEY_CIPHER_SIZE,
                      KeyFile + 44 + DPAPI_KEY_CIPHER_SIZE);

done:
    RtlSecureZeroMemory(Derived, sizeof(Derived));
    if (Cipher != NULL)
    {
        RtlSecureZeroMemory(Cipher, CipherLength);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Cipher);
    }
    return Status;
}

static
NTSTATUS
DpapiDecryptKeyFile(
    _In_ PLSA_STRING UserPrekey,
    _In_reads_bytes_(DPAPI_KEYFILE_SIZE) PBYTE KeyFile,
    _Out_writes_bytes_(DPAPI_MASTER_KEY_SIZE) PBYTE MasterKey)
{
    BYTE Derived[DPAPI_DERIVED_SIZE];
    BYTE Mac[DPAPI_MAC_SIZE];
    PBYTE Plain = NULL;
    ULONG PlainLength = 0;
    ULONG Version;
    ULONG KdfId;
    NTSTATUS Status;

    RtlCopyMemory(&Version, KeyFile, 4);
    RtlCopyMemory(&KdfId, KeyFile + 4, 4);
    if (Version != DPAPI_KEY_VERSION || KdfId != DPAPI_KEY_KDF_PBKDF2_SHA512)
        return STATUS_NOT_SUPPORTED;

    Status = DpapiDeriveKeys(DPAPI_SCOPE_USER_KEYFILE, UserPrekey, NULL, 0,
                             KeyFile + 12, Derived);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = DpapiMac(Derived + DPAPI_AES_KEY_SIZE, KeyFile,
                      44 + DPAPI_KEY_CIPHER_SIZE, Mac);
    if (NT_SUCCESS(Status) && !DpapiEqualMac(Mac, KeyFile + 44 + DPAPI_KEY_CIPHER_SIZE))
        Status = STATUS_WRONG_PASSWORD;

    if (NT_SUCCESS(Status))
    {
        Status = DpapiAesCrypt(FALSE, Derived, KeyFile + 28, KeyFile + 44,
                               DPAPI_KEY_CIPHER_SIZE, &Plain, &PlainLength);
        if (NT_SUCCESS(Status))
        {
            if (PlainLength != DPAPI_MASTER_KEY_SIZE)
                Status = STATUS_UNSUCCESSFUL;
            else
                RtlCopyMemory(MasterKey, Plain, DPAPI_MASTER_KEY_SIZE);
        }
    }

    if (Plain != NULL)
    {
        RtlSecureZeroMemory(Plain, PlainLength);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Plain);
    }
    RtlSecureZeroMemory(Derived, sizeof(Derived));
    return Status;
}

/* Read Preferred (24 bytes: GUID + FILETIME). Missing = no preferred key. */
static
NTSTATUS
DpapiReadPreferred(
    _In_ PCWSTR Dir,
    _In_ DPAPI_IO_MODE IoMode,
    _Out_ GUID *KeyGuid)
{
    WCHAR Path[MAX_PATH * 2];
    PBYTE Data = NULL;
    ULONG DataLength;
    NTSTATUS Status;

    if (IoMode != DpapiIoImpersonated)
        return STATUS_INVALID_PARAMETER;

    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\Preferred", Dir);
    Status = DpapiSecureReadFile(Path, IoMode, 24, 24, &Data, &DataLength);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlCopyMemory(KeyGuid, Data, 16);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Data);
    return STATUS_SUCCESS;
}

static
NTSTATUS
DpapiWritePreferred(
    _In_ PCWSTR Dir,
    _In_ const GUID *KeyGuid,
    _In_ DPAPI_IO_MODE IoMode,
    _In_opt_ PSECURITY_DESCRIPTOR Sd)
{
    BYTE Data[24];
    LARGE_INTEGER Time;

    RtlCopyMemory(Data, KeyGuid, 16);
    NtQuerySystemTime(&Time);
    RtlCopyMemory(Data + 16, &Time, 8);

    /* Preferred names the key; it is written last, temp plus rename (D3). */
    return DpapiSecureWriteFile(Dir, L"Preferred", Data, sizeof(Data), IoMode, Sd);
}

/* Credhist lives in the user's Protect directory (per profile, like Win11).
 * Header { magic, version, count } then fixed-size entries, newest last.
 * Each entry holds the previous password digest, encrypted with a key derived
 * from the next (newer) password digest. The chain is not SID-bound: the file
 * itself is already per user. */
static
NTSTATUS
DpapiCredhistEncryptEntry(
    _In_reads_bytes_(DPAPI_DIGEST_SIZE) const UCHAR *OlderDigest,
    _In_reads_bytes_(DPAPI_DIGEST_SIZE) const UCHAR *NewerDigest,
    _Out_writes_bytes_(DPAPI_CREDHIST_ENTRY_SIZE) PBYTE Entry)
{
    BYTE Derived[DPAPI_DERIVED_SIZE];
    LSA_STRING NewerPrekey;
    PBYTE Cipher = NULL;
    ULONG CipherLength = 0;
    NTSTATUS Status;

    NewerPrekey.Buffer = (PCHAR)NewerDigest;
    NewerPrekey.Length = DPAPI_DIGEST_SIZE;
    NewerPrekey.MaximumLength = DPAPI_DIGEST_SIZE;

    Status = BCryptGenRandom(NULL, Entry, DPAPI_SALT_SIZE, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (NT_SUCCESS(Status))
        Status = BCryptGenRandom(NULL, Entry + DPAPI_SALT_SIZE, DPAPI_IV_SIZE,
                                 BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = DpapiDeriveKeys(DPAPI_SCOPE_CREDHIST, &NewerPrekey, NULL, 0, Entry, Derived);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = DpapiAesCrypt(TRUE, Derived, Entry + DPAPI_SALT_SIZE,
                           (PBYTE)OlderDigest, DPAPI_DIGEST_SIZE,
                           &Cipher, &CipherLength);
    if (!NT_SUCCESS(Status))
        goto done;
    if (CipherLength != 32)
    {
        Status = STATUS_UNSUCCESSFUL;
        goto done;
    }

    RtlCopyMemory(Entry + DPAPI_SALT_SIZE + DPAPI_IV_SIZE, Cipher, 32);
    Status = DpapiMac(Derived + DPAPI_AES_KEY_SIZE, Entry,
                      DPAPI_SALT_SIZE + DPAPI_IV_SIZE + 32,
                      Entry + DPAPI_SALT_SIZE + DPAPI_IV_SIZE + 32);

done:
    RtlSecureZeroMemory(Derived, sizeof(Derived));
    if (Cipher != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Cipher);
    return Status;
}

/* Decrypt credhist entry Index (1-based, newest = Count) with the digest of
 * the password that followed it, yielding the older digest. */
static
NTSTATUS
DpapiCredhistDecryptEntry(
    _In_reads_bytes_(CredhistLength) PBYTE Credhist,
    _In_ ULONG CredhistLength,
    _In_ DWORD Index,
    _In_reads_bytes_(DPAPI_DIGEST_SIZE) const UCHAR *NewerDigest,
    _Out_writes_bytes_(DPAPI_DIGEST_SIZE) PBYTE OlderDigest)
{
    BYTE Derived[DPAPI_DERIVED_SIZE];
    BYTE Mac[DPAPI_MAC_SIZE];
    LSA_STRING NewerPrekey;
    PBYTE Entry;
    PBYTE Plain = NULL;
    ULONG PlainLength = 0;
    ULONG Magic, Version, Count;
    NTSTATUS Status;

    RtlCopyMemory(&Magic, Credhist, 4);
    RtlCopyMemory(&Version, Credhist + 4, 4);
    RtlCopyMemory(&Count, Credhist + 8, 4);
    if (Magic != DPAPI_CREDHIST_MAGIC || Version != DPAPI_CREDHIST_VERSION ||
        CredhistLength < 12 || Index < 1 || Index > Count ||
        12 + Count * DPAPI_CREDHIST_ENTRY_SIZE != CredhistLength)
    {
        return STATUS_WRONG_PASSWORD;
    }

    Entry = Credhist + 12 + (Index - 1) * DPAPI_CREDHIST_ENTRY_SIZE;
    NewerPrekey.Buffer = (PCHAR)NewerDigest;
    NewerPrekey.Length = DPAPI_DIGEST_SIZE;
    NewerPrekey.MaximumLength = DPAPI_DIGEST_SIZE;

    Status = DpapiDeriveKeys(DPAPI_SCOPE_CREDHIST, &NewerPrekey, NULL, 0, Entry, Derived);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = DpapiMac(Derived + DPAPI_AES_KEY_SIZE, Entry,
                      DPAPI_SALT_SIZE + DPAPI_IV_SIZE + 32, Mac);
    if (NT_SUCCESS(Status) &&
        !DpapiEqualMac(Mac, Entry + DPAPI_SALT_SIZE + DPAPI_IV_SIZE + 32))
    {
        Status = STATUS_WRONG_PASSWORD;
    }

    if (NT_SUCCESS(Status))
        Status = DpapiAesCrypt(FALSE, Derived, Entry + DPAPI_SALT_SIZE,
                               Entry + DPAPI_SALT_SIZE + DPAPI_IV_SIZE, 32,
                               &Plain, &PlainLength);

    RtlSecureZeroMemory(Derived, sizeof(Derived));

    if (NT_SUCCESS(Status))
    {
        if (PlainLength != DPAPI_DIGEST_SIZE)
            Status = STATUS_WRONG_PASSWORD;
        else
            RtlCopyMemory(OlderDigest, Plain, DPAPI_DIGEST_SIZE);
    }

    if (Plain != NULL)
    {
        RtlSecureZeroMemory(Plain, PlainLength);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Plain);
    }
    return Status;
}

/* Decrypt a key file with the current pre-key; on WRONG_PASSWORD walk the
 * credhist chain newest-to-oldest. After a credhist hit, lazily re-encrypts
 * the file under the current key (D3/D4). */
static
NTSTATUS
DpapiOpenMasterKey(
    _In_ PCWSTR Dir,
    _In_ PCWSTR ProtectRoot,
    _In_ PSID Sid,
    _In_ PLSA_STRING CurrentPrekey,
    _In_ const GUID *KeyGuid,
    _In_ DPAPI_IO_MODE IoMode,
    _In_ BOOLEAN LazyRekey,
    _Out_writes_bytes_(DPAPI_MASTER_KEY_SIZE) PBYTE MasterKey)
{
    WCHAR Name[37];
    WCHAR Path[MAX_PATH * 2];
    WCHAR CredhistPath[MAX_PATH * 2];
    PBYTE KeyFile = NULL;
    ULONG KeyFileLength;
    PBYTE Credhist = NULL;
    ULONG CredhistLength;
    UCHAR Digest[DPAPI_DIGEST_SIZE];
    ULONG Count, Index;
    NTSTATUS Status;

    if (IoMode != DpapiIoImpersonated)
        return STATUS_INVALID_PARAMETER;

    DpapiFormatKeyGuid(KeyGuid, Name);
    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s", Dir, Name);

    Status = DpapiSecureReadFile(Path, IoMode, DPAPI_KEYFILE_SIZE, DPAPI_KEYFILE_SIZE,
                                 &KeyFile, &KeyFileLength);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = DpapiDecryptKeyFile(CurrentPrekey, KeyFile, MasterKey);
    if (Status != STATUS_WRONG_PASSWORD)
        goto done;

    /* The password changed since this key was written: walk credhist from the
     * newest entry backwards. The digest that decrypts the newest entry is the
     * current password digest (the first bytes of CurrentPrekey). Credhist
     * lives at the Protect root (C3), never inside <SID>. */
    _snwprintf(CredhistPath, ARRAYSIZE(CredhistPath), L"%s\\CREDHIST", ProtectRoot);
    Status = DpapiSecureReadFile(CredhistPath, IoMode, DPAPI_CREDHIST_MAX_SIZE, 0,
                                 &Credhist, &CredhistLength);
    if (!NT_SUCCESS(Status) ||
        !NT_SUCCESS(DpapiCredhistValidate(Credhist, CredhistLength, &Count)))
    {
        Status = STATUS_WRONG_PASSWORD;
        goto done;
    }

    RtlCopyMemory(Digest, CurrentPrekey->Buffer, DPAPI_DIGEST_SIZE);

    for (Index = Count; Index > 0; Index--)
    {
        LSA_STRING OlderPrekey = {0};

        Status = DpapiCredhistDecryptEntry(Credhist, CredhistLength, Index,
                                           Digest, Digest);
        if (!NT_SUCCESS(Status))
        {
            Status = STATUS_WRONG_PASSWORD;
            goto done;
        }

        Status = DpapiBuildUserPrekey(Digest, Sid, &OlderPrekey);
        if (!NT_SUCCESS(Status))
            goto done;

        Status = DpapiDecryptKeyFile(&OlderPrekey, KeyFile, MasterKey);
        RtlSecureZeroMemory(OlderPrekey.Buffer, OlderPrekey.Length);
        RtlFreeHeap(RtlGetProcessHeap(), 0, OlderPrekey.Buffer);

        if (Status != STATUS_WRONG_PASSWORD)
        {
            if (NT_SUCCESS(Status) && LazyRekey)
            {
                PBYTE NewFile;
                PSECURITY_DESCRIPTOR Sd = NULL;
                ULONG SdSize = 0;

                NewFile = RtlAllocateHeap(RtlGetProcessHeap(), 0, DPAPI_KEYFILE_SIZE);
                if (NewFile != NULL &&
                    NT_SUCCESS(DpapiCreateUserKeySd(Sid, &Sd, &SdSize)))
                {
                    if (NT_SUCCESS(DpapiEncryptKeyFile(CurrentPrekey, MasterKey, NewFile)))
                    {
                        DpapiSecureWriteFile(Dir, Name, NewFile, DPAPI_KEYFILE_SIZE,
                                             IoMode, Sd);
                    }
                    RtlFreeHeap(RtlGetProcessHeap(), 0, Sd);
                }
                if (NewFile != NULL)
                {
                    RtlSecureZeroMemory(NewFile, DPAPI_KEYFILE_SIZE);
                    RtlFreeHeap(RtlGetProcessHeap(), 0, NewFile);
                }
            }
            goto done;
        }
    }

    Status = STATUS_WRONG_PASSWORD;

done:
    if (KeyFile != NULL)
    {
        RtlSecureZeroMemory(KeyFile, KeyFileLength);
        RtlFreeHeap(RtlGetProcessHeap(), 0, KeyFile);
    }
    if (Credhist != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Credhist);
    RtlSecureZeroMemory(Digest, sizeof(Digest));
    return Status;
}

/* Load the preferred master key, creating it on first use. The per-SID lock
 * is held by the caller (D3). */
static
NTSTATUS
DpapiGetOrCreateMasterKey(
    _In_ PCWSTR Dir,
    _In_ PCWSTR ProtectRoot,
    _In_ PSID Sid,
    _In_ PLSA_STRING CurrentPrekey,
    _In_ DPAPI_IO_MODE IoMode,
    _Out_ GUID *KeyGuid,
    _Out_writes_bytes_(DPAPI_MASTER_KEY_SIZE) PBYTE MasterKey)
{
    WCHAR Name[37];
    WCHAR Path[MAX_PATH * 2];
    PSECURITY_DESCRIPTOR Sd = NULL;
    ULONG SdSize;
    NTSTATUS Status;

    Status = DpapiEnsureProtectDir(Dir, Sid);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = DpapiReadPreferred(Dir, IoMode, KeyGuid);
    if (NT_SUCCESS(Status))
    {
        Status = DpapiOpenMasterKey(Dir, ProtectRoot, Sid, CurrentPrekey, KeyGuid,
                                    IoMode, TRUE, MasterKey);
        if (NT_SUCCESS(Status))
            return Status;
        if (Status != STATUS_WRONG_PASSWORD && Status != STATUS_OBJECT_NAME_NOT_FOUND &&
            Status != STATUS_IO_REPARSE_TAG_MISMATCH)
        {
            return Status;
        }
        /* Fall through and make a fresh key: unreadable or missing key files
         * must not wedge the user's DPAPI (a new key just orphans old blobs,
         * exactly like a password reset does). */
    }
    else if (Status != STATUS_OBJECT_NAME_NOT_FOUND && Status != STATUS_UNSUCCESSFUL &&
             Status != STATUS_IO_REPARSE_TAG_MISMATCH)
    {
        return Status;
    }

    Status = UuidCreate(KeyGuid);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = BCryptGenRandom(NULL, MasterKey, DPAPI_MASTER_KEY_SIZE,
                             BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = DpapiCreateUserKeySd(Sid, &Sd, &SdSize);
    if (!NT_SUCCESS(Status))
        return Status;

    {
        PBYTE KeyFile = RtlAllocateHeap(RtlGetProcessHeap(), 0, DPAPI_KEYFILE_SIZE);

        if (KeyFile == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            DpapiFormatKeyGuid(KeyGuid, Name);

            Status = DpapiEncryptKeyFile(CurrentPrekey, MasterKey, KeyFile);
            if (NT_SUCCESS(Status))
            {
                /* CREATE_NEW: a concurrent creator wins and we adopt its key. */
                _snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s", Dir, Name);
                if (GetFileAttributesW(Path) == INVALID_FILE_ATTRIBUTES)
                {
                    Status = DpapiSecureWriteFile(Dir, Name, KeyFile,
                                                  DPAPI_KEYFILE_SIZE, IoMode, Sd);
                }
                else
                {
                    PBYTE OtherFile = NULL;
                    ULONG OtherLength;

                    Status = DpapiSecureReadFile(Path, IoMode, DPAPI_KEYFILE_SIZE,
                                                 DPAPI_KEYFILE_SIZE,
                                                 &OtherFile, &OtherLength);
                    if (NT_SUCCESS(Status))
                        Status = DpapiDecryptKeyFile(CurrentPrekey, OtherFile, MasterKey);
                    if (OtherFile != NULL)
                        RtlFreeHeap(RtlGetProcessHeap(), 0, OtherFile);
                    if (Status == STATUS_WRONG_PASSWORD)
                        Status = DpapiOpenMasterKey(Dir, ProtectRoot, Sid, CurrentPrekey,
                                                    KeyGuid, IoMode, TRUE, MasterKey);
                }
            }
            if (NT_SUCCESS(Status))
                Status = DpapiWritePreferred(Dir, KeyGuid, IoMode, Sd);

            RtlSecureZeroMemory(KeyFile, DPAPI_KEYFILE_SIZE);
            RtlFreeHeap(RtlGetProcessHeap(), 0, KeyFile);
        }
    }
    if (!NT_SUCCESS(Status))
        ERR("fresh master key under %s: 0x%08lx\n", debugstr_w(Dir), Status);

    RtlFreeHeap(RtlGetProcessHeap(), 0, Sd);
    return Status;
}

/* Append a credhist entry through the verified Protect handle (C1/C3),
 * rewriting the file through temp plus rename. Whole-file rewrite keeps the
 * on-disk layout canonical (header + entries). */
static
NTSTATUS
DpapiCredhistAppend(
    _In_ HANDLE ProtectHandle,
    _In_reads_bytes_(DPAPI_DIGEST_SIZE) const UCHAR *OldDigest,
    _In_reads_bytes_(DPAPI_DIGEST_SIZE) const UCHAR *NewDigest,
    _In_ PSID OwnerSid)
{
    PBYTE OldFile = NULL;
    ULONG OldLength = 0;
    PBYTE NewFile = NULL;
    ULONG Count = 0;
    ULONG NewLength;
    PSECURITY_DESCRIPTOR Sd = NULL;
    ULONG SdSize = 0;
    NTSTATUS Status;

    Status = DpapiSysRead(ProtectHandle, L"CREDHIST", DPAPI_CREDHIST_MAX_SIZE, 0,
                          &OldFile, &OldLength);
    if (NT_SUCCESS(Status))
    {
        if (!NT_SUCCESS(DpapiCredhistValidate(OldFile, OldLength, &Count)))
        {
            Status = STATUS_UNSUCCESSFUL;
            goto done;
        }
    }
    else if (Status == STATUS_OBJECT_NAME_NOT_FOUND || Status == STATUS_NOT_FOUND)
    {
        OldLength = 0;
    }
    else
    {
        goto done;
    }

    NewLength = 12 + (Count + 1) * DPAPI_CREDHIST_ENTRY_SIZE;
    NewFile = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, NewLength);
    if (NewFile == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    {
        ULONG Magic = DPAPI_CREDHIST_MAGIC;
        ULONG Version = DPAPI_CREDHIST_VERSION;
        ULONG NewCount = Count + 1;

        RtlCopyMemory(NewFile, &Magic, 4);
        RtlCopyMemory(NewFile + 4, &Version, 4);
        RtlCopyMemory(NewFile + 8, &NewCount, 4);
    }
    if (Count != 0)
        RtlCopyMemory(NewFile + 12, OldFile + 12, Count * DPAPI_CREDHIST_ENTRY_SIZE);

    Status = DpapiCredhistEncryptEntry(OldDigest, NewDigest,
                                       NewFile + 12 + Count * DPAPI_CREDHIST_ENTRY_SIZE);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = DpapiCreateUserKeySd(OwnerSid, &Sd, &SdSize);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = DpapiSysWrite(ProtectHandle, L"CREDHIST", NewFile, NewLength, Sd);

done:
    if (Sd != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Sd);
    if (NewFile != NULL)
    {
        RtlSecureZeroMemory(NewFile, NewLength);
        RtlFreeHeap(RtlGetProcessHeap(), 0, NewFile);
    }
    if (OldFile != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, OldFile);
    return Status;
}

/* Local SAM prototypes, same style as lookup.c (the generated sam.h is not
 * included tree-wide). */
typedef wchar_t *PSAMPR_SERVER_NAME;
typedef void *SAMPR_HANDLE;

typedef struct _SAMPR_RETURNED_USTRING_ARRAY
{
    unsigned long Count;
    PRPC_UNICODE_STRING Element;
} SAMPR_RETURNED_USTRING_ARRAY, *PSAMPR_RETURNED_USTRING_ARRAY;

typedef struct _SAMPR_RID_ENUMERATION
{
    ULONG RelativeId;
    RPC_UNICODE_STRING Name;
} SAMPR_RID_ENUMERATION, *PSAMPR_RID_ENUMERATION;

typedef struct _SAMPR_ENUMERATION_BUFFER
{
    unsigned long EntriesRead;
    PSAMPR_RID_ENUMERATION Buffer;
} SAMPR_ENUMERATION_BUFFER, *PSAMPR_ENUMERATION_BUFFER;

NTSTATUS
NTAPI
SamIConnect(IN PSAMPR_SERVER_NAME ServerName,
            OUT SAMPR_HANDLE *ServerHandle,
            IN ACCESS_MASK DesiredAccess,
            IN BOOLEAN Trusted);

NTSTATUS
NTAPI
SamrCloseHandle(IN OUT SAMPR_HANDLE *SamHandle);

NTSTATUS
NTAPI
SamrEnumerateDomainsInSamServer(IN SAMPR_HANDLE ServerHandle,
                                IN OUT ULONG *EnumerationContext,
                                OUT PSAMPR_ENUMERATION_BUFFER *Buffer,
                                IN ULONG PreferedMaximumLength,
                                OUT ULONG *CountReturned);

NTSTATUS
NTAPI
SamrLookupDomainInSamServer(IN SAMPR_HANDLE ServerHandle,
                            IN PRPC_UNICODE_STRING Name,
                            OUT PRPC_SID *DomainId);

NTSTATUS
NTAPI
SamrOpenDomain(IN SAMPR_HANDLE ServerHandle,
               IN ACCESS_MASK DesiredAccess,
               IN PRPC_SID DomainId,
               OUT SAMPR_HANDLE *DomainHandle);

NTSTATUS
NTAPI
SamrLookupNamesInDomain(IN SAMPR_HANDLE DomainHandle,
                        IN ULONG Count,
                        IN RPC_UNICODE_STRING Names[],
                        OUT PSAMPR_ULONG_ARRAY RelativeIds,
                        OUT PSAMPR_ULONG_ARRAY Use);

VOID
NTAPI
SamIFreeVoid(PVOID Ptr);

/* Resolve a local account name to its SID by enumerating the SAM domains. */
static
NTSTATUS
DpapiLookupAccountSid(
    _In_ PUNICODE_STRING AccountName,
    _Outptr_ PSID *Sid)
{
    SAMPR_HANDLE ServerHandle = NULL;
    SAMPR_HANDLE DomainHandle = NULL;
    PSAMPR_ENUMERATION_BUFFER EnumBuffer = NULL;
    ULONG Count = 0, Index, ResumeHandle = 0;
    NTSTATUS Status;

    *Sid = NULL;

    Status = SamIConnect(NULL, &ServerHandle,
                         SAM_SERVER_CONNECT | SAM_SERVER_ENUMERATE_DOMAINS |
                         SAM_SERVER_LOOKUP_DOMAIN,
                         TRUE);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = SamrEnumerateDomainsInSamServer(ServerHandle, &ResumeHandle,
                                             &EnumBuffer, 0x100, &Count);
    if (!NT_SUCCESS(Status) && Status != STATUS_MORE_ENTRIES)
        goto done;

    for (Index = 0; Index < Count && *Sid == NULL; Index++)
    {
        UNICODE_STRING DomainName;
        PRPC_SID DomainSid = NULL;
        RPC_UNICODE_STRING Names[1];
        SAMPR_ULONG_ARRAY RelativeIds = {0}, Use = {0};

        DomainName.Length = DomainName.MaximumLength =
            (USHORT)EnumBuffer->Buffer[Index].Name.Length;
        DomainName.Buffer = EnumBuffer->Buffer[Index].Name.Buffer;

        if (!NT_SUCCESS(SamrLookupDomainInSamServer(ServerHandle,
                                                    (PRPC_UNICODE_STRING)&DomainName,
                                                    &DomainSid)))
        {
            continue;
        }

        if (!NT_SUCCESS(SamrOpenDomain(ServerHandle, DOMAIN_LOOKUP,
                                       DomainSid, &DomainHandle)))
        {
            SamIFreeVoid(DomainSid);
            continue;
        }

        Names[0].Length = Names[0].MaximumLength = AccountName->Length;
        Names[0].Buffer = AccountName->Buffer;
        if (NT_SUCCESS(SamrLookupNamesInDomain(DomainHandle, 1, Names,
                                               &RelativeIds, &Use)) &&
            RelativeIds.Count != 0 && Use.Element != NULL &&
            Use.Element[0] == SidTypeUser)
        {
            ULONG SidLength = RtlLengthSid((PSID)DomainSid) + sizeof(ULONG);

            *Sid = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, SidLength);
            if (*Sid != NULL)
            {
                RtlCopySid(SidLength, *Sid, (PSID)DomainSid);
                (*GetSidSubAuthorityCount(*Sid))++;
                *GetSidSubAuthority(*Sid,
                                    *GetSidSubAuthorityCount(*Sid) - 1) =
                    RelativeIds.Element[0];
            }
            else
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
            }
        }

        if (RelativeIds.Element != NULL)
            SamIFreeVoid(RelativeIds.Element);
        if (Use.Element != NULL)
            SamIFreeVoid(Use.Element);
        SamrCloseHandle(&DomainHandle);
        DomainHandle = NULL;
        SamIFreeVoid(DomainSid);
    }

    if (*Sid == NULL && NT_SUCCESS(Status))
        Status = STATUS_OBJECT_NAME_NOT_FOUND;

done:
    if (DomainHandle != NULL)
        SamrCloseHandle(&DomainHandle);
    if (EnumBuffer != NULL)
        SamIFreeVoid(EnumBuffer);
    if (ServerHandle != NULL)
        SamrCloseHandle(&ServerHandle);
    return Status;
}

/* Q8 re-key entry point, called by msv1_0 after a successful interactive
 * password change. Credhist is extended FIRST (D4) so every master key left
 * under the old password stays reachable even if a re-encrypt fails. All I/O
 * goes through the verified handle tree (C1): no path strings after the
 * profile root, no junction can redirect a create or rename. */
NTSTATUS
NTAPI
LsapDpapiOnPasswordChange(
    _In_ PUNICODE_STRING DomainName,
    _In_ PUNICODE_STRING AccountName,
    _In_reads_bytes_(DPAPI_DIGEST_SIZE) const UCHAR *OldDigest,
    _In_reads_bytes_(DPAPI_DIGEST_SIZE) const UCHAR *NewDigest)
{
    DPAPI_PROTECT_TREE Tree;
    PSID Sid = NULL;
    LSA_STRING OldPrekey = {0}, NewPrekey = {0};
    PSECURITY_DESCRIPTOR Sd = NULL;
    ULONG SdSize = 0;
    PBYTE DirBuffer = NULL;
    ULONG DirBufferSize = 64 * 1024;
    NTSTATUS Status, KeyStatus;

    UNREFERENCED_PARAMETER(DomainName);

    if (AccountName == NULL || OldDigest == NULL || NewDigest == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = DpapiLookupAccountSid(AccountName, &Sid);
    if (!NT_SUCCESS(Status))
        return Status;

    /* No user token here: open the whole tree under the SYSTEM rules (C1). */
    Status = DpapiOpenProtectTree(Sid, &Tree);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = DpapiAcquireSidLock(Sid);
    if (!NT_SUCCESS(Status))
    {
        DpapiCloseProtectTree(&Tree);
        goto done;
    }

    /* D4: the chain first, so a key that fails to re-encrypt stays reachable. */
    KeyStatus = DpapiCredhistAppend(Tree.ProtectHandle, OldDigest, NewDigest, Sid);
    if (!NT_SUCCESS(KeyStatus))
        Status = KeyStatus;

    if (NT_SUCCESS(DpapiBuildUserPrekey(OldDigest, Sid, &OldPrekey)) &&
        NT_SUCCESS(DpapiBuildUserPrekey(NewDigest, Sid, &NewPrekey)) &&
        NT_SUCCESS(DpapiCreateUserKeySd(Sid, &Sd, &SdSize)))
    {
        /* Enumerate the key files on the verified <SID> handle (C1); a planted
         * junction or directory fails the no-reparse open and is skipped. */
        DirBuffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, DirBufferSize);
        if (DirBuffer != NULL)
        {
            IO_STATUS_BLOCK IoStatusBlock;
            BOOLEAN Restart = TRUE;

            while (NT_SUCCESS(NtQueryDirectoryFile(Tree.SidHandle, NULL, NULL, NULL,
                                                   &IoStatusBlock, DirBuffer,
                                                   DirBufferSize,
                                                   FileDirectoryInformation, FALSE,
                                                   NULL, Restart)))
            {
                PFILE_DIRECTORY_INFORMATION Entry;
                ULONG_PTR Offset = 0;

                Restart = FALSE;
                do
                {
                    WCHAR Name[64];
                    PBYTE KeyFile = NULL;
                    ULONG KeyFileLength;
                    BYTE MasterKey[DPAPI_MASTER_KEY_SIZE];
                    PBYTE NewFile = NULL;

                    Entry = (PFILE_DIRECTORY_INFORMATION)(DirBuffer + Offset);

                    /* R1: a long name skips THIS entry only; the rest of the
                     * batch must still re-key (the user owns this dir). */
                    if (Entry->FileNameLength < sizeof(Name))
                    {
                        RtlCopyMemory(Name, Entry->FileName, Entry->FileNameLength);
                        Name[Entry->FileNameLength / sizeof(WCHAR)] = UNICODE_NULL;

                        /* Key files are GUID-named (36 chars); skip Preferred
                         * and any temp residue. */
                        if (wcslen(Name) == 36 &&
                            NT_SUCCESS(DpapiSysRead(Tree.SidHandle, Name,
                                                    DPAPI_KEYFILE_SIZE, DPAPI_KEYFILE_SIZE,
                                                    &KeyFile, &KeyFileLength)))
                        {
                            if (NT_SUCCESS(DpapiDecryptKeyFile(&OldPrekey, KeyFile,
                                                               MasterKey)))
                            {
                                NewFile = RtlAllocateHeap(RtlGetProcessHeap(), 0,
                                                          DPAPI_KEYFILE_SIZE);
                                if (NewFile != NULL)
                                {
                                    NTSTATUS WriteStatus;

                                    if (NT_SUCCESS(DpapiEncryptKeyFile(&NewPrekey,
                                                                       MasterKey,
                                                                       NewFile)))
                                    {
                                        WriteStatus = DpapiSysWrite(Tree.SidHandle, Name,
                                                                    NewFile,
                                                                    DPAPI_KEYFILE_SIZE,
                                                                    Sd);
                                        if (!NT_SUCCESS(WriteStatus) &&
                                            NT_SUCCESS(Status))
                                        {
                                            Status = WriteStatus;
                                        }
                                    }
                                    RtlSecureZeroMemory(NewFile, DPAPI_KEYFILE_SIZE);
                                    RtlFreeHeap(RtlGetProcessHeap(), 0, NewFile);
                                }
                            }
                            RtlSecureZeroMemory(KeyFile, KeyFileLength);
                            RtlFreeHeap(RtlGetProcessHeap(), 0, KeyFile);
                            RtlSecureZeroMemory(MasterKey, sizeof(MasterKey));
                        }
                    }

                    if (Entry->NextEntryOffset == 0)
                        break;
                    Offset += Entry->NextEntryOffset;
                } while (TRUE);

                if (IoStatusBlock.Information == 0)
                    break;
            }
        }
    }

    DpapiReleaseSidLock(Sid);
    DpapiCloseProtectTree(&Tree);

done:
    if (DirBuffer != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, DirBuffer);
    if (Sd != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Sd);
    if (OldPrekey.Buffer != NULL)
    {
        RtlSecureZeroMemory(OldPrekey.Buffer, OldPrekey.Length);
        RtlFreeHeap(RtlGetProcessHeap(), 0, OldPrekey.Buffer);
    }
    if (NewPrekey.Buffer != NULL)
    {
        RtlSecureZeroMemory(NewPrekey.Buffer, NewPrekey.Length);
        RtlFreeHeap(RtlGetProcessHeap(), 0, NewPrekey.Buffer);
    }
    if (Sid != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Sid);
    return Status;
}

/* Q8 user-scope protect: the blob names the per-user master key that
 * encrypted it (D1). All file I/O runs impersonating the caller (D2). */
typedef struct _DPAPI_USERKEY_CONTEXT
{
    PSID Sid;
    GUID KeyGuid;
} DPAPI_USERKEY_CONTEXT;

static
NTSTATUS
DpapiProtectForUser(
    _In_ unsigned long Flags,
    _In_ DPAPI_BLOB DataIn,
    _In_ DPAPI_BLOB Entropy,
    _Out_ DPAPI_BLOB *DataOut)
{
    LSA_STRING Prekey = {0};
    LSA_STRING MasterKeyString;
    BYTE MasterKey[DPAPI_MASTER_KEY_SIZE];
    BYTE Derived[DPAPI_DERIVED_SIZE];
    BYTE Salt[DPAPI_SALT_SIZE];
    BYTE Iv[DPAPI_IV_SIZE];
    DPAPI_USERKEY_CONTEXT Op;
    PBYTE Cipher = NULL;
    PBYTE Blob = NULL;
    ULONG CipherLength = 0;
    ULONG BlobLength;
    ULONG Magic = DPAPI_BLOB_MAGIC_V2;
    NTSTATUS Status;

    DataOut->cbData = 0;
    DataOut->pbData = NULL;

    Status = DpapiAcquireKey(Flags, &Prekey);
    if (!NT_SUCCESS(Status))
        return Status;

    Op.Sid = NULL;
    Status = DpapiGetCallerSid(&Op.Sid);
    if (!NT_SUCCESS(Status))
        goto done;

    {
        WCHAR Dir[MAX_PATH * 2];
        WCHAR ProtectRoot[MAX_PATH * 2];

        /* The paths come from ProfileList as SYSTEM; dir creation and key
         * loading then run under the caller's token (D2). */
        Status = DpapiGetUserProtectDir(Op.Sid, Dir, sizeof(Dir));
        if (NT_SUCCESS(Status))
            Status = DpapiGetUserProtectRoot(Op.Sid, ProtectRoot, sizeof(ProtectRoot));
        if (!NT_SUCCESS(Status))
            goto done;

        if (RpcImpersonateClient(NULL) != RPC_S_OK)
        {
            Status = STATUS_CANNOT_IMPERSONATE;
            goto done;
        }

        Status = DpapiAcquireSidLock(Op.Sid);
        if (NT_SUCCESS(Status))
        {
            Status = DpapiGetOrCreateMasterKey(Dir, ProtectRoot, Op.Sid, &Prekey,
                                               DpapiIoImpersonated,
                                               &Op.KeyGuid, MasterKey);
            DpapiReleaseSidLock(Op.Sid);
        }
        RpcRevertToSelf();
    }
    if (!NT_SUCCESS(Status))
        goto done;

    Status = BCryptGenRandom(NULL, Salt, DPAPI_SALT_SIZE, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (NT_SUCCESS(Status))
        Status = BCryptGenRandom(NULL, Iv, DPAPI_IV_SIZE, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!NT_SUCCESS(Status))
        goto done;

    MasterKeyString.Buffer = (PCHAR)MasterKey;
    MasterKeyString.Length = DPAPI_MASTER_KEY_SIZE;
    MasterKeyString.MaximumLength = DPAPI_MASTER_KEY_SIZE;

    Status = DpapiDeriveKeys(DPAPI_SCOPE_BLOB_USER, &MasterKeyString,
                             Entropy.pbData, Entropy.cbData, Salt, Derived);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = DpapiAesCrypt(TRUE, Derived, Iv, DataIn.pbData, DataIn.cbData,
                           &Cipher, &CipherLength);
    if (!NT_SUCCESS(Status))
        goto done;

    BlobLength = DPAPI_V2_HEADER_SIZE + CipherLength + DPAPI_MAC_SIZE;
    Blob = midl_user_allocate(BlobLength);
    if (Blob == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    RtlCopyMemory(Blob, &Magic, 4);
    RtlCopyMemory(Blob + 4, &Flags, 4);
    RtlCopyMemory(Blob + 8, &Op.KeyGuid, 16);
    RtlCopyMemory(Blob + 24, Salt, DPAPI_SALT_SIZE);
    RtlCopyMemory(Blob + 40, Iv, DPAPI_IV_SIZE);
    RtlCopyMemory(Blob + 56, &CipherLength, 4);
    RtlCopyMemory(Blob + DPAPI_V2_HEADER_SIZE, Cipher, CipherLength);

    Status = DpapiMac(Derived + DPAPI_AES_KEY_SIZE, Blob,
                      DPAPI_V2_HEADER_SIZE + CipherLength,
                      Blob + DPAPI_V2_HEADER_SIZE + CipherLength);
    if (!NT_SUCCESS(Status))
    {
        midl_user_free(Blob);
        goto done;
    }

    DataOut->cbData = BlobLength;
    DataOut->pbData = Blob;
    Status = STATUS_SUCCESS;

done:
    if (!NT_SUCCESS(Status))
        ERR("user-scope protect failed: 0x%08lx\n", Status);
    RtlSecureZeroMemory(MasterKey, sizeof(MasterKey));
    RtlSecureZeroMemory(Derived, sizeof(Derived));
    if (Op.Sid != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Op.Sid);
    if (Prekey.Buffer != NULL)
    {
        RtlSecureZeroMemory(Prekey.Buffer, Prekey.Length);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Prekey.Buffer);
    }
    if (Cipher != NULL)
    {
        RtlSecureZeroMemory(Cipher, CipherLength);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Cipher);
    }
    return Status;
}

/* Q8 user-scope unprotect for LDP2 blobs: the blob's GUID names the master
 * key file; decryption falls back through credhist and lazily re-keys (D3). */
static
NTSTATUS
DpapiUnprotectForUser(
    _In_ DPAPI_BLOB DataIn,
    _In_reads_bytes_(DataIn.cbData) PBYTE Blob,
    _In_ ULONG CipherLength,
    _In_ DPAPI_BLOB Entropy,
    _Out_ DPAPI_BLOB *DataOut)
{
    LSA_STRING Prekey = {0};
    LSA_STRING MasterKeyString;
    BYTE MasterKey[DPAPI_MASTER_KEY_SIZE];
    BYTE Derived[DPAPI_DERIVED_SIZE];
    DPAPI_USERKEY_CONTEXT Op;
    PBYTE Plain = NULL;
    ULONG PlainLength = 0;
    GUID KeyGuid;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DataIn);

    DataOut->cbData = 0;
    DataOut->pbData = NULL;

    RtlCopyMemory(&KeyGuid, Blob + 8, 16);

    Status = DpapiAcquireKey(0, &Prekey);
    if (!NT_SUCCESS(Status))
        return Status;

    Op.Sid = NULL;
    Status = DpapiGetCallerSid(&Op.Sid);
    if (!NT_SUCCESS(Status))
        goto done;

    {
        WCHAR Dir[MAX_PATH * 2];
        WCHAR ProtectRoot[MAX_PATH * 2];

        Status = DpapiGetUserProtectDir(Op.Sid, Dir, sizeof(Dir));
        if (NT_SUCCESS(Status))
            Status = DpapiGetUserProtectRoot(Op.Sid, ProtectRoot, sizeof(ProtectRoot));
        if (!NT_SUCCESS(Status))
            goto done;

        if (RpcImpersonateClient(NULL) != RPC_S_OK)
        {
            Status = STATUS_CANNOT_IMPERSONATE;
            goto done;
        }

        Status = DpapiAcquireSidLock(Op.Sid);
        if (NT_SUCCESS(Status))
        {
            Status = DpapiOpenMasterKey(Dir, ProtectRoot, Op.Sid, &Prekey, &KeyGuid,
                                        DpapiIoImpersonated, TRUE, MasterKey);
            DpapiReleaseSidLock(Op.Sid);
        }
        RpcRevertToSelf();
    }
    if (!NT_SUCCESS(Status))
        goto done;

    MasterKeyString.Buffer = (PCHAR)MasterKey;
    MasterKeyString.Length = DPAPI_MASTER_KEY_SIZE;
    MasterKeyString.MaximumLength = DPAPI_MASTER_KEY_SIZE;

    Status = DpapiDeriveKeys(DPAPI_SCOPE_BLOB_USER, &MasterKeyString,
                             Entropy.pbData, Entropy.cbData, Blob + 24, Derived);
    if (!NT_SUCCESS(Status))
        goto done;

    {
        BYTE Mac[DPAPI_MAC_SIZE];

        Status = DpapiMac(Derived + DPAPI_AES_KEY_SIZE, Blob,
                          DPAPI_V2_HEADER_SIZE + CipherLength, Mac);
        if (NT_SUCCESS(Status) && !DpapiEqualMac(Mac, Blob + DPAPI_V2_HEADER_SIZE + CipherLength))
            Status = STATUS_UNSUCCESSFUL;
    }
    if (!NT_SUCCESS(Status))
        goto done;

    Status = DpapiAesCrypt(FALSE, Derived, Blob + 40, Blob + DPAPI_V2_HEADER_SIZE,
                           CipherLength, &Plain, &PlainLength);
    if (!NT_SUCCESS(Status))
        goto done;

    DataOut->pbData = midl_user_allocate(PlainLength != 0 ? PlainLength : 1);
    if (DataOut->pbData == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    RtlCopyMemory(DataOut->pbData, Plain, PlainLength);
    DataOut->cbData = PlainLength;
    Status = STATUS_SUCCESS;

done:
    if (!NT_SUCCESS(Status))
        ERR("user-scope unprotect failed: 0x%08lx\n", Status);
    RtlSecureZeroMemory(MasterKey, sizeof(MasterKey));
    RtlSecureZeroMemory(Derived, sizeof(Derived));
    if (Op.Sid != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Op.Sid);
    if (Prekey.Buffer != NULL)
    {
        RtlSecureZeroMemory(Prekey.Buffer, Prekey.Length);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Prekey.Buffer);
    }
    if (Plain != NULL)
    {
        RtlSecureZeroMemory(Plain, PlainLength);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Plain);
    }
    return Status;
}

long
s_DpapiProtect(
    handle_t HandleBinding,
    unsigned long Flags,
    wchar_t *Description,
    DPAPI_BLOB DataIn,
    DPAPI_BLOB Entropy,
    DPAPI_BLOB *DataOut)
{
    LSA_STRING Prekey = {0};
    BYTE Derived[DPAPI_DERIVED_SIZE];
    BYTE Salt[DPAPI_SALT_SIZE];
    BYTE Iv[DPAPI_IV_SIZE];
    PBYTE Cipher = NULL;
    PBYTE Blob = NULL;
    ULONG CipherLength = 0;
    ULONG BlobLength;
    ULONG Magic = DPAPI_BLOB_MAGIC;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(HandleBinding);
    UNREFERENCED_PARAMETER(Description);

    DataOut->cbData = 0;
    DataOut->pbData = NULL;

    if ((DataIn.cbData != 0 && DataIn.pbData == NULL) ||
        (Entropy.cbData != 0 && Entropy.pbData == NULL) ||
        DataIn.cbData > DPAPI_MAX_DATA_SIZE ||
        Entropy.cbData > DPAPI_MAX_ENTROPY_SIZE)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* User scope: the blob is bound to the caller's per-user master key (Q8/D1). */
    if (!(Flags & CRYPTPROTECT_LOCAL_MACHINE))
        return DpapiProtectForUser(Flags, DataIn, Entropy, DataOut);

    Status = DpapiAcquireKey(Flags, &Prekey);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = BCryptGenRandom(NULL, Salt, DPAPI_SALT_SIZE, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (NT_SUCCESS(Status))
        Status = BCryptGenRandom(NULL, Iv, DPAPI_IV_SIZE, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = DpapiDeriveKeys((Flags & CRYPTPROTECT_LOCAL_MACHINE) ? 1 : 0,
                             &Prekey, Entropy.pbData, Entropy.cbData, Salt, Derived);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = DpapiAesCrypt(TRUE, Derived, Iv, DataIn.pbData, DataIn.cbData, &Cipher, &CipherLength);
    if (!NT_SUCCESS(Status))
        goto done;

    BlobLength = DPAPI_HEADER_SIZE + CipherLength + DPAPI_MAC_SIZE;
    Blob = midl_user_allocate(BlobLength);
    if (Blob == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    RtlCopyMemory(Blob, &Magic, 4);
    RtlCopyMemory(Blob + 4, &Flags, 4);
    RtlCopyMemory(Blob + 8, Salt, DPAPI_SALT_SIZE);
    RtlCopyMemory(Blob + 24, Iv, DPAPI_IV_SIZE);
    RtlCopyMemory(Blob + 40, &CipherLength, 4);
    RtlCopyMemory(Blob + DPAPI_HEADER_SIZE, Cipher, CipherLength);

    Status = DpapiMac(Derived + DPAPI_AES_KEY_SIZE, Blob, DPAPI_HEADER_SIZE + CipherLength,
                      Blob + DPAPI_HEADER_SIZE + CipherLength);
    if (!NT_SUCCESS(Status))
    {
        midl_user_free(Blob);
        goto done;
    }

    DataOut->cbData = BlobLength;
    DataOut->pbData = Blob;
    Status = STATUS_SUCCESS;

done:
    RtlSecureZeroMemory(Derived, sizeof(Derived));
    if (Prekey.Buffer != NULL)
    {
        RtlSecureZeroMemory(Prekey.Buffer, Prekey.Length);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Prekey.Buffer);
    }
    if (Cipher != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Cipher);
    return Status;
}

long
s_DpapiUnprotect(
    handle_t HandleBinding,
    unsigned long Flags,
    DPAPI_BLOB DataIn,
    DPAPI_BLOB Entropy,
    DPAPI_BLOB *DataOut)
{
    LSA_STRING Prekey = {0};
    BYTE Derived[DPAPI_DERIVED_SIZE];
    BYTE Mac[DPAPI_MAC_SIZE];
    PBYTE Blob;
    PBYTE Plain = NULL;
    ULONG Magic;
    ULONG StoredFlags;
    ULONG CipherLength;
    ULONG PlainLength = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(HandleBinding);
    UNREFERENCED_PARAMETER(Flags);

    DataOut->cbData = 0;
    DataOut->pbData = NULL;

    if (DataIn.pbData == NULL ||
        (Entropy.cbData != 0 && Entropy.pbData == NULL) ||
        Entropy.cbData > DPAPI_MAX_ENTROPY_SIZE ||
        DataIn.cbData > DPAPI_HEADER_SIZE + DPAPI_MAX_DATA_SIZE + DPAPI_IV_SIZE + DPAPI_MAC_SIZE)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Blob = DataIn.pbData;
    if (DataIn.cbData < DPAPI_MIN_BLOB_SIZE)
        return STATUS_INVALID_PARAMETER;

    RtlCopyMemory(&Magic, Blob, 4);
    if (Magic == DPAPI_BLOB_MAGIC_V2)
    {
        ULONG V2CipherLength;

        if (DataIn.cbData < DPAPI_MIN_BLOB_SIZE_V2)
            return STATUS_INVALID_PARAMETER;

        RtlCopyMemory(&V2CipherLength, Blob + 56, 4);
        if (V2CipherLength > DataIn.cbData - DPAPI_MIN_BLOB_SIZE_V2 ||
            DPAPI_V2_HEADER_SIZE + V2CipherLength + DPAPI_MAC_SIZE != DataIn.cbData)
        {
            return STATUS_INVALID_PARAMETER;
        }

        RtlCopyMemory(&StoredFlags, Blob + 4, 4);
        if (StoredFlags & CRYPTPROTECT_LOCAL_MACHINE)
            return STATUS_INVALID_PARAMETER;

        return DpapiUnprotectForUser(DataIn, Blob, V2CipherLength, Entropy, DataOut);
    }
    if (Magic != DPAPI_BLOB_MAGIC)
        return STATUS_INVALID_PARAMETER;

    RtlCopyMemory(&CipherLength, Blob + 40, 4);
    if (CipherLength > DataIn.cbData - DPAPI_MIN_BLOB_SIZE ||
        DPAPI_HEADER_SIZE + CipherLength + DPAPI_MAC_SIZE != DataIn.cbData)
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory(&StoredFlags, Blob + 4, 4);

    Status = DpapiAcquireKey(StoredFlags, &Prekey);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = DpapiDeriveKeys((StoredFlags & CRYPTPROTECT_LOCAL_MACHINE) ? 1 : 0,
                             &Prekey, Entropy.pbData, Entropy.cbData, Blob + 8, Derived);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = DpapiMac(Derived + DPAPI_AES_KEY_SIZE, Blob, DPAPI_HEADER_SIZE + CipherLength, Mac);
    if (!NT_SUCCESS(Status))
        goto done;

    if (!DpapiEqualMac(Mac, Blob + DPAPI_HEADER_SIZE + CipherLength))
    {
        Status = STATUS_UNSUCCESSFUL;
        goto done;
    }

    Status = DpapiAesCrypt(FALSE, Derived, Blob + 24, Blob + DPAPI_HEADER_SIZE, CipherLength,
                           &Plain, &PlainLength);
    if (!NT_SUCCESS(Status))
        goto done;

    DataOut->pbData = midl_user_allocate(PlainLength != 0 ? PlainLength : 1);
    if (DataOut->pbData == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    RtlCopyMemory(DataOut->pbData, Plain, PlainLength);
    DataOut->cbData = PlainLength;
    Status = STATUS_SUCCESS;

done:
    RtlSecureZeroMemory(Derived, sizeof(Derived));
    if (Prekey.Buffer != NULL)
    {
        RtlSecureZeroMemory(Prekey.Buffer, Prekey.Length);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Prekey.Buffer);
    }
    if (Plain != NULL)
    {
        RtlSecureZeroMemory(Plain, PlainLength);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Plain);
    }
    return Status;
}
