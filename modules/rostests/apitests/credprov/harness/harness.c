/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Runs a credential provider serialization through LsaLogonUser as SYSTEM
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <stdio.h>
#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <sddl.h>
#include <lm.h>
#include <ntsecapi.h>
#include <credentialprovider.h>
#include <credser.h>

static HANDLE Com1 = INVALID_HANDLE_VALUE;

static VOID
Report(_In_ PCSTR Format, ...)
{
    char Line[512];
    va_list Args;
    DWORD Written;
    int Length;

    va_start(Args, Format);
    Length = _vsnprintf(Line, sizeof(Line) - 1, Format, Args);
    va_end(Args);
    if (Length < 0)
        Length = sizeof(Line) - 1;
    Line[Length] = 0;
    fputs(Line, stdout);
    if (Com1 != INVALID_HANDLE_VALUE)
        WriteFile(Com1, Line, (DWORD)Length, &Written, NULL);
}

static NTSTATUS
LogonWithBlob(_In_ HANDLE Lsa, _In_ ULONG Msv, _In_reads_bytes_(Size) const VOID *Blob, _In_ ULONG Size,
              _Out_writes_opt_(SidChars) LPWSTR SidText, _In_ DWORD SidChars)
{
    CREDSER_LOGON Logon;
    PMSV1_0_INTERACTIVE_LOGON MsvLogon = NULL;
    ULONG MsvSize = 0, ProfileSize = 0;
    PVOID Copy = NULL, Profile = NULL;
    LSA_STRING Origin;
    TOKEN_SOURCE Source;
    LUID LogonId;
    HANDLE Token = NULL;
    QUOTA_LIMITS Quotas;
    NTSTATUS Status, SubStatus = 0;
    BYTE Buffer[256];
    DWORD Length;
    LPWSTR String;

    Status = CredSerCapture(Blob, Size, &Copy);
    if (NT_SUCCESS(Status))
        Status = CredSerUnpackLogon(Copy, Size, &Logon);
    if (NT_SUCCESS(Status))
        Status = CredSerBuildMsv1_0Logon(&Logon, &MsvLogon, &MsvSize);
    CredSerFree(Copy, Size);
    if (!NT_SUCCESS(Status))
        return Status;

    Origin.Buffer = "credprov_harness";
    Origin.Length = (USHORT)strlen(Origin.Buffer);
    Origin.MaximumLength = Origin.Length + 1;
    CopyMemory(Source.SourceName, "CredProv", TOKEN_SOURCE_LENGTH);
    AllocateLocallyUniqueId(&Source.SourceIdentifier);

    Status = LsaLogonUser(Lsa, &Origin, Interactive, Msv, MsvLogon, MsvSize, NULL, &Source, &Profile, &ProfileSize,
                          &LogonId, &Token, &Quotas, &SubStatus);
    CredSerFree(MsvLogon, MsvSize);
    if (Profile)
        LsaFreeReturnBuffer(Profile);
    if (!NT_SUCCESS(Status))
        return Status;

    if (SidText && SidChars)
    {
        SidText[0] = UNICODE_NULL;
        if (GetTokenInformation(Token, TokenUser, Buffer, sizeof(Buffer), &Length) &&
            ConvertSidToStringSidW(((TOKEN_USER *)Buffer)->User.Sid, &String))
        {
            lstrcpynW(SidText, String, SidChars);
            LocalFree(String);
        }
    }
    CloseHandle(Token);
    return Status;
}

static NTSTATUS
SerializeWithProvider(_In_ PCWSTR User, _In_ PCWSTR Password, _Out_ PBYTE *Blob, _Out_ PULONG Size)
{
    ICredentialProvider *Provider;
    ICredentialProviderCredential *Credential;
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE Response;
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION Cs;
    CREDENTIAL_PROVIDER_STATUS_ICON Icon;
    LPWSTR Text = NULL;
    DWORD Count, Default;
    BOOL AutoLogon;
    HRESULT hr;

    *Blob = NULL;
    *Size = 0;
    hr = CoCreateInstance(&CLSID_PasswordCredentialProvider, NULL, CLSCTX_INPROC_SERVER, &IID_ICredentialProvider,
                          (void **)&Provider);
    if (FAILED(hr))
        return STATUS_NOT_FOUND;
    hr = ICredentialProvider_SetUsageScenario(Provider, CPUS_LOGON, 0);
    if (SUCCEEDED(hr))
        hr = ICredentialProvider_GetCredentialCount(Provider, &Count, &Default, &AutoLogon);
    if (SUCCEEDED(hr))
        hr = ICredentialProvider_GetCredentialAt(Provider, 0, &Credential);
    if (FAILED(hr))
    {
        ICredentialProvider_Release(Provider);
        return STATUS_UNSUCCESSFUL;
    }
    ICredentialProviderCredential_SetStringValue(Credential, 1, User);
    ICredentialProviderCredential_SetStringValue(Credential, 2, Password);
    ZeroMemory(&Cs, sizeof(Cs));
    hr = ICredentialProviderCredential_GetSerialization(Credential, &Response, &Cs, &Text, &Icon);
    CoTaskMemFree(Text);
    ICredentialProviderCredential_SetDeselected(Credential);
    ICredentialProviderCredential_Release(Credential);
    ICredentialProvider_Release(Provider);
    if (FAILED(hr) || Response != CPGSR_RETURN_CREDENTIAL_FINISHED || !Cs.rgbSerialization)
        return STATUS_UNSUCCESSFUL;
    *Blob = Cs.rgbSerialization;
    *Size = Cs.cbSerialization;
    return STATUS_SUCCESS;
}

static int
RunLogon(_In_ HANDLE Lsa, _In_ ULONG Msv, _In_ PCWSTR User, _In_ PCWSTR Password)
{
    WCHAR Sid[128];
    PBYTE Blob;
    ULONG Size;
    NTSTATUS Status;

    Status = SerializeWithProvider(User, Password, &Blob, &Size);
    if (!NT_SUCCESS(Status))
    {
        Report("HARNESS logon %ls serialize 0x%08lx\n", User, Status);
        return 2;
    }
    Status = LogonWithBlob(Lsa, Msv, Blob, Size, Sid, ARRAYSIZE(Sid));
    SecureZeroMemory(Blob, Size);
    CoTaskMemFree(Blob);
    Report("HARNESS logon %ls status 0x%08lx sid %ls\n", User, Status, NT_SUCCESS(Status) ? Sid : L"-");
    return 0;
}

static int
RunCorpus(_In_ HANDLE Lsa, _In_ ULONG Msv)
{
    UCHAR Valid[512], Blob[512];
    PKERB_INTERACTIVE_UNLOCK_LOGON Packed = (PKERB_INTERACTIVE_UNLOCK_LOGON)Blob;
    ULONG Size = sizeof(Valid), Cut, Rejected = 0, Accepted = 0, Seed = 0x1badb002, Round, i;
    NTSTATUS Status;

    Status = CredSerPackLogon(KerbInteractiveLogon, NULL, L"credprovtest", L"wrong-password", NULL, Valid, &Size);
    if (!NT_SUCCESS(Status))
        return 2;

    for (Cut = 0; Cut < Size; Cut++)
    {
        Status = LogonWithBlob(Lsa, Msv, Valid, Cut, NULL, 0);
        if (Status == STATUS_INVALID_PARAMETER)
            Rejected++;
        else
            Accepted++;
    }
    for (Round = 0; Round < 500; Round++)
    {
        CopyMemory(Blob, Valid, Size);
        for (i = 0; i < 4; i++)
        {
            Seed = Seed * 1103515245 + 12345;
            Blob[(Seed >> 8) % Size] = (UCHAR)(Seed >> 16);
        }
        Status = LogonWithBlob(Lsa, Msv, Blob, Size, NULL, 0);
        if (Status == STATUS_INVALID_PARAMETER)
            Rejected++;
        else if (NT_SUCCESS(Status))
            Report("HARNESS corpus round %lu logged on\n", Round);
    }
    CopyMemory(Blob, Valid, Size);
    Packed->Logon.UserName.Buffer = (PWSTR)(ULONG_PTR)0xFFFFFFF0;
    Status = LogonWithBlob(Lsa, Msv, Blob, Size, NULL, 0);
    if (Status == STATUS_INVALID_PARAMETER)
        Rejected++;
    else
        Accepted++;

    Report("HARNESS corpus truncation+offset rejected %lu accepted %lu\n", Rejected, Accepted);
    return Accepted ? 1 : 0;
}

static BOOL
LockoutDisabled(void)
{
    USER_MODALS_INFO_3 *Info = NULL;
    BOOL Ret;

    if (NetUserModalsGet(NULL, 3, (LPBYTE *)&Info) != NERR_Success)
        return FALSE;
    Ret = Info->usrmod3_lockout_threshold == 0;
    NetApiBufferFree(Info);
    return Ret;
}

int
wmain(int argc, WCHAR **argv)
{
    LSA_STRING Name, Package;
    LSA_OPERATIONAL_MODE Mode;
    HANDLE Lsa;
    ULONG Msv;
    NTSTATUS Status;
    int Ret = 2;

    Com1 = CreateFileW(L"\\\\.\\COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (argc < 2)
    {
        Report("usage: credprov_harness logon <user> <password> | corpus\n");
        return 2;
    }
    if (!LockoutDisabled())
    {
        Report("HARNESS refused: the account lockout threshold must be 0\n");
        return 3;
    }

    Name.Buffer = "credprov_harness";
    Name.Length = (USHORT)strlen(Name.Buffer);
    Name.MaximumLength = Name.Length + 1;
    Status = LsaRegisterLogonProcess(&Name, &Lsa, &Mode);
    if (!NT_SUCCESS(Status))
    {
        Report("HARNESS LsaRegisterLogonProcess 0x%08lx (needs SYSTEM)\n", Status);
        return 3;
    }
    Package.Buffer = MSV1_0_PACKAGE_NAME;
    Package.Length = (USHORT)strlen(Package.Buffer);
    Package.MaximumLength = Package.Length + 1;
    Status = LsaLookupAuthenticationPackage(Lsa, &Package, &Msv);
    if (NT_SUCCESS(Status))
    {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (!wcscmp(argv[1], L"logon") && argc >= 4)
            Ret = RunLogon(Lsa, Msv, argv[2], argv[3]);
        else if (!wcscmp(argv[1], L"corpus"))
            Ret = RunCorpus(Lsa, Msv);
        CoUninitialize();
        Status = LsaLookupAuthenticationPackage(Lsa, &Package, &Msv);
        Report("HARNESS lsa alive 0x%08lx\n", Status);
    }
    LsaDeregisterLogonProcess(Lsa);
    if (Com1 != INVALID_HANDLE_VALUE)
        CloseHandle(Com1);
    return Ret;
}
