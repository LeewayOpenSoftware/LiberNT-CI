/*
 * PROJECT:     LiberNT wevtapi
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Rendering events (Event XML schema, value arrays) and bookmarks
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <stdio.h>

#include "wevtapi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wevtapi);

#define SEL_NULL     (-1)
#define SEL_BINARY   (EvtSystemPropertyIdEND + 1)
#define SEL_DATA     1000     /* SEL_DATA + n: the n-th insertion string, 0-based */
#define EVT_MAX_RENDER_VALUES 0x10000   /* keeps every size computation far from wrapping */

/* --------------------------------------------------------------------------
 * Record accessors (records are validated by evt_record_valid on read)
 * -------------------------------------------------------------------------- */

static const WCHAR *record_source(const EVENTLOGRECORD *r)
{
    return (const WCHAR *)(r + 1);
}

static const WCHAR *record_computer(const EVENTLOGRECORD *r)
{
    const WCHAR *s = record_source(r);
    return s + lstrlenW(s) + 1;
}

/* The n-th insertion string, or NULL when the record holds fewer */
static const WCHAR *record_string(const EVENTLOGRECORD *r, DWORD n)
{
    const WCHAR *p, *end = (const WCHAR *)((const BYTE *)r + r->Length);
    DWORD i;

    if (n >= r->NumStrings || r->StringOffset < sizeof(*r) || (r->StringOffset & 1))
        return NULL;
    p = (const WCHAR *)((const BYTE *)r + r->StringOffset);
    for (i = 0; ; i++)
    {
        const WCHAR *start = p;
        while (p < end && *p) p++;
        if (p >= end) return NULL;
        if (i == n) return start;
        p++;
    }
}

static PSID record_sid(const EVENTLOGRECORD *r)
{
    PSID sid;
    if (!r->UserSidLength || r->UserSidOffset < sizeof(*r))
        return NULL;
    sid = (PSID)((const BYTE *)r + r->UserSidOffset);
    if (!IsValidSid(sid) || GetLengthSid(sid) > r->UserSidLength)
        return NULL;
    return sid;
}

/* --------------------------------------------------------------------------
 * XML
 * -------------------------------------------------------------------------- */

struct strbuf
{
    WCHAR *buf;
    SIZE_T len, cap;
    BOOL error;
};

static void sb_append_n(struct strbuf *sb, const WCHAR *s, SIZE_T n)
{
    WCHAR *grown;
    SIZE_T cap;

    if (sb->error) return;
    if (sb->len + n + 1 > sb->cap)
    {
        cap = max(sb->cap * 2, sb->len + n + 256);
        if (!(grown = heap_realloc(sb->buf, cap * sizeof(WCHAR))))
        {
            sb->error = TRUE;
            return;
        }
        sb->buf = grown;
        sb->cap = cap;
    }
    memcpy(sb->buf + sb->len, s, n * sizeof(WCHAR));
    sb->len += n;
    sb->buf[sb->len] = 0;
}

static void sb_append(struct strbuf *sb, const WCHAR *s)
{
    sb_append_n(sb, s, lstrlenW(s));
}

/* Text escapes & < >; attribute values (always in single quotes) also ' and " */
static void sb_append_escaped(struct strbuf *sb, const WCHAR *s, BOOL attr)
{
    const WCHAR *start = s;

    for (; *s; s++)
    {
        const WCHAR *ent = NULL;
        switch (*s)
        {
        case '&': ent = L"&amp;"; break;
        case '<': ent = L"&lt;"; break;
        case '>': ent = L"&gt;"; break;
        case '\'': if (attr) ent = L"&apos;"; break;
        case '"': if (attr) ent = L"&quot;"; break;
        }
        if (ent)
        {
            sb_append_n(sb, start, s - start);
            sb_append(sb, ent);
            start = s + 1;
        }
    }
    sb_append_n(sb, start, s - start);
}

static void sb_printf(struct strbuf *sb, const WCHAR *fmt, ...)
{
    WCHAR tmp[256];
    va_list args;
    int n;

    va_start(args, fmt);
    n = _vsnwprintf(tmp, ARRAY_SIZE(tmp) - 1, fmt, args);
    va_end(args);
    if (n < 0 || n >= (int)ARRAY_SIZE(tmp) - 1)
    {
        sb->error = TRUE;   /* never emit a truncated fragment */
        return;
    }
    tmp[n] = 0;
    sb_append(sb, tmp);
}

static void format_systemtime(ULONGLONG filetime, WCHAR *out, SIZE_T count)
{
    FILETIME ft;
    SYSTEMTIME st;

    ft.dwLowDateTime = (DWORD)filetime;
    ft.dwHighDateTime = (DWORD)(filetime >> 32);
    if (!FileTimeToSystemTime(&ft, &st))
        memset(&st, 0, sizeof(st));
    _snwprintf(out, count - 1, L"%04u-%02u-%02uT%02u:%02u:%02u.%07luZ", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, (ULONG)(filetime % 10000000));
    out[count - 1] = 0;
}

static WCHAR *event_xml(const struct evt_event *event)
{
    const EVENTLOGRECORD *r = event->record;
    struct strbuf sb = { 0 };
    WCHAR time[40], *sidstr = NULL;
    PSID sid;
    DWORD i;

    sb_append(&sb, L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='");
    sb_append_escaped(&sb, record_source(r), TRUE);
    sb_printf(&sb, L"'/><EventID Qualifiers='%u'>%u</EventID>", (r->EventID >> 16) & 0xFFFF, r->EventID & 0xFFFF);
    sb_printf(&sb, L"<Version>0</Version><Level>%u</Level><Task>%u</Task><Opcode>0</Opcode>", evt_record_level(r),
              r->EventCategory);
    sb_printf(&sb, L"<Keywords>0x%I64x</Keywords>", evt_record_keywords(r));
    format_systemtime(evt_record_filetime(r), time, ARRAY_SIZE(time));
    sb_append(&sb, L"<TimeCreated SystemTime='");
    sb_append(&sb, time);
    sb_printf(&sb, L"'/><EventRecordID>%lu</EventRecordID><Correlation/><Channel>", r->RecordNumber);
    sb_append_escaped(&sb, event->channel, FALSE);
    sb_append(&sb, L"</Channel><Computer>");
    sb_append_escaped(&sb, record_computer(r), FALSE);
    sb_append(&sb, L"</Computer>");
    if ((sid = record_sid(r)) && ConvertSidToStringSidW(sid, &sidstr))
    {
        sb_append(&sb, L"<Security UserID='");
        sb_append(&sb, sidstr);
        sb_append(&sb, L"'/>");
        LocalFree(sidstr);
    }
    else
        sb_append(&sb, L"<Security/>");
    sb_append(&sb, L"</System><EventData>");
    for (i = 0; i < r->NumStrings; i++)
    {
        const WCHAR *s = record_string(r, i);
        if (!s) break;
        sb_append(&sb, L"<Data>");
        sb_append_escaped(&sb, s, FALSE);
        sb_append(&sb, L"</Data>");
    }
    if (r->DataLength && r->DataOffset >= sizeof(*r))
    {
        const BYTE *data = (const BYTE *)r + r->DataOffset;
        sb_append(&sb, L"<Binary>");
        for (i = 0; i < r->DataLength; i++)
            sb_printf(&sb, L"%02X", data[i]);
        sb_append(&sb, L"</Binary>");
    }
    sb_append(&sb, L"</EventData></Event>");
    if (sb.error)
    {
        heap_free(sb.buf);
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }
    return sb.buf;
}

static WCHAR *bookmark_xml(const struct evt_bookmark *bm)
{
    struct strbuf sb = { 0 };

    if (!bm->valid)
        sb_append(&sb, L"<BookmarkList>\r\n</BookmarkList>");
    else
    {
        sb_append(&sb, L"<BookmarkList>\r\n  <Bookmark Channel='");
        sb_append_escaped(&sb, bm->channel, TRUE);
        sb_printf(&sb, L"' RecordId='%I64u' IsCurrent='true'/>\r\n</BookmarkList>", bm->record_id);
    }
    if (sb.error)
    {
        heap_free(sb.buf);
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }
    return sb.buf;
}

/* --------------------------------------------------------------------------
 * Render contexts
 * -------------------------------------------------------------------------- */

static int path_selector(const WCHAR *path)
{
    static const struct { const WCHAR *path; int sel; } map[] =
    {
        { L"System/Provider/@Name",          EvtSystemProviderName },
        { L"System/Provider/@Guid",          EvtSystemProviderGuid },
        { L"System/EventID",                 EvtSystemEventID },
        { L"System/EventID/@Qualifiers",     EvtSystemQualifiers },
        { L"System/Level",                   EvtSystemLevel },
        { L"System/Task",                    EvtSystemTask },
        { L"System/Opcode",                  EvtSystemOpcode },
        { L"System/Keywords",                EvtSystemKeywords },
        { L"System/TimeCreated/@SystemTime", EvtSystemTimeCreated },
        { L"System/EventRecordID",           EvtSystemEventRecordId },
        { L"System/Channel",                 EvtSystemChannel },
        { L"System/Computer",                EvtSystemComputer },
        { L"System/Security/@UserID",        EvtSystemUserID },
        { L"System/Version",                 EvtSystemVersion },
        { L"EventData/Binary",               SEL_BINARY },
    };
    unsigned int i;

    while (*path == ' ') path++;
    if (!wcsncmp(path, L"Event/", 6)) path += 6;
    for (i = 0; i < ARRAY_SIZE(map); i++)
        if (!wcscmp(path, map[i].path)) return map[i].sel;
    if (!wcsncmp(path, L"EventData/Data", 14))
    {
        const WCHAR *p = path + 14;
        unsigned long n = 1;
        if (*p == '[')
        {
            WCHAR *end;
            n = wcstoul(p + 1, &end, 10);
            if (!n || n > 0xFFFF || end[0] != ']' || end[1]) return SEL_NULL;
        }
        else if (*p)
            return SEL_NULL;
        return SEL_DATA + (int)(n - 1);
    }
    return SEL_NULL;
}

void evt_free_render_context(struct evt_render_context *context)
{
    DWORD i;
    if (context->paths)
        for (i = 0; i < context->count; i++) heap_free(context->paths[i]);
    heap_free(context->paths);
}

EVT_HANDLE WINAPI EvtCreateRenderContext(DWORD count, const WCHAR **paths, DWORD flags)
{
    struct evt_render_context *context;
    DWORD i;

    TRACE("(%lu %p %#lx)\n", count, paths, flags);

    if (flags > EvtRenderContextUser || count > EVT_MAX_RENDER_VALUES ||
        (flags == EvtRenderContextValues && (!count || !paths)) ||
        (flags != EvtRenderContextValues && (count || paths)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!(context = evt_alloc_object(EVT_TYPE_RENDER_CONTEXT, sizeof(*context))))
        return NULL;
    context->flags = flags;
    if (flags == EvtRenderContextValues)
    {
        if (!(context->paths = heap_alloc_zero(count * sizeof(WCHAR *))))
        {
            heap_free(context);
            SetLastError(ERROR_OUTOFMEMORY);
            return NULL;
        }
        context->count = count;
        for (i = 0; i < count; i++)
        {
            if (!paths[i] || !(context->paths[i] = evt_strdupW(paths[i])))
            {
                DWORD err = paths[i] ? ERROR_OUTOFMEMORY : ERROR_INVALID_PARAMETER;
                evt_free_render_context(context);
                heap_free(context);
                SetLastError(err);
                return NULL;
            }
        }
    }
    return context;
}

#define ALIGN8(x) (((x) + 7) & ~(SIZE_T)7)

/* Lays out one value. With out == NULL it only measures the extra bytes. */
static SIZE_T render_value(const struct evt_event *event, int sel, EVT_VARIANT *out, BYTE *extra)
{
    const EVENTLOGRECORD *r = event->record;
    const WCHAR *s = NULL;
    PSID sid;
    SIZE_T len;

    if (out)
    {
        memset(out, 0, sizeof(*out));
        out->Type = EvtVarTypeNull;
    }
    switch (sel)
    {
    case EvtSystemProviderName: s = record_source(r); break;
    case EvtSystemChannel:      s = event->channel; break;
    case EvtSystemComputer:     s = record_computer(r); break;
    case EvtSystemOpcode:
    case EvtSystemVersion:
        /* Classic events carry neither: Windows reports 0 for both */
        if (out) { out->Type = EvtVarTypeByte; out->ByteVal = 0; }
        return 0;
    case EvtSystemEventID:
        if (out) { out->Type = EvtVarTypeUInt16; out->UInt16Val = r->EventID & 0xFFFF; }
        return 0;
    case EvtSystemQualifiers:
        if (out) { out->Type = EvtVarTypeUInt16; out->UInt16Val = (r->EventID >> 16) & 0xFFFF; }
        return 0;
    case EvtSystemLevel:
        if (out) { out->Type = EvtVarTypeByte; out->ByteVal = evt_record_level(r); }
        return 0;
    case EvtSystemTask:
        if (out) { out->Type = EvtVarTypeUInt16; out->UInt16Val = r->EventCategory; }
        return 0;
    case EvtSystemKeywords:
        if (out) { out->Type = EvtVarTypeHexInt64; out->UInt64Val = evt_record_keywords(r); }
        return 0;
    case EvtSystemTimeCreated:
        if (out) { out->Type = EvtVarTypeFileTime; out->FileTimeVal = evt_record_filetime(r); }
        return 0;
    case EvtSystemEventRecordId:
        if (out) { out->Type = EvtVarTypeUInt64; out->UInt64Val = r->RecordNumber; }
        return 0;
    case EvtSystemUserID:
        if (!(sid = record_sid(r))) return 0;
        len = GetLengthSid(sid);
        if (out)
        {
            memcpy(extra, sid, len);
            out->Type = EvtVarTypeSid;
            out->SidVal = extra;
        }
        return ALIGN8(len);
    case SEL_BINARY:
        if (!r->DataLength || r->DataOffset < sizeof(*r)) return 0;
        if (out)
        {
            memcpy(extra, (const BYTE *)r + r->DataOffset, r->DataLength);
            out->Type = EvtVarTypeBinary;
            out->BinaryVal = extra;
            out->Count = r->DataLength;
        }
        return ALIGN8(r->DataLength);
    default:
        if (sel >= SEL_DATA)
            s = record_string(r, sel - SEL_DATA);
        break;
    }
    if (!s) return 0;
    len = (lstrlenW(s) + 1) * sizeof(WCHAR);
    if (out)
    {
        memcpy(extra, s, len);
        out->Type = EvtVarTypeString;
        out->StringVal = (const WCHAR *)extra;
        out->Count = lstrlenW(s);   /* Windows reports the length in characters */
    }
    return ALIGN8(len);
}

static BOOL render_values(const struct evt_render_context *context, const struct evt_event *event,
                          DWORD size, void *buffer, DWORD *used, DWORD *count)
{
    int sels_static[EvtSystemPropertyIdEND], *sels = sels_static;
    DWORD n, i;
    SIZE_T total;
    BYTE *extra;
    BOOL ret = FALSE;

    switch (context->flags)
    {
    case EvtRenderContextSystem:
        n = EvtSystemPropertyIdEND;
        for (i = 0; i < n; i++) sels[i] = i;
        break;
    case EvtRenderContextUser:
        n = event->record->NumStrings;   /* bounded by the record size (evt_record_valid) */
        if (n > EVT_MAX_RENDER_VALUES)
        {
            SetLastError(ERROR_INVALID_DATA);
            return FALSE;
        }
        if (n > EvtSystemPropertyIdEND && !(sels = heap_alloc((SIZE_T)n * sizeof(int))))
        {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        for (i = 0; i < n; i++) sels[i] = SEL_DATA + i;
        break;
    default:
        n = context->count;   /* bounded by EvtCreateRenderContext */
        if (n > EvtSystemPropertyIdEND && !(sels = heap_alloc((SIZE_T)n * sizeof(int))))
        {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        for (i = 0; i < n; i++) sels[i] = path_selector(context->paths[i]);
        break;
    }

    /* n is bounded below, so the array size cannot wrap; every later sum is checked */
    total = ALIGN8((SIZE_T)n * sizeof(EVT_VARIANT));
    for (i = 0; i < n; i++)
    {
        SIZE_T extra_len = render_value(event, sels[i], NULL, NULL);
        if (extra_len > MAXDWORD - total)
        {
            SetLastError(ERROR_INVALID_DATA);
            goto done;
        }
        total += extra_len;
    }
    *used = (DWORD)total;
    *count = n;
    if (size < total || !buffer)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        goto done;
    }
    extra = (BYTE *)buffer + ALIGN8((SIZE_T)n * sizeof(EVT_VARIANT));
    for (i = 0; i < n; i++)
        extra += render_value(event, sels[i], (EVT_VARIANT *)buffer + i, extra);
    ret = TRUE;
done:
    if (sels != sels_static) heap_free(sels);
    return ret;
}

static BOOL return_string(WCHAR *str, DWORD size, void *buffer, DWORD *used, DWORD *count)
{
    SIZE_T bytes;

    if (!str) return FALSE;
    bytes = (lstrlenW(str) + 1) * sizeof(WCHAR);
    *used = (DWORD)bytes;
    *count = 1;   /* one fragment: Windows reports 1 */
    if (size < bytes || !buffer)
    {
        heap_free(str);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(buffer, str, bytes);
    heap_free(str);
    return TRUE;
}

BOOL WINAPI EvtRender(EVT_HANDLE context, EVT_HANDLE fragment, DWORD flags, DWORD buffer_size, void *buffer,
                      DWORD *buffer_used, DWORD *property_count)
{
    struct evt_render_context *ctx;
    struct evt_event *event;
    struct evt_bookmark *bm;

    TRACE("(%p %p %#lx %lu %p %p %p)\n", context, fragment, flags, buffer_size, buffer, buffer_used, property_count);

    if (!buffer_used || !property_count || (buffer_size && !buffer))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    switch (flags)
    {
    case EvtRenderEventXml:
        if (!(event = evt_get_object(fragment, EVT_TYPE_EVENT)))
            return FALSE;
        return return_string(event_xml(event), buffer_size, buffer, buffer_used, property_count);
    case EvtRenderBookmark:
        if (!(bm = evt_get_object(fragment, EVT_TYPE_BOOKMARK)))
            return FALSE;
        return return_string(bookmark_xml(bm), buffer_size, buffer, buffer_used, property_count);
    case EvtRenderEventValues:
        if (!(ctx = evt_get_object(context, EVT_TYPE_RENDER_CONTEXT)) ||
            !(event = evt_get_object(fragment, EVT_TYPE_EVENT)))
            return FALSE;
        return render_values(ctx, event, buffer_size, buffer, buffer_used, property_count);
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
}

/* --------------------------------------------------------------------------
 * Bookmarks
 * -------------------------------------------------------------------------- */

EVT_HANDLE WINAPI EvtCreateBookmark(const WCHAR *xml)
{
    struct evt_bookmark *bm;
    const WCHAR *tag, *tag_end;
    WCHAR *channel = NULL, *recid = NULL, *end;

    TRACE("(%s)\n", debugstr_w(xml));

    if (xml)
    {
        tag = xml;
        while ((tag = wcsstr(tag, L"<Bookmark")) && tag[9] != ' ' && tag[9] != '\t' && tag[9] != '\r' &&
               tag[9] != '\n')
            tag += 9;
        if (!wcsstr(xml, L"<BookmarkList") || !tag || !(tag_end = wcschr(tag, '>')) ||
            !(channel = evt_xml_attr(tag, tag_end, L"Channel")) || !(recid = evt_xml_attr(tag, tag_end, L"RecordId")) ||
            !*recid || (wcstoul(recid, &end, 10), *end))
        {
            heap_free(channel);
            heap_free(recid);
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }
    }
    if (!(bm = evt_alloc_object(EVT_TYPE_BOOKMARK, sizeof(*bm))))
    {
        heap_free(channel);
        heap_free(recid);
        return NULL;
    }
    if (channel)
    {
        bm->channel = channel;
        bm->record_id = _wcstoui64(recid, NULL, 10);
        bm->valid = TRUE;
        heap_free(recid);
    }
    return bm;
}

BOOL WINAPI EvtUpdateBookmark(EVT_HANDLE bookmark, EVT_HANDLE event_handle)
{
    struct evt_bookmark *bm;
    struct evt_event *event;
    WCHAR *channel;

    TRACE("(%p %p)\n", bookmark, event_handle);

    if (!(bm = evt_get_object(bookmark, EVT_TYPE_BOOKMARK)) || !(event = evt_get_object(event_handle, EVT_TYPE_EVENT)))
        return FALSE;
    if (!(channel = evt_strdupW(event->channel)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    heap_free(bm->channel);
    bm->channel = channel;
    bm->record_id = event->record->RecordNumber;
    bm->valid = TRUE;
    return TRUE;
}
