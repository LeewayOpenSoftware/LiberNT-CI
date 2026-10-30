/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     pending\plan.json and pending\attempt.json (S1-CONTRACTS.md sections 1 and 5)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * attempt.json: {"schema":1,"tx":"<id>","attempt":N,"healthConfirmed":false,
 *                "failureRecorded":false,"lastResult":"started|applied|failed"}
 *
 * The boot applier calls LnServicingShouldRollback BEFORE LnServicingMarkBootStarted. The mark
 * writes attempt+1 and lastResult=started with a flush before anything is changed, so a power
 * cut still consumes an attempt (a counter written only at the end of a run would never advance
 * after a crash). healthConfirmed is written only by the service (LnServicingConfirmHealth)
 * and resets attempt to 0.
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
    err |= LnBufAppendStr(&b, "{\"schema\":1,\"tx\":");
    err |= LnBufAppendJsonString(&b, a->transactionId);
    LnI64ToStr(a->attempt, num);
    err |= LnBufAppendStr(&b, ",\"attempt\":");
    err |= LnBufAppendStr(&b, num);
    err |= LnBufAppendStr(&b, a->healthConfirmed ? ",\"healthConfirmed\":true" : ",\"healthConfirmed\":false");
    err |= LnBufAppendStr(&b, a->failureRecorded ? ",\"failureRecorded\":true" : ",\"failureRecorded\":false");
    err |= LnBufAppendStr(&b, ",\"lastResult\":");
    err |= LnBufAppendJsonString(&b, a->lastResult);
    err |= LnBufAppendStr(&b, "}\n");
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
        return LN_E_NOT_FOUND;                          /* absent = new plan, attempt 0 */
    if (err)
        return err;
    err = LnJsonParse((const char *)data, len, &root);
    free(data);
    if (err || !root || root->type != LnJsonObject || !LnJsonGetInt(root, "schema", &v) || v != 1)
    {
        LnJsonFree(root);
        return LN_E_MANIFEST_INVALID;
    }
    s = LnJsonGetString(root, "tx");
    if (s && LnIsSha256Hex(s)) strcpy(a->transactionId, s);
    if (LnJsonGetInt(root, "attempt", &v)) a->attempt = v;
    if (LnJsonGetBool(root, "healthConfirmed", &bv)) a->healthConfirmed = bv;
    if (LnJsonGetBool(root, "failureRecorded", &bv)) a->failureRecorded = bv;
    s = LnJsonGetString(root, "lastResult");
    if (s) { strncpy(a->lastResult, s, sizeof(a->lastResult) - 1); }
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

    strcpy(cur.previousResult, cur.lastResult);         /* "started" here = the last boot never finished */
    cur.attempt++;
    strcpy(cur.lastResult, "started");
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
        cur.failureRecorded = TRUE;
        strcpy(cur.lastResult, "failed");
    }
    else if (completed)
        strcpy(cur.lastResult, "applied");
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
    if (a->attempt >= ATTEMPT_LIMIT)
    {
        *reason = a->failureRecorded ? "verifiable-failure" : "unexpected-reboot";
        return TRUE;
    }
    *reason = "within-limit";
    return FALSE;
}

/* Called by LnServicingConfirmHealth after the final hashes were re-checked: the only writer of
 * healthConfirmed. It resets the attempt counter. */
DWORD LnAttemptSetHealth(const LN_TARGET *t, const char *transactionId)
{
    LN_ATTEMPT cur;
    DWORD err = LnServicingReadAttempt(t, &cur);

    if (err == LN_E_NOT_FOUND)
    {
        memset(&cur, 0, sizeof(cur));
        strcpy(cur.transactionId, transactionId);
    }
    else if (err)
        return err;
    if (strcmp(cur.transactionId, transactionId))
        return ERROR_SUCCESS;                           /* attempt belongs to another transaction */
    cur.healthConfirmed = TRUE;
    cur.failureRecorded = FALSE;
    cur.attempt = 0;
    strcpy(cur.lastResult, "applied");
    return WriteAttempt(t, &cur);
}
