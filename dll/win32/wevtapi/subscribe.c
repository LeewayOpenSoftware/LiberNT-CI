/*
 * PROJECT:     LiberNT wevtapi
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Event subscriptions (pull and push models)
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 *
 * Each subscription has a thread that wakes when the service reports a change
 * of the log (NotifyChangeEventLog), with a slow look at the log range as a
 * safety net. Where the service cannot notify (an older or remote one) the
 * thread looks every EVT_POLL_MS instead. Push subscriptions read and deliver
 * from that thread; pull subscriptions only signal the caller's event and
 * EvtNext reads.
 */

#include "wevtapi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wevtapi);

#define EVT_POLL_MS     250
#define EVT_FALLBACK_MS 5000

static void free_subscription(struct evt_subscription *sub)
{
    evt_free_result_set(&sub->reader);
    if (sub->change) CloseHandle(sub->change);
    if (sub->stop) CloseHandle(sub->stop);
    if (sub->thread) CloseHandle(sub->thread);
    sub->hdr.magic = 0;
    heap_free(sub);
}

/* TRUE when the reader has at least one record it has not read yet */
static BOOL records_pending(struct evt_subscription *sub)
{
    DWORD oldest, count;
    BOOL pending;

    EnterCriticalSection(&sub->reader.cs);
    pending = evt_log_range(sub->reader.classic, &oldest, &count) && count &&
              (!sub->reader.started || sub->reader.next <= oldest + count - 1);
    LeaveCriticalSection(&sub->reader.cs);
    return pending;
}

static void deliver_push(struct evt_subscription *sub)
{
    const EVENTLOGRECORD *r;
    EVT_HANDLE event;
    DWORD err;

    while (!sub->closing)
    {
        EnterCriticalSection(&sub->reader.cs);
        r = evt_reader_next(&sub->reader, evt_now_filetime());
        err = r ? ERROR_SUCCESS : GetLastError();
        event = r ? evt_create_event(r, sub->reader.channel) : NULL;
        if (r && !event) err = GetLastError();
        LeaveCriticalSection(&sub->reader.cs);

        if (err == ERROR_NO_MORE_ITEMS || err == ERROR_CANCELLED)
            return;
        if (err != ERROR_SUCCESS)
        {
            sub->callback(EvtSubscribeActionError, sub->context, (EVT_HANDLE)(ULONG_PTR)err);
            return;
        }
        sub->callback(EvtSubscribeActionDeliver, sub->context, event);
        EvtClose(event);
    }
}

static DWORD WINAPI subscription_thread(void *arg)
{
    struct evt_subscription *sub = arg;
    HANDLE waits[2];
    DWORD n = 0, ret;

    waits[n++] = sub->stop;
    if (sub->change) waits[n++] = sub->change;

    for (;;)
    {
        if (sub->closing)
            break;
        if (records_pending(sub))
        {
            if (sub->callback)
                deliver_push(sub);
            else
                SetEvent(sub->signal);
        }
        if (sub->closing)
            break;
        ret = WaitForMultipleObjects(n, waits, FALSE, sub->change ? EVT_FALLBACK_MS : EVT_POLL_MS);
        if (ret != WAIT_TIMEOUT && ret != WAIT_OBJECT_0 + 1)
            break;
    }
    /* EvtClose called from inside a callback left the object to this thread */
    if (InterlockedCompareExchange(&sub->closing, 0, 0) == 2)
        free_subscription(sub);
    return 0;
}

EVT_HANDLE WINAPI EvtSubscribe(EVT_HANDLE session, HANDLE signal, const WCHAR *channel, const WCHAR *query,
                               EVT_HANDLE bookmark, void *context, EVT_SUBSCRIBE_CALLBACK callback, DWORD flags)
{
    struct evt_subscription *sub;
    struct evt_session *s = NULL;
    struct evt_bookmark *bm = NULL;
    struct evt_filter *filter;
    WCHAR *ql_channel;
    DWORD origin = flags & EvtSubscribeOriginMask, oldest, count, err;

    TRACE("(%p %p %s %s %p %p %p %#lx)\n", session, signal, debugstr_w(channel), debugstr_w(query), bookmark,
          context, callback, flags);

    if (session && !(s = evt_get_object(session, EVT_TYPE_SESSION)))
        return NULL;
    if ((!signal && !callback) || !origin ||
        (flags & ~(EvtSubscribeOriginMask | EvtSubscribeTolerateQueryErrors | EvtSubscribeStrict)) ||
        ((origin == EvtSubscribeStartAfterBookmark) != (bookmark != NULL)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (bookmark && (!(bm = evt_get_object(bookmark, EVT_TYPE_BOOKMARK)) || !bm->valid))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!evt_parse_query(query, &filter, &ql_channel))
        return NULL;
    if (!channel && !ql_channel)
    {
        evt_free_filter(filter);
        SetLastError(ERROR_EVT_INVALID_CHANNEL_PATH);
        return NULL;
    }
    if (!(sub = evt_alloc_object(EVT_TYPE_SUBSCRIPTION, sizeof(*sub))))
    {
        evt_free_filter(filter);
        heap_free(ql_channel);
        return NULL;
    }
    InitializeCriticalSection(&sub->reader.cs);
    sub->reader.filter = filter;
    sub->reader.channel = evt_strdupW(channel ? channel : ql_channel);
    heap_free(ql_channel);
    sub->signal = signal;
    sub->callback = callback;
    sub->context = context;

    if (!sub->reader.channel)
    {
        err = ERROR_OUTOFMEMORY;
        goto fail;
    }
    if (bm && lstrcmpiW(bm->channel, sub->reader.channel))
    {
        if (flags & EvtSubscribeStrict)
        {
            err = ERROR_NOT_FOUND;
            goto fail;
        }
        origin = EvtSubscribeStartAtOldestRecord;
    }
    if (!(sub->reader.classic = evt_open_classic(s ? s->server : NULL, sub->reader.channel, FALSE)) ||
        !evt_log_range(sub->reader.classic, &oldest, &count))
    {
        err = GetLastError();
        goto fail;
    }
    switch (origin)
    {
    case EvtSubscribeToFutureEvents:
        /* An empty log: the first record that appears is a future one */
        sub->reader.started = (count != 0);
        sub->reader.next = oldest + count;
        break;
    case EvtSubscribeStartAfterBookmark:
        if (bm->record_id < 0xFFFFFFFF)
        {
            sub->reader.started = TRUE;
            sub->reader.next = (DWORD)bm->record_id + 1;
            break;
        }
        /* fall through */
    default:
        sub->reader.started = FALSE;   /* starts at the oldest record */
        break;
    }

    /* Change notification from the service; without it the thread polls */
    if ((sub->change = CreateEventW(NULL, FALSE, FALSE, NULL)) &&
        !NotifyChangeEventLog(sub->reader.classic, sub->change))
    {
        TRACE("no change notification (error %lu): polling\n", GetLastError());
        CloseHandle(sub->change);
        sub->change = NULL;
    }

    if (!(sub->stop = CreateEventW(NULL, TRUE, FALSE, NULL)) ||
        !(sub->thread = CreateThread(NULL, 0, subscription_thread, sub, 0, &sub->thread_id)))
    {
        err = GetLastError();
        goto fail;
    }
    return sub;

fail:
    free_subscription(sub);
    SetLastError(err);
    return NULL;
}

BOOL evt_subscription_next(struct evt_subscription *sub, DWORD size, EVT_HANDLE *events, DWORD *returned)
{
    const EVENTLOGRECORD *r;
    ULONGLONG now = evt_now_filetime();
    DWORD n = 0, err = ERROR_SUCCESS;

    if (sub->callback)
    {
        /* Push subscriptions deliver through the callback only */
        SetLastError(ERROR_INVALID_OPERATION);
        return FALSE;
    }
    EnterCriticalSection(&sub->reader.cs);
    while (n < size)
    {
        if (!(r = evt_reader_next(&sub->reader, now)))
        {
            err = GetLastError();
            break;
        }
        if (!(events[n] = evt_create_event(r, sub->reader.channel)))
        {
            err = GetLastError();
            break;
        }
        n++;
    }
    LeaveCriticalSection(&sub->reader.cs);

    *returned = n;
    if (n)
        return TRUE;
    SetLastError(err == ERROR_SUCCESS ? ERROR_NO_MORE_ITEMS : err);
    return FALSE;
}

BOOL evt_close_subscription(struct evt_subscription *sub)
{
    if (GetCurrentThreadId() == sub->thread_id)
    {
        /* From inside a push callback: the thread frees the object when it returns */
        InterlockedExchange(&sub->closing, 2);
        SetEvent(sub->stop);
        return TRUE;
    }
    InterlockedExchange(&sub->closing, 1);
    SetEvent(sub->stop);
    WaitForSingleObject(sub->thread, INFINITE);
    free_subscription(sub);
    return TRUE;
}

BOOL evt_cancel_subscription(struct evt_subscription *sub)
{
    EnterCriticalSection(&sub->reader.cs);
    sub->reader.cancelled = TRUE;
    LeaveCriticalSection(&sub->reader.cs);
    return TRUE;
}
