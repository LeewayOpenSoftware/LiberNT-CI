/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests CredProtect, CredUnprotect and CredIsProtected
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <wincred.h>

typedef BOOL (WINAPI *PFN_PROTECTW)(BOOL, LPWSTR, DWORD, LPWSTR, DWORD *, CRED_PROTECTION_TYPE *);
typedef BOOL (WINAPI *PFN_PROTECTA)(BOOL, LPSTR, DWORD, LPSTR, DWORD *, CRED_PROTECTION_TYPE *);
typedef BOOL (WINAPI *PFN_UNPROTECTW)(BOOL, LPWSTR, DWORD, LPWSTR, DWORD *);
typedef BOOL (WINAPI *PFN_ISPROTECTEDW)(LPWSTR, CRED_PROTECTION_TYPE *);

static PFN_PROTECTW pProtectW;
static PFN_PROTECTA pProtectA;
static PFN_UNPROTECTW pUnprotectW;
static PFN_ISPROTECTEDW pIsProtectedW;

static const WCHAR Prefix[] = {'@', '@', 'D', 7, 8, 12, 10, 13};

static BOOL
Protect(_In_ PCWSTR Plain, _In_ DWORD Cch, _Out_writes_(Capacity) PWSTR Out, _In_ DWORD Capacity, _Out_ DWORD *Chars)
{
    CRED_PROTECTION_TYPE Type = 99;
    BOOL Ret;

    *Chars = Capacity;
    Ret = pProtectW(FALSE, (LPWSTR)Plain, Cch, Out, Chars, &Type);
    ok(Ret, "CredProtectW failed: %lu\n", GetLastError());
    if (Ret)
        ok_long(Type, CredUserProtection);
    return Ret;
}

static VOID
TestProtect(void)
{
    WCHAR Out[2048], Twice[2048];
    CRED_PROTECTION_TYPE Type;
    DWORD Chars, Need;

    if (!Protect(L"pw", 3, Out, ARRAYSIZE(Out), &Need))
        return;
    ok_long(Need, lstrlenW(Out) + 1);
    ok(!memcmp(Out, Prefix, sizeof(Prefix)), "protected text does not start with the marker\n");

    Chars = 0;
    SetLastError(0xdeadbeef);
    ok(!pProtectW(FALSE, L"pw", 3, NULL, &Chars, NULL), "size query succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_long(Chars, Need);

    Chars = 10;
    SetLastError(0xdeadbeef);
    ok(!pProtectW(FALSE, L"pw", 3, Twice, &Chars, NULL), "protect into a short buffer succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_long(Chars, Need);

    Chars = ARRAYSIZE(Twice);
    SetLastError(0xdeadbeef);
    ok(!pProtectW(FALSE, NULL, 3, Twice, &Chars, NULL), "protect of NULL succeeded\n");
    ok_err(ERROR_INVALID_PARAMETER);

    Chars = ARRAYSIZE(Twice);
    Type = 99;
    ok(pProtectW(TRUE, L"pw", 3, Twice, &Chars, &Type), "protect as self failed: %lu\n", GetLastError());
    ok_long(Type, CredUserProtection);

    Chars = ARRAYSIZE(Twice);
    ok(pProtectW(FALSE, L"pw", 0, Twice, &Chars, NULL), "protect of a zero count failed: %lu\n", GetLastError());
    ok_long(Chars, 0);
    ok(!memcmp(Twice, Prefix, sizeof(Prefix)), "a zero count was not protected\n");

    if (Protect(Out, lstrlenW(Out) + 1, Twice, ARRAYSIZE(Twice), &Chars))
        ok(Chars > Need, "protected text was not protected again (%lu, %lu)\n", Chars, Need);
}

static VOID
TestUnprotect(void)
{
    WCHAR P3[2048], P2[2048], Twice[2048], Out[2048], Bad[64];
    DWORD Chars, Len;

    if (!Protect(L"pw", 3, P3, ARRAYSIZE(P3), &Chars) || !Protect(L"pw", 2, P2, ARRAYSIZE(P2), &Chars))
        return;
    Len = lstrlenW(P3);

    Chars = ARRAYSIZE(Out);
    ok(pUnprotectW(FALSE, P3, Len + 1, Out, &Chars), "unprotect failed: %lu\n", GetLastError());
    ok(!wcscmp(Out, L"pw"), "unprotected '%ls'\n", Out);
    ok_long(Chars, 3);

    Chars = ARRAYSIZE(Out);
    ok(pUnprotectW(FALSE, P3, Len, Out, &Chars), "unprotect without the terminator failed\n");
    ok(!wcscmp(Out, L"pw"), "unprotected '%ls'\n", Out);

    Chars = ARRAYSIZE(Out);
    ok(pUnprotectW(FALSE, P3, 10, Out, &Chars), "unprotect with a short count failed\n");
    ok(!wcscmp(Out, L"pw"), "a short count changed the result: '%ls'\n", Out);

    Chars = ARRAYSIZE(Out);
    ok(pUnprotectW(FALSE, P2, lstrlenW(P2) + 1, Out, &Chars), "unprotect of two characters failed\n");
    ok_long(Chars, 2);
    ok(!memcmp(Out, L"pw", 4), "unprotected two characters mismatch\n");

    Chars = 0;
    SetLastError(0xdeadbeef);
    ok(!pUnprotectW(FALSE, P3, Len + 1, NULL, &Chars), "unprotect size query succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_long(Chars, 3);

    Chars = 2;
    SetLastError(0xdeadbeef);
    ok(!pUnprotectW(FALSE, P3, Len + 1, Out, &Chars), "unprotect into two characters succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_long(Chars, 3);

    Chars = 3;
    ok(pUnprotectW(FALSE, P3, Len + 1, Out, &Chars), "unprotect into three characters failed\n");
    ok(!wcscmp(Out, L"pw"), "unprotected '%ls'\n", Out);

    Chars = ARRAYSIZE(Out);
    ok(pUnprotectW(TRUE, P3, Len + 1, Out, &Chars), "unprotect as self failed\n");
    ok(!wcscmp(Out, L"pw"), "unprotected '%ls'\n", Out);

    Chars = ARRAYSIZE(Out);
    ok(pUnprotectW(FALSE, L"pw", 3, Out, &Chars), "unprotect of plain text failed\n");
    ok(!wcscmp(Out, L"pw"), "plain text came back as '%ls'\n", Out);
    ok_long(Chars, 3);

    lstrcpyW(Bad, L"@@DAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    Chars = ARRAYSIZE(Out);
    ok(pUnprotectW(FALSE, Bad, lstrlenW(Bad) + 1, Out, &Chars), "unprotect of a fake marker failed\n");
    ok(!wcscmp(Out, Bad), "a fake marker came back as '%ls'\n", Out);
    ok_long(Chars, lstrlenW(Bad) + 1);

    if (Protect(P3, Len + 1, Twice, ARRAYSIZE(Twice), &Chars))
    {
        Chars = ARRAYSIZE(Out);
        ok(pUnprotectW(FALSE, Twice, lstrlenW(Twice) + 1, Out, &Chars), "unprotect of a double protection failed\n");
        ok(!wcscmp(Out, P3), "one unprotect did not peel one layer\n");
    }
}

static VOID
ExpectType(_In_opt_ PCWSTR String, _In_ CRED_PROTECTION_TYPE Expected, _In_ PCSTR What)
{
    CRED_PROTECTION_TYPE Type = 99;

    ok(pIsProtectedW((LPWSTR)String, &Type), "CredIsProtectedW(%s) failed: %lu\n", What, GetLastError());
    ok(Type == Expected, "%s: type %d, expected %d\n", What, Type, Expected);
}

static VOID
TestIsProtected(void)
{
    WCHAR P3[2048];
    CRED_PROTECTION_TYPE Type;
    DWORD Chars;

    if (Protect(L"pw", 3, P3, ARRAYSIZE(P3), &Chars))
        ExpectType(P3, CredUserProtection, "protected text");
    ExpectType(L"pw", CredUnprotected, "plain text");
    ExpectType(L"", CredUnprotected, "an empty string");
    ExpectType(L"@@DAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", CredUnprotected, "a fake marker");
    ExpectType(L"@@D", CredUnprotected, "a bare marker");
    ExpectType(L"@@Cxyz", CredUnprotected, "@@C");
    ExpectType(L"@@AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", CredUnprotected, "a marshaled certificate");

    SetLastError(0xdeadbeef);
    ok(!pIsProtectedW(NULL, &Type), "CredIsProtectedW(NULL) succeeded\n");
    ok_err(ERROR_INVALID_PARAMETER);
}

/* advapi32's cap (marker + encoded largest blob) is 87560 characters. */
#define OVER_CAP_CHARS 87561

static VOID
ExpectVerbatim(_In_ LPWSTR String, _In_ PCSTR What)
{
    DWORD Cch = lstrlenW(String) + 1, Chars = Cch;
    LPWSTR Out = HeapAlloc(GetProcessHeap(), 0, Cch * sizeof(WCHAR));

    ExpectType(String, CredUnprotected, What);
    if (!Out)
        return;
    ok(pUnprotectW(FALSE, String, Cch, Out, &Chars), "unprotect of %s failed: %lu\n", What, GetLastError());
    ok(!wcscmp(Out, String), "%s did not come back unchanged\n", What);
    ok(Chars == Cch, "%s: count %lu, expected %lu\n", What, Chars, Cch);
    HeapFree(GetProcessHeap(), 0, Out);
}

static VOID
TestCorrupted(void)
{
    WCHAR P[2048], C[2048], Out[2048];
    LPWSTR Big;
    DWORD Chars, Len, i;

    if (!Protect(L"pw", 3, P, ARRAYSIZE(P), &Chars))
        return;
    Len = lstrlenW(P);

    lstrcpyW(C, P);
    C[20] = (C[20] == L'A') ? L'B' : L'A';
    ExpectType(C, CredUserProtection, "a flipped payload character");
    Chars = ARRAYSIZE(Out);
    SetLastError(0xdeadbeef);
    ok(!pUnprotectW(FALSE, C, Len + 1, Out, &Chars), "unprotect of a flipped payload character succeeded\n");

    lstrcpyW(C, P);
    C[20] = L'!';
    ExpectVerbatim(C, "a character outside the alphabet");
    lstrcpyW(C, P);
    C[Len - 1] = 0;
    ExpectVerbatim(C, "text truncated by one");
    lstrcpyW(C, P);
    C[Len - 4] = 0;
    ExpectVerbatim(C, "text truncated by four");
    lstrcpyW(C, P);
    lstrcatW(C, L"AAAA");
    ExpectVerbatim(C, "text with four extra characters");
    lstrcpyW(C, P);
    C[3] = L'X';
    ExpectVerbatim(C, "a broken marker");

    Big = HeapAlloc(GetProcessHeap(), 0, (OVER_CAP_CHARS + 1) * sizeof(WCHAR));
    if (Big)
    {
        memcpy(Big, Prefix, sizeof(Prefix));
        for (i = ARRAYSIZE(Prefix); i < OVER_CAP_CHARS; i++)
            Big[i] = L'A';
        Big[OVER_CAP_CHARS] = UNICODE_NULL;
        ExpectVerbatim(Big, "text one character over the protected-length cap");
        HeapFree(GetProcessHeap(), 0, Big);
    }

    Chars = ARRAYSIZE(Out);
    ok(pUnprotectW(FALSE, P, 10, Out, &Chars), "unprotect with a count of ten failed\n");
    ok(!wcscmp(Out, L"pw"), "a count of ten changed the result: '%ls'\n", Out);
    Chars = ARRAYSIZE(Out);
    ok(pUnprotectW(FALSE, P, 0, Out, &Chars), "unprotect with a zero count failed\n");
    ok(!wcscmp(Out, L"pw"), "a zero count changed the result: '%ls'\n", Out);
    Chars = ARRAYSIZE(Out);
    ok(pUnprotectW(FALSE, P, 1, Out, &Chars), "unprotect with a count of one failed\n");
    ok(!wcscmp(Out, L"pw"), "a count of one changed the result: '%ls'\n", Out);
}

static VOID
TestProtectA(void)
{
    CHAR Out[2048];
    DWORD Chars = sizeof(Out);
    CRED_PROTECTION_TYPE Type = 99;

    ok(pProtectA(FALSE, "pw", 3, Out, &Chars, &Type), "CredProtectA failed: %lu\n", GetLastError());
    ok_long(Type, CredUserProtection);
    ok(!strncmp(Out, "@@D", 3), "CredProtectA text does not start with the marker\n");
    ok(lstrlenA(Out) > (int)ARRAYSIZE(Prefix), "CredProtectA text is too short\n");
    ok_long(Chars, lstrlenA(Out) + 1);
}

START_TEST(CredProtect)
{
    HMODULE Advapi = GetModuleHandleW(L"advapi32.dll");

    pProtectW = (PFN_PROTECTW)GetProcAddress(Advapi, "CredProtectW");
    pProtectA = (PFN_PROTECTA)GetProcAddress(Advapi, "CredProtectA");
    pUnprotectW = (PFN_UNPROTECTW)GetProcAddress(Advapi, "CredUnprotectW");
    pIsProtectedW = (PFN_ISPROTECTEDW)GetProcAddress(Advapi, "CredIsProtectedW");
    if (!pProtectW || !pProtectA || !pUnprotectW || !pIsProtectedW)
    {
        skip("the CredProtect functions are missing\n");
        return;
    }
    TestProtect();
    TestUnprotect();
    TestIsProtected();
    TestCorrupted();
    TestProtectA();
}
