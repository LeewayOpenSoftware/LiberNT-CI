/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     First-boot pending operations applier (bootstrap-stable)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * LiberNTUpdatePending.exe: runs from the SMSS BootExecute list before the
 * service control manager starts. It applies the signed pending plan of
 * LiberNT Update, journals every step, verifies final hashes before
 * declaring a component installed, and reverts from backup when an
 * operation fails or the boot has been attempted twice without a health
 * confirmation. Native only: ntdll and the DLLs already mapped by the
 * session manager; it must keep working even when the very components it
 * updates are damaged. The kernel is never updated in stage 1.
 *
 * Integration note: this target is deliberately NOT wired into the build
 * tree nor the ISO in stage 1 (owner condition 3, bootstrap-only). The
 * bootstrap installs it online or offline and adds it to BootExecute
 * without removing existing entries (autocheck AutoChk.exe * stays first).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winnt.h>
#include <winternl.h>
#include <ntstatus.h>
#include <ndk/ntndk.h>

#define S1_SERVICING_ROOT   L"\\SystemRoot\\LiberNT\\Servicing"
#define S1_PLAN_PATH        L"\\SystemRoot\\LiberNT\\Servicing\\pending\\plan.json"
#define S1_ATTEMPT_PATH     L"\\SystemRoot\\LiberNT\\Servicing\\pending\\attempt.json"
#define S1_JOURNAL_DIR      L"\\SystemRoot\\LiberNT\\Servicing\\journal"
#define S1_JOURNAL_PATH     L"\\SystemRoot\\LiberNT\\Servicing\\journal\\transactions.jsonl"
#define S1_STATE_PATH       L"\\SystemRoot\\LiberNT\\Servicing\\journal\\state.json"
#define S1_BACKUP_DIR       L"\\SystemRoot\\LiberNT\\Servicing\\backup"
#define S1_MAX_BOOT_ATTEMPTS 2

/*
 * Plan model (DESIGN-v0 sections 5-7). The on-disk JSON schema is owned by
 * the journal/plan contract shared with Oscar (libservicing); the structs
 * below are the in-memory shape this applier consumes. The plan is signed;
 * the development public key is embedded here and the matching private key
 * stays outside the repository and the test systems (owner condition 6).
 */
typedef enum _S1_OPERATION_TYPE
{
    S1OpReplaceFile,   /* swap a projected file with a staged object */
    S1OpAddFile,       /* project a new file */
    S1OpServiceConfig, /* service configuration change */
    S1OpRegistryValue  /* registry value under the servicing hive paths */
} S1_OPERATION_TYPE;

typedef struct _S1_OPERATION
{
    S1_OPERATION_TYPE Type;
    ULONG Sequence;
    WCHAR ComponentId[64];
    WCHAR Version[32];
    WCHAR SourceObject[260];      /* store object path (immutable, hashed) */
    WCHAR TargetPath[260];        /* projected path */
    WCHAR ExpectedSha256[64 + 1]; /* lowercase hex, pre-calculated */
    WCHAR BackupPath[260];        /* per-operation backup written at apply */
} S1_OPERATION;

typedef struct _S1_PLAN
{
    ULONG SchemaVersion;
    WCHAR TransactionId[64];
    WCHAR TargetIdentity[128];
    WCHAR PackageDigest[64 + 1];
    ULONG OperationCount;
    S1_OPERATION Operations[ANYSIZE_ARRAY];
} S1_PLAN;

static VOID
S1LogF(
    _In_ PCWSTR Format,
    ...)
{
    /* TODO(S1): append-only JSONL journal entry + optional serial log.
     * Every journal entry is written and flushed before the next step is
     * made visible (owner condition: the journal survives interruptions). */
}

static NTSTATUS
S1ReadEntireFile(
    _In_ PCWSTR Path,
    _Out_ PVOID *Buffer,
    _Out_ PULONG Size)
{
    /* TODO(S1): NtOpenFile/NtReadFile over the servicing root; the applier
     * runs before Win32 subsystem services, so no kernel32 file APIs. */
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS
S1ParsePlan(
    _In_ PVOID PlanData,
    _In_ ULONG PlanSize,
    _Out_ PS1_PLAN *Plan)
{
    /* TODO(S1): minimal JSON reader for the plan schema (contract with
     * Oscar). The plan is rejected on any parse error: a plan this binary
     * cannot read is not applied, it is reported and left for the rescue
     * CLI (DESIGN-v0 rule 4 of the boot applier). */
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS
S1VerifyPlan(
    _In_ PS1_PLAN Plan,
    _In_ PVOID PlanData,
    _In_ ULONG PlanSize)
{
    /* TODO(S1): verify target identity against the machine, then the plan
     * signature with the embedded development public key. Owner condition 2:
     * Update packages enforce signature; a plan that does not verify is
     * never applied (no audit mode for Update). The crypto primitive is
     * bcrypt if it is mapped at this boot stage; otherwise the plan is
     * rejected and journaled, never applied silently. */
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS
S1LoadAttempt(
    _Out_ PULONG Attempt,
    _Out_ PBOOLEAN HealthConfirmed,
    _Out_ PBOOLEAN FailureRecorded)
{
    /* TODO(S1): attempt.json holds the boot attempt counter, the health
     * confirmation flag (written by the service once check-health passes)
     * and the last failure marker. Missing file means a fresh plan. */
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS
S1PersistAttempt(
    _In_ ULONG Attempt,
    _In_ BOOLEAN HealthConfirmed,
    _In_ BOOLEAN FailureRecorded)
{
    /* TODO(S1): rewrite attempt.json atomically (write, flush, rename). */
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS
S1JournalAppend(
    _In_ PCWSTR TransactionId,
    _In_ PCWSTR Phase,
    _In_ PCWSTR Operation,
    _In_ PCWSTR Result,
    _In_ PCWSTR Error OPTIONAL)
{
    /* TODO(S1): append-only JSONL: sequence, transactionId, phase,
     * operation, timestamp, result, error, previous-entry hash. The entry
     * is flushed to disk before returning. */
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS
S1VerifyObjectHash(
    _In_ PCWSTR ObjectPath,
    _In_ PCWSTR ExpectedSha256)
{
    /* TODO(S1): stream SHA-256 over the object; the component is never
     * marked installed before its final on-disk hash matches (owner
     * condition). */
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS
S1BackupForOperation(
    _In_ PS1_OPERATION Operation)
{
    /* TODO(S1): for a replace, move the current target aside into the
     * transaction backup; for an add, record that the target did not
     * exist. Payloads in the store are never deleted during rollback. */
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS
S1ApplyOperation(
    _In_ PS1_OPERATION Operation)
{
    /* TODO(S1): replace/add the projected file from the staged store object
     * with an atomic rename, then verify the final hash. Registry and
     * service operations snapshot their previous value first. */
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS
S1RevertOperation(
    _In_ PS1_OPERATION Operation)
{
    /* TODO(S1): restore the per-operation backup (file or snapshot). */
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS
S1ApplyPlan(
    _In_ PS1_PLAN Plan)
{
    ULONG i;
    NTSTATUS Status;

    for (i = 0; i < Plan->OperationCount; i++)
    {
        Status = S1BackupForOperation(&Plan->Operations[i]);
        if (NT_SUCCESS(Status))
            Status = S1ApplyOperation(&Plan->Operations[i]);
        if (!NT_SUCCESS(Status))
        {
            /* Immediate rollback of the operations already applied, in
             * reverse order, using the backup. */
            while (i > 0)
            {
                i--;
                S1RevertOperation(&Plan->Operations[i]);
            }
            return Status;
        }
        S1JournalAppend(Plan->TransactionId, L"applying", Plan->Operations[i].TargetPath,
                        L"ok", NULL);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
S1RollbackWholeTransaction(
    _In_ PS1_PLAN Plan)
{
    /* TODO(S1): rollback de primer arranque — restore the whole transaction
     * backup and mark the transaction rolled-back in the journal. */
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * Two boot attempts without a health confirmation trigger the automatic
 * rollback (owner condition 4). The rule distinguishes:
 *   - unexpected reboot (power loss, reset): the previous boot wrote no
 *     failure marker; the attempt counter alone reaches the limit;
 *   - verifiable failure: the previous boot journaled a failure (health
 *     check failed or an operation/applier error): the failure marker is
 *     present. Both paths revert, but the journal records which one it was,
 *     because diagnostics and rescue guidance differ.
 */
static BOOLEAN
S1ShouldRollback(
    _In_ ULONG Attempt,
    _In_ BOOLEAN HealthConfirmed,
    _In_ BOOLEAN FailureRecorded)
{
    if (HealthConfirmed)
        return FALSE;
    if (FailureRecorded)
        return Attempt >= S1_MAX_BOOT_ATTEMPTS;
    return Attempt >= S1_MAX_BOOT_ATTEMPTS;
}

VOID
NTAPI
NtProcessStartup(
    _In_ PRTL_USER_PROCESS_PARAMETERS ProcessParameters)
{
    PS1_PLAN Plan = NULL;
    PVOID PlanData = NULL;
    ULONG PlanSize = 0, Attempt = 0;
    BOOLEAN HealthConfirmed = FALSE, FailureRecorded = FALSE;
    NTSTATUS Status;

    /* 1. No plan is a successful no-op: the boot must continue. */
    Status = S1ReadEntireFile(S1_PLAN_PATH, &PlanData, &PlanSize);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        NtTerminateProcess(NtCurrentProcess(), 0);
    if (!NT_SUCCESS(Status))
    {
        /* Without a readable plan the applier cannot prove what should
         * happen; it logs and exits zero so the boot continues, leaving the
         * diagnosis to the rescue CLI. */
        S1LogF(L"plan unreadable: 0x%08lx", Status);
        NtTerminateProcess(NtCurrentProcess(), 0);
    }

    /* 2. Parse and verify the signed plan. */
    Status = S1ParsePlan(PlanData, PlanSize, &Plan);
    if (NT_SUCCESS(Status))
        Status = S1VerifyPlan(Plan, PlanData, PlanSize);
    if (!NT_SUCCESS(Status))
    {
        S1JournalAppend(L"unknown", L"verify", L"plan", L"failed", NULL);
        NtTerminateProcess(NtCurrentProcess(), 0);
    }

    /* 3. Boot attempt accounting and the two-attempt rollback rule. */
    S1LoadAttempt(&Attempt, &HealthConfirmed, &FailureRecorded);
    if (S1ShouldRollback(Attempt, HealthConfirmed, FailureRecorded))
    {
        S1JournalAppend(Plan->TransactionId, L"rollback",
                        FailureRecorded ? L"verifiable-failure" : L"unexpected-reboot",
                        L"started", NULL);
        S1RollbackWholeTransaction(Plan);
        S1JournalAppend(Plan->TransactionId, L"rollback", NULL, L"rolled-back", NULL);
        S1PersistAttempt(0, FALSE, FALSE);
        NtTerminateProcess(NtCurrentProcess(), 0);
    }

    /* 4. Apply the plan; each step is journaled before the next. */
    S1JournalAppend(Plan->TransactionId, L"applying", NULL, L"started", NULL);
    Status = S1ApplyPlan(Plan);
    if (NT_SUCCESS(Status))
    {
        /* Applied, not confirmed: confirmation requires the service health
         * check after the boot (DESIGN-v0 states). The next boot without a
         * health confirmation consumes an attempt. */
        S1JournalAppend(Plan->TransactionId, L"applying", NULL, L"applied", NULL);
        S1PersistAttempt(Attempt + 1, FALSE, FALSE);
    }
    else
    {
        S1JournalAppend(Plan->TransactionId, L"applying", NULL, L"failed", NULL);
        S1PersistAttempt(Attempt + 1, FALSE, TRUE);
    }

    /* 5. The boot always continues; a failed update must never wedge the
     * machine into a retry loop here (the rollback rule above handles the
     * recovery on the next boots). */
    NtTerminateProcess(NtCurrentProcess(), 0);
}
