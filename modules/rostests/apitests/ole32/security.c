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

static void CheckNullServices(const char *Stage)
{
    DWORD Count = 0xdeadbeef;
    HRESULT hr;

    hr = CoQueryAuthenticationServices(&Count, NULL);
    ok(hr == E_INVALIDARG, "%s: CoQueryAuthenticationServices without a list returned %#lx\n", Stage, hr);
    ok(Count == 0xdeadbeef, "%s: Count without a list is %lu\n", Stage, Count);
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
    CheckNullServices(Stage);
}

static void CheckOneService(const char *Stage, const WCHAR *Principal)
{
    SOLE_AUTHENTICATION_SERVICE *Services = NULL, *Again = NULL;
    DWORD Count = 0, AgainCount = 0;
    HRESULT hr;

    hr = CoQueryAuthenticationServices(&Count, &Services);
    ok(hr == S_OK, "%s: CoQueryAuthenticationServices returned %#lx\n", Stage, hr);
    ok(Count == 1, "%s: Count is %lu\n", Stage, Count);
    CheckNullServices(Stage);
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

typedef struct
{
    HRESULT ContextHr;
    HRESULT BlanketHr;
    DWORD AuthnSvc, AuthzSvc, AuthnLevel, ImpLevel, Capabilities;
    OLECHAR *ServerName;
    WCHAR Privs[300];
    BOOL HasPrivs;
    BOOL ImpersonatingBefore;
    HRESULT ImpersonateHr;
    BOOL ImpersonatingDuring;
    BOOL GotToken;
    SECURITY_IMPERSONATION_LEVEL TokenLevel;
    BOOL SameUser;
    HRESULT RevertHr;
    BOOL TokenAfterRevert;
    DWORD TokenAfterRevertError;
    BOOL ImpersonatingAfter;
    HRESULT SecondRevertHr;
    HRESULT NullBlanketHr;
    IServerSecurity *Kept;
} CALL_RECORD;

static CALL_RECORD Calls[2];
static LONG CallCount;

static BOOL GetProcessSid(BYTE *Buffer, DWORD Length)
{
    HANDLE Token;
    DWORD Size;
    BOOL Ret;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
        return FALSE;
    Ret = GetTokenInformation(Token, TokenUser, Buffer, Length, &Size);
    CloseHandle(Token);
    return Ret;
}

static BOOL TokenUserEquals(HANDLE Token, BYTE *ProcessUser)
{
    BYTE ThreadUser[256];
    DWORD Size;

    return GetTokenInformation(Token, TokenUser, ThreadUser, sizeof(ThreadUser), &Size) &&
           EqualSid(((TOKEN_USER *)ThreadUser)->User.Sid, ((TOKEN_USER *)ProcessUser)->User.Sid);
}

static void RecordCall(CALL_RECORD *Record)
{
    IServerSecurity *Security = NULL;
    RPC_AUTHZ_HANDLE Privs = NULL;
    BYTE ProcessUser[256];
    BOOL HaveProcessUser;
    HANDLE Token;
    DWORD Size;

    HaveProcessUser = GetProcessSid(ProcessUser, sizeof(ProcessUser));
    Record->ContextHr = CoGetCallContext(&IID_IServerSecurity, (void **)&Security);
    Record->AuthnSvc = Record->AuthzSvc = Record->AuthnLevel = Record->ImpLevel = Record->Capabilities = 0xdead;
    Record->ServerName = (OLECHAR *)(ULONG_PTR)0xdead;
    Record->BlanketHr = CoQueryClientBlanket(&Record->AuthnSvc, &Record->AuthzSvc, &Record->ServerName,
                                             &Record->AuthnLevel, &Record->ImpLevel, &Privs, &Record->Capabilities);
    Record->HasPrivs = Privs != NULL;
    if (Privs)
        lstrcpynW(Record->Privs, Privs, ARRAYSIZE(Record->Privs));
    if (!Security)
        return;
    Record->ImpersonatingBefore = IServerSecurity_IsImpersonating(Security);
    Record->ImpersonateHr = CoImpersonateClient();
    Record->ImpersonatingDuring = IServerSecurity_IsImpersonating(Security);
    Record->GotToken = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &Token);
    if (Record->GotToken)
    {
        GetTokenInformation(Token, TokenImpersonationLevel, &Record->TokenLevel, sizeof(Record->TokenLevel), &Size);
        Record->SameUser = HaveProcessUser && TokenUserEquals(Token, ProcessUser);
        CloseHandle(Token);
    }
    Record->RevertHr = CoRevertToSelf();
    Record->TokenAfterRevert = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &Token);
    Record->TokenAfterRevertError = GetLastError();
    if (Record->TokenAfterRevert)
        CloseHandle(Token);
    Record->ImpersonatingAfter = IServerSecurity_IsImpersonating(Security);
    Record->SecondRevertHr = IServerSecurity_RevertToSelf(Security);
    Record->NullBlanketHr = IServerSecurity_QueryBlanket(Security, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    Record->Kept = Security;
}

static void CheckCall(const char *Stage, CALL_RECORD *Record, SECURITY_IMPERSONATION_LEVEL TokenLevel, BOOL CrossProcess)
{
    WCHAR User[300];
    DWORD Dummy;

    ok(Record->ContextHr == S_OK, "%s: CoGetCallContext returned 0x%lx\n", Stage, Record->ContextHr);
    ok(Record->BlanketHr == S_OK, "%s: CoQueryClientBlanket returned 0x%lx\n", Stage, Record->BlanketHr);
    ok(Record->AuthnSvc == RPC_C_AUTHN_WINNT, "%s: authn service %lu\n", Stage, Record->AuthnSvc);
    ok(Record->AuthzSvc == RPC_C_AUTHZ_NONE, "%s: authz service %lu\n", Stage, Record->AuthzSvc);
    ok(Record->ServerName == NULL, "%s: server name %p\n", Stage, Record->ServerName);
    ok(Record->AuthnLevel == RPC_C_AUTHN_LEVEL_PKT_PRIVACY, "%s: authn level %lu\n", Stage, Record->AuthnLevel);
    ok(Record->ImpLevel == RPC_C_IMP_LEVEL_ANONYMOUS, "%s: imp level %lu\n", Stage, Record->ImpLevel);
    ok(Record->Capabilities == EOAC_NONE, "%s: capabilities 0x%lx\n", Stage, Record->Capabilities);
    if (CrossProcess)
    {
        ok(Record->HasPrivs && GetProcessUser(User, ARRAYSIZE(User)) && !lstrcmpiW(Record->Privs, User),
           "%s: client name %s\n", Stage, wine_dbgstr_w(Record->HasPrivs ? Record->Privs : NULL));
    }
    else
    {
        ok(!Record->HasPrivs, "%s: client name %s\n", Stage, wine_dbgstr_w(Record->Privs));
    }
    ok(!Record->ImpersonatingBefore, "%s: impersonating before the call to CoImpersonateClient\n", Stage);
    ok(Record->ImpersonateHr == S_OK, "%s: CoImpersonateClient returned 0x%lx\n", Stage, Record->ImpersonateHr);
    ok(Record->ImpersonatingDuring, "%s: not impersonating\n", Stage);
    ok(Record->GotToken, "%s: no thread token while impersonating\n", Stage);
    ok(Record->TokenLevel == TokenLevel, "%s: token level %d\n", Stage, Record->TokenLevel);
    ok(Record->SameUser, "%s: thread token user is not the client user\n", Stage);
    ok(Record->RevertHr == S_OK, "%s: CoRevertToSelf returned 0x%lx\n", Stage, Record->RevertHr);
    ok(!Record->TokenAfterRevert && Record->TokenAfterRevertError == ERROR_NO_TOKEN,
       "%s: thread token after revert %d, error %lu\n", Stage, Record->TokenAfterRevert, Record->TokenAfterRevertError);
    ok(!Record->ImpersonatingAfter, "%s: still impersonating after revert\n", Stage);
    ok(Record->SecondRevertHr == S_OK, "%s: second RevertToSelf returned 0x%lx\n", Stage, Record->SecondRevertHr);
    ok(Record->NullBlanketHr == S_OK, "%s: QueryBlanket without outputs returned 0x%lx\n", Stage, Record->NullBlanketHr);
    if (Record->Kept)
    {
        ok(IServerSecurity_QueryBlanket(Record->Kept, &Dummy, NULL, NULL, NULL, NULL, NULL, NULL) == RPC_E_CALL_COMPLETE,
           "%s: QueryBlanket after the call did not fail\n", Stage);
        ok(IServerSecurity_ImpersonateClient(Record->Kept) == RPC_E_CALL_COMPLETE,
           "%s: ImpersonateClient after the call did not fail\n", Stage);
        ok(!IServerSecurity_IsImpersonating(Record->Kept), "%s: impersonating after the call\n", Stage);
        IServerSecurity_Release(Record->Kept);
        Record->Kept = NULL;
    }
}

static void CheckOutsideCall(void)
{
    DWORD AuthnSvc;
    void *Context;

    ok(CoGetCallContext(&IID_IServerSecurity, &Context) == RPC_E_CALL_COMPLETE, "CoGetCallContext did not fail\n");
    ok(CoQueryClientBlanket(&AuthnSvc, NULL, NULL, NULL, NULL, NULL, NULL) == RPC_E_CALL_COMPLETE,
       "CoQueryClientBlanket did not fail\n");
    ok(CoImpersonateClient() == RPC_E_CALL_COMPLETE, "CoImpersonateClient did not fail\n");
    ok(CoRevertToSelf() == RPC_E_CALL_COMPLETE, "CoRevertToSelf did not fail\n");
}

static HRESULT WINAPI Factory_QueryInterface(IClassFactory *Iface, REFIID Riid, void **Object)
{
    if (IsEqualIID(Riid, &IID_IUnknown) || IsEqualIID(Riid, &IID_IClassFactory))
    {
        *Object = Iface;
        return S_OK;
    }
    *Object = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Factory_AddRef(IClassFactory *Iface)
{
    return 2;
}

static ULONG WINAPI Factory_Release(IClassFactory *Iface)
{
    return 1;
}

static HRESULT WINAPI Factory_CreateInstance(IClassFactory *Iface, IUnknown *Outer, REFIID Riid, void **Object)
{
    LONG Index = CallCount;

    *Object = NULL;
    if (Index < ARRAYSIZE(Calls))
    {
        RecordCall(&Calls[Index]);
        CallCount = Index + 1;
    }
    return E_NOTIMPL;
}

static HRESULT WINAPI Factory_LockServer(IClassFactory *Iface, BOOL Lock)
{
    return S_OK;
}

static IClassFactoryVtbl FactoryVtbl =
{
    Factory_QueryInterface, Factory_AddRef, Factory_Release, Factory_CreateInstance, Factory_LockServer
};
static IClassFactory Factory = { &FactoryVtbl };

static void CallTwice(const char *Stage, IClassFactory *Proxy, BOOL CrossProcess)
{
    IClientSecurity *ClientSecurity;
    DWORD AuthnSvc, AuthzSvc, AuthnLevel, ImpLevel, Capabilities;
    OLECHAR *ServerName;
    WCHAR User[300];
    void *AuthInfo;
    IUnknown *Object;
    HRESULT hr;

    hr = IClassFactory_QueryInterface(Proxy, &IID_IClientSecurity, (void **)&ClientSecurity);
    ok(hr == S_OK, "%s: no IClientSecurity, 0x%lx\n", Stage, hr);
    if (FAILED(hr))
        return;
    ServerName = NULL;
    AuthInfo = (void *)(ULONG_PTR)0xdead;
    hr = IClientSecurity_QueryBlanket(ClientSecurity, (IUnknown *)Proxy, &AuthnSvc, &AuthzSvc, &ServerName,
                                      &AuthnLevel, &ImpLevel, &AuthInfo, &Capabilities);
    ok(hr == S_OK, "%s: QueryBlanket returned 0x%lx\n", Stage, hr);
    ok(AuthnSvc == RPC_C_AUTHN_WINNT, "%s: authn service %lu\n", Stage, AuthnSvc);
    ok(AuthzSvc == RPC_C_AUTHZ_NONE, "%s: authz service %lu\n", Stage, AuthzSvc);
    if (CrossProcess)
    {
        ok(ServerName && GetProcessUser(User, ARRAYSIZE(User)) && !lstrcmpiW(ServerName, User),
           "%s: server principal %s\n", Stage, wine_dbgstr_w(ServerName));
    }
    else
    {
        ok(ServerName == NULL, "%s: server principal %s\n", Stage, wine_dbgstr_w(ServerName));
    }
    CoTaskMemFree(ServerName);
    ok(AuthnLevel == RPC_C_AUTHN_LEVEL_PKT_PRIVACY, "%s: authn level %lu\n", Stage, AuthnLevel);
    ok(ImpLevel == RPC_C_IMP_LEVEL_IDENTIFY, "%s: imp level %lu\n", Stage, ImpLevel);
    ok(AuthInfo == NULL, "%s: auth info %p\n", Stage, AuthInfo);
    ok(Capabilities == (CrossProcess ? EOAC_MUTUAL_AUTH : EOAC_NONE), "%s: capabilities 0x%lx\n", Stage, Capabilities);

    hr = IClassFactory_CreateInstance(Proxy, NULL, &IID_IUnknown, (void **)&Object);
    ok(hr == E_NOTIMPL, "%s: first call returned 0x%lx\n", Stage, hr);

    hr = CoSetProxyBlanket((IUnknown *)Proxy, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                           RPC_C_AUTHN_LEVEL_PKT_INTEGRITY, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    ok(hr == S_OK, "%s: CoSetProxyBlanket returned 0x%lx\n", Stage, hr);
    hr = IClientSecurity_QueryBlanket(ClientSecurity, (IUnknown *)Proxy, &AuthnSvc, NULL, NULL, &AuthnLevel,
                                      &ImpLevel, NULL, &Capabilities);
    ok(hr == S_OK, "%s: QueryBlanket returned 0x%lx\n", Stage, hr);
    ok(AuthnLevel == RPC_C_AUTHN_LEVEL_PKT_PRIVACY, "%s: authn level after SetBlanket %lu\n", Stage, AuthnLevel);
    ok(ImpLevel == RPC_C_IMP_LEVEL_IMPERSONATE, "%s: imp level after SetBlanket %lu\n", Stage, ImpLevel);
    ok(Capabilities == EOAC_NONE, "%s: capabilities after SetBlanket 0x%lx\n", Stage, Capabilities);

    hr = IClassFactory_CreateInstance(Proxy, NULL, &IID_IUnknown, (void **)&Object);
    ok(hr == E_NOTIMPL, "%s: second call returned 0x%lx\n", Stage, hr);
    IClientSecurity_Release(ClientSecurity);
}

static DWORD WINAPI InprocCaller(void *Param)
{
    IClassFactory *Proxy;
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoUnmarshalInterface(Param, &IID_IClassFactory, (void **)&Proxy);
    ok(hr == S_OK, "inproc: CoUnmarshalInterface returned 0x%lx\n", hr);
    if (SUCCEEDED(hr))
    {
        CallTwice("inproc client", Proxy, FALSE);
        IClassFactory_Release(Proxy);
    }
    CoUninitialize();
    return 0;
}

static void ChildInproc(void)
{
    LARGE_INTEGER Zero = { { 0 } };
    IStream *Stream;
    HANDLE Thread;
    MSG Msg;
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CheckOutsideCall();
    CreateStreamOnHGlobal(NULL, TRUE, &Stream);
    hr = CoMarshalInterface(Stream, &IID_IClassFactory, (IUnknown *)&Factory, MSHCTX_INPROC, NULL, MSHLFLAGS_NORMAL);
    ok(hr == S_OK, "inproc: CoMarshalInterface returned 0x%lx\n", hr);
    IStream_Seek(Stream, Zero, STREAM_SEEK_SET, NULL);
    Thread = CreateThread(NULL, 0, InprocCaller, Stream, 0, NULL);
    while (MsgWaitForMultipleObjects(1, &Thread, FALSE, 20000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
    {
        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&Msg);
    }
    CloseHandle(Thread);
    ok(CallCount == 2, "inproc: %ld calls reached the server\n", CallCount);
    if (CallCount > 0)
        CheckCall("inproc default", &Calls[0], SecurityIdentification, FALSE);
    if (CallCount > 1)
        CheckCall("inproc impersonate", &Calls[1], SecurityImpersonation, FALSE);
    CheckOutsideCall();
    IStream_Release(Stream);
    CoUninitialize();
}

static void ChildRpcServer(HANDLE Pipe)
{
    IStream *Stream;
    HGLOBAL Global;
    DWORD Size, Written, Start = GetTickCount();
    MSG Msg;
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CreateStreamOnHGlobal(NULL, TRUE, &Stream);
    hr = CoMarshalInterface(Stream, &IID_IClassFactory, (IUnknown *)&Factory, MSHCTX_LOCAL, NULL, MSHLFLAGS_NORMAL);
    ok(hr == S_OK, "rpcserver: CoMarshalInterface returned 0x%lx\n", hr);
    GetHGlobalFromStream(Stream, &Global);
    Size = (DWORD)GlobalSize(Global);
    WriteFile(Pipe, &Size, sizeof(Size), &Written, NULL);
    WriteFile(Pipe, GlobalLock(Global), Size, &Written, NULL);
    GlobalUnlock(Global);
    CloseHandle(Pipe);
    while (CallCount < 2 && GetTickCount() - Start < 20000)
    {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_ALLINPUT);
        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&Msg);
    }
    ok(CallCount == 2, "rpcserver: %ld calls reached the server\n", CallCount);
    if (CallCount > 0)
        CheckCall("rpcserver default", &Calls[0], SecurityIdentification, TRUE);
    if (CallCount > 1)
        CheckCall("rpcserver impersonate", &Calls[1], SecurityImpersonation, TRUE);
    CoReleaseMarshalData(Stream);
    IStream_Release(Stream);
    CoUninitialize();
}

static void TestCrossProcess(const char *Exe)
{
    SECURITY_ATTRIBUTES Attributes = { sizeof(Attributes), NULL, TRUE };
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limit = { { { { 0 } } } };
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    char CommandLine[MAX_PATH + 64];
    HANDLE Read, Write, Job;
    DWORD Size = 0, Got, Total = 0, ExitCode;
    IClassFactory *Proxy;
    HGLOBAL Global;
    IStream *Stream;
    HRESULT hr;

    if (!CreatePipe(&Read, &Write, &Attributes, 0))
    {
        skip("CreatePipe failed with error %lu\n", GetLastError());
        return;
    }
    SetHandleInformation(Read, HANDLE_FLAG_INHERIT, 0);
    Job = CreateJobObjectW(NULL, NULL);
    Limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Limit.BasicLimitInformation.ActiveProcessLimit = 1;
    SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Limit, sizeof(Limit));
    snprintf(CommandLine, sizeof(CommandLine), "\"%s\" security child rpcserver %Iu", Exe, (ULONG_PTR)Write);
    if (!CreateProcessA(NULL, CommandLine, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcessA for rpcserver failed with error %lu\n", GetLastError());
        CloseHandle(Read);
        CloseHandle(Write);
        CloseHandle(Job);
        return;
    }
    AssignProcessToJobObject(Job, ProcessInfo.hProcess);
    ResumeThread(ProcessInfo.hThread);
    CloseHandle(Write);

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (ReadFile(Read, &Size, sizeof(Size), &Got, NULL) && Got == sizeof(Size) && Size && Size < 0x10000 &&
        (Global = GlobalAlloc(GMEM_MOVEABLE, Size)))
    {
        BYTE *Data = GlobalLock(Global);
        while (Total < Size && ReadFile(Read, Data + Total, Size - Total, &Got, NULL) && Got)
            Total += Got;
        GlobalUnlock(Global);
        CreateStreamOnHGlobal(Global, TRUE, &Stream);
        hr = CoUnmarshalInterface(Stream, &IID_IClassFactory, (void **)&Proxy);
        ok(hr == S_OK, "rpcserver: CoUnmarshalInterface returned 0x%lx\n", hr);
        if (SUCCEEDED(hr))
        {
            CallTwice("rpcserver client", Proxy, TRUE);
            IClassFactory_Release(Proxy);
        }
        IStream_Release(Stream);
    }
    else
        ok(0, "rpcserver: no marshal data\n");
    CoUninitialize();
    CloseHandle(Read);

    ok(WaitForSingleObject(ProcessInfo.hProcess, 30000) == WAIT_OBJECT_0, "Child rpcserver did not end\n");
    ok(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode) && ExitCode == 0, "Child rpcserver had %lu failures\n",
       ExitCode);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
    CloseHandle(Job);
}

static BOOL EnableCallSecurity(DWORD *Previous, BOOL *HadValue)
{
    DWORD One = 1, Size = sizeof(*Previous);
    HKEY Key;
    LONG Error;

    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\LiberNT\\COM", 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                            NULL, &Key, NULL);
    if (Error)
        return FALSE;
    *HadValue = !RegQueryValueExW(Key, L"CallSecurity", NULL, NULL, (BYTE *)Previous, &Size);
    Error = RegSetValueExW(Key, L"CallSecurity", 0, REG_DWORD, (BYTE *)&One, sizeof(One));
    RegCloseKey(Key);
    return !Error;
}

static void RestoreCallSecurity(DWORD Previous, BOOL HadValue)
{
    HKEY Key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\LiberNT\\COM", 0, KEY_SET_VALUE, &Key))
        return;
    if (HadValue)
        RegSetValueExW(Key, L"CallSecurity", 0, REG_DWORD, (BYTE *)&Previous, sizeof(Previous));
    else
        RegDeleteValueW(Key, L"CallSecurity");
    RegCloseKey(Key);
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
    { "inproc", ChildInproc },
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

    DWORD Previous = 0;
    BOOL HadValue = FALSE, Enabled;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 5 && !strcmp(Argv[2], "child") && !strcmp(Argv[3], "rpcserver"))
    {
        ChildRpcServer((HANDLE)(ULONG_PTR)_strtoui64(Argv[4], NULL, 10));
        return;
    }
    if (Argc >= 4 && !strcmp(Argv[2], "child"))
    {
        for (i = 0; i < ARRAYSIZE(Children); i++)
        {
            if (!strcmp(Argv[3], Children[i].Name))
                Children[i].Run();
        }
        return;
    }

    Enabled = EnableCallSecurity(&Previous, &HadValue);
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
    TestCrossProcess(Argv[0]);
    if (Enabled)
        RestoreCallSecurity(Previous, HadValue);
}
