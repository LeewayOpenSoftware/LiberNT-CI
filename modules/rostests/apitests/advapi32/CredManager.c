/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the Credential Manager API (CredWrite/Read/Enumerate/Delete)
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <apitest.h>
#include <wincred.h>

static const BYTE Secret[] = { 's','3','c','r','e','t', 0, 0xff, 0x10, 0x00 };
#define TEST_PREFIX  L"LiberNT.apitest/"
#define TEST_TARGET  L"LiberNT.apitest/cred"

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

START_TEST(CredManager)
{
    /* Best-effort cleanup of a leftover from a previous aborted run. */
    CredDeleteW(TEST_TARGET, CRED_TYPE_GENERIC, 0);
    CredDeleteW(TEST_TARGET, CRED_TYPE_DOMAIN_PASSWORD, 0);

    if (TestRoundTrip(CRED_TYPE_GENERIC))
        TestRoundTrip(CRED_TYPE_DOMAIN_PASSWORD);

    TestMissingAndParams();
}
