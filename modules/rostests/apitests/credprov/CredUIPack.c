/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests CredPackAuthenticationBufferW and CredUnPackAuthenticationBufferW
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <ntsecapi.h>
#include <wincred.h>

typedef BOOL (WINAPI *PFN_PACK)(DWORD, LPWSTR, LPWSTR, PBYTE, DWORD *);
typedef BOOL (WINAPI *PFN_UNPACK)(DWORD, PVOID, DWORD, LPWSTR, DWORD *, LPWSTR, DWORD *, LPWSTR, DWORD *);

static PFN_PACK pPack;
static PFN_UNPACK pUnPack;

#define HEADER sizeof(KERB_INTERACTIVE_UNLOCK_LOGON)

static VOID
CheckString(_In_ const BYTE *Blob, _In_ const UNICODE_STRING *String, _In_opt_ PCWSTR Expected,
            _In_ ULONG_PTR Offset, _In_ PCSTR What)
{
    SIZE_T Bytes = Expected ? wcslen(Expected) * sizeof(WCHAR) : 0;

    ok(String->Length == Bytes, "%s Length %u, expected %Iu\n", What, String->Length, Bytes);
    ok(String->MaximumLength == (Expected ? Bytes + sizeof(WCHAR) : 0), "%s MaximumLength %u\n", What,
       String->MaximumLength);
    ok((ULONG_PTR)String->Buffer == Offset, "%s offset %Iu, expected %Iu\n", What, (ULONG_PTR)String->Buffer, Offset);
    if (Expected && (ULONG_PTR)String->Buffer == Offset)
    {
        ok(!memcmp(Blob + Offset, Expected, Bytes), "%s text mismatch\n", What);
        ok(!*(const WCHAR *)(Blob + Offset + Bytes), "%s is not NUL-terminated\n", What);
    }
}

static VOID
TestPack(void)
{
    BYTE Blob[512];
    const KERB_INTERACTIVE_UNLOCK_LOGON *Packed = (const KERB_INTERACTIVE_UNLOCK_LOGON *)Blob;
    DWORD Size;

    Size = 0;
    SetLastError(0xdeadbeef);
    ok(!pPack(0, L"user", L"pw", NULL, &Size), "pack without a buffer succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_long(Size, HEADER + 10 + 6);

    Size = HEADER + 15;
    SetLastError(0xdeadbeef);
    ok(!pPack(0, L"user", L"pw", Blob, &Size), "pack into a short buffer succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_long(Size, HEADER + 16);

    Size = sizeof(Blob);
    ok(pPack(0, L"user", L"pw", Blob, &Size), "pack failed: %lu\n", GetLastError());
    ok_long(Size, HEADER + 16);
    ok_long(Packed->Logon.MessageType, KerbInteractiveLogon);
    CheckString(Blob, &Packed->Logon.LogonDomainName, NULL, 0, "domain");
    CheckString(Blob, &Packed->Logon.UserName, L"user", HEADER, "user");
    CheckString(Blob, &Packed->Logon.Password, L"pw", HEADER + 10, "password");

    Size = sizeof(Blob);
    ok(pPack(0, L"DOM\\user", L"pw", Blob, &Size), "pack failed: %lu\n", GetLastError());
    ok_long(Size, HEADER + 24);
    CheckString(Blob, &Packed->Logon.LogonDomainName, L"DOM", HEADER, "split domain");
    CheckString(Blob, &Packed->Logon.UserName, L"user", HEADER + 8, "split user");
    CheckString(Blob, &Packed->Logon.Password, L"pw", HEADER + 18, "split password");

    Size = sizeof(Blob);
    ok(pPack(0, L"user@dom.example", L"pw", Blob, &Size), "pack failed: %lu\n", GetLastError());
    ok_long(Size, HEADER + 34 + 6);
    CheckString(Blob, &Packed->Logon.LogonDomainName, NULL, 0, "UPN domain");
    CheckString(Blob, &Packed->Logon.UserName, L"user@dom.example", HEADER, "UPN user");

    Size = sizeof(Blob);
    ok(pPack(0, L"user", L"", Blob, &Size), "pack failed: %lu\n", GetLastError());
    ok_long(Size, HEADER + 10 + 2);
    CheckString(Blob, &Packed->Logon.Password, L"", HEADER + 10, "empty password");

    Size = sizeof(Blob);
    ok(pPack(0x40, L"user", L"pw", Blob, &Size), "pack with an unknown flag failed: %lu\n", GetLastError());
    ok_long(Size, HEADER + 16);
}

static VOID
TestUnPack(void)
{
    BYTE Blob[512];
    WCHAR User[64], Domain[64], Password[64];
    DWORD Size, cchUser, cchDomain, cchPassword;

    Size = sizeof(Blob);
    ok(pPack(0, L"user", L"pw", Blob, &Size), "pack failed\n");
    cchUser = cchDomain = cchPassword = 64;
    ok(pUnPack(0, Blob, Size, User, &cchUser, Domain, &cchDomain, Password, &cchPassword),
       "unpack failed: %lu\n", GetLastError());
    ok(!wcscmp(User, L"user"), "user '%ls'\n", User);
    ok(!wcscmp(Password, L"pw"), "password '%ls'\n", Password);
    ok_long(cchUser, 6);
    ok_long(cchDomain, 64);
    ok_long(cchPassword, 3);

    cchUser = cchPassword = 64;
    ok(pUnPack(0, Blob, Size, User, &cchUser, NULL, NULL, Password, &cchPassword), "unpack without a domain failed\n");
    ok(!wcscmp(User, L"user"), "user '%ls'\n", User);

    cchUser = 2;
    cchDomain = cchPassword = 64;
    SetLastError(0xdeadbeef);
    ok(!pUnPack(0, Blob, Size, User, &cchUser, Domain, &cchDomain, Password, &cchPassword),
       "unpack into a short user buffer succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_long(cchUser, 6);
    ok_long(cchDomain, 0);
    ok_long(cchPassword, 3);

    Size = sizeof(Blob);
    ok(pPack(0, L"DOM\\user", L"", Blob, &Size), "pack failed\n");
    cchUser = cchDomain = cchPassword = 64;
    ok(pUnPack(0, Blob, Size, User, &cchUser, Domain, &cchDomain, Password, &cchPassword), "unpack failed\n");
    ok(!wcscmp(User, L"DOM\\user"), "user '%ls'\n", User);
    ok(!wcscmp(Password, L""), "password '%ls'\n", Password);
    ok_long(cchUser, 9);
    ok_long(cchDomain, 64);
    ok_long(cchPassword, 1);
}

typedef struct _GENERIC_PACKED
{
    ULONG Reserved;
    GUID Type;
    UNICODE_STRING UserName;
    UNICODE_STRING Password;
} GENERIC_PACKED;

#define GHDR sizeof(GENERIC_PACKED)

static const GUID GenericType = {0x2a818d91, 0x248e, 0x4af7, {0xb2, 0x0f, 0x09, 0xf6, 0xc1, 0xfd, 0x80, 0x76}};

static VOID
CheckRaw(_In_ const BYTE *Blob, _In_ const UNICODE_STRING *String, _In_ PCWSTR Expected, _In_ ULONG_PTR Offset,
         _In_ PCSTR What)
{
    SIZE_T Bytes = wcslen(Expected) * sizeof(WCHAR);

    ok(String->Length == Bytes, "%s Length %u, expected %Iu\n", What, String->Length, Bytes);
    ok(String->MaximumLength == Bytes, "%s MaximumLength %u\n", What, String->MaximumLength);
    ok((ULONG_PTR)String->Buffer == Offset, "%s offset %Iu, expected %Iu\n", What, (ULONG_PTR)String->Buffer, Offset);
    if (Bytes && (ULONG_PTR)String->Buffer == Offset)
        ok(!memcmp(Blob + Offset, Expected, Bytes), "%s text mismatch\n", What);
}

static VOID
TestPackGeneric(void)
{
    BYTE Blob[512], Other[512];
    const GENERIC_PACKED *Packed = (const GENERIC_PACKED *)Blob;
    DWORD Size;

    Size = 0;
    SetLastError(0xdeadbeef);
    ok(!pPack(CRED_PACK_GENERIC_CREDENTIALS, L"user", L"pw", NULL, &Size), "generic size query succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_long(Size, GHDR + 12);

    Size = GHDR + 11;
    SetLastError(0xdeadbeef);
    ok(!pPack(CRED_PACK_GENERIC_CREDENTIALS, L"user", L"pw", Blob, &Size), "generic pack into a short buffer succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_long(Size, GHDR + 12);

    memset(Blob, 0xcc, sizeof(Blob));
    Size = sizeof(Blob);
    ok(pPack(CRED_PACK_GENERIC_CREDENTIALS, L"user", L"pw", Blob, &Size), "generic pack failed: %lu\n", GetLastError());
    ok_long(Size, sizeof(Blob));
    ok_long(Packed->Reserved, 0);
    ok(IsEqualGUID(&Packed->Type, &GenericType), "generic type GUID mismatch\n");
    CheckRaw(Blob, &Packed->UserName, L"user", GHDR, "generic user");
    CheckRaw(Blob, &Packed->Password, L"pw", GHDR + 8, "generic password");

    memset(Other, 0xcc, sizeof(Other));
    Size = GHDR + 12;
    ok(pPack(CRED_PACK_GENERIC_CREDENTIALS | CRED_PACK_PROTECTED_CREDENTIALS, L"user", L"pw", Other, &Size),
       "generic protected pack failed: %lu\n", GetLastError());
    ok(!memcmp(Blob, Other, GHDR + 12), "the protected flag changed a generic buffer\n");

    Size = sizeof(Blob);
    ok(pPack(CRED_PACK_GENERIC_CREDENTIALS, L"DOM\\user", L"pw", Blob, &Size), "generic pack failed\n");
    CheckRaw(Blob, &Packed->UserName, L"DOM\\user", GHDR, "generic domain user");
    CheckRaw(Blob, &Packed->Password, L"pw", GHDR + 16, "generic domain password");

    Size = 0;
    pPack(CRED_PACK_GENERIC_CREDENTIALS, L"user", L"", NULL, &Size);
    ok_long(Size, GHDR + 8);
    Size = sizeof(Blob);
    ok(pPack(CRED_PACK_GENERIC_CREDENTIALS, L"user", L"", Blob, &Size), "generic pack failed\n");
    CheckRaw(Blob, &Packed->Password, L"", GHDR + 8, "generic empty password");

    Size = 0;
    pPack(CRED_PACK_GENERIC_CREDENTIALS, L"", L"pw", NULL, &Size);
    ok_long(Size, GHDR + 4);
    Size = sizeof(Blob);
    ok(pPack(CRED_PACK_GENERIC_CREDENTIALS, L"", L"pw", Blob, &Size), "generic pack failed\n");
    CheckRaw(Blob, &Packed->UserName, L"", GHDR, "generic empty user");
    CheckRaw(Blob, &Packed->Password, L"pw", GHDR, "generic password after an empty user");
}

static VOID
TestUnPackGeneric(void)
{
    static const DWORD Flags[] = {0, CRED_PACK_PROTECTED_CREDENTIALS, CRED_PACK_GENERIC_CREDENTIALS};
    BYTE Blob[512];
    WCHAR User[64], Domain[64], Password[64];
    DWORD Size, cchUser, cchDomain, cchPassword, i;

    Size = sizeof(Blob);
    ok(pPack(CRED_PACK_GENERIC_CREDENTIALS, L"user", L"pw", Blob, &Size), "generic pack failed\n");
    for (i = 0; i < ARRAYSIZE(Flags); i++)
    {
        cchUser = cchDomain = cchPassword = 64;
        ok(pUnPack(Flags[i], Blob, GHDR + 12, User, &cchUser, Domain, &cchDomain, Password, &cchPassword),
           "generic unpack with flags %lx failed: %lu\n", Flags[i], GetLastError());
        ok(!wcscmp(User, L"user"), "user '%ls'\n", User);
        ok(!wcscmp(Password, L"pw"), "password '%ls'\n", Password);
        ok_long(cchUser, 5);
        ok_long(cchDomain, 0);
        ok_long(cchPassword, 3);
    }

    cchUser = 2;
    cchDomain = cchPassword = 64;
    SetLastError(0xdeadbeef);
    ok(!pUnPack(0, Blob, GHDR + 12, User, &cchUser, Domain, &cchDomain, Password, &cchPassword),
       "generic unpack into a short user buffer succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_long(cchUser, 5);
    ok_long(cchDomain, 0);
    ok_long(cchPassword, 3);

    cchUser = cchDomain = 64;
    cchPassword = 1;
    SetLastError(0xdeadbeef);
    ok(!pUnPack(0, Blob, GHDR + 12, User, &cchUser, Domain, &cchDomain, Password, &cchPassword),
       "generic unpack into a short password buffer succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_long(cchUser, 5);
    ok_long(cchPassword, 3);

    Size = sizeof(Blob);
    ok(pPack(CRED_PACK_GENERIC_CREDENTIALS, L"DOM\\user", L"pw", Blob, &Size), "generic pack failed\n");
    cchUser = cchDomain = cchPassword = 64;
    ok(pUnPack(0, Blob, GHDR + 20, User, &cchUser, Domain, &cchDomain, Password, &cchPassword),
       "generic unpack failed\n");
    ok(!wcscmp(User, L"DOM\\user"), "user '%ls'\n", User);
    ok_long(cchUser, 9);
    ok_long(cchDomain, 0);

    Size = sizeof(Blob);
    ok(pPack(CRED_PACK_GENERIC_CREDENTIALS, L"", L"pw", Blob, &Size), "generic pack failed\n");
    cchUser = 1;
    cchDomain = cchPassword = 64;
    ok(pUnPack(0, Blob, GHDR + 4, User, &cchUser, Domain, &cchDomain, Password, &cchPassword),
       "generic unpack of an empty user into one character failed: %lu\n", GetLastError());
    ok(!wcscmp(User, L""), "user '%ls'\n", User);
    ok_long(cchUser, 1);

    Size = sizeof(Blob);
    ok(pPack(0, L"user", L"pw", Blob, &Size), "pack failed\n");
    cchUser = cchDomain = cchPassword = 64;
    ok(pUnPack(CRED_PACK_GENERIC_CREDENTIALS, Blob, Size, User, &cchUser, Domain, &cchDomain, Password, &cchPassword),
       "unpack of a logon buffer with the generic flag failed\n");
    ok(!wcscmp(User, L"user"), "user '%ls'\n", User);
    ok_long(cchUser, 6);
    ok_long(cchDomain, 64);
}

START_TEST(CredUIPack)
{
    WCHAR Path[MAX_PATH];
    HMODULE CredUI;

    if (!GetEnvironmentVariableW(L"CREDPROV_TEST_CREDUI", Path, ARRAYSIZE(Path)))
        lstrcpyW(Path, L"credui.dll");
    CredUI = LoadLibraryW(Path);
    trace("testing %ls\n", Path);

    pPack = (PFN_PACK)GetProcAddress(CredUI, "CredPackAuthenticationBufferW");
    pUnPack = (PFN_UNPACK)GetProcAddress(CredUI, "CredUnPackAuthenticationBufferW");
    if (!pPack || !pUnPack)
    {
        skip("credui pack functions are missing\n");
        return;
    }
    TestPack();
    TestUnPack();
    TestPackGeneric();
    TestUnPackGeneric();
}
