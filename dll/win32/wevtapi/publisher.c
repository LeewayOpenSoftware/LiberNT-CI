/*
 * PROJECT:     LiberNT wevtapi
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Publisher metadata and message formatting for classic event sources
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <stdio.h>
#include <wchar.h>

#include "wevtapi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wevtapi);

static const WCHAR eventlog_keyW[] = L"SYSTEM\\CurrentControlSet\\Services\\EventLog";

#define KEYWORD_CLASSIC         0x0080000000000000ULL
#define KEYWORD_AUDIT_SUCCESS   0x0020000000000000ULL
#define KEYWORD_AUDIT_FAILURE   0x0010000000000000ULL

struct msgbuf
{
    WCHAR *buf;
    SIZE_T len, cap;
    BOOL error;
};

static void mb_append_n(struct msgbuf *mb, const WCHAR *s, SIZE_T n)
{
    WCHAR *grown;
    SIZE_T cap;

    if (mb->error) return;
    if (mb->len + n + 1 > mb->cap)
    {
        cap = max(mb->cap * 2, mb->len + n + 256);
        if (!(grown = heap_realloc(mb->buf, cap * sizeof(WCHAR))))
        {
            mb->error = TRUE;
            return;
        }
        mb->buf = grown;
        mb->cap = cap;
    }
    memcpy(mb->buf + mb->len, s, n * sizeof(WCHAR));
    mb->len += n;
    mb->buf[mb->len] = 0;
}

static void mb_append(struct msgbuf *mb, const WCHAR *s)
{
    mb_append_n(mb, s, lstrlenW(s));
}

static void mb_append_escaped(struct msgbuf *mb, const WCHAR *s)
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
        }
        if (!ent) continue;
        mb_append_n(mb, start, s - start);
        mb_append(mb, ent);
        start = s + 1;
    }
    mb_append_n(mb, start, s - start);
}

static WCHAR *mb_finish(struct msgbuf *mb)
{
    if (mb->error)
    {
        heap_free(mb->buf);
        return NULL;
    }
    if (!mb->buf) mb_append_n(mb, L"", 0);
    return mb->error ? NULL : mb->buf;
}

static void trim_line_ends(WCHAR *s)
{
    SIZE_T len = lstrlenW(s);
    while (len && (s[len - 1] == '\r' || s[len - 1] == '\n')) s[--len] = 0;
}

static BOOL source_key_name(const WCHAR *channel, const WCHAR *source, WCHAR *keyname, SIZE_T count)
{
    if (!channel || !source || !*source || wcschr(source, '\\') || wcschr(source, '/') ||
        lstrlenW(channel) + lstrlenW(source) + ARRAY_SIZE(eventlog_keyW) + 2 > count)
        return FALSE;
    lstrcpyW(keyname, eventlog_keyW);
    lstrcatW(keyname, L"\\");
    lstrcatW(keyname, channel);
    lstrcatW(keyname, L"\\");
    lstrcatW(keyname, source);
    return TRUE;
}

static WCHAR *find_source_channel(const WCHAR *source)
{
    HKEY key, sub;
    WCHAR log[256], keyname[512], *channel = NULL;
    DWORD i, len;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, eventlog_keyW, 0, KEY_READ, &key))
        return NULL;
    for (i = 0; !channel; i++)
    {
        len = ARRAY_SIZE(log);
        if (RegEnumKeyExW(key, i, log, &len, NULL, NULL, NULL, NULL))
            break;
        if (!source_key_name(log, source, keyname, ARRAY_SIZE(keyname)))
            continue;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyname, 0, KEY_READ, &sub))
            continue;
        RegCloseKey(sub);
        channel = evt_strdupW(log);
    }
    RegCloseKey(key);
    return channel;
}

static WCHAR *read_string_value(const WCHAR *keyname, const WCHAR *value)
{
    DWORD size = 0;
    WCHAR *data;

    if (RegGetValueW(HKEY_LOCAL_MACHINE, keyname, value, RRF_RT_REG_SZ, NULL, NULL, &size))
        return NULL;
    size += 1024 * sizeof(WCHAR);
    if (!(data = heap_alloc(size + sizeof(WCHAR))))
        return NULL;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, keyname, value, RRF_RT_REG_SZ, NULL, data, &size))
    {
        heap_free(data);
        return NULL;
    }
    return data;
}

static WCHAR *read_source_value(const struct evt_publisher *pub, const WCHAR *value)
{
    WCHAR keyname[512];

    if (!source_key_name(pub->channel, pub->name, keyname, ARRAY_SIZE(keyname)))
        return NULL;
    return read_string_value(keyname, value);
}

static WCHAR *load_template(WCHAR *files, DWORD id, LANGID lang)
{
    WCHAR *path, *next, *text = NULL;
    HMODULE module;

    for (path = files; path && *path && !text; path = next)
    {
        next = wcschr(path, ';');
        if (next) *next++ = 0;
        while (*path == ' ') path++;
        if (!*path) continue;
        if (!(module = LoadLibraryExW(path, NULL, LOAD_LIBRARY_AS_DATAFILE)))
            continue;
        if (!FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                            module, id, lang, (WCHAR *)&text, 0, NULL) && lang)
        {
            text = NULL;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                           module, id, 0, (WCHAR *)&text, 0, NULL);
        }
        FreeLibrary(module);
    }
    return text;
}

static WCHAR *load_source_message(const struct evt_publisher *pub, const WCHAR *value, DWORD id, LANGID lang,
                                  DWORD *status)
{
    WCHAR *files, *text;

    if (!(files = read_source_value(pub, value)))
    {
        *status = ERROR_EVT_MESSAGE_NOT_FOUND;
        return NULL;
    }
    text = load_template(files, id, lang);
    heap_free(files);
    if (!text)
        *status = ERROR_EVT_MESSAGE_LOCALE_NOT_FOUND;
    return text;
}

static WCHAR *expand_parameters(const WCHAR *text, const struct evt_publisher *pub, LANGID lang, DWORD *status)
{
    struct msgbuf mb = { 0 };
    const WCHAR *p = text;
    WCHAR *end, *param;
    DWORD id, ignored;

    while (*p)
    {
        if (p[0] != '%' || p[1] != '%' || !iswdigit(p[2]))
        {
            mb_append_n(&mb, p, 1);
            p++;
            continue;
        }
        id = wcstoul(p + 2, &end, 10);
        if ((param = load_source_message(pub, L"ParameterMessageFile", id, lang, &ignored)))
        {
            trim_line_ends(param);
            mb_append(&mb, param);
            LocalFree(param);
        }
        else
        {
            *status = ERROR_EVT_UNRESOLVED_PARAMETER_INSERT;
            mb_append_n(&mb, p, end - p);
        }
        p = end;
    }
    return mb_finish(&mb);
}

/* Expands %N, %n, %r, %t, %0 and %% ourselves: FormatMessage with an argument
 * array reads past it when the template names more inserts than there are. */
static WCHAR *expand_template(const WCHAR *tmpl, const WCHAR **args, DWORD count, const struct evt_publisher *pub,
                              LANGID lang, DWORD *status)
{
    struct msgbuf mb = { 0 };
    const WCHAR *p = tmpl, *q;
    WCHAR *expanded, *result;
    DWORD n;
    BOOL params = FALSE;

    *status = ERROR_SUCCESS;
    while (*p)
    {
        if (*p != '%')
        {
            mb_append_n(&mb, p, 1);
            p++;
            continue;
        }
        if (iswdigit(p[1]))
        {
            q = p + 1;
            n = *q++ - '0';
            if (iswdigit(*q)) n = n * 10 + (*q++ - '0');
            if (*q == '!')
            {
                const WCHAR *close = wcschr(q + 1, '!');
                if (close) q = close + 1;
            }
            if (n >= 1 && n <= count && args[n - 1])
            {
                mb_append(&mb, args[n - 1]);
                if (wcsstr(args[n - 1], L"%%")) params = TRUE;
            }
            else
            {
                *status = ERROR_EVT_UNRESOLVED_VALUE_INSERT;
                mb_append_n(&mb, p, q - p);
            }
            p = q;
            continue;
        }
        switch (p[1])
        {
        case 'n': mb_append(&mb, L"\r\n"); break;
        case 'r': mb_append(&mb, L"\r"); break;
        case 't': mb_append(&mb, L"\t"); break;
        case '0': goto done;
        case '%':
            mb_append(&mb, L"%");
            if (iswdigit(p[2]))
            {
                mb_append(&mb, L"%");
                params = TRUE;
            }
            break;
        case 0:
            mb_append(&mb, L"%");
            p++;
            continue;
        default:
            mb_append_n(&mb, p, 2);
            break;
        }
        p += 2;
    }
done:
    if (!(expanded = mb_finish(&mb)))
        return NULL;
    if (!params)
        return expanded;
    result = expand_parameters(expanded, pub, lang, status);
    heap_free(expanded);
    return result;
}

static WCHAR *variant_to_string(const EVT_VARIANT *v)
{
    WCHAR buf[64], *sid = NULL, *result;
    SYSTEMTIME st;
    FILETIME ft;
    int len;

    switch (v->Type)
    {
    case EvtVarTypeString:
        return evt_strdupW(v->StringVal ? v->StringVal : L"");
    case EvtVarTypeAnsiString:
        if (!v->AnsiStringVal) return evt_strdupW(L"");
        len = MultiByteToWideChar(CP_ACP, 0, v->AnsiStringVal, -1, NULL, 0);
        if (!len || !(result = heap_alloc(len * sizeof(WCHAR)))) return NULL;
        MultiByteToWideChar(CP_ACP, 0, v->AnsiStringVal, -1, result, len);
        return result;
    case EvtVarTypeSByte:   _snwprintf(buf, ARRAY_SIZE(buf), L"%d", v->SByteVal); break;
    case EvtVarTypeByte:    _snwprintf(buf, ARRAY_SIZE(buf), L"%u", v->ByteVal); break;
    case EvtVarTypeInt16:   _snwprintf(buf, ARRAY_SIZE(buf), L"%d", v->Int16Val); break;
    case EvtVarTypeUInt16:  _snwprintf(buf, ARRAY_SIZE(buf), L"%u", v->UInt16Val); break;
    case EvtVarTypeInt32:   _snwprintf(buf, ARRAY_SIZE(buf), L"%d", v->Int32Val); break;
    case EvtVarTypeUInt32:  _snwprintf(buf, ARRAY_SIZE(buf), L"%u", v->UInt32Val); break;
    case EvtVarTypeInt64:   _snwprintf(buf, ARRAY_SIZE(buf), L"%I64d", v->Int64Val); break;
    case EvtVarTypeUInt64:  _snwprintf(buf, ARRAY_SIZE(buf), L"%I64u", v->UInt64Val); break;
    case EvtVarTypeSizeT:   _snwprintf(buf, ARRAY_SIZE(buf), L"%I64u", (ULONGLONG)v->SizeTVal); break;
    case EvtVarTypeHexInt32: _snwprintf(buf, ARRAY_SIZE(buf), L"0x%X", v->UInt32Val); break;
    case EvtVarTypeHexInt64: _snwprintf(buf, ARRAY_SIZE(buf), L"0x%I64X", v->UInt64Val); break;
    case EvtVarTypeSingle:  _snwprintf(buf, ARRAY_SIZE(buf), L"%g", v->SingleVal); break;
    case EvtVarTypeDouble:  _snwprintf(buf, ARRAY_SIZE(buf), L"%g", v->DoubleVal); break;
    case EvtVarTypeBoolean: return evt_strdupW(v->BooleanVal ? L"true" : L"false");
    case EvtVarTypeGuid:
        if (!v->GuidVal) return evt_strdupW(L"");
        _snwprintf(buf, ARRAY_SIZE(buf), L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                   v->GuidVal->Data1, v->GuidVal->Data2, v->GuidVal->Data3,
                   v->GuidVal->Data4[0], v->GuidVal->Data4[1], v->GuidVal->Data4[2], v->GuidVal->Data4[3],
                   v->GuidVal->Data4[4], v->GuidVal->Data4[5], v->GuidVal->Data4[6], v->GuidVal->Data4[7]);
        break;
    case EvtVarTypeFileTime:
        ft.dwLowDateTime = (DWORD)v->FileTimeVal;
        ft.dwHighDateTime = (DWORD)(v->FileTimeVal >> 32);
        if (!FileTimeToSystemTime(&ft, &st)) return evt_strdupW(L"");
        _snwprintf(buf, ARRAY_SIZE(buf), L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        break;
    case EvtVarTypeSysTime:
        if (!v->SysTimeVal) return evt_strdupW(L"");
        _snwprintf(buf, ARRAY_SIZE(buf), L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                   v->SysTimeVal->wYear, v->SysTimeVal->wMonth, v->SysTimeVal->wDay, v->SysTimeVal->wHour,
                   v->SysTimeVal->wMinute, v->SysTimeVal->wSecond, v->SysTimeVal->wMilliseconds);
        break;
    case EvtVarTypeSid:
        if (!v->SidVal || !ConvertSidToStringSidW(v->SidVal, &sid)) return evt_strdupW(L"");
        result = evt_strdupW(sid);
        LocalFree(sid);
        return result;
    default:
        return evt_strdupW(L"");
    }
    buf[ARRAY_SIZE(buf) - 1] = 0;
    return evt_strdupW(buf);
}

static void free_args(WCHAR **args, DWORD count)
{
    DWORD i;
    if (!args) return;
    for (i = 0; i < count; i++) heap_free(args[i]);
    heap_free(args);
}

static WCHAR **collect_args(const struct evt_event *event, const EVT_VARIANT *values, DWORD count, DWORD *out_count)
{
    WCHAR **args;
    DWORD i, n;

    if (values && count)
        n = count;
    else if (event)
        n = event->record->NumStrings;
    else
        n = 0;
    *out_count = n;
    if (!n) return NULL;
    if (!(args = heap_alloc_zero(n * sizeof(*args))))
    {
        *out_count = 0;
        return NULL;
    }
    for (i = 0; i < n; i++)
    {
        if (values && count)
            args[i] = variant_to_string(&values[i]);
        else
        {
            const WCHAR *s = evt_record_string_at(event->record, i);
            args[i] = evt_strdupW(s ? s : L"");
        }
        if (!args[i])
        {
            free_args(args, n);
            *out_count = 0;
            return NULL;
        }
    }
    return args;
}

static LANGID publisher_langid(const struct evt_publisher *pub)
{
    return pub->locale ? LANGIDFROMLCID(pub->locale) : 0;
}

static WCHAR *format_message_id(const struct evt_publisher *pub, const struct evt_event *event, DWORD id,
                                const EVT_VARIANT *values, DWORD count, DWORD *status)
{
    WCHAR *tmpl, **args, *text;
    DWORD n;

    if (!(tmpl = load_source_message(pub, L"EventMessageFile", id, publisher_langid(pub), status)))
        return NULL;
    args = collect_args(event, values, count, &n);
    text = expand_template(tmpl, (const WCHAR **)args, n, pub, publisher_langid(pub), status);
    free_args(args, n);
    LocalFree(tmpl);
    if (!text)
    {
        *status = ERROR_OUTOFMEMORY;
        return NULL;
    }
    trim_line_ends(text);
    return text;
}

static const WCHAR *level_name(BYTE level)
{
    switch (level)
    {
    case 1: return L"Critical";
    case 2: return L"Error";
    case 3: return L"Warning";
    case 4: return L"Information";
    case 5: return L"Verbose";
    /* FIXME: level 0 (classic audit records) is shown as Information; winmeta names it Log Always */
    case 0: return L"Information";
    }
    return NULL;
}

static WCHAR *format_level(const struct evt_event *event, DWORD *status)
{
    const WCHAR *name = level_name(evt_record_level(event->record));
    if (!name)
    {
        *status = ERROR_EVT_MESSAGE_ID_NOT_FOUND;
        return NULL;
    }
    return evt_strdupW(name);
}

static WCHAR *format_task(const struct evt_publisher *pub, const struct evt_event *event, DWORD *status)
{
    WCHAR *tmpl, *text;
    DWORD ignored;

    if (!event->record->EventCategory ||
        !(tmpl = load_source_message(pub, L"CategoryMessageFile", event->record->EventCategory,
                                     publisher_langid(pub), &ignored)))
    {
        *status = ERROR_EVT_MESSAGE_ID_NOT_FOUND;
        return NULL;
    }
    text = expand_template(tmpl, NULL, 0, pub, publisher_langid(pub), &ignored);
    LocalFree(tmpl);
    if (!text)
    {
        *status = ERROR_OUTOFMEMORY;
        return NULL;
    }
    trim_line_ends(text);
    return text;
}

static WCHAR *format_keywords(const struct evt_event *event, SIZE_T *len, DWORD *status)
{
    static const struct { ULONGLONG mask; const WCHAR *name; } names[] =
    {
        { KEYWORD_AUDIT_FAILURE, L"Audit Failure" },
        { KEYWORD_AUDIT_SUCCESS, L"Audit Success" },
        { KEYWORD_CLASSIC, L"Classic" },
    };
    ULONGLONG keywords = evt_record_keywords(event->record);
    struct msgbuf mb = { 0 };
    WCHAR *text;
    SIZE_T i;

    for (i = 0; i < ARRAY_SIZE(names); i++)
    {
        if (!(keywords & names[i].mask)) continue;
        mb_append_n(&mb, names[i].name, lstrlenW(names[i].name) + 1);
    }
    if (!mb.len)
    {
        heap_free(mb.buf);
        *status = ERROR_EVT_MESSAGE_ID_NOT_FOUND;
        return NULL;
    }
    *len = mb.len;
    if (!(text = mb_finish(&mb)))
        *status = ERROR_OUTOFMEMORY;
    return text;
}

static WCHAR *format_channel(const struct evt_publisher *pub, const struct evt_event *event, DWORD *status)
{
    WCHAR keyname[512], *files, *tmpl, *text;
    DWORD id, size = sizeof(id), ignored;

    if (lstrlenW(event->channel) + ARRAY_SIZE(eventlog_keyW) + 1 > ARRAY_SIZE(keyname))
    {
        *status = ERROR_EVT_MESSAGE_ID_NOT_FOUND;
        return NULL;
    }
    lstrcpyW(keyname, eventlog_keyW);
    lstrcatW(keyname, L"\\");
    lstrcatW(keyname, event->channel);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, keyname, L"DisplayNameID", RRF_RT_REG_DWORD, NULL, &id, &size) ||
        !(files = read_string_value(keyname, L"DisplayNameFile")))
    {
        *status = ERROR_EVT_MESSAGE_ID_NOT_FOUND;
        return NULL;
    }
    tmpl = load_template(files, id, publisher_langid(pub));
    heap_free(files);
    if (!tmpl)
    {
        *status = ERROR_EVT_MESSAGE_ID_NOT_FOUND;
        return NULL;
    }
    text = expand_template(tmpl, NULL, 0, pub, publisher_langid(pub), &ignored);
    LocalFree(tmpl);
    if (!text)
    {
        *status = ERROR_OUTOFMEMORY;
        return NULL;
    }
    trim_line_ends(text);
    return text;
}

static WCHAR *format_provider(const struct evt_publisher *pub, const struct evt_event *event, DWORD *status)
{
    const WCHAR *name = pub->name ? pub->name : evt_record_source_name(event->record);
    WCHAR *text = evt_strdupW(name);
    if (!text) *status = ERROR_OUTOFMEMORY;
    return text;
}

static void culture_name(const struct evt_publisher *pub, WCHAR *name, int count)
{
    LCID lcid = pub->locale ? pub->locale : LOCALE_USER_DEFAULT;
    WCHAR lang[16], country[16];

    if (GetLocaleInfoW(lcid, LOCALE_SISO639LANGNAME, lang, ARRAY_SIZE(lang)) &&
        GetLocaleInfoW(lcid, LOCALE_SISO3166CTRYNAME, country, ARRAY_SIZE(country)) &&
        lstrlenW(lang) + lstrlenW(country) + 2 <= count)
    {
        lstrcpyW(name, lang);
        lstrcatW(name, L"-");
        lstrcatW(name, country);
        return;
    }
    lstrcpynW(name, L"en-US", count);
}

static WCHAR *format_xml(const struct evt_publisher *pub, const struct evt_event *event, DWORD *status)
{
    struct msgbuf mb = { 0 };
    WCHAR *xml, *end = NULL, *p, *part, culture[32];
    SIZE_T len;
    DWORD ignored;

    if (!(xml = evt_event_xml(event)))
    {
        *status = ERROR_OUTOFMEMORY;
        return NULL;
    }
    for (p = wcsstr(xml, L"</Event>"); p; p = wcsstr(p + 1, L"</Event>")) end = p;
    if (!end)
    {
        heap_free(xml);
        *status = ERROR_EVT_INVALID_EVENT_DATA;
        return NULL;
    }
    mb_append_n(&mb, xml, end - xml);
    heap_free(xml);

    culture_name(pub, culture, ARRAY_SIZE(culture));
    mb_append(&mb, L"<RenderingInfo Culture='");
    mb_append(&mb, culture);
    mb_append(&mb, L"'><Message>");
    if ((part = format_message_id(pub, event, event->record->EventID, NULL, 0, &ignored)))
    {
        mb_append_escaped(&mb, part);
        heap_free(part);
    }
    mb_append(&mb, L"</Message><Level>");
    if ((part = format_level(event, &ignored)))
    {
        mb_append_escaped(&mb, part);
        heap_free(part);
    }
    mb_append(&mb, L"</Level>");
    if ((part = format_task(pub, event, &ignored)))
    {
        mb_append(&mb, L"<Task>");
        mb_append_escaped(&mb, part);
        mb_append(&mb, L"</Task>");
        heap_free(part);
    }
    if ((part = format_channel(pub, event, &ignored)))
    {
        mb_append(&mb, L"<Channel>");
        mb_append_escaped(&mb, part);
        mb_append(&mb, L"</Channel>");
        heap_free(part);
    }
    if ((part = format_provider(pub, event, &ignored)))
    {
        mb_append(&mb, L"<Provider>");
        mb_append_escaped(&mb, part);
        mb_append(&mb, L"</Provider>");
        heap_free(part);
    }
    if ((part = format_keywords(event, &len, &ignored)))
    {
        mb_append(&mb, L"<Keywords>");
        for (p = part; *p; p += lstrlenW(p) + 1)
        {
            mb_append(&mb, L"<Keyword>");
            mb_append_escaped(&mb, p);
            mb_append(&mb, L"</Keyword>");
        }
        mb_append(&mb, L"</Keywords>");
        heap_free(part);
    }
    mb_append(&mb, L"</RenderingInfo></Event>");
    if (!(xml = mb_finish(&mb)))
        *status = ERROR_OUTOFMEMORY;
    return xml;
}

/* Copies text plus one more terminator: Windows reports the length, its terminator and one character */
static BOOL copy_out(const WCHAR *text, SIZE_T len, DWORD size, WCHAR *buffer, DWORD *used)
{
    if (len + 1 > MAXDWORD)
    {
        SetLastError(ERROR_EVT_INVALID_EVENT_DATA);
        return FALSE;
    }
    if (used) *used = (DWORD)(len + 1);
    if (!buffer || size < len + 1)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(buffer, text, len * sizeof(WCHAR));
    buffer[len] = 0;
    return TRUE;
}

EVT_HANDLE WINAPI EvtOpenPublisherMetadata(EVT_HANDLE session, const WCHAR *identity, const WCHAR *logfile,
                                           LCID locale, DWORD flags)
{
    struct evt_publisher *pub;

    TRACE("(%p %s %s %#lx %#lx)\n", session, debugstr_w(identity), debugstr_w(logfile), locale, flags);

    if (flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (session)
    {
        const struct evt_session *s = evt_get_object(session, EVT_TYPE_SESSION);
        if (!s)
            return NULL;
        if (s->server)
        {
            FIXME("remote publisher metadata is not supported\n");
            SetLastError(ERROR_NOT_SUPPORTED);
            return NULL;
        }
    }
    if (logfile)
        FIXME("metadata of an archived log (%s) is not read; the local sources are used\n", debugstr_w(logfile));
    if (!(pub = evt_alloc_object(EVT_TYPE_PUBLISHER, sizeof(*pub))))
        return NULL;
    pub->locale = locale;
    if (identity && *identity)
    {
        pub->name = evt_strdupW(identity);
        pub->channel = pub->name ? find_source_channel(identity) : NULL;
        if (!pub->channel)
        {
            DWORD err = pub->name ? ERROR_FILE_NOT_FOUND : ERROR_OUTOFMEMORY;
            heap_free(pub->name);
            heap_free(pub);
            SetLastError(err);
            return NULL;
        }
    }
    return pub;
}

void evt_free_publisher(struct evt_publisher *pub)
{
    heap_free(pub->name);
    heap_free(pub->channel);
}

static BOOL put_variant_string(EVT_VARIANT *v, DWORD size, DWORD *used, const WCHAR *str)
{
    DWORD need = sizeof(EVT_VARIANT) + (str ? (lstrlenW(str) + 1) * sizeof(WCHAR) : 0);

    *used = need;
    if (size < need || !v)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(v, 0, sizeof(*v));
    if (!str)
    {
        v->Type = EvtVarTypeNull;
        return TRUE;
    }
    v->Type = EvtVarTypeString;
    v->StringVal = (WCHAR *)(v + 1);
    lstrcpyW((WCHAR *)(v + 1), str);
    return TRUE;
}

BOOL WINAPI EvtGetPublisherMetadataProperty(EVT_HANDLE handle, EVT_PUBLISHER_METADATA_PROPERTY_ID id, DWORD flags,
                                            DWORD size, EVT_VARIANT *buffer, DWORD *used)
{
    struct evt_publisher *pub;
    const WCHAR *value = NULL;
    WCHAR *text;
    BOOL ret;

    TRACE("(%p %u %#lx %lu %p %p)\n", handle, id, flags, size, buffer, used);

    if (!used || flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!(pub = evt_get_object(handle, EVT_TYPE_PUBLISHER)))
        return FALSE;

    switch (id)
    {
    case EvtPublisherMetadataPublisherGuid:
    case EvtPublisherMetadataResourceFilePath:
    case EvtPublisherMetadataHelpLink:
        return put_variant_string(buffer, size, used, NULL);
    case EvtPublisherMetadataMessageFilePath:
        value = L"EventMessageFile";
        break;
    case EvtPublisherMetadataParameterFilePath:
        value = L"ParameterMessageFile";
        break;
    default:
        FIXME("property %u is not supported\n", id);
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    text = pub->name ? read_source_value(pub, value) : NULL;
    ret = put_variant_string(buffer, size, used, text);
    heap_free(text);
    return ret;
}

BOOL WINAPI EvtFormatMessage(EVT_HANDLE publisher, EVT_HANDLE event_handle, DWORD id, DWORD count,
                             EVT_VARIANT *values, DWORD flags, DWORD size, WCHAR *buffer, DWORD *used)
{
    struct evt_publisher *pub = NULL;
    struct evt_event *event = NULL;
    WCHAR *text = NULL;
    SIZE_T len = 0;
    DWORD status = ERROR_SUCCESS;
    BOOL ret;

    TRACE("(%p %p %lu %lu %p %#lx %lu %p %p)\n", publisher, event_handle, id, count, values, flags, size, buffer, used);

    if (flags < EvtFormatMessageEvent || flags > EvtFormatMessageXml || (flags != EvtFormatMessageId && !event_handle) ||
        (count && !values))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (publisher && !(pub = evt_get_object(publisher, EVT_TYPE_PUBLISHER)))
        return FALSE;
    if (event_handle && !(event = evt_get_object(event_handle, EVT_TYPE_EVENT)))
        return FALSE;
    if (!pub)
    {
        SetLastError(ERROR_EVT_MESSAGE_NOT_FOUND);
        return FALSE;
    }

    switch (flags)
    {
    case EvtFormatMessageEvent:
        text = format_message_id(pub, event, event->record->EventID, values, count, &status);
        break;
    case EvtFormatMessageLevel:
        text = format_level(event, &status);
        break;
    case EvtFormatMessageTask:
        text = format_task(pub, event, &status);
        break;
    case EvtFormatMessageOpcode:
        /* Classic records carry no opcode */
        status = ERROR_EVT_MESSAGE_ID_NOT_FOUND;
        break;
    case EvtFormatMessageKeyword:
        text = format_keywords(event, &len, &status);
        break;
    case EvtFormatMessageChannel:
        text = format_channel(pub, event, &status);
        break;
    case EvtFormatMessageProvider:
        text = format_provider(pub, event, &status);
        break;
    case EvtFormatMessageId:
        text = format_message_id(pub, event, id, values, count, &status);
        break;
    case EvtFormatMessageXml:
        text = format_xml(pub, event, &status);
        break;
    }
    if (!text)
    {
        SetLastError(status ? status : ERROR_OUTOFMEMORY);
        return FALSE;
    }
    if (flags != EvtFormatMessageKeyword)
        len = lstrlenW(text) + 1;
    ret = copy_out(text, len, size, buffer, used);
    heap_free(text);
    if (ret && status)
    {
        /* The buffer holds the message with the unresolved insert left in place */
        SetLastError(status);
        return FALSE;
    }
    return ret;
}
