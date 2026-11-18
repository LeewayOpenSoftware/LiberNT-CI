/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests activation of a DLL server outside the caller's process
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS

#include <wine/test.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <objbase.h>
#include <oleauto.h>
#include <tlhelp32.h>

#define MAX_SURROGATES 64
#define SURROGATE_EXIT_WAIT 15000

static const CLSID CLSID_TestServer = { 0xee09b103, 0x97e0, 0x11cf, { 0x97, 0x8f, 0x00, 0xa0, 0x24, 0x63, 0xe0, 0x6f } };
static const WCHAR MachineServerKey[] = L"Software\\Classes\\CLSID\\{EE09B103-97E0-11CF-978F-00A02463E06F}\\InprocServer32";
static const WCHAR UserClassKey[] = L"Software\\Classes\\CLSID\\{EE09B103-97E0-11CF-978F-00A02463E06F}";
static const WCHAR UserAppIdKey[] = L"Software\\Classes\\AppID\\{3F6C2B8E-51D4-4A7B-9C0E-8D2A5B71E4F3}";
static const WCHAR AppIdString[] = L"{3F6C2B8E-51D4-4A7B-9C0E-8D2A5B71E4F3}";
static const WCHAR MarkerName[] = L"SurrogateTest";

static BOOL CopyValue(HKEY Source, HKEY Target, const WCHAR *Name)
{
    BYTE Data[2 * MAX_PATH * sizeof(WCHAR)];
    DWORD Size = sizeof(Data), Type;

    if (RegQueryValueExW(Source, Name, NULL, &Type, Data, &Size) != ERROR_SUCCESS)
        return FALSE;
    return RegSetValueExW(Target, Name, 0, Type, Data, Size) == ERROR_SUCCESS;
}

static void RemoveRegistration(void)
{
    DWORD Marker, Size = sizeof(Marker);

    if (RegGetValueW(HKEY_CURRENT_USER, UserClassKey, MarkerName, RRF_RT_REG_DWORD, NULL, &Marker, &Size) != ERROR_SUCCESS)
        return;

    RegDeleteTreeW(HKEY_CURRENT_USER, UserClassKey);
    RegDeleteKeyW(HKEY_CURRENT_USER, UserClassKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, UserAppIdKey);
    RegDeleteKeyW(HKEY_CURRENT_USER, UserAppIdKey);
}

static BOOL AddRegistration(void)
{
    HKEY MachineKey, ClassKey, ServerKey, AppIdKey;
    DWORD Marker = 1;
    BOOL Done = FALSE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, MachineServerKey, 0, KEY_READ, &MachineKey) != ERROR_SUCCESS)
        return FALSE;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, UserClassKey, 0, NULL, 0, KEY_WRITE, NULL, &ClassKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(ClassKey, MarkerName, 0, REG_DWORD, (const BYTE *)&Marker, sizeof(Marker));
        if (RegCreateKeyExW(ClassKey, L"InprocServer32", 0, NULL, 0, KEY_WRITE, NULL, &ServerKey, NULL) == ERROR_SUCCESS)
        {
            Done = CopyValue(MachineKey, ServerKey, NULL);
            CopyValue(MachineKey, ServerKey, L"ThreadingModel");
            RegCloseKey(ServerKey);
        }
        if (Done)
            Done = RegSetValueExW(ClassKey, L"AppID", 0, REG_SZ, (const BYTE *)AppIdString, sizeof(AppIdString)) == ERROR_SUCCESS;
        RegCloseKey(ClassKey);
    }
    RegCloseKey(MachineKey);
    if (!Done)
        return FALSE;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, UserAppIdKey, 0, NULL, 0, KEY_WRITE, NULL, &AppIdKey, NULL) != ERROR_SUCCESS)
        return FALSE;
    Done = RegSetValueExW(AppIdKey, L"DllSurrogate", 0, REG_SZ, (const BYTE *)L"", sizeof(WCHAR)) == ERROR_SUCCESS;
    RegCloseKey(AppIdKey);
    return Done;
}

static DWORD ListSurrogates(DWORD *Pids, DWORD *Parents)
{
    PROCESSENTRY32W Entry;
    HANDLE Snapshot;
    DWORD Count = 0;

    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return 0;

    Entry.dwSize = sizeof(Entry);
    if (Process32FirstW(Snapshot, &Entry))
    {
        do
        {
            if (Count < MAX_SURROGATES && !_wcsicmp(Entry.szExeFile, L"dllhost.exe"))
            {
                Pids[Count] = Entry.th32ProcessID;
                Parents[Count] = Entry.th32ParentProcessID;
                Count++;
            }
        } while (Process32NextW(Snapshot, &Entry));
    }
    CloseHandle(Snapshot);
    return Count;
}

static DWORD FindNewSurrogates(const DWORD *Before, DWORD BeforeCount, DWORD *NewPid, DWORD *NewParent)
{
    DWORD Pids[MAX_SURROGATES], Parents[MAX_SURROGATES];
    DWORD Count, Index, Other, NewCount = 0;

    Count = ListSurrogates(Pids, Parents);
    for (Index = 0; Index < Count; Index++)
    {
        for (Other = 0; Other < BeforeCount; Other++)
        {
            if (Before[Other] == Pids[Index])
                break;
        }
        if (Other == BeforeCount)
        {
            *NewPid = Pids[Index];
            *NewParent = Parents[Index];
            NewCount++;
        }
    }
    return NewCount;
}

static void TestWithoutSurrogate(void)
{
    IClassFactory *ClassFactory = (IClassFactory *)(ULONG_PTR)0xdeadbeef;
    IUnknown *Unknown = (IUnknown *)(ULONG_PTR)0xdeadbeef;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_TestServer, NULL, CLSCTX_LOCAL_SERVER, &IID_IUnknown, (void **)&Unknown);
    ok(hr == REGDB_E_CLASSNOTREG, "CoCreateInstance without a surrogate returned %#lx\n", hr);
    ok(Unknown == NULL, "Unknown is %p\n", Unknown);

    hr = CoGetClassObject(&CLSID_TestServer, CLSCTX_LOCAL_SERVER, NULL, &IID_IClassFactory, (void **)&ClassFactory);
    ok(hr == REGDB_E_CLASSNOTREG, "CoGetClassObject without a surrogate returned %#lx\n", hr);
    ok(ClassFactory == NULL, "ClassFactory is %p\n", ClassFactory);
}

static void TestInSurrogate(void)
{
    DWORD Before[MAX_SURROGATES], Parents[MAX_SURROGATES];
    DWORD BeforeCount, FirstCount, SecondCount, NewPid = 0, NewParent = 0, Start;
    IUnknown *First = NULL, *Second = NULL;
    UINT TypeInfoCount = 0xdeadbeef;
    IDispatch *Dispatch = NULL;
    HANDLE Process;
    HRESULT hr;

    BeforeCount = ListSurrogates(Before, Parents);

    hr = CoCreateInstance(&CLSID_TestServer, NULL, CLSCTX_LOCAL_SERVER, &IID_IUnknown, (void **)&First);
    ok(hr == S_OK, "CoCreateInstance in the surrogate failed, hr %#lx\n", hr);
    if (hr != S_OK)
        return;

    FirstCount = FindNewSurrogates(Before, BeforeCount, &NewPid, &NewParent);
    ok(FirstCount >= 1, "No new dllhost.exe process hosts the object\n");

    hr = IUnknown_QueryInterface(First, &IID_IDispatch, (void **)&Dispatch);
    ok(hr == S_OK, "QueryInterface for IDispatch across processes failed, hr %#lx\n", hr);
    if (hr == S_OK)
    {
        hr = IDispatch_GetTypeInfoCount(Dispatch, &TypeInfoCount);
        ok(hr == S_OK, "GetTypeInfoCount across processes failed, hr %#lx\n", hr);
        ok(TypeInfoCount == 1, "TypeInfoCount is %u\n", TypeInfoCount);
        IDispatch_Release(Dispatch);
    }

    hr = CoCreateInstance(&CLSID_TestServer, NULL, CLSCTX_LOCAL_SERVER, &IID_IUnknown, (void **)&Second);
    ok(hr == S_OK, "The second CoCreateInstance in the surrogate failed, hr %#lx\n", hr);
    SecondCount = FindNewSurrogates(Before, BeforeCount, &NewPid, &NewParent);
    ok(SecondCount == FirstCount, "%lu new dllhost.exe processes after the second object, %lu after the first\n", SecondCount, FirstCount);
    if (hr == S_OK)
    {
        ok(Second != First, "Both activations returned the same pointer\n");
        IUnknown_Release(Second);
    }
    IUnknown_Release(First);

    if (FirstCount != 1)
        return;

    Process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, NewPid);
    if (!Process)
        return;

    Start = GetTickCount();
    if (WaitForSingleObject(Process, SURROGATE_EXIT_WAIT) == WAIT_OBJECT_0)
    {
        trace("The surrogate left %lu ms after the last release\n", GetTickCount() - Start);
    }
    else
    {
        trace("The surrogate is still running %u ms after the last release\n", SURROGATE_EXIT_WAIT);
        if (NewParent == GetCurrentProcessId())
            TerminateProcess(Process, 0);
    }
    CloseHandle(Process);
}

START_TEST(surrogate)
{
    IUnknown *Unknown;
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ok(hr == S_OK, "CoInitializeEx failed, hr %#lx\n", hr);

    hr = CoCreateInstance(&CLSID_TestServer, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&Unknown);
    if (hr != S_OK)
    {
        skip("The test class cannot be created in process, hr %#lx\n", hr);
        CoUninitialize();
        return;
    }
    IUnknown_Release(Unknown);

    RemoveRegistration();
    TestWithoutSurrogate();

    if (AddRegistration())
        TestInSurrogate();
    else
        skip("The test class cannot be registered for the surrogate\n");
    RemoveRegistration();

    CoUninitialize();
}
