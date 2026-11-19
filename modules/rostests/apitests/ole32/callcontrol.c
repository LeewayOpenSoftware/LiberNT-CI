/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test COM call control: cancellation, caller thread, waits, suspended classes
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <objbase.h>

#include <wine/test.h>

enum
{
    MODE_RECORD,
    MODE_WAIT_CANCEL,
    MODE_IGNORE_CANCEL,
};

static volatile LONG Mode;
static HRESULT CallerHr, TestCancelHr;
static DWORD CallerTid;
static HANDLE ServerEntered, ServerDone, StopServer;
static LONG FactoryLocks;

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
    UINT i;

    *Object = NULL;
    switch (Mode)
    {
    case MODE_RECORD:
        CallerTid = 0xdeadbeef;
        CallerHr = CoGetCallerTID(&CallerTid);
        TestCancelHr = CoTestCancel();
        return E_NOTIMPL;

    case MODE_WAIT_CANCEL:
        TestCancelHr = E_FAIL;
        SetEvent(ServerEntered);
        for (i = 0; i < 1000; i++)
        {
            TestCancelHr = CoTestCancel();
            if (TestCancelHr != RPC_S_CALLPENDING || !WaitForSingleObject(StopServer, 0))
                break;
            Sleep(10);
        }
        SetEvent(ServerDone);
        return E_ABORT;

    case MODE_IGNORE_CANCEL:
        SetEvent(ServerEntered);
        Sleep(3000);
        TestCancelHr = CoTestCancel();
        SetEvent(ServerDone);
        return E_NOTIMPL;
    }
    return E_UNEXPECTED;
}

static HRESULT WINAPI Factory_LockServer(IClassFactory *Iface, BOOL Lock)
{
    InterlockedIncrement(&FactoryLocks);
    return S_OK;
}

static IClassFactoryVtbl FactoryVtbl =
{
    Factory_QueryInterface, Factory_AddRef, Factory_Release, Factory_CreateInstance, Factory_LockServer
};
static IClassFactory Factory = { &FactoryVtbl };

/* A cancel object of our own, for the registration functions */
typedef struct
{
    ICancelMethodCalls ICancelMethodCalls_iface;
    LONG Refs;
    ULONG Seconds;
    HRESULT TestResult;
} TEST_CANCEL;

static TEST_CANCEL *impl_from_ICancelMethodCalls(ICancelMethodCalls *Iface)
{
    return CONTAINING_RECORD(Iface, TEST_CANCEL, ICancelMethodCalls_iface);
}

static HRESULT WINAPI TestCancel_QueryInterface(ICancelMethodCalls *Iface, REFIID Riid, void **Object)
{
    if (IsEqualIID(Riid, &IID_IUnknown) || IsEqualIID(Riid, &IID_ICancelMethodCalls))
    {
        *Object = Iface;
        ICancelMethodCalls_AddRef(Iface);
        return S_OK;
    }
    *Object = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI TestCancel_AddRef(ICancelMethodCalls *Iface)
{
    return InterlockedIncrement(&impl_from_ICancelMethodCalls(Iface)->Refs);
}

static ULONG WINAPI TestCancel_Release(ICancelMethodCalls *Iface)
{
    return InterlockedDecrement(&impl_from_ICancelMethodCalls(Iface)->Refs);
}

static HRESULT WINAPI TestCancel_Cancel(ICancelMethodCalls *Iface, ULONG Seconds)
{
    impl_from_ICancelMethodCalls(Iface)->Seconds = Seconds;
    return S_OK;
}

static HRESULT WINAPI TestCancel_TestCancel(ICancelMethodCalls *Iface)
{
    return impl_from_ICancelMethodCalls(Iface)->TestResult;
}

static ICancelMethodCallsVtbl TestCancelVtbl =
{
    TestCancel_QueryInterface, TestCancel_AddRef, TestCancel_Release, TestCancel_Cancel, TestCancel_TestCancel
};

static DWORD WINAPI OtherThreadGetCancel(void *Param)
{
    IUnknown *Object;
    return (DWORD)CoGetCancelObject(*(DWORD *)Param, &IID_ICancelMethodCalls, (void **)&Object);
}

static void TestCancelRegistration(void)
{
    TEST_CANCEL First = { { &TestCancelVtbl }, 1, 0, RPC_S_CALLPENDING };
    TEST_CANCEL Second = { { &TestCancelVtbl }, 1, 0, RPC_E_CALL_CANCELED };
    DWORD Tid = GetCurrentThreadId(), Result = 0;
    IUnknown *Object;
    IUnknown *Old = NULL;
    HANDLE Thread;
    HRESULT hr;

    hr = CoGetCancelObject(0, &IID_ICancelMethodCalls, (void **)&Object);
    ok(hr == E_NOINTERFACE, "CoGetCancelObject without an object returned 0x%lx\n", hr);
    hr = CoCancelCall(0, 0);
    ok(hr == E_NOINTERFACE, "CoCancelCall without an object returned 0x%lx\n", hr);
    hr = CoSetCancelObject((IUnknown *)&Factory);
    ok(hr == E_NOINTERFACE, "CoSetCancelObject without ICancelMethodCalls returned 0x%lx\n", hr);

    hr = CoSetCancelObject((IUnknown *)&First.ICancelMethodCalls_iface);
    ok(hr == S_OK, "CoSetCancelObject returned 0x%lx\n", hr);
    ok(First.Refs == 2, "registration holds %ld references\n", First.Refs - 1);
    hr = CoSetCancelObject((IUnknown *)&Second.ICancelMethodCalls_iface);
    ok(hr == S_OK, "second CoSetCancelObject returned 0x%lx\n", hr);

    /* The innermost object answers, for this thread only */
    hr = CoGetCancelObject(Tid, &IID_ICancelMethodCalls, (void **)&Object);
    ok(hr == S_OK && Object == (IUnknown *)&Second.ICancelMethodCalls_iface, "CoGetCancelObject returned 0x%lx, %p\n",
       hr, Object);
    if (hr == S_OK)
        IUnknown_Release(Object);
    hr = CoGetCancelObject(0, &IID_IClassFactory, (void **)&Object);
    ok(hr == E_NOINTERFACE, "CoGetCancelObject for another interface returned 0x%lx\n", hr);
    Thread = CreateThread(NULL, 0, OtherThreadGetCancel, &Result, 0, NULL);
    WaitForSingleObject(Thread, 5000);
    GetExitCodeThread(Thread, &Result);
    CloseHandle(Thread);
    ok(Result == (DWORD)E_NOINTERFACE, "another thread saw a cancel object, 0x%lx\n", Result);

    hr = CoCancelCall(Tid, 7);
    ok(hr == S_OK, "CoCancelCall returned 0x%lx\n", hr);
    ok(Second.Seconds == 7 && First.Seconds == 0, "Cancel reached %lu / %lu\n", First.Seconds, Second.Seconds);

    hr = CoSetCancelObject(NULL);
    ok(hr == S_OK, "resetting the cancel object returned 0x%lx\n", hr);
    ok(Second.Refs == 1, "reset left %ld references\n", Second.Refs - 1);
    hr = CoGetCancelObject(0, &IID_ICancelMethodCalls, (void **)&Object);
    ok(hr == S_OK && Object == (IUnknown *)&First.ICancelMethodCalls_iface, "outer object not back, 0x%lx\n", hr);
    if (hr == S_OK)
        IUnknown_Release(Object);
    CoSetCancelObject(NULL);
    ok(First.Refs == 1, "reset left %ld references\n", First.Refs - 1);
    hr = CoGetCancelObject(0, &IID_ICancelMethodCalls, (void **)&Object);
    ok(hr == E_NOINTERFACE, "CoGetCancelObject after reset returned 0x%lx\n", hr);

    /* Outside a call; a context installed by the caller answers CoTestCancel */
    hr = CoTestCancel();
    ok(hr != RPC_S_CALLPENDING && hr != RPC_E_CALL_CANCELED, "CoTestCancel outside a call returned 0x%lx\n", hr);
    hr = CoSwitchCallContext((IUnknown *)&Second.ICancelMethodCalls_iface, &Old);
    ok(hr == S_OK, "CoSwitchCallContext returned 0x%lx\n", hr);
    hr = CoTestCancel();
    ok(hr == RPC_E_CALL_CANCELED, "CoTestCancel with a context returned 0x%lx\n", hr);
    CoSwitchCallContext(Old, &Old);

    hr = CoGetCallerTID(&Result);
    ok(hr != S_OK && hr != S_FALSE, "CoGetCallerTID outside a call returned 0x%lx\n", hr);
}

typedef struct
{
    DWORD Tid;
    ULONG Timeout;
    HRESULT First, Second;
} CANCEL_JOB;

static DWORD WINAPI Canceler(void *Param)
{
    CANCEL_JOB *Job = Param;

    if (WaitForSingleObject(ServerEntered, 10000))
        return 1;
    Sleep(50);
    Job->First = CoCancelCall(Job->Tid, Job->Timeout);
    Job->Second = CoCancelCall(Job->Tid, Job->Timeout);
    SetEvent(StopServer);
    return 0;
}

static HRESULT CallWithCanceler(const char *Stage, IClassFactory *Proxy, LONG CallMode, CANCEL_JOB *Job,
                                DWORD *Elapsed)
{
    IUnknown *Object;
    HANDLE Thread;
    DWORD Start;
    HRESULT hr;

    ResetEvent(ServerEntered);
    ResetEvent(ServerDone);
    ResetEvent(StopServer);
    Mode = CallMode;
    Job->Tid = GetCurrentThreadId();
    Job->First = Job->Second = E_FAIL;
    Thread = CreateThread(NULL, 0, Canceler, Job, 0, NULL);
    Start = GetTickCount();
    hr = IClassFactory_CreateInstance(Proxy, NULL, &IID_IUnknown, (void **)&Object);
    *Elapsed = GetTickCount() - Start;
    ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "%s: canceler did not end\n", Stage);
    CloseHandle(Thread);
    ok(WaitForSingleObject(ServerDone, 10000) == WAIT_OBJECT_0, "%s: server call did not end\n", Stage);
    return hr;
}

static void RunCalls(const char *Stage, IClassFactory *Proxy, BOOL Sta)
{
    IUnknown *Object;
    CANCEL_JOB Job = { 0 };
    DWORD Elapsed;
    HRESULT hr;

    Mode = MODE_RECORD;
    hr = IClassFactory_CreateInstance(Proxy, NULL, &IID_IUnknown, (void **)&Object);
    ok(hr == E_NOTIMPL, "%s: record call returned 0x%lx\n", Stage, hr);
    ok(CallerHr == S_OK, "%s: CoGetCallerTID returned 0x%lx\n", Stage, CallerHr);
    ok(CallerTid == (Sta ? GetCurrentThreadId() : 0), "%s: caller apartment %lu\n", Stage, CallerTid);
    ok(TestCancelHr == RPC_S_CALLPENDING, "%s: CoTestCancel in a call returned 0x%lx\n", Stage, TestCancelHr);

    /* Without CoEnableCallCancellation the call cannot be canceled */
    Job.Timeout = 5;
    hr = CallWithCanceler(Stage, Proxy, MODE_WAIT_CANCEL, &Job, &Elapsed);
    ok(Job.First == CO_E_CANCEL_DISABLED, "%s: CoCancelCall while disabled returned 0x%lx\n", Stage, Job.First);
    ok(TestCancelHr == RPC_S_CALLPENDING, "%s: server saw 0x%lx while disabled\n", Stage, TestCancelHr);
    ok(hr == E_ABORT, "%s: disabled call returned 0x%lx\n", Stage, hr);

    hr = CoEnableCallCancellation(NULL);
    ok(hr == S_OK, "%s: CoEnableCallCancellation returned 0x%lx\n", Stage, hr);

    /* The server notices and returns within the caller's timeout */
    hr = CallWithCanceler(Stage, Proxy, MODE_WAIT_CANCEL, &Job, &Elapsed);
    ok(Job.First == S_OK, "%s: CoCancelCall returned 0x%lx\n", Stage, Job.First);
    ok(TestCancelHr == RPC_E_CALL_CANCELED, "%s: server saw 0x%lx\n", Stage, TestCancelHr);
    ok(hr == E_ABORT || hr == RPC_E_CALL_CANCELED, "%s: canceled call returned 0x%lx\n", Stage, hr);

    /* A server that does not look is abandoned after the timeout */
    Job.Timeout = 1;
    hr = CallWithCanceler(Stage, Proxy, MODE_IGNORE_CANCEL, &Job, &Elapsed);
    ok(Job.First == S_OK, "%s: CoCancelCall returned 0x%lx\n", Stage, Job.First);
    ok(Job.Second == RPC_E_CALL_CANCELED, "%s: second CoCancelCall returned 0x%lx\n", Stage, Job.Second);
    ok(hr == RPC_E_CALL_CANCELED, "%s: abandoned call returned 0x%lx\n", Stage, hr);
    ok(Elapsed < 2800, "%s: abandoned call took %lu ms\n", Stage, Elapsed);
    ok(TestCancelHr == RPC_E_CALL_CANCELED, "%s: late server saw 0x%lx\n", Stage, TestCancelHr);

    hr = CoDisableCallCancellation(NULL);
    ok(hr == S_OK, "%s: CoDisableCallCancellation returned 0x%lx\n", Stage, hr);
    hr = CoDisableCallCancellation(NULL);
    ok(hr == CO_E_CANCEL_DISABLED, "%s: extra CoDisableCallCancellation returned 0x%lx\n", Stage, hr);

    /* The proxy still works after an abandoned call */
    Sleep(200);
    Mode = MODE_RECORD;
    hr = IClassFactory_CreateInstance(Proxy, NULL, &IID_IUnknown, (void **)&Object);
    ok(hr == E_NOTIMPL, "%s: call after abandon returned 0x%lx\n", Stage, hr);
}

typedef struct
{
    IStream *Stream;
    DWORD Model;
    const char *Stage;
} CLIENT_ARGS;

static DWORD WINAPI ClientThread(void *Param)
{
    CLIENT_ARGS *Args = Param;
    IClassFactory *Proxy;
    HRESULT hr;

    CoInitializeEx(NULL, Args->Model);
    hr = CoUnmarshalInterface(Args->Stream, &IID_IClassFactory, (void **)&Proxy);
    ok(hr == S_OK, "%s: CoUnmarshalInterface returned 0x%lx\n", Args->Stage, hr);
    if (SUCCEEDED(hr))
    {
        RunCalls(Args->Stage, Proxy, Args->Model == COINIT_APARTMENTTHREADED);
        IClassFactory_Release(Proxy);
    }
    CoUninitialize();
    return 0;
}

static void CreateEvents(void)
{
    ServerEntered = CreateEventW(NULL, TRUE, FALSE, NULL);
    ServerDone = CreateEventW(NULL, TRUE, FALSE, NULL);
    StopServer = CreateEventW(NULL, TRUE, FALSE, NULL);
}

/* Server in this STA, client in the MTA: the call is posted to the apartment window */
static void ChildPosted(void)
{
    LARGE_INTEGER Zero = { { 0 } };
    CLIENT_ARGS Args = { NULL, COINIT_MULTITHREADED, "posted" };
    HANDLE Thread;
    DWORD Index;
    HRESULT hr;

    CreateEvents();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    TestCancelRegistration();
    CreateStreamOnHGlobal(NULL, TRUE, &Args.Stream);
    hr = CoMarshalInterface(Args.Stream, &IID_IClassFactory, (IUnknown *)&Factory, MSHCTX_INPROC, NULL,
                            MSHLFLAGS_NORMAL);
    ok(hr == S_OK, "posted: CoMarshalInterface returned 0x%lx\n", hr);
    IStream_Seek(Args.Stream, Zero, STREAM_SEEK_SET, NULL);
    Thread = CreateThread(NULL, 0, ClientThread, &Args, 0, NULL);
    /* Incoming calls run while an STA waits in CoWaitForMultipleObjects */
    hr = CoWaitForMultipleObjects(CWMO_DISPATCH_CALLS, 60000, 1, &Thread, &Index);
    ok(hr == S_OK && Index == 0, "posted: wait returned 0x%lx, %lu\n", hr, Index);
    CloseHandle(Thread);
    IStream_Release(Args.Stream);
    CoUninitialize();
}

/* Server in this MTA, client in an STA: the call goes through the RPC runtime */
static void ChildRpc(void)
{
    LARGE_INTEGER Zero = { { 0 } };
    CLIENT_ARGS Args = { NULL, COINIT_APARTMENTTHREADED, "rpc" };
    HANDLE Thread;
    HRESULT hr;

    CreateEvents();
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    TestCancelRegistration();
    CreateStreamOnHGlobal(NULL, TRUE, &Args.Stream);
    hr = CoMarshalInterface(Args.Stream, &IID_IClassFactory, (IUnknown *)&Factory, MSHCTX_INPROC, NULL,
                            MSHLFLAGS_NORMAL);
    ok(hr == S_OK, "rpc: CoMarshalInterface returned 0x%lx\n", hr);
    IStream_Seek(Args.Stream, Zero, STREAM_SEEK_SET, NULL);
    Thread = CreateThread(NULL, 0, ClientThread, &Args, 0, NULL);
    ok(WaitForSingleObject(Thread, 60000) == WAIT_OBJECT_0, "rpc: client did not end\n");
    CloseHandle(Thread);
    IStream_Release(Args.Stream);
    CoUninitialize();
}

static LONG WindowMessages;

static LRESULT CALLBACK CountProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    if (Message == WM_USER)
        InterlockedIncrement(&WindowMessages);
    return DefWindowProcW(Window, Message, WParam, LParam);
}

static void ChildWait(void)
{
    WNDCLASSW Class = { 0 };
    HANDLE Events[2];
    HWND Window;
    DWORD Index;
    MSG Msg;
    HRESULT hr;

    Events[0] = CreateEventW(NULL, TRUE, FALSE, NULL);
    Events[1] = CreateEventW(NULL, TRUE, TRUE, NULL);

    hr = CoWaitForMultipleObjects(4, 0, 1, Events, &Index);
    ok(hr == E_INVALIDARG, "unknown flag returned 0x%lx\n", hr);
    hr = CoWaitForMultipleObjects(CWMO_DEFAULT, 0, CWMO_MAX_HANDLES + 1, Events, &Index);
    ok(hr == E_INVALIDARG, "too many handles returned 0x%lx\n", hr);
    hr = CoWaitForMultipleObjects(CWMO_DEFAULT, 0, 1, Events, NULL);
    ok(hr == E_INVALIDARG, "no index without COM returned 0x%lx\n", hr);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = CoWaitForMultipleObjects(CWMO_DEFAULT, 0, 1, Events, NULL);
    ok(hr == E_INVALIDARG, "no index returned 0x%lx\n", hr);
    hr = CoWaitForMultipleObjects(CWMO_DEFAULT, 0, 0, Events, &Index);
    ok(hr == RPC_E_NO_SYNC, "no handles returned 0x%lx\n", hr);
    Index = 5;
    hr = CoWaitForMultipleObjects(CWMO_DEFAULT, 1000, 2, Events, &Index);
    ok(hr == S_OK && Index == 1, "signaled wait returned 0x%lx, %lu\n", hr, Index);
    hr = CoWaitForMultipleObjects(CWMO_DEFAULT, 50, 1, Events, &Index);
    ok(hr == RPC_S_CALLPENDING, "timed-out wait returned 0x%lx\n", hr);

    /* An STA leaves window messages alone unless asked */
    Class.lpfnWndProc = CountProc;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.lpszClassName = L"CallControlCount";
    RegisterClassW(&Class);
    Window = CreateWindowExW(0, Class.lpszClassName, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    ok(Window != NULL, "CreateWindowExW failed with error %lu\n", GetLastError());
    PostMessageW(Window, WM_USER, 0, 0);
    hr = CoWaitForMultipleObjects(CWMO_DEFAULT, 100, 1, Events, &Index);
    ok(hr == RPC_S_CALLPENDING, "default wait returned 0x%lx\n", hr);
    ok(WindowMessages == 0, "default wait dispatched %ld messages\n", WindowMessages);
    ok(PeekMessageW(&Msg, Window, WM_USER, WM_USER, PM_REMOVE), "message left the queue\n");
    PostMessageW(Window, WM_USER, 0, 0);
    hr = CoWaitForMultipleObjects(CWMO_DISPATCH_WINDOW_MESSAGES, 100, 1, Events, &Index);
    ok(hr == RPC_S_CALLPENDING, "dispatching wait returned 0x%lx\n", hr);
    ok(WindowMessages == 1, "dispatching wait dispatched %ld messages\n", WindowMessages);

    /* A wait does not consume WM_QUIT */
    PostQuitMessage(3);
    hr = CoWaitForMultipleObjects(CWMO_DEFAULT, 50, 1, Events, &Index);
    ok(hr == RPC_S_CALLPENDING, "wait with WM_QUIT returned 0x%lx\n", hr);
    ok(PeekMessageW(&Msg, NULL, WM_QUIT, WM_QUIT, PM_REMOVE) && Msg.wParam == 3, "WM_QUIT was lost\n");
    DestroyWindow(Window);
    CoUninitialize();

    /* An MTA ignores the flags */
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoWaitForMultipleObjects(CWMO_DEFAULT, 0, 1, Events, NULL);
    ok(hr == E_INVALIDARG, "MTA no index returned 0x%lx\n", hr);
    hr = CoWaitForMultipleObjects(CWMO_DISPATCH_CALLS | CWMO_DISPATCH_WINDOW_MESSAGES, 1000, 2, Events, &Index);
    ok(hr == S_OK && Index == 1, "MTA wait returned 0x%lx, %lu\n", hr, Index);
    hr = CoWaitForMultipleObjects(CWMO_DEFAULT, 50, 1, Events, &Index);
    ok(hr == RPC_S_CALLPENDING, "MTA timed-out wait returned 0x%lx\n", hr);
    CoUninitialize();
    CloseHandle(Events[0]);
    CloseHandle(Events[1]);
}

/* {6f2f4b3e-8a4c-4b8e-9d6e-1c2b3a4d5e6f} */
static const CLSID CLSID_CallControlTest = { 0x6f2f4b3e, 0x8a4c, 0x4b8e, { 0x9d, 0x6e, 0x1c, 0x2b, 0x3a, 0x4d, 0x5e, 0x6f } };

static void ChildSuspend(void)
{
    IClassFactory *Object;
    DWORD Cookie;
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = CoRegisterClassObject(&CLSID_CallControlTest, (IUnknown *)&Factory, CLSCTX_LOCAL_SERVER,
                               REGCLS_MULTIPLEUSE | REGCLS_SUSPENDED, &Cookie);
    ok(hr == S_OK, "CoRegisterClassObject returned 0x%lx\n", hr);
    if (FAILED(hr))
    {
        CoUninitialize();
        return;
    }

    hr = CoGetClassObject(&CLSID_CallControlTest, CLSCTX_LOCAL_SERVER, NULL, &IID_IClassFactory, (void **)&Object);
    ok(FAILED(hr), "a suspended class was handed out, 0x%lx\n", hr);
    if (SUCCEEDED(hr))
        IClassFactory_Release(Object);

    hr = CoResumeClassObjects();
    ok(hr == S_OK, "CoResumeClassObjects returned 0x%lx\n", hr);
    hr = CoGetClassObject(&CLSID_CallControlTest, CLSCTX_LOCAL_SERVER, NULL, &IID_IClassFactory, (void **)&Object);
    ok(hr == S_OK, "a resumed class was not handed out, 0x%lx\n", hr);
    if (SUCCEEDED(hr))
        IClassFactory_Release(Object);

    hr = CoSuspendClassObjects();
    ok(hr == S_OK, "CoSuspendClassObjects returned 0x%lx\n", hr);
    hr = CoGetClassObject(&CLSID_CallControlTest, CLSCTX_LOCAL_SERVER, NULL, &IID_IClassFactory, (void **)&Object);
    ok(FAILED(hr), "a class suspended again was handed out, 0x%lx\n", hr);
    if (SUCCEEDED(hr))
        IClassFactory_Release(Object);

    hr = CoRevokeClassObject(Cookie);
    ok(hr == S_OK, "CoRevokeClassObject returned 0x%lx\n", hr);
    CoUninitialize();
}

/* The client of the cross-process call: one call from its MTA */
static void ChildRemoteClient(HANDLE Pipe)
{
    DWORD Size = 0, Got, Total = 0;
    IClassFactory *Proxy;
    IUnknown *Object;
    HGLOBAL Global;
    IStream *Stream;
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (ReadFile(Pipe, &Size, sizeof(Size), &Got, NULL) && Got == sizeof(Size) && Size && Size < 0x10000 &&
        (Global = GlobalAlloc(GMEM_MOVEABLE, Size)))
    {
        BYTE *Data = GlobalLock(Global);
        while (Total < Size && ReadFile(Pipe, Data + Total, Size - Total, &Got, NULL) && Got)
            Total += Got;
        GlobalUnlock(Global);
        CreateStreamOnHGlobal(Global, TRUE, &Stream);
        hr = CoUnmarshalInterface(Stream, &IID_IClassFactory, (void **)&Proxy);
        ok(hr == S_OK, "remote: CoUnmarshalInterface returned 0x%lx\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = IClassFactory_CreateInstance(Proxy, NULL, &IID_IUnknown, (void **)&Object);
            ok(hr == E_NOTIMPL, "remote: call returned 0x%lx\n", hr);
            IClassFactory_Release(Proxy);
        }
        IStream_Release(Stream);
    }
    else
        ok(0, "remote: no marshal data\n");
    CloseHandle(Pipe);
    CoUninitialize();
}

static void TestRemoteCaller(const char *Exe)
{
    SECURITY_ATTRIBUTES Attributes = { sizeof(Attributes), NULL, TRUE };
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    char CommandLine[MAX_PATH + 64];
    HANDLE Read, Write;
    DWORD Size, Written, ExitCode, Index;
    HGLOBAL Global;
    IStream *Stream;
    HRESULT hr;

    if (!CreatePipe(&Read, &Write, &Attributes, 0))
    {
        skip("CreatePipe failed with error %lu\n", GetLastError());
        return;
    }
    SetHandleInformation(Write, HANDLE_FLAG_INHERIT, 0);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CreateStreamOnHGlobal(NULL, TRUE, &Stream);
    hr = CoMarshalInterface(Stream, &IID_IClassFactory, (IUnknown *)&Factory, MSHCTX_LOCAL, NULL, MSHLFLAGS_NORMAL);
    ok(hr == S_OK, "remote: CoMarshalInterface returned 0x%lx\n", hr);
    GetHGlobalFromStream(Stream, &Global);
    Size = (DWORD)GlobalSize(Global);
    WriteFile(Write, &Size, sizeof(Size), &Written, NULL);
    WriteFile(Write, GlobalLock(Global), Size, &Written, NULL);
    GlobalUnlock(Global);
    CloseHandle(Write);

    Mode = MODE_RECORD;
    CallerHr = E_FAIL;
    snprintf(CommandLine, sizeof(CommandLine), "\"%s\" callcontrol child remote %Iu", Exe, (ULONG_PTR)Read);
    if (CreateProcessA(NULL, CommandLine, NULL, NULL, TRUE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        hr = CoWaitForMultipleObjects(CWMO_DISPATCH_CALLS, 30000, 1, &ProcessInfo.hProcess, &Index);
        ok(hr == S_OK, "remote: child did not end, 0x%lx\n", hr);
        ok(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode) && ExitCode == 0, "remote: child had %lu failures\n",
           ExitCode);
        ok(CallerHr == S_FALSE, "remote: CoGetCallerTID returned 0x%lx\n", CallerHr);
        ok(CallerTid == 0, "remote: caller apartment %lu\n", CallerTid);
        CloseHandle(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hProcess);
    }
    else
        skip("CreateProcessA for the remote client failed with error %lu\n", GetLastError());
    CloseHandle(Read);
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
    { "posted", ChildPosted },
    { "rpc", ChildRpc },
    { "wait", ChildWait },
    { "suspend", ChildSuspend },
};

START_TEST(callcontrol)
{
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    char CommandLine[MAX_PATH + 64];
    DWORD ExitCode;
    char **Argv;
    int Argc;
    UINT i;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 5 && !strcmp(Argv[2], "child") && !strcmp(Argv[3], "remote"))
    {
        ChildRemoteClient((HANDLE)(ULONG_PTR)_strtoui64(Argv[4], NULL, 10));
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

    for (i = 0; i < ARRAYSIZE(Children); i++)
    {
        snprintf(CommandLine, sizeof(CommandLine), "\"%s\" callcontrol child %s", Argv[0], Children[i].Name);
        if (!CreateProcessA(NULL, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
        {
            skip("CreateProcessA for %s failed with error %lu\n", Children[i].Name, GetLastError());
            continue;
        }
        ok(WaitForSingleObject(ProcessInfo.hProcess, 90000) == WAIT_OBJECT_0, "Child %s did not end\n",
           Children[i].Name);
        ok(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode) && ExitCode == 0, "Child %s had %lu failures\n",
           Children[i].Name, ExitCode);
        CloseHandle(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hProcess);
    }
    TestRemoteCaller(Argv[0]);
}
