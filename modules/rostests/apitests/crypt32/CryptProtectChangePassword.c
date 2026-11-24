/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the DPAPI master-key re-key across a password change (Q8)
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <apitest.h>
#include <wincrypt.h>
#include <ntsecapi.h>
#include <ntstatus.h>
#include <userenv.h>
#include <sddl.h>
#include <lm.h>

static const BYTE ChangeSecret[] = { 'q', '8', 0, 'r', 'e', 'k', 'e', 'y', 0x42 };

/* The full cycle: protect under password 1, change the account password
 * through the MSV1_0 package (the path that triggers the lsasrv re-key),
 * then unprotect in the same session and after a re-logon. Skips unless the
 * harness provides a scratch account (DPAPI_CHANGE_USER and
 * DPAPI_CHANGE_PASSWORD) or allows the test to create one
 * (DPAPI_CHANGE_CREATE=1, needs an elevated token). The junction and
 * oversized-file cases of the C6 gate run in the lab lane only. */

static BOOL
GetEnv(_In_ PCWSTR Name, _Out_writes_(Count) PWSTR Value, _In_ DWORD Count)
{
    DWORD Length = GetEnvironmentVariableW(Name, Value, Count);

    return Length != 0 && Length < Count;
}

static BOOL
ProtectAndCompare(_In_ HANDLE Token, _Inout_ DATA_BLOB *Blob)
{
    DATA_BLOB In = { sizeof(ChangeSecret), (BYTE *)ChangeSecret }, Back = { 0 };
    BOOL Ok;

    if (Blob->pbData)
    {
        LocalFree(Blob->pbData);
        Blob->pbData = NULL;
        Blob->cbData = 0;
    }

    if (!ImpersonateLoggedOnUser(Token))
        return FALSE;
    Ok = CryptProtectData(&In, NULL, NULL, NULL, NULL, 0, Blob) &&
         CryptUnprotectData(Blob, NULL, NULL, NULL, NULL, 0, &Back) &&
         Back.cbData == sizeof(ChangeSecret) &&
         memcmp(Back.pbData, ChangeSecret, sizeof(ChangeSecret)) == 0;
    if (Back.pbData)
        LocalFree(Back.pbData);
    RevertToSelf();
    return Ok;
}

static BOOL
UnprotectCompare(_In_ HANDLE Token, _In_ DATA_BLOB *Blob)
{
    DATA_BLOB Back = { 0 };
    BOOL Ok;

    if (!ImpersonateLoggedOnUser(Token))
        return FALSE;
    Ok = CryptUnprotectData(Blob, NULL, NULL, NULL, NULL, 0, &Back) &&
         Back.cbData == sizeof(ChangeSecret) &&
         memcmp(Back.pbData, ChangeSecret, sizeof(ChangeSecret)) == 0;
    if (Back.pbData)
        LocalFree(Back.pbData);
    RevertToSelf();
    return Ok;
}

/* Change the password through the MSV1_0 call package, as the user: a flat
 * MSV1_0_CHANGEPASSWORD_REQUEST in one buffer, over an untrusted handle. */
static BOOL
ChangePasswordViaMsv1_0(_In_ PCWSTR Domain,
                        _In_ PCWSTR Account,
                        _In_ PCWSTR OldPassword,
                        _In_ PCWSTR NewPassword)
{
    PMSV1_0_CHANGEPASSWORD_REQUEST Request;
    UNICODE_STRING *Fields[4];
    PMSV1_0_CHANGEPASSWORD_RESPONSE Response = NULL;
    LPCWSTR Values[4];
    ULONG RequestSize, ResponseSize = 0, PackageId, Index;
    LSA_STRING PackageName;
    NTSTATUS Status, ProtocolStatus;
    HANDLE LsaHandle;
    PWSTR Ptr;
    BOOL Ok = FALSE;

    PackageName.Buffer = (PCHAR)MSV1_0_PACKAGE_NAME;
    PackageName.Length = sizeof(MSV1_0_PACKAGE_NAME) - 1;
    PackageName.MaximumLength = sizeof(MSV1_0_PACKAGE_NAME);

    Status = LsaConnectUntrusted(&LsaHandle);
    if (Status != STATUS_SUCCESS)
        return FALSE;

    if (LsaLookupAuthenticationPackage(LsaHandle, &PackageName, &PackageId) != STATUS_SUCCESS)
    {
        LsaClose(LsaHandle);
        return FALSE;
    }

    Values[0] = Domain;
    Values[1] = Account;
    Values[2] = OldPassword;
    Values[3] = NewPassword;

    RequestSize = sizeof(*Request);
    for (Index = 0; Index < 4; Index++)
        RequestSize += (ULONG)((wcslen(Values[Index]) + 1) * sizeof(WCHAR));

    Request = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, RequestSize);
    if (Request == NULL)
    {
        LsaClose(LsaHandle);
        return FALSE;
    }

    Request->MessageType = MsV1_0ChangePassword;
    Request->Impersonating = TRUE;

    Fields[0] = &Request->DomainName;
    Fields[1] = &Request->AccountName;
    Fields[2] = &Request->OldPassword;
    Fields[3] = &Request->NewPassword;

    Ptr = (PWSTR)((PBYTE)Request + sizeof(*Request));
    for (Index = 0; Index < 4; Index++)
    {
        Fields[Index]->Length = (USHORT)(wcslen(Values[Index]) * sizeof(WCHAR));
        Fields[Index]->MaximumLength = Fields[Index]->Length + sizeof(WCHAR);
        Fields[Index]->Buffer = Ptr;
        wcscpy(Ptr, Values[Index]);
        Ptr += Fields[Index]->MaximumLength / sizeof(WCHAR);
    }

    Status = LsaCallAuthenticationPackage(LsaHandle,
                                          PackageId,
                                          Request,
                                          RequestSize,
                                          (PVOID *)&Response,
                                          &ResponseSize,
                                          &ProtocolStatus);
    Ok = Status == STATUS_SUCCESS && ProtocolStatus == STATUS_SUCCESS;

    HeapFree(GetProcessHeap(), 0, Request);
    if (Response != NULL)
        LsaFreeReturnBuffer(Response);
    LsaClose(LsaHandle);
    return Ok;
}

/* LoadUserProfileW needs SeRestorePrivilege and SeBackupPrivilege enabled. */
static BOOL
EnableProfilePrivileges(void)
{
    HANDLE Token;
    TOKEN_PRIVILEGES Privileges;
    LUID Luid;
    ULONG Index;
    static const PCWSTR Names[] = { L"SeRestorePrivilege", L"SeBackupPrivilege" };

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &Token))
        return FALSE;

    Privileges.PrivilegeCount = 0;
    for (Index = 0; Index < ARRAYSIZE(Names); Index++)
    {
        if (LookupPrivilegeValueW(NULL, Names[Index], &Luid))
        {
            Privileges.Privileges[Privileges.PrivilegeCount].Luid = Luid;
            Privileges.Privileges[Privileges.PrivilegeCount].Attributes = SE_PRIVILEGE_ENABLED;
            Privileges.PrivilegeCount++;
        }
    }

    if (Privileges.PrivilegeCount != ARRAYSIZE(Names) ||
        !AdjustTokenPrivileges(Token, TRUE, &Privileges, 0, NULL, NULL) ||
        GetLastError() != ERROR_SUCCESS)
    {
        CloseHandle(Token);
        return FALSE;
    }
    CloseHandle(Token);
    return TRUE;
}

static BOOL
MakeScratchAccount(_In_ PCWSTR Name, _In_ PCWSTR Password)
{
    USER_INFO_1 Info = { 0 };
    DWORD Error, BadParam;

    Info.usri1_name = (PWSTR)Name;
    Info.usri1_password = (PWSTR)Password;
    Info.usri1_priv = USER_PRIV_USER;
    Info.usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT;
    Error = NetUserAdd(NULL, 1, (PBYTE)&Info, &BadParam);
    return Error == NERR_Success || Error == NERR_UserExists;
}

static BOOL
DeleteScratchAccount(_In_ PCWSTR Name)
{
    return NetUserDel(NULL, (PWSTR)Name) == NERR_Success;
}

START_TEST(CryptProtectChangePassword)
{
    WCHAR Account[64], Password1[64], Password2[64];
    PROFILEINFOW Profile = { 0 };
    BOOL Created = FALSE, ProfileLoaded = FALSE;
    HANDLE Token1 = NULL, Token2 = NULL;
    LPWSTR SidString = NULL;
    DATA_BLOB Blob = { 0 };

    if (!GetEnv(L"DPAPI_CHANGE_USER", Account, ARRAYSIZE(Account)))
    {
        if (GetEnvironmentVariableW(L"DPAPI_CHANGE_CREATE", NULL, 0) == 0)
        {
            skip("no DPAPI_CHANGE_USER scratch account and DPAPI_CHANGE_CREATE not set\n");
            return;
        }
        wcscpy(Account, L"dpapiq8t");
        if (!MakeScratchAccount(Account, L"q8First#1"))
        {
            skip("NetUserAdd for the scratch account failed (not elevated?)\n");
            return;
        }
        Created = TRUE;
        wcscpy(Password1, L"q8First#1");
    }
    else if (!GetEnv(L"DPAPI_CHANGE_PASSWORD", Password1, ARRAYSIZE(Password1)))
    {
        skip("DPAPI_CHANGE_PASSWORD missing for the scratch account\n");
        return;
    }
    wcscpy(Password2, L"q8Second#2");

    /* Logon with password 1 and protect a blob (creates the master key). The
     * profile must be loaded or a fresh account has no ProfileList entry and
     * the protect fails as a test failure instead of exercising the re-key. */
    if (!LogonUserW(Account, L".", Password1, LOGON32_LOGON_INTERACTIVE,
                    LOGON32_PROVIDER_DEFAULT, &Token1))
    {
        skip("LogonUser(%ls) failed, error %lu\n", Account, GetLastError());
        goto done;
    }

    if (!EnableProfilePrivileges())
    {
        skip("SeRestore/SeBackup enable failed (not elevated?)\n");
        goto done;
    }

    Profile.dwSize = sizeof(Profile);
    Profile.dwFlags = PI_NOUI;
    Profile.lpUserName = Account;
    if (!LoadUserProfileW(Token1, &Profile))
    {
        skip("LoadUserProfileW failed, error %lu\n", GetLastError());
        goto done;
    }
    ProfileLoaded = TRUE;
    ok(Profile.hProfile != NULL, "LoadUserProfileW returned no profile handle\n");

    {
        BYTE TokenBuffer[256];
        TOKEN_USER *TokenInfo = (TOKEN_USER *)TokenBuffer;
        DWORD Size;

        if (GetTokenInformation(Token1, TokenUser, TokenBuffer, sizeof(TokenBuffer), &Size))
            ConvertSidToStringSidW(TokenInfo->User.Sid, &SidString);
    }

    ok(ProtectAndCompare(Token1, &Blob), "protect under password 1 failed, error %lu\n",
       GetLastError());

    /* Change the password through the package path (triggers the re-key). */
    ok(ChangePasswordViaMsv1_0(L"", Account, Password1, Password2),
       "MsV1_0ChangePassword failed\n");

    /* C4: the SAME session must still open the old blob (Primary refreshed). */
    ok(UnprotectCompare(Token1, &Blob), "same-session unprotect after change failed\n");

    /* Re-logon with the new password: the re-keyed file must open the blob. */
    if (LogonUserW(Account, L".", Password2, LOGON32_LOGON_INTERACTIVE,
                   LOGON32_PROVIDER_DEFAULT, &Token2))
    {
        ok(UnprotectCompare(Token2, &Blob), "re-logon unprotect after change failed\n");
        ok(ProtectAndCompare(Token2, &Blob), "protect under password 2 failed\n");
        ok(UnprotectCompare(Token2, &Blob), "password-2 blob did not round-trip\n");
    }
    else
    {
        ok(0, "LogonUser with the new password failed, error %lu\n", GetLastError());
    }

done:
    /* R4: DeleteProfileW only for an account the test created - a supplied
     * DPAPI_CHANGE_USER account keeps its profile and DPAPI keys. */
    if (ProfileLoaded)
    {
        UnloadUserProfile(Token1, Profile.hProfile);
        if (Created && SidString)
            DeleteProfileW(SidString, NULL, NULL);
    }
    if (SidString)
        LocalFree(SidString);
    if (Created)
        ok(DeleteScratchAccount(Account), "NetUserDel failed\n");
    else
    {
        /* Leave a supplied account on its original password; the restore
         * doubles as a second re-key round (result ignored). */
        ChangePasswordViaMsv1_0(L"", Account, Password2, Password1);
    }
    if (Blob.pbData)
        LocalFree(Blob.pbData);
    if (Token2)
        CloseHandle(Token2);
    if (Token1)
        CloseHandle(Token1);
}
