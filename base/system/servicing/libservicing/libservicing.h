/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Internal API of the stage 1 servicing core (contract: S1-CORE.md)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Every function returns a Win32 error code (ERROR_SUCCESS on success) or one
 * of the LN_E_* codes below. Nothing here is a public ABI.
 */

#ifndef LIBSERVICING_H
#define LIBSERVICING_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- error codes (customer bit set, so they never collide with Win32) --- */
#define LN_E_BASE               0xE0000000UL
#define LN_E_MANIFEST_INVALID   (LN_E_BASE + 1)   /* syntax, schema or path rule violated */
#define LN_E_HASH_MISMATCH      (LN_E_BASE + 2)   /* payload or object differs from its declared hash */
#define LN_E_UNSIGNED           (LN_E_BASE + 3)   /* no catalog or no signature */
#define LN_E_CATALOG_INVALID    (LN_E_BASE + 4)   /* signature invalid, or members do not cover the package */
#define LN_E_CERT_MISMATCH      (LN_E_BASE + 5)   /* signer is not the pinned development certificate */
#define LN_E_JOURNAL_CORRUPT    (LN_E_BASE + 6)   /* hash chain broken before the last entry */
#define LN_E_PRECONDITION       (LN_E_BASE + 7)   /* target state does not match what the package expects */
#define LN_E_NOT_FOUND          (LN_E_BASE + 8)
#define LN_E_FORBIDDEN_TARGET   (LN_E_BASE + 9)   /* kernel / boot path / outside the allowed roots */
#define LN_E_BAD_STATE          (LN_E_BASE + 10)  /* transaction is not in a state that allows the request */
#define LN_E_VERIFY_FINAL       (LN_E_BASE + 11)  /* final hash check after an operation failed */
#define LN_E_NO_TRUST_ANCHOR    (LN_E_BASE + 12)  /* target has no trusted certificate installed */

#define LN_SHA256_HEX_LEN   64
#define LN_MAX_ITEMS        4096

/* ---- sha256.c ---------------------------------------------------------- */
typedef struct _LN_SHA256_CTX
{
    unsigned long state[8];
    unsigned long long bitlen;
    unsigned char data[64];
    unsigned long datalen;
} LN_SHA256_CTX;

void LnSha256Init(LN_SHA256_CTX *ctx);
void LnSha256Update(LN_SHA256_CTX *ctx, const void *data, size_t len);
void LnSha256Final(LN_SHA256_CTX *ctx, unsigned char out[32]);
void LnSha256(const void *data, size_t len, unsigned char out[32]);
void LnHexEncode(const unsigned char *in, size_t len, char *out);          /* out: 2*len+1 bytes */
void LnSha256Hex(const void *data, size_t len, char out[65]);
DWORD LnSha256FileHex(const WCHAR *path, char out[65]);
BOOL LnIsSha256Hex(const char *s);                                          /* 64 lowercase hex */

/* ---- util.c ------------------------------------------------------------ */
typedef struct _LN_BUF
{
    char *data;
    size_t len;
    size_t cap;
} LN_BUF;

void LnBufInit(LN_BUF *b);
void LnBufFree(LN_BUF *b);
DWORD LnBufAppend(LN_BUF *b, const char *s, size_t n);
DWORD LnBufAppendStr(LN_BUF *b, const char *s);
DWORD LnBufAppendJsonString(LN_BUF *b, const char *utf8);                   /* with quotes */
DWORD LnBufAppendJsonWide(LN_BUF *b, const WCHAR *w);                       /* with quotes */

char *LnWideToUtf8(const WCHAR *w);                                         /* free() */
WCHAR *LnUtf8ToWide(const char *s);                                         /* free() */
DWORD LnReadFileAll(const WCHAR *path, BYTE **data, size_t *len);           /* free(*data) */
DWORD LnWriteFileAtomic(const WCHAR *path, const void *data, size_t len);   /* tmp + flush + replace */
DWORD LnEnsureDir(const WCHAR *path);                                       /* recursive */
DWORD LnCopyFileFlush(const WCHAR *src, const WCHAR *dst);                  /* copy + flush, parent created */
BOOL LnPathExists(const WCHAR *path);
BOOL LnIsSafeRelativePath(const WCHAR *path);                               /* no drive, no root, no .. */
void LnNowUtc(char out[32]);
void LnI64ToStr(long long v, char out[24]);
WCHAR *LnJoin(const WCHAR *a, const WCHAR *b);                              /* a\b, free() */

/* ---- json.c ------------------------------------------------------------ */
typedef enum _LN_JSON_TYPE
{
    LnJsonNull, LnJsonBool, LnJsonNumber, LnJsonString, LnJsonArray, LnJsonObject
} LN_JSON_TYPE;

typedef struct _LN_JSON
{
    LN_JSON_TYPE type;
    char *str;                  /* string value (UTF-8) */
    char *key;                  /* member name when the parent is an object */
    long long num;
    int boolean;
    struct _LN_JSON *child;     /* first child (array element or object member) */
    struct _LN_JSON *next;      /* next sibling */
} LN_JSON;

DWORD LnJsonParse(const char *text, size_t len, LN_JSON **root);
void LnJsonFree(LN_JSON *n);
const LN_JSON *LnJsonGet(const LN_JSON *obj, const char *key);              /* NULL if absent */
const char *LnJsonGetString(const LN_JSON *obj, const char *key);          /* NULL unless a string */
BOOL LnJsonGetInt(const LN_JSON *obj, const char *key, long long *out);
BOOL LnJsonGetBool(const LN_JSON *obj, const char *key, BOOL *out);

/* ---- target.c ---------------------------------------------------------- */
typedef enum _LN_TARGET_MODE { LnTargetOnline = 1, LnTargetImage = 2 } LN_TARGET_MODE;

typedef struct _LN_TARGET
{
    LN_TARGET_MODE mode;
    WCHAR root[MAX_PATH];         /* ends with a backslash */
    WCHAR servicing[MAX_PATH];    /* <root>LiberNT\Servicing */
    char identity[65];            /* stable per target (see LnTargetOpen) */
} LN_TARGET;

/* Online: root = the drive of the running system. Image: an unmounted or offline root. */
DWORD LnTargetOpen(LN_TARGET *t, LN_TARGET_MODE mode, const WCHAR *imageRoot, BOOL create);
WCHAR *LnTargetPath(const LN_TARGET *t, const WCHAR *relInServicing);       /* free() */
WCHAR *LnTargetFile(const LN_TARGET *t, const WCHAR *relInRoot);            /* free() */
/* Rule of stage 1: user components and drivers only; never the kernel or the boot path. */
BOOL LnTargetPathAllowed(const WCHAR *relInRoot);

/* ---- manifest.c -------------------------------------------------------- */
typedef struct _LN_ITEM
{
    WCHAR *path;                  /* inside payload\  (NULL for delete) */
    WCHAR *target;                /* inside the target root */
    char sha256[65];              /* of the new content ("" for delete) */
    BOOL isDelete;
} LN_ITEM;

typedef struct _LN_MANIFEST
{
    char packageId[65];
    char releaseVersion[65];
    char type[24];                /* system-component | driver | tool */
    char architecture[16];
    char certificateSha256[65];
    LN_ITEM *items;
    size_t itemCount;
    char manifestSha256[65];      /* of manifest.json bytes */
} LN_MANIFEST;

DWORD LnManifestParse(const char *text, size_t len, LN_MANIFEST *m);
DWORD LnManifestLoad(const WCHAR *path, LN_MANIFEST *m);
void LnManifestFree(LN_MANIFEST *m);

/* ---- verify.c ---------------------------------------------------------- */
/* pinnedCertSha256: hash of the trusted development certificate (from the target's trust\ dir). */
DWORD LnServicingVerifyPackage(const WCHAR *pkgDir, const char *pinnedCertSha256, LN_MANIFEST *out);
DWORD LnCertFileSha256(const WCHAR *cerPath, char out[65]);

/* ---- journal.c --------------------------------------------------------- */
typedef struct _LN_JENTRY
{
    long long sequence;
    char transactionId[65];
    char targetIdentity[65];
    char packageId[65];
    char packageVersion[65];
    char manifestSha256[65];
    char operation[24];           /* add-package | apply | rollback | remove-package */
    char phase[24];
    char result[12];              /* ok | error | pending */
    BOOL rebootRequired;
    BOOL healthConfirmed;
    long long attempt;
    char previousEntrySha256[65];
    char timestampUtc[32];
    char error[160];
} LN_JENTRY;

typedef struct _LN_JOURNAL
{
    LN_JENTRY *entries;
    size_t count;
    BOOL tailTruncated;           /* last line was partial and was ignored */
} LN_JOURNAL;

void LnTransactionId(const char *targetIdentity, const char *packageId, const char *version,
                     const char *manifestSha256, const char *operation, char out[65]);
DWORD LnJournalAppend(const LN_TARGET *t, LN_JENTRY *e);                    /* fills sequence/prev/time */
DWORD LnJournalRead(const LN_TARGET *t, LN_JOURNAL *j);
void LnJournalFree(LN_JOURNAL *j);
const LN_JENTRY *LnJournalLast(const LN_JOURNAL *j, const char *transactionId);
DWORD LnJournalWriteState(const LN_TARGET *t);                              /* derived view */

/* ---- store.c ----------------------------------------------------------- */
DWORD LnStoreObjectPath(const LN_TARGET *t, const char *sha256, WCHAR **path);
DWORD LnStoreImport(const LN_TARGET *t, const WCHAR *pkgDir, const LN_MANIFEST *m);   /* idempotent */
DWORD LnStoreLoadManifest(const LN_TARGET *t, const char *packageId, const char *version, LN_MANIFEST *m);
DWORD LnStoreRemovePackage(const LN_TARGET *t, const char *packageId, const char *version);
DWORD LnStoreList(const LN_TARGET *t, char (**ids)[130], size_t *count);   /* "id\\version", free(*ids) */

/* ---- servicing.c: add / apply / rollback ------------------------------- */
DWORD LnServicingAddPackage(const LN_TARGET *t, const WCHAR *pkgDir, char txId[65]);
/* Applies a package that is already in the store. Idempotent: the same request resumes or returns. */
DWORD LnServicingApplyPackage(const LN_TARGET *t, const char *packageId, const char *version,
                              BOOL *rebootRequired, char txId[65]);
DWORD LnServicingRollback(const LN_TARGET *t, const char *packageId, const char *version, char txId[65]);
DWORD LnServicingRemovePackage(const LN_TARGET *t, const char *packageId, const char *version);
/* Re-checks every final hash and, only then, writes confirmed / healthConfirmed. */
DWORD LnServicingConfirmHealth(const LN_TARGET *t, const char *packageId, const char *version);
DWORD LnServicingExportLog(const LN_TARGET *t, const WCHAR *destDir);

/* Test hook: treat a locked target of an Image as in use (pending), as an Online target would. */
extern BOOL LnTestLockedAsPending;

/* ---- pending.c --------------------------------------------------------- */
typedef struct _LN_ATTEMPT
{
    char transactionId[65];
    long long bootAttemptCount;
    long long unexpectedRestartCount;
    long long verifiedFailureCount;
    char lastStartUtc[32];
    char lastOutcome[24];         /* started | completed | failed | unexpected-restart */
    BOOL healthConfirmed;
} LN_ATTEMPT;

DWORD LnServicingReadPendingPlan(const LN_TARGET *t, char **planJson);      /* free() */
DWORD LnServicingReadAttempt(const LN_TARGET *t, LN_ATTEMPT *a);
DWORD LnServicingMarkBootStarted(const LN_TARGET *t, LN_ATTEMPT *a);
DWORD LnServicingMarkBootOutcome(const LN_TARGET *t, BOOL verifiedFailure, BOOL completed);
/* internal: written only after the final hashes were re-checked */
DWORD LnAttemptSetHealth(const LN_TARGET *t, const char *transactionId);
/* The rule of the stage: 2 attempts without health -> roll back; says why. */
BOOL LnServicingShouldRollback(const LN_ATTEMPT *a, const char **reason);

#ifdef __cplusplus
}
#endif

#endif /* LIBSERVICING_H */
