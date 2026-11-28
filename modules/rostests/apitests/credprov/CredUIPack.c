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

    ok(String->Length == Bytes, "%s Length %u, expected %lu\n", What, String->Length, (ULONG)Bytes);
    ok(String->MaximumLength == (Expected ? Bytes + sizeof(WCHAR) : 0), "%s MaximumLength %u\n", What,
       String->MaximumLength);
    ok((ULONG_PTR)String->Buffer == Offset, "%s offset %lu, expected %lu\n", What, (ULONG)(ULONG_PTR)String->Buffer, (ULONG)Offset);
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

    ok(String->Length == Bytes, "%s Length %u, expected %lu\n", What, String->Length, (ULONG)Bytes);
    ok(String->MaximumLength == Bytes, "%s MaximumLength %u\n", What, String->MaximumLength);
    ok((ULONG_PTR)String->Buffer == Offset, "%s offset %lu, expected %lu\n", What, (ULONG)(ULONG_PTR)String->Buffer, (ULONG)Offset);
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

static LPWSTR
Repeat(_In_ WCHAR Char, _In_ DWORD Count)
{
    LPWSTR String = HeapAlloc(GetProcessHeap(), 0, (Count + 1) * sizeof(WCHAR));
    DWORD i;

    if (!String)
        return NULL;
    for (i = 0; i < Count; i++)
        String[i] = Char;
    String[Count] = UNICODE_NULL;
    return String;
}

static VOID
ExpectCap(_In_ DWORD Flags, _In_ DWORD UserChars, _In_ DWORD PasswordChars, _In_ DWORD Error)
{
    static BYTE Blob[8192];
    LPWSTR User = Repeat(L'u', UserChars), Password = Repeat(L'p', PasswordChars);
    DWORD Size;
    BOOL Ret;

    if (!User || !Password)
    {
        skip("out of memory\n");
        goto Done;
    }
    Size = 0;
    SetLastError(0xdeadbeef);
    ok(!pPack(Flags, User, Password, NULL, &Size), "size query for %lu/%lu succeeded\n", UserChars, PasswordChars);
    if (Error == ERROR_SUCCESS)
    {
        ok_err(ERROR_INSUFFICIENT_BUFFER);
        ok(Size != 0, "size query for %lu/%lu returned 0\n", UserChars, PasswordChars);
    }
    else
    {
        ok(GetLastError() == Error, "size query for %lu/%lu: error %lu, expected %lu\n", UserChars, PasswordChars,
           GetLastError(), Error);
        ok_long(Size, 0);
    }
    Size = sizeof(Blob);
    SetLastError(0xdeadbeef);
    Ret = pPack(Flags, User, Password, Blob, &Size);
    if (Error == ERROR_SUCCESS)
        ok(Ret, "pack of %lu/%lu failed: %lu\n", UserChars, PasswordChars, GetLastError());
    else
        ok(!Ret && GetLastError() == Error, "pack of %lu/%lu: %d, error %lu, expected %lu\n", UserChars,
           PasswordChars, Ret, GetLastError(), Error);

Done:
    HeapFree(GetProcessHeap(), 0, User);
    HeapFree(GetProcessHeap(), 0, Password);
}

static VOID
TestCaps(void)
{
    ExpectCap(CRED_PACK_GENERIC_CREDENTIALS, 513, 2, ERROR_SUCCESS);
    ExpectCap(CRED_PACK_GENERIC_CREDENTIALS, 514, 2, ERROR_NOT_SUPPORTED);
    ExpectCap(CRED_PACK_GENERIC_CREDENTIALS, 4, 256, ERROR_SUCCESS);
    ExpectCap(CRED_PACK_GENERIC_CREDENTIALS, 4, 257, ERROR_NOT_SUPPORTED);
    ExpectCap(0, 513, 2, ERROR_SUCCESS);
    ExpectCap(0, 4, 1281, ERROR_SUCCESS);
    ExpectCap(CRED_PACK_PROTECTED_CREDENTIALS, 4, 256, ERROR_SUCCESS);
}

static VOID
TestKerbUserCap(void)
{
    LPWSTR User = Repeat(L'u', 514);
    DWORD Size = 0;

    if (!User)
        return;
    SetLastError(0xdeadbeef);
    ok(!pPack(0, User, L"pw", NULL, &Size), "size query for a 514-character user succeeded\n");
    ok_err(ERROR_NO_SYSTEM_RESOURCES);
    ok_long(Size, 0);
    HeapFree(GetProcessHeap(), 0, User);
}

static VOID
TestProtected(void)
{
    BYTE Blob[4096], Again[8192];
    const KERB_INTERACTIVE_UNLOCK_LOGON *Packed = (const KERB_INTERACTIVE_UNLOCK_LOGON *)Blob;
    WCHAR User[64], Domain[64], Password[2048];
    DWORD Size, AgainSize, cchUser, cchDomain, cchPassword;
    CRED_PROTECTION_TYPE Type;
    const WCHAR *Text;

    Size = sizeof(Blob);
    if (!pPack(CRED_PACK_PROTECTED_CREDENTIALS, L"user", L"pw", Blob, &Size))
    {
        ok(0, "protected pack failed: %lu\n", GetLastError());
        return;
    }
    ok_long(Packed->Logon.MessageType, KerbInteractiveLogon);
    CheckString(Blob, &Packed->Logon.UserName, L"user", HEADER, "protected user");
    ok((ULONG_PTR)Packed->Logon.Password.Buffer == HEADER + 10, "protected password offset %lu\n",
       (ULONG)(ULONG_PTR)Packed->Logon.Password.Buffer);
    ok(Packed->Logon.Password.MaximumLength == Packed->Logon.Password.Length + sizeof(WCHAR),
       "protected password MaximumLength %u, Length %u\n", Packed->Logon.Password.MaximumLength,
       Packed->Logon.Password.Length);
    ok_long(Size, HEADER + 10 + Packed->Logon.Password.MaximumLength);
    Text = (const WCHAR *)(Blob + HEADER + 10);
    ok(Packed->Logon.Password.Length > 16 && !wcsncmp(Text, L"@@D", 3), "the password was not protected\n");

    cchUser = cchDomain = 64;
    cchPassword = ARRAYSIZE(Password);
    ok(pUnPack(0, Blob, Size, User, &cchUser, Domain, &cchDomain, Password, &cchPassword),
       "unpack without the protected flag failed: %lu\n", GetLastError());
    ok(CredIsProtectedW(Password, &Type) && Type == CredUserProtection, "flag 0 did not return protected text\n");
    ok_long(cchPassword, Packed->Logon.Password.Length / sizeof(WCHAR) + 1);

    cchUser = cchDomain = 64;
    cchPassword = ARRAYSIZE(Password);
    ok(pUnPack(CRED_PACK_PROTECTED_CREDENTIALS, Blob, Size, User, &cchUser, Domain, &cchDomain, Password,
               &cchPassword), "protected unpack failed: %lu\n", GetLastError());
    ok(!wcscmp(User, L"user"), "user '%ls'\n", User);
    ok(!wcscmp(Password, L"pw"), "password '%ls'\n", Password);
    ok_long(cchUser, 6);
    ok_long(cchPassword, 3);

    cchUser = cchDomain = 64;
    cchPassword = 2;
    SetLastError(0xdeadbeef);
    ok(!pUnPack(CRED_PACK_PROTECTED_CREDENTIALS, Blob, Size, User, &cchUser, Domain, &cchDomain, Password,
                &cchPassword), "protected unpack into a short password buffer succeeded\n");
    ok_err(ERROR_INSUFFICIENT_BUFFER);
    ok_long(cchPassword, 3);

    cchUser = cchDomain = 64;
    cchPassword = ARRAYSIZE(Password);
    ok(pUnPack(0, Blob, Size, User, &cchUser, Domain, &cchDomain, Password, &cchPassword), "unpack failed\n");
    AgainSize = sizeof(Again);
    ok(pPack(CRED_PACK_PROTECTED_CREDENTIALS, L"user", Password, Again, &AgainSize),
       "protected pack of protected text failed: %lu\n", GetLastError());
    ok(AgainSize > Size, "protected text was not protected again (%lu, %lu)\n", AgainSize, Size);
    cchUser = cchDomain = 64;
    cchPassword = ARRAYSIZE(Password);
    ok(pUnPack(CRED_PACK_PROTECTED_CREDENTIALS, Again, AgainSize, User, &cchUser, Domain, &cchDomain, Password,
               &cchPassword), "protected unpack of a double protection failed\n");
    ok(CredIsProtectedW(Password, &Type) && Type == CredUserProtection, "one unpack peeled more than one layer\n");

    Size = sizeof(Blob);
    ok(pPack(0, L"user", L"pw", Blob, &Size), "pack failed\n");
    cchUser = cchDomain = 64;
    cchPassword = ARRAYSIZE(Password);
    ok(pUnPack(CRED_PACK_PROTECTED_CREDENTIALS, Blob, Size, User, &cchUser, Domain, &cchDomain, Password,
               &cchPassword), "protected unpack of a plain password failed\n");
    ok(!wcscmp(Password, L"pw"), "password '%ls'\n", Password);
    ok_long(cchPassword, 3);
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
    TestCaps();
    TestKerbUserCap();
    TestProtected();
}
