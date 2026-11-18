/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test the process-wide COM security settings
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS
#include <apitest.h>
#include <objbase.h>

static HRESULT InitSecurity(LONG Count, SOLE_AUTHENTICATION_SERVICE *Services, DWORD AuthnLevel, DWORD ImpLevel,
                            DWORD Capabilities)
{
    return CoInitializeSecurity(NULL, Count, Services, NULL, AuthnLevel, ImpLevel, NULL, Capabilities, NULL);
}

static void FreeServices(SOLE_AUTHENTICATION_SERVICE *Services, DWORD Count)
{
    DWORD i;

    for (i = 0; i < Count; i++)
        CoTaskMemFree(Services[i].pPrincipalName);
    CoTaskMemFree(Services);
}

static BOOL GetProcessUser(WCHAR *Buffer, DWORD Length)
{
    WCHAR Name[256], Domain[256];
    DWORD NameLength = ARRAYSIZE(Name), DomainLength = ARRAYSIZE(Domain), Size = 0;
    TOKEN_USER *User;
    SID_NAME_USE Use;
    HANDLE Token;
    BOOL Ret = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
        return FALSE;
    GetTokenInformation(Token, TokenUser, NULL, 0, &Size);
    User = HeapAlloc(GetProcessHeap(), 0, Size);
    if (User && GetTokenInformation(Token, TokenUser, User, Size, &Size) &&
        LookupAccountSidW(NULL, User->User.Sid, Name, &NameLength, Domain, &DomainLength, &Use) &&
        DomainLength + NameLength + 2 <= Length)
    {
        wsprintfW(Buffer, L"%s\\%s", Domain, Name);
        Ret = TRUE;
    }
    HeapFree(GetProcessHeap(), 0, User);
    CloseHandle(Token);
    return Ret;
}

static void CheckNoServices(const char *Stage)
{
    SOLE_AUTHENTICATION_SERVICE *Services = (SOLE_AUTHENTICATION_SERVICE *)(ULONG_PTR)0xdeadbeef;
    DWORD Count = 0xdeadbeef;
    HRESULT hr;

    hr = CoQueryAuthenticationServices(&Count, &Services);
    ok(hr == S_OK, "%s: CoQueryAuthenticationServices returned %#lx\n", Stage, hr);
    ok(Count == 0, "%s: Count is %lu\n", Stage, Count);
    ok(Services == NULL, "%s: Services is %p\n", Stage, Services);
}

static void CheckOneService(const char *Stage, const WCHAR *Principal)
{
    SOLE_AUTHENTICATION_SERVICE *Services = NULL, *Again = NULL;
    DWORD Count = 0, AgainCount = 0;
    HRESULT hr;

    hr = CoQueryAuthenticationServices(&Count, &Services);
    ok(hr == S_OK, "%s: CoQueryAuthenticationServices returned %#lx\n", Stage, hr);
    ok(Count == 1, "%s: Count is %lu\n", Stage, Count);
    if (hr != S_OK || Count != 1 || !Services)
        return;
    ok(Services[0].dwAuthnSvc == RPC_C_AUTHN_WINNT, "%s: dwAuthnSvc is %lu\n", Stage, Services[0].dwAuthnSvc);
    ok(Services[0].dwAuthzSvc == RPC_C_AUTHZ_NONE, "%s: dwAuthzSvc is %lu\n", Stage, Services[0].dwAuthzSvc);
    ok(Services[0].hr == S_OK, "%s: hr is %#lx\n", Stage, Services[0].hr);
    ok(Services[0].pPrincipalName != NULL, "%s: pPrincipalName is NULL\n", Stage);
    if (Services[0].pPrincipalName)
        ok(!lstrcmpiW(Services[0].pPrincipalName, Principal), "%s: pPrincipalName is %ls, expected %ls\n", Stage,
           Services[0].pPrincipalName, Principal);

    hr = CoQueryAuthenticationServices(&AgainCount, &Again);
    ok(hr == S_OK, "%s: second CoQueryAuthenticationServices returned %#lx\n", Stage, hr);
    ok(Again != Services, "%s: both queries returned the same array %p\n", Stage, Services);
    if (hr == S_OK && Again)
    {
        ok(Again[0].pPrincipalName != Services[0].pPrincipalName, "%s: both queries share the principal name\n",
           Stage);
        FreeServices(Again, AgainCount);
    }
    FreeServices(Services, Count);
}

static void ChildNone(void)
{
    HRESULT hr;

    CheckNoServices("before CoInitializeEx");
    hr = InitSecurity(-1, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, EOAC_NONE);
    ok(hr == CO_E_NOTINITIALIZED, "CoInitializeSecurity before CoInitializeEx returned %#lx\n", hr);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    CheckNoServices("after CoInitializeEx");
    CoUninitialize();
}

static void ChildDefault(void)
{
    WCHAR User[600];
    HRESULT hr;

    if (!GetProcessUser(User, ARRAYSIZE(User)))
    {
        skip("Cannot get the process user, error %lu\n", GetLastError());
        return;
    }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = InitSecurity(-1, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, EOAC_NONE);
    ok(hr == S_OK, "First CoInitializeSecurity returned %#lx\n", hr);
    hr = InitSecurity(-1, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, EOAC_NONE);
    ok(hr == RPC_E_TOO_LATE, "Second CoInitializeSecurity returned %#lx\n", hr);
    CheckOneService("default", User);
    CoUninitialize();

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CheckNoServices("after the last CoUninitialize");
    hr = InitSecurity(-1, NULL, RPC_C_AUTHN_LEVEL_CONNECT, RPC_C_IMP_LEVEL_IMPERSONATE, EOAC_NONE);
    ok(hr == S_OK, "CoInitializeSecurity after the last CoUninitialize returned %#lx\n", hr);
    CheckOneService("reinitialized", User);
    CoUninitialize();
}

static void ChildList(void)
{
    SOLE_AUTHENTICATION_SERVICE Service = { RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, S_OK };
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = InitSecurity(1, &Service, RPC_C_AUTHN_LEVEL_PKT, RPC_C_IMP_LEVEL_IMPERSONATE, EOAC_NONE);
    ok(hr == S_OK, "CoInitializeSecurity with one service returned %#lx\n", hr);
    CheckOneService("list", L"");
    CoUninitialize();
}

static void ChildInvalid(void)
{
    SOLE_AUTHENTICATION_SERVICE Service = { RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, S_OK };
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = InitSecurity(-1, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY + 1, RPC_C_IMP_LEVEL_IDENTIFY, EOAC_NONE);
    ok(hr == E_INVALIDARG, "Authentication level 7 returned %#lx\n", hr);
    hr = InitSecurity(-1, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_DEFAULT, EOAC_NONE);
    ok(hr == E_INVALIDARG, "Impersonation level 0 returned %#lx\n", hr);
    hr = InitSecurity(-1, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_DELEGATE + 1, EOAC_NONE);
    ok(hr == E_INVALIDARG, "Impersonation level 5 returned %#lx\n", hr);
    hr = InitSecurity(0, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, EOAC_NONE);
    ok(hr == E_INVALIDARG, "No service returned %#lx\n", hr);
    hr = InitSecurity(1, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, EOAC_NONE);
    ok(hr == E_INVALIDARG, "One service without a list returned %#lx\n", hr);
    hr = InitSecurity(-2, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, EOAC_NONE);
    ok(hr == E_INVALIDARG, "Service count -2 returned %#lx\n", hr);
    hr = InitSecurity(-1, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, 0x80000000);
    ok(hr == E_INVALIDARG, "Unknown capability returned %#lx\n", hr);
    hr = CoInitializeSecurity(NULL, -1, NULL, (void *)1, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, NULL,
                              EOAC_NONE, NULL);
    ok(hr == E_INVALIDARG, "Non-NULL first reserved parameter returned %#lx\n", hr);
    CheckNoServices("after the rejected calls");

    hr = InitSecurity(-1, &Service, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, EOAC_NONE);
    ok(hr == S_OK, "Service count -1 with a list returned %#lx\n", hr);
    hr = InitSecurity(-1, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY + 1, RPC_C_IMP_LEVEL_IDENTIFY, EOAC_NONE);
    ok(hr == E_INVALIDARG, "Authentication level 7 after initialization returned %#lx\n", hr);
    CoUninitialize();
}

static void ChildImplicit(void)
{
    LARGE_INTEGER Zero = { { 0 } };
    WCHAR User[600];
    IStream *Stream;
    HRESULT hr;

    if (!GetProcessUser(User, ARRAYSIZE(User)))
    {
        skip("Cannot get the process user, error %lu\n", GetLastError());
        return;
    }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CreateStreamOnHGlobal(NULL, TRUE, &Stream);
    ok(hr == S_OK, "CreateStreamOnHGlobal returned %#lx\n", hr);
    if (hr != S_OK)
    {
        CoUninitialize();
        return;
    }
    hr = CoMarshalInterface(Stream, &IID_IUnknown, (IUnknown *)Stream, MSHCTX_INPROC, NULL, MSHLFLAGS_NORMAL);
    ok(hr == S_OK, "CoMarshalInterface returned %#lx\n", hr);
    CheckOneService("after marshaling", User);
    hr = InitSecurity(-1, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, EOAC_NONE);
    ok(hr == RPC_E_TOO_LATE, "CoInitializeSecurity after marshaling returned %#lx\n", hr);
    IStream_Seek(Stream, Zero, STREAM_SEEK_SET, NULL);
    CoReleaseMarshalData(Stream);
    IStream_Release(Stream);
    CoUninitialize();
}

static const struct
{
    const char *Name;
    void (*Run)(void);
} Children[] =
{
    { "none", ChildNone },
    { "default", ChildDefault },
    { "list", ChildList },
    { "invalid", ChildInvalid },
    { "implicit", ChildImplicit },
};

START_TEST(security)
{
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    char CommandLine[MAX_PATH + 64];
    DWORD ExitCode;
    char **Argv;
    int Argc;
    UINT i;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 4 && !strcmp(Argv[2], "child"))
    {
        for (i = 0; i < ARRAYSIZE(Children); i++)
        {
            if (!strcmp(Argv[3], Children[i].Name))
                Children[i].Run();
        }
        return;
    }

    for (i = 0; i < ARRAYSIZE(Children); i++)
    {
        snprintf(CommandLine, sizeof(CommandLine), "\"%s\" security child %s", Argv[0], Children[i].Name);
        if (!CreateProcessA(NULL, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
        {
            skip("CreateProcessA for %s failed with error %lu\n", Children[i].Name, GetLastError());
            continue;
        }
        ok(WaitForSingleObject(ProcessInfo.hProcess, 30000) == WAIT_OBJECT_0, "Child %s did not end\n",
           Children[i].Name);
        ok(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode) && ExitCode == 0, "Child %s had %lu failures\n",
           Children[i].Name, ExitCode);
        CloseHandle(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hProcess);
    }
}
