/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Bundle manifest loading and signature verification for the bootstrap
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "bootstrap.h"

/* Development public key, identical to the one embedded in the boot applier
 * (base/system/servicing/pending/pending.c); pinned by S1-CONTRACTS.md 1b. */
static const uint8_t BsDevPublicKey[S1_RSA2048_SIZE] = {
0xd0,0x5f,0x78,0x21,0x47,0x1b,0xf6,0xcd,0x23,0xba,0x6e,0x6b,0xa9,0x98,0x3e,0xfa,
0x52,0xb1,0x45,0xf2,0x74,0xd8,0x52,0x2a,0x9c,0x4a,0x50,0xce,0xd3,0x14,0xae,0xcb,
0xdf,0x46,0xe4,0xff,0x83,0x1f,0x0c,0x24,0xe4,0x78,0x72,0xb3,0x46,0xd4,0xdd,0xc8,
0x1e,0x99,0x94,0x34,0xdd,0x84,0xc5,0x41,0x00,0xc6,0x4c,0x5c,0x6f,0x44,0xa1,0xfb,
0x80,0x8c,0x82,0xed,0x67,0x4b,0x9f,0xc8,0x10,0x5d,0x0b,0x07,0x86,0x78,0xed,0x5e,
0x11,0x53,0x23,0x59,0x90,0xce,0xc4,0x60,0x6b,0xc9,0x31,0xe4,0xcd,0x44,0x51,0xb6,
0x73,0x18,0xc8,0x03,0xd6,0xd3,0xb5,0xd9,0x69,0x69,0x98,0x9b,0xb8,0xd9,0x15,0xd8,
0xa4,0x5a,0xc5,0x3b,0x42,0x32,0xe1,0xe7,0x32,0xef,0xa7,0xaa,0x6b,0xdf,0x5a,0x60,
0xca,0xd5,0xa5,0x64,0x60,0x84,0x89,0xdb,0xec,0x2e,0x69,0x7d,0x12,0x94,0xe6,0x10,
0xf5,0xa5,0xea,0x97,0x54,0x89,0x88,0xe1,0x0e,0xbb,0x85,0x6b,0xad,0xe3,0x2b,0x32,
0x7b,0x5d,0xae,0xac,0xae,0x2d,0x12,0x88,0x65,0xe3,0x36,0xdc,0x1c,0xd3,0x1e,0xc9,
0xba,0xe4,0xbc,0xb4,0x81,0xaf,0x33,0xfe,0x33,0x02,0xce,0xdb,0x5d,0x17,0x76,0xd5,
0x9f,0x96,0xb8,0x9a,0x62,0x96,0xaf,0x91,0x96,0x13,0xdf,0xe1,0x31,0x50,0x1a,0xdd,
0xe0,0xc2,0xde,0xe6,0x5f,0x9f,0x53,0x96,0x54,0x05,0x80,0xe6,0x20,0xf3,0x11,0x5b,
0xb6,0x64,0x1f,0x69,0xa0,0xce,0x5c,0x20,0xd2,0xaf,0x67,0x6f,0x8c,0x0d,0xae,0x21,
0xb3,0x5e,0xa3,0x91,0xb3,0xc6,0xf2,0x56,0x10,0x48,0xde,0x33,0x5a,0x3d,0x88,0x87,
};

const uint8_t *
BsDevPublicKeyBytes(VOID)
{
    return BsDevPublicKey;
}

static BOOL
BsReadWholeFile(
    _In_ PCWSTR Path,
    _Out_ uint8_t **Data,
    _Out_ ULONG *Size)
{
    HANDLE Handle;
    LARGE_INTEGER FileSize;
    DWORD Read;
    uint8_t *Buffer;

    *Data = NULL;
    *Size = 0;
    Handle = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!GetFileSizeEx(Handle, &FileSize) || FileSize.QuadPart <= 0 ||
        FileSize.QuadPart > 16 * 1024 * 1024)
    {
        CloseHandle(Handle);
        SetLastError(ERROR_FILE_INVALID);
        return FALSE;
    }
    Buffer = malloc((size_t)FileSize.QuadPart);
    if (!Buffer)
    {
        CloseHandle(Handle);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (!ReadFile(Handle, Buffer, (DWORD)FileSize.QuadPart, &Read, NULL) ||
        Read != (DWORD)FileSize.QuadPart)
    {
        free(Buffer);
        CloseHandle(Handle);
        SetLastError(ERROR_READ_FAULT);
        return FALSE;
    }
    CloseHandle(Handle);
    *Data = Buffer;
    *Size = (ULONG)FileSize.QuadPart;
    return TRUE;
}

static BOOL
BsCopyNarrow(
    _Out_ void *Dst,
    _In_ size_t DstSize,
    _In_ const char *Value,
    _In_ size_t ValueLength,
    _In_ BOOL ToWide)
{
    if (ValueLength + 1 > DstSize)
        return FALSE;
    if (ToWide)
    {
        size_t i;
        WCHAR *Wide = Dst;
        for (i = 0; i < ValueLength; i++)
            Wide[i] = (WCHAR)(unsigned char)Value[i];
        Wide[ValueLength] = 0;
    }
    else
    {
        memcpy(Dst, Value, ValueLength);
        ((char *)Dst)[ValueLength] = 0;
    }
    return TRUE;
}

static BOOL
BsParseFiles(
    _Inout_ BS_BUNDLE *Bundle,
    _In_ const char *Json,
    _In_ ULONG Size)
{
    S1_JSON *Root;
    size_t Count, i;

    if (!S1JsonParse(Json, Size, &Root))
        return FALSE;

    Count = S1JsonGetArrayCount(Root, "binaries");
    if (Count == 0 || Count > RTL_NUMBER_OF(Bundle->Files))
    {
        S1JsonFree(Root);
        return FALSE;
    }

    for (i = 0; i < Count; i++)
    {
        BS_BUNDLE_FILE *File = &Bundle->Files[i];
        char Path[64];
        const char *Value;
        size_t ValueLength;
        S1Bool BoolValue;

        snprintf(Path, sizeof(Path), "binaries.%lu.source", (unsigned long)i);
        if (!S1JsonGetString(Root, Path, &Value, &ValueLength) ||
            !BsCopyNarrow(File->Source, sizeof(File->Source), Value, ValueLength, TRUE))
            goto fail;
        snprintf(Path, sizeof(Path), "binaries.%lu.dest", (unsigned long)i);
        if (!S1JsonGetString(Root, Path, &Value, &ValueLength) ||
            !BsCopyNarrow(File->Dest, sizeof(File->Dest), Value, ValueLength, TRUE))
            goto fail;
        snprintf(Path, sizeof(Path), "binaries.%lu.sha256", (unsigned long)i);
        if (!S1JsonGetString(Root, Path, &Value, &ValueLength) ||
            ValueLength != 64 ||
            !BsCopyNarrow(File->Sha256, sizeof(File->Sha256), Value, ValueLength, FALSE))
            goto fail;
        snprintf(Path, sizeof(Path), "binaries.%lu.required", (unsigned long)i);
        if (S1JsonGetBool(Root, Path, &BoolValue) == S1_FALSE)
            File->Required = TRUE;
        else
            File->Required = BoolValue != 0;
        snprintf(Path, sizeof(Path), "binaries.%lu.service", (unsigned long)i);
        if (S1JsonGetBool(Root, Path, &BoolValue) != S1_FALSE)
            File->IsService = BoolValue != 0;
        Bundle->FileCount++;
    }

    S1JsonFree(Root);
    return TRUE;

fail:
    S1JsonFree(Root);
    return FALSE;
}

DWORD
BsBundleLoad(
    _Out_ BS_BUNDLE *Bundle,
    _In_ PCWSTR BundlePath)
{
    WCHAR ManifestPath[BS_MAX_PATH_BUF];
    uint8_t *Manifest;
    ULONG ManifestSize;
    S1_SHA256_CTX Ctx;
    uint8_t Digest[S1_SHA256_SIZE];
    ULONG i;

    memset(Bundle, 0, sizeof(*Bundle));
    wcscpy(Bundle->Path, BundlePath);

    if (swprintf(ManifestPath, RTL_NUMBER_OF(ManifestPath),
                 L"%s\\bootstrap-manifest.json", BundlePath) < 0)
        return ERROR_FILENAME_EXCED_RANGE;
    if (!BsReadWholeFile(ManifestPath, &Manifest, &ManifestSize))
        return GetLastError();

    S1Sha256Init(&Ctx);
    S1Sha256Update(&Ctx, Manifest, ManifestSize);
    S1Sha256Final(&Ctx, Digest);
    S1HexEncode(Digest, S1_SHA256_SIZE, Bundle->ManifestDigest);

    if (!BsParseFiles(Bundle, (const char *)Manifest, ManifestSize))
    {
        free(Manifest);
        return ERROR_BAD_FORMAT;
    }
    free(Manifest);

    for (i = 0; i < Bundle->FileCount; i++)
    {
        WCHAR SourcePath[BS_MAX_PATH_BUF];
        WIN32_FILE_ATTRIBUTE_DATA Attr;

        if (swprintf(SourcePath, RTL_NUMBER_OF(SourcePath),
                     L"%s\\%s", BundlePath, Bundle->Files[i].Source) < 0)
            return ERROR_FILENAME_EXCED_RANGE;
        if (!GetFileAttributesExW(SourcePath, GetFileExInfoStandard, &Attr))
            return GetLastError();
        Bundle->TotalBytes += ((ULONG64)Attr.nFileSizeHigh << 32) | Attr.nFileSizeLow;
    }
    return ERROR_SUCCESS;
}

DWORD
BsBundleVerifySignature(
    _In_ const BS_BUNDLE *Bundle)
{
    WCHAR ManifestPath[BS_MAX_PATH_BUF], SignaturePath[BS_MAX_PATH_BUF];
    uint8_t *Manifest = NULL, *Signature = NULL;
    ULONG ManifestSize, SignatureSize;
    S1Bool Ok;

    if (swprintf(ManifestPath, RTL_NUMBER_OF(ManifestPath),
                 L"%s\\bootstrap-manifest.json", Bundle->Path) < 0 ||
        swprintf(SignaturePath, RTL_NUMBER_OF(SignaturePath),
                 L"%s\\bootstrap-manifest.sig", Bundle->Path) < 0)
        return ERROR_FILENAME_EXCED_RANGE;
    if (!BsReadWholeFile(ManifestPath, &Manifest, &ManifestSize) ||
        !BsReadWholeFile(SignaturePath, &Signature, &SignatureSize))
    {
        free(Manifest);
        free(Signature);
        return GetLastError();
    }
    if (SignatureSize != S1_RSA2048_SIZE)
    {
        free(Manifest);
        free(Signature);
        return ERROR_BAD_LENGTH;
    }
    Ok = S1Rsa2048Sha256Verify(BsDevPublicKey, 65537, Manifest, ManifestSize, Signature);
    free(Manifest);
    free(Signature);
    return Ok == S1_TRUE ? ERROR_SUCCESS : ERROR_TRUST_FAILURE;
}
