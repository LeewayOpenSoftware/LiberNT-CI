/*
 * PROJECT:     LiberNT wevtapi
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Structured queries, result sets and event objects
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 *
 * Query language: the XPath 1.0 subset the event log documents for event
 * selection, applied to the System part of classic records:
 *
 *   *  |  Event  |  *[expr]  |  Event/System[expr]
 *   expr := and-expr ('or' and-expr)*
 *   and-expr := unary ('and' unary)*
 *   unary := '(' expr ')' | 'System' '[' expr ']' | ['System/'] predicate
 *   predicate := field op number
 *              | 'Provider' '[' '@Name' ('=' | '!=') string ']'
 *              | 'TimeCreated' '[' 'timediff' '(' '@SystemTime' ')' op number ']'
 *              | 'band' '(' 'Keywords' ',' number ')'
 *   field := EventID | Level | Task | Keywords | EventRecordID
 *   op := '=' | '!=' | '<' | '<=' | '>' | '>='
 *
 * plus a QueryList with one Query holding one Select. Anything else is
 * ERROR_EVT_INVALID_QUERY.
 */

#include "wevtapi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wevtapi);

enum filter_kind { F_TRUE, F_AND, F_OR, F_CMP, F_PROVIDER, F_TIMEDIFF, F_BAND };
enum filter_op { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE };
enum filter_field { FLD_EVENTID, FLD_LEVEL, FLD_TASK, FLD_KEYWORDS, FLD_RECORDID };

struct evt_filter
{
    enum filter_kind kind;
    enum filter_op op;
    enum filter_field field;
    ULONGLONG value;
    WCHAR *str;
    struct evt_filter *left, *right;
};

/* Parsing, matching and freeing recurse over the tree: bound its depth and size */
#define EVT_MAX_QUERY_DEPTH  32
#define EVT_MAX_FILTER_NODES 512

struct parser
{
    const WCHAR *p;
    BOOL error;
    int depth;
    int nodes;
};

void evt_free_filter(struct evt_filter *f)
{
    if (!f) return;
    evt_free_filter(f->left);
    evt_free_filter(f->right);
    heap_free(f->str);
    heap_free(f);
}

static struct evt_filter *new_filter(struct parser *ps, enum filter_kind kind)
{
    struct evt_filter *f;

    if (++ps->nodes > EVT_MAX_FILTER_NODES)
    {
        ps->error = TRUE;
        return NULL;
    }
    f = heap_alloc_zero(sizeof(*f));
    if (!f) ps->error = TRUE;
    else f->kind = kind;
    return f;
}

static void skip_ws(struct parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r' || *ps->p == '\n') ps->p++;
}

static BOOL is_name_char(WCHAR c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '@';
}

/* Consumes a keyword or punctuation; a word must not run into a longer name */
static BOOL match_tok(struct parser *ps, const WCHAR *tok)
{
    int len = lstrlenW(tok);

    skip_ws(ps);
    if (wcsncmp(ps->p, tok, len)) return FALSE;
    if (is_name_char(tok[len - 1]) && is_name_char(ps->p[len])) return FALSE;
    ps->p += len;
    return TRUE;
}

static void expect_tok(struct parser *ps, const WCHAR *tok)
{
    if (!match_tok(ps, tok)) ps->error = TRUE;
}

static BOOL parse_number(struct parser *ps, ULONGLONG *value)
{
    ULONGLONG v = 0;
    const WCHAR *start;

    skip_ws(ps);
    start = ps->p;
    if (ps->p[0] == '0' && (ps->p[1] == 'x' || ps->p[1] == 'X'))
    {
        ps->p += 2;
        start = ps->p;
        for (;;)
        {
            WCHAR c = *ps->p;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            if (v >> 60) { ps->error = TRUE; return FALSE; }
            v = (v << 4) | d;
            ps->p++;
        }
    }
    else
    {
        while (*ps->p >= '0' && *ps->p <= '9')
        {
            if (v > (~(ULONGLONG)0 - 9) / 10) { ps->error = TRUE; return FALSE; }
            v = v * 10 + (*ps->p - '0');
            ps->p++;
        }
    }
    if (ps->p == start || is_name_char(*ps->p))
    {
        ps->error = TRUE;
        return FALSE;
    }
    *value = v;
    return TRUE;
}

static BOOL parse_op(struct parser *ps, enum filter_op *op)
{
    skip_ws(ps);
    if (match_tok(ps, L"!=")) *op = OP_NE;
    else if (match_tok(ps, L"<=")) *op = OP_LE;
    else if (match_tok(ps, L">=")) *op = OP_GE;
    else if (match_tok(ps, L"=")) *op = OP_EQ;
    else if (match_tok(ps, L"<")) *op = OP_LT;
    else if (match_tok(ps, L">")) *op = OP_GT;
    else { ps->error = TRUE; return FALSE; }
    return TRUE;
}

static WCHAR *parse_string(struct parser *ps)
{
    WCHAR quote, *str;
    const WCHAR *start;
    int len;

    skip_ws(ps);
    quote = *ps->p;
    if (quote != '\'' && quote != '"') { ps->error = TRUE; return NULL; }
    start = ++ps->p;
    while (*ps->p && *ps->p != quote) ps->p++;
    if (!*ps->p) { ps->error = TRUE; return NULL; }
    len = ps->p - start;
    ps->p++;
    if (!(str = heap_alloc((len + 1) * sizeof(WCHAR)))) { ps->error = TRUE; return NULL; }
    memcpy(str, start, len * sizeof(WCHAR));
    str[len] = 0;
    return str;
}

static struct evt_filter *parse_expr(struct parser *ps);

static struct evt_filter *parse_predicate(struct parser *ps)
{
    static const struct { const WCHAR *name; enum filter_field field; } fields[] =
    {
        { L"EventID", FLD_EVENTID }, { L"Level", FLD_LEVEL }, { L"Task", FLD_TASK },
        { L"Keywords", FLD_KEYWORDS }, { L"EventRecordID", FLD_RECORDID },
    };
    struct evt_filter *f;
    unsigned int i;

    if (match_tok(ps, L"Provider"))
    {
        if (!(f = new_filter(ps, F_PROVIDER))) return NULL;
        expect_tok(ps, L"[");
        expect_tok(ps, L"@Name");
        if (!ps->error && parse_op(ps, &f->op) && f->op != OP_EQ && f->op != OP_NE) ps->error = TRUE;
        if (!ps->error) f->str = parse_string(ps);
        expect_tok(ps, L"]");
        return f;
    }
    if (match_tok(ps, L"TimeCreated"))
    {
        if (!(f = new_filter(ps, F_TIMEDIFF))) return NULL;
        expect_tok(ps, L"[");
        expect_tok(ps, L"timediff");
        expect_tok(ps, L"(");
        expect_tok(ps, L"@SystemTime");
        expect_tok(ps, L")");
        if (!ps->error) parse_op(ps, &f->op);
        if (!ps->error) parse_number(ps, &f->value);
        expect_tok(ps, L"]");
        return f;
    }
    if (match_tok(ps, L"band"))
    {
        if (!(f = new_filter(ps, F_BAND))) return NULL;
        expect_tok(ps, L"(");
        expect_tok(ps, L"Keywords");
        expect_tok(ps, L",");
        if (!ps->error) parse_number(ps, &f->value);
        expect_tok(ps, L")");
        return f;
    }
    for (i = 0; i < ARRAY_SIZE(fields); i++)
    {
        if (match_tok(ps, fields[i].name))
        {
            if (!(f = new_filter(ps, F_CMP))) return NULL;
            f->field = fields[i].field;
            if (parse_op(ps, &f->op)) parse_number(ps, &f->value);
            return f;
        }
    }
    ps->error = TRUE;
    return NULL;
}

static struct evt_filter *parse_unary(struct parser *ps)
{
    struct evt_filter *f;

    if (match_tok(ps, L"("))
    {
        f = parse_expr(ps);
        expect_tok(ps, L")");
        return f;
    }
    if (match_tok(ps, L"System"))
    {
        if (match_tok(ps, L"/"))
            return parse_predicate(ps);
        expect_tok(ps, L"[");
        f = parse_expr(ps);
        expect_tok(ps, L"]");
        return f;
    }
    return parse_predicate(ps);
}

static struct evt_filter *parse_binary(struct parser *ps, BOOL is_or)
{
    struct evt_filter *left, *node;

    left = is_or ? parse_binary(ps, FALSE) : parse_unary(ps);
    while (!ps->error && match_tok(ps, is_or ? L"or" : L"and"))
    {
        if (!(node = new_filter(ps, is_or ? F_OR : F_AND)))
            break;
        node->left = left;
        node->right = is_or ? parse_binary(ps, FALSE) : parse_unary(ps);
        left = node;
    }
    return left;
}

static struct evt_filter *parse_expr(struct parser *ps)
{
    struct evt_filter *f;

    if (++ps->depth > EVT_MAX_QUERY_DEPTH)
    {
        ps->error = TRUE;
        ps->depth--;
        return NULL;
    }
    f = parse_binary(ps, TRUE);
    ps->depth--;
    return f;
}

static BOOL parse_xpath(const WCHAR *query, struct evt_filter **out)
{
    struct parser ps;
    struct evt_filter *f = NULL;

    ps.p = query;
    ps.error = FALSE;
    ps.depth = 0;
    ps.nodes = 0;

    if (match_tok(&ps, L"*")) { }
    else if (match_tok(&ps, L"Event"))
    {
        if (match_tok(&ps, L"/"))
        {
            expect_tok(&ps, L"System");
            expect_tok(&ps, L"[");
            f = parse_expr(&ps);
            expect_tok(&ps, L"]");
        }
    }
    else ps.error = TRUE;

    if (!ps.error && !f && match_tok(&ps, L"["))
    {
        f = parse_expr(&ps);
        expect_tok(&ps, L"]");
    }
    skip_ws(&ps);
    if (ps.error || *ps.p)
    {
        evt_free_filter(f);
        return FALSE;
    }
    if (!f && !(f = heap_alloc_zero(sizeof(*f))))
        return FALSE;   /* F_TRUE */
    *out = f;
    return TRUE;
}

/* XML text of a QueryList Select: undo the five predefined entities */
static WCHAR *xml_unescape(const WCHAR *start, int len)
{
    static const struct { const WCHAR *ent; WCHAR c; } ents[] =
    {
        { L"&lt;", '<' }, { L"&gt;", '>' }, { L"&amp;", '&' }, { L"&apos;", '\'' }, { L"&quot;", '"' },
    };
    WCHAR *out = heap_alloc((len + 1) * sizeof(WCHAR)), *q = out;
    const WCHAR *end = start + len;
    unsigned int i;

    if (!out) return NULL;
    while (start < end)
    {
        if (*start == '&')
        {
            for (i = 0; i < ARRAY_SIZE(ents); i++)
            {
                int el = lstrlenW(ents[i].ent);
                if (end - start >= el && !wcsncmp(start, ents[i].ent, el))
                {
                    *q++ = ents[i].c;
                    start += el;
                    break;
                }
            }
            if (i < ARRAY_SIZE(ents)) continue;
        }
        *q++ = *start++;
    }
    *q = 0;
    return out;
}

WCHAR *evt_xml_attr(const WCHAR *tag_start, const WCHAR *tag_end, const WCHAR *name)
{
    const WCHAR *p = tag_start + 1;   /* tag_start is the '<' */
    int len = lstrlenW(name);

    while (p < tag_end)
    {
        if (!wcsncmp(p, name, len) && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r'))
        {
            const WCHAR *v = p + len, *ve;
            WCHAR quote;
            while (*v == ' ') v++;
            if (*v++ != '=') return NULL;
            while (*v == ' ') v++;
            quote = *v;
            if (quote != '"' && quote != '\'') return NULL;
            ve = ++v;
            while (ve < tag_end && *ve != quote) ve++;
            if (ve >= tag_end) return NULL;
            return xml_unescape(v, ve - v);
        }
        p++;
    }
    return NULL;
}

static BOOL parse_query_list(const WCHAR *query, struct evt_filter **filter, WCHAR **channel)
{
    const WCHAR *sel, *sel_end, *text_end;
    WCHAR *xpath;
    BOOL ret;

    if (!wcsstr(query, L"<QueryList") || !(sel = wcsstr(query, L"<Select")))
        return FALSE;
    if (wcsstr(sel + 7, L"<Select") || wcsstr(query, L"<Suppress"))
    {
        FIXME("query lists with more than one Select, or with Suppress, are not supported\n");
        return FALSE;
    }
    if (!(sel_end = wcschr(sel, '>')) || !(text_end = wcsstr(sel_end, L"</Select>")))
        return FALSE;
    *channel = evt_xml_attr(sel, sel_end, L"Path");
    if (!(xpath = xml_unescape(sel_end + 1, text_end - (sel_end + 1))))
        return FALSE;
    ret = parse_xpath(xpath, filter);
    heap_free(xpath);
    if (!ret)
    {
        heap_free(*channel);
        *channel = NULL;
    }
    return ret;
}

BOOL evt_parse_query(const WCHAR *query, struct evt_filter **filter, WCHAR **channel)
{
    const WCHAR *p = query;

    *filter = NULL;
    *channel = NULL;
    if (!query || !*query)
    {
        if (!(*filter = heap_alloc_zero(sizeof(**filter))))
        {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        return TRUE;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '<' ? parse_query_list(p, filter, channel) : parse_xpath(p, filter))
        return TRUE;
    WARN("unsupported or invalid query %s\n", debugstr_w(query));
    SetLastError(ERROR_EVT_INVALID_QUERY);
    return FALSE;
}

/* --------------------------------------------------------------------------
 * Classic record -> System values
 * -------------------------------------------------------------------------- */

BYTE evt_record_level(const EVENTLOGRECORD *r)
{
    switch (r->EventType)
    {
    case EVENTLOG_ERROR_TYPE:        return 2;
    case EVENTLOG_WARNING_TYPE:      return 3;
    case EVENTLOG_AUDIT_SUCCESS:
    case EVENTLOG_AUDIT_FAILURE:     return 0;
    default:                         return 4;
    }
}

ULONGLONG evt_record_keywords(const EVENTLOGRECORD *r)
{
    /* The classic keyword (bit 55), plus the audit outcome bits */
    if (r->EventType == EVENTLOG_AUDIT_SUCCESS) return 0x8020000000000000ULL;
    if (r->EventType == EVENTLOG_AUDIT_FAILURE) return 0x8010000000000000ULL;
    return 0x0080000000000000ULL;
}

ULONGLONG evt_record_filetime(const EVENTLOGRECORD *r)
{
    return EVT_FILETIME_1970 + (ULONGLONG)r->TimeGenerated * 10000000;
}

ULONGLONG evt_now_filetime(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static BOOL compare(ULONGLONG a, enum filter_op op, ULONGLONG b)
{
    switch (op)
    {
    case OP_EQ: return a == b;
    case OP_NE: return a != b;
    case OP_LT: return a < b;
    case OP_LE: return a <= b;
    case OP_GT: return a > b;
    default:    return a >= b;
    }
}

BOOL evt_filter_match(const struct evt_filter *f, const EVENTLOGRECORD *r, ULONGLONG now)
{
    ULONGLONG v;

    switch (f->kind)
    {
    case F_TRUE:
        return TRUE;
    case F_AND:
        return evt_filter_match(f->left, r, now) && evt_filter_match(f->right, r, now);
    case F_OR:
        return evt_filter_match(f->left, r, now) || evt_filter_match(f->right, r, now);
    case F_PROVIDER:
    {
        BOOL eq = !lstrcmpiW((const WCHAR *)(r + 1), f->str);
        return f->op == OP_EQ ? eq : !eq;
    }
    case F_TIMEDIFF:
    {
        ULONGLONG t = evt_record_filetime(r);
        v = now > t ? (now - t) / 10000 : 0;   /* milliseconds */
        return compare(v, f->op, f->value);
    }
    case F_BAND:
        return (evt_record_keywords(r) & f->value) != 0;
    case F_CMP:
        switch (f->field)
        {
        case FLD_EVENTID:  v = r->EventID & 0xFFFF; break;
        case FLD_LEVEL:    v = evt_record_level(r); break;
        case FLD_TASK:     v = r->EventCategory; break;
        case FLD_KEYWORDS: v = evt_record_keywords(r); break;
        default:           v = r->RecordNumber; break;
        }
        return compare(v, f->op, f->value);
    }
    return FALSE;
}

/* --------------------------------------------------------------------------
 * Event objects
 * -------------------------------------------------------------------------- */

EVT_HANDLE evt_create_event(const EVENTLOGRECORD *record, const WCHAR *channel)
{
    struct evt_event *event;

    if (!(event = evt_alloc_object(EVT_TYPE_EVENT, sizeof(*event))))
        return NULL;
    event->record = heap_alloc(record->Length);
    event->channel = evt_strdupW(channel);
    if (!event->record || !event->channel)
    {
        heap_free(event->record);
        heap_free(event->channel);
        heap_free(event);
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }
    memcpy(event->record, record, record->Length);
    return event;
}

void evt_free_event(struct evt_event *event)
{
    heap_free(event->record);
    heap_free(event->channel);
}

/* A record as read from the service: bounds-checked before any use */
BOOL evt_record_valid(const EVENTLOGRECORD *r, DWORD avail)
{
    if (avail < sizeof(*r) || r->Length < sizeof(*r) || r->Length > avail || (r->Length & 3))
        return FALSE;
    if (r->StringOffset > r->Length || r->UserSidOffset > r->Length ||
        r->UserSidLength > r->Length - r->UserSidOffset ||
        r->DataOffset > r->Length || r->DataLength > r->Length - r->DataOffset)
        return FALSE;
    /* Each insertion string takes at least its terminator */
    if (r->NumStrings && (r->StringOffset < sizeof(*r) || (r->StringOffset & 1) ||
                          r->NumStrings > (r->Length - r->StringOffset) / sizeof(WCHAR)))
        return FALSE;
    /* A SID is at least 8 bytes and DWORD aligned */
    if (r->UserSidLength && (r->UserSidLength < 8 || r->UserSidOffset < sizeof(*r) || (r->UserSidOffset & 3)))
        return FALSE;
    /* SourceName and ComputerName: two terminated strings right after the header */
    {
        const WCHAR *p = (const WCHAR *)(r + 1);
        const WCHAR *end = (const WCHAR *)((const BYTE *)r + r->Length);
        int found = 0;
        while (p < end && found < 2)
            if (!*p++) found++;
        if (found < 2)
            return FALSE;
    }
    return TRUE;
}

/* --------------------------------------------------------------------------
 * Result sets
 * -------------------------------------------------------------------------- */

void evt_free_result_set(struct evt_result_set *set)
{
    if (set->classic) CloseEventLog(set->classic);
    heap_free(set->channel);
    heap_free(set->buffer);
    evt_free_filter(set->filter);
    DeleteCriticalSection(&set->cs);
}

EVT_HANDLE WINAPI EvtQuery(EVT_HANDLE session, const WCHAR *path, const WCHAR *query, DWORD flags)
{
    struct evt_result_set *set;
    struct evt_session *s = NULL;
    struct evt_filter *filter;
    WCHAR *ql_channel;
    const WCHAR *source;
    DWORD kind = flags & (EvtQueryChannelPath | EvtQueryFilePath);
    DWORD dir = flags & (EvtQueryForwardDirection | EvtQueryReverseDirection);

    TRACE("(%p %s %s %#lx)\n", session, debugstr_w(path), debugstr_w(query), flags);

    if (session && !(s = evt_get_object(session, EVT_TYPE_SESSION)))
        return NULL;
    if ((kind != EvtQueryChannelPath && kind != EvtQueryFilePath) || dir == (EvtQueryForwardDirection | EvtQueryReverseDirection) ||
        (flags & ~(EvtQueryChannelPath | EvtQueryFilePath | EvtQueryForwardDirection | EvtQueryReverseDirection |
                   EvtQueryTolerateQueryErrors)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!evt_parse_query(query, &filter, &ql_channel))
        return NULL;
    source = path ? path : ql_channel;
    if (!source)
    {
        evt_free_filter(filter);
        SetLastError(ERROR_EVT_INVALID_CHANNEL_PATH);
        return NULL;
    }
    if (!(set = evt_alloc_object(EVT_TYPE_RESULT_SET, sizeof(*set))))
    {
        evt_free_filter(filter);
        heap_free(ql_channel);
        return NULL;
    }
    InitializeCriticalSection(&set->cs);
    set->filter = filter;
    set->reverse = (dir == EvtQueryReverseDirection);
    set->channel = evt_strdupW(source);
    heap_free(ql_channel);
    set->classic = set->channel ? evt_open_classic(s ? s->server : NULL, set->channel, kind == EvtQueryFilePath) : NULL;
    if (!set->classic)
    {
        DWORD err = set->channel ? GetLastError() : ERROR_OUTOFMEMORY;
        evt_free_result_set(set);
        heap_free(set);
        SetLastError(err);
        return NULL;
    }
    return set;
}

/* The record range [oldest, oldest + count) of the log right now */
BOOL evt_log_range(HANDLE log, DWORD *oldest, DWORD *count)
{
    if (!GetNumberOfEventLogRecords(log, count))
        return FALSE;
    if (!*count)
    {
        *oldest = 0;
        return TRUE;
    }
    return GetOldestEventLogRecord(log, oldest);
}

/* Fills set->buffer starting at set->next. FALSE with ERROR_NO_MORE_ITEMS at the end. */
static BOOL fill_buffer(struct evt_result_set *set)
{
    DWORD oldest, count, read, needed;
    BYTE *grown;

    if (!evt_log_range(set->classic, &oldest, &count))
        return FALSE;
    if (!count)
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    if (!set->started)
    {
        set->next = set->reverse ? oldest + count - 1 : oldest;
        set->started = TRUE;
    }
    if (set->reverse ? (set->next < oldest || set->next > oldest + count - 1)
                     : (set->next > oldest + count - 1))
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    if (!set->reverse && set->next < oldest)
        set->next = oldest;   /* records were overwritten under us */

    if (!set->buffer)
    {
        set->buffer_size = 0x10000;
        if (!(set->buffer = heap_alloc(set->buffer_size)))
        {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
    }
    for (;;)
    {
        if (ReadEventLogW(set->classic, EVENTLOG_SEEK_READ |
                          (set->reverse ? EVENTLOG_BACKWARDS_READ : EVENTLOG_FORWARDS_READ),
                          set->next, set->buffer, set->buffer_size, &read, &needed))
            break;
        if (GetLastError() == ERROR_HANDLE_EOF)
        {
            SetLastError(ERROR_NO_MORE_ITEMS);
            return FALSE;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed <= set->buffer_size || needed > 0x1000000)
            return FALSE;
        if (!(grown = heap_realloc(set->buffer, needed)))
        {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        set->buffer = grown;
        set->buffer_size = needed;
    }
    set->buffer_len = read;
    set->buffer_pos = 0;
    return TRUE;
}

/* Next matching record of the set, or NULL with the error set */
const EVENTLOGRECORD *evt_reader_next(struct evt_result_set *set, ULONGLONG now)
{
    const EVENTLOGRECORD *r;

    for (;;)
    {
        if (set->cancelled)
        {
            set->cancelled = FALSE;
            SetLastError(ERROR_CANCELLED);
            return NULL;
        }
        if (set->buffer_pos >= set->buffer_len && !fill_buffer(set))
            return NULL;
        r = (const EVENTLOGRECORD *)(set->buffer + set->buffer_pos);
        if (!evt_record_valid(r, set->buffer_len - set->buffer_pos))
        {
            ERR("malformed record at offset %lu\n", set->buffer_pos);
            set->buffer_len = 0;
            SetLastError(ERROR_EVT_INVALID_EVENT_DATA);
            return NULL;
        }
        set->buffer_pos += r->Length;
        set->next = set->reverse ? r->RecordNumber - 1 : r->RecordNumber + 1;
        if (evt_filter_match(set->filter, r, now))
            return r;
    }
}

BOOL WINAPI EvtNext(EVT_HANDLE handle, DWORD size, EVT_HANDLE *events, DWORD timeout, DWORD flags, DWORD *returned)
{
    struct evt_object *obj;
    struct evt_result_set *set;
    const EVENTLOGRECORD *r;
    ULONGLONG now = evt_now_filetime();
    DWORD n = 0, err = ERROR_SUCCESS;

    TRACE("(%p %lu %p %lu %#lx %p)\n", handle, size, events, timeout, flags, returned);

    if (!events || !returned || !size || flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *returned = 0;
    if (!(obj = evt_get_object(handle, EVT_TYPE_ANY)))
        return FALSE;
    if (obj->type == EVT_TYPE_SUBSCRIPTION)
        return evt_subscription_next((struct evt_subscription *)obj, size, events, returned);
    if (obj->type != EVT_TYPE_RESULT_SET)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    set = (struct evt_result_set *)obj;

    EnterCriticalSection(&set->cs);
    while (n < size)
    {
        if (!(r = evt_reader_next(set, now)))
        {
            err = GetLastError();
            break;
        }
        if (!(events[n] = evt_create_event(r, set->channel)))
        {
            /* Not consumed: the next call reads this record again */
            err = GetLastError();
            set->next = r->RecordNumber;
            set->buffer_len = set->buffer_pos = 0;
            break;
        }
        n++;
    }
    LeaveCriticalSection(&set->cs);

    *returned = n;
    if (n)
        return TRUE;
    SetLastError(err == ERROR_SUCCESS ? ERROR_NO_MORE_ITEMS : err);
    return FALSE;
}

BOOL WINAPI EvtSeek(EVT_HANDLE handle, LONGLONG position, EVT_HANDLE bookmark, DWORD timeout, DWORD flags)
{
    struct evt_result_set *set;
    struct evt_bookmark *bm = NULL;
    DWORD oldest, count;
    LONGLONG index, current;

    TRACE("(%p %s %p %lu %#lx)\n", handle, wine_dbgstr_longlong(position), bookmark, timeout, flags);

    if (!(set = evt_get_object(handle, EVT_TYPE_RESULT_SET)))
        return FALSE;
    if ((flags & EvtSeekOriginMask) == EvtSeekRelativeToBookmark &&
        (!(bm = evt_get_object(bookmark, EVT_TYPE_BOOKMARK)) || !bm->valid))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    /* Classic record numbers are 32-bit: anything farther cannot exist (and keeps the sums below exact) */
    if (position > ((LONGLONG)1 << 40) || position < -((LONGLONG)1 << 40) || (bm && bm->record_id > 0xFFFFFFFF))
    {
        SetLastError(ERROR_NOT_FOUND);
        return FALSE;
    }

    EnterCriticalSection(&set->cs);
    if (!evt_log_range(set->classic, &oldest, &count))
    {
        LeaveCriticalSection(&set->cs);
        return FALSE;
    }
    /* Positions count records in the direction of the set */
#define REC_TO_INDEX(rec) (set->reverse ? (LONGLONG)(oldest + count - 1) - (LONGLONG)(rec) : (LONGLONG)(rec) - (LONGLONG)oldest)
    current = set->started ? REC_TO_INDEX(set->next) - 1 : -1;
    switch (flags & EvtSeekOriginMask)
    {
    case EvtSeekRelativeToFirst:    index = position; break;
    case EvtSeekRelativeToLast:     index = (LONGLONG)count - 1 + position; break;
    case EvtSeekRelativeToCurrent:  index = current + position; break;
    case EvtSeekRelativeToBookmark: index = REC_TO_INDEX(bm->record_id) + position; break;
    default:
        LeaveCriticalSection(&set->cs);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
#undef REC_TO_INDEX
    if (!count || index < 0 || index >= (LONGLONG)count)
    {
        LeaveCriticalSection(&set->cs);
        SetLastError(ERROR_NOT_FOUND);
        return FALSE;
    }
    set->next = set->reverse ? (DWORD)(oldest + count - 1 - index) : (DWORD)(oldest + index);
    set->started = TRUE;
    set->buffer_len = set->buffer_pos = 0;
    LeaveCriticalSection(&set->cs);
    return TRUE;
}
