/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the identity, session and bitness a COM local server is started with
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS

#include <wine/test.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <objbase.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <lm.h>
#include <ntsecapi.h>
#include <stdio.h>

static const CLSID CLSID_LaunchTest = { 0x5d7c2e91, 0x8b3a, 0x4f61, { 0xa2, 0x0c, 0x6e, 0x19, 0x4b, 0xd8, 0x73, 0x05 } };
static const WCHAR ClassKey[] = L"Software\\Classes\\CLSID\\{5D7C2E91-8B3A-4F61-A20C-6E194BD87305}";
static const WCHAR AppIdKey[] = L"Software\\Classes\\AppID\\{9A4F06D3-27C1-4E8B-B5D2-0C83E6F1A947}";
static const WCHAR AppIdString[] = L"{9A4F06D3-27C1-4E8B-B5D2-0C83E6F1A947}";
static const WCHAR SwitchKey[] = L"Software\\LiberNT\\COM";
static const WCHAR TestUser[] = L"LiberComRunAs";

#define SERVER_LIFETIME 30000

/* The server's answer: who and where it runs */
typedef struct
{
    DWORD Pid;
    DWORD Session;
    DWORD Bits;
    WCHAR Sid[184];
} SERVER_IDENTITY;

/* ---- server side: a stream whose Read returns SERVER_IDENTITY ---- */

static HANDLE ServerDone;
static LONG ServerObjects;

static HRESULT WINAPI Stream_QueryInterface(IStream *iface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IStream) || IsEqualIID(riid, &IID_ISequentialStream))
    {
        *ppv = iface;
        IStream_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Stream_AddRef(IStream *iface)
{
    return InterlockedIncrement(&ServerObjects);
}

static ULONG WINAPI Stream_Release(IStream *iface)
{
    LONG Refs = InterlockedDecrement(&ServerObjects);

    if (!Refs)
        SetEvent(ServerDone);
    return Refs;
}

static void GetIdentity(SERVER_IDENTITY *Identity)
{
    BYTE Buffer[256];
    TOKEN_USER *User = (TOKEN_USER *)Buffer;
    WCHAR *Sid = NULL;
    HANDLE Token;
    DWORD Size;
    BOOL Wow64 = FALSE;

    ZeroMemory(Identity, sizeof(*Identity));
    Identity->Pid = GetCurrentProcessId();
    ProcessIdToSessionId(GetCurrentProcessId(), &Identity->Session);
    IsWow64Process(GetCurrentProcess(), &Wow64);
    Identity->Bits = sizeof(void *) * 8;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    {
        if (GetTokenInformation(Token, TokenUser, Buffer, sizeof(Buffer), &Size) && ConvertSidToStringSidW(User->User.Sid, &Sid))
        {
            lstrcpynW(Identity->Sid, Sid, ARRAYSIZE(Identity->Sid));
            LocalFree(Sid);
        }
        CloseHandle(Token);
    }
}

static HRESULT WINAPI Stream_Read(IStream *iface, void *pv, ULONG cb, ULONG *pcbRead)
{
    SERVER_IDENTITY Identity;

    GetIdentity(&Identity);
    cb = min(cb, sizeof(Identity));
    memcpy(pv, &Identity, cb);
    if (pcbRead)
        *pcbRead = cb;
    return S_OK;
}

static HRESULT WINAPI Stream_NotImpl(void)
{
    return E_NOTIMPL;
}

static IStreamVtbl StreamVtbl =
{
    Stream_QueryInterface,
    Stream_AddRef,
    Stream_Release,
    Stream_Read,
    (void *)Stream_NotImpl, /* Write */
    (void *)Stream_NotImpl, /* Seek */
    (void *)Stream_NotImpl, /* SetSize */
    (void *)Stream_NotImpl, /* CopyTo */
    (void *)Stream_NotImpl, /* Commit */
    (void *)Stream_NotImpl, /* Revert */
    (void *)Stream_NotImpl, /* LockRegion */
    (void *)Stream_NotImpl, /* UnlockRegion */
    (void *)Stream_NotImpl, /* Stat */
    (void *)Stream_NotImpl  /* Clone */
};

static IStream ServerStream = { &StreamVtbl };

static HRESULT WINAPI Factory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory))
    {
        *ppv = iface;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Factory_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI Factory_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI Factory_CreateInstance(IClassFactory *iface, IUnknown *Outer, REFIID riid, void **ppv)
{
    if (Outer)
        return CLASS_E_NOAGGREGATION;
    return IStream_QueryInterface(&ServerStream, riid, ppv);
}

static HRESULT WINAPI Factory_LockServer(IClassFactory *iface, BOOL Lock)
{
    return S_OK;
}

static IClassFactoryVtbl FactoryVtbl =
{
    Factory_QueryInterface,
    Factory_AddRef,
    Factory_Release,
    Factory_CreateInstance,
    Factory_LockServer
};

static IClassFactory ServerFactory = { &FactoryVtbl };

static int RunServer(void)
{
    DWORD Cookie;

    ServerDone = CreateEventW(NULL, TRUE, FALSE, NULL);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(CoRegisterClassObject(&CLSID_LaunchTest, (IUnknown *)&ServerFactory, CLSCTX_LOCAL_SERVER,
                                     REGCLS_SINGLEUSE, &Cookie)))
        return 1;
    /* Until the client released the object, or the lifetime is up */
    WaitForSingleObject(ServerDone, SERVER_LIFETIME);
    CoRevokeClassObject(Cookie);
    CoUninitialize();
    return 0;
}

/* ---- client side ---- */

static void RemoveRegistration(REGSAM View)
{
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClassKey);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, ClassKey, View, 0);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, AppIdKey);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, AppIdKey, View, 0);
}

/* Removes the test class from one registry view (0: this process's own) */
static void RemoveRegistrationView(REGSAM View)
{
    HKEY Classes;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes", 0, KEY_ALL_ACCESS | View, &Classes))
        return;
    RegDeleteTreeW(Classes, ClassKey + 17);
    RegDeleteTreeW(Classes, AppIdKey + 17);
    RegCloseKey(Classes);
}

/* The test class in one registry view (0: this process's own); RunAs and bitness preference optional */
static BOOL AddRegistrationView(const WCHAR *RunAs, DWORD PreferredBitness, REGSAM View)
{
    WCHAR Exe[MAX_PATH], Command[MAX_PATH + 32];
    HKEY Class, Server, AppId;
    BOOL Done = FALSE;

    GetModuleFileNameW(NULL, Exe, ARRAYSIZE(Exe));
    _snwprintf(Command, ARRAYSIZE(Command), L"\"%ls\" launchid server", Exe);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, ClassKey, 0, NULL, 0, KEY_WRITE | View, NULL, &Class, NULL))
        return FALSE;
    RegSetValueExW(Class, L"AppID", 0, REG_SZ, (const BYTE *)AppIdString, sizeof(AppIdString));
    if (!RegCreateKeyExW(Class, L"LocalServer32", 0, NULL, 0, KEY_WRITE, NULL, &Server, NULL))
    {
        Done = !RegSetValueExW(Server, NULL, 0, REG_SZ, (const BYTE *)Command, (DWORD)(wcslen(Command) + 1) * sizeof(WCHAR));
        RegCloseKey(Server);
    }
    RegCloseKey(Class);
    if (!Done || RegCreateKeyExW(HKEY_LOCAL_MACHINE, AppIdKey, 0, NULL, 0, KEY_WRITE | View, NULL, &AppId, NULL))
        return FALSE;
    if (RunAs)
        RegSetValueExW(AppId, L"RunAs", 0, REG_SZ, (const BYTE *)RunAs, (DWORD)(wcslen(RunAs) + 1) * sizeof(WCHAR));
    if (PreferredBitness)
        RegSetValueExW(AppId, L"PreferredServerBitness", 0, REG_DWORD, (const BYTE *)&PreferredBitness, sizeof(PreferredBitness));
    RegCloseKey(AppId);
    return TRUE;
}

static BOOL AddRegistration(const WCHAR *RunAs, DWORD PreferredBitness)
{
    return AddRegistrationView(RunAs, PreferredBitness, 0);
}

static HRESULT Activate(DWORD Context, SERVER_IDENTITY *Identity)
{
    IStream *Stream = NULL;
    ULONG Read = 0;
    HANDLE Server;
    HRESULT hr;

    ZeroMemory(Identity, sizeof(*Identity));
    hr = CoCreateInstance(&CLSID_LaunchTest, NULL, Context, &IID_IStream, (void **)&Stream);
    if (FAILED(hr))
        return hr;
    hr = IStream_Read(Stream, Identity, sizeof(*Identity), &Read);
    if (SUCCEEDED(hr) && Read != sizeof(*Identity))
        hr = E_UNEXPECTED;
    IStream_Release(Stream);
    /* The server leaves after its one object; the next activation must start a new one */
    if (SUCCEEDED(hr) && (Server = OpenProcess(SYNCHRONIZE, FALSE, Identity->Pid)))
    {
        WaitForSingleObject(Server, 10000);
        CloseHandle(Server);
    }
    return hr;
}

static void GetTokenSid(HANDLE Token, WCHAR *Out, DWORD Count)
{
    BYTE Buffer[256];
    TOKEN_USER *User = (TOKEN_USER *)Buffer;
    WCHAR *Sid;
    DWORD Size;

    Out[0] = 0;
    if (GetTokenInformation(Token, TokenUser, Buffer, sizeof(Buffer), &Size) && ConvertSidToStringSidW(User->User.Sid, &Sid))
    {
        lstrcpynW(Out, Sid, Count);
        LocalFree(Sid);
    }
}

/* The user of the shell in this session, which is who "Interactive User" means here */
static BOOL GetInteractiveSid(WCHAR *Out, DWORD Count)
{
    PROCESSENTRY32W Entry = { sizeof(Entry) };
    DWORD Session = 0, Mine = 0;
    HANDLE Snapshot, Process, Token;
    BOOL Found = FALSE;

    ProcessIdToSessionId(GetCurrentProcessId(), &Mine);
    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return FALSE;
    if (Process32FirstW(Snapshot, &Entry))
    {
        do
        {
            if (_wcsicmp(Entry.szExeFile, L"explorer.exe") || !ProcessIdToSessionId(Entry.th32ProcessID, &Session) || Session != Mine)
                continue;
            if ((Process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, Entry.th32ProcessID)))
            {
                if (OpenProcessToken(Process, TOKEN_QUERY, &Token))
                {
                    GetTokenSid(Token, Out, Count);
                    Found = Out[0] != 0;
                    CloseHandle(Token);
                }
                CloseHandle(Process);
            }
        } while (!Found && Process32NextW(Snapshot, &Entry));
    }
    CloseHandle(Snapshot);
    return Found;
}

static void TestDefaultIdentity(const WCHAR *MySid, DWORD MySession)
{
    SERVER_IDENTITY Identity;
    HRESULT hr;

    RemoveRegistration(0);
    if (!AddRegistration(NULL, 0))
    {
        skip("cannot register the test class\n");
        return;
    }
    hr = Activate(CLSCTX_LOCAL_SERVER, &Identity);
    ok(hr == S_OK, "activation without RunAs failed, hr %#lx\n", hr);
    if (hr == S_OK)
    {
        ok(Identity.Pid != GetCurrentProcessId(), "the server runs in this process\n");
        ok(!wcscmp(Identity.Sid, MySid), "the server runs as %ls, not as the caller %ls\n", Identity.Sid, MySid);
        ok(Identity.Session == MySession, "the server runs in session %lu, the caller in %lu\n", Identity.Session, MySession);
    }
    RemoveRegistration(0);
}

static void TestInteractiveUser(DWORD MySession)
{
    WCHAR Interactive[184];
    SERVER_IDENTITY Identity;
    HRESULT hr;

    if (!GetInteractiveSid(Interactive, ARRAYSIZE(Interactive)))
    {
        skip("no shell in this session tells who the interactive user is\n");
        return;
    }
    RemoveRegistration(0);
    if (!AddRegistration(L"Interactive User", 0))
        return;
    hr = Activate(CLSCTX_LOCAL_SERVER, &Identity);
    ok(hr == S_OK, "activation with RunAs Interactive User failed, hr %#lx\n", hr);
    if (hr == S_OK)
    {
        ok(!wcscmp(Identity.Sid, Interactive), "the server runs as %ls, the interactive user is %ls\n", Identity.Sid, Interactive);
        ok(Identity.Session == MySession, "the server runs in session %lu, the caller in %lu\n", Identity.Session, MySession);
    }
    RemoveRegistration(0);

    /* The AppID's LaunchPermission decides, whatever the Enforcement switch says */
    if (AddRegistration(L"Interactive User", 0))
    {
        PSECURITY_DESCRIPTOR Sd;
        ULONG SdSize;
        HKEY AppId;

        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:(A;;0x3;;;SY)", SDDL_REVISION_1, &Sd, &SdSize) &&
            !RegOpenKeyExW(HKEY_LOCAL_MACHINE, AppIdKey, 0, KEY_SET_VALUE, &AppId))
        {
            RegSetValueExW(AppId, L"LaunchPermission", 0, REG_BINARY, Sd, SdSize);
            RegCloseKey(AppId);
            LocalFree(Sd);
            hr = Activate(CLSCTX_LOCAL_SERVER, &Identity);
            ok(hr == E_ACCESSDENIED, "RunAs activation against a SYSTEM-only LaunchPermission returned %#lx\n", hr);
        }
    }
    RemoveRegistration(0);
}

static BOOL ViewsAreSeparate(void);

/* A named account: a local test user whose password the configuration stores as SCM:{AppID} */
static void TestNamedUser(void)
{
    WCHAR Password[32], SecretName[64], Expected[184], *SidString, Domain[64], Account[96];
    BYTE Sid[SECURITY_MAX_SID_SIZE];
    DWORD SidSize = sizeof(Sid), DomainSize = ARRAYSIZE(Domain), Error = 0;
    LSA_OBJECT_ATTRIBUTES Attributes = { sizeof(Attributes) };
    LSA_UNICODE_STRING Key, Data;
    USER_INFO_1 User = { 0 };
    SID_NAME_USE Use;
    LSA_HANDLE Policy = NULL;
    SERVER_IDENTITY Identity;
    HRESULT hr;

    _snwprintf(Password, ARRAYSIZE(Password), L"Lc%08lx!%04lx", GetTickCount() ^ 0x5a5a5a5a, GetCurrentProcessId() & 0xffff);
    User.usri1_name = (WCHAR *)TestUser;
    User.usri1_password = Password;
    User.usri1_priv = USER_PRIV_USER;
    User.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    NetUserDel(NULL, TestUser);
    if (NetUserAdd(NULL, 1, (BYTE *)&User, &Error) != NERR_Success)
    {
        skip("cannot create the test account (parameter %lu)\n", Error);
        return;
    }
    if (!LookupAccountNameW(NULL, TestUser, Sid, &SidSize, Domain, &DomainSize, &Use) || !ConvertSidToStringSidW(Sid, &SidString))
    {
        skip("cannot look up the test account\n");
        NetUserDel(NULL, TestUser);
        return;
    }
    lstrcpynW(Expected, SidString, ARRAYSIZE(Expected));
    LocalFree(SidString);

    _snwprintf(SecretName, ARRAYSIZE(SecretName), L"SCM:%ls", AppIdString);
    Key.Buffer = SecretName;
    Key.Length = (USHORT)(wcslen(SecretName) * sizeof(WCHAR));
    Key.MaximumLength = Key.Length + sizeof(WCHAR);
    Data.Buffer = Password;
    Data.Length = (USHORT)(wcslen(Password) * sizeof(WCHAR));
    Data.MaximumLength = Data.Length + sizeof(WCHAR);
    if (LsaOpenPolicy(NULL, &Attributes, POLICY_CREATE_SECRET, &Policy) || LsaStorePrivateData(Policy, &Key, &Data))
    {
        skip("cannot store the account password for the AppID\n");
        goto Done;
    }

    _snwprintf(Account, ARRAYSIZE(Account), L".\\%ls", TestUser);
    RemoveRegistration(0);
    if (AddRegistration(Account, 0))
    {
        hr = Activate(CLSCTX_LOCAL_SERVER, &Identity);
        ok(hr == S_OK, "activation with RunAs %ls failed, hr %#lx\n", Account, hr);
        if (hr == S_OK)
            ok(!wcscmp(Identity.Sid, Expected), "the server runs as %ls, the account is %ls\n", Identity.Sid, Expected);
    }
    RemoveRegistration(0);

    /* Registered with RunAs only in the 32-bit view: a 64-bit client still gets the account, never itself */
    if (sizeof(void *) == 8 && AddRegistration(NULL, 0))
    {
        BOOL Separate = ViewsAreSeparate();

        RemoveRegistration(0);
        if (Separate && AddRegistrationView(Account, 0, KEY_WOW64_32KEY))
        {
            hr = Activate(CLSCTX_LOCAL_SERVER, &Identity);
            ok(hr == S_OK, "activation of a 32-bit-view RunAs class failed, hr %#lx\n", hr);
            if (hr == S_OK)
                ok(!wcscmp(Identity.Sid, Expected), "a 32-bit-view RunAs class ran as %ls, the account is %ls\n",
                   Identity.Sid, Expected);
        }
        else if (!Separate)
            skip("the 32-bit registry view shows the 64-bit classes\n");
        RemoveRegistrationView(KEY_WOW64_32KEY);
    }

    /* A wrong password is a logon failure, and the server is not started as the caller instead */
    Data.Buffer = L"wrong";
    Data.Length = 5 * sizeof(WCHAR);
    Data.MaximumLength = 6 * sizeof(WCHAR);
    LsaStorePrivateData(Policy, &Key, &Data);
    if (AddRegistration(Account, 0))
    {
        hr = Activate(CLSCTX_LOCAL_SERVER, &Identity);
        ok(hr == CO_E_RUNAS_LOGON_FAILURE, "activation with a wrong password returned %#lx\n", hr);
    }
    RemoveRegistration(0);

Done:
    if (Policy)
    {
        LsaStorePrivateData(Policy, &Key, NULL);
        LsaClose(Policy);
    }
    NetUserDel(NULL, TestUser);
}

/* Whether the 32-bit registry view is separate here: else no bitness can be told apart */
static BOOL ViewsAreSeparate(void)
{
    HKEY Key;
    BOOL Separate;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ClassKey, 0, KEY_READ | KEY_WOW64_32KEY, &Key))
        return TRUE;
    RegCloseKey(Key);
    Separate = FALSE;
    return Separate;
}

static void TestBitness(void)
{
    const DWORD Native = sizeof(void *) * 8, Other = Native == 64 ? 32 : 64;
    const DWORD NativeFlag = Native == 64 ? CLSCTX_ACTIVATE_64_BIT_SERVER : CLSCTX_ACTIVATE_32_BIT_SERVER;
    const DWORD OtherFlag = Native == 64 ? CLSCTX_ACTIVATE_32_BIT_SERVER : CLSCTX_ACTIVATE_64_BIT_SERVER;
    SERVER_IDENTITY Identity;
    HRESULT hr;

    RemoveRegistration(0);
    if (!AddRegistration(NULL, 0))
        return;
    if (Native == 64 && !ViewsAreSeparate())
    {
        skip("the 32-bit registry view shows the 64-bit classes\n");
        RemoveRegistration(0);
        return;
    }
    /* Only this bitness is registered */
    hr = Activate(CLSCTX_LOCAL_SERVER | NativeFlag, &Identity);
    ok(hr == S_OK && Identity.Bits == Native, "activation asking for %lu bits: hr %#lx, %lu bits\n", Native, hr, Identity.Bits);
    hr = Activate(CLSCTX_LOCAL_SERVER | OtherFlag, &Identity);
    ok(FAILED(hr), "activation asking for %lu bits found a server (%lu bits)\n", Other, Identity.Bits);
    RemoveRegistration(0);

    AddRegistration(NULL, Native == 64 ? 2 : 3);
    hr = Activate(CLSCTX_LOCAL_SERVER, &Identity);
    ok(FAILED(hr), "PreferredServerBitness for %lu bits found a server (%lu bits)\n", Other, Identity.Bits);
    /* The caller's flag comes before the AppID's preference */
    hr = Activate(CLSCTX_LOCAL_SERVER | NativeFlag, &Identity);
    ok(hr == S_OK, "the caller's bitness did not override PreferredServerBitness, hr %#lx\n", hr);
    RemoveRegistration(0);

    AddRegistration(NULL, 1);
    hr = Activate(CLSCTX_LOCAL_SERVER, &Identity);
    ok(hr == S_OK && Identity.Bits == Native, "PreferredServerBitness 1 (as the client): hr %#lx, %lu bits\n", hr, Identity.Bits);
    RemoveRegistration(0);
}

START_TEST(launchid)
{
    DWORD Old = 0, Size = sizeof(Old), On = 1, MySession = 0;
    WCHAR MySid[184];
    HANDLE Token;
    BOOL HadOld;
    char **Argv;
    HKEY Key;
    int Argc;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 3 && !strcmp(Argv[2], "server"))
        ExitProcess(RunServer());

    /* Launch identity ships switched off; combase reads it once per process, rpcss per request */
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, SwitchKey, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &Key, NULL))
    {
        skip("Switching launch identity on needs administrator rights\n");
        return;
    }
    HadOld = !RegQueryValueExW(Key, L"LaunchIdentity", NULL, NULL, (BYTE *)&Old, &Size);
    RegSetValueExW(Key, L"LaunchIdentity", 0, REG_DWORD, (const BYTE *)&On, sizeof(On));

    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token);
    GetTokenSid(Token, MySid, ARRAYSIZE(MySid));
    CloseHandle(Token);
    ProcessIdToSessionId(GetCurrentProcessId(), &MySession);

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    TestDefaultIdentity(MySid, MySession);
    TestInteractiveUser(MySession);
    TestNamedUser();
    TestBitness();
    RemoveRegistration(0);
    CoUninitialize();

    if (HadOld)
        RegSetValueExW(Key, L"LaunchIdentity", 0, REG_DWORD, (const BYTE *)&Old, sizeof(Old));
    else
        RegDeleteValueW(Key, L"LaunchIdentity");
    RegCloseKey(Key);
}
