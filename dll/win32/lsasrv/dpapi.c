/*
 * PROJECT:     LiberNT Local Security Authority (LSA) Server
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     DPAPI (Data Protection API) server
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "lsasrv.h"
#include "dpapi_s.h"
#include <bcrypt.h>

#define DPAPI_BLOB_MAGIC            0x3150444C
#define DPAPI_PBKDF2_ITERATIONS     8000
#define DPAPI_SALT_SIZE             16
#define DPAPI_IV_SIZE               16
#define DPAPI_AES_KEY_SIZE          32
#define DPAPI_HMAC_KEY_SIZE         64
#define DPAPI_MAC_SIZE              64
#define DPAPI_DERIVED_SIZE          (DPAPI_AES_KEY_SIZE + DPAPI_HMAC_KEY_SIZE)
#define DPAPI_HEADER_SIZE           (4 + 4 + DPAPI_SALT_SIZE + DPAPI_IV_SIZE + 4)
#define DPAPI_MIN_BLOB_SIZE         (DPAPI_HEADER_SIZE + DPAPI_MAC_SIZE)
#define DPAPI_MAX_DATA_SIZE         (16 * 1024 * 1024)
#define DPAPI_MAX_ENTROPY_SIZE      (64 * 1024)

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

VOID
DpapiServerInitialize(VOID)
{
    RtlInitializeCriticalSection(&DpapiMachineKeyLock);
}

static NTSTATUS DpapiGetCallerLogonId(_Out_ PLUID LogonId);

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

    Status = LsapCreateSecretSd(&SecretSd, &SecretSdSize);
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

    return LsapGetDpapiPrekey(&LogonId, Key);
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
