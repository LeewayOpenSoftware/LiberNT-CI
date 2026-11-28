/*
 * PROJECT:     LiberNT Task Scheduler client
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IRunningTask and IRunningTaskCollection over the Schedule service
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "oleauto.h"
#include "taskschd.h"
#include "schrpc.h"
#include "taskschd_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(taskschd);

#ifndef SCHED_E_TASK_NOT_RUNNING
#define SCHED_E_TASK_NOT_RUNNING ((HRESULT)0x8004130B)
#endif

typedef struct
{
    IRunningTask IRunningTask_iface;
    LONG ref;
    GUID guid;
    WCHAR *path;
    WCHAR *action;
    DWORD state;
    DWORD pid;
} RunningTask;

static inline RunningTask *impl_from_IRunningTask(IRunningTask *iface)
{
    return CONTAINING_RECORD(iface, RunningTask, IRunningTask_iface);
}

/* Re-reads the instance; an instance that has finished keeps its path. */
static HRESULT running_refresh(RunningTask *task)
{
    WCHAR *path = NULL, *action = NULL, *info = NULL;
    DWORD state = TASK_STATE_UNKNOWN, n_instances = 0, pid = 0;
    GUID *instances = NULL;
    HRESULT hr;

    hr = SchRpcGetInstanceInfo(task->guid, &path, &state, &action, &info, &n_instances, &instances, &pid);
    if (hr == S_OK)
    {
        if (path)
        {
            free(task->path);
            task->path = wcsdup(path);
        }
        free(task->action);
        task->action = action ? wcsdup(action) : NULL;
        task->state = state;
        task->pid = pid;
    }
    else if (hr == SCHED_E_TASK_NOT_RUNNING)
    {
        free(task->action);
        task->action = NULL;
        task->state = TASK_STATE_UNKNOWN;
        task->pid = 0;
        hr = S_OK;
    }

    MIDL_user_free(path);
    MIDL_user_free(action);
    MIDL_user_free(info);
    MIDL_user_free(instances);
    return hr;
}

static ULONG WINAPI RunningTask_AddRef(IRunningTask *iface)
{
    RunningTask *task = impl_from_IRunningTask(iface);
    return InterlockedIncrement(&task->ref);
}

static ULONG WINAPI RunningTask_Release(IRunningTask *iface)
{
    RunningTask *task = impl_from_IRunningTask(iface);
    LONG ref = InterlockedDecrement(&task->ref);

    if (!ref)
    {
        TRACE("destroying %p\n", iface);
        free(task->path);
        free(task->action);
        free(task);
    }
    return ref;
}

static HRESULT WINAPI RunningTask_QueryInterface(IRunningTask *iface, REFIID riid, void **obj)
{
    if (!riid || !obj) return E_INVALIDARG;

    TRACE("%p,%s,%p\n", iface, debugstr_guid(riid), obj);

    if (IsEqualGUID(riid, &IID_IRunningTask) ||
        IsEqualGUID(riid, &IID_IDispatch) ||
        IsEqualGUID(riid, &IID_IUnknown))
    {
        IRunningTask_AddRef(iface);
        *obj = iface;
        return S_OK;
    }

    FIXME("interface %s is not implemented\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI RunningTask_GetTypeInfoCount(IRunningTask *iface, UINT *count)
{
    FIXME("%p,%p: stub\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI RunningTask_GetTypeInfo(IRunningTask *iface, UINT index, LCID lcid, ITypeInfo **info)
{
    FIXME("%p,%u,%lu,%p: stub\n", iface, index, lcid, info);
    return E_NOTIMPL;
}

static HRESULT WINAPI RunningTask_GetIDsOfNames(IRunningTask *iface, REFIID riid, LPOLESTR *names,
                                                UINT count, LCID lcid, DISPID *dispid)
{
    FIXME("%p,%s,%p,%u,%lu,%p: stub\n", iface, debugstr_guid(riid), names, count, lcid, dispid);
    return E_NOTIMPL;
}

static HRESULT WINAPI RunningTask_Invoke(IRunningTask *iface, DISPID dispid, REFIID riid, LCID lcid, WORD flags,
                                         DISPPARAMS *params, VARIANT *result, EXCEPINFO *excepinfo, UINT *argerr)
{
    FIXME("%p,%ld,%s,%04lx,%04x,%p,%p,%p,%p: stub\n", iface, dispid, debugstr_guid(riid), lcid, flags,
          params, result, excepinfo, argerr);
    return E_NOTIMPL;
}

static HRESULT WINAPI RunningTask_get_Name(IRunningTask *iface, BSTR *name)
{
    RunningTask *task = impl_from_IRunningTask(iface);
    const WCHAR *p;

    TRACE("%p,%p\n", iface, name);

    if (!name) return E_POINTER;
    if (!task->path) return E_UNEXPECTED;

    p = wcsrchr(task->path, '\\');
    p = (p && p[1]) ? p + 1 : task->path;
    *name = SysAllocString(p);
    return *name ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI RunningTask_get_InstanceGuid(IRunningTask *iface, BSTR *guid)
{
    RunningTask *task = impl_from_IRunningTask(iface);
    WCHAR buf[39];

    TRACE("%p,%p\n", iface, guid);

    if (!guid) return E_POINTER;

    StringFromGUID2(&task->guid, buf, ARRAYSIZE(buf));
    *guid = SysAllocString(buf);
    return *guid ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI RunningTask_get_Path(IRunningTask *iface, BSTR *path)
{
    RunningTask *task = impl_from_IRunningTask(iface);

    TRACE("%p,%p\n", iface, path);

    if (!path) return E_POINTER;
    if (!task->path) return E_UNEXPECTED;

    *path = SysAllocString(task->path);
    return *path ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI RunningTask_get_State(IRunningTask *iface, TASK_STATE *state)
{
    RunningTask *task = impl_from_IRunningTask(iface);
    HRESULT hr;

    TRACE("%p,%p\n", iface, state);

    if (!state) return E_POINTER;

    hr = running_refresh(task);
    *state = task->state;
    return hr;
}

static HRESULT WINAPI RunningTask_get_CurrentAction(IRunningTask *iface, BSTR *name)
{
    RunningTask *task = impl_from_IRunningTask(iface);

    TRACE("%p,%p\n", iface, name);

    if (!name) return E_POINTER;

    *name = SysAllocString(task->action ? task->action : L"");
    return *name ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI RunningTask_Stop(IRunningTask *iface)
{
    RunningTask *task = impl_from_IRunningTask(iface);

    TRACE("%p\n", iface);

    return SchRpcStopInstance(task->guid, 0);
}

static HRESULT WINAPI RunningTask_Refresh(IRunningTask *iface)
{
    RunningTask *task = impl_from_IRunningTask(iface);

    TRACE("%p\n", iface);

    return running_refresh(task);
}

static HRESULT WINAPI RunningTask_get_EnginePID(IRunningTask *iface, DWORD *pid)
{
    RunningTask *task = impl_from_IRunningTask(iface);

    TRACE("%p,%p\n", iface, pid);

    if (!pid) return E_POINTER;

    *pid = task->pid;
    return S_OK;
}

static const IRunningTaskVtbl RunningTask_vtbl =
{
    RunningTask_QueryInterface,
    RunningTask_AddRef,
    RunningTask_Release,
    RunningTask_GetTypeInfoCount,
    RunningTask_GetTypeInfo,
    RunningTask_GetIDsOfNames,
    RunningTask_Invoke,
    RunningTask_get_Name,
    RunningTask_get_InstanceGuid,
    RunningTask_get_Path,
    RunningTask_get_State,
    RunningTask_get_CurrentAction,
    RunningTask_Stop,
    RunningTask_Refresh,
    RunningTask_get_EnginePID
};

HRESULT RunningTask_create(const GUID *guid, const WCHAR *path, IRunningTask **obj)
{
    RunningTask *task;

    if (!(task = calloc(1, sizeof(*task)))) return E_OUTOFMEMORY;

    task->IRunningTask_iface.lpVtbl = &RunningTask_vtbl;
    task->ref = 1;
    task->guid = *guid;
    if (path && !(task->path = wcsdup(path)))
    {
        free(task);
        return E_OUTOFMEMORY;
    }
    running_refresh(task);

    *obj = &task->IRunningTask_iface;
    TRACE("created %p\n", *obj);
    return S_OK;
}

typedef struct
{
    IRunningTaskCollection IRunningTaskCollection_iface;
    LONG ref;
    DWORD count;
    GUID *guids;
} RunningTaskCollection;

static inline RunningTaskCollection *impl_from_IRunningTaskCollection(IRunningTaskCollection *iface)
{
    return CONTAINING_RECORD(iface, RunningTaskCollection, IRunningTaskCollection_iface);
}

static ULONG WINAPI RunningTaskCollection_AddRef(IRunningTaskCollection *iface)
{
    RunningTaskCollection *coll = impl_from_IRunningTaskCollection(iface);
    return InterlockedIncrement(&coll->ref);
}

static ULONG WINAPI RunningTaskCollection_Release(IRunningTaskCollection *iface)
{
    RunningTaskCollection *coll = impl_from_IRunningTaskCollection(iface);
    LONG ref = InterlockedDecrement(&coll->ref);

    if (!ref)
    {
        TRACE("destroying %p\n", iface);
        free(coll->guids);
        free(coll);
    }
    return ref;
}

static HRESULT WINAPI RunningTaskCollection_QueryInterface(IRunningTaskCollection *iface, REFIID riid, void **obj)
{
    if (!riid || !obj) return E_INVALIDARG;

    TRACE("%p,%s,%p\n", iface, debugstr_guid(riid), obj);

    if (IsEqualGUID(riid, &IID_IRunningTaskCollection) ||
        IsEqualGUID(riid, &IID_IDispatch) ||
        IsEqualGUID(riid, &IID_IUnknown))
    {
        IRunningTaskCollection_AddRef(iface);
        *obj = iface;
        return S_OK;
    }

    FIXME("interface %s is not implemented\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI RunningTaskCollection_GetTypeInfoCount(IRunningTaskCollection *iface, UINT *count)
{
    FIXME("%p,%p: stub\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI RunningTaskCollection_GetTypeInfo(IRunningTaskCollection *iface, UINT index, LCID lcid,
                                                        ITypeInfo **info)
{
    FIXME("%p,%u,%lu,%p: stub\n", iface, index, lcid, info);
    return E_NOTIMPL;
}

static HRESULT WINAPI RunningTaskCollection_GetIDsOfNames(IRunningTaskCollection *iface, REFIID riid,
                                                          LPOLESTR *names, UINT count, LCID lcid, DISPID *dispid)
{
    FIXME("%p,%s,%p,%u,%lu,%p: stub\n", iface, debugstr_guid(riid), names, count, lcid, dispid);
    return E_NOTIMPL;
}

static HRESULT WINAPI RunningTaskCollection_Invoke(IRunningTaskCollection *iface, DISPID dispid, REFIID riid,
                                                   LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result,
                                                   EXCEPINFO *excepinfo, UINT *argerr)
{
    FIXME("%p,%ld,%s,%04lx,%04x,%p,%p,%p,%p: stub\n", iface, dispid, debugstr_guid(riid), lcid, flags,
          params, result, excepinfo, argerr);
    return E_NOTIMPL;
}

static HRESULT WINAPI RunningTaskCollection_get_Count(IRunningTaskCollection *iface, LONG *count)
{
    RunningTaskCollection *coll = impl_from_IRunningTaskCollection(iface);

    TRACE("%p,%p\n", iface, count);

    if (!count) return E_POINTER;

    *count = coll->count;
    return S_OK;
}

static HRESULT WINAPI RunningTaskCollection_get_Item(IRunningTaskCollection *iface, VARIANT index, IRunningTask **task)
{
    RunningTaskCollection *coll = impl_from_IRunningTaskCollection(iface);
    VARIANT v;
    HRESULT hr;

    TRACE("%p,%s,%p\n", iface, debugstr_variant(&index), task);

    if (!task) return E_POINTER;
    *task = NULL;

    VariantInit(&v);
    if (FAILED(hr = VariantChangeType(&v, &index, 0, VT_I4))) return E_INVALIDARG;

    /* the collection is 1-based, as on Windows */
    if (V_I4(&v) < 1 || (DWORD)V_I4(&v) > coll->count) return E_INVALIDARG;

    return RunningTask_create(&coll->guids[V_I4(&v) - 1], NULL, task);
}

static HRESULT WINAPI RunningTaskCollection_get__NewEnum(IRunningTaskCollection *iface, IUnknown **penum)
{
    FIXME("%p,%p: stub\n", iface, penum);
    return E_NOTIMPL;
}

static const IRunningTaskCollectionVtbl RunningTaskCollection_vtbl =
{
    RunningTaskCollection_QueryInterface,
    RunningTaskCollection_AddRef,
    RunningTaskCollection_Release,
    RunningTaskCollection_GetTypeInfoCount,
    RunningTaskCollection_GetTypeInfo,
    RunningTaskCollection_GetIDsOfNames,
    RunningTaskCollection_Invoke,
    RunningTaskCollection_get_Count,
    RunningTaskCollection_get_Item,
    RunningTaskCollection_get__NewEnum
};

/* path NULL: every running task (ITaskService::GetRunningTasks). */
HRESULT RunningTaskCollection_create(const WCHAR *path, LONG flags, IRunningTaskCollection **obj)
{
    RunningTaskCollection *coll;
    GUID *guids = NULL;
    DWORD count = 0;
    HRESULT hr;

    hr = SchRpcEnumInstances(path, flags & TASK_ENUM_HIDDEN, &count, &guids);
    if (FAILED(hr)) return hr;

    if (!(coll = calloc(1, sizeof(*coll))))
    {
        MIDL_user_free(guids);
        return E_OUTOFMEMORY;
    }
    coll->IRunningTaskCollection_iface.lpVtbl = &RunningTaskCollection_vtbl;
    coll->ref = 1;
    if (count)
    {
        if (!(coll->guids = malloc(count * sizeof(GUID))))
        {
            MIDL_user_free(guids);
            free(coll);
            return E_OUTOFMEMORY;
        }
        memcpy(coll->guids, guids, count * sizeof(GUID));
        coll->count = count;
    }
    MIDL_user_free(guids);

    *obj = &coll->IRunningTaskCollection_iface;
    TRACE("created %p (%lu instances)\n", *obj, count);
    return S_OK;
}
