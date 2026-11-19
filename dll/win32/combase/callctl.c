/*
 * PROJECT:     LiberNT
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     COM call control: cancellation, the caller's apartment, CoWaitForMultipleObjects
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "objbase.h"

#include "wine/debug.h"
#include "wine/list.h"

#include "combase_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(ole);

/* Cancel objects registered with CoSetCancelObject, innermost first */
struct cancel_entry
{
    struct list entry;
    DWORD tid;
    IUnknown *object;
};

static struct list cancel_entries = LIST_INIT(cancel_entries);
static CRITICAL_SECTION cancel_cs;
static CRITICAL_SECTION_DEBUG cancel_cs_debug =
{
    0, 0, &cancel_cs,
    { &cancel_cs_debug.ProcessLocksList, &cancel_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": cancel_cs") }
};
static CRITICAL_SECTION cancel_cs = { &cancel_cs_debug, -1, 0, 0, 0, 0 };

enum cancel_state
{
    CANCEL_PENDING,
    CANCEL_DONE,
    CANCEL_TOO_LATE,
};

struct call_cancel
{
    ICancelMethodCalls ICancelMethodCalls_iface;
    LONG refs;
    GUID cid;
    BOOL enabled;
    LONG state;
    ULONG timeout;
    HANDLE event;
};

static const ICancelMethodCallsVtbl call_cancel_vtbl;

static inline struct call_cancel *impl_from_ICancelMethodCalls(ICancelMethodCalls *iface)
{
    return CONTAINING_RECORD(iface, struct call_cancel, ICancelMethodCalls_iface);
}

static HRESULT WINAPI call_cancel_QueryInterface(ICancelMethodCalls *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ICancelMethodCalls))
    {
        *obj = iface;
        ICancelMethodCalls_AddRef(iface);
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI call_cancel_AddRef(ICancelMethodCalls *iface)
{
    struct call_cancel *cancel = impl_from_ICancelMethodCalls(iface);
    return InterlockedIncrement(&cancel->refs);
}

static ULONG WINAPI call_cancel_Release(ICancelMethodCalls *iface)
{
    struct call_cancel *cancel = impl_from_ICancelMethodCalls(iface);
    ULONG refs = InterlockedDecrement(&cancel->refs);

    if (!refs)
    {
        if (cancel->event)
            CloseHandle(cancel->event);
        free(cancel);
    }
    return refs;
}

static HRESULT WINAPI call_cancel_Cancel(ICancelMethodCalls *iface, ULONG seconds)
{
    struct call_cancel *cancel = impl_from_ICancelMethodCalls(iface);
    LONG previous;

    TRACE("%p, %lu\n", iface, seconds);

    /* Cancellation is decided by the calling thread's state when the call was made */
    if (!cancel->enabled)
        return CO_E_CANCEL_DISABLED;

    cancel->timeout = seconds;
    previous = InterlockedCompareExchange(&cancel->state, CANCEL_DONE, CANCEL_PENDING);
    if (previous == CANCEL_TOO_LATE)
        return RPC_E_CALL_COMPLETE;
    if (previous == CANCEL_DONE)
        return RPC_E_CALL_CANCELED;
    if (cancel->event)
        SetEvent(cancel->event);
    return S_OK;
}

static HRESULT WINAPI call_cancel_TestCancel(ICancelMethodCalls *iface)
{
    struct call_cancel *cancel = impl_from_ICancelMethodCalls(iface);

    return cancel->state == CANCEL_DONE ? RPC_E_CALL_CANCELED : RPC_S_CALLPENDING;
}

static const ICancelMethodCallsVtbl call_cancel_vtbl =
{
    call_cancel_QueryInterface,
    call_cancel_AddRef,
    call_cancel_Release,
    call_cancel_Cancel,
    call_cancel_TestCancel,
};

static HRESULT cancel_push(IUnknown *object)
{
    struct cancel_entry *entry;

    if (!(entry = malloc(sizeof(*entry))))
        return E_OUTOFMEMORY;
    entry->tid = GetCurrentThreadId();
    entry->object = object;
    IUnknown_AddRef(object);

    EnterCriticalSection(&cancel_cs);
    list_add_head(&cancel_entries, &entry->entry);
    LeaveCriticalSection(&cancel_cs);
    return S_OK;
}

/* Removes the given object, or the current thread's innermost one if object is NULL */
static void cancel_remove(IUnknown *object)
{
    struct cancel_entry *entry, *found = NULL;
    DWORD tid = GetCurrentThreadId();

    EnterCriticalSection(&cancel_cs);
    LIST_FOR_EACH_ENTRY(entry, &cancel_entries, struct cancel_entry, entry)
    {
        if (entry->tid == tid && (!object || entry->object == object))
        {
            found = entry;
            list_remove(&entry->entry);
            break;
        }
    }
    LeaveCriticalSection(&cancel_cs);

    if (found)
    {
        IUnknown_Release(found->object);
        free(found);
    }
}

struct call_cancel *callctl_client_begin(struct tlsdata *tlsdata)
{
    struct call_cancel *cancel;

    if (!(cancel = calloc(1, sizeof(*cancel))))
        return NULL;
    cancel->ICancelMethodCalls_iface.lpVtbl = &call_cancel_vtbl;
    cancel->refs = 1;
    cancel->state = CANCEL_PENDING;
    cancel->enabled = tlsdata->cancelcount != 0;
    CoGetCurrentLogicalThreadId(&cancel->cid);
    if (cancel->enabled)
        cancel->event = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (FAILED(cancel_push((IUnknown *)&cancel->ICancelMethodCalls_iface)))
    {
        call_cancel_Release(&cancel->ICancelMethodCalls_iface);
        return NULL;
    }
    return cancel;
}

/* Signaled when an abandonable call is canceled; NULL when the call cannot be canceled */
HANDLE callctl_client_event(struct call_cancel *cancel)
{
    return cancel ? cancel->event : NULL;
}

DWORD callctl_client_timeout(struct call_cancel *cancel)
{
    if (cancel->timeout == RPC_C_CANCEL_INFINITE_TIMEOUT || cancel->timeout >= INFINITE / 1000)
        return INFINITE;
    return cancel->timeout * 1000;
}

void callctl_client_end(struct call_cancel *cancel)
{
    if (!cancel)
        return;
    InterlockedCompareExchange(&cancel->state, CANCEL_TOO_LATE, CANCEL_PENDING);
    cancel_remove((IUnknown *)&cancel->ICancelMethodCalls_iface);
    call_cancel_Release(&cancel->ICancelMethodCalls_iface);
}

/* An in-process caller's call with the same causality id, innermost first */
static struct call_cancel *cancel_find_by_cid(const GUID *cid)
{
    struct call_cancel *found = NULL;
    struct cancel_entry *entry;

    EnterCriticalSection(&cancel_cs);
    LIST_FOR_EACH_ENTRY(entry, &cancel_entries, struct cancel_entry, entry)
    {
        ICancelMethodCalls *object = (ICancelMethodCalls *)entry->object;

        if (object->lpVtbl == &call_cancel_vtbl && IsEqualGUID(&impl_from_ICancelMethodCalls(object)->cid, cid))
        {
            found = impl_from_ICancelMethodCalls(object);
            call_cancel_AddRef(&found->ICancelMethodCalls_iface);
            break;
        }
    }
    LeaveCriticalSection(&cancel_cs);
    return found;
}

void callctl_thread_detach(void)
{
    struct cancel_entry *entry, *next;
    DWORD tid = GetCurrentThreadId();
    struct list dead = LIST_INIT(dead);

    EnterCriticalSection(&cancel_cs);
    LIST_FOR_EACH_ENTRY_SAFE(entry, next, &cancel_entries, struct cancel_entry, entry)
    {
        if (entry->tid == tid)
        {
            list_remove(&entry->entry);
            list_add_tail(&dead, &entry->entry);
        }
    }
    LeaveCriticalSection(&cancel_cs);

    LIST_FOR_EACH_ENTRY_SAFE(entry, next, &dead, struct cancel_entry, entry)
    {
        IUnknown_Release(entry->object);
        free(entry);
    }
}

/***********************************************************************
 *           CoSetCancelObject    (combase.@)
 */
HRESULT WINAPI CoSetCancelObject(IUnknown *object)
{
    ICancelMethodCalls *cancel;
    HRESULT hr;

    TRACE("%p\n", object);

    if (!object)
    {
        cancel_remove(NULL);
        return S_OK;
    }

    if (FAILED(hr = IUnknown_QueryInterface(object, &IID_ICancelMethodCalls, (void **)&cancel)))
        return hr;
    ICancelMethodCalls_Release(cancel);

    return cancel_push(object);
}

/***********************************************************************
 *           CoGetCancelObject    (combase.@)
 */
HRESULT WINAPI CoGetCancelObject(DWORD tid, REFIID riid, void **obj)
{
    struct cancel_entry *entry;
    IUnknown *object = NULL;
    HRESULT hr;

    TRACE("%#lx, %s, %p\n", tid, debugstr_guid(riid), obj);

    if (!obj)
        return E_INVALIDARG;
    *obj = NULL;

    if (!tid)
        tid = GetCurrentThreadId();

    EnterCriticalSection(&cancel_cs);
    LIST_FOR_EACH_ENTRY(entry, &cancel_entries, struct cancel_entry, entry)
    {
        if (entry->tid == tid)
        {
            object = entry->object;
            IUnknown_AddRef(object);
            break;
        }
    }
    LeaveCriticalSection(&cancel_cs);

    if (!object)
        return E_NOINTERFACE;

    hr = IUnknown_QueryInterface(object, riid, obj);
    IUnknown_Release(object);
    return hr;
}

/***********************************************************************
 *           CoCancelCall    (combase.@)
 */
HRESULT WINAPI CoCancelCall(DWORD tid, ULONG timeout)
{
    ICancelMethodCalls *cancel;
    HRESULT hr;

    TRACE("%#lx, %lu\n", tid, timeout);

    if (FAILED(hr = CoGetCancelObject(tid, &IID_ICancelMethodCalls, (void **)&cancel)))
        return hr;

    hr = ICancelMethodCalls_Cancel(cancel, timeout);
    ICancelMethodCalls_Release(cancel);
    return hr;
}

/***********************************************************************
 *           CoTestCancel    (combase.@)
 */
HRESULT WINAPI CoTestCancel(void)
{
    struct callctl_server_call *call;
    ICancelMethodCalls *cancel;
    struct tlsdata *tlsdata;
    HRESULT hr;

    TRACE("\n");

    if (FAILED(hr = com_get_tlsdata(&tlsdata)))
        return hr;

    /* A context installed with CoSwitchCallContext answers first */
    if (tlsdata->call_state &&
        SUCCEEDED(IUnknown_QueryInterface(tlsdata->call_state, &IID_ICancelMethodCalls, (void **)&cancel)))
    {
        hr = ICancelMethodCalls_TestCancel(cancel);
        ICancelMethodCalls_Release(cancel);
        return hr;
    }

    if (!(call = tlsdata->server_call))
        return RPC_E_CALL_COMPLETE;

    /* Only a caller in this process can tell us about a cancellation */
    return call->cancel ? call_cancel_TestCancel(&call->cancel->ICancelMethodCalls_iface) : RPC_S_CALLPENDING;
}

void callctl_server_prepare(struct tlsdata *tlsdata)
{
    tlsdata->caller_valid = FALSE;
}

void callctl_server_begin(struct tlsdata *tlsdata, struct callctl_server_call *call, const GUID *cid)
{
    call->caller_known = tlsdata->caller_valid;
    call->caller_pid = tlsdata->caller_pid;
    call->caller_apt = tlsdata->caller_apt;
    tlsdata->caller_valid = FALSE;
    call->cancel = cancel_find_by_cid(cid);
    call->prev = tlsdata->server_call;
    tlsdata->server_call = call;
}

void callctl_server_end(struct tlsdata *tlsdata, struct callctl_server_call *call)
{
    tlsdata->server_call = call->prev;
    if (call->cancel)
        call_cancel_Release(&call->cancel->ICancelMethodCalls_iface);
}

/* The caller's process and apartment travel in an ORPC extension of every call */
struct caller_extent
{
    DWORD pid;
    DWORD apt;
};

/* {4a159fdd-89e9-4c98-8fc4-9ccd757131c0} */
static const GUID caller_extent_id = { 0x4a159fdd, 0x89e9, 0x4c98, { 0x8f, 0xc4, 0x9c, 0xcd, 0x75, 0x71, 0x31, 0xc0 } };

static HRESULT WINAPI caller_hook_QueryInterface(IChannelHook *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IChannelHook))
    {
        *obj = iface;
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI caller_hook_AddRef(IChannelHook *iface)
{
    return 2;
}

static ULONG WINAPI caller_hook_Release(IChannelHook *iface)
{
    return 1;
}

static void WINAPI caller_hook_ClientGetSize(IChannelHook *iface, REFGUID extent, REFIID riid, ULONG *size)
{
    *size = sizeof(struct caller_extent);
}

static void WINAPI caller_hook_ClientFillBuffer(IChannelHook *iface, REFGUID extent, REFIID riid, ULONG *size,
        void *buffer)
{
    struct apartment *apt = com_get_current_apt();
    struct caller_extent data;

    /* An MTA is 0; a thread without an apartment calls from the implicit MTA */
    data.pid = GetCurrentProcessId();
    data.apt = (apt && !apt->multi_threaded) ? apt->tid : 0;
    memcpy(buffer, &data, sizeof(data));
    *size = sizeof(data);
}

static void WINAPI caller_hook_ClientNotify(IChannelHook *iface, REFGUID extent, REFIID riid, ULONG size,
        void *buffer, DWORD data_rep, HRESULT fault)
{
}

static void WINAPI caller_hook_ServerNotify(IChannelHook *iface, REFGUID extent, REFIID riid, ULONG size,
        void *buffer, DWORD data_rep)
{
    struct caller_extent data;
    struct tlsdata *tlsdata;

    if (!buffer || size < sizeof(data) || FAILED(com_get_tlsdata(&tlsdata)))
        return;
    memcpy(&data, buffer, sizeof(data));
    tlsdata->caller_pid = data.pid;
    tlsdata->caller_apt = data.apt;
    tlsdata->caller_valid = TRUE;
}

static void WINAPI caller_hook_ServerGetSize(IChannelHook *iface, REFGUID extent, REFIID riid, HRESULT fault,
        ULONG *size)
{
    *size = 0;
}

static void WINAPI caller_hook_ServerFillBuffer(IChannelHook *iface, REFGUID extent, REFIID riid, ULONG *size,
        void *buffer, HRESULT fault)
{
    *size = 0;
}

static const IChannelHookVtbl caller_hook_vtbl =
{
    caller_hook_QueryInterface,
    caller_hook_AddRef,
    caller_hook_Release,
    caller_hook_ClientGetSize,
    caller_hook_ClientFillBuffer,
    caller_hook_ClientNotify,
    caller_hook_ServerNotify,
    caller_hook_ServerGetSize,
    caller_hook_ServerFillBuffer,
};

static IChannelHook caller_hook = { &caller_hook_vtbl };

void callctl_register_hook(void)
{
    rpc_register_channel_hook(&caller_extent_id, &caller_hook);
}

/***********************************************************************
 *           CoGetCallerTID    (combase.@)
 */
HRESULT WINAPI CoGetCallerTID(DWORD *tid)
{
    struct callctl_server_call *call;
    struct tlsdata *tlsdata;
    HRESULT hr;

    TRACE("%p\n", tid);

    if (!tid)
        return E_INVALIDARG;

    if (FAILED(hr = com_get_tlsdata(&tlsdata)))
        return hr;

    if (!(call = tlsdata->server_call))
        return RPC_E_CALL_COMPLETE;

    /* A caller that did not say where it runs is treated like one on another machine */
    if (!call->caller_known)
        return S_FALSE;

    *tid = call->caller_apt;
    return call->caller_pid == GetCurrentProcessId() ? S_OK : S_FALSE;
}

/* An STA waits for its handles and runs incoming COM calls, but leaves other messages queued */
static HRESULT wait_dispatching_calls(struct apartment *apt, DWORD timeout, ULONG count, const HANDLE *handles,
        DWORD *index)
{
    DWORD start = GetTickCount(), elapsed, res;
    HWND win;
    MSG msg;

    rpc_start_remoting(apt);
    win = apartment_getwindow(apt);

    for (;;)
    {
        elapsed = GetTickCount() - start;
        res = MsgWaitForMultipleObjectsEx(count, handles,
                timeout == INFINITE ? INFINITE : (elapsed >= timeout ? 0 : timeout - elapsed),
                QS_SENDMESSAGE | QS_POSTMESSAGE, 0);

        if (res == WAIT_OBJECT_0 + count)
        {
            while (win && PeekMessageW(&msg, win, 0, 0, PM_REMOVE | PM_NOYIELD))
                DispatchMessageW(&msg);
            /* Marks the rest as seen, so that only new messages wake the wait */
            PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE | PM_NOYIELD);
            continue;
        }
        if (res == WAIT_TIMEOUT)
            return RPC_S_CALLPENDING;
        if (res == WAIT_FAILED)
            return HRESULT_FROM_WIN32(GetLastError());
        *index = res;
        return S_OK;
    }
}

/***********************************************************************
 *           CoWaitForMultipleObjects    (combase.@)
 */
HRESULT WINAPI CoWaitForMultipleObjects(DWORD flags, DWORD timeout, ULONG count, const HANDLE *handles,
        DWORD *index)
{
    struct apartment *apt;
    struct tlsdata *tlsdata;
    HRESULT hr;

    TRACE("%#lx, %#lx, %lu, %p, %p\n", flags, timeout, count, handles, index);

    if (flags & ~(CWMO_DISPATCH_CALLS | CWMO_DISPATCH_WINDOW_MESSAGES))
        return E_INVALIDARG;
    if (count > CWMO_MAX_HANDLES)
        return E_INVALIDARG;
    if (FAILED(hr = com_get_tlsdata(&tlsdata)))
        return hr;

    /* No ASTA here: an MTA ignores both flags, and an STA always runs incoming calls */
    apt = tlsdata->apt;
    if (!apt || apt->multi_threaded || (flags & CWMO_DISPATCH_WINDOW_MESSAGES))
        return CoWaitForMultipleHandles(0, timeout, count, (HANDLE *)handles, index);

    if (!index)
        return E_INVALIDARG;
    *index = 0;
    if (!handles)
        return E_INVALIDARG;
    if (!count)
        return RPC_E_NO_SYNC;

    return wait_dispatching_calls(apt, timeout, count, handles, index);
}
