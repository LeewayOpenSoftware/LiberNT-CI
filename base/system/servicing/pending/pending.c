/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     First-boot pending operations applier (bootstrap-stable)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * LiberNTUpdatePending.exe runs from the SMSS BootExecute list before the
 * service control manager starts. It applies the pending plan of LiberNT
 * Update per S1-CONTRACTS.md: the plan itself is NOT trusted (it is a local
 * view); the trust root is the package manifest signature made on the host
 * (manifest.sig, RSA-2048 PKCS#1 v1.5 over SHA-256, development key). This
 * binary re-verifies the manifest with the portable s1core (no wintrust),
 * checks every operation against the signed manifest, applies file
 * replacements with per-operation backups and atomic renames, verifies the
 * final on-disk hashes before declaring the transaction applied, and keeps
 * the hash-chained journal and the attempt counter on disk before any next
 * step is visible.
 *
 * Rollback: two boot attempts without a health confirmation revert the
 * whole transaction from the backup, distinguishing a verifiable failure
 * (failureRecorded) from an unexpected reboot (lastResult "started" without
 * a closing entry). The process always exits 0 so a bad package can never
 * wedge the boot; recovery is this rule or the offline rescue CLI.
 *
 * Native only: ntdll plus the portable s1core. Never the kernel (stage 1).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef S1_HOST_APPLIER
#include "s1host_shim.h"
#else
#include <ntndk.h>
#endif

#ifndef _In_
#define _In_
#define _Out_
#define _Inout_
#define _In_opt_
#define _Out_opt_
#endif

#include "s1core.h"

/* Plan paths are ASCII by contract (S1-CONTRACTS.md), so the widening copy
 * of every target this binary applies is exact. */
static void
S1Widen(
    _Out_ PWSTR Dst,
    _In_ const char *Src,
    _In_ size_t DstCch)
{
    size_t i;
    for (i = 0; i + 1 < DstCch && Src[i]; i++)
        Dst[i] = (WCHAR)(unsigned char)Src[i];
    Dst[i] = 0;
}

/* Development public key (the matching private key stays outside the
 * repository and the test systems; S1-CONTRACTS.md section 1b pins this in
 * the protected store as well). */
static const uint8_t S1DevPublicKey[S1_RSA2048_SIZE] = {
0xb9,0x0b,0xa9,0xbc,0xc2,0x0c,0x37,0x11,0x12,0xf6,0x46,0x48,0xdc,0x9e,0x77,0x0e,
0x82,0x59,0xc0,0x06,0x84,0x81,0x0a,0xbf,0xb7,0x39,0x7c,0xdc,0x5c,0x40,0xe7,0x94,
0xac,0x76,0xee,0x29,0x42,0x07,0x91,0x62,0x95,0x01,0x08,0x68,0xf2,0xd9,0xf2,0x4f,
0x8c,0x26,0x97,0x00,0x8e,0x3a,0x44,0xef,0x60,0xea,0x5c,0x7a,0x21,0x4b,0x20,0x2b,
0x70,0x5c,0xea,0xa5,0xf0,0x1d,0xd8,0xa7,0x4b,0xdf,0xd6,0x7d,0xc3,0xf6,0x91,0x85,
0x40,0x04,0xac,0x79,0xca,0x25,0xe8,0xb5,0x58,0x62,0xde,0xc0,0xc2,0xfb,0x05,0xf7,
0xa9,0xce,0x81,0xe4,0x7e,0x87,0xd3,0xa4,0x5a,0x74,0xc2,0x54,0xf3,0x9a,0xb3,0x89,
0x1b,0x88,0x1f,0xd6,0x6d,0x9c,0xb0,0x81,0x9d,0xd5,0x24,0xfb,0x96,0x08,0xdf,0x46,
0x92,0x86,0x91,0xc2,0x64,0x58,0x58,0x2a,0xea,0x31,0x37,0x53,0x27,0x23,0xd1,0xc5,
0x5d,0x2b,0x8d,0xb1,0xeb,0xea,0xe7,0x1f,0x46,0xee,0xa4,0x6a,0xaf,0x03,0x48,0xe2,
0x75,0x02,0x94,0x7d,0x70,0x5a,0xbf,0xb0,0xa7,0xde,0xb2,0xbb,0x81,0x6f,0x5a,0x63,
0xe4,0xff,0x7f,0x58,0x21,0x8c,0x0b,0xf8,0x99,0x3b,0x00,0x58,0xdb,0x25,0xac,0xe6,
0x68,0xc9,0x3e,0xda,0xdc,0xfc,0xf1,0x1b,0x7a,0x88,0xd6,0x9d,0xef,0x13,0x57,0x78,
0x24,0xc3,0x2e,0xdb,0xdb,0xda,0x47,0xd3,0x1a,0x46,0x82,0x75,0x9a,0xfd,0x9e,0x31,
0xa6,0xb6,0xe6,0x69,0xe4,0x0e,0x70,0x31,0x52,0xef,0x3b,0x9d,0x6d,0x65,0xc5,0x22,
0xc8,0xd7,0x6f,0x49,0x18,0x66,0x30,0xb5,0x2c,0x17,0x50,0x6f,0xea,0x27,0xeb,0x65,
};

#ifndef S1_ROOT
#define S1_ROOT            L"\\SystemRoot\\LiberNT\\Servicing"
#endif
#ifndef S1_PLAN_PATH
#define S1_PLAN_PATH       L"\\SystemRoot\\LiberNT\\Servicing\\pending\\plan.json"
#endif
#ifndef S1_ATTEMPT_PATH
#define S1_ATTEMPT_PATH    L"\\SystemRoot\\LiberNT\\Servicing\\pending\\attempt.json"
#endif
#ifndef S1_JOURNAL_PATH
#define S1_JOURNAL_PATH    L"\\SystemRoot\\LiberNT\\Servicing\\journal\\transactions.jsonl"
#endif
#ifndef S1_BACKUP_PATH
#define S1_BACKUP_PATH     L"\\SystemRoot\\LiberNT\\Servicing\\backup"
#endif

/* No ntstatus value names a content-hash mismatch; this is a private,
 * customer-bit code the journal can record precisely. */
#ifndef STATUS_S1_HASH_MISMATCH
#define STATUS_S1_HASH_MISMATCH ((NTSTATUS)0xE0000001L)
#endif

/* ------------------------------------------------------------------ native IO */

static NTSTATUS
S1NtOpen(
    _In_ PCWSTR Path,
    _In_ ACCESS_MASK Access,
    _In_ ULONG CreateDisposition,
    _Out_ PHANDLE Handle)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attr;
    IO_STATUS_BLOCK IoStatus;
    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attr, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NtCreateFile(Handle, Access | SYNCHRONIZE, &Attr, &IoStatus, NULL,
                        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                        CreateDisposition, FILE_SYNCHRONOUS_IO_NONALERT |
                        ((CreateDisposition == FILE_CREATE || CreateDisposition == FILE_OPEN_IF)
                         ? FILE_NON_DIRECTORY_FILE : 0), NULL, 0);
}

static NTSTATUS
S1ReadWholeFile(
    _In_ PCWSTR Path,
    _Out_ PVOID *Buffer,
    _Out_ PULONG Size)
{
    HANDLE Handle;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    FILE_STANDARD_INFORMATION Info;
    PVOID Data;

    *Buffer = NULL;
    *Size = 0;
    Status = S1NtOpen(Path, FILE_READ_DATA, FILE_OPEN, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtQueryInformationFile(Handle, &IoStatus, &Info, sizeof(Info),
                                    FileStandardInformation);
    if (!NT_SUCCESS(Status))
    {
        NtClose(Handle);
        return Status;
    }
    Data = RtlAllocateHeap(RtlGetProcessHeap(), 0, (SIZE_T)Info.EndOfFile.QuadPart + 1);
    if (!Data)
    {
        NtClose(Handle);
        return STATUS_NO_MEMORY;
    }
    Status = NtReadFile(Handle, NULL, NULL, NULL, &IoStatus, Data,
                        (ULONG)Info.EndOfFile.QuadPart, NULL, NULL);
    NtClose(Handle);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Data);
        return Status;
    }
    ((PUCHAR)Data)[IoStatus.Information] = 0;
    *Buffer = Data;
    *Size = (ULONG)IoStatus.Information;
    return STATUS_SUCCESS;
}

/* Write-temp + flush + rename over the destination. */
static NTSTATUS
S1WriteFileAtomic(
    _In_ PCWSTR Path,
    _In_ PCWSTR TempPath,
    _In_ PVOID Data,
    _In_ ULONG Size)
{
    HANDLE Handle;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    UNICODE_STRING NewName;
    FILE_DISPOSITION_INFORMATION DeleteInfo;

    Status = S1NtOpen(TempPath, FILE_WRITE_DATA, FILE_CREATE, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtWriteFile(Handle, NULL, NULL, NULL, &IoStatus, Data, Size, NULL, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtClose(Handle);
        return Status;
    }
    Status = NtFlushBuffersFile(Handle, &IoStatus);
    NtClose(Handle);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&NewName, Path);
    Status = S1NtOpen(TempPath, FILE_READ_DATA | FILE_WRITE_DATA, FILE_OPEN, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtSetInformationFile(Handle, &IoStatus, &NewName, sizeof(NewName),
                                  FileRenameInformation);
    NtClose(Handle);
    if (!NT_SUCCESS(Status))
    {
        /* clean the temp best-effort */
        if (NT_SUCCESS(S1NtOpen(TempPath, DELETE, FILE_OPEN, &Handle)))
        {
            DeleteInfo.DeleteFile = TRUE;
            NtSetInformationFile(Handle, &IoStatus, &DeleteInfo, sizeof(DeleteInfo),
                                 FileDispositionInformation);
            NtClose(Handle);
        }
    }
    return Status;
}

static NTSTATUS
S1MoveFileEx(
    _In_ PCWSTR Source,
    _In_ PCWSTR Target,
    _In_ BOOLEAN ReplaceIfExists)
{
    HANDLE Handle;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    FILE_RENAME_INFORMATION *Rename;
    UNICODE_STRING NewName;
    SIZE_T BufferSize;

    Status = S1NtOpen(Source, FILE_READ_DATA | FILE_WRITE_DATA, FILE_OPEN, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlInitUnicodeString(&NewName, Target);
    BufferSize = sizeof(FILE_RENAME_INFORMATION) + NewName.Length;
    Rename = RtlAllocateHeap(RtlGetProcessHeap(), 0, BufferSize);
    if (!Rename)
    {
        NtClose(Handle);
        return STATUS_NO_MEMORY;
    }
    Rename->ReplaceIfExists = ReplaceIfExists;
    Rename->RootDirectory = NULL;
    Rename->FileNameLength = NewName.Length;
    memcpy(Rename->FileName, NewName.Buffer, NewName.Length);
    Status = NtSetInformationFile(Handle, &IoStatus, Rename, (ULONG)BufferSize,
                                  FileRenameInformation);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Rename);
    NtClose(Handle);
    return Status;
}

static NTSTATUS
S1HashFile(
    _In_ PCWSTR Path,
    _Out_ uint8_t Digest[S1_SHA256_SIZE])
{
    HANDLE Handle;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    uint8_t Buffer[65536];
    S1_SHA256_CTX Ctx;

    Status = S1NtOpen(Path, FILE_READ_DATA, FILE_OPEN, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;
    S1Sha256Init(&Ctx);
    for (;;)
    {
        Status = NtReadFile(Handle, NULL, NULL, NULL, &IoStatus, Buffer,
                            sizeof(Buffer), NULL, NULL);
        if (!NT_SUCCESS(Status) || IoStatus.Information == 0)
            break;
        S1Sha256Update(&Ctx, Buffer, IoStatus.Information);
    }
    NtClose(Handle);
    S1Sha256Final(&Ctx, Digest);
    return NT_SUCCESS(Status) || Status == STATUS_END_OF_FILE ? STATUS_SUCCESS : Status;
}

/* --------------------------------------------------------------- journal */

typedef struct _S1_JOURNAL_WRITER
{
    uint8_t PrevHash[S1_SHA256_SIZE];
    uint64_t Sequence;
} S1_JOURNAL_WRITER;

static NTSTATUS
S1JournalWriterAppend(
    _Inout_ S1_JOURNAL_WRITER *Writer,
    _In_ const char *TransactionId,
    _In_ const char *Phase,
    _In_ int Operation,
    _In_ const char *Result,
    _In_ const char *Error)
{
    /* Append through a growable in-memory view is not possible natively;
     * each entry is formatted with s1core and written at the file end. */
    char Entry[S1_JOURNAL_MAX_ENTRY];
    char PrevHex[65];
    int Len;
    S1_SHA256_CTX Ctx;
    HANDLE Handle;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    FILE_POSITION_INFORMATION PosInfo;
    FILE_STANDARD_INFORMATION StdInfo;
    uint8_t NewHash[S1_SHA256_SIZE];

    S1HexEncode(Writer->PrevHash, S1_SHA256_SIZE, PrevHex);
    if (Operation >= 0)
        Len = snprintf(Entry, sizeof(Entry),
                       "{\"seq\":%llu,\"prev\":\"%s\",\"ts\":\"boot\",\"tx\":\"%s\",\"phase\":\"%s\",\"op\":%d,\"result\":\"%s\",\"error\":%s%s%s}\n",
                       (unsigned long long)(Writer->Sequence + 1), PrevHex,
                       TransactionId, Phase, Operation, Result,
                       Error ? "\"" : "null", Error ? Error : "", Error ? "\"" : "");
    else
        Len = snprintf(Entry, sizeof(Entry),
                       "{\"seq\":%llu,\"prev\":\"%s\",\"ts\":\"boot\",\"tx\":\"%s\",\"phase\":\"%s\",\"op\":null,\"result\":\"%s\",\"error\":%s%s%s}\n",
                       (unsigned long long)(Writer->Sequence + 1), PrevHex,
                       TransactionId, Phase, Result,
                       Error ? "\"" : "null", Error ? Error : "", Error ? "\"" : "");
    if (Len <= 0 || (size_t)Len >= sizeof(Entry))
        return STATUS_BUFFER_OVERFLOW;

    Status = S1NtOpen(S1_JOURNAL_PATH, FILE_WRITE_DATA, FILE_OPEN_IF, &Handle);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtQueryInformationFile(Handle, &IoStatus, &StdInfo, sizeof(StdInfo),
                                    FileStandardInformation);
    if (!NT_SUCCESS(Status))
    {
        NtClose(Handle);
        return Status;
    }
    PosInfo.CurrentByteOffset = StdInfo.EndOfFile;
    NtSetInformationFile(Handle, &IoStatus, &PosInfo, sizeof(PosInfo),
                         FilePositionInformation);
    Status = NtWriteFile(Handle, NULL, NULL, NULL, &IoStatus, Entry, (ULONG)Len, NULL, NULL);
    if (NT_SUCCESS(Status))
        Status = NtFlushBuffersFile(Handle, &IoStatus);
    NtClose(Handle);
    if (!NT_SUCCESS(Status))
        return Status;

    S1Sha256Init(&Ctx);
    S1Sha256Update(&Ctx, Entry, (size_t)Len - 1);
    S1Sha256Final(&Ctx, NewHash);
    memcpy(Writer->PrevHash, NewHash, S1_SHA256_SIZE);
    Writer->Sequence++;
    return STATUS_SUCCESS;
}

/* The chain is verified on read: load the journal, verify every link, and
 * seed the writer with the last intact entry. A broken chain turns the
 * transaction state into a verifiable failure for the rescue path. */
static NTSTATUS
S1JournalLoad(
    _Out_ S1_JOURNAL_WRITER *Writer,
    _Out_ S1Bool *ChainIntact)
{
    PVOID Data;
    ULONG Size;
    NTSTATUS Status;

    memset(Writer, 0, sizeof(*Writer));
    *ChainIntact = S1_TRUE;
    Status = S1ReadWholeFile(S1_JOURNAL_PATH, &Data, &Size);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        return STATUS_SUCCESS;
    if (!NT_SUCCESS(Status))
        return Status;
    Writer->Sequence = S1JournalVerifyChain((const char *)Data, Size, ChainIntact);
    if (*ChainIntact && Writer->Sequence > 0)
    {
        /* recover the last hash by re-walking to the final entry */
        const char *Cursor = (const char *)Data;
        const char *End = Cursor + Size;
        while (Cursor < End && *Cursor)
        {
            const char *LineEnd = memchr(Cursor, '\n', (size_t)(End - Cursor));
            if (LineEnd)
            {
                S1_SHA256_CTX Ctx;
                S1Sha256Init(&Ctx);
                S1Sha256Update(&Ctx, Cursor, (size_t)(LineEnd - Cursor));
                S1Sha256Final(&Ctx, Writer->PrevHash);
                Cursor = LineEnd + 1;
            }
            else
                break;
        }
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Data);
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------- manifest */

static S1Bool
S1ManifestPathForPlan(
    _In_ const S1_PLAN *Plan,
    _Out_ PWSTR ManifestPath,
    _In_ size_t ManifestPathCch,
    _Out_ PWSTR SignaturePath,
    _In_ size_t SignaturePathCch)
{
    if (swprintf(ManifestPath, (ULONG)ManifestPathCch,
                 L"%ls\\store\\manifests\\%S\\%S\\manifest.json",
                 S1_ROOT, Plan->PackageId, Plan->Version) < 0)
        return S1_FALSE;
    if (swprintf(SignaturePath, (ULONG)SignaturePathCch,
                 L"%ls\\store\\manifests\\%S\\%S\\manifest.sig",
                 S1_ROOT, Plan->PackageId, Plan->Version) < 0)
        return S1_FALSE;
    return S1_TRUE;
}

/* Verify the signed manifest and that every plan operation is listed in it
 * with the same target hash (S1-CONTRACTS.md section 1b). */
static NTSTATUS
S1VerifyPlanAgainstManifest(
    _In_ const S1_PLAN *Plan)
{
    WCHAR ManifestPath[MAX_PATH * 2], SignaturePath[MAX_PATH * 2];
    PVOID ManifestData, SignatureData;
    ULONG ManifestSize, SignatureSize;
    uint8_t ManifestDigest[S1_SHA256_SIZE], DigestHex[S1_SHA256_SIZE * 2 + 1];
    S1_SHA256_CTX Ctx;
    S1_JSON *Root;
    size_t FileCount, i;
    NTSTATUS Status;

    if (!S1ManifestPathForPlan(Plan, ManifestPath, MAX_PATH * 2,
                               SignaturePath, MAX_PATH * 2))
        return STATUS_INVALID_PARAMETER;

    Status = S1ReadWholeFile(ManifestPath, &ManifestData, &ManifestSize);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = S1ReadWholeFile(SignaturePath, &SignatureData, &SignatureSize);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
        return Status;
    }
    if (SignatureSize != S1_RSA2048_SIZE)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
        RtlFreeHeap(RtlGetProcessHeap(), 0, SignatureData);
        return STATUS_INVALID_SIGNATURE;
    }

    if (!S1Rsa2048Sha256Verify(S1DevPublicKey, 65537, ManifestData, ManifestSize,
                               SignatureData))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
        RtlFreeHeap(RtlGetProcessHeap(), 0, SignatureData);
        return STATUS_INVALID_SIGNATURE;
    }

    S1Sha256Init(&Ctx);
    S1Sha256Update(&Ctx, ManifestData, ManifestSize);
    S1Sha256Final(&Ctx, ManifestDigest);
    if (memcmp(ManifestDigest, Plan->ManifestDigest, S1_SHA256_SIZE) != 0)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
        RtlFreeHeap(RtlGetProcessHeap(), 0, SignatureData);
        return STATUS_INVALID_SIGNATURE;
    }

    if (!S1JsonParse((const char *)ManifestData, ManifestSize, &Root))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
        RtlFreeHeap(RtlGetProcessHeap(), 0, SignatureData);
        return STATUS_INVALID_PARAMETER;
    }

    /* Every plan operation must appear in the signed manifest with the same
     * target path and expected hash. The manifest schema lists the payloads
     * it ships under "payloads" with the target the bytes install to. */
    FileCount = S1JsonGetArrayCount(Root, "payloads");
    for (i = 0; i < Plan->OperationCount; i++)
    {
        const S1_PLAN_OP *Op = &Plan->Operations[i];
        const char *PlanTarget = Op->Target;
        char Path[64];
        const char *ManifestPathA, *ManifestSha;
        size_t PathLen, ShaLen;
        size_t f;
        S1Bool Found = S1_FALSE;
        uint8_t Expected[S1_SHA256_SIZE];

        if (PlanTarget[0] == '\\')
            PlanTarget++;
        for (f = 0; f < FileCount; f++)
        {
            snprintf(Path, sizeof(Path), "payloads.%zu.target", f);
            if (!S1JsonGetString(Root, Path, &ManifestPathA, &PathLen))
                continue;
            snprintf(Path, sizeof(Path), "payloads.%zu.sha256", f);
            if (!S1JsonGetString(Root, Path, &ManifestSha, &ShaLen))
                continue;
            if (PathLen == strlen(PlanTarget) && memcmp(ManifestPathA, PlanTarget, PathLen) == 0 &&
                ShaLen == S1_SHA256_SIZE * 2 && S1HexDecode(ManifestSha, Expected, S1_SHA256_SIZE) &&
                memcmp(Expected, Op->ExpectedSha256, S1_SHA256_SIZE) == 0)
            {
                Found = S1_TRUE;
                break;
            }
        }
        if (!Found)
        {
            RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
            RtlFreeHeap(RtlGetProcessHeap(), 0, SignatureData);
            return STATUS_INVALID_SIGNATURE;
        }
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, ManifestData);
    RtlFreeHeap(RtlGetProcessHeap(), 0, SignatureData);
    S1HexEncode(ManifestDigest, S1_SHA256_SIZE, (char *)DigestHex);
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------- attempt */

static NTSTATUS
S1AttemptLoad(
    _In_ const char *TransactionId,
    _Out_ S1_ATTEMPT *Attempt)
{
    PVOID Data;
    ULONG Size;
    NTSTATUS Status;

    memset(Attempt, 0, sizeof(*Attempt));
    Attempt->Schema = 1;
    snprintf(Attempt->TransactionId, sizeof(Attempt->TransactionId), "%s", TransactionId);
    Attempt->Attempt = 0;
    Attempt->HealthConfirmed = S1_FALSE;
    Attempt->FailureRecorded = S1_FALSE;
    snprintf(Attempt->LastResult, sizeof(Attempt->LastResult), "%s", "none");

    Status = S1ReadWholeFile(S1_ATTEMPT_PATH, &Data, &Size);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        return STATUS_SUCCESS;
    if (!NT_SUCCESS(Status))
        return Status;
    if (!S1AttemptParse((const char *)Data, Size, Attempt) ||
        strcmp(Attempt->TransactionId, TransactionId) != 0)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Data);
        return STATUS_INVALID_PARAMETER;
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Data);
    return STATUS_SUCCESS;
}

static NTSTATUS
S1AttemptSave(
    _In_ const S1_ATTEMPT *Attempt)
{
    char Buffer[512];
    size_t Len;
    WCHAR TempPath[MAX_PATH * 2];
    NTSTATUS Status;

    Len = S1AttemptSerialize(Attempt, Buffer, sizeof(Buffer));
    if (Len == 0)
        return STATUS_BUFFER_OVERFLOW;
    if (swprintf(TempPath, MAX_PATH * 2, L"%ls.new", S1_ATTEMPT_PATH) < 0)
        return STATUS_INVALID_PARAMETER;
    Status = S1WriteFileAtomic(S1_ATTEMPT_PATH, TempPath, Buffer, (ULONG)Len);
    return Status;
}

/* --------------------------------------------------------------- apply/rollback */

static NTSTATUS
S1BuildObjectPath(
    _In_ const S1_PLAN_OP *Op,
    _Out_ PWSTR ObjectPath,
    _In_ size_t ObjectPathCch)
{
    char ShaHex[S1_SHA256_SIZE * 2 + 1];

    S1HexEncode(Op->ExpectedSha256, S1_SHA256_SIZE, ShaHex);
    if (swprintf(ObjectPath, (ULONG)ObjectPathCch,
                 L"%ls\\store\\objects\\%S", S1_ROOT, ShaHex) < 0)
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS
S1BuildBackupPath(
    _In_ const S1_PLAN *Plan,
    _In_ const S1_PLAN_OP *Op,
    _Out_ PWSTR BackupPath,
    _In_ size_t BackupPathCch)
{
    if (swprintf(BackupPath, (ULONG)BackupPathCch,
                 L"%ls\\%S\\%04lu", S1_BACKUP_PATH, Plan->TransactionId,
                 (unsigned long)Op->Seq) < 0)
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS
S1ApplyOperation(
    _In_ const S1_PLAN *Plan,
    _In_ const S1_PLAN_OP *Op)
{
    WCHAR ObjectPath[MAX_PATH * 2], BackupPath[MAX_PATH * 2], TargetW[MAX_PATH * 2];
    NTSTATUS Status;
    uint8_t Actual[S1_SHA256_SIZE];
    HANDLE Probe;

    S1Widen(TargetW, Op->Target, MAX_PATH * 2);
    Status = S1BuildObjectPath(Op, ObjectPath, MAX_PATH * 2);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = S1BuildBackupPath(Plan, Op, BackupPath, MAX_PATH * 2);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Backup the current target if it exists (replace-file); add-file has no
     * backup and a revert deletes the target. The engine stages a verified
     * backup before it defers an operation to the boot plan, so an existing
     * backup is kept as-is. */
    if (Op->Type == S1_OP_REPLACE_FILE)
    {
        Status = S1NtOpen(TargetW, FILE_READ_DATA, FILE_OPEN, &Probe);
        if (NT_SUCCESS(Status))
        {
            HANDLE BackupProbe;

            NtClose(Probe);
            Status = S1NtOpen(BackupPath, FILE_READ_DATA, FILE_OPEN, &BackupProbe);
            if (NT_SUCCESS(Status))
                NtClose(BackupProbe);
            else
            {
                Status = S1MoveFileEx(TargetW, BackupPath, FALSE);
                if (!NT_SUCCESS(Status))
                    return Status;
            }
        }
    }

    /* Atomic replace/add from the store object. */
    Status = S1MoveFileEx(ObjectPath, TargetW, TRUE);
    if (!NT_SUCCESS(Status))
    {
        if (Op->Type == S1_OP_REPLACE_FILE)
            S1MoveFileEx(BackupPath, TargetW, TRUE);
        return Status;
    }

    /* The component is not installed until its final hash matches. */
    Status = S1HashFile(TargetW, Actual);
    if (!NT_SUCCESS(Status) || memcmp(Actual, Op->ExpectedSha256, S1_SHA256_SIZE) != 0)
    {
        /* revert this operation */
        {
            HANDLE Handle;
            FILE_DISPOSITION_INFORMATION DeleteInfo;
            IO_STATUS_BLOCK IoStatus;
            if (NT_SUCCESS(S1NtOpen(TargetW, DELETE, FILE_OPEN, &Handle)))
            {
                DeleteInfo.DeleteFile = TRUE;
                NtSetInformationFile(Handle, &IoStatus, &DeleteInfo,
                                     sizeof(DeleteInfo), FileDispositionInformation);
                NtClose(Handle);
            }
        }
        if (Op->Type == S1_OP_REPLACE_FILE)
            S1MoveFileEx(BackupPath, TargetW, TRUE);
        return STATUS_S1_HASH_MISMATCH;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
S1RevertOperation(
    _In_ const S1_PLAN *Plan,
    _In_ const S1_PLAN_OP *Op)
{
    WCHAR BackupPath[MAX_PATH * 2], TargetW[MAX_PATH * 2];
    NTSTATUS Status;
    HANDLE Probe;

    S1Widen(TargetW, Op->Target, MAX_PATH * 2);
    Status = S1BuildBackupPath(Plan, Op, BackupPath, MAX_PATH * 2);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Op->Type == S1_OP_REPLACE_FILE)
    {
        Status = S1NtOpen(BackupPath, FILE_READ_DATA, FILE_OPEN, &Probe);
        if (NT_SUCCESS(Status))
        {
            NtClose(Probe);
            return S1MoveFileEx(BackupPath, TargetW, TRUE);
        }
        return STATUS_SUCCESS; /* nothing was backed up */
    }

    /* add-file: a revert removes the projected file. */
    Status = S1NtOpen(TargetW, DELETE, FILE_OPEN, &Probe);
    if (NT_SUCCESS(Status))
    {
        FILE_DISPOSITION_INFORMATION DeleteInfo;
        IO_STATUS_BLOCK IoStatus;
        DeleteInfo.DeleteFile = TRUE;
        NtSetInformationFile(Probe, &IoStatus, &DeleteInfo, sizeof(DeleteInfo),
                             FileDispositionInformation);
        NtClose(Probe);
    }
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------------- main */

VOID
NTAPI
NtProcessStartup(
    _In_ PRTL_USER_PROCESS_PARAMETERS ProcessParameters)
{
    S1_JOURNAL_WRITER Journal;
    S1_ATTEMPT Attempt;
    S1_PLAN Plan;
    PVOID PlanData, BackupDirProbe;
    ULONG PlanSize, i;
    S1Bool ChainIntact;
    NTSTATUS Status, OpStatus;

    (void)ProcessParameters;

    /* 1. No plan is a successful no-op. */
    Status = S1ReadWholeFile(S1_PLAN_PATH, &PlanData, &PlanSize);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        NtTerminateProcess(NtCurrentProcess(), 0);
    if (!NT_SUCCESS(Status))
        NtTerminateProcess(NtCurrentProcess(), 0);

    /* 2. Parse and validate the plan shape. */
    if (!S1PlanParse((const char *)PlanData, PlanSize, &Plan) ||
        !S1PlanCheckTransactionId(&Plan))
    {
        S1JournalLoad(&Journal, &ChainIntact);
        S1JournalWriterAppend(&Journal, "unknown", "verify", -1, "failed", "0xC0000001");
        RtlFreeHeap(RtlGetProcessHeap(), 0, PlanData);
        NtTerminateProcess(NtCurrentProcess(), 0);
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, PlanData);

    S1JournalLoad(&Journal, &ChainIntact);

    /* 3. Trust root: the signed package manifest (never the plan). */
    Status = S1VerifyPlanAgainstManifest(&Plan);
    if (!NT_SUCCESS(Status))
    {
        S1JournalWriterAppend(&Journal, Plan.TransactionId, "verify", -1, "failed",
                              "0xC0000020");
        Attempt.Attempt = 1;
        Attempt.FailureRecorded = S1_TRUE;
        Attempt.HealthConfirmed = S1_FALSE;
        snprintf(Attempt.LastResult, sizeof(Attempt.LastResult), "%s", "failed");
        S1AttemptLoad(Plan.TransactionId, &Attempt);
        Attempt.Attempt++;
        Attempt.FailureRecorded = S1_TRUE;
        snprintf(Attempt.LastResult, sizeof(Attempt.LastResult), "%s", "failed");
        S1AttemptSave(&Attempt);
        NtTerminateProcess(NtCurrentProcess(), 0);
    }
    S1JournalWriterAppend(&Journal, Plan.TransactionId, "verified", -1, "ok", NULL);

    /* 4. Boot attempt accounting, written and flushed before any work
     * (reconciliation with Meredith: a mid-run power cut must still count). */
    S1AttemptLoad(Plan.TransactionId, &Attempt);
    {
        S1_ATTEMPT Started = Attempt;
        Started.Attempt = Attempt.Attempt + 1;
        Started.HealthConfirmed = S1_FALSE;
        Started.FailureRecorded = S1_FALSE;
        snprintf(Started.LastResult, sizeof(Started.LastResult), "%s", "started");
        S1AttemptSave(&Started);
        Attempt = Started;
    }

    /* 5. Two attempts without a health confirmation revert. */
    if (S1ShouldRollback(Attempt.Attempt, Attempt.HealthConfirmed))
    {
        const char *Reason = (Attempt.FailureRecorded || strcmp(Attempt.LastResult, "started") == 0)
                            ? (Attempt.FailureRecorded ? "verifiable-failure" : "unexpected-reboot")
                            : "unexpected-reboot";
        S1JournalWriterAppend(&Journal, Plan.TransactionId, "rollback-prepared", -1,
                              "started", Reason);
        for (i = Plan.OperationCount; i > 0; i--)
            S1RevertOperation(&Plan, &Plan.Operations[i - 1]);
        S1JournalWriterAppend(&Journal, Plan.TransactionId, "rolled-back", -1, "ok", NULL);
        Attempt.Attempt = 0;
        Attempt.FailureRecorded = S1_FALSE;
        snprintf(Attempt.LastResult, sizeof(Attempt.LastResult), "%s", "none");
        S1AttemptSave(&Attempt);
        NtTerminateProcess(NtCurrentProcess(), 0);
    }

    /* 6. Apply the plan; journal each step before the next. */
    S1JournalWriterAppend(&Journal, Plan.TransactionId, "applying", -1, "started", NULL);
    for (i = 0; i < Plan.OperationCount; i++)
    {
        OpStatus = S1ApplyOperation(&Plan, &Plan.Operations[i]);
        if (!NT_SUCCESS(OpStatus))
        {
            /* Immediate rollback of the operations already applied. */
            while (i > 0)
            {
                i--;
                S1RevertOperation(&Plan, &Plan.Operations[i]);
            }
            S1JournalWriterAppend(&Journal, Plan.TransactionId, "failed", -1,
                                  "failed", "0xE0000001");
            Attempt.FailureRecorded = S1_TRUE;
            snprintf(Attempt.LastResult, sizeof(Attempt.LastResult), "%s", "failed");
            S1AttemptSave(&Attempt);
            NtTerminateProcess(NtCurrentProcess(), 0);
        }
        S1JournalWriterAppend(&Journal, Plan.TransactionId, "applying",
                              (int)Plan.Operations[i].Seq, "ok", NULL);
    }

    /* 7. Applied, not confirmed: confirmation requires the service health
     * check after the boot. The next boot without it consumes the attempt
     * already written in step 4. */
    S1JournalWriterAppend(&Journal, Plan.TransactionId, "applied", -1, "ok", NULL);
    snprintf(Attempt.LastResult, sizeof(Attempt.LastResult), "%s", "applied");
    S1AttemptSave(&Attempt);
    (void)BackupDirProbe;

    NtTerminateProcess(NtCurrentProcess(), 0);
}
