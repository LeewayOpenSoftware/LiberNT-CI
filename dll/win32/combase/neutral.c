/*
 * PROJECT:     LiberNT COM base
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Neutral apartment and default contexts
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS
#define USE_COM_CONTEXT_DEF
#include "objbase.h"
#include "comsvcs.h"
#include "ctxtcall.h"
#include "combase_private.h"

#include "wine/debug.h"
#include "wine/exception.h"

WINE_DEFAULT_DEBUG_CHANNEL(ole);

static IObjContext *neutral_token;

static IObjContext *read_token(void)
{
    return InterlockedCompareExchangePointer((void **)&neutral_token, NULL, NULL);
}

/* The neutral apartment has no thread: a call into it runs on the calling thread, which
 * switches its apartment and context for the duration of the call */
struct apartment *neutral_get_apartment(void)
{
    struct apartment *apt;
    IObjContext *token;

    if (!(apt = apartment_get_na()))
        return NULL;

    if (!read_token())
    {
        if (!(token = context_create_token(apt->oxid)))
        {
            apartment_release(apt);
            return NULL;
        }
        if (InterlockedCompareExchangePointer((void **)&neutral_token, token, NULL))
            context_release_token(token);
    }
    return apt;
}

BOOL neutral_is_context(IObjContext *token)
{
    return token && token == read_token();
}

IObjContext *neutral_context_token(void)
{
    return read_token();
}

HRESULT neutral_enter(struct neutral_frame *frame)
{
    struct tlsdata *tlsdata;
    struct apartment *apt;
    HRESULT hr;

    frame->entered = FALSE;
    if (FAILED(hr = com_get_tlsdata(&tlsdata)))
        return hr;
    if (tlsdata->apt && tlsdata->apt->neutral)
        return S_OK;
    if (!(apt = neutral_get_apartment()))
        return E_OUTOFMEMORY;

    frame->apt = tlsdata->apt;
    frame->token = tlsdata->context_token;
    frame->prev = tlsdata->neutral_frame;
    frame->neutral = apt;
    /* keeps a CoInitializeEx/CoUninitialize pair inside the call from leaving the apartment */
    tlsdata->inits++;
    tlsdata->apt = apt;
    tlsdata->context_token = read_token();
    tlsdata->neutral_frame = frame;
    frame->entered = TRUE;
    return S_OK;
}

void neutral_leave(struct neutral_frame *frame)
{
    struct tlsdata *tlsdata;

    if (!frame->entered || FAILED(com_get_tlsdata(&tlsdata)))
        return;

    if (tlsdata->apt != frame->neutral)
        ERR("the thread left the neutral apartment inside a neutral call\n");
    tlsdata->apt = frame->apt;
    tlsdata->context_token = frame->token;
    tlsdata->neutral_frame = frame->prev;
    if (tlsdata->inits)
        tlsdata->inits--;
    apartment_release(frame->neutral);
}

/* Switches a thread that is in the neutral apartment back to the apartment it came from */
BOOL neutral_suspend(struct neutral_frame *saved)
{
    struct neutral_frame *frame;
    struct tlsdata *tlsdata;

    saved->entered = FALSE;
    if (FAILED(com_get_tlsdata(&tlsdata)) || !tlsdata->apt || !tlsdata->apt->neutral ||
        !(frame = tlsdata->neutral_frame))
        return FALSE;

    saved->apt = tlsdata->apt;
    saved->token = tlsdata->context_token;
    saved->prev = frame;
    tlsdata->apt = frame->apt;
    tlsdata->context_token = frame->token;
    tlsdata->neutral_frame = frame->prev;
    saved->entered = TRUE;
    return TRUE;
}

void neutral_resume(struct neutral_frame *saved)
{
    struct tlsdata *tlsdata;

    if (!saved->entered || FAILED(com_get_tlsdata(&tlsdata)))
        return;

    tlsdata->apt = saved->apt;
    tlsdata->context_token = saved->token;
    tlsdata->neutral_frame = saved->prev;
}

/* The apartment a neutral call on this thread came from, if the call target is that apartment */
BOOL neutral_is_outer(OXID oxid)
{
    struct neutral_frame *frame;
    struct tlsdata *tlsdata;

    if (FAILED(com_get_tlsdata(&tlsdata)) || !tlsdata->apt || !tlsdata->apt->neutral ||
        !(frame = tlsdata->neutral_frame) || !frame->apt)
        return FALSE;
    return frame->apt->oxid == oxid;
}

/* A thread in the neutral apartment waits like the apartment it came from */
struct apartment *neutral_wait_apartment(struct apartment *apt)
{
    struct neutral_frame *frame;
    struct tlsdata *tlsdata;

    if (!apt || !apt->neutral || FAILED(com_get_tlsdata(&tlsdata)) || !(frame = tlsdata->neutral_frame))
        return apt;
    return frame->apt ? frame->apt : apt;
}

BOOL neutral_get_apartment_type(APTTYPE *type, APTTYPEQUALIFIER *qualifier)
{
    struct neutral_frame *frame;
    struct tlsdata *tlsdata;

    if (FAILED(com_get_tlsdata(&tlsdata)) || !tlsdata->apt || !tlsdata->apt->neutral)
        return FALSE;

    frame = tlsdata->neutral_frame;
    *type = APTTYPE_NA;
    if (!frame || !frame->apt)
        *qualifier = APTTYPEQUALIFIER_NA_ON_IMPLICIT_MTA;
    else if (frame->apt->multi_threaded)
        *qualifier = APTTYPEQUALIFIER_NA_ON_MTA;
    else if (frame->apt->main)
        *qualifier = APTTYPEQUALIFIER_NA_ON_MAINSTA;
    else
        *qualifier = APTTYPEQUALIFIER_NA_ON_STA;
    return TRUE;
}

HRESULT neutral_get_class_object(struct apartment *apt, const struct class_reg_data *regdata,
        REFCLSID rclsid, REFIID riid, void **ppv)
{
    static const LARGE_INTEGER zero;
    struct neutral_frame frame;
    IUnknown *object;
    IStream *stream;
    HRESULT hr;

    if (apt->neutral)
        return apartment_load_class_object(apt, regdata, rclsid, riid, ppv);

    if (FAILED(hr = CreateStreamOnHGlobal(NULL, TRUE, &stream)))
        return hr;

    if (SUCCEEDED(hr = neutral_enter(&frame)))
    {
        hr = apartment_load_class_object(com_get_current_apt(), regdata, rclsid, riid, (void **)&object);
        if (SUCCEEDED(hr))
        {
            hr = CoMarshalInterface(stream, riid, object, MSHCTX_INPROC, NULL, MSHLFLAGS_NORMAL);
            IUnknown_Release(object);
        }
        neutral_leave(&frame);
    }

    if (SUCCEEDED(hr))
    {
        IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
        hr = CoUnmarshalInterface(stream, riid, ppv);
    }
    IStream_Release(stream);
    return hr;
}

HRESULT neutral_context_callback(IComThreadingInfo *info, PFNCONTEXTCALL callback, ComCallData *param, REFIID riid)
{
    struct neutral_frame frame;
    BOOL activity = IsEqualIID(riid, &IID_IEnterActivityWithNoLock);
    GUID thread_id;
    HRESULT hr;

    if (activity)
    {
        if (FAILED(hr = IComThreadingInfo_GetCurrentLogicalThreadId(info, &thread_id)) ||
            FAILED(hr = IComThreadingInfo_SetCurrentLogicalThreadId(info, riid)))
            return hr;
    }

    if (SUCCEEDED(hr = neutral_enter(&frame)))
    {
        __TRY
        {
            hr = callback(param);
        }
        __EXCEPT_ALL
        {
            hr = RPC_E_SERVERFAULT;
        }
        __ENDTRY
        neutral_leave(&frame);
    }

    if (activity)
        IComThreadingInfo_SetCurrentLogicalThreadId(info, &thread_id);

    TRACE("callback returned %lx\n", hr);
    return hr;
}

static HRESULT query_token(IObjContext *token, REFIID riid, void **obj)
{
    return token ? IObjContext_QueryInterface(token, riid, obj) : E_OUTOFMEMORY;
}

/***********************************************************************
 *           CoGetDefaultContext    (combase.@)
 */
HRESULT WINAPI CoGetDefaultContext(APTTYPE type, REFIID riid, void **obj)
{
    struct apartment *apt, *target;
    IObjContext *token;
    HRESULT hr;

    TRACE("%d, %s, %p\n", type, debugstr_guid(riid), obj);

    if (!obj)
        return E_INVALIDARG;
    *obj = NULL;

    if (type != APTTYPE_CURRENT && type != APTTYPE_MTA && type != APTTYPE_NA && type != APTTYPE_MAINSTA)
        return E_INVALIDARG;

    if (!(apt = apartment_get_current_or_mta()))
        return CO_E_NOTINITIALIZED;

    if (type == APTTYPE_CURRENT ||
        (type == APTTYPE_MTA && apt->multi_threaded && !apt->neutral) ||
        (type == APTTYPE_NA && apt->neutral))
    {
        if (SUCCEEDED(hr = CoGetContextToken((ULONG_PTR *)&token)))
            hr = IObjContext_QueryInterface(token, riid, obj);
    }
    else if (type == APTTYPE_NA)
    {
        if (!(target = neutral_get_apartment()))
            hr = E_OUTOFMEMORY;
        else
        {
            hr = query_token(read_token(), riid, obj);
            apartment_release(target);
        }
    }
    else if (type == APTTYPE_MTA)
    {
        if (!(target = apartment_get_mta()))
            hr = CO_E_NOTINITIALIZED;
        else
        {
            token = context_get_mta_token(target->oxid);
            hr = query_token(token, riid, obj);
            if (token)
                context_release_token(token);
            apartment_release(target);
        }
    }
    else if (!(target = apartment_get_main_sta()))
    {
        hr = CO_E_NOTINITIALIZED;
    }
    else
    {
        if (target == apt)
        {
            if (SUCCEEDED(hr = CoGetContextToken((ULONG_PTR *)&token)))
                hr = IObjContext_QueryInterface(token, riid, obj);
        }
        else
        {
            token = context_create_token(target->oxid);
            hr = query_token(token, riid, obj);
            if (token)
                context_release_token(token);
        }
        apartment_release(target);
    }

    apartment_release(apt);
    return hr;
}

/***********************************************************************
 *           CoDisconnectContext    (combase.@)
 */
HRESULT WINAPI CoDisconnectContext(DWORD timeout)
{
    struct apartment *apt;

    TRACE("%lu\n", timeout);

    if (!(apt = apartment_get_current_or_mta()))
        return RPC_E_NO_CONTEXT;
    apartment_release(apt);

    /* Every context here is the default context of its apartment, and those cannot be disconnected */
    return CO_E_NOT_SUPPORTED;
}
