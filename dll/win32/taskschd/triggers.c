/*
 * PROJECT:     LiberNT Task Scheduler client
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Time, weekly, monthly, boot, idle and event trigger objects
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * One object backs the six interfaces: they all start with the ITrigger
 * methods, so each interface gets a vtable of thin wrappers around the
 * shared implementation plus its own properties.
 */

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "oleauto.h"
#include "taskschd.h"
#include "taskschd_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(taskschd);

typedef struct
{
    ITrigger ITrigger_iface;    /* lpVtbl points at the vtable of 'type' */
    LONG ref;
    TASK_TRIGGER_TYPE2 type;
    WCHAR *id;
    WCHAR *start_boundary;
    WCHAR *end_boundary;
    WCHAR *execution_time_limit;
    WCHAR *delay;
    WCHAR *random_delay;
    WCHAR *subscription;
    IRepetitionPattern *repetition;
    BOOL enabled;
    short days_of_week;
    short weeks_interval;
    short months_of_year;
    LONG days_of_month;
    BOOL last_day_of_month;
} GenericTrigger;

static inline GenericTrigger *trigger_from(void *iface)
{
    return CONTAINING_RECORD((ITrigger *)iface, GenericTrigger, ITrigger_iface);
}

static HRESULT get_str(const WCHAR *value, BSTR *str)
{
    if (!str) return E_POINTER;
    if (!value)
    {
        *str = NULL;
        return S_OK;
    }
    *str = SysAllocString(value);
    return *str ? S_OK : E_OUTOFMEMORY;
}

static HRESULT put_str(WCHAR **value, const WCHAR *str)
{
    WCHAR *copy = NULL;

    if (str && !(copy = wcsdup(str))) return E_OUTOFMEMORY;
    free(*value);
    *value = copy;
    return S_OK;
}

static const IID *trigger_iid(TASK_TRIGGER_TYPE2 type)
{
    switch (type)
    {
    case TASK_TRIGGER_TIME: return &IID_ITimeTrigger;
    case TASK_TRIGGER_WEEKLY: return &IID_IWeeklyTrigger;
    case TASK_TRIGGER_MONTHLY: return &IID_IMonthlyTrigger;
    case TASK_TRIGGER_BOOT: return &IID_IBootTrigger;
    case TASK_TRIGGER_IDLE: return &IID_IIdleTrigger;
    case TASK_TRIGGER_EVENT: return &IID_IEventTrigger;
    default: return &IID_ITrigger;
    }
}

/* ------------------------------------------------- shared ITrigger part */

static HRESULT trig_QueryInterface(ITrigger *iface, REFIID riid, void **obj)
{
    GenericTrigger *This = trigger_from(iface);

    if (!riid || !obj) return E_INVALIDARG;

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), obj);

    if (IsEqualGUID(riid, trigger_iid(This->type)) ||
        IsEqualGUID(riid, &IID_ITrigger) ||
        IsEqualGUID(riid, &IID_IDispatch) ||
        IsEqualGUID(riid, &IID_IUnknown))
    {
        ITrigger_AddRef(iface);
        *obj = iface;
        return S_OK;
    }

    FIXME("unsupported riid %s\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG trig_AddRef(ITrigger *iface)
{
    GenericTrigger *This = trigger_from(iface);
    return InterlockedIncrement(&This->ref);
}

static ULONG trig_Release(ITrigger *iface)
{
    GenericTrigger *This = trigger_from(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    if (!ref)
    {
        TRACE("destroying %p\n", iface);
        free(This->id);
        free(This->start_boundary);
        free(This->end_boundary);
        free(This->execution_time_limit);
        free(This->delay);
        free(This->random_delay);
        free(This->subscription);
        if (This->repetition) IRepetitionPattern_Release(This->repetition);
        free(This);
    }
    return ref;
}

static HRESULT trig_GetTypeInfoCount(ITrigger *iface, UINT *count)
{
    FIXME("(%p)->(%p)\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT trig_GetTypeInfo(ITrigger *iface, UINT index, LCID lcid, ITypeInfo **info)
{
    FIXME("(%p)->(%u %lu %p)\n", iface, index, lcid, info);
    return E_NOTIMPL;
}

static HRESULT trig_GetIDsOfNames(ITrigger *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid,
                                  DISPID *dispid)
{
    FIXME("(%p)->(%s %p %u %lu %p)\n", iface, debugstr_guid(riid), names, count, lcid, dispid);
    return E_NOTIMPL;
}

static HRESULT trig_Invoke(ITrigger *iface, DISPID dispid, REFIID riid, LCID lcid, WORD flags,
                           DISPPARAMS *params, VARIANT *result, EXCEPINFO *excepinfo, UINT *argerr)
{
    FIXME("(%p)->(%ld %s %lx %x %p %p %p %p)\n", iface, dispid, debugstr_guid(riid), lcid, flags,
          params, result, excepinfo, argerr);
    return E_NOTIMPL;
}

static HRESULT trig_get_Type(ITrigger *iface, TASK_TRIGGER_TYPE2 *type)
{
    GenericTrigger *This = trigger_from(iface);

    TRACE("(%p)->(%p)\n", This, type);

    if (!type) return E_POINTER;
    *type = This->type;
    return S_OK;
}

static HRESULT trig_get_Id(ITrigger *iface, BSTR *id)
{
    return get_str(trigger_from(iface)->id, id);
}

static HRESULT trig_put_Id(ITrigger *iface, BSTR id)
{
    return put_str(&trigger_from(iface)->id, id);
}

static HRESULT trig_get_Repetition(ITrigger *iface, IRepetitionPattern **repeat)
{
    GenericTrigger *This = trigger_from(iface);
    HRESULT hr;

    TRACE("(%p)->(%p)\n", This, repeat);

    if (!repeat) return E_POINTER;
    if (!This->repetition && FAILED(hr = RepetitionPattern_create(&This->repetition))) return hr;

    IRepetitionPattern_AddRef(This->repetition);
    *repeat = This->repetition;
    return S_OK;
}

static HRESULT trig_put_Repetition(ITrigger *iface, IRepetitionPattern *repeat)
{
    GenericTrigger *This = trigger_from(iface);
    IRepetitionPattern *mine;
    VARIANT_BOOL stop = VARIANT_FALSE;
    BSTR str = NULL;
    HRESULT hr;

    TRACE("(%p)->(%p)\n", This, repeat);

    if (!repeat) return E_POINTER;
    if (FAILED(hr = trig_get_Repetition(iface, &mine))) return hr;

    /* copy the values: the pattern passed in may belong to another trigger */
    if (SUCCEEDED(IRepetitionPattern_get_Interval(repeat, &str)))
        hr = IRepetitionPattern_put_Interval(mine, str);
    SysFreeString(str);
    str = NULL;
    if (SUCCEEDED(hr) && SUCCEEDED(IRepetitionPattern_get_Duration(repeat, &str)))
        hr = IRepetitionPattern_put_Duration(mine, str);
    SysFreeString(str);
    if (SUCCEEDED(hr) && SUCCEEDED(IRepetitionPattern_get_StopAtDurationEnd(repeat, &stop)))
        hr = IRepetitionPattern_put_StopAtDurationEnd(mine, stop);
    IRepetitionPattern_Release(mine);
    return hr;
}

static HRESULT trig_get_ExecutionTimeLimit(ITrigger *iface, BSTR *limit)
{
    return get_str(trigger_from(iface)->execution_time_limit, limit);
}

static HRESULT trig_put_ExecutionTimeLimit(ITrigger *iface, BSTR limit)
{
    return put_str(&trigger_from(iface)->execution_time_limit, limit);
}

static HRESULT trig_get_StartBoundary(ITrigger *iface, BSTR *start)
{
    return get_str(trigger_from(iface)->start_boundary, start);
}

static HRESULT trig_put_StartBoundary(ITrigger *iface, BSTR start)
{
    return put_str(&trigger_from(iface)->start_boundary, start);
}

static HRESULT trig_get_EndBoundary(ITrigger *iface, BSTR *end)
{
    return get_str(trigger_from(iface)->end_boundary, end);
}

static HRESULT trig_put_EndBoundary(ITrigger *iface, BSTR end)
{
    return put_str(&trigger_from(iface)->end_boundary, end);
}

static HRESULT trig_get_Enabled(ITrigger *iface, VARIANT_BOOL *enabled)
{
    if (!enabled) return E_POINTER;
    *enabled = trigger_from(iface)->enabled ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

static HRESULT trig_put_Enabled(ITrigger *iface, VARIANT_BOOL enabled)
{
    trigger_from(iface)->enabled = !!enabled;
    return S_OK;
}

/* Wrappers with the exact signature of each interface's ITrigger slots. */
#define TRIGGER_COMMON(I, P) \
static HRESULT WINAPI P##_QueryInterface(I *iface, REFIID riid, void **obj) \
{ return trig_QueryInterface((ITrigger *)iface, riid, obj); } \
static ULONG WINAPI P##_AddRef(I *iface) { return trig_AddRef((ITrigger *)iface); } \
static ULONG WINAPI P##_Release(I *iface) { return trig_Release((ITrigger *)iface); } \
static HRESULT WINAPI P##_GetTypeInfoCount(I *iface, UINT *count) \
{ return trig_GetTypeInfoCount((ITrigger *)iface, count); } \
static HRESULT WINAPI P##_GetTypeInfo(I *iface, UINT index, LCID lcid, ITypeInfo **info) \
{ return trig_GetTypeInfo((ITrigger *)iface, index, lcid, info); } \
static HRESULT WINAPI P##_GetIDsOfNames(I *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, \
                                       DISPID *dispid) \
{ return trig_GetIDsOfNames((ITrigger *)iface, riid, names, count, lcid, dispid); } \
static HRESULT WINAPI P##_Invoke(I *iface, DISPID dispid, REFIID riid, LCID lcid, WORD flags, \
                                 DISPPARAMS *params, VARIANT *result, EXCEPINFO *excepinfo, UINT *argerr) \
{ return trig_Invoke((ITrigger *)iface, dispid, riid, lcid, flags, params, result, excepinfo, argerr); } \
static HRESULT WINAPI P##_get_Type(I *iface, TASK_TRIGGER_TYPE2 *type) \
{ return trig_get_Type((ITrigger *)iface, type); } \
static HRESULT WINAPI P##_get_Id(I *iface, BSTR *id) { return trig_get_Id((ITrigger *)iface, id); } \
static HRESULT WINAPI P##_put_Id(I *iface, BSTR id) { return trig_put_Id((ITrigger *)iface, id); } \
static HRESULT WINAPI P##_get_Repetition(I *iface, IRepetitionPattern **repeat) \
{ return trig_get_Repetition((ITrigger *)iface, repeat); } \
static HRESULT WINAPI P##_put_Repetition(I *iface, IRepetitionPattern *repeat) \
{ return trig_put_Repetition((ITrigger *)iface, repeat); } \
static HRESULT WINAPI P##_get_ExecutionTimeLimit(I *iface, BSTR *limit) \
{ return trig_get_ExecutionTimeLimit((ITrigger *)iface, limit); } \
static HRESULT WINAPI P##_put_ExecutionTimeLimit(I *iface, BSTR limit) \
{ return trig_put_ExecutionTimeLimit((ITrigger *)iface, limit); } \
static HRESULT WINAPI P##_get_StartBoundary(I *iface, BSTR *start) \
{ return trig_get_StartBoundary((ITrigger *)iface, start); } \
static HRESULT WINAPI P##_put_StartBoundary(I *iface, BSTR start) \
{ return trig_put_StartBoundary((ITrigger *)iface, start); } \
static HRESULT WINAPI P##_get_EndBoundary(I *iface, BSTR *end) \
{ return trig_get_EndBoundary((ITrigger *)iface, end); } \
static HRESULT WINAPI P##_put_EndBoundary(I *iface, BSTR end) \
{ return trig_put_EndBoundary((ITrigger *)iface, end); } \
static HRESULT WINAPI P##_get_Enabled(I *iface, VARIANT_BOOL *enabled) \
{ return trig_get_Enabled((ITrigger *)iface, enabled); } \
static HRESULT WINAPI P##_put_Enabled(I *iface, VARIANT_BOOL enabled) \
{ return trig_put_Enabled((ITrigger *)iface, enabled); }

#define TRIGGER_COMMON_VTBL(P) \
    P##_QueryInterface, P##_AddRef, P##_Release, P##_GetTypeInfoCount, P##_GetTypeInfo, \
    P##_GetIDsOfNames, P##_Invoke, P##_get_Type, P##_get_Id, P##_put_Id, P##_get_Repetition, \
    P##_put_Repetition, P##_get_ExecutionTimeLimit, P##_put_ExecutionTimeLimit, \
    P##_get_StartBoundary, P##_put_StartBoundary, P##_get_EndBoundary, P##_put_EndBoundary, \
    P##_get_Enabled, P##_put_Enabled

/* ----------------------------------------------------------- ITimeTrigger */

TRIGGER_COMMON(ITimeTrigger, TimeTrigger)

static HRESULT WINAPI TimeTrigger_get_RandomDelay(ITimeTrigger *iface, BSTR *delay)
{
    return get_str(trigger_from(iface)->random_delay, delay);
}

static HRESULT WINAPI TimeTrigger_put_RandomDelay(ITimeTrigger *iface, BSTR delay)
{
    return put_str(&trigger_from(iface)->random_delay, delay);
}

static const ITimeTriggerVtbl TimeTrigger_vtbl =
{
    TRIGGER_COMMON_VTBL(TimeTrigger),
    TimeTrigger_get_RandomDelay,
    TimeTrigger_put_RandomDelay
};

/* --------------------------------------------------------- IWeeklyTrigger */

TRIGGER_COMMON(IWeeklyTrigger, WeeklyTrigger)

static HRESULT WINAPI WeeklyTrigger_get_DaysOfWeek(IWeeklyTrigger *iface, short *days)
{
    if (!days) return E_POINTER;
    *days = trigger_from(iface)->days_of_week;
    return S_OK;
}

static HRESULT WINAPI WeeklyTrigger_put_DaysOfWeek(IWeeklyTrigger *iface, short days)
{
    if (days & ~0x7f) return E_INVALIDARG;
    trigger_from(iface)->days_of_week = days;
    return S_OK;
}

static HRESULT WINAPI WeeklyTrigger_get_WeeksInterval(IWeeklyTrigger *iface, short *weeks)
{
    if (!weeks) return E_POINTER;
    *weeks = trigger_from(iface)->weeks_interval;
    return S_OK;
}

static HRESULT WINAPI WeeklyTrigger_put_WeeksInterval(IWeeklyTrigger *iface, short weeks)
{
    if (weeks <= 0) return E_INVALIDARG;
    trigger_from(iface)->weeks_interval = weeks;
    return S_OK;
}

static HRESULT WINAPI WeeklyTrigger_get_RandomDelay(IWeeklyTrigger *iface, BSTR *delay)
{
    return get_str(trigger_from(iface)->random_delay, delay);
}

static HRESULT WINAPI WeeklyTrigger_put_RandomDelay(IWeeklyTrigger *iface, BSTR delay)
{
    return put_str(&trigger_from(iface)->random_delay, delay);
}

static const IWeeklyTriggerVtbl WeeklyTrigger_vtbl =
{
    TRIGGER_COMMON_VTBL(WeeklyTrigger),
    WeeklyTrigger_get_DaysOfWeek,
    WeeklyTrigger_put_DaysOfWeek,
    WeeklyTrigger_get_WeeksInterval,
    WeeklyTrigger_put_WeeksInterval,
    WeeklyTrigger_get_RandomDelay,
    WeeklyTrigger_put_RandomDelay
};

/* -------------------------------------------------------- IMonthlyTrigger */

TRIGGER_COMMON(IMonthlyTrigger, MonthlyTrigger)

static HRESULT WINAPI MonthlyTrigger_get_DaysOfMonth(IMonthlyTrigger *iface, LONG *days)
{
    if (!days) return E_POINTER;
    *days = trigger_from(iface)->days_of_month;
    return S_OK;
}

static HRESULT WINAPI MonthlyTrigger_put_DaysOfMonth(IMonthlyTrigger *iface, LONG days)
{
    if (days & 0x80000000) return E_INVALIDARG;
    trigger_from(iface)->days_of_month = days;
    return S_OK;
}

static HRESULT WINAPI MonthlyTrigger_get_MonthsOfYear(IMonthlyTrigger *iface, short *months)
{
    if (!months) return E_POINTER;
    *months = trigger_from(iface)->months_of_year;
    return S_OK;
}

static HRESULT WINAPI MonthlyTrigger_put_MonthsOfYear(IMonthlyTrigger *iface, short months)
{
    if (months & ~0xfff) return E_INVALIDARG;
    trigger_from(iface)->months_of_year = months;
    return S_OK;
}

static HRESULT WINAPI MonthlyTrigger_get_RunOnLastDayOfMonth(IMonthlyTrigger *iface, VARIANT_BOOL *last)
{
    if (!last) return E_POINTER;
    *last = trigger_from(iface)->last_day_of_month ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

static HRESULT WINAPI MonthlyTrigger_put_RunOnLastDayOfMonth(IMonthlyTrigger *iface, VARIANT_BOOL last)
{
    trigger_from(iface)->last_day_of_month = !!last;
    return S_OK;
}

static HRESULT WINAPI MonthlyTrigger_get_RandomDelay(IMonthlyTrigger *iface, BSTR *delay)
{
    return get_str(trigger_from(iface)->random_delay, delay);
}

static HRESULT WINAPI MonthlyTrigger_put_RandomDelay(IMonthlyTrigger *iface, BSTR delay)
{
    return put_str(&trigger_from(iface)->random_delay, delay);
}

static const IMonthlyTriggerVtbl MonthlyTrigger_vtbl =
{
    TRIGGER_COMMON_VTBL(MonthlyTrigger),
    MonthlyTrigger_get_DaysOfMonth,
    MonthlyTrigger_put_DaysOfMonth,
    MonthlyTrigger_get_MonthsOfYear,
    MonthlyTrigger_put_MonthsOfYear,
    MonthlyTrigger_get_RunOnLastDayOfMonth,
    MonthlyTrigger_put_RunOnLastDayOfMonth,
    MonthlyTrigger_get_RandomDelay,
    MonthlyTrigger_put_RandomDelay
};

/* ----------------------------------------------------------- IBootTrigger */

TRIGGER_COMMON(IBootTrigger, BootTrigger)

static HRESULT WINAPI BootTrigger_get_Delay(IBootTrigger *iface, BSTR *delay)
{
    return get_str(trigger_from(iface)->delay, delay);
}

static HRESULT WINAPI BootTrigger_put_Delay(IBootTrigger *iface, BSTR delay)
{
    return put_str(&trigger_from(iface)->delay, delay);
}

static const IBootTriggerVtbl BootTrigger_vtbl =
{
    TRIGGER_COMMON_VTBL(BootTrigger),
    BootTrigger_get_Delay,
    BootTrigger_put_Delay
};

/* ----------------------------------------------------------- IIdleTrigger */

TRIGGER_COMMON(IIdleTrigger, IdleTrigger)

static const IIdleTriggerVtbl IdleTrigger_vtbl =
{
    TRIGGER_COMMON_VTBL(IdleTrigger)
};

/* ---------------------------------------------------------- IEventTrigger */

TRIGGER_COMMON(IEventTrigger, EventTrigger)

static HRESULT WINAPI EventTrigger_get_Subscription(IEventTrigger *iface, BSTR *query)
{
    return get_str(trigger_from(iface)->subscription, query);
}

static HRESULT WINAPI EventTrigger_put_Subscription(IEventTrigger *iface, BSTR query)
{
    return put_str(&trigger_from(iface)->subscription, query);
}

static HRESULT WINAPI EventTrigger_get_Delay(IEventTrigger *iface, BSTR *delay)
{
    return get_str(trigger_from(iface)->delay, delay);
}

static HRESULT WINAPI EventTrigger_put_Delay(IEventTrigger *iface, BSTR delay)
{
    return put_str(&trigger_from(iface)->delay, delay);
}

static HRESULT WINAPI EventTrigger_get_ValueQueries(IEventTrigger *iface, ITaskNamedValueCollection **queries)
{
    FIXME("(%p)->(%p): ValueQueries are not supported\n", iface, queries);
    return E_NOTIMPL;
}

static HRESULT WINAPI EventTrigger_put_ValueQueries(IEventTrigger *iface, ITaskNamedValueCollection *queries)
{
    FIXME("(%p)->(%p): ValueQueries are not supported\n", iface, queries);
    return E_NOTIMPL;
}

static const IEventTriggerVtbl EventTrigger_vtbl =
{
    TRIGGER_COMMON_VTBL(EventTrigger),
    EventTrigger_get_Subscription,
    EventTrigger_put_Subscription,
    EventTrigger_get_Delay,
    EventTrigger_put_Delay,
    EventTrigger_get_ValueQueries,
    EventTrigger_put_ValueQueries
};

/* ----------------------------------------------------------------- create */

HRESULT GenericTrigger_create(TASK_TRIGGER_TYPE2 type, ITrigger **trigger)
{
    GenericTrigger *This;
    const void *vtbl;

    switch (type)
    {
    case TASK_TRIGGER_TIME: vtbl = &TimeTrigger_vtbl; break;
    case TASK_TRIGGER_WEEKLY: vtbl = &WeeklyTrigger_vtbl; break;
    case TASK_TRIGGER_MONTHLY: vtbl = &MonthlyTrigger_vtbl; break;
    case TASK_TRIGGER_BOOT: vtbl = &BootTrigger_vtbl; break;
    case TASK_TRIGGER_IDLE: vtbl = &IdleTrigger_vtbl; break;
    case TASK_TRIGGER_EVENT: vtbl = &EventTrigger_vtbl; break;
    default: return E_INVALIDARG;
    }

    if (!(This = calloc(1, sizeof(*This)))) return E_OUTOFMEMORY;

    This->ITrigger_iface.lpVtbl = (ITriggerVtbl *)vtbl;
    This->ref = 1;
    This->type = type;
    This->enabled = TRUE;
    This->weeks_interval = 1;

    *trigger = &This->ITrigger_iface;
    TRACE("created %p, type %d\n", *trigger, type);
    return S_OK;
}
