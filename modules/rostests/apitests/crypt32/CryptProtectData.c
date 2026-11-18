/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests CryptProtectData and CryptUnprotectData
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <apitest.h>
#include <wincrypt.h>

static const BYTE Secret[] = { 'h','u','n','t','2',0,'w','a','b','b','i','t',0xff,0x00,0x10 };

/* Protect Secret (optionally with a description and entropy) and read it back, checking that the
 * plaintext and the description both round-trip and that the blob is not clear text. Returns the
 * CryptProtectData result so the caller can skip when the logon has no DPAPI pre-key. */
static BOOL
TestRoundTrip(
    _In_ DWORD Flags,
    _In_opt_ LPCWSTR Description,
    _In_reads_bytes_opt_(EntropyLen) const BYTE *Entropy,
    _In_ DWORD EntropyLen)
{
    DATA_BLOB In, Out = { 0 }, Back = { 0 }, Ent = { 0 };
    LPWSTR DescrOut = NULL;

    In.cbData = sizeof(Secret);
    In.pbData = (BYTE *)Secret;
    if (Entropy)
    {
        Ent.cbData = EntropyLen;
        Ent.pbData = (BYTE *)Entropy;
    }

    if (!CryptProtectData(&In, Description, Entropy ? &Ent : NULL, NULL, NULL, Flags, &Out))
        return FALSE;

    ok(Out.cbData != 0 && Out.pbData != NULL, "protect produced an empty blob\n");
    ok(Out.cbData < sizeof(Secret) || memcmp(Out.pbData, Secret, sizeof(Secret)) != 0,
       "the blob still contains the plaintext\n");

    ok(CryptUnprotectData(&Out, &DescrOut, Entropy ? &Ent : NULL, NULL, NULL, Flags, &Back),
       "unprotect failed, error %lu\n", GetLastError());
    ok(Back.cbData == sizeof(Secret) && Back.pbData != NULL &&
       memcmp(Back.pbData, Secret, sizeof(Secret)) == 0, "plaintext did not round-trip\n");

    if (Description)
        ok(DescrOut != NULL && wcscmp(DescrOut, Description) == 0,
           "description did not round-trip (got %p)\n", DescrOut);
    else
        ok(DescrOut != NULL && DescrOut[0] == L'\0',
           "expected an empty description (got %p)\n", DescrOut);

    if (Out.pbData) LocalFree(Out.pbData);
    if (Back.pbData) LocalFree(Back.pbData);
    if (DescrOut) LocalFree(DescrOut);
    return TRUE;
}

static void
TestTamper(void)
{
    DATA_BLOB In, Out = { 0 }, Back = { 0 };

    In.cbData = sizeof(Secret);
    In.pbData = (BYTE *)Secret;

    if (!CryptProtectData(&In, L"tamper", NULL, NULL, NULL, CRYPTPROTECT_LOCAL_MACHINE, &Out))
    {
        skip("LOCAL_MACHINE protect unavailable, error %lu\n", GetLastError());
        return;
    }

    /* Flip a byte near the end (inside the ciphertext/MAC) and expect the integrity check to reject it. */
    Out.pbData[Out.cbData - 1] ^= 0xFF;
    ok(!CryptUnprotectData(&Out, NULL, NULL, NULL, NULL, 0, &Back),
       "unprotect accepted a tampered blob\n");
    ok(GetLastError() == ERROR_INVALID_DATA, "expected ERROR_INVALID_DATA, got %lu\n", GetLastError());

    if (Out.pbData) LocalFree(Out.pbData);
    if (Back.pbData) LocalFree(Back.pbData);
}

/* A blob protected with one entropy cannot be opened with another: the in-process analogue of a
 * different key failing. True cross-user / cross-machine-key separation needs a multi-session lab. */
static void
TestWrongEntropy(void)
{
    static const BYTE EntA[] = { 1, 2, 3, 4 };
    static const BYTE EntB[] = { 4, 3, 2, 1 };
    DATA_BLOB In, Out = { 0 }, Back = { 0 }, A, B;

    In.cbData = sizeof(Secret);
    In.pbData = (BYTE *)Secret;
    A.cbData = sizeof(EntA); A.pbData = (BYTE *)EntA;
    B.cbData = sizeof(EntB); B.pbData = (BYTE *)EntB;

    if (!CryptProtectData(&In, NULL, &A, NULL, NULL, CRYPTPROTECT_LOCAL_MACHINE, &Out))
    {
        skip("LOCAL_MACHINE protect unavailable, error %lu\n", GetLastError());
        return;
    }

    ok(!CryptUnprotectData(&Out, NULL, &B, NULL, NULL, 0, &Back),
       "unprotect succeeded with the wrong entropy\n");

    /* Sanity: the correct entropy still opens it. */
    ok(CryptUnprotectData(&Out, NULL, &A, NULL, NULL, 0, &Back),
       "unprotect failed with the right entropy, error %lu\n", GetLastError());

    if (Out.pbData) LocalFree(Out.pbData);
    if (Back.pbData) LocalFree(Back.pbData);
}

static void
TestParams(void)
{
    DATA_BLOB In, Out = { 0 };
    BYTE reserved = 0;

    In.cbData = sizeof(Secret);
    In.pbData = (BYTE *)Secret;

    SetLastError(0xdeadbeef);
    ok(!CryptProtectData(NULL, NULL, NULL, NULL, NULL, 0, &Out), "protect accepted a NULL input\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ok(!CryptProtectData(&In, NULL, NULL, NULL, NULL, 0, NULL), "protect accepted a NULL output\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ok(!CryptProtectData(&In, NULL, NULL, &reserved, NULL, 0, &Out), "protect accepted a non-NULL pvReserved\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", GetLastError());
}

START_TEST(CryptProtectData)
{
    static const BYTE Entropy[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x55 };

    /* User-scoped: depends on an interactive logon having a DPAPI pre-key. */
    if (!TestRoundTrip(0, L"user secret", NULL, 0))
        skip("user-scoped CryptProtectData unavailable (no logon pre-key?), error %lu\n", GetLastError());
    else
        TestRoundTrip(0, NULL, Entropy, sizeof(Entropy));

    /* Machine-scoped: uses the LSA machine key, so it does not need a logon pre-key. */
    if (!TestRoundTrip(CRYPTPROTECT_LOCAL_MACHINE, L"machine secret", NULL, 0))
        skip("LOCAL_MACHINE CryptProtectData unavailable, error %lu\n", GetLastError());

    TestTamper();
    TestWrongEntropy();
    TestParams();
}
