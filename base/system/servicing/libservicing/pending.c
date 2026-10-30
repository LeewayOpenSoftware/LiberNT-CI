/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     pending\plan.json and pending\attempt.json (contract with LiberNTUpdatePending.exe)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * This library writes the plan and the attempt marks; the boot applier (Kevin) consumes
 * them. Rule of the stage: two boot attempts without a health confirmation -> roll back.
 * A boot marked "started" that never reached an outcome is an unexpected restart (power cut,
 * reset): it is recorded and it consumes an attempt, but it is not a verifiable failure.
 */

#include "libservicing.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define ATTEMPT_LIMIT 2

DWORD LnServicingReadPendingPlan(const LN_TARGET *t, char **planJson)
{
    WCHAR *path = LnTargetPath(t, L"pending\\plan.json");
    BYTE *data;
    size_t len;
    DWORD err;

    *planJson = NULL;
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnReadFileAll(path, &data, &len);
    free(path);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return LN_E_NOT_FOUND;
    if (err)
        return err;
    *planJson = (char *)data;
    return ERROR_SUCCESS;
}

static DWORD PlanTransactionId(const LN_TARGET *t, char txid[65])
{
    char *plan;
    LN_JSON *root;
    const char *id;
    DWORD err = LnServicingReadPendingPlan(t, &plan);

    txid[0] = 0;
    if (err)
        return err;
    err = LnJsonParse(plan, strlen(plan), &root);
    free(plan);
    if (err)
        return LN_E_MANIFEST_INVALID;
    id = LnJsonGetString(root, "transactionId");
    if (id && LnIsSha256Hex(id))
        strcpy(txid, id);
    else
        err = LN_E_MANIFEST_INVALID;
    LnJsonFree(root);
    return err;
}

static DWORD WriteAttempt(const LN_TARGET *t, const LN_ATTEMPT *a)
{
    LN_BUF b;
    WCHAR *path;
    char num[24];
    DWORD err = 0;

    LnBufInit(&b);
    err |= LnBufAppendStr(&b, "{\"schemaVersion\":1,\"transactionId\":");
    err |= LnBufAppendJsonString(&b, a->transactionId);
    LnI64ToStr(a->bootAttemptCount, num);
    err |= LnBufAppendStr(&b, ",\"bootAttemptCount\":"); err |= LnBufAppendStr(&b, num);
    LnI64ToStr(a->unexpectedRestartCount, num);
    err |= LnBufAppendStr(&b, ",\"unexpectedRestartCount\":"); err |= LnBufAppendStr(&b, num);
    LnI64ToStr(a->verifiedFailureCount, num);
    err |= LnBufAppendStr(&b, ",\"verifiedFailureCount\":"); err |= LnBufAppendStr(&b, num);
    err |= LnBufAppendStr(&b, ",\"lastStartUtc\":"); err |= LnBufAppendJsonString(&b, a->lastStartUtc);
    err |= LnBufAppendStr(&b, ",\"lastOutcome\":"); err |= LnBufAppendJsonString(&b, a->lastOutcome);
    err |= LnBufAppendStr(&b, a->healthConfirmed ? ",\"healthConfirmed\":true}\n" : ",\"healthConfirmed\":false}\n");
    if (err)
    {
        LnBufFree(&b);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    path = LnTargetPath(t, L"pending\\attempt.json");
    err = path ? LnWriteFileAtomic(path, b.data, b.len) : ERROR_NOT_ENOUGH_MEMORY;
    free(path);
    LnBufFree(&b);
    return err;
}

DWORD LnServicingReadAttempt(const LN_TARGET *t, LN_ATTEMPT *a)
{
    WCHAR *path = LnTargetPath(t, L"pending\\attempt.json");
    BYTE *data;
    size_t len;
    LN_JSON *root;
    DWORD err;
    long long v;
    BOOL bv;
    const char *s;

    memset(a, 0, sizeof(*a));
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnReadFileAll(path, &data, &len);
    free(path);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return LN_E_NOT_FOUND;
    if (err)
        return err;
    err = LnJsonParse((const char *)data, len, &root);
    free(data);
    if (err || !root || root->type != LnJsonObject)
    {
        LnJsonFree(root);
        return LN_E_MANIFEST_INVALID;
    }
    s = LnJsonGetString(root, "transactionId");
    if (s && LnIsSha256Hex(s)) strcpy(a->transactionId, s);
    if (LnJsonGetInt(root, "bootAttemptCount", &v)) a->bootAttemptCount = v;
    if (LnJsonGetInt(root, "unexpectedRestartCount", &v)) a->unexpectedRestartCount = v;
    if (LnJsonGetInt(root, "verifiedFailureCount", &v)) a->verifiedFailureCount = v;
    s = LnJsonGetString(root, "lastStartUtc");
    if (s) { strncpy(a->lastStartUtc, s, sizeof(a->lastStartUtc) - 1); }
    s = LnJsonGetString(root, "lastOutcome");
    if (s) { strncpy(a->lastOutcome, s, sizeof(a->lastOutcome) - 1); }
    if (LnJsonGetBool(root, "healthConfirmed", &bv)) a->healthConfirmed = bv;
    LnJsonFree(root);
    return ERROR_SUCCESS;
}

DWORD LnServicingMarkBootStarted(const LN_TARGET *t, LN_ATTEMPT *a)
{
    LN_ATTEMPT cur;
    char planTx[65];
    DWORD err = PlanTransactionId(t, planTx);

    if (err)
        return err;                                     /* no plan: nothing to attempt */
    err = LnServicingReadAttempt(t, &cur);
    if (err == LN_E_NOT_FOUND || (!err && strcmp(cur.transactionId, planTx)))
    {
        memset(&cur, 0, sizeof(cur));                   /* first boot of this plan */
        strcpy(cur.transactionId, planTx);
    }
    else if (err)
        return err;

    if (!strcmp(cur.lastOutcome, "started"))
    {
        /* the previous boot began and never finished: power cut or reset */
        cur.unexpectedRestartCount++;
        strcpy(cur.lastOutcome, "unexpected-restart");
    }
    cur.bootAttemptCount++;
    LnNowUtc(cur.lastStartUtc);
    strcpy(cur.lastOutcome, "started");
    err = WriteAttempt(t, &cur);                        /* flushed before the applier operates */
    if (!err && a)
        *a = cur;
    return err;
}

DWORD LnServicingMarkBootOutcome(const LN_TARGET *t, BOOL verifiedFailure, BOOL completed)
{
    LN_ATTEMPT cur;
    DWORD err = LnServicingReadAttempt(t, &cur);

    if (err)
        return err;
    if (verifiedFailure)
    {
        cur.verifiedFailureCount++;
        strcpy(cur.lastOutcome, "failed");
    }
    else if (completed)
        strcpy(cur.lastOutcome, "completed");
    else
        return ERROR_INVALID_PARAMETER;
    return WriteAttempt(t, &cur);
}

BOOL LnServicingShouldRollback(const LN_ATTEMPT *a, const char **reason)
{
    if (a->healthConfirmed)
    {
        *reason = "health-confirmed";
        return FALSE;
    }
    if (a->verifiedFailureCount > 0)
    {
        *reason = "verified-failure";
        return TRUE;
    }
    if (a->bootAttemptCount >= ATTEMPT_LIMIT)
    {
        *reason = a->unexpectedRestartCount > 0 ? "two-attempts-without-health-with-unexpected-restart"
                                                : "two-attempts-without-health";
        return TRUE;
    }
    *reason = a->unexpectedRestartCount > 0 ? "retry-after-unexpected-restart" : "within-limit";
    return FALSE;
}

/* Called by LnServicingConfirmHealth after the final hashes were re-checked. */
DWORD LnAttemptSetHealth(const LN_TARGET *t, const char *transactionId)
{
    LN_ATTEMPT cur;
    DWORD err = LnServicingReadAttempt(t, &cur);

    if (err == LN_E_NOT_FOUND)
    {
        memset(&cur, 0, sizeof(cur));
        strcpy(cur.transactionId, transactionId);
        strcpy(cur.lastOutcome, "completed");
    }
    else if (err)
        return err;
    if (strcmp(cur.transactionId, transactionId))
        return ERROR_SUCCESS;                           /* attempt belongs to another transaction */
    cur.healthConfirmed = TRUE;
    return WriteAttempt(t, &cur);
}
