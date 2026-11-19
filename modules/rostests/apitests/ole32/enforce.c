/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     COM access, launch/activation and registration-owner checks in the three
 *              positions of HKLM\Software\LiberNT\COM Enforcement (DCOM step 5)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Needs write access to HKLM (run elevated or as Local System); skipped otherwise.
 * Every position runs the same cases:
 *  access     a server whose CoInitializeSecurity descriptor admits only Guests is called;
 *             then one that admits this user
 *  activation a running server's class has an AppID whose LaunchPermission admits only Guests;
 *             then one that admits this user
 *  owner      a class object registered by a low-integrity process is asked for by this process
 * Expected: the admitting cases succeed in every position; for the others off and audit
 * allow, enforce refuses; audit and enforce log the denial.
 *
 * "ole32_apitest enforce reference" runs only the access cases with the enforce
 * expectations and without the event log, as a reference on Windows; it needs no elevation.
 */

#include <apitest.h>

#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <sddl.h>
#include <stdio.h>

static const CLSID CLSID_EnforceActivation = {0x5f1d7a62,0x3c0e,0x4b8a,{0x9e,0x41,0x2a,0x6d,0x8c,0x17,0xb3,0x50}};
static const CLSID CLSID_EnforceOwner      = {0x5f1d7a62,0x3c0e,0x4b8a,{0x9e,0x41,0x2a,0x6d,0x8c,0x17,0xb3,0x51}};
static const WCHAR ActivationKey[] = L"Software\\Classes\\CLSID\\{5F1D7A62-3C0E-4B8A-9E41-2A6D8C17B350}";
static const WCHAR AppIdKey[]      = L"Software\\Classes\\AppID\\{5F1D7A62-3C0E-4B8A-9E41-2A6D8C17B352}";
static const WCHAR AppIdString[]   = L"{5F1D7A62-3C0E-4B8A-9E41-2A6D8C17B352}";
static const WCHAR SwitchKey[]     = L"Software\\LiberNT\\COM";

static const char *ModeName[] = { "off", "audit", "enforce" };
static BOOL Reference;

/* "O:BAG:BAD:(A;;<rights>;;;SY)(A;;<rights>;;;<this user>)" */
static BOOL SelfSddl(WCHAR *Buffer, DWORD Length, const WCHAR *Rights)
{
    BYTE Data[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    WCHAR *String = NULL;
    HANDLE Token;
    DWORD Size;
    BOOL Ret = FALSE;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    {
        if (GetTokenInformation(Token, TokenUser, Data, sizeof(Data), &Size) &&
            ConvertSidToStringSidW(((TOKEN_USER *)Data)->User.Sid, &String))
        {
            Ret = _snwprintf(Buffer, Length, L"O:BAG:BAD:(A;;%s;;;SY)(A;;%s;;;%s)", Rights, Rights, String) > 0;
            Buffer[Length - 1] = 0;
            LocalFree(String);
        }
        CloseHandle(Token);
    }
    return Ret;
}

/* ---------------------------------------------------------------- server side */

static LONG CallCount;

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
    *Object = NULL;
    InterlockedIncrement(&CallCount);
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

static void Serve(void)
{
    MSG Msg;

    for (;;)
    {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_ALLINPUT);
        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&Msg);
    }
}

static void WriteAll(HANDLE Pipe, const void *Data, DWORD Size)
{
    DWORD Written;

    WriteFile(Pipe, Data, Size, &Written, NULL);
}

/* CoInitializeSecurity is given the descriptor in absolute format */
static PSECURITY_DESCRIPTOR AbsoluteSd(const WCHAR *Sddl)
{
    DWORD SdSize = 0, DaclSize = 0, SaclSize = 0, OwnerSize = 0, GroupSize = 0;
    PSECURITY_DESCRIPTOR Relative, Absolute = NULL;
    BYTE *Block;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl, SDDL_REVISION_1, &Relative, NULL))
        return NULL;
    MakeAbsoluteSD(Relative, NULL, &SdSize, NULL, &DaclSize, NULL, &SaclSize, NULL, &OwnerSize, NULL, &GroupSize);
    if ((Block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, SdSize + DaclSize + SaclSize + OwnerSize + GroupSize + 16)))
    {
        BYTE *Dacl = Block + SdSize, *Sacl = Dacl + DaclSize, *Owner = Sacl + SaclSize, *Group = Owner + OwnerSize;

        if (MakeAbsoluteSD(Relative, Block, &SdSize, (PACL)Dacl, &DaclSize, (PACL)Sacl, &SaclSize, Owner, &OwnerSize,
                           Group, &GroupSize))
            Absolute = Block;
        else
            HeapFree(GetProcessHeap(), 0, Block);
    }
    LocalFree(Relative);
    return Absolute;
}

/* "access": CoInitializeSecurity with a descriptor admitting only Guests (or this user), then marshal a
 * factory. On failure the size is 0 and the failing HRESULT follows. */
static void ChildAccess(HANDLE Pipe, BOOL Admit)
{
    WCHAR Sddl[256] = L"O:BAG:BAD:(A;;0x7;;;BG)";
    PSECURITY_DESCRIPTOR Sd = NULL;
    IStream *Stream;
    HGLOBAL Global;
    DWORD Size = 0;
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if ((Admit && !SelfSddl(Sddl, ARRAYSIZE(Sddl), L"0x7")) || !(Sd = AbsoluteSd(Sddl)))
        hr = HRESULT_FROM_WIN32(GetLastError());
    else if ((hr = CoInitializeSecurity(Sd, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY,
                                        NULL, EOAC_NONE, NULL)) == S_OK)
    {
        CreateStreamOnHGlobal(NULL, TRUE, &Stream);
        hr = CoMarshalInterface(Stream, &IID_IClassFactory, (IUnknown *)&Factory, MSHCTX_LOCAL, NULL,
                                MSHLFLAGS_NORMAL);
    }
    if (hr != S_OK)
    {
        WriteAll(Pipe, &Size, sizeof(Size));
        WriteAll(Pipe, &hr, sizeof(hr));
        return;
    }
    GetHGlobalFromStream(Stream, &Global);
    Size = (DWORD)GlobalSize(Global);
    WriteAll(Pipe, &Size, sizeof(Size));
    WriteAll(Pipe, GlobalLock(Global), Size);
    GlobalUnlock(Global);
    CloseHandle(Pipe);
    Serve();
}

/* "class": register a class object for the local-server context and report the result */
static void ChildClass(HANDLE Pipe, const CLSID *Clsid)
{
    DWORD Cookie;
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr))
        hr = CoRegisterClassObject(Clsid, (IUnknown *)&Factory, CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &Cookie);
    WriteAll(Pipe, &hr, sizeof(hr));
    CloseHandle(Pipe);
    if (SUCCEEDED(hr))
        Serve();
}

/* ---------------------------------------------------------------- client side */

static HANDLE Job;

static BOOL StartChild(const char *Exe, const char *Kind, BOOL LowIntegrity, HANDLE *Read, HANDLE *Process)
{
    SECURITY_ATTRIBUTES Attributes = { sizeof(Attributes), NULL, TRUE };
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    char CommandLine[MAX_PATH + 64];
    HANDLE Write, Token = NULL, Primary = NULL;
    BOOL Ret;

    if (!CreatePipe(Read, &Write, &Attributes, 0))
        return FALSE;
    SetHandleInformation(*Read, HANDLE_FLAG_INHERIT, 0);
    snprintf(CommandLine, sizeof(CommandLine), "\"%s\" enforce child %s %Iu", Exe, Kind, (ULONG_PTR)Write);
    if (LowIntegrity)
    {
        SID_IDENTIFIER_AUTHORITY Mandatory = { SECURITY_MANDATORY_LABEL_AUTHORITY };
        TOKEN_MANDATORY_LABEL Label = { { NULL, SE_GROUP_INTEGRITY } };

        Ret = OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT |
                               TOKEN_ASSIGN_PRIMARY, &Token) &&
              DuplicateTokenEx(Token, 0, NULL, SecurityImpersonation, TokenPrimary, &Primary) &&
              AllocateAndInitializeSid(&Mandatory, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0,
                                       &Label.Label.Sid);
        if (Ret)
        {
            Ret = SetTokenInformation(Primary, TokenIntegrityLevel, &Label,
                                      sizeof(Label) + GetLengthSid(Label.Label.Sid)) &&
                  CreateProcessAsUserA(Primary, NULL, CommandLine, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL,
                                       &StartupInfo, &ProcessInfo);
            FreeSid(Label.Label.Sid);
        }
        if (Primary)
            CloseHandle(Primary);
        if (Token)
            CloseHandle(Token);
    }
    else
    {
        Ret = CreateProcessA(NULL, CommandLine, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &StartupInfo,
                             &ProcessInfo);
    }
    CloseHandle(Write);
    if (!Ret)
    {
        CloseHandle(*Read);
        return FALSE;
    }
    AssignProcessToJobObject(Job, ProcessInfo.hProcess);
    ResumeThread(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hThread);
    *Process = ProcessInfo.hProcess;
    return TRUE;
}

static void StopChild(HANDLE Read, HANDLE Process)
{
    TerminateProcess(Process, 0);
    WaitForSingleObject(Process, 10000);
    CloseHandle(Process);
    CloseHandle(Read);
}

static BOOL ReadAll(HANDLE Read, void *Buffer, DWORD Size)
{
    DWORD Got, Total = 0;

    while (Total < Size && ReadFile(Read, (BYTE *)Buffer + Total, Size - Total, &Got, NULL) && Got)
        Total += Got;
    return Total == Size;
}

/* Events of source LiberNT-COM with the given id in the Application log */
static LONG CountEvents(DWORD Id)
{
    BYTE Buffer[0x4000];
    DWORD Read, Needed, Offset;
    HANDLE Log;
    LONG Count = 0;

    if (!(Log = OpenEventLogW(NULL, L"Application")))
        return -1;
    while (ReadEventLogW(Log, EVENTLOG_SEQUENTIAL_READ | EVENTLOG_FORWARDS_READ, 0, Buffer, sizeof(Buffer), &Read,
                         &Needed))
    {
        for (Offset = 0; Offset + sizeof(EVENTLOGRECORD) <= Read;)
        {
            EVENTLOGRECORD *Record = (EVENTLOGRECORD *)(Buffer + Offset);

            if (Record->Length < sizeof(EVENTLOGRECORD) || Offset + Record->Length > Read)
                break;
            if ((Record->EventID & 0xFFFF) == Id && !lstrcmpW((WCHAR *)(Record + 1), L"LiberNT-COM"))
                Count++;
            Offset += Record->Length;
        }
    }
    CloseEventLog(Log);
    return Count;
}

static void CheckLogged(DWORD Mode, BOOL Admitted, LONG Before[3], const char *Case)
{
    LONG Refused, Audited;

    if (Reference)
        return;
    Refused = CountEvents(1);
    Audited = CountEvents(2);
    if (Before[1] < 0 || Refused < 0)
    {
        skip("%s/%s: Application log not readable\n", ModeName[Mode], Case);
        return;
    }
    if (Mode == 0 || Admitted)
        ok(Refused == Before[1] && Audited == Before[2], "%s/%s: %ld refusal and %ld audit events were logged\n",
           ModeName[Mode], Case, Refused - Before[1], Audited - Before[2]);
    else if (Mode == 1)
        ok(Audited > Before[2] && Refused == Before[1], "%s/%s: audit events %ld, refusal events %ld\n",
           ModeName[Mode], Case, Audited - Before[2], Refused - Before[1]);
    else
        ok(Refused > Before[1], "%s/%s: no refusal event was logged\n", ModeName[Mode], Case);
}

static void SnapshotEvents(LONG Counts[3])
{
    Counts[1] = CountEvents(1);
    Counts[2] = CountEvents(2);
}

static void TestAccess(const char *Exe, DWORD Mode, BOOL Admit)
{
    const char *Case = Admit ? "access admitted" : "access";
    HANDLE Read, Process;
    IClassFactory *Proxy;
    IUnknown *Object;
    HGLOBAL Global;
    IStream *Stream;
    LONG Events[3];
    DWORD Size = 0;
    HRESULT hr;

    if (!StartChild(Exe, Admit ? "accessadmit" : "access", FALSE, &Read, &Process))
    {
        skip("%s/%s: child not started, error %lu\n", ModeName[Mode], Case, GetLastError());
        return;
    }
    if (!ReadAll(Read, &Size, sizeof(Size)) || !Size || Size > 0x10000 || !(Global = GlobalAlloc(GMEM_MOVEABLE, Size)))
    {
        HRESULT Failed = E_FAIL;

        if (!Size)
            ReadAll(Read, &Failed, sizeof(Failed));
        ok(0, "%s/%s: the server reported no marshal data, 0x%lx\n", ModeName[Mode], Case, Failed);
        StopChild(Read, Process);
        return;
    }
    ReadAll(Read, GlobalLock(Global), Size);
    GlobalUnlock(Global);
    CreateStreamOnHGlobal(Global, TRUE, &Stream);
    hr = CoUnmarshalInterface(Stream, &IID_IClassFactory, (void **)&Proxy);
    ok(hr == S_OK, "%s/%s: CoUnmarshalInterface returned 0x%lx\n", ModeName[Mode], Case, hr);
    if (SUCCEEDED(hr))
    {
        SnapshotEvents(Events);
        hr = IClassFactory_CreateInstance(Proxy, NULL, &IID_IUnknown, (void **)&Object);
        if (Mode == 2 && !Admit)
            ok(hr == E_ACCESSDENIED, "%s/%s: call returned 0x%lx, expected E_ACCESSDENIED\n", ModeName[Mode], Case, hr);
        else
            ok(hr == E_NOTIMPL, "%s/%s: call returned 0x%lx, expected E_NOTIMPL\n", ModeName[Mode], Case, hr);
        CheckLogged(Mode, Admit, Events, Case);
        IClassFactory_Release(Proxy);
    }
    IStream_Release(Stream);
    StopChild(Read, Process);
}

static BOOL SetSdValue(HKEY Key, const WCHAR *Name, const WCHAR *Sddl)
{
    PSECURITY_DESCRIPTOR Sd;
    ULONG Size;
    LONG Error;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl, SDDL_REVISION_1, &Sd, &Size))
        return FALSE;
    Error = RegSetValueExW(Key, Name, 0, REG_BINARY, Sd, Size);
    LocalFree(Sd);
    return !Error;
}

static BOOL RegisterActivationClass(void)
{
    HKEY Key;
    BOOL Ret;

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, ActivationKey, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &Key,
                        NULL))
        return FALSE;
    Ret = !RegSetValueExW(Key, L"AppID", 0, REG_SZ, (const BYTE *)AppIdString, sizeof(AppIdString));
    RegCloseKey(Key);
    if (!Ret || RegCreateKeyExW(HKEY_LOCAL_MACHINE, AppIdKey, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL,
                                &Key, NULL))
        return FALSE;
    Ret = SetSdValue(Key, L"LaunchPermission", L"O:BAG:BAD:(A;;0x1f;;;BG)");
    RegCloseKey(Key);
    return Ret;
}

static void UnregisterActivationClass(void)
{
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, ActivationKey, KEY_WOW64_64KEY, 0);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, AppIdKey, KEY_WOW64_64KEY, 0);
}

static BOOL SetLaunchPermission(BOOL Admit)
{
    WCHAR Sddl[256] = L"O:BAG:BAD:(A;;0x1f;;;BG)";
    HKEY Key;
    BOOL Ret;

    if (Admit && !SelfSddl(Sddl, ARRAYSIZE(Sddl), L"0x1f"))
        return FALSE;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, AppIdKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &Key))
        return FALSE;
    Ret = SetSdValue(Key, L"LaunchPermission", Sddl);
    RegCloseKey(Key);
    return Ret;
}

static void TestActivation(const char *Exe, DWORD Mode, BOOL Admit)
{
    const char *Case = Admit ? "activation admitted" : "activation";
    HANDLE Read, Process;
    IClassFactory *Factory;
    LONG Events[3];
    HRESULT hr, Registered = E_FAIL;

    if (!SetLaunchPermission(Admit))
    {
        ok(0, "%s/%s: LaunchPermission not written\n", ModeName[Mode], Case);
        return;
    }
    if (!StartChild(Exe, "activation", FALSE, &Read, &Process))
    {
        skip("%s/%s: child not started, error %lu\n", ModeName[Mode], Case, GetLastError());
        return;
    }
    if (!ReadAll(Read, &Registered, sizeof(Registered)) || FAILED(Registered))
    {
        ok(0, "%s/%s: the server could not register its class, 0x%lx\n", ModeName[Mode], Case, Registered);
        StopChild(Read, Process);
        return;
    }
    SnapshotEvents(Events);
    hr = CoGetClassObject(&CLSID_EnforceActivation, CLSCTX_LOCAL_SERVER, NULL, &IID_IClassFactory, (void **)&Factory);
    if (Mode == 2 && !Admit)
        ok(hr == E_ACCESSDENIED, "%s/%s: CoGetClassObject returned 0x%lx, expected E_ACCESSDENIED\n",
           ModeName[Mode], Case, hr);
    else
        ok(hr == S_OK, "%s/%s: CoGetClassObject returned 0x%lx\n", ModeName[Mode], Case, hr);
    if (SUCCEEDED(hr))
        IClassFactory_Release(Factory);
    CheckLogged(Mode, Admit, Events, Case);
    StopChild(Read, Process);
}

static void TestOwner(const char *Exe, DWORD Mode)
{
    HANDLE Read, Process;
    IClassFactory *Factory;
    HRESULT hr, Registered = E_FAIL;

    if (!StartChild(Exe, "owner", TRUE, &Read, &Process))
    {
        skip("%s/owner: low-integrity child not started, error %lu\n", ModeName[Mode], GetLastError());
        return;
    }
    if (!ReadAll(Read, &Registered, sizeof(Registered)) || FAILED(Registered))
    {
        skip("%s/owner: a low-integrity process cannot register a class here, 0x%lx\n", ModeName[Mode], Registered);
        StopChild(Read, Process);
        return;
    }
    hr = CoGetClassObject(&CLSID_EnforceOwner, CLSCTX_LOCAL_SERVER, NULL, &IID_IClassFactory, (void **)&Factory);
    if (Mode == 2)
        ok(FAILED(hr), "%s/owner: the low-integrity registration was handed out\n", ModeName[Mode]);
    else
        ok(hr == S_OK, "%s/owner: CoGetClassObject returned 0x%lx\n", ModeName[Mode], hr);
    if (SUCCEEDED(hr))
        IClassFactory_Release(Factory);
    StopChild(Read, Process);
}

static BOOL RunsAsService(void)
{
    BYTE Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    HANDLE Token;
    DWORD Size;
    BOOL Ret = FALSE;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    {
        if (GetTokenInformation(Token, TokenUser, Buffer, sizeof(Buffer), &Size))
        {
            PSID Sid = ((TOKEN_USER *)Buffer)->User.Sid;
            Ret = IsWellKnownSid(Sid, WinLocalSystemSid) || IsWellKnownSid(Sid, WinLocalServiceSid) ||
                  IsWellKnownSid(Sid, WinNetworkServiceSid);
        }
        CloseHandle(Token);
    }
    return Ret;
}

static BOOL SetMode(DWORD Mode)
{
    HKEY Key;
    LONG Error;

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, SwitchKey, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &Key, NULL))
        return FALSE;
    Error = RegSetValueExW(Key, L"Enforcement", 0, REG_DWORD, (const BYTE *)&Mode, sizeof(Mode));
    RegCloseKey(Key);
    return !Error;
}

START_TEST(enforce)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limit = { { { { 0 } } } };
    DWORD Previous = 0, Size = sizeof(Previous), Mode;
    BOOL HadValue = FALSE, Service;
    char **Argv;
    HKEY Key;
    int Argc;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 5 && !strcmp(Argv[2], "child"))
    {
        HANDLE Pipe = (HANDLE)(ULONG_PTR)_strtoui64(Argv[4], NULL, 10);

        if (!strcmp(Argv[3], "access"))
            ChildAccess(Pipe, FALSE);
        else if (!strcmp(Argv[3], "accessadmit"))
            ChildAccess(Pipe, TRUE);
        else if (!strcmp(Argv[3], "activation"))
            ChildClass(Pipe, &CLSID_EnforceActivation);
        else if (!strcmp(Argv[3], "owner"))
            ChildClass(Pipe, &CLSID_EnforceOwner);
        return;
    }

    Job = CreateJobObjectW(NULL, NULL);
    Limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Limit, sizeof(Limit));
    if (Argc >= 3 && !strcmp(Argv[2], "reference"))
    {
        Reference = TRUE;
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        TestAccess(Argv[0], 2, FALSE);
        TestAccess(Argv[0], 2, TRUE);
        CoUninitialize();
        CloseHandle(Job);
        return;
    }

    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, SwitchKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &Key))
    {
        HadValue = !RegQueryValueExW(Key, L"Enforcement", NULL, NULL, (BYTE *)&Previous, &Size);
        RegCloseKey(Key);
    }
    if (!SetMode(0) || !RegisterActivationClass())
    {
        skip("HKLM is not writable; run elevated\n");
        UnregisterActivationClass();
        CloseHandle(Job);
        return;
    }
    Service = RunsAsService();

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (Mode = 0; Mode <= 2; Mode++)
    {
        if (!SetMode(Mode))
        {
            ok(0, "could not set Enforcement to %lu\n", Mode);
            continue;
        }
        TestAccess(Argv[0], Mode, FALSE);
        TestAccess(Argv[0], Mode, TRUE);
        TestActivation(Argv[0], Mode, FALSE);
        TestActivation(Argv[0], Mode, TRUE);
        if (Service)
            skip("%s/owner: this process runs as a service account, whose registrations serve everyone\n",
                 ModeName[Mode]);
        else
            TestOwner(Argv[0], Mode);
    }
    CoUninitialize();

    UnregisterActivationClass();
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, SwitchKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &Key))
    {
        if (HadValue)
            RegSetValueExW(Key, L"Enforcement", 0, REG_DWORD, (const BYTE *)&Previous, sizeof(Previous));
        else
            RegDeleteValueW(Key, L"Enforcement");
        RegCloseKey(Key);
    }
    CloseHandle(Job);
}
