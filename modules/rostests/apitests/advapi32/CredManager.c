/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the Credential Manager API (CredWrite/Read/Enumerate/Delete)
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <apitest.h>
#include <wincred.h>
#include <winreg.h>

static const BYTE Secret[] = { 's','3','c','r','e','t', 0, 0xff, 0x10, 0x00 };
#define TEST_PREFIX  L"LiberNT.apitest/"
#define TEST_TARGET  L"LiberNT.apitest/cred"
#define STORE_PATH   L"Software\\Wine\\Credential Manager\\"

/* Open the registry key where cred.c stores a credential, to inspect/plant the raw value. Couples to
 * the internal store layout on purpose, for the secret-at-rest and legacy-blob checks. */
static HKEY
OpenStoredKey(
    _In_ LPCWSTR Target,
    _In_ DWORD Type,
    _In_ REGSAM Access,
    _In_ BOOL Create)
{
    WCHAR path[260];
    HKEY key = NULL;

    wcscpy(path, STORE_PATH);
    wcscat(path, Type == CRED_TYPE_GENERIC ? L"Generic: " : L"DomPasswd: ");
    wcscat(path, Target);

    if (Create)
        RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, Access, NULL, &key, NULL);
    else
        RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, Access, &key);
    return key;
}

static BOOL
StoredSecretInClear(
    _In_ LPCWSTR Target,
    _In_ DWORD Type)
{
    HKEY key = OpenStoredKey(Target, Type, KEY_QUERY_VALUE, FALSE);
    BYTE buf[4096];
    DWORD regType, size = sizeof(buf), i;
    BOOL found = FALSE;

    if (!key)
        return FALSE;
    if (RegQueryValueExW(key, L"Password", NULL, &regType, buf, &size) == ERROR_SUCCESS)
        for (i = 0; i + sizeof(Secret) <= size; i++)
            if (memcmp(buf + i, Secret, sizeof(Secret)) == 0) { found = TRUE; break; }
    RegCloseKey(key);
    return found;
}

/* Write then read back a credential of the given type, checking the secret, user name, type and
 * persistence round-trip and that the secret is not stored in the clear; then enumerate it with a
 * prefix filter and delete it. Returns FALSE (after a skip) when CredWrite is unavailable (no DPAPI
 * pre-key for this logon, or lsass/DPAPI down), so the suite does not hard-fail off a lab. */
static BOOL
TestRoundTrip(
    _In_ DWORD Type)
{
    CREDENTIALW cred;
    PCREDENTIALW out = NULL;
    PCREDENTIALW *list = NULL;
    DWORD count = 0, i;
    BOOL found = FALSE;

    ZeroMemory(&cred, sizeof(cred));
    cred.Type = Type;
    cred.TargetName = (LPWSTR)TEST_TARGET;
    cred.UserName = (LPWSTR)L"tester";
    cred.Comment = (LPWSTR)L"apitest";
    cred.CredentialBlobSize = sizeof(Secret);
    cred.CredentialBlob = (BYTE *)Secret;
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    if (!CredWriteW(&cred, 0))
    {
        skip("CredWriteW(type %lu) unavailable (no DPAPI pre-key / lsass?), error %lu\n", Type, GetLastError());
        return FALSE;
    }

    ok(CredReadW(TEST_TARGET, Type, 0, &out), "CredReadW(type %lu) failed, error %lu\n", Type, GetLastError());
    if (out)
    {
        ok(out->CredentialBlobSize == sizeof(Secret) && out->CredentialBlob != NULL &&
           memcmp(out->CredentialBlob, Secret, sizeof(Secret)) == 0, "secret did not round-trip\n");
        ok(out->Type == Type, "type not preserved: %lu\n", out->Type);
        ok(out->UserName != NULL && wcscmp(out->UserName, L"tester") == 0, "user name not preserved\n");
        ok(out->Persist == CRED_PERSIST_LOCAL_MACHINE, "persist not preserved: %lu\n", out->Persist);
        CredFree(out);
    }

    ok(!StoredSecretInClear(TEST_TARGET, Type), "the secret is stored in the clear\n");

    /* Enumerate with a prefix filter and find our credential. */
    if (CredEnumerateW(TEST_PREFIX L"*", 0, &count, &list))
    {
        for (i = 0; i < count; i++)
            if (list[i]->TargetName && wcscmp(list[i]->TargetName, TEST_TARGET) == 0)
                found = TRUE;
        ok(found, "CredEnumerateW did not return our credential (count %lu)\n", count);
        CredFree(list);
    }
    else
    {
        ok(0, "CredEnumerateW failed, error %lu\n", GetLastError());
    }

    ok(CredDeleteW(TEST_TARGET, Type, 0), "CredDeleteW failed, error %lu\n", GetLastError());

    out = NULL;
    SetLastError(0xdeadbeef);
    ok(!CredReadW(TEST_TARGET, Type, 0, &out), "CredReadW after delete should fail\n");
    ok(GetLastError() == ERROR_NOT_FOUND, "expected ERROR_NOT_FOUND after delete, got %lu\n", GetLastError());
    if (out) CredFree(out);
    return TRUE;
}

static void
TestMissingAndParams(void)
{
    CREDENTIALW cred;
    PCREDENTIALW out = NULL;

    SetLastError(0xdeadbeef);
    ok(!CredReadW(L"LiberNT.apitest/does-not-exist", CRED_TYPE_GENERIC, 0, &out),
       "CredReadW of a missing credential should fail\n");
    ok(GetLastError() == ERROR_NOT_FOUND, "expected ERROR_NOT_FOUND, got %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ok(!CredDeleteW(L"LiberNT.apitest/does-not-exist", CRED_TYPE_GENERIC, 0),
       "CredDeleteW of a missing credential should fail\n");
    ok(GetLastError() == ERROR_NOT_FOUND, "expected ERROR_NOT_FOUND, got %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ok(!CredReadW(NULL, CRED_TYPE_GENERIC, 0, &out), "CredReadW(NULL target) should fail\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", GetLastError());

    /* A write with no TargetName is invalid. */
    ZeroMemory(&cred, sizeof(cred));
    cred.Type = CRED_TYPE_GENERIC;
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    SetLastError(0xdeadbeef);
    ok(!CredWriteW(&cred, 0), "CredWriteW with no TargetName should fail\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", GetLastError());
}

/* A read with the wrong type must not return a credential written under another type. */
static void
TestWrongType(void)
{
    CREDENTIALW cred;
    PCREDENTIALW out = NULL;

    ZeroMemory(&cred, sizeof(cred));
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = (LPWSTR)TEST_TARGET;
    cred.CredentialBlobSize = sizeof(Secret);
    cred.CredentialBlob = (BYTE *)Secret;
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&cred, 0))
    {
        skip("CredWriteW unavailable, error %lu\n", GetLastError());
        return;
    }
    ok(!CredReadW(TEST_TARGET, CRED_TYPE_DOMAIN_PASSWORD, 0, &out),
       "a GENERIC credential was read back as DOMAIN_PASSWORD\n");
    if (out) CredFree(out);
    CredDeleteW(TEST_TARGET, CRED_TYPE_GENERIC, 0);
}

/* The blob size is capped (CRED_MAX_CREDENTIAL_BLOB_SIZE); this is rejected before any DPAPI call. */
static void
TestSizeCap(void)
{
    CREDENTIALW cred;
    static BYTE big[CRED_MAX_CREDENTIAL_BLOB_SIZE + 1];

    ZeroMemory(&cred, sizeof(cred));
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = (LPWSTR)TEST_TARGET;
    cred.CredentialBlobSize = sizeof(big);
    cred.CredentialBlob = big;
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    SetLastError(0xdeadbeef);
    ok(!CredWriteW(&cred, 0), "an over-size credential blob was accepted\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", GetLastError());
}

/* A credential whose Password predates DPAPI (no LDP1 header) cannot be decrypted; it must read as
 * absent, and it must not break enumeration of the others. */
static void
TestLegacyBlob(void)
{
    static const BYTE legacy[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const WCHAR target[] = L"LiberNT.apitest/cred";
    DWORD type = CRED_TYPE_GENERIC, cnt = 0;
    PCREDENTIALW out = NULL, *list = NULL;
    HKEY key;

    key = OpenStoredKey(target, CRED_TYPE_GENERIC, KEY_SET_VALUE, TRUE);
    if (!key)
    {
        skip("could not plant a legacy credential\n");
        return;
    }
    RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)target, sizeof(target));
    RegSetValueExW(key, L"Type", 0, REG_DWORD, (const BYTE *)&type, sizeof(type));
    RegSetValueExW(key, L"Password", 0, REG_BINARY, legacy, sizeof(legacy));
    RegCloseKey(key);

    ok(!CredReadW(target, CRED_TYPE_GENERIC, 0, &out), "a legacy (RC4) credential was read back\n");
    if (out) CredFree(out);

    /* Enumeration must not fail just because one entry is undecryptable. */
    ok(CredEnumerateW(TEST_PREFIX L"*", 0, &cnt, &list) || GetLastError() == ERROR_NOT_FOUND,
       "CredEnumerateW aborted on an undecryptable entry, error %lu\n", GetLastError());
    if (list) CredFree(list);

    CredDeleteW(target, CRED_TYPE_GENERIC, 0);
}

START_TEST(CredManager)
{
    /* Best-effort cleanup of a leftover from a previous aborted run. */
    CredDeleteW(TEST_TARGET, CRED_TYPE_GENERIC, 0);
    CredDeleteW(TEST_TARGET, CRED_TYPE_DOMAIN_PASSWORD, 0);

    if (TestRoundTrip(CRED_TYPE_GENERIC))
        TestRoundTrip(CRED_TYPE_DOMAIN_PASSWORD);

    TestWrongType();
    TestSizeCap();
    TestLegacyBlob();
    TestMissingAndParams();
}
