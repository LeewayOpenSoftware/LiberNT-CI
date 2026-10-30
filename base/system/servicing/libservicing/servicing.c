/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Transactions: add-package, apply, rollback, remove-package, confirm, export
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Rules enforced here (owner conditions, DESIGN-v0):
 *  - verification is ENFORCE and happens before anything reaches the store or the target;
 *  - every state transition is journaled (append-only, flushed) before the next step;
 *  - a backup of every target is taken and verified BEFORE the first change, and undo.json
 *    lists it, so a rollback needs only the backup files and this code (not the updated
 *    components);
 *  - nothing is reported applied before the final hash of the target was checked;
 *  - "confirmed" is only written by LnServicingConfirmHealth, after re-checking every hash;
 *  - the same request is idempotent: it resumes or returns the existing result;
 *  - user components and drivers only; the kernel and the boot path are refused (target.c).
 */

#include "libservicing.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CER_NAME L"trust\\Leeway-LiberNT-Update-DEV.cer"

BOOL LnTestLockedAsPending = FALSE;

typedef struct _CTX
{
    const LN_TARGET *t;
    LN_MANIFEST m;
    char txid[65];
    const char *op;
} CTX;

/* ---- journal + log helpers --------------------------------------------- */

static void LogLine(const LN_TARGET *t, const char *txid, const char *text)
{
    WCHAR *wid = LnUtf8ToWide(txid), rel[160], *path;
    HANDLE h;
    char ts[32];
    LN_BUF b;
    DWORD wrote;

    if (!wid)
        return;
    wcscpy(rel, L"logs\\");
    wcscat(rel, wid);
    wcscat(rel, L".log");
    free(wid);
    path = LnTargetPath(t, rel);
    if (!path)
        return;
    {
        WCHAR dir[MAX_PATH], *sl;

        wcscpy(dir, path);
        sl = wcsrchr(dir, L'\\');
        if (sl)
            *sl = 0;
        LnEnsureDir(dir);
    }
    LnNowUtc(ts);
    LnBufInit(&b);
    LnBufAppendStr(&b, ts);
    LnBufAppendStr(&b, " ");
    LnBufAppendStr(&b, text);
    LnBufAppendStr(&b, "\r\n");
    h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        WriteFile(h, b.data, (DWORD)b.len, &wrote, NULL);
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    LnBufFree(&b);
    free(path);
}

static DWORD Journal(const CTX *c, const char *phase, const char *result, BOOL reboot, BOOL health,
                     long long attempt, const char *error)
{
    LN_JENTRY e;
    DWORD err;

    memset(&e, 0, sizeof(e));
    strcpy(e.transactionId, c->txid);
    strcpy(e.targetIdentity, c->t->identity);
    strcpy(e.packageId, c->m.packageId);
    strcpy(e.packageVersion, c->m.releaseVersion);
    strcpy(e.manifestSha256, c->m.manifestSha256);
    strncpy(e.operation, c->op, sizeof(e.operation) - 1);
    strncpy(e.phase, phase, sizeof(e.phase) - 1);
    strncpy(e.result, result, sizeof(e.result) - 1);
    e.rebootRequired = reboot;
    e.healthConfirmed = health;
    e.attempt = attempt;
    if (error)
        strncpy(e.error, error, sizeof(e.error) - 1);
    err = LnJournalAppend(c->t, &e);
    if (!err)
        err = LnJournalWriteState(c->t);
    {
        char line[260];

        strcpy(line, c->op);
        strcat(line, " ");
        strcat(line, phase);
        strcat(line, " ");
        strcat(line, result);
        strcat(line, " ");
        strncat(line, error ? error : "", 160);
        LogLine(c->t, c->txid, line);
    }
    return err;
}

static const char *ErrText(DWORD err, char *buf)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    strcpy(buf, "error 0x");
    for (i = 0; i < 8; i++)
        buf[8 + i] = hex[(err >> (28 - 4 * i)) & 15];
    buf[16] = 0;
    return buf;
}

static DWORD CtxInit(CTX *c, const LN_TARGET *t, const char *id, const char *ver, const char *op)
{
    DWORD err;

    memset(c, 0, sizeof(*c));
    c->t = t;
    c->op = op;
    err = LnStoreLoadManifest(t, id, ver, &c->m);
    if (!err)
        LnTransactionId(t->identity, c->m.packageId, c->m.releaseVersion, c->m.manifestSha256, op, c->txid);
    return err;
}

static const LN_JENTRY *LastOf(const LN_JOURNAL *j, const char *txid)
{
    return LnJournalLast(j, txid);
}

/* ---- trust anchor ------------------------------------------------------ */

static DWORD PinnedCert(const LN_TARGET *t, char hash[65])
{
    WCHAR *p = LnTargetPath(t, CER_NAME);
    DWORD err;

    if (!p)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnCertFileSha256(p, hash);
    free(p);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return LN_E_NO_TRUST_ANCHOR;
    return err;
}

/* ---- add-package ------------------------------------------------------- */

DWORD LnServicingAddPackage(const LN_TARGET *t, const WCHAR *pkgDir, char txId[65])
{
    CTX c;
    LN_JOURNAL j;
    const LN_JENTRY *last;
    WCHAR *mp;
    char pinned[65], buf[48];
    DWORD err;

    if (txId)
        txId[0] = 0;
    mp = LnJoin(pkgDir, L"manifest.json");
    if (!mp)
        return ERROR_NOT_ENOUGH_MEMORY;
    memset(&c, 0, sizeof(c));
    c.t = t;
    c.op = "add-package";
    err = LnManifestLoad(mp, &c.m);
    free(mp);
    if (err)
        return err;                                     /* no identity to journal against */
    LnTransactionId(t->identity, c.m.packageId, c.m.releaseVersion, c.m.manifestSha256, c.op, c.txid);
    if (txId)
        strcpy(txId, c.txid);

    err = PinnedCert(t, pinned);
    if (!err)
        err = LnServicingVerifyPackage(pkgDir, pinned, NULL);
    if (err)
    {
        Journal(&c, "failed", "error", FALSE, FALSE, 0, ErrText(err, buf));  /* rejected before staging */
        LnManifestFree(&c.m);
        return err;
    }

    err = LnJournalRead(t, &j);
    if (err)
    {
        LnManifestFree(&c.m);
        return err;
    }
    last = LastOf(&j, c.txid);
    if (last && !strcmp(last->phase, "staged") && !strcmp(last->result, "ok"))
    {
        LN_MANIFEST stored;

        /* already staged: make sure the store still has it, otherwise re-import */
        if (!LnStoreLoadManifest(t, c.m.packageId, c.m.releaseVersion, &stored) &&
            !strcmp(stored.manifestSha256, c.m.manifestSha256))
        {
            LnManifestFree(&stored);
            LnJournalFree(&j);
            LnManifestFree(&c.m);
            return ERROR_SUCCESS;
        }
    }
    LnJournalFree(&j);

    err = LnStoreImport(t, pkgDir, &c.m);
    if (err)
        Journal(&c, "failed", "error", FALSE, FALSE, 0, ErrText(err, buf));
    else
        err = Journal(&c, "staged", "ok", FALSE, FALSE, 0, NULL);
    LnManifestFree(&c.m);
    return err;
}

/* ---- backup / undo ------------------------------------------------------ */

typedef struct _UNDO
{
    char oldSha[65];            /* "" when the target did not exist */
} UNDO;

static WCHAR *BackupDir(const CTX *c)
{
    WCHAR *w = LnUtf8ToWide(c->txid), rel[160], *p;

    if (!w)
        return NULL;
    wcscpy(rel, L"backup\\");
    wcscat(rel, w);
    free(w);
    p = LnTargetPath(c->t, rel);
    return p;
}

static WCHAR *BackupFile(const CTX *c, size_t i)
{
    WCHAR *dir = BackupDir(c), name[40], *p;
    char num[24];
    int k;

    if (!dir)
        return NULL;
    LnI64ToStr((long long)i, num);
    wcscpy(name, L"files\\");
    for (k = 0; num[k]; k++)
    {
        WCHAR one[2] = { (WCHAR)num[k], 0 };

        wcscat(name, one);
    }
    wcscat(name, L".bak");
    p = LnJoin(dir, name);
    free(dir);
    return p;
}

static DWORD WriteUndo(const CTX *c, const UNDO *u)
{
    LN_BUF b;
    WCHAR *dir = BackupDir(c), *path;
    size_t i;
    DWORD err = 0;

    if (!dir)
        return ERROR_NOT_ENOUGH_MEMORY;
    path = LnJoin(dir, L"undo.json");
    free(dir);
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    LnBufInit(&b);
    err |= LnBufAppendStr(&b, "{\"schemaVersion\":1,\"transactionId\":");
    err |= LnBufAppendJsonString(&b, c->txid);
    err |= LnBufAppendStr(&b, ",\"items\":[");
    for (i = 0; i < c->m.itemCount; i++)
    {
        char *tg = LnWideToUtf8(c->m.items[i].target);

        if (i)
            err |= LnBufAppendStr(&b, ",");
        err |= LnBufAppendStr(&b, "{\"target\":");
        err |= LnBufAppendJsonString(&b, tg ? tg : "");
        err |= LnBufAppendStr(&b, ",\"oldSha256\":");
        err |= LnBufAppendJsonString(&b, u[i].oldSha);
        err |= LnBufAppendStr(&b, "}");
        free(tg);
    }
    err |= LnBufAppendStr(&b, "]}\n");
    if (!err)
        err = LnWriteFileAtomic(path, b.data, b.len);
    else
        err = ERROR_NOT_ENOUGH_MEMORY;
    LnBufFree(&b);
    free(path);
    return err;
}

/* Reads undo.json back; the targets must match the manifest of this transaction. */
static DWORD ReadUndo(const CTX *c, UNDO *u)
{
    WCHAR *dir = BackupDir(c), *path;
    BYTE *data;
    size_t len, i = 0;
    LN_JSON *root;
    const LN_JSON *items, *it;
    DWORD err;

    if (!dir)
        return ERROR_NOT_ENOUGH_MEMORY;
    path = LnJoin(dir, L"undo.json");
    free(dir);
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
    if (err)
        return LN_E_MANIFEST_INVALID;
    items = LnJsonGet(root, "items");
    err = LN_E_MANIFEST_INVALID;
    if (items && items->type == LnJsonArray)
    {
        err = ERROR_SUCCESS;
        for (it = items->child; it && i < c->m.itemCount; it = it->next, i++)
        {
            const char *tg = LnJsonGetString(it, "target"), *old = LnJsonGetString(it, "oldSha256");
            WCHAR *wt = tg ? LnUtf8ToWide(tg) : NULL;

            if (!wt || !old || _wcsicmp(wt, c->m.items[i].target) || (*old && !LnIsSha256Hex(old)))
                err = LN_E_MANIFEST_INVALID;
            else
                strcpy(u[i].oldSha, old);
            free(wt);
        }
        if (it || i != c->m.itemCount)
            err = LN_E_MANIFEST_INVALID;
    }
    LnJsonFree(root);
    return err;
}

/* Hash of the current content of a target; "" when it does not exist. */
static DWORD CurrentHash(const WCHAR *path, char out[65])
{
    DWORD attrs = GetFileAttributesW(path), err;

    out[0] = 0;
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        err = GetLastError();
        return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? ERROR_SUCCESS : err;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        return LN_E_PRECONDITION;
    return LnSha256FileHex(path, out);
}

static DWORD TakeBackups(const CTX *c, UNDO *u)
{
    size_t i;
    DWORD err;

    for (i = 0; i < c->m.itemCount; i++)
    {
        WCHAR *tp = LnTargetFile(c->t, c->m.items[i].target);
        WCHAR *bp = BackupFile(c, i);
        char h[65];

        if (!tp || !bp)
        {
            free(tp);
            free(bp);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        err = CurrentHash(tp, u[i].oldSha);
        if (!err && u[i].oldSha[0])
        {
            err = LnCopyFileFlush(tp, bp);
            if (!err)
                err = LnSha256FileHex(bp, h);
            if (!err && strcmp(h, u[i].oldSha))
                err = LN_E_VERIFY_FINAL;                /* the backup is not the file we saw */
        }
        free(tp);
        free(bp);
        if (err)
            return err;
    }
    return WriteUndo(c, u);
}

/* ---- apply one item ----------------------------------------------------- */

static BOOL IsLockedError(DWORD e)
{
    return e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION || e == ERROR_USER_MAPPED_FILE ||
           e == ERROR_LOCK_VIOLATION;
}

/* *pending is set when the target is in use (Online only): the plan takes it at boot. */
static DWORD ApplyItem(const CTX *c, const LN_ITEM *it, BOOL *pending)
{
    WCHAR *tp = LnTargetFile(c->t, it->target), *tmp = NULL, *obj = NULL;
    char cur[65], fin[65];
    DWORD err;

    *pending = FALSE;
    if (!tp)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = CurrentHash(tp, cur);
    if (err)
        goto out;

    if (it->isDelete)
    {
        if (!cur[0])
            goto verify;
        if (!DeleteFileW(tp))
        {
            err = GetLastError();
            if ((c->t->mode == LnTargetOnline || LnTestLockedAsPending) && IsLockedError(err))
            {
                *pending = TRUE;
                err = ERROR_SUCCESS;
            }
            goto out;
        }
        goto verify;
    }

    if (!strcmp(cur, it->sha256))
        goto verify;                                    /* already in place (resumed transaction) */

    err = LnStoreObjectPath(c->t, it->sha256, &obj);
    if (err)
        goto out;
    {
        size_t n = wcslen(tp) + 24;
        WCHAR suffix[24], *parent, *sl;

        tmp = malloc(n * sizeof(WCHAR));
        if (!tmp)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            goto out;
        }
        wcscpy(tmp, tp);
        {
            WCHAR *w = LnUtf8ToWide(c->txid);

            wcscpy(suffix, L".lnupd-");
            if (w)
                wcsncat(suffix, w, 8);
            free(w);
        }
        wcscat(tmp, suffix);
        parent = _wcsdup(tp);
        sl = parent ? wcsrchr(parent, L'\\') : NULL;
        if (sl)
        {
            *sl = 0;
            err = LnEnsureDir(parent);
        }
        free(parent);
        if (err)
            goto out;
    }
    err = LnCopyFileFlush(obj, tmp);
    if (!err)
        err = LnSha256FileHex(tmp, fin);
    if (!err && strcmp(fin, it->sha256))
        err = LN_E_HASH_MISMATCH;
    if (err)
    {
        DeleteFileW(tmp);
        goto out;
    }
    if (!MoveFileExW(tmp, tp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        err = GetLastError();
        DeleteFileW(tmp);
        if ((c->t->mode == LnTargetOnline || LnTestLockedAsPending) && IsLockedError(err))
        {
            *pending = TRUE;
            err = ERROR_SUCCESS;
        }
        goto out;
    }

verify:
    /* not installed until the final state is proven */
    err = CurrentHash(tp, fin);
    if (!err)
    {
        if (it->isDelete ? fin[0] != 0 : strcmp(fin, it->sha256))
            err = LN_E_VERIFY_FINAL;
    }
out:
    free(tp);
    free(tmp);
    free(obj);
    return err;
}

/* ---- restoring from the backup (rollback) ------------------------------- */

static DWORD RestoreItem(const CTX *c, size_t i, const UNDO *u)
{
    WCHAR *tp = LnTargetFile(c->t, c->m.items[i].target), *bp = NULL, *tmp = NULL;
    char cur[65], fin[65];
    DWORD err;

    if (!tp)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = CurrentHash(tp, cur);
    if (err)
        goto out;
    if (!strcmp(cur, u[i].oldSha))
        goto out;                                       /* already as it was */

    if (!u[i].oldSha[0])
    {
        if (!DeleteFileW(tp) && GetLastError() != ERROR_FILE_NOT_FOUND)
            err = GetLastError();
    }
    else
    {
        bp = BackupFile(c, i);
        if (!bp)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            goto out;
        }
        err = LnSha256FileHex(bp, fin);
        if (!err && strcmp(fin, u[i].oldSha))
            err = LN_E_VERIFY_FINAL;                    /* the backup itself is damaged */
        if (err)
            goto out;
        tmp = malloc((wcslen(tp) + 16) * sizeof(WCHAR));
        if (!tmp)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            goto out;
        }
        wcscpy(tmp, tp);
        wcscat(tmp, L".lnundo");
        err = LnCopyFileFlush(bp, tmp);
        if (!err && !MoveFileExW(tmp, tp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            err = GetLastError();
            DeleteFileW(tmp);
        }
    }
    if (!err)
    {
        err = CurrentHash(tp, fin);
        if (!err && strcmp(fin, u[i].oldSha))
            err = LN_E_VERIFY_FINAL;
    }
out:
    free(tp);
    free(bp);
    free(tmp);
    return err;
}

static DWORD RemovePlanOf(const CTX *c)
{
    WCHAR *p = LnTargetPath(c->t, L"pending\\plan.json");
    BYTE *data;
    size_t len;
    DWORD err = LN_E_NOT_FOUND;

    if (!p)
        return ERROR_NOT_ENOUGH_MEMORY;
    if (!LnReadFileAll(p, &data, &len))
    {
        LN_JSON *root;

        if (!LnJsonParse((const char *)data, len, &root))
        {
            const char *id = LnJsonGetString(root, "transactionId");

            if (id && !strcmp(id, c->txid))
            {
                DeleteFileW(p);
                err = ERROR_SUCCESS;
            }
            LnJsonFree(root);
        }
        free(data);
    }
    if (err == ERROR_SUCCESS)
    {
        WCHAR *a = LnTargetPath(c->t, L"pending\\attempt.json");

        if (a)
            DeleteFileW(a);
        free(a);
    }
    free(p);
    return err;
}

static DWORD DoRollback(CTX *c, const char *why)
{
    UNDO *u = calloc(c->m.itemCount, sizeof(UNDO));
    size_t i;
    DWORD err, first = ERROR_SUCCESS;
    char buf[48];

    if (!u)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = ReadUndo(c, u);
    if (err == LN_E_NOT_FOUND)
    {
        /* nothing was backed up, so nothing was changed */
        err = ERROR_SUCCESS;
        free(u);
        RemovePlanOf(c);
        return err;
    }
    if (err)
    {
        free(u);
        return err;
    }
    Journal(c, "rollback-prepared", "pending", FALSE, FALSE, 0, why);
    for (i = c->m.itemCount; i > 0; i--)
    {
        err = RestoreItem(c, i - 1, u);
        if (err && !first)
            first = err;
    }
    free(u);
    if (first)
    {
        Journal(c, "failed", "error", FALSE, FALSE, 0, ErrText(first, buf));
        return first;
    }
    RemovePlanOf(c);
    Journal(c, "rolled-back", "ok", FALSE, FALSE, 0, why);
    return ERROR_SUCCESS;
}

/* ---- plan.json ---------------------------------------------------------- */

static DWORD WritePlan(const CTX *c, const BOOL *pending, const char *certSha)
{
    LN_BUF b;
    char token[65], seed[160], ts[32], num[24];
    WCHAR *path = LnTargetPath(c->t, L"pending\\plan.json");
    size_t i, k = 0;
    DWORD err = 0;
    LARGE_INTEGER pc;

    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    {
        BYTE *old;
        size_t olen;

        if (!LnReadFileAll(path, &old, &olen))
        {
            LN_JSON *root;
            const char *id = NULL;

            if (!LnJsonParse((const char *)old, olen, &root))
                id = LnJsonGetString(root, "transactionId");
            if (!id || strcmp(id, c->txid))
            {
                LnJsonFree(root);
                free(old);
                free(path);
                return LN_E_BAD_STATE;                  /* another transaction owns the plan */
            }
            LnJsonFree(root);
            free(old);
        }
    }
    QueryPerformanceCounter(&pc);
    LnI64ToStr(pc.QuadPart, num);
    strcpy(seed, c->txid);
    strcat(seed, num);
    LnSha256Hex(seed, strlen(seed), token);
    LnNowUtc(ts);

    LnBufInit(&b);
    err |= LnBufAppendStr(&b, "{\"schemaVersion\":1,\"transactionId\":");
    err |= LnBufAppendJsonString(&b, c->txid);
    err |= LnBufAppendStr(&b, ",\"targetIdentity\":");
    err |= LnBufAppendJsonString(&b, c->t->identity);
    err |= LnBufAppendStr(&b, ",\"packageId\":");
    err |= LnBufAppendJsonString(&b, c->m.packageId);
    err |= LnBufAppendStr(&b, ",\"packageVersion\":");
    err |= LnBufAppendJsonString(&b, c->m.releaseVersion);
    err |= LnBufAppendStr(&b, ",\"manifestSha256\":");
    err |= LnBufAppendJsonString(&b, c->m.manifestSha256);
    err |= LnBufAppendStr(&b, ",\"createdUtc\":");
    err |= LnBufAppendJsonString(&b, ts);
    err |= LnBufAppendStr(&b, ",\"attemptLimit\":2,\"attempt\":0,\"healthToken\":");
    err |= LnBufAppendJsonString(&b, token);
    err |= LnBufAppendStr(&b, ",\"certificateSha256\":");
    err |= LnBufAppendJsonString(&b, certSha);
    err |= LnBufAppendStr(&b, ",\"mode\":\"apply\",\"actions\":[");
    for (i = 0; i < c->m.itemCount; i++)
    {
        const LN_ITEM *it = &c->m.items[i];
        char *tg;
        WCHAR *obj;

        if (!pending[i])
            continue;
        if (k++)
            err |= LnBufAppendStr(&b, ",");
        tg = LnWideToUtf8(it->target);
        err |= LnBufAppendStr(&b, it->isDelete ? "{\"kind\":\"delete\",\"source\":\"\",\"target\":" : "{\"kind\":\"replace\",\"source\":");
        if (!it->isDelete)
        {
            char *src = NULL;

            if (!LnStoreObjectPath(c->t, it->sha256, &obj))
            {
                /* the plan names the object relative to the servicing root */
                WCHAR rel[160], *w = LnUtf8ToWide(it->sha256);

                wcscpy(rel, L"store\\objects\\");
                if (w)
                    wcscat(rel, w);
                wcscat(rel, L"\\payload.bin");
                free(w);
                src = LnWideToUtf8(rel);
                free(obj);
            }
            err |= LnBufAppendJsonString(&b, src ? src : "");
            free(src);
            err |= LnBufAppendStr(&b, ",\"target\":");
        }
        err |= LnBufAppendJsonString(&b, tg ? tg : "");
        free(tg);
        err |= LnBufAppendStr(&b, ",\"oldObjectSha256\":");
        {
            UNDO uu;
            WCHAR *tp = LnTargetFile(c->t, it->target);

            uu.oldSha[0] = 0;
            if (tp)
                CurrentHash(tp, uu.oldSha);
            free(tp);
            err |= LnBufAppendJsonString(&b, uu.oldSha);
        }
        err |= LnBufAppendStr(&b, ",\"newObjectSha256\":");
        err |= LnBufAppendJsonString(&b, it->isDelete ? "" : it->sha256);
        err |= LnBufAppendStr(&b, "}");
    }
    err |= LnBufAppendStr(&b, "]}\n");
    if (err)
        err = ERROR_NOT_ENOUGH_MEMORY;
    else
        err = LnWriteFileAtomic(path, b.data, b.len);
    LnBufFree(&b);
    free(path);
    return err;
}

/* ---- apply -------------------------------------------------------------- */

DWORD LnServicingApplyPackage(const LN_TARGET *t, const char *packageId, const char *version,
                              BOOL *rebootRequired, char txId[65])
{
    CTX c;
    LN_JOURNAL j;
    const LN_JENTRY *last;
    UNDO *u = NULL;
    BOOL *pending = NULL, any = FALSE;
    char pinned[65], buf[48];
    size_t i;
    DWORD err;

    if (rebootRequired)
        *rebootRequired = FALSE;
    if (txId)
        txId[0] = 0;
    err = CtxInit(&c, t, packageId, version, "apply");
    if (err)
        return err;
    if (txId)
        strcpy(txId, c.txid);

    err = LnJournalRead(t, &j);
    if (err)
        goto fail;
    last = LastOf(&j, c.txid);
    if (last)
    {
        if (!strcmp(last->phase, "confirmed") || !strcmp(last->phase, "applied"))
        {
            LnJournalFree(&j);
            LnManifestFree(&c.m);
            return ERROR_SUCCESS;                       /* the same request: nothing to do */
        }
        if (!strcmp(last->phase, "pending-first-boot"))
        {
            if (rebootRequired)
                *rebootRequired = TRUE;
            LnJournalFree(&j);
            LnManifestFree(&c.m);
            return ERROR_SUCCESS;
        }
        if (!strcmp(last->phase, "rollback-prepared"))
        {
            LnJournalFree(&j);
            err = LN_E_BAD_STATE;
            goto fail;
        }
    }
    LnJournalFree(&j);

    /* the store must still hold the objects (immutable, but verify before trusting) */
    for (i = 0; i < c.m.itemCount; i++)
    {
        WCHAR *op;
        char h[65];

        if (c.m.items[i].isDelete)
            continue;
        err = LnStoreObjectPath(t, c.m.items[i].sha256, &op);
        if (err)
            goto fail;
        err = LnSha256FileHex(op, h);
        free(op);
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            err = LN_E_NOT_FOUND;
        else if (!err && strcmp(h, c.m.items[i].sha256))
            err = LN_E_HASH_MISMATCH;
        if (err)
            goto fail;
    }
    err = PinnedCert(t, pinned);
    if (err)
        goto fail;

    u = calloc(c.m.itemCount, sizeof(UNDO));
    pending = calloc(c.m.itemCount, sizeof(BOOL));
    if (!u || !pending)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto fail;
    }

    /* prepared: backups taken and verified, undo.json written, before any change */
    {
        UNDO existing[1];
        BOOL resumed = (last && (!strcmp(last->phase, "prepared") || !strcmp(last->phase, "applying")));

        (void)existing;
        if (resumed)
        {
            err = ReadUndo(&c, u);
            if (err == LN_E_NOT_FOUND)
                resumed = FALSE;                        /* died before undo.json: start over */
            else if (err)
                goto fail;
        }
        if (!resumed)
        {
            err = TakeBackups(&c, u);
            if (err)
            {
                Journal(&c, "failed", "error", FALSE, FALSE, 0, ErrText(err, buf));
                goto fail_logged;
            }
            err = Journal(&c, "prepared", "ok", FALSE, FALSE, 0, NULL);
            if (err)
                goto fail_logged;
        }
    }

    err = Journal(&c, "applying", "pending", FALSE, FALSE, 0, NULL);
    if (err)
        goto fail_logged;
    for (i = 0; i < c.m.itemCount; i++)
    {
        err = ApplyItem(&c, &c.m.items[i], &pending[i]);
        if (err)
        {
            Journal(&c, "failed", "error", FALSE, FALSE, 0, ErrText(err, buf));
            DoRollback(&c, "immediate rollback after a failed operation");
            goto fail_logged;
        }
        if (pending[i])
            any = TRUE;
    }

    if (any)
    {
        err = WritePlan(&c, pending, pinned);
        if (err)
        {
            Journal(&c, "failed", "error", FALSE, FALSE, 0, ErrText(err, buf));
            DoRollback(&c, "plan could not be written");
            goto fail_logged;
        }
        err = Journal(&c, "pending-first-boot", "pending", TRUE, FALSE, 0, NULL);
        if (rebootRequired)
            *rebootRequired = TRUE;
    }
    else
        err = Journal(&c, "applied", "ok", FALSE, FALSE, 0, NULL);
    goto cleanup;

fail:
    if (err && err != LN_E_BAD_STATE)
    {
        /* failed before anything changed: record it, there is nothing to roll back */
        Journal(&c, "failed", "error", FALSE, FALSE, 0, ErrText(err, buf));
    }
fail_logged:
cleanup:
    free(u);
    free(pending);
    LnManifestFree(&c.m);
    return err;
}

/* ---- rollback ----------------------------------------------------------- */

DWORD LnServicingRollback(const LN_TARGET *t, const char *packageId, const char *version, char txId[65])
{
    CTX c;
    LN_JOURNAL j;
    const LN_JENTRY *last;
    DWORD err;

    if (txId)
        txId[0] = 0;
    err = CtxInit(&c, t, packageId, version, "apply");
    if (err)
        return err;
    if (txId)
        strcpy(txId, c.txid);
    err = LnJournalRead(t, &j);
    if (err)
    {
        LnManifestFree(&c.m);
        return err;
    }
    last = LastOf(&j, c.txid);
    if (!last)
        err = LN_E_NOT_FOUND;
    else if (!strcmp(last->phase, "rolled-back") || !strcmp(last->phase, "staged"))
        err = ERROR_SUCCESS;                            /* nothing applied, or already reverted */
    LnJournalFree(&j);
    if (!err && last)
    {
        /* re-read the phase: a rolled-back transaction needs nothing more */
        LN_JOURNAL j2;

        err = LnJournalRead(t, &j2);
        if (!err)
        {
            last = LastOf(&j2, c.txid);
            if (last && !strcmp(last->phase, "rolled-back"))
            {
                LnJournalFree(&j2);
                LnManifestFree(&c.m);
                return ERROR_SUCCESS;
            }
            LnJournalFree(&j2);
        }
    }
    if (!err || err == ERROR_SUCCESS)
        err = DoRollback(&c, "requested rollback");
    LnManifestFree(&c.m);
    return err;
}

/* ---- confirm health ------------------------------------------------------ */

DWORD LnServicingConfirmHealth(const LN_TARGET *t, const char *packageId, const char *version)
{
    CTX c;
    LN_JOURNAL j;
    const LN_JENTRY *last;
    size_t i;
    DWORD err;
    char buf[48];

    err = CtxInit(&c, t, packageId, version, "apply");
    if (err)
        return err;
    err = LnJournalRead(t, &j);
    if (err)
    {
        LnManifestFree(&c.m);
        return err;
    }
    last = LastOf(&j, c.txid);
    if (!last)
        err = LN_E_NOT_FOUND;
    else if (!strcmp(last->phase, "confirmed"))
        err = ERROR_SUCCESS;
    else if (strcmp(last->phase, "applied") && strcmp(last->phase, "pending-first-boot"))
        err = LN_E_BAD_STATE;
    {
        BOOL already = last && !strcmp(last->phase, "confirmed");

        LnJournalFree(&j);
        if (err || already)
        {
            LnManifestFree(&c.m);
            return err;
        }
    }

    /* the proof: every target is exactly what the manifest says */
    for (i = 0; i < c.m.itemCount && !err; i++)
    {
        WCHAR *tp = LnTargetFile(t, c.m.items[i].target);
        char h[65];

        if (!tp)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        err = CurrentHash(tp, h);
        free(tp);
        if (!err && (c.m.items[i].isDelete ? h[0] != 0 : strcmp(h, c.m.items[i].sha256)))
            err = LN_E_VERIFY_FINAL;
    }
    if (err)
    {
        Journal(&c, "failed", "error", FALSE, FALSE, 0, ErrText(err, buf));
        LnManifestFree(&c.m);
        return err;
    }
    err = Journal(&c, "confirmed", "ok", FALSE, TRUE, 0, NULL);
    if (!err)
    {
        LnAttemptSetHealth(t, c.txid);
        RemovePlanOf(&c);
    }
    LnManifestFree(&c.m);
    return err;
}

/* ---- remove-package ------------------------------------------------------ */

DWORD LnServicingRemovePackage(const LN_TARGET *t, const char *packageId, const char *version)
{
    CTX c, rm;
    LN_JOURNAL j;
    const LN_JENTRY *last;
    DWORD err = CtxInit(&c, t, packageId, version, "apply");

    if (err)
        return err;
    err = LnJournalRead(t, &j);
    if (err)
    {
        LnManifestFree(&c.m);
        return err;
    }
    last = LastOf(&j, c.txid);
    if (last && strcmp(last->phase, "rolled-back") && strcmp(last->phase, "staged") && strcmp(last->phase, "failed"))
    {
        /* installed (or half installed): uninstall = the inverse plan from the backup */
        LnJournalFree(&j);
        err = LnServicingRollback(t, packageId, version, NULL);
        if (err)
        {
            LnManifestFree(&c.m);
            return err;
        }
    }
    else
        LnJournalFree(&j);

    rm = c;
    rm.op = "remove-package";
    LnTransactionId(t->identity, c.m.packageId, c.m.releaseVersion, c.m.manifestSha256, rm.op, rm.txid);
    err = LnStoreRemovePackage(t, packageId, version);
    if (!err)
        err = Journal(&rm, "applied", "ok", FALSE, FALSE, 0, NULL);
    LnManifestFree(&c.m);
    return err;
}

/* ---- export-log ---------------------------------------------------------- */

static void CopyIfExists(const LN_TARGET *t, const WCHAR *rel, const WCHAR *destDir, const WCHAR *name)
{
    WCHAR *src = LnTargetPath(t, rel), *dst = LnJoin(destDir, name);

    if (src && dst && LnPathExists(src))
        LnCopyFileFlush(src, dst);
    free(src);
    free(dst);
}

DWORD LnServicingExportLog(const LN_TARGET *t, const WCHAR *destDir)
{
    DWORD err = LnEnsureDir(destDir);
    WCHAR *logs, *pat;
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (err)
        return err;
    CopyIfExists(t, L"journal\\transactions.jsonl", destDir, L"transactions.jsonl");
    CopyIfExists(t, L"journal\\state.json", destDir, L"state.json");
    CopyIfExists(t, L"pending\\plan.json", destDir, L"plan.json");
    CopyIfExists(t, L"pending\\attempt.json", destDir, L"attempt.json");
    CopyIfExists(t, L"target-id", destDir, L"target-id.txt");
    logs = LnTargetPath(t, L"logs");
    pat = logs ? LnJoin(logs, L"*.log") : NULL;
    h = pat ? FindFirstFileW(pat, &fd) : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            WCHAR *src = LnJoin(logs, fd.cFileName), *dst = LnJoin(destDir, fd.cFileName);

            if (src && dst)
                LnCopyFileFlush(src, dst);
            free(src);
            free(dst);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(pat);
    free(logs);
    return ERROR_SUCCESS;
}
