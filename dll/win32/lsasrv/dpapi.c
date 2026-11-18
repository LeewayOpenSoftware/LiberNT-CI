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
#define DPAPI_PREKEY_SIZE           20

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

    PasswordLength = Prekey->Length + EntropyLength;
    if (PasswordLength == 0)
        return STATUS_INVALID_PARAMETER;

    Password = RtlAllocateHeap(RtlGetProcessHeap(), 0, PasswordLength);
    if (Password == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Password, Prekey->Buffer, Prekey->Length);
    if (EntropyLength != 0)
        RtlCopyMemory(Password + Prekey->Length, Entropy, EntropyLength);

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
    LUID LogonId;
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

    Status = DpapiGetCallerLogonId(&LogonId);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = LsapGetDpapiPrekey(&LogonId, &Prekey);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = BCryptGenRandom(NULL, Salt, DPAPI_SALT_SIZE, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (NT_SUCCESS(Status))
        Status = BCryptGenRandom(NULL, Iv, DPAPI_IV_SIZE, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!NT_SUCCESS(Status))
        goto done;

    Status = DpapiDeriveKeys(&Prekey, Entropy.pbData, Entropy.cbData, Salt, Derived);
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
    LUID LogonId;
    LSA_STRING Prekey = {0};
    BYTE Derived[DPAPI_DERIVED_SIZE];
    BYTE Mac[DPAPI_MAC_SIZE];
    PBYTE Blob;
    PBYTE Plain = NULL;
    ULONG Magic;
    ULONG CipherLength;
    ULONG PlainLength = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(HandleBinding);
    UNREFERENCED_PARAMETER(Flags);

    DataOut->cbData = 0;
    DataOut->pbData = NULL;

    if (DataIn.pbData == NULL ||
        (Entropy.cbData != 0 && Entropy.pbData == NULL) ||
        Entropy.cbData > DPAPI_MAX_ENTROPY_SIZE)
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

    Status = DpapiGetCallerLogonId(&LogonId);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = LsapGetDpapiPrekey(&LogonId, &Prekey);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = DpapiDeriveKeys(&Prekey, Entropy.pbData, Entropy.cbData, Blob + 8, Derived);
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
