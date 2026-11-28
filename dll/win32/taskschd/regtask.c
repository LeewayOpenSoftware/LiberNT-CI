/*
 * Copyright 2014 Dmitry Timoshkov
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "objbase.h"
#include "taskschd.h"
#include "schrpc.h"
#include "taskschd_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(taskschd);

typedef struct
{
    IRegisteredTask IRegisteredTask_iface;
    LONG ref;
    WCHAR *path;
    ITaskDefinition *taskdef;
} RegisteredTask;

static inline RegisteredTask *impl_from_IRegisteredTask(IRegisteredTask *iface)
{
    return CONTAINING_RECORD(iface, RegisteredTask, IRegisteredTask_iface);
}

static ULONG WINAPI regtask_AddRef(IRegisteredTask *iface)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);
    return InterlockedIncrement(&regtask->ref);
}

static ULONG WINAPI regtask_Release(IRegisteredTask *iface)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);
    LONG ref = InterlockedDecrement(&regtask->ref);

    if (!ref)
    {
        TRACE("destroying %p\n", iface);
        ITaskDefinition_Release(regtask->taskdef);
        free(regtask->path);
        free(regtask);
    }

    return ref;
}

static HRESULT WINAPI regtask_QueryInterface(IRegisteredTask *iface, REFIID riid, void **obj)
{
    if (!riid || !obj) return E_INVALIDARG;

    TRACE("%p,%s,%p\n", iface, debugstr_guid(riid), obj);

    if (IsEqualGUID(riid, &IID_IRegisteredTask) ||
        IsEqualGUID(riid, &IID_IDispatch) ||
        IsEqualGUID(riid, &IID_IUnknown))
    {
        IRegisteredTask_AddRef(iface);
        *obj = iface;
        return S_OK;
    }

    FIXME("interface %s is not implemented\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI regtask_GetTypeInfoCount(IRegisteredTask *iface, UINT *count)
{
    FIXME("%p,%p: stub\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI regtask_GetTypeInfo(IRegisteredTask *iface, UINT index, LCID lcid, ITypeInfo **info)
{
    FIXME("%p,%u,%lu,%p: stub\n", iface, index, lcid, info);
    return E_NOTIMPL;
}

static HRESULT WINAPI regtask_GetIDsOfNames(IRegisteredTask *iface, REFIID riid, LPOLESTR *names,
                                            UINT count, LCID lcid, DISPID *dispid)
{
    FIXME("%p,%s,%p,%u,%lu,%p: stub\n", iface, debugstr_guid(riid), names, count, lcid, dispid);
    return E_NOTIMPL;
}

static HRESULT WINAPI regtask_Invoke(IRegisteredTask *iface, DISPID dispid, REFIID riid, LCID lcid, WORD flags,
                                     DISPPARAMS *params, VARIANT *result, EXCEPINFO *excepinfo, UINT *argerr)
{
    FIXME("%p,%ld,%s,%04lx,%04x,%p,%p,%p,%p: stub\n", iface, dispid, debugstr_guid(riid), lcid, flags,
          params, result, excepinfo, argerr);
    return E_NOTIMPL;
}

static HRESULT WINAPI regtask_get_Name(IRegisteredTask *iface, BSTR *name)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);
    const WCHAR *p_name;

    TRACE("%p,%p\n", iface, name);

    if (!name) return E_POINTER;

    p_name = wcsrchr(regtask->path, '\\');
    if (!p_name)
        p_name = regtask->path;
    else
        if (p_name[1] != 0) p_name++;

    *name = SysAllocString(p_name);
    if (!*name) return E_OUTOFMEMORY;

    return S_OK;
}

static HRESULT WINAPI regtask_get_Path(IRegisteredTask *iface, BSTR *path)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);

    TRACE("%p,%p\n", iface, path);

    if (!path) return E_POINTER;

    *path = SysAllocString(regtask->path);
    if (!*path) return E_OUTOFMEMORY;

    return S_OK;
}

static HRESULT WINAPI regtask_get_State(IRegisteredTask *iface, TASK_STATE *state)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);
    DWORD enabled;

    TRACE("%p,%p\n", iface, state);

    if (!state) return E_POINTER;

    return SchRpcGetTaskInfo(regtask->path, SCH_FLAG_STATE, &enabled, (DWORD *)state);
}

static HRESULT WINAPI regtask_get_Enabled(IRegisteredTask *iface, VARIANT_BOOL *v_enabled)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);
    DWORD enabled, state;
    HRESULT hr;

    TRACE("%p,%p\n", iface, v_enabled);

    if (!v_enabled) return E_POINTER;

    hr = SchRpcGetTaskInfo(regtask->path, 0, &enabled, &state);
    if (hr == S_OK)
        *v_enabled = enabled ? VARIANT_TRUE : VARIANT_FALSE;
    return hr;
}

static HRESULT WINAPI regtask_put_Enabled(IRegisteredTask *iface, VARIANT_BOOL enabled)
{
#ifdef __REACTOS__
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);

    TRACE("%p,%d\n", iface, enabled);

    return SchRpcEnableTask(regtask->path, enabled != VARIANT_FALSE);
#else
    FIXME("%p,%d: stub\n", iface, enabled);
    return S_OK;
#endif
}

#define MAX_RUN_PARAMS 32

static void free_run_params(DWORD count, WCHAR **args)
{
    DWORD i;

    for (i = 0; i < count; i++) SysFreeString(args[i]);
    free(args);
}

/* IRegisteredTask::Run parameters: empty, one value, or a 1-D SAFEARRAY ($(Arg0)..$(Arg31)). */
static HRESULT get_run_params(VARIANT *params, DWORD *count, WCHAR ***args)
{
    VARIANT *in = params, v;
    SAFEARRAY *sa;
    LONG lbound, ubound, i;
    HRESULT hr;

    *count = 0;
    *args = NULL;

    if (V_VT(in) == (VT_VARIANT | VT_BYREF)) in = V_VARIANTREF(in);
    if (V_VT(in) == VT_EMPTY || V_VT(in) == VT_NULL || V_VT(in) == VT_ERROR) return S_OK;

    if (!(V_VT(in) & VT_ARRAY))
    {
        VariantInit(&v);
        if (FAILED(hr = VariantChangeType(&v, in, 0, VT_BSTR))) return E_INVALIDARG;
        if (!(*args = malloc(sizeof(WCHAR *))))
        {
            VariantClear(&v);
            return E_OUTOFMEMORY;
        }
        (*args)[0] = V_BSTR(&v);
        *count = 1;
        return S_OK;
    }

    sa = (V_VT(in) & VT_BYREF) ? *V_ARRAYREF(in) : V_ARRAY(in);
    if (!sa || SafeArrayGetDim(sa) != 1) return E_INVALIDARG;
    if (FAILED(SafeArrayGetLBound(sa, 1, &lbound)) || FAILED(SafeArrayGetUBound(sa, 1, &ubound)))
        return E_INVALIDARG;
    if (ubound < lbound) return S_OK;
    if (ubound - lbound + 1 > MAX_RUN_PARAMS) return E_INVALIDARG;

    if (!(*args = calloc(ubound - lbound + 1, sizeof(WCHAR *)))) return E_OUTOFMEMORY;

    for (i = lbound; i <= ubound; i++)
    {
        VARTYPE vt = V_VT(in) & VT_TYPEMASK;

        VariantInit(&v);
        if (vt == VT_VARIANT)
        {
            VARIANT elem;

            VariantInit(&elem);
            hr = SafeArrayGetElement(sa, &i, &elem);
            if (SUCCEEDED(hr)) hr = VariantChangeType(&v, &elem, 0, VT_BSTR);
            VariantClear(&elem);
        }
        else if (vt == VT_BSTR)
        {
            BSTR str = NULL;

            hr = SafeArrayGetElement(sa, &i, &str);
            if (SUCCEEDED(hr))
            {
                V_VT(&v) = VT_BSTR;
                V_BSTR(&v) = str;
            }
        }
        else
            hr = E_INVALIDARG;

        if (FAILED(hr))
        {
            free_run_params(*count, *args);
            *count = 0;
            *args = NULL;
            return E_INVALIDARG;
        }
        (*args)[(*count)++] = V_BSTR(&v);
    }
    return S_OK;
}

static HRESULT WINAPI regtask_RunEx(IRegisteredTask *iface, VARIANT params, LONG flags,
                                    LONG session_id, BSTR user, IRunningTask **task)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);
    WCHAR **args;
    DWORD count;
    GUID guid;
    HRESULT hr;

    TRACE("%p,%s,%lx,%lx,%s,%p\n", iface, debugstr_variant(&params), flags, session_id, debugstr_w(user), task);

    if (task) *task = NULL;

    hr = get_run_params(&params, &count, &args);
    if (FAILED(hr)) return hr;

    hr = SchRpcRun(regtask->path, count, (const WCHAR **)args, flags, session_id,
                   (user && *user) ? user : NULL, &guid);
    free_run_params(count, args);

    if (hr == S_OK && task)
        hr = RunningTask_create(&guid, regtask->path, task);
    return hr;
}

static HRESULT WINAPI regtask_Run(IRegisteredTask *iface, VARIANT params, IRunningTask **task)
{
    TRACE("%p,%s,%p\n", iface, debugstr_variant(&params), task);

    return regtask_RunEx(iface, params, 0, 0, NULL, task);
}

static HRESULT WINAPI regtask_GetInstances(IRegisteredTask *iface, LONG flags, IRunningTaskCollection **tasks)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);

    TRACE("%p,%lx,%p\n", iface, flags, tasks);

    if (!tasks) return E_POINTER;

    return RunningTaskCollection_create(regtask->path, flags, tasks);
}

static HRESULT WINAPI regtask_get_LastRunTime(IRegisteredTask *iface, DATE *date)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);
    SYSTEMTIME st;
    DWORD code;
    HRESULT hr;

    TRACE("%p,%p\n", iface, date);

    if (!date) return E_POINTER;
    *date = 0;

    hr = SchRpcGetLastRunInfo(regtask->path, &st, &code);
    if (hr != S_OK) return hr;

    /* a task that never ran has no run time */
    if (!st.wYear) return SCHED_S_TASK_HAS_NOT_RUN;
    return SystemTimeToVariantTime(&st, date) ? S_OK : E_FAIL;
}

static HRESULT WINAPI regtask_get_LastTaskResult(IRegisteredTask *iface, LONG *result)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);
    SYSTEMTIME st;
    DWORD code;
    HRESULT hr;

    TRACE("%p,%p\n", iface, result);

    if (!result) return E_POINTER;

    hr = SchRpcGetLastRunInfo(regtask->path, &st, &code);
    if (hr == S_OK) *result = code;
    return hr;
}

static HRESULT WINAPI regtask_get_NumberOfMissedRuns(IRegisteredTask *iface, LONG *runs)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);
    DWORD count = 0;
    HRESULT hr;

    TRACE("%p,%p\n", iface, runs);

    if (!runs) return E_POINTER;

    hr = SchRpcGetNumberOfMissedRuns(regtask->path, &count);
    if (hr == S_OK) *runs = count;
    return hr;
}

static HRESULT WINAPI regtask_get_NextRunTime(IRegisteredTask *iface, DATE *date)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);
    SYSTEMTIME *times = NULL;
    DWORD count = 0;
    HRESULT hr;

    TRACE("%p,%p\n", iface, date);

    if (!date) return E_POINTER;
    *date = 0;

    hr = SchRpcScheduledRuntimes(regtask->path, NULL, NULL, 0, 1, &count, &times);
    if (SUCCEEDED(hr) && count && !SystemTimeToVariantTime(&times[0], date)) hr = E_FAIL;
    MIDL_user_free(times);
    /* no time or calendar trigger left: DATE 0 */
    return FAILED(hr) ? hr : S_OK;
}

static HRESULT WINAPI regtask_get_Definition(IRegisteredTask *iface, ITaskDefinition **task)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);

    TRACE("%p,%p\n", iface, task);

    if (!task) return E_POINTER;

    ITaskDefinition_AddRef(regtask->taskdef);
    *task = regtask->taskdef;

    return S_OK;
}

static HRESULT WINAPI regtask_get_Xml(IRegisteredTask *iface, BSTR *xml)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);

    TRACE("%p,%p\n", iface, xml);

    if (!xml) return E_POINTER;

    return ITaskDefinition_get_XmlText(regtask->taskdef, xml);
}

static HRESULT WINAPI regtask_GetSecurityDescriptor(IRegisteredTask *iface, LONG info, BSTR *sddl)
{
    FIXME("%p,%lx,%p: stub\n", iface, info, sddl);
    return E_NOTIMPL;
}

static HRESULT WINAPI regtask_SetSecurityDescriptor(IRegisteredTask *iface, BSTR sddl, LONG flags)
{
    FIXME("%p,%s,%lx: stub\n", iface, debugstr_w(sddl), flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI regtask_Stop(IRegisteredTask *iface, LONG flags)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);

    TRACE("%p,%lx\n", iface, flags);

    if (flags) FIXME("flags %#lx ignored\n", flags);

    return SchRpcStop(regtask->path, 0);
}

/* *count: on input the most run times wanted (0 = all), on output how many were returned. */
static HRESULT WINAPI regtask_GetRunTimes(IRegisteredTask *iface, const LPSYSTEMTIME start, const LPSYSTEMTIME end,
                                          DWORD *count, LPSYSTEMTIME *time)
{
    RegisteredTask *regtask = impl_from_IRegisteredTask(iface);
    SYSTEMTIME *times = NULL;
    DWORD n = 0;
    HRESULT hr;

    TRACE("%p,%p.%p,%p,%p\n", iface, start, end, count, time);

    if (!count || !time) return E_POINTER;
    *time = NULL;

    hr = SchRpcScheduledRuntimes(regtask->path, start, end, 0, *count, &n, &times);
    if (FAILED(hr))
    {
        *count = 0;
        return hr;
    }
    if (n)
    {
        if (!(*time = CoTaskMemAlloc(n * sizeof(SYSTEMTIME))))
        {
            MIDL_user_free(times);
            *count = 0;
            return E_OUTOFMEMORY;
        }
        memcpy(*time, times, n * sizeof(SYSTEMTIME));
    }
    MIDL_user_free(times);
    *count = n;
    return n ? S_OK : S_FALSE;
}

static const IRegisteredTaskVtbl RegisteredTask_vtbl =
{
    regtask_QueryInterface,
    regtask_AddRef,
    regtask_Release,
    regtask_GetTypeInfoCount,
    regtask_GetTypeInfo,
    regtask_GetIDsOfNames,
    regtask_Invoke,
    regtask_get_Name,
    regtask_get_Path,
    regtask_get_State,
    regtask_get_Enabled,
    regtask_put_Enabled,
    regtask_Run,
    regtask_RunEx,
    regtask_GetInstances,
    regtask_get_LastRunTime,
    regtask_get_LastTaskResult,
    regtask_get_NumberOfMissedRuns,
    regtask_get_NextRunTime,
    regtask_get_Definition,
    regtask_get_Xml,
    regtask_GetSecurityDescriptor,
    regtask_SetSecurityDescriptor,
    regtask_Stop,
    regtask_GetRunTimes
};

HRESULT RegisteredTask_create(const WCHAR *path, const WCHAR *name, ITaskDefinition *definition, LONG flags,
                              TASK_LOGON_TYPE logon, IRegisteredTask **obj, BOOL create)
{
    WCHAR *full_name;
    RegisteredTask *regtask;
    HRESULT hr;

    if (!name)
    {
        if (!create) return E_INVALIDARG;

        /* NULL task name is allowed only in the root folder */
        if (path[0] != '\\' || path[1])
            return E_INVALIDARG;

        full_name = NULL;
    }
    else
    {
        full_name = get_full_path(path, name);
        if (!full_name) return E_OUTOFMEMORY;
    }

    regtask = malloc(sizeof(*regtask));
    if (!regtask)
    {
        free(full_name);
        return E_OUTOFMEMORY;
    }

    if (create)
    {
        WCHAR *actual_path = NULL;
        TASK_XML_ERROR_INFO *error_info = NULL;
        BSTR xml = NULL;

        hr = ITaskDefinition_get_XmlText(definition, &xml);
        if (hr != S_OK || (hr = SchRpcRegisterTask(full_name, xml, flags, NULL, logon, 0, NULL, &actual_path, &error_info)) != S_OK)
        {
            free(full_name);
            free(regtask);
            SysFreeString(xml);
            return hr;
        }
        SysFreeString(xml);

        free(full_name);
        full_name = wcsdup(actual_path);
        MIDL_user_free(actual_path);
    }
    else
    {
        DWORD count = 0;
        WCHAR *xml = NULL;

        hr = SchRpcRetrieveTask(full_name, L"", &count, &xml);
        if (hr != S_OK || (hr = ITaskDefinition_put_XmlText(definition, xml)) != S_OK)
        {
            free(full_name);
            free(regtask);
            MIDL_user_free(xml);
            return hr;
        }
        MIDL_user_free(xml);
    }

    regtask->IRegisteredTask_iface.lpVtbl = &RegisteredTask_vtbl;
    regtask->path = full_name;
    regtask->ref = 1;
    regtask->taskdef = definition;
    *obj = &regtask->IRegisteredTask_iface;

    TRACE("created %p\n", *obj);

    return S_OK;
}

typedef struct
{
    IRegisteredTaskCollection IRegisteredTaskCollection_iface;
    LONG ref;
    WCHAR *path;
} RegisteredTaskCollection;

static inline RegisteredTaskCollection *impl_from_IRegisteredTaskCollection(IRegisteredTaskCollection *iface)
{
    return CONTAINING_RECORD(iface, RegisteredTaskCollection, IRegisteredTaskCollection_iface);
}

static ULONG WINAPI regtasks_AddRef(IRegisteredTaskCollection *iface)
{
    RegisteredTaskCollection *regtasks = impl_from_IRegisteredTaskCollection(iface);
    return InterlockedIncrement(&regtasks->ref);
}

static ULONG WINAPI regtasks_Release(IRegisteredTaskCollection *iface)
{
    RegisteredTaskCollection *regtasks = impl_from_IRegisteredTaskCollection(iface);
    LONG ref = InterlockedDecrement(&regtasks->ref);

    if (!ref)
    {
        TRACE("destroying %p\n", iface);
        free(regtasks->path);
        free(regtasks);
    }

    return ref;
}

static HRESULT WINAPI regtasks_QueryInterface(IRegisteredTaskCollection *iface, REFIID riid, void **obj)
{
    if (!riid || !obj) return E_INVALIDARG;

    TRACE("%p,%s,%p\n", iface, debugstr_guid(riid), obj);

    if (IsEqualGUID(riid, &IID_IRegisteredTaskCollection) ||
        IsEqualGUID(riid, &IID_IDispatch) ||
        IsEqualGUID(riid, &IID_IUnknown))
    {
        IRegisteredTaskCollection_AddRef(iface);
        *obj = iface;
        return S_OK;
    }

    FIXME("interface %s is not implemented\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI regtasks_GetTypeInfoCount(IRegisteredTaskCollection *iface, UINT *count)
{
    FIXME("%p,%p: stub\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI regtasks_GetTypeInfo(IRegisteredTaskCollection *iface, UINT index, LCID lcid, ITypeInfo **info)
{
    FIXME("%p,%u,%lu,%p: stub\n", iface, index, lcid, info);
    return E_NOTIMPL;
}

static HRESULT WINAPI regtasks_GetIDsOfNames(IRegisteredTaskCollection *iface, REFIID riid, LPOLESTR *names,
                                                UINT count, LCID lcid, DISPID *dispid)
{
    FIXME("%p,%s,%p,%u,%lu,%p: stub\n", iface, debugstr_guid(riid), names, count, lcid, dispid);
    return E_NOTIMPL;
}

static HRESULT WINAPI regtasks_Invoke(IRegisteredTaskCollection *iface, DISPID dispid, REFIID riid, LCID lcid, WORD flags,
                                         DISPPARAMS *params, VARIANT *result, EXCEPINFO *excepinfo, UINT *argerr)
{
    FIXME("%p,%ld,%s,%04lx,%04x,%p,%p,%p,%p: stub\n", iface, dispid, debugstr_guid(riid), lcid, flags,
          params, result, excepinfo, argerr);
    return E_NOTIMPL;
}

static HRESULT WINAPI regtasks_get_Count(IRegisteredTaskCollection *iface, LONG *count)
{
    HRESULT hr;
    RegisteredTaskCollection *reg_tasks = NULL;
    TASK_NAMES task_names = NULL;
    DWORD start_index = 0, num_tasks;

    reg_tasks = impl_from_IRegisteredTaskCollection(iface);

    if (!count)
        return E_POINTER;

    hr = SchRpcEnumTasks(reg_tasks->path, 0, &start_index, 0, &num_tasks, &task_names);
    if (FAILED(hr))
    {
        *count = 0;
        return hr;
    }

    *count = num_tasks;

    for (DWORD i = 0; i < num_tasks; i++)
        MIDL_user_free(task_names[i]);
    MIDL_user_free(task_names);

    return S_OK;
}

static HRESULT WINAPI regtasks_get_Item(IRegisteredTaskCollection *iface, VARIANT index, IRegisteredTask **regtask)
{
    HRESULT hr;
    VARIANT converted_index;
    TASK_NAMES task_names = NULL;
    ITaskDefinition *definition = NULL;
    RegisteredTaskCollection *collection = NULL;
    DWORD start_index = 0, num_tasks = 0;

    collection = impl_from_IRegisteredTaskCollection(iface);
    if (!regtask)
        return E_POINTER;
    *regtask = NULL;

    VariantInit(&converted_index);
    hr = VariantChangeType(&converted_index, &index, 0, VT_UI4);
    if (FAILED(hr))
        return hr;

    start_index = V_UI4(&index);
    if (start_index == 0)
    {
        VariantClear(&converted_index);
        return E_INVALIDARG;
    }
    start_index -= 1;

    hr = SchRpcEnumTasks(collection->path, 0, &start_index, 1, &num_tasks, &task_names);
    if (FAILED(hr))
    {
        VariantClear(&converted_index);
        return hr;
    }
    if (!task_names)
    {
        VariantClear(&converted_index);
        return E_INVALIDARG;
    }

    hr = TaskDefinition_create(&definition);
    if (FAILED(hr))
    {
        if(hr != E_OUTOFMEMORY)
            ITaskDefinition_Release(definition);
        VariantClear(&converted_index);
        return hr;
    }

    hr = RegisteredTask_create(collection->path, task_names[0], definition, TASK_VALIDATE_ONLY, TASK_LOGON_INTERACTIVE_TOKEN, regtask, FALSE);

    VariantClear(&converted_index);
    MIDL_user_free(task_names[0]);
    MIDL_user_free(task_names);

    if (hr != S_OK)
        ITaskDefinition_Release(definition);

    return hr;
}

static HRESULT WINAPI regtasks_get__NewEnum(IRegisteredTaskCollection *iface, IUnknown **penum)
{
    FIXME("%p,%p: stub\n", iface, penum);
    return E_NOTIMPL;
}

static const IRegisteredTaskCollectionVtbl RegisteredTaskCollection_vtbl =
{
    regtasks_QueryInterface,
    regtasks_AddRef,
    regtasks_Release,
    regtasks_GetTypeInfoCount,
    regtasks_GetTypeInfo,
    regtasks_GetIDsOfNames,
    regtasks_Invoke,
    regtasks_get_Count,
    regtasks_get_Item,
    regtasks_get__NewEnum
};

HRESULT RegisteredTaskCollection_create(const WCHAR *path, IRegisteredTaskCollection **obj)
{
    RegisteredTaskCollection *regtasks;

    regtasks = malloc(sizeof(*regtasks));
    if (!regtasks) return E_OUTOFMEMORY;

    regtasks->IRegisteredTaskCollection_iface.lpVtbl = &RegisteredTaskCollection_vtbl;
    regtasks->ref = 1;
    regtasks->path = wcsdup(path);
    *obj = &regtasks->IRegisteredTaskCollection_iface;

    TRACE("created %p\n", *obj);

    return S_OK;
}
