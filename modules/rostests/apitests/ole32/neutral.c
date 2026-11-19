/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the neutral apartment, default contexts and CoDisconnectContext
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS
#define CONST_VTABLE

#include <wine/test.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <objbase.h>
#include <oleauto.h>
#include <ctxtcall.h>

#include "neutral/neutral.h"

static const WCHAR MarkerName[] = L"NeutralTest";

static DWORD CallbackThread;
static APTTYPE CallbackType;
static APTTYPEQUALIFIER CallbackQualifier;

static void RemoveRegistration(void)
{
    DWORD Marker, Size = sizeof(Marker);

    if (RegGetValueW(HKEY_CURRENT_USER, NEUTRAL_CLASS_KEY, MarkerName, RRF_RT_REG_DWORD, NULL, &Marker, &Size) != ERROR_SUCCESS)
        return;
    RegDeleteTreeW(HKEY_CURRENT_USER, NEUTRAL_CLASS_KEY);
    RegDeleteKeyW(HKEY_CURRENT_USER, NEUTRAL_CLASS_KEY);
}

static BOOL AddRegistration(void)
{
    WCHAR Path[MAX_PATH];
    HKEY ClassKey, ServerKey;
    DWORD Marker = 1, Length;
    WCHAR *Name;
    BOOL Done = FALSE;

    Length = GetModuleFileNameW(NULL, Path, ARRAY_SIZE(Path));
    if (!Length || Length >= ARRAY_SIZE(Path))
        return FALSE;
    Name = wcsrchr(Path, L'\\');
    if (!Name || (Name + 1 - Path) + wcslen(NEUTRAL_SERVER_NAME) >= ARRAY_SIZE(Path))
        return FALSE;
    wcscpy(Name + 1, NEUTRAL_SERVER_NAME);
    if (GetFileAttributesW(Path) == INVALID_FILE_ATTRIBUTES)
        return FALSE;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, NEUTRAL_CLASS_KEY, 0, NULL, 0, KEY_WRITE, NULL, &ClassKey, NULL) != ERROR_SUCCESS)
        return FALSE;
    RegSetValueExW(ClassKey, MarkerName, 0, REG_DWORD, (const BYTE *)&Marker, sizeof(Marker));
    if (RegCreateKeyExW(ClassKey, L"InprocServer32", 0, NULL, 0, KEY_WRITE, NULL, &ServerKey, NULL) == ERROR_SUCCESS)
    {
        Done = RegSetValueExW(ServerKey, NULL, 0, REG_SZ, (const BYTE *)Path, (DWORD)(wcslen(Path) + 1) * sizeof(WCHAR)) == ERROR_SUCCESS &&
               RegSetValueExW(ServerKey, L"ThreadingModel", 0, REG_SZ, (const BYTE *)L"Neutral", sizeof(L"Neutral")) == ERROR_SUCCESS;
        RegCloseKey(ServerKey);
    }
    RegCloseKey(ClassKey);
    return Done;
}

static HRESULT WINAPI Callback_QueryInterface(IDispatch *iface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch))
    {
        *ppv = iface;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Callback_AddRef(IDispatch *iface)
{
    return 2;
}

static ULONG WINAPI Callback_Release(IDispatch *iface)
{
    return 1;
}

static HRESULT WINAPI Callback_GetTypeInfoCount(IDispatch *iface, UINT *Count)
{
    *Count = 0;
    return S_OK;
}

static HRESULT WINAPI Callback_GetTypeInfo(IDispatch *iface, UINT Index, LCID Lcid, ITypeInfo **TypeInfo)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI Callback_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *Names, UINT Count, LCID Lcid, DISPID *Ids)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI Callback_Invoke(IDispatch *iface, DISPID Id, REFIID riid, LCID Lcid, WORD Flags,
        DISPPARAMS *Params, VARIANT *Result, EXCEPINFO *ExcepInfo, UINT *ArgErr)
{
    CallbackThread = GetCurrentThreadId();
    if (FAILED(CoGetApartmentType(&CallbackType, &CallbackQualifier)))
        CallbackType = APTTYPE_CURRENT;
    if (Result)
    {
        V_VT(Result) = VT_I4;
        V_I4(Result) = (LONG)CallbackThread;
    }
    return S_OK;
}

static const IDispatchVtbl CallbackVtbl =
{
    Callback_QueryInterface,
    Callback_AddRef,
    Callback_Release,
    Callback_GetTypeInfoCount,
    Callback_GetTypeInfo,
    Callback_GetIDsOfNames,
    Callback_Invoke
};

static IDispatch Callback = { &CallbackVtbl };

static HRESULT InvokeLong(IDispatch *Dispatch, DISPID Id, IDispatch *Argument, LONG *Value)
{
    DISPPARAMS Params = { NULL, NULL, 0, 0 };
    VARIANT Arg, Result;
    HRESULT hr;

    if (Argument)
    {
        V_VT(&Arg) = VT_DISPATCH;
        V_DISPATCH(&Arg) = Argument;
        Params.rgvarg = &Arg;
        Params.cArgs = 1;
    }
    VariantInit(&Result);
    hr = IDispatch_Invoke(Dispatch, Id, &IID_NULL, 0, DISPATCH_METHOD, &Params, &Result, NULL, NULL);
    *Value = (SUCCEEDED(hr) && V_VT(&Result) == VT_I4) ? V_I4(&Result) : -1;
    VariantClear(&Result);
    return hr;
}

static void CheckObject(IDispatch *Dispatch, APTTYPE OuterType, APTTYPEQUALIFIER Qualifier, BOOL CallBackSameThread, int Line)
{
    APTTYPEQUALIFIER AfterQualifier;
    APTTYPE AfterType;
    LONG Value;
    HRESULT hr;

    hr = InvokeLong(Dispatch, NEUTRAL_DISPID_THREAD, NULL, &Value);
    ok_(__FILE__, Line)(hr == S_OK, "thread: hr %#lx\n", hr);
    ok_(__FILE__, Line)((DWORD)Value == GetCurrentThreadId(), "the call ran on thread %#lx, the caller is %#lx\n", Value, GetCurrentThreadId());

    hr = InvokeLong(Dispatch, NEUTRAL_DISPID_TYPE, NULL, &Value);
    ok_(__FILE__, Line)(hr == S_OK && Value == APTTYPE_NA, "type in the call: hr %#lx, %ld\n", hr, Value);
    hr = InvokeLong(Dispatch, NEUTRAL_DISPID_QUALIFIER, NULL, &Value);
    ok_(__FILE__, Line)(hr == S_OK && Value == Qualifier, "qualifier in the call: hr %#lx, %ld, expected %d\n", hr, Value, Qualifier);

    hr = CoGetApartmentType(&AfterType, &AfterQualifier);
    ok_(__FILE__, Line)(hr == S_OK && AfterType == OuterType, "type after the call: hr %#lx, %d, expected %d\n", hr, AfterType, OuterType);

    CallbackThread = 0;
    CallbackType = APTTYPE_CURRENT;
    hr = InvokeLong(Dispatch, NEUTRAL_DISPID_CALLBACK, &Callback, &Value);
    ok_(__FILE__, Line)(hr == S_OK, "callback: hr %#lx\n", hr);
    ok_(__FILE__, Line)(CallbackType == OuterType, "the callback ran in apartment type %d, expected %d\n", CallbackType, OuterType);
    if (CallBackSameThread)
        ok_(__FILE__, Line)(CallbackThread == GetCurrentThreadId(), "the callback ran on thread %#lx, the caller is %#lx\n", CallbackThread, GetCurrentThreadId());

    hr = InvokeLong(Dispatch, NEUTRAL_DISPID_INIT_STA, NULL, &Value);
    ok_(__FILE__, Line)(hr == S_OK && Value == RPC_E_CHANGED_MODE, "CoInitializeEx(STA) in the neutral apartment: hr %#lx, %#lx\n", hr, Value);
    hr = InvokeLong(Dispatch, NEUTRAL_DISPID_INIT_MTA, NULL, &Value);
    ok_(__FILE__, Line)(hr == S_OK && Value == RPC_E_CHANGED_MODE, "CoInitializeEx(MTA) in the neutral apartment: hr %#lx, %#lx\n", hr, Value);
    hr = InvokeLong(Dispatch, NEUTRAL_DISPID_TYPE, NULL, &Value);
    ok_(__FILE__, Line)(hr == S_OK && Value == APTTYPE_NA, "type after CoUninitialize in the call: hr %#lx, %ld\n", hr, Value);
    hr = CoGetApartmentType(&AfterType, &AfterQualifier);
    ok_(__FILE__, Line)(hr == S_OK && AfterType == OuterType, "type after CoUninitialize in the call: hr %#lx, %d, expected %d\n", hr, AfterType, OuterType);

    hr = InvokeLong(Dispatch, NEUTRAL_DISPID_DISCONNECT, NULL, &Value);
    ok_(__FILE__, Line)(hr == S_OK && Value == CO_E_NOT_SUPPORTED, "CoDisconnectContext in the neutral apartment: hr %#lx, %#lx\n", hr, Value);
}

static IDispatch *CreateAndCheck(APTTYPE OuterType, APTTYPEQUALIFIER Qualifier, BOOL CallBackSameThread, int Line)
{
    IDispatch *Dispatch = NULL;
    LONG Value;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_NeutralTest, NULL, CLSCTX_INPROC_SERVER, &IID_IDispatch, (void **)&Dispatch);
    ok_(__FILE__, Line)(hr == S_OK, "CoCreateInstance: hr %#lx\n", hr);
    if (hr != S_OK)
        return NULL;

    hr = InvokeLong(Dispatch, NEUTRAL_DISPID_CREATED_TYPE, NULL, &Value);
    ok_(__FILE__, Line)(hr == S_OK && Value == APTTYPE_NA, "type at creation: hr %#lx, %ld\n", hr, Value);
    hr = InvokeLong(Dispatch, NEUTRAL_DISPID_CREATED_QUALIFIER, NULL, &Value);
    ok_(__FILE__, Line)(hr == S_OK && Value == Qualifier, "qualifier at creation: hr %#lx, %ld, expected %d\n", hr, Value, Qualifier);

    CheckObject(Dispatch, OuterType, Qualifier, CallBackSameThread, Line);
    return Dispatch;
}

struct thread_test
{
    DWORD Init;
    BOOL Initialize;
    APTTYPE OuterType;
    APTTYPEQUALIFIER Qualifier;
    IStream *Marshaled;
    HANDLE Ready;
    HANDLE Done;
};

static DWORD WINAPI ThreadTest(void *Parameter)
{
    struct thread_test *Test = Parameter;
    IDispatch *Dispatch;
    HRESULT hr;

    if (Test->Initialize)
    {
        hr = CoInitializeEx(NULL, Test->Init);
        ok(hr == S_OK, "CoInitializeEx: hr %#lx\n", hr);
    }

    if (Test->Marshaled)
    {
        hr = CoGetInterfaceAndReleaseStream(Test->Marshaled, &IID_IDispatch, (void **)&Dispatch);
        ok(hr == S_OK, "CoGetInterfaceAndReleaseStream: hr %#lx\n", hr);
        if (hr == S_OK)
        {
            CheckObject(Dispatch, Test->OuterType, Test->Qualifier, Test->OuterType != APTTYPE_MTA, __LINE__);
            IDispatch_Release(Dispatch);
        }
    }
    else if ((Dispatch = CreateAndCheck(Test->OuterType, Test->Qualifier, Test->OuterType != APTTYPE_MTA, __LINE__)))
    {
        IDispatch_Release(Dispatch);
    }

    if (Test->Initialize)
        CoUninitialize();
    return 0;
}

static void RunThread(struct thread_test *Test)
{
    HANDLE Thread;
    DWORD Index;

    Thread = CreateThread(NULL, 0, ThreadTest, Test, 0, NULL);
    ok(Thread != NULL, "CreateThread failed, error %lu\n", GetLastError());
    if (!Thread)
        return;
    while (CoWaitForMultipleHandles(0, 30000, 1, &Thread, &Index) == S_OK && WaitForSingleObject(Thread, 0))
        ;
    ok(WaitForSingleObject(Thread, 0) == WAIT_OBJECT_0, "the test thread did not finish\n");
    CloseHandle(Thread);
}

static DWORD WINAPI MtaKeeper(void *Parameter)
{
    struct thread_test *Test = Parameter;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetEvent(Test->Ready);
    WaitForSingleObject(Test->Done, INFINITE);
    CoUninitialize();
    return 0;
}

static HRESULT CALLBACK ContextCallbackFunction(ComCallData *Data)
{
    CallbackThread = GetCurrentThreadId();
    if (FAILED(CoGetApartmentType(&CallbackType, &CallbackQualifier)))
        CallbackType = APTTYPE_CURRENT;
    return (HRESULT)Data->dwReserved;
}

static void TestContextCallback(APTTYPE Type, APTTYPE ExpectedType, APTTYPEQUALIFIER ExpectedQualifier, BOOL SameThread, int Line)
{
    IContextCallback *Context = NULL;
    ComCallData Data = { 0, 0x1234, NULL };
    HRESULT hr;

    hr = CoGetDefaultContext(Type, &IID_IContextCallback, (void **)&Context);
    ok_(__FILE__, Line)(hr == S_OK, "CoGetDefaultContext(%d): hr %#lx\n", Type, hr);
    if (hr != S_OK)
        return;

    CallbackThread = 0;
    CallbackType = APTTYPE_CURRENT;
    hr = IContextCallback_ContextCallback(Context, ContextCallbackFunction, &Data, &IID_IContextCallback, 2, NULL);
    ok_(__FILE__, Line)(hr == 0x1234, "ContextCallback(%d): hr %#lx\n", Type, hr);
    ok_(__FILE__, Line)(CallbackType == ExpectedType, "the callback ran in apartment type %d, expected %d\n", CallbackType, ExpectedType);
    if (ExpectedType == APTTYPE_NA)
        ok_(__FILE__, Line)(CallbackQualifier == ExpectedQualifier, "qualifier %d, expected %d\n", CallbackQualifier, ExpectedQualifier);
    if (SameThread)
        ok_(__FILE__, Line)(CallbackThread == GetCurrentThreadId(), "the callback ran on thread %#lx, the caller is %#lx\n", CallbackThread, GetCurrentThreadId());
    IContextCallback_Release(Context);
}

static void TestDefaultContext(void)
{
    IUnknown *Default, *Current;
    void *Object;
    HRESULT hr;

    Object = (void *)(ULONG_PTR)0xdeadbeef;
    hr = CoGetDefaultContext(APTTYPE_STA, &IID_IContextCallback, &Object);
    ok(hr == E_INVALIDARG, "APTTYPE_STA: hr %#lx\n", hr);
    ok(Object == NULL, "Object is %p\n", Object);
    hr = CoGetDefaultContext((APTTYPE)42, &IID_IContextCallback, &Object);
    ok(hr == E_INVALIDARG, "an unknown type: hr %#lx\n", hr);

    hr = CoGetDefaultContext(APTTYPE_CURRENT, &IID_IUnknown, (void **)&Default);
    ok(hr == S_OK, "APTTYPE_CURRENT: hr %#lx\n", hr);
    hr = CoGetObjectContext(&IID_IUnknown, (void **)&Current);
    ok(hr == S_OK, "CoGetObjectContext: hr %#lx\n", hr);
    if (Default && Current)
        ok(Default == Current, "the default context %p is not the object context %p\n", Default, Current);
    if (Current)
        IUnknown_Release(Current);
    if (Default)
        IUnknown_Release(Default);

    Object = (void *)(ULONG_PTR)0xdeadbeef;
    hr = CoGetDefaultContext(APTTYPE_MTA, &IID_IContextCallback, &Object);
    ok(hr == CO_E_NOTINITIALIZED, "APTTYPE_MTA without an MTA: hr %#lx\n", hr);
    ok(Object == NULL, "Object is %p\n", Object);

    hr = CoGetDefaultContext(APTTYPE_MAINSTA, &IID_IUnknown, (void **)&Default);
    ok(hr == S_OK, "APTTYPE_MAINSTA: hr %#lx\n", hr);
    if (hr == S_OK)
        IUnknown_Release(Default);

    TestContextCallback(APTTYPE_CURRENT, APTTYPE_MAINSTA, APTTYPEQUALIFIER_NONE, TRUE, __LINE__);
    TestContextCallback(APTTYPE_NA, APTTYPE_NA, APTTYPEQUALIFIER_NA_ON_MAINSTA, TRUE, __LINE__);

    hr = CoDisconnectContext(0);
    ok(hr == CO_E_NOT_SUPPORTED, "CoDisconnectContext in the default context of the main STA: hr %#lx\n", hr);
}

static DWORD WINAPI UninitializedThread(void *Parameter)
{
    void *Object = (void *)(ULONG_PTR)0xdeadbeef;
    HRESULT hr;

    hr = CoGetDefaultContext(APTTYPE_CURRENT, &IID_IContextCallback, &Object);
    ok(hr == CO_E_NOTINITIALIZED, "CoGetDefaultContext without COM: hr %#lx\n", hr);
    ok(Object == NULL, "Object is %p\n", Object);
    hr = CoDisconnectContext(0);
    ok(hr == RPC_E_NO_CONTEXT, "CoDisconnectContext without COM: hr %#lx\n", hr);
    return 0;
}

START_TEST(neutral)
{
    struct thread_test Test = { 0 };
    IContextCallback *Context;
    IDispatch *Dispatch;
    HANDLE Thread;
    HRESULT hr;

    RemoveRegistration();
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok(hr == S_OK, "CoInitializeEx: hr %#lx\n", hr);

    Thread = CreateThread(NULL, 0, UninitializedThread, NULL, 0, NULL);
    WaitForSingleObject(Thread, INFINITE);
    CloseHandle(Thread);

    TestDefaultContext();

    if (!AddRegistration())
    {
        skip("the test server %ls was not found next to the test\n", NEUTRAL_SERVER_NAME);
        RemoveRegistration();
        CoUninitialize();
        return;
    }

    Dispatch = CreateAndCheck(APTTYPE_MAINSTA, APTTYPEQUALIFIER_NA_ON_MAINSTA, TRUE, __LINE__);

    Test.Initialize = TRUE;
    Test.Init = COINIT_APARTMENTTHREADED;
    Test.OuterType = APTTYPE_STA;
    Test.Qualifier = APTTYPEQUALIFIER_NA_ON_STA;
    RunThread(&Test);

    Test.Init = COINIT_MULTITHREADED;
    Test.OuterType = APTTYPE_MTA;
    Test.Qualifier = APTTYPEQUALIFIER_NA_ON_MTA;
    RunThread(&Test);

    if (Dispatch)
    {
        hr = CoMarshalInterThreadInterfaceInStream(&IID_IDispatch, (IUnknown *)Dispatch, &Test.Marshaled);
        ok(hr == S_OK, "CoMarshalInterThreadInterfaceInStream: hr %#lx\n", hr);
        if (hr == S_OK)
            RunThread(&Test);
        Test.Marshaled = NULL;
    }

    Test.Ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    Test.Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    Thread = CreateThread(NULL, 0, MtaKeeper, &Test, 0, NULL);
    WaitForSingleObject(Test.Ready, INFINITE);

    hr = CoGetDefaultContext(APTTYPE_MTA, &IID_IContextCallback, (void **)&Context);
    ok(hr == S_OK, "APTTYPE_MTA with an MTA: hr %#lx\n", hr);
    if (hr == S_OK)
        IContextCallback_Release(Context);

    Test.Initialize = FALSE;
    Test.OuterType = APTTYPE_MTA;
    Test.Qualifier = APTTYPEQUALIFIER_NA_ON_IMPLICIT_MTA;
    RunThread(&Test);

    SetEvent(Test.Done);
    WaitForSingleObject(Thread, INFINITE);
    CloseHandle(Thread);
    CloseHandle(Test.Ready);
    CloseHandle(Test.Done);

    if (Dispatch)
        IDispatch_Release(Dispatch);
    RemoveRegistration();
    CoUninitialize();
}
