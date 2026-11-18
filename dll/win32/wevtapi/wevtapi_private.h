/*
 * PROJECT:     LiberNT wevtapi
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Private definitions shared by the wevtapi sources
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#ifndef __WEVTAPI_PRIVATE_H
#define __WEVTAPI_PRIVATE_H

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winnls.h"
#include "sddl.h"
#include "winevt.h"
#include "wine/debug.h"
#include "wine/heap.h"

#define EVT_MAGIC 0x45767448   /* 'HtvE' */

enum
{
    EVT_TYPE_ANY = 0,
    EVT_TYPE_SESSION,
    EVT_TYPE_LOG,
    EVT_TYPE_CHANNEL_ENUM,
    EVT_TYPE_CHANNEL_CONFIG,
    EVT_TYPE_RESULT_SET,
    EVT_TYPE_EVENT,
    EVT_TYPE_RENDER_CONTEXT,
    EVT_TYPE_BOOKMARK,
    EVT_TYPE_SUBSCRIPTION
};

struct evt_object
{
    DWORD magic;
    DWORD type;
};

struct evt_session
{
    struct evt_object hdr;
    WCHAR *server;               /* NULL = local */
};

struct evt_log
{
    struct evt_object hdr;
    HANDLE classic;
    WCHAR *path;
    BOOL is_file;
    BOOL remote;
};

struct evt_channel_enum
{
    struct evt_object hdr;
    HKEY root;                   /* remote registry, or NULL */
    HKEY key;
    DWORD index;
};

struct evt_channel_config
{
    struct evt_object hdr;
    WCHAR *name;
    WCHAR *keyname;
    DWORD max_size;
    DWORD retention;
    BOOL dirty;
};

/* query.c: parsed structured query (XPath subset) */
struct evt_filter;

BOOL evt_parse_query(const WCHAR *query, struct evt_filter **filter, WCHAR **channel);
BOOL evt_filter_match(const struct evt_filter *filter, const EVENTLOGRECORD *record, ULONGLONG now);
void evt_free_filter(struct evt_filter *filter);

/* One event: a private copy of the classic record */
struct evt_event
{
    struct evt_object hdr;
    EVENTLOGRECORD *record;
    WCHAR *channel;
};

struct evt_result_set
{
    struct evt_object hdr;
    CRITICAL_SECTION cs;
    HANDLE classic;
    WCHAR *channel;              /* channel name, or file path for a file query */
    struct evt_filter *filter;
    BOOL reverse;
    BOOL cancelled;
    BOOL started;
    DWORD next;                  /* record number read next */
    BYTE *buffer;                /* last ReadEventLog block */
    DWORD buffer_size;
    DWORD buffer_len;
    DWORD buffer_pos;
};

struct evt_render_context
{
    struct evt_object hdr;
    DWORD flags;
    DWORD count;
    WCHAR **paths;
};

struct evt_bookmark
{
    struct evt_object hdr;
    WCHAR *channel;
    ULONGLONG record_id;
    BOOL valid;
};

struct evt_subscription
{
    struct evt_object hdr;
    struct evt_result_set reader;      /* forward reader over the channel; reader.cs guards it */
    HANDLE signal;                     /* pull model */
    EVT_SUBSCRIBE_CALLBACK callback;   /* push model */
    void *context;
    HANDLE thread;
    DWORD thread_id;
    HANDLE stop;
    HANDLE change;                     /* NotifyChangeEventLog event, or NULL when polling */
    volatile LONG closing;
};

void *evt_alloc_object(DWORD type, SIZE_T size);
void *evt_get_object(EVT_HANDLE handle, DWORD type);
WCHAR *evt_strdupW(const WCHAR *src);
BOOL evt_channel_exists(const WCHAR *server, const WCHAR *channel);
HANDLE evt_open_classic(const WCHAR *server, const WCHAR *path, BOOL is_file);

/* query.c */
EVT_HANDLE evt_create_event(const EVENTLOGRECORD *record, const WCHAR *channel);
void evt_free_event(struct evt_event *event);
void evt_free_result_set(struct evt_result_set *set);
BOOL evt_log_range(HANDLE log, DWORD *oldest, DWORD *count);
const EVENTLOGRECORD *evt_reader_next(struct evt_result_set *set, ULONGLONG now);
ULONGLONG evt_now_filetime(void);

WCHAR *evt_xml_attr(const WCHAR *tag_start, const WCHAR *tag_end, const WCHAR *name);
BOOL evt_record_valid(const EVENTLOGRECORD *record, DWORD avail);
BYTE evt_record_level(const EVENTLOGRECORD *record);
ULONGLONG evt_record_keywords(const EVENTLOGRECORD *record);
ULONGLONG evt_record_filetime(const EVENTLOGRECORD *record);

/* render.c */
void evt_free_render_context(struct evt_render_context *context);

/* subscribe.c */
BOOL evt_subscription_next(struct evt_subscription *sub, DWORD size, EVT_HANDLE *events, DWORD *returned);
BOOL evt_close_subscription(struct evt_subscription *sub);
BOOL evt_cancel_subscription(struct evt_subscription *sub);

#define EVT_FILETIME_1970 ((ULONGLONG)116444736000000000)

#endif /* __WEVTAPI_PRIVATE_H */
