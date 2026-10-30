/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Unit tests of the stage 1 core. They run on a Windows host against a temporary
 *              Image target; nothing touches the real system or the servicing tree of the host.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * usage: libservicing-tests <fixtures-dir> <dev-certificate.cer>
 * Fixtures are built by tools/libernt-update/Build-And-Test.ps1 (they need the signing tools).
 */

#include "../libservicing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int g_checks, g_failed;
static const WCHAR *g_fx;
static const WCHAR *g_cer;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_ERR(expected, expr) do { DWORD _e = (expr); g_checks++; if (_e != (DWORD)(expected)) { g_failed++; \
    printf("  FAIL %s:%d: %s returned 0x%08lx, expected 0x%08lx\n", __FILE__, __LINE__, #expr, (unsigned long)_e, (unsigned long)(expected)); } } while (0)

static WCHAR g_work[MAX_PATH];

static void Section(const char *name)
{
    printf("[%s]\n", name);
}

static void WriteText(const WCHAR *path, const char *text)
{
    LnWriteFileAtomic(path, text, strlen(text));
}

static BOOL FileIs(const WCHAR *path, const char *text)
{
    BYTE *d;
    size_t n;
    BOOL ok;

    if (LnReadFileAll(path, &d, &n))
        return FALSE;
    ok = n == strlen(text) && !memcmp(d, text, n);
    free(d);
    return ok;
}

static WCHAR *Fx(const WCHAR *name)
{
    return LnJoin(g_fx, name);
}

static void NewImage(WCHAR *root, const WCHAR *name)
{
    WCHAR sys[MAX_PATH];

    wcscpy(root, g_work);
    wcscat(root, L"\\");
    wcscat(root, name);
    LnEnsureDir(root);
    wcscpy(sys, root);
    wcscat(sys, L"\\Windows\\System32");
    LnEnsureDir(sys);
}

static void InstallTrust(const LN_TARGET *t)
{
    WCHAR *dst = LnTargetPath(t, L"trust\\Leeway-LiberNT-Update-DEV.cer");

    LnCopyFileFlush(g_cer, dst);
    free(dst);
}

static BOOL OpenImage(LN_TARGET *t, const WCHAR *name)
{
    WCHAR root[MAX_PATH];

    NewImage(root, name);
    return LnTargetOpen(t, LnTargetImage, root, TRUE) == ERROR_SUCCESS;
}

/* ---- tests -------------------------------------------------------------- */

static void TestSha256(void)
{
    char h[65];
    char big[1000];

    Section("sha256");
    LnSha256Hex("", 0, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    LnSha256Hex("abc", 3, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    LnSha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
    CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    memset(big, 'a', sizeof(big));
    LnSha256Hex(big, sizeof(big), h);
    CHECK(strlen(h) == 64 && LnIsSha256Hex(h));
    CHECK(!LnIsSha256Hex("ABCDEF") && !LnIsSha256Hex("zz") && !LnIsSha256Hex(NULL));
}

static DWORD JP(const char *t, LN_JSON **r)
{
    return LnJsonParse(t, strlen(t), r);
}

static void TestJson(void)
{
    LN_JSON *r;
    long long v;
    BOOL b;
    char deep[200];

    Section("json");
    CHECK_ERR(0, JP("{\"a\":1,\"b\":[true,null,\"x\\n\\u00e9\"],\"c\":{\"d\":\"e\\\\f\"}}", &r));
    CHECK(LnJsonGetInt(r, "a", &v) && v == 1);
    CHECK(LnJsonGet(r, "b")->type == LnJsonArray);
    CHECK(!strcmp(LnJsonGetString(LnJsonGet(r, "c"), "d"), "e\\f"));
    CHECK(!LnJsonGetBool(r, "a", &b));
    LnJsonFree(r);
    CHECK(JP("{\"a\":1,}", &r) != 0);
    CHECK(JP("{\"a\":", &r) != 0);
    CHECK(JP("[1] x", &r) != 0);
    CHECK(JP("{\"a\":\"\\u0000\"}", &r) != 0);          /* NUL would truncate a path */
    CHECK(JP("{\"a\":1.5}", &r) != 0);
    memset(deep, '[', 100);
    CHECK(LnJsonParse(deep, 100, &r) != 0);                          /* depth bomb */
}

static void TestPaths(void)
{
    Section("paths");
    CHECK(LnIsSafeRelativePath(L"Windows\\System32\\a.dll"));
    CHECK(!LnIsSafeRelativePath(L"..\\x") && !LnIsSafeRelativePath(L"a\\..\\..\\x"));
    CHECK(!LnIsSafeRelativePath(L"\\Windows\\a") && !LnIsSafeRelativePath(L"C:\\a") && !LnIsSafeRelativePath(L"a/b"));
    CHECK(!LnIsSafeRelativePath(L"") && !LnIsSafeRelativePath(L"a\\\\b") && !LnIsSafeRelativePath(L"a\\b."));
    CHECK(!LnIsSafeRelativePath(L"a:stream") && !LnIsSafeRelativePath(L"a\\b*"));
    CHECK(LnTargetPathAllowed(L"Windows\\System32\\drivers\\lnfix.sys"));
    CHECK(LnTargetPathAllowed(L"Program Files\\App\\app.exe"));
    CHECK(!LnTargetPathAllowed(L"Windows\\System32\\ntoskrnl.exe"));
    CHECK(!LnTargetPathAllowed(L"windows\\system32\\NTDLL.DLL"));
    CHECK(!LnTargetPathAllowed(L"Windows\\System32\\hal.dll") && !LnTargetPathAllowed(L"Windows\\System32\\halmacpi.dll"));
    CHECK(!LnTargetPathAllowed(L"Windows\\System32\\config\\SYSTEM"));
    CHECK(!LnTargetPathAllowed(L"Windows\\System32\\smss.exe"));
    CHECK(!LnTargetPathAllowed(L"LiberNT\\Servicing\\journal\\transactions.jsonl"));
    CHECK(!LnTargetPathAllowed(L"Users\\x\\a.dll"));
}

static void TestManifest(void)
{
    LN_MANIFEST m;
    static const char head[] = "{\"schemaVersion\":1,\"packageId\":\"p.id\",\"releaseVersion\":\"1.0\",\"type\":\"driver\","
        "\"targetFamily\":\"LiberNT\",\"architecture\":\"amd64\","
        "\"certificateSha256\":\"829459d8db6adc0a4fe7500e0572149e1d469b6a3707cf48e374b7660d754a34\",\"payloads\":[";
    static const char sha[] = "197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16";
    char text[1400];

#define MK(items) do { strcpy(text, head); strcat(text, items); strcat(text, "]}"); } while (0)
    Section("manifest");
    MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\a.dll\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
    CHECK_ERR(0, LnManifestParse(text, strlen(text), &m));
    CHECK(m.itemCount == 1 && !strcmp(m.packageId, "p.id") && !strcmp(m.items[0].sha256, sha));
    CHECK(strlen(m.manifestSha256) == 64);
    LnManifestFree(&m);

    MK("{\"action\":\"delete\",\"target\":\"Windows\\\\System32\\\\old.dll\"}");
    CHECK_ERR(0, LnManifestParse(text, strlen(text), &m));
    CHECK(m.items[0].isDelete);
    LnManifestFree(&m);

    MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\ntoskrnl.exe\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
    CHECK_ERR(LN_E_FORBIDDEN_TARGET, LnManifestParse(text, strlen(text), &m));
    MK("{\"path\":\"..\\\\a.bin\",\"target\":\"Windows\\\\System32\\\\a.dll\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
    CHECK_ERR(LN_E_MANIFEST_INVALID, LnManifestParse(text, strlen(text), &m));
    MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\..\\\\..\\\\x.dll\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
    CHECK_ERR(LN_E_FORBIDDEN_TARGET, LnManifestParse(text, strlen(text), &m));
    MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\a.dll\",\"sha256\":\"NOTAHASH\"}");
    CHECK_ERR(LN_E_MANIFEST_INVALID, LnManifestParse(text, strlen(text), &m));
    MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\a.dll\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"},"
       "{\"path\":\"b.bin\",\"target\":\"windows\\\\system32\\\\A.DLL\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
    CHECK_ERR(LN_E_MANIFEST_INVALID, LnManifestParse(text, strlen(text), &m));   /* same target twice */
    CHECK_ERR(LN_E_MANIFEST_INVALID, LnManifestParse("{\"schemaVersion\":2}", strlen("{\"schemaVersion\":2}"), &m));
    CHECK_ERR(LN_E_MANIFEST_INVALID, LnManifestParse("not json", 8, &m));
#undef MK
}

static void TestTransactionId(void)
{
    char a[65], b[65], c[65], d[65];

    Section("transaction id (S1-CONTRACTS.md section 3)");
    LnTransactionId("t", "p", "1", "m", "install", a);
    LnTransactionId("t", "p", "1", "m", "install", b);
    LnTransactionId("t", "p", "1", "m", "rollback", c);
    LnTransactionId("tp", "", "1", "m", "install", d);      /* the separator keeps fields apart */
    CHECK(!strcmp(a, b) && strcmp(a, c) && strcmp(a, d));
    {
        char expect[65];
        static const char raw[] = "t\np\n1\nm\ninstall";   /* machineId \n packageId \n version \n digest \n operation */

        LnSha256Hex(raw, sizeof(raw) - 1, expect);
        CHECK(!strcmp(a, expect));
    }
}

static void TestJournal(void)
{
    LN_TARGET t;
    LN_JENTRY e;
    LN_JOURNAL j;
    WCHAR *jp;
    BYTE *data;
    size_t len;
    int i;

    Section("journal");
    CHECK(OpenImage(&t, L"journal"));
    for (i = 0; i < 3; i++)
    {
        memset(&e, 0, sizeof(e));
        strcpy(e.transactionId, "aa");
        strcpy(e.phase, i == 0 ? "prepared" : i == 1 ? "applying" : "applied");
        strcpy(e.result, "ok");
        strcpy(e.error, "quote\" backslash\\ tab\t");
        CHECK_ERR(0, LnJournalAppend(&t, &e));
    }
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(j.count == 3 && j.entries[2].sequence == 3 && !strcmp(j.entries[2].phase, "applied"));
    CHECK(!strcmp(j.entries[0].error, "quote\" backslash\\ tab\t"));
    CHECK(strcmp(j.entries[1].previousEntrySha256, j.entries[0].previousEntrySha256) != 0);
    CHECK(!strcmp(LnJournalLast(&j, "aa")->phase, "applied"));
    LnJournalFree(&j);

    /* a torn last line is ignored, the next append terminates it and the chain stays valid */
    jp = LnTargetPath(&t, L"journal\\transactions.jsonl");
    CHECK_ERR(0, LnReadFileAll(jp, &data, &len));
    {
        LN_BUF b;

        LnBufInit(&b);
        LnBufAppend(&b, (char *)data, len);
        LnBufAppendStr(&b, "{\"schemaVersion\":1,\"sequence\":4,\"transac");
        CHECK_ERR(0, LnWriteFileAtomic(jp, b.data, b.len));
        LnBufFree(&b);
    }
    free(data);
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(j.count == 3 && j.tailTruncated);
    LnJournalFree(&j);
    memset(&e, 0, sizeof(e));
    strcpy(e.transactionId, "aa"); strcpy(e.phase, "confirmed"); strcpy(e.result, "ok");
    CHECK_ERR(0, LnJournalAppend(&t, &e));
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(j.count == 4 && j.entries[3].sequence == 4);
    LnJournalFree(&j);

    /* altering or removing an earlier entry breaks the chain */
    CHECK_ERR(0, LnReadFileAll(jp, &data, &len));
    {
        char *p = strstr((char *)data, "\"phase\":\"applying\"");

        CHECK(p != NULL);
        if (p)
        {
            memcpy(p, "\"phase\":\"xpplying\"", 18);
            CHECK_ERR(0, LnWriteFileAtomic(jp, data, len));
            CHECK_ERR(LN_E_JOURNAL_CORRUPT, LnJournalRead(&t, &j));
        }
    }
    free(data);
    free(jp);
    CHECK(OpenImage(&t, L"journal2"));
    for (i = 0; i < 3; i++)
    {
        memset(&e, 0, sizeof(e));
        strcpy(e.transactionId, "bb"); strcpy(e.phase, "applying"); strcpy(e.result, "ok");
        LnJournalAppend(&t, &e);
    }
    jp = LnTargetPath(&t, L"journal\\transactions.jsonl");
    LnReadFileAll(jp, &data, &len);
    {
        char *nl = strchr((char *)data, '\n');              /* drop the first line */
        CHECK(nl != NULL);
        if (nl)
        {
            LnWriteFileAtomic(jp, nl + 1, len - (size_t)(nl + 1 - (char *)data));
            CHECK_ERR(LN_E_JOURNAL_CORRUPT, LnJournalRead(&t, &j));
        }
    }
    free(data);
    free(jp);
}

static void TestTarget(void)
{
    LN_TARGET a, b;
    WCHAR root[MAX_PATH];

    Section("target");
    NewImage(root, L"target");
    CHECK_ERR(ERROR_PATH_NOT_FOUND, LnTargetOpen(&a, LnTargetImage, root, FALSE));   /* no servicing tree yet */
    CHECK_ERR(0, LnTargetOpen(&a, LnTargetImage, root, TRUE));
    CHECK_ERR(0, LnTargetOpen(&b, LnTargetImage, root, FALSE));
    CHECK(strlen(a.identity) == 36 && a.identity[8] == '-' && !strcmp(a.identity, b.identity));   /* machineId GUID */
    CHECK_ERR(ERROR_PATH_NOT_FOUND, LnTargetOpen(&a, LnTargetImage, L"C:\\does\\not\\exist\\xyz", TRUE));
}

static void TestVerify(void)
{
    char pin[65];
    WCHAR *p;
    LN_MANIFEST m;

    Section("verify (enforce)");
    CHECK_ERR(0, LnCertFileSha256(g_cer, pin));

    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingVerifyPackage(p, pin, &m));
    CHECK(m.itemCount == 1);
    LnManifestFree(&m);
    CHECK_ERR(LN_E_NO_TRUST_ANCHOR, LnServicingVerifyPackage(p, NULL, NULL));
    {
        char other[65];

        memset(other, 'a', 64);
        other[64] = 0;
        CHECK_ERR(LN_E_CERT_MISMATCH, LnServicingVerifyPackage(p, other, NULL));      /* pinned differs */
    }
    free(p);

    p = Fx(L"tampered-payload");
    CHECK_ERR(LN_E_HASH_MISMATCH, LnServicingVerifyPackage(p, pin, NULL));
    free(p);
    p = Fx(L"tampered-manifest");
    CHECK_ERR(LN_E_CATALOG_INVALID, LnServicingVerifyPackage(p, pin, NULL));          /* not covered by the catalog */
    free(p);
    p = Fx(L"tampered-sig");
    CHECK_ERR(LN_E_CATALOG_INVALID, LnServicingVerifyPackage(p, pin, NULL));          /* manifest.sig does not verify */
    free(p);
    p = Fx(L"no-manifest-sig");
    CHECK_ERR(LN_E_UNSIGNED, LnServicingVerifyPackage(p, pin, NULL));                 /* manifest.sig missing */
    free(p);
    p = Fx(L"unsigned");
    CHECK_ERR(LN_E_UNSIGNED, LnServicingVerifyPackage(p, pin, NULL));                 /* no catalog at all */
    free(p);
    p = Fx(L"unsigned-catalog");
    CHECK_ERR(LN_E_UNSIGNED, LnServicingVerifyPackage(p, pin, NULL));                 /* catalog without signature */
    free(p);
    p = Fx(L"wrongcert");
    CHECK_ERR(LN_E_CERT_MISMATCH, LnServicingVerifyPackage(p, pin, NULL));            /* valid signature, other signer */
    free(p);
    p = Fx(L"kernel");
    CHECK_ERR(LN_E_FORBIDDEN_TARGET, LnServicingVerifyPackage(p, pin, NULL));
    free(p);
    p = Fx(L"missing-payload");
    CHECK_ERR(LN_E_HASH_MISMATCH, LnServicingVerifyPackage(p, pin, NULL));
    free(p);
}

static void TestAddRejects(void)
{
    LN_TARGET t;
    WCHAR *p;
    char tx[65];
    LN_JOURNAL j;
    char (*ids)[130];
    size_t n;

    Section("add-package rejects before staging");
    CHECK(OpenImage(&t, L"rejects"));
    p = Fx(L"good1");
    CHECK_ERR(LN_E_NO_TRUST_ANCHOR, LnServicingAddPackage(&t, p, tx));                /* no certificate installed */
    free(p);
    InstallTrust(&t);
    p = Fx(L"tampered-payload");
    CHECK_ERR(LN_E_HASH_MISMATCH, LnServicingAddPackage(&t, p, tx));
    free(p);
    p = Fx(L"unsigned");
    CHECK_ERR(LN_E_UNSIGNED, LnServicingAddPackage(&t, p, tx));
    free(p);
    p = Fx(L"wrongcert");
    CHECK_ERR(LN_E_CERT_MISMATCH, LnServicingAddPackage(&t, p, tx));
    free(p);
    CHECK_ERR(0, LnStoreList(&t, &ids, &n));
    CHECK(n == 0);                                        /* nothing reached the store */
    free(ids);
    {
        WCHAR *objs = LnTargetPath(&t, L"store\\objects");

        CHECK(!LnPathExists(objs));
        free(objs);
    }
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(j.count >= 3 && !strcmp(j.entries[j.count - 1].phase, "failed"));          /* rejections are journaled */
    LnJournalFree(&j);
}

static void TestLifecycle(void)
{
    LN_TARGET t;
    WCHAR *p, *target, *sys;
    char tx[65], tx2[65];
    BOOL reboot;
    LN_JOURNAL j;
    size_t before;

    Section("lifecycle: add, apply, idempotence, confirm, rollback");
    CHECK(OpenImage(&t, L"life"));
    InstallTrust(&t);
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    WriteText(target, "original");

    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx2));                                   /* idempotent */
    CHECK(!strcmp(tx, tx2));
    CHECK(FileIs(target, "original"));                                                 /* staging touches nothing */
    free(p);

    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    CHECK(!reboot && FileIs(target, "version one"));
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(!strcmp(LnJournalLast(&j, tx)->phase, "applied") && !LnJournalLast(&j, tx)->healthConfirmed);
    before = j.count;
    LnJournalFree(&j);
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx2));      /* same request again */
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(j.count == before);                                                          /* no duplicate work */
    LnJournalFree(&j);

    /* the backup was verified before the first change */
    {
        WCHAR bak[MAX_PATH];
        WCHAR *w = LnUtf8ToWide(tx), *bp;

        wcscpy(bak, L"backup\\");
        wcscat(bak, w);
        wcscat(bak, L"\\0001");
        bp = LnTargetPath(&t, bak);
        CHECK(FileIs(bp, "original"));
        free(bp);
        free(w);
    }

    /* not confirmed yet; corrupting the component makes confirmation refuse */
    WriteText(target, "damaged by someone");
    CHECK_ERR(LN_E_VERIFY_FINAL, LnServicingConfirmHealth(&t, "ln.fixture", "1.0"));
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(!LnJournalLast(&j, tx)->healthConfirmed);
    LnJournalFree(&j);

    /* rollback works even though the updated component is damaged */
    CHECK_ERR(0, LnServicingRollback(&t, "ln.fixture", "1.0", tx));
    CHECK(FileIs(target, "original"));
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(!strcmp(LnJournalLast(&j, tx)->phase, "rolled-back"));
    LnJournalFree(&j);
    CHECK_ERR(0, LnServicingRollback(&t, "ln.fixture", "1.0", tx));                    /* idempotent */

    /* apply again after a rollback, confirm, then remove-package restores the original */
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    CHECK(FileIs(target, "version one"));
    CHECK_ERR(0, LnServicingConfirmHealth(&t, "ln.fixture", "1.0"));
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(!strcmp(LnJournalLast(&j, tx)->phase, "confirmed") && LnJournalLast(&j, tx)->healthConfirmed);
    LnJournalFree(&j);
    CHECK_ERR(0, LnServicingConfirmHealth(&t, "ln.fixture", "1.0"));                   /* idempotent */
    CHECK_ERR(0, LnServicingRemovePackage(&t, "ln.fixture", "1.0"));
    CHECK(FileIs(target, "original"));
    CHECK_ERR(LN_E_NOT_FOUND, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));

    /* the state view is derived and reconstructible */
    {
        WCHAR *sp = LnTargetPath(&t, L"journal\\state.json");

        CHECK(LnPathExists(sp));
        DeleteFileW(sp);
        CHECK_ERR(0, LnJournalWriteState(&t));
        CHECK(LnPathExists(sp));
        free(sp);
    }
    sys = NULL;
    free(sys);
    free(target);
}

static void TestFailureRollsBack(void)
{
    LN_TARGET t;
    WCHAR *p, *t1, *blocker;
    char tx[65];
    BOOL reboot;
    LN_JOURNAL j;

    Section("failed operation: immediate rollback, then recovery");
    CHECK(OpenImage(&t, L"fail"));
    InstallTrust(&t);
    t1 = LnTargetFile(&t, L"Windows\\System32\\first.dll");
    blocker = LnTargetFile(&t, L"Windows\\System32\\blocked");                          /* a FILE where a directory is needed */
    WriteText(t1, "first original");
    WriteText(blocker, "i am a file");

    p = Fx(L"twoitems");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    free(p);
    CHECK(LnServicingApplyPackage(&t, "ln.twoitems", "1.0", &reboot, tx) != 0);       /* second item cannot be created */
    CHECK(FileIs(t1, "first original"));                                               /* the first one was reverted */
    CHECK_ERR(0, LnJournalRead(&t, &j));
    {
        const LN_JENTRY *l = LnJournalLast(&j, tx);

        CHECK(l && !strcmp(l->phase, "rolled-back"));
    }
    LnJournalFree(&j);

    DeleteFileW(blocker);                                                              /* cause removed: retry succeeds */
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.twoitems", "1.0", &reboot, tx));
    CHECK(FileIs(t1, "first new"));
    free(t1);
    free(blocker);
}

static void TestCorruptStore(void)
{
    LN_TARGET t;
    WCHAR *p, *obj, *target;
    char tx[65];
    BOOL reboot;
    LN_MANIFEST m;
    char pin[65];

    Section("corrupt store object is refused before any change");
    CHECK(OpenImage(&t, L"corrupt"));
    InstallTrust(&t);
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    CHECK_ERR(0, LnCertFileSha256(g_cer, pin));
    CHECK_ERR(0, LnServicingVerifyPackage(p, pin, &m));
    free(p);
    CHECK_ERR(0, LnStoreObjectPath(&t, m.items[0].sha256, &obj));
    WriteText(obj, "flipped bits");
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    CHECK_ERR(LN_E_HASH_MISMATCH, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    CHECK(!LnPathExists(target));                                                      /* nothing was written */
    LnManifestFree(&m);
    free(obj);
    free(target);
}

static void TestPendingPlan(void)
{
    LN_TARGET t;
    WCHAR *p, *target;
    char tx[65], *plan;
    BOOL reboot;
    HANDLE lock;
    LN_ATTEMPT a;
    const char *why;
    LN_JSON *root;

    Section("locked target: pending-first-boot plan and attempt rules (S1-CONTRACTS.md 1, 5)");
    CHECK(OpenImage(&t, L"pending"));
    InstallTrust(&t);
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    WriteText(target, "locked original");
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    free(p);

    lock = CreateFileW(target, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);   /* readers only: replace is denied */
    CHECK(lock != INVALID_HANDLE_VALUE);
    LnTestLockedAsPending = TRUE;
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    LnTestLockedAsPending = FALSE;
    CHECK(reboot);
    CloseHandle(lock);
    CHECK(FileIs(target, "locked original"));                                          /* not installed, not claimed */
    {
        LN_JOURNAL j;

        CHECK_ERR(0, LnJournalRead(&t, &j));
        CHECK(!strcmp(LnJournalLast(&j, tx)->phase, "pending-first-boot") && LnJournalLast(&j, tx)->rebootRequired);
        LnJournalFree(&j);
    }
    CHECK_ERR(0, LnServicingReadPendingPlan(&t, &plan));
    CHECK_ERR(0, LnJsonParse(plan, strlen(plan), &root));
    {
        long long schema;
        BOOL rr;
        const LN_JSON *ops = LnJsonGet(root, "operations"), *op0 = ops ? ops->child : NULL;
        const LN_JSON *tg = LnJsonGet(root, "target"), *pk = LnJsonGet(root, "package");
        char want[65], man[65];

        CHECK(LnJsonGetInt(root, "schema", &schema) && schema == 1);
        CHECK(!strcmp(LnJsonGetString(root, "transactionId"), tx));
        CHECK(!strcmp(LnJsonGetString(root, "operation"), "install"));
        CHECK(LnJsonGetBool(root, "requiresReboot", &rr) && rr);
        CHECK(!strcmp(LnJsonGetString(tg, "machineId"), t.identity) && !strcmp(LnJsonGetString(tg, "arch"), "amd64"));
        CHECK(!strcmp(LnJsonGetString(tg, "line"), "main-nt10") && LnJsonGetString(tg, "base") != NULL);
        CHECK(!strcmp(LnJsonGetString(pk, "id"), "ln.fixture") && !strcmp(LnJsonGetString(pk, "version"), "1.0"));
        {
            WCHAR *fx = Fx(L"good1\\manifest.json");
            BYTE *d;
            size_t n;

            CHECK_ERR(0, LnReadFileAll(fx, &d, &n));
            LnSha256Hex(d, n, man);
            free(d);
            free(fx);
        }
        CHECK(!strcmp(LnJsonGetString(pk, "digest"), man));
        CHECK(op0 && !op0->next);                                                       /* only the locked item is deferred */
        CHECK(!strcmp(LnJsonGetString(op0, "type"), "replace-file"));                  /* the file existed */
        CHECK(!strcmp(LnJsonGetString(op0, "component"), "ln.fixture"));
        CHECK(!strcmp(LnJsonGetString(op0, "target"), "\\Windows\\System32\\lnfixture.dll"));
        LnSha256Hex("version one", 11, want);
        CHECK(!strcmp(LnJsonGetString(op0, "sha256"), want));
        {
            char src[128], bk[128];

            strcpy(src, "store/objects/");
            strcat(src, want);
            CHECK(!strcmp(LnJsonGetString(op0, "source"), src));
            strcpy(bk, "backup/");
            strcat(bk, tx);
            strcat(bk, "/0001");
            CHECK(!strcmp(LnJsonGetString(op0, "backup"), bk));
        }
        CHECK(strlen(LnJsonGetString(root, "signature")) > 300);                       /* base64 of an RSA-3072 signature */
    }
    LnJsonFree(root);
    free(plan);
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));       /* resumes: still pending */
    CHECK(reboot);

    /* confirming before the boot applier did its work is refused: the hash is not final */
    CHECK_ERR(LN_E_VERIFY_FINAL, LnServicingConfirmHealth(&t, "ln.fixture", "1.0"));

    /* boot attempt rules: ShouldRollback is evaluated BEFORE MarkBootStarted */
    CHECK_ERR(LN_E_NOT_FOUND, LnServicingReadAttempt(&t, &a));                         /* absent = new plan */
    memset(&a, 0, sizeof(a));
    CHECK(!LnServicingShouldRollback(&a, &why));
    CHECK_ERR(0, LnServicingMarkBootStarted(&t, &a));
    CHECK(a.attempt == 1 && !strcmp(a.lastResult, "started") && !a.failureRecorded);
    /* the machine lost power: no result was written; the next boot reads attempt=1 */
    CHECK_ERR(0, LnServicingReadAttempt(&t, &a));
    CHECK(!LnServicingShouldRollback(&a, &why));                                       /* within the limit: try again */
    CHECK_ERR(0, LnServicingMarkBootStarted(&t, &a));
    CHECK(a.attempt == 2 && !strcmp(a.previousResult, "started"));                     /* the last boot never finished */
    /* a third boot without health confirmation: roll back, as an unexpected reboot */
    CHECK_ERR(0, LnServicingReadAttempt(&t, &a));
    CHECK(LnServicingShouldRollback(&a, &why) && !strcmp(why, "unexpected-reboot"));

    {
        LN_ATTEMPT one;

        memset(&one, 0, sizeof(one));
        one.attempt = 1;
        CHECK(!LnServicingShouldRollback(&one, &why));
        one.attempt = 2;
        one.failureRecorded = TRUE;                                                    /* a verifiable failure names itself */
        CHECK(LnServicingShouldRollback(&one, &why) && !strcmp(why, "verifiable-failure"));
        one.healthConfirmed = TRUE;
        CHECK(!LnServicingShouldRollback(&one, &why));                                 /* health wins */
    }
    CHECK_ERR(0, LnServicingMarkBootOutcome(&t, TRUE, FALSE));
    CHECK_ERR(0, LnServicingReadAttempt(&t, &a));
    CHECK(a.failureRecorded && !strcmp(a.lastResult, "failed"));

    /* rolling back a transaction that never reached the target removes the plan */
    CHECK_ERR(0, LnServicingRollback(&t, "ln.fixture", "1.0", tx));
    CHECK(FileIs(target, "locked original"));
    CHECK_ERR(LN_E_NOT_FOUND, LnServicingReadPendingPlan(&t, &plan));
    free(target);
}

static void TestExportLog(void)
{
    LN_TARGET t;
    WCHAR dest[MAX_PATH], *f;

    Section("export-log");
    CHECK(OpenImage(&t, L"life"));
    wcscpy(dest, g_work);
    wcscat(dest, L"\\exported");
    CHECK_ERR(0, LnServicingExportLog(&t, dest));
    f = LnJoin(dest, L"transactions.jsonl");
    CHECK(LnPathExists(f));
    free(f);
    f = LnJoin(dest, L"state.json");
    CHECK(LnPathExists(f));
    free(f);
}

int wmain(int argc, WCHAR **argv)
{
    WCHAR tmp[MAX_PATH];

    if (argc < 3)
    {
        fputs("usage: libservicing-tests <fixtures-dir> <dev-certificate.cer>\n", stderr);
        return 2;
    }
    g_fx = argv[1];
    g_cer = argv[2];
    GetTempPathW(MAX_PATH, tmp);
    wsprintfW(g_work, L"%slnsvc-tests-%lu", tmp, GetCurrentProcessId());
    LnEnsureDir(g_work);

    TestSha256();
    TestJson();
    TestPaths();
    TestManifest();
    TestTransactionId();
    TestJournal();
    TestTarget();
    TestVerify();
    TestAddRejects();
    TestLifecycle();
    TestFailureRollsBack();
    TestCorruptStore();
    TestPendingPlan();
    TestExportLog();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
