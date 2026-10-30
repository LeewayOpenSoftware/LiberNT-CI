/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Append-only JSONL journal with a hash chain, and the derived state.json
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * One entry per line. Each append opens with FILE_APPEND_DATA, writes the whole line,
 * calls FlushFileBuffers and closes (S1-CORE.md). previousEntrySha256 is the SHA-256 of
 * the previous valid line, so a removed or altered entry breaks the chain.
 *
 * A write torn by a power cut leaves a partial last line. The reader ignores an
 * unparseable line (tailTruncated is set), and the next append first terminates it with a
 * newline. A line that parses but does not continue the chain is corruption.
 */

#include "libservicing.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define ZERO_HASH "0000000000000000000000000000000000000000000000000000000000000000"

void LnTransactionId(const char *targetIdentity, const char *packageId, const char *version,
                     const char *manifestSha256, const char *operation, char out[65])
{
    LN_BUF b;

    LnBufInit(&b);
    LnBufAppend(&b, targetIdentity, strlen(targetIdentity) + 1);       /* include the NUL separator */
    LnBufAppend(&b, packageId, strlen(packageId) + 1);
    LnBufAppend(&b, version, strlen(version) + 1);
    LnBufAppend(&b, manifestSha256, strlen(manifestSha256) + 1);
    LnBufAppend(&b, operation, strlen(operation));                      /* last field: no trailing NUL */
    LnSha256Hex(b.data, b.len, out);
    LnBufFree(&b);
}

static DWORD Serialize(const LN_JENTRY *e, LN_BUF *b)
{
    char num[32];
    DWORD err = 0;

#define STR(name, val) do { err |= LnBufAppendStr(b, "\"" name "\":"); err |= LnBufAppendJsonString(b, val); err |= LnBufAppendStr(b, ","); } while (0)
#define INT(name, val) do { LnI64ToStr((long long)(val), num); err |= LnBufAppendStr(b, "\"" name "\":"); err |= LnBufAppendStr(b, num); err |= LnBufAppendStr(b, ","); } while (0)
#define BOOLV(name, val) do { err |= LnBufAppendStr(b, "\"" name "\":"); err |= LnBufAppendStr(b, (val) ? "true," : "false,"); } while (0)

    err |= LnBufAppendStr(b, "{\"schemaVersion\":1,");
    INT("sequence", e->sequence);
    STR("transactionId", e->transactionId);
    STR("targetIdentity", e->targetIdentity);
    STR("packageId", e->packageId);
    STR("packageVersion", e->packageVersion);
    STR("manifestSha256", e->manifestSha256);
    STR("operation", e->operation);
    STR("phase", e->phase);
    STR("result", e->result);
    BOOLV("rebootRequired", e->rebootRequired);
    BOOLV("healthConfirmed", e->healthConfirmed);
    INT("attempt", e->attempt);
    STR("previousEntrySha256", e->previousEntrySha256);
    STR("timestampUtc", e->timestampUtc);
    err |= LnBufAppendStr(b, "\"error\":");
    err |= LnBufAppendJsonString(b, e->error);
    err |= LnBufAppendStr(b, "}");
#undef STR
#undef INT
#undef BOOLV
    return err ? ERROR_NOT_ENOUGH_MEMORY : ERROR_SUCCESS;
}

static void CopyField(char *dst, size_t cap, const char *src)
{
    if (!src)
        src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static BOOL ParseEntry(const char *line, size_t len, LN_JENTRY *e)
{
    LN_JSON *root = NULL;
    long long v;
    BOOL b;
    const char *s;

    memset(e, 0, sizeof(*e));
    if (LnJsonParse(line, len, &root) || !root || root->type != LnJsonObject)
    {
        LnJsonFree(root);
        return FALSE;
    }
    if (!LnJsonGetInt(root, "schemaVersion", &v) || v != 1 || !LnJsonGetInt(root, "sequence", &e->sequence) ||
        !LnJsonGetString(root, "transactionId") || !LnJsonGetString(root, "phase") ||
        !LnJsonGetString(root, "previousEntrySha256"))
    {
        LnJsonFree(root);
        return FALSE;
    }
    CopyField(e->transactionId, sizeof(e->transactionId), LnJsonGetString(root, "transactionId"));
    CopyField(e->targetIdentity, sizeof(e->targetIdentity), LnJsonGetString(root, "targetIdentity"));
    CopyField(e->packageId, sizeof(e->packageId), LnJsonGetString(root, "packageId"));
    CopyField(e->packageVersion, sizeof(e->packageVersion), LnJsonGetString(root, "packageVersion"));
    CopyField(e->manifestSha256, sizeof(e->manifestSha256), LnJsonGetString(root, "manifestSha256"));
    CopyField(e->operation, sizeof(e->operation), LnJsonGetString(root, "operation"));
    CopyField(e->phase, sizeof(e->phase), LnJsonGetString(root, "phase"));
    CopyField(e->result, sizeof(e->result), LnJsonGetString(root, "result"));
    CopyField(e->previousEntrySha256, sizeof(e->previousEntrySha256), LnJsonGetString(root, "previousEntrySha256"));
    CopyField(e->timestampUtc, sizeof(e->timestampUtc), LnJsonGetString(root, "timestampUtc"));
    s = LnJsonGetString(root, "error");
    CopyField(e->error, sizeof(e->error), s);
    if (LnJsonGetBool(root, "rebootRequired", &b)) e->rebootRequired = b;
    if (LnJsonGetBool(root, "healthConfirmed", &b)) e->healthConfirmed = b;
    LnJsonGetInt(root, "attempt", &e->attempt);
    LnJsonFree(root);
    return TRUE;
}

static WCHAR *JournalPath(const LN_TARGET *t)
{
    return LnTargetPath(t, L"journal\\transactions.jsonl");
}

void LnJournalFree(LN_JOURNAL *j)
{
    free(j->entries);
    memset(j, 0, sizeof(*j));
}

/* Reads and validates; lastHash/lastSeq describe the last valid entry; needsNewline tells
 * whether the file ends in a partial line. */
static DWORD ReadInternal(const LN_TARGET *t, LN_JOURNAL *j, char lastHash[65], long long *lastSeq, BOOL *needsNewline)
{
    WCHAR *path = JournalPath(t);
    BYTE *data = NULL;
    size_t len = 0, pos = 0, cap = 0;
    DWORD err;

    memset(j, 0, sizeof(*j));
    strcpy(lastHash, ZERO_HASH);
    *lastSeq = 0;
    *needsNewline = FALSE;
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnReadFileAll(path, &data, &len);
    free(path);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return ERROR_SUCCESS;                           /* empty journal */
    if (err)
        return err;

    if (len && data[len - 1] != '\n')
        *needsNewline = TRUE;

    while (pos < len)
    {
        size_t end = pos;
        LN_JENTRY e;
        char h[65];

        while (end < len && data[end] != '\n')
            end++;
        if (end > pos)
        {
            if (ParseEntry((const char *)data + pos, end - pos, &e))
            {
                if (strcmp(e.previousEntrySha256, lastHash) || e.sequence != *lastSeq + 1)
                {
                    free(data);
                    LnJournalFree(j);
                    return LN_E_JOURNAL_CORRUPT;
                }
                if (j->count == cap)
                {
                    size_t ncap = cap ? cap * 2 : 64;
                    LN_JENTRY *p = realloc(j->entries, ncap * sizeof(*p));

                    if (!p)
                    {
                        free(data);
                        LnJournalFree(j);
                        return ERROR_NOT_ENOUGH_MEMORY;
                    }
                    j->entries = p;
                    cap = ncap;
                }
                j->entries[j->count++] = e;
                LnSha256Hex(data + pos, end - pos, h);
                strcpy(lastHash, h);
                *lastSeq = e.sequence;
            }
            else
                j->tailTruncated = TRUE;                /* a torn line: ignored, the chain skips it */
        }
        pos = end + 1;
    }
    free(data);
    return ERROR_SUCCESS;
}

DWORD LnJournalRead(const LN_TARGET *t, LN_JOURNAL *j)
{
    char h[65];
    long long seq;
    BOOL nl;

    return ReadInternal(t, j, h, &seq, &nl);
}

DWORD LnJournalAppend(const LN_TARGET *t, LN_JENTRY *e)
{
    LN_JOURNAL j;
    char lastHash[65];
    long long lastSeq;
    BOOL needsNewline;
    LN_BUF line;
    WCHAR *path;
    HANDLE h;
    DWORD err, wrote;

    err = ReadInternal(t, &j, lastHash, &lastSeq, &needsNewline);
    LnJournalFree(&j);
    if (err)
        return err;

    e->sequence = lastSeq + 1;
    strcpy(e->previousEntrySha256, lastHash);
    LnNowUtc(e->timestampUtc);
    if (!e->targetIdentity[0])
        strcpy(e->targetIdentity, t->identity);

    LnBufInit(&line);
    if (needsNewline)
        LnBufAppendStr(&line, "\n");
    err = Serialize(e, &line);
    if (!err)
        err = LnBufAppendStr(&line, "\n");
    if (err)
    {
        LnBufFree(&line);
        return err;
    }

    path = JournalPath(t);
    if (!path)
    {
        LnBufFree(&line);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    {
        WCHAR dir[MAX_PATH], *sl;

        wcscpy(dir, path);
        sl = wcsrchr(dir, L'\\');
        if (sl)
            *sl = 0;
        err = LnEnsureDir(dir);
    }
    if (!err)
    {
        h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h == INVALID_HANDLE_VALUE)
            err = GetLastError();
        else
        {
            if (!WriteFile(h, line.data, (DWORD)line.len, &wrote, NULL) || wrote != (DWORD)line.len)
                err = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
            else if (!FlushFileBuffers(h))
                err = GetLastError();
            CloseHandle(h);
        }
    }
    free(path);
    LnBufFree(&line);
    return err;
}

const LN_JENTRY *LnJournalLast(const LN_JOURNAL *j, const char *transactionId)
{
    size_t i;

    for (i = j->count; i > 0; i--)
    {
        if (!strcmp(j->entries[i - 1].transactionId, transactionId))
            return &j->entries[i - 1];
    }
    return NULL;
}

DWORD LnJournalWriteState(const LN_TARGET *t)
{
    LN_JOURNAL j;
    LN_BUF b;
    WCHAR *path;
    size_t i, k;
    DWORD err = LnJournalRead(t, &j);

    if (err)
        return err;
    LnBufInit(&b);
    LnBufAppendStr(&b, "{\"schemaVersion\":1,\"derivedFromJournal\":true,\"entries\":");
    {
        char num[32];

        LnI64ToStr((long long)j.count, num);
        LnBufAppendStr(&b, num);
    }
    LnBufAppendStr(&b, ",\"transactions\":[");
    for (i = 0, k = 0; i < j.count; i++)
    {
        const LN_JENTRY *e = &j.entries[i];
        size_t later;
        BOOL superseded = FALSE;

        for (later = i + 1; later < j.count; later++)
        {
            if (!strcmp(j.entries[later].transactionId, e->transactionId))
                superseded = TRUE;
        }
        if (superseded)
            continue;                                   /* only the latest phase of each transaction */
        if (k++)
            LnBufAppendStr(&b, ",");
        LnBufAppendStr(&b, "{\"transactionId\":");
        LnBufAppendJsonString(&b, e->transactionId);
        LnBufAppendStr(&b, ",\"packageId\":");
        LnBufAppendJsonString(&b, e->packageId);
        LnBufAppendStr(&b, ",\"packageVersion\":");
        LnBufAppendJsonString(&b, e->packageVersion);
        LnBufAppendStr(&b, ",\"operation\":");
        LnBufAppendJsonString(&b, e->operation);
        LnBufAppendStr(&b, ",\"phase\":");
        LnBufAppendJsonString(&b, e->phase);
        LnBufAppendStr(&b, ",\"result\":");
        LnBufAppendJsonString(&b, e->result);
        LnBufAppendStr(&b, e->rebootRequired ? ",\"rebootRequired\":true" : ",\"rebootRequired\":false");
        LnBufAppendStr(&b, e->healthConfirmed ? ",\"healthConfirmed\":true}" : ",\"healthConfirmed\":false}");
    }
    LnBufAppendStr(&b, "]}\n");
    LnJournalFree(&j);

    path = LnTargetPath(t, L"journal\\state.json");
    err = path ? LnWriteFileAtomic(path, b.data, b.len) : ERROR_NOT_ENOUGH_MEMORY;
    free(path);
    LnBufFree(&b);
    return err;
}
