/*
 * PROJECT:     LiberNT Task Scheduler service
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Task Scheduler 2.0 engine: triggers, Exec actions, principals,
 *              run state and caller authorization
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Tasks live as XML files under %SystemRoot%\system32\Tasks. The engine loads
 * every file, works out when each trigger fires next, starts the Exec actions
 * with the token of the task principal and writes the run state (last run
 * time, last result, missed runs) under
 * HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskState\<path>.
 * That key is LiberNT's own format; Windows keeps the same facts in the binary
 * DynamicInfo value of its TaskCache, which nothing outside the service reads.
 */

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winsvc.h"
#include "winuser.h"
#include "sddl.h"
#include "objbase.h"
#include "xmllite.h"
#include "userenv.h"
#include "wtsapi32.h"
#include "rpc.h"
#include "schrpc.h"
#include "taskschd.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "schedsvc_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(schedsvc);

#ifndef SCHED_S_TASK_RUNNING
#define SCHED_S_TASK_RUNNING        ((HRESULT)0x00041301)
#endif
#ifndef SCHED_S_TASK_HAS_NOT_RUN
#define SCHED_S_TASK_HAS_NOT_RUN    ((HRESULT)0x00041303)
#endif
#ifndef SCHED_S_TASK_TERMINATED
#define SCHED_S_TASK_TERMINATED     ((HRESULT)0x00041306)
#endif
#ifndef SCHED_E_TASK_NOT_RUNNING
#define SCHED_E_TASK_NOT_RUNNING    ((HRESULT)0x8004130B)
#endif
#ifndef SCHED_E_ALREADY_RUNNING
#define SCHED_E_ALREADY_RUNNING     ((HRESULT)0x8004131F)
#endif
#ifndef SCHED_E_USER_NOT_LOGGED_ON
#define SCHED_E_USER_NOT_LOGGED_ON  ((HRESULT)0x80041320)
#endif
#ifndef SCHED_S_TASK_QUEUED
#define SCHED_S_TASK_QUEUED         ((HRESULT)0x00041325)
#endif
#ifndef SCHED_E_TASK_DISABLED
#define SCHED_E_TASK_DISABLED       ((HRESULT)0x80041326)
#endif
#ifndef SCHED_E_START_ON_DEMAND
#define SCHED_E_START_ON_DEMAND     ((HRESULT)0x80041328)
#endif

/* TASK_RUN_FLAGS (not in our taskschd.idl yet) */
#define RUN_FLAG_AS_SELF            0x1
#define RUN_FLAG_IGNORE_CONSTRAINTS 0x2
#define RUN_FLAG_USE_SESSION_ID     0x4

#define TICKS_PER_SEC   ((ULONGLONG)10000000)
#define TICKS_PER_MIN   (60 * TICKS_PER_SEC)
#define TICKS_PER_HOUR  (60 * TICKS_PER_MIN)
#define TICKS_PER_DAY   (24 * TICKS_PER_HOUR)
#define NO_TIME         ((ULONGLONG)0)

#define MAX_TRIGGERS    64
#define MAX_ACTIONS     32
#define MAX_TASK_FILE   (1024 * 1024)
#define MAX_TASK_DEPTH  16
#define POLL_MS         60000
/* Boot triggers fire only when the service starts this soon after boot. */
#define BOOT_WINDOW_MS  (10 * 60 * 1000)
/* No user input for this long (and an idle CPU) counts as idle. */
#define IDLE_INPUT_MS   (10 * 60 * 1000)
#define IDLE_CPU_PCT    80

static const WCHAR state_root[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskState";

enum trigger_type
{
    TRIG_TIME,
    TRIG_CALENDAR,
    TRIG_BOOT,
    TRIG_LOGON,
    TRIG_IDLE,
    TRIG_EVENT,
    TRIG_REGISTRATION,
    TRIG_SESSION,
    TRIG_UNKNOWN
};

enum calendar_kind
{
    CAL_NONE,
    CAL_DAILY,
    CAL_WEEKLY,
    CAL_MONTHLY,
    CAL_MONTHLY_DOW
};

enum instances_policy
{
    POLICY_PARALLEL,
    POLICY_QUEUE,
    POLICY_IGNORE_NEW,
    POLICY_STOP_EXISTING
};

struct trigger
{
    enum trigger_type type;
    enum calendar_kind cal;
    BOOL enabled;
    BOOL has_start;
    BOOL has_end;
    ULONGLONG start_lt;         /* local wall clock, 100 ns units */
    ULONGLONG start_utc;
    ULONGLONG end_utc;
    ULONGLONG delay;
    ULONGLONG rep_interval;
    ULONGLONG rep_duration;     /* 0 = indefinitely */
    DWORD days_interval;
    DWORD weeks_interval;
    DWORD days_of_week;         /* bit 0 = Sunday */
    DWORD months;               /* bit 0 = January */
    DWORD days_of_month;        /* bit 0 = day 1 */
    BOOL last_day_of_month;
    WCHAR *user_id;             /* LogonTrigger only */

    /* runtime */
    ULONGLONG next;             /* next calendar/time activation, UTC */
    ULONGLONG pending;          /* event activation + delay, UTC */
    ULONGLONG activated;        /* last event activation, UTC (repetition base) */
};

struct action
{
    WCHAR *command;
    WCHAR *arguments;
    WCHAR *directory;
};

struct task_def
{
    LONG ref;
    BOOL enabled;
    BOOL allow_demand_start;
    BOOL start_when_available;
    BOOL disallow_on_batteries;
    enum instances_policy policy;
    ULONGLONG exec_limit;       /* 0 = no limit */
    DWORD n_triggers;
    struct trigger triggers[MAX_TRIGGERS];
    DWORD n_actions;
    struct action actions[MAX_ACTIONS];
    WCHAR *user_id;
    WCHAR *group_id;
    TASK_LOGON_TYPE logon_type;
    BOOL highest;
    BOOL principal_seen;
};

struct task
{
    struct list entry;
    WCHAR *path;                /* normalized: "\Folder\Name" */
    WCHAR *file;
    FILETIME write_time;
    FILETIME create_time;
    ULONGLONG size;
    struct task_def *def;
    DWORD queued;
    BOOL seen;
};

struct instance
{
    struct list entry;
    GUID guid;
    WCHAR *path;
    struct task_def *def;
    HANDLE token;
    BOOL interactive;
    DWORD n_args;
    WCHAR **args;
    DWORD action;
    HANDLE process;
    DWORD pid;
    HANDLE wait;
    ULONGLONG start;
    ULONGLONG deadline;
    BOOL stopping;
};

struct seen_logon
{
    struct list entry;
    LUID luid;
    BOOL present;
};

static CRITICAL_SECTION engine_cs;
static CRITICAL_SECTION_DEBUG engine_cs_debug =
{
    0, 0, &engine_cs,
    { &engine_cs_debug.ProcessLocksList, &engine_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": engine_cs") }
};
static CRITICAL_SECTION engine_cs = { &engine_cs_debug, -1, 0, 0, 0, 0 };

static struct list tasks = LIST_INIT(tasks);
static struct list instances = LIST_INIT(instances);
static struct list logons = LIST_INIT(logons);
static HANDLE engine_thread;
static HANDLE wake_event;
static HANDLE stop_event;
static BOOL rescan_needed = TRUE;
static BOOL logon_check_needed;
static BOOL idle_fired;
static ULONGLONG cpu_idle_prev, cpu_total_prev;
static WCHAR tasks_dir[MAX_PATH];
static BOOL tasks_dir_has_acls = TRUE;

/* ---------------------------------------------------------------- time */

static ULONGLONG ft_to_u64(const FILETIME *ft)
{
    return ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

static void u64_to_ft(ULONGLONG v, FILETIME *ft)
{
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

static ULONGLONG utc_now(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ft_to_u64(&ft);
}

static ULONGLONG lt_to_utc(ULONGLONG lt)
{
    SYSTEMTIME local, utc;
    FILETIME ft;

    u64_to_ft(lt, &ft);
    if (!FileTimeToSystemTime(&ft, &local)) return lt;
    if (!TzSpecificLocalTimeToSystemTime(NULL, &local, &utc))
    {
        FILETIME out;
        if (!LocalFileTimeToFileTime(&ft, &out)) return lt;
        return ft_to_u64(&out);
    }
    if (!SystemTimeToFileTime(&utc, &ft)) return lt;
    return ft_to_u64(&ft) + (lt % (TICKS_PER_SEC / 1000));
}

static ULONGLONG utc_to_lt(ULONGLONG utc)
{
    SYSTEMTIME st, local;
    FILETIME ft;

    u64_to_ft(utc, &ft);
    if (!FileTimeToSystemTime(&ft, &st)) return utc;
    if (!SystemTimeToTzSpecificLocalTime(NULL, &st, &local))
    {
        FILETIME out;
        if (!FileTimeToLocalFileTime(&ft, &out)) return utc;
        return ft_to_u64(&out);
    }
    if (!SystemTimeToFileTime(&local, &ft)) return utc;
    return ft_to_u64(&ft) + (utc % (TICKS_PER_SEC / 1000));
}

static void utc_to_local_systemtime(ULONGLONG utc, SYSTEMTIME *st)
{
    FILETIME ft;

    u64_to_ft(utc_to_lt(utc), &ft);
    FileTimeToSystemTime(&ft, st);
}

static BOOL local_systemtime_to_utc(const SYSTEMTIME *st, ULONGLONG *utc)
{
    FILETIME ft;

    if (!SystemTimeToFileTime(st, &ft)) return FALSE;
    *utc = lt_to_utc(ft_to_u64(&ft));
    return TRUE;
}

static const WCHAR *parse_number(const WCHAR *s, int digits, int *value)
{
    int i;

    *value = 0;
    for (i = 0; i < digits; i++)
    {
        if (s[i] < '0' || s[i] > '9') return NULL;
        *value = *value * 10 + (s[i] - '0');
    }
    return s + digits;
}

/* xs:dateTime "YYYY-MM-DDTHH:MM:SS[.fff][Z|(+|-)HH:MM]", returned as local time. */
static BOOL parse_boundary(const WCHAR *s, ULONGLONG *lt)
{
    SYSTEMTIME st;
    FILETIME ft;
    int year, month, day, hour, minute, second, oh, om;
    ULONGLONG value;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;

    if (!(s = parse_number(s, 4, &year)) || *s++ != '-') return FALSE;
    if (!(s = parse_number(s, 2, &month)) || *s++ != '-') return FALSE;
    if (!(s = parse_number(s, 2, &day))) return FALSE;
    hour = minute = second = 0;
    if (*s == 'T')
    {
        s++;
        if (!(s = parse_number(s, 2, &hour)) || *s++ != ':') return FALSE;
        if (!(s = parse_number(s, 2, &minute))) return FALSE;
        if (*s == ':')
        {
            s++;
            if (!(s = parse_number(s, 2, &second))) return FALSE;
            if (*s == '.')
            {
                s++;
                while (*s >= '0' && *s <= '9') s++;
            }
        }
    }

    memset(&st, 0, sizeof(st));
    st.wYear = year;
    st.wMonth = month;
    st.wDay = day;
    st.wHour = hour;
    st.wMinute = minute;
    st.wSecond = second;
    if (!SystemTimeToFileTime(&st, &ft)) return FALSE;
    value = ft_to_u64(&ft);

    if (*s == 'Z')
    {
        *lt = utc_to_lt(value);
        return TRUE;
    }
    if (*s == '+' || *s == '-')
    {
        BOOL minus = (*s == '-');
        ULONGLONG offset;

        s++;
        if (!(s = parse_number(s, 2, &oh)) || *s++ != ':') return FALSE;
        if (!parse_number(s, 2, &om)) return FALSE;
        offset = (ULONGLONG)oh * TICKS_PER_HOUR + (ULONGLONG)om * TICKS_PER_MIN;
        /* local-at-offset minus offset = UTC */
        value = minus ? value + offset : value - offset;
        *lt = utc_to_lt(value);
        return TRUE;
    }

    *lt = value;
    return TRUE;
}

/* xs:duration "PnYnMnDTnHnMnS" (also nW); years count 365 days, months 30. */
static BOOL parse_duration(const WCHAR *s, ULONGLONG *out)
{
    ULONGLONG total = 0, num;
    BOOL time_part = FALSE, any = FALSE;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s++ != 'P') return FALSE;

    while (*s && *s != ' ' && *s != '\r' && *s != '\n')
    {
        if (*s == 'T')
        {
            time_part = TRUE;
            s++;
            continue;
        }
        if (*s < '0' || *s > '9') return FALSE;
        num = 0;
        while (*s >= '0' && *s <= '9')
        {
            num = num * 10 + (*s - '0');
            if (num > 1000000) return FALSE;
            s++;
        }
        if (*s == '.' || *s == ',')
        {
            s++;
            while (*s >= '0' && *s <= '9') s++;
        }
        switch (*s++)
        {
        case 'Y': total += num * 365 * TICKS_PER_DAY; break;
        case 'M': total += time_part ? num * TICKS_PER_MIN : num * 30 * TICKS_PER_DAY; break;
        case 'W': total += num * 7 * TICKS_PER_DAY; break;
        case 'D': total += num * TICKS_PER_DAY; break;
        case 'H': total += num * TICKS_PER_HOUR; break;
        case 'S': total += num * TICKS_PER_SEC; break;
        default: return FALSE;
        }
        any = TRUE;
    }

    if (!any) return FALSE;
    *out = total;
    return TRUE;
}

/* ------------------------------------------------------------ calendar */

static void day_to_date(ULONGLONG dayno, SYSTEMTIME *st)
{
    FILETIME ft;

    u64_to_ft(dayno * TICKS_PER_DAY, &ft);
    FileTimeToSystemTime(&ft, st);
}

static BOOL is_last_day_of_month(ULONGLONG dayno)
{
    SYSTEMTIME a, b;

    day_to_date(dayno, &a);
    day_to_date(dayno + 1, &b);
    return a.wMonth != b.wMonth;
}

static BOOL calendar_day_matches(const struct trigger *t, ULONGLONG dayno)
{
    ULONGLONG start_day = t->start_lt / TICKS_PER_DAY;
    SYSTEMTIME st, sst;

    if (dayno < start_day) return FALSE;

    switch (t->cal)
    {
    case CAL_DAILY:
        return ((dayno - start_day) % t->days_interval) == 0;

    case CAL_WEEKLY:
    {
        ULONGLONG week0;

        day_to_date(dayno, &st);
        if (!(t->days_of_week & (1 << st.wDayOfWeek))) return FALSE;
        day_to_date(start_day, &sst);
        week0 = start_day - sst.wDayOfWeek;
        return (((dayno - week0) / 7) % t->weeks_interval) == 0;
    }

    case CAL_MONTHLY:
        day_to_date(dayno, &st);
        if (!(t->months & (1 << (st.wMonth - 1)))) return FALSE;
        if (t->days_of_month & (1u << (st.wDay - 1))) return TRUE;
        return t->last_day_of_month && is_last_day_of_month(dayno);

    default:
        return FALSE;
    }
}

static DWORD calendar_search_days(const struct trigger *t)
{
    switch (t->cal)
    {
    case CAL_DAILY: return t->days_interval + 2;
    case CAL_WEEKLY: return 7 * t->weeks_interval + 8;
    case CAL_MONTHLY: return 4 * 366 + 2;
    default: return 0;
    }
}

/* First base activation strictly after 'after' (local time). */
static ULONGLONG base_next(const struct trigger *t, ULONGLONG after)
{
    ULONGLONG day, tod, start_day, c;
    DWORD i, limit;

    if (!t->has_start) return NO_TIME;
    if (t->type == TRIG_TIME)
        return t->start_lt > after ? t->start_lt : NO_TIME;
    if (t->type != TRIG_CALENDAR) return NO_TIME;

    start_day = t->start_lt / TICKS_PER_DAY;
    tod = t->start_lt % TICKS_PER_DAY;
    day = after / TICKS_PER_DAY;
    if (day < start_day) day = start_day;

    limit = calendar_search_days(t);
    for (i = 0; i < limit; i++, day++)
    {
        if (!calendar_day_matches(t, day)) continue;
        c = day * TICKS_PER_DAY + tod;
        if (c > after) return c;
    }
    return NO_TIME;
}

/* Last base activation at or before 'at' (local time). */
static ULONGLONG base_prev(const struct trigger *t, ULONGLONG at)
{
    ULONGLONG day, tod, start_day, c;
    DWORD i, limit;

    if (!t->has_start) return NO_TIME;
    if (t->type == TRIG_TIME)
        return t->start_lt <= at ? t->start_lt : NO_TIME;
    if (t->type != TRIG_CALENDAR || at < t->start_lt) return NO_TIME;

    start_day = t->start_lt / TICKS_PER_DAY;
    tod = t->start_lt % TICKS_PER_DAY;
    day = at / TICKS_PER_DAY;

    limit = calendar_search_days(t);
    for (i = 0; i < limit && day >= start_day; i++, day--)
    {
        if (!calendar_day_matches(t, day)) continue;
        c = day * TICKS_PER_DAY + tod;
        if (c <= at) return c;
    }
    return NO_TIME;
}

/* Next repetition of a series that began at 'base' (UTC), strictly after 'after'. */
static ULONGLONG repetition_next(const struct trigger *t, ULONGLONG base, ULONGLONG after)
{
    ULONGLONG j, c;

    if (!t->rep_interval || base == NO_TIME || after < base) return NO_TIME;
    j = (after - base) / t->rep_interval + 1;
    c = base + j * t->rep_interval;
    if (t->rep_duration && j * t->rep_interval >= t->rep_duration) return NO_TIME;
    return c;
}

static ULONGLONG min_time(ULONGLONG a, ULONGLONG b)
{
    if (a == NO_TIME) return b;
    if (b == NO_TIME) return a;
    return a < b ? a : b;
}

/* Next time-based activation strictly after 'after' (UTC). */
static ULONGLONG trigger_next_time(const struct trigger *t, ULONGLONG after)
{
    ULONGLONG after_lt, best = NO_TIME, b, p;

    if (!t->enabled) return NO_TIME;

    if (t->type == TRIG_TIME || t->type == TRIG_CALENDAR)
    {
        after_lt = utc_to_lt(after);
        b = base_next(t, after_lt);
        if (b != NO_TIME) best = lt_to_utc(b);
        p = base_prev(t, after_lt);
        if (p != NO_TIME) best = min_time(best, repetition_next(t, lt_to_utc(p), after));
    }
    else
    {
        /* event triggers: the pending activation, then the next repetition */
        best = min_time(t->pending, t->next);
    }

    if (best != NO_TIME && t->has_end && best > t->end_utc) return NO_TIME;
    return best;
}

/* ----------------------------------------------------------- task XML */

static void free_def(struct task_def *def)
{
    DWORD i;

    for (i = 0; i < def->n_triggers; i++)
        free(def->triggers[i].user_id);
    for (i = 0; i < def->n_actions; i++)
    {
        free(def->actions[i].command);
        free(def->actions[i].arguments);
        free(def->actions[i].directory);
    }
    free(def->user_id);
    free(def->group_id);
    free(def);
}

static void def_addref(struct task_def *def)
{
    InterlockedIncrement(&def->ref);
}

static void def_release(struct task_def *def)
{
    if (def && !InterlockedDecrement(&def->ref))
        free_def(def);
}

static BOOL is_true(const WCHAR *s)
{
    return !wcscmp(s, L"true") || !wcscmp(s, L"1");
}

static void set_string(WCHAR **dst, const WCHAR *value)
{
    free(*dst);
    *dst = wcsdup(value);
}

static const WCHAR *day_names[] = { L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday" };
static const WCHAR *month_names[] = { L"January", L"February", L"March", L"April", L"May", L"June", L"July",
                                      L"August", L"September", L"October", L"November", L"December" };

static struct trigger *current_trigger(struct task_def *def)
{
    return def->n_triggers ? &def->triggers[def->n_triggers - 1] : NULL;
}

/* 'rel' is the element path below <Task>, for example "Triggers/BootTrigger/Delay". */
static void parse_element_start(struct task_def *def, const WCHAR *rel, BOOL *in_principal, BOOL *skip_principal)
{
    struct trigger *t;
    const WCHAR *name;
    DWORD i;

    if (!wcsncmp(rel, L"Triggers/", 9) && !wcschr(rel + 9, '/'))
    {
        static const struct { const WCHAR *name; enum trigger_type type; } map[] =
        {
            { L"TimeTrigger", TRIG_TIME },
            { L"CalendarTrigger", TRIG_CALENDAR },
            { L"BootTrigger", TRIG_BOOT },
            { L"LogonTrigger", TRIG_LOGON },
            { L"IdleTrigger", TRIG_IDLE },
            { L"EventTrigger", TRIG_EVENT },
            { L"RegistrationTrigger", TRIG_REGISTRATION },
            { L"SessionStateChangeTrigger", TRIG_SESSION },
        };

        if (def->n_triggers >= MAX_TRIGGERS)
        {
            WARN("too many triggers\n");
            return;
        }
        t = &def->triggers[def->n_triggers++];
        memset(t, 0, sizeof(*t));
        t->type = TRIG_UNKNOWN;
        t->enabled = TRUE;
        for (i = 0; i < ARRAYSIZE(map); i++)
            if (!wcscmp(rel + 9, map[i].name)) t->type = map[i].type;
        if (t->type == TRIG_UNKNOWN) FIXME("unsupported trigger %s\n", debugstr_w(rel + 9));
        return;
    }

    if (!wcscmp(rel, L"Actions/Exec"))
    {
        if (def->n_actions >= MAX_ACTIONS)
        {
            WARN("too many actions\n");
            return;
        }
        memset(&def->actions[def->n_actions++], 0, sizeof(def->actions[0]));
        return;
    }

    if (!wcsncmp(rel, L"Actions/", 8) && !wcschr(rel + 8, '/'))
    {
        FIXME("unsupported action %s (only Exec runs)\n", debugstr_w(rel + 8));
        return;
    }

    if (!wcscmp(rel, L"Principals/Principal"))
    {
        /* Only the first principal is used; Actions Context selects it on Windows. */
        if (def->principal_seen) *skip_principal = TRUE;
        def->principal_seen = TRUE;
        *in_principal = TRUE;
        return;
    }

    if (!wcsncmp(rel, L"Triggers/CalendarTrigger/", 25) && (t = current_trigger(def)) && t->type == TRIG_CALENDAR)
    {
        name = rel + 25;
        if (!wcscmp(name, L"ScheduleByDay"))
        {
            t->cal = CAL_DAILY;
            t->days_interval = 1;
        }
        else if (!wcscmp(name, L"ScheduleByWeek"))
        {
            t->cal = CAL_WEEKLY;
            t->weeks_interval = 1;
        }
        else if (!wcscmp(name, L"ScheduleByMonth"))
        {
            t->cal = CAL_MONTHLY;
        }
        else if (!wcscmp(name, L"ScheduleByMonthDayOfWeek"))
        {
            FIXME("ScheduleByMonthDayOfWeek is not supported, trigger never fires\n");
            t->cal = CAL_MONTHLY_DOW;
        }
        else if (!wcsncmp(name, L"ScheduleByWeek/DaysOfWeek/", 26))
        {
            for (i = 0; i < 7; i++)
                if (!wcscmp(name + 26, day_names[i])) t->days_of_week |= 1 << i;
        }
        else if (!wcsncmp(name, L"ScheduleByMonth/Months/", 23))
        {
            for (i = 0; i < 12; i++)
                if (!wcscmp(name + 23, month_names[i])) t->months |= 1 << i;
        }
    }
}

static void parse_text(struct task_def *def, const WCHAR *rel, const WCHAR *value, BOOL in_principal, BOOL skip_principal)
{
    struct trigger *t;
    struct action *a;
    const WCHAR *name;
    ULONGLONG v;

    if (!wcsncmp(rel, L"Triggers/", 9) && (t = current_trigger(def)))
    {
        name = wcschr(rel + 9, '/');
        if (!name) return;
        name++;

        if (!wcscmp(name, L"StartBoundary"))
        {
            if (parse_boundary(value, &t->start_lt))
            {
                t->has_start = TRUE;
                t->start_utc = lt_to_utc(t->start_lt);
            }
            else WARN("bad StartBoundary %s\n", debugstr_w(value));
        }
        else if (!wcscmp(name, L"EndBoundary"))
        {
            if (parse_boundary(value, &v))
            {
                t->has_end = TRUE;
                t->end_utc = lt_to_utc(v);
            }
            else WARN("bad EndBoundary %s\n", debugstr_w(value));
        }
        else if (!wcscmp(name, L"Enabled"))
            t->enabled = is_true(value);
        else if (!wcscmp(name, L"Delay"))
            parse_duration(value, &t->delay);
        else if (!wcscmp(name, L"UserId"))
            set_string(&t->user_id, value);
        else if (!wcscmp(name, L"Repetition/Interval"))
            parse_duration(value, &t->rep_interval);
        else if (!wcscmp(name, L"Repetition/Duration"))
            parse_duration(value, &t->rep_duration);
        else if (!wcscmp(name, L"ScheduleByDay/DaysInterval"))
            t->days_interval = wcstoul(value, NULL, 10);
        else if (!wcscmp(name, L"ScheduleByWeek/WeeksInterval"))
            t->weeks_interval = wcstoul(value, NULL, 10);
        else if (!wcscmp(name, L"ScheduleByMonth/DaysOfMonth/Day"))
        {
            if (!wcscmp(value, L"Last"))
                t->last_day_of_month = TRUE;
            else
            {
                DWORD d = wcstoul(value, NULL, 10);
                if (d >= 1 && d <= 31) t->days_of_month |= 1u << (d - 1);
            }
        }
        return;
    }

    if (!wcsncmp(rel, L"Actions/Exec/", 13) && def->n_actions)
    {
        a = &def->actions[def->n_actions - 1];
        name = rel + 13;
        if (!wcscmp(name, L"Command")) set_string(&a->command, value);
        else if (!wcscmp(name, L"Arguments")) set_string(&a->arguments, value);
        else if (!wcscmp(name, L"WorkingDirectory")) set_string(&a->directory, value);
        return;
    }

    if (!wcsncmp(rel, L"Principals/Principal/", 21) && in_principal && !skip_principal)
    {
        name = rel + 21;
        if (!wcscmp(name, L"UserId")) set_string(&def->user_id, value);
        else if (!wcscmp(name, L"GroupId")) set_string(&def->group_id, value);
        else if (!wcscmp(name, L"RunLevel")) def->highest = !wcscmp(value, L"HighestAvailable");
        else if (!wcscmp(name, L"LogonType"))
        {
            if (!wcscmp(value, L"Password")) def->logon_type = TASK_LOGON_PASSWORD;
            else if (!wcscmp(value, L"S4U")) def->logon_type = TASK_LOGON_S4U;
            else if (!wcscmp(value, L"InteractiveToken")) def->logon_type = TASK_LOGON_INTERACTIVE_TOKEN;
            else if (!wcscmp(value, L"Group")) def->logon_type = TASK_LOGON_GROUP;
            else if (!wcscmp(value, L"ServiceAccount")) def->logon_type = TASK_LOGON_SERVICE_ACCOUNT;
            else if (!wcscmp(value, L"InteractiveTokenOrPassword"))
                def->logon_type = TASK_LOGON_INTERACTIVE_TOKEN_OR_PASSWORD;
        }
        return;
    }

    if (!wcsncmp(rel, L"Settings/", 9))
    {
        name = rel + 9;
        if (!wcscmp(name, L"Enabled")) def->enabled = is_true(value);
        else if (!wcscmp(name, L"AllowStartOnDemand")) def->allow_demand_start = is_true(value);
        else if (!wcscmp(name, L"StartWhenAvailable")) def->start_when_available = is_true(value);
        else if (!wcscmp(name, L"DisallowStartIfOnBatteries")) def->disallow_on_batteries = is_true(value);
        else if (!wcscmp(name, L"ExecutionTimeLimit"))
        {
            if (!parse_duration(value, &def->exec_limit)) def->exec_limit = 72 * TICKS_PER_HOUR;
        }
        else if (!wcscmp(name, L"MultipleInstancesPolicy"))
        {
            if (!wcscmp(value, L"Parallel")) def->policy = POLICY_PARALLEL;
            else if (!wcscmp(value, L"Queue")) def->policy = POLICY_QUEUE;
            else if (!wcscmp(value, L"StopExisting")) def->policy = POLICY_STOP_EXISTING;
            else def->policy = POLICY_IGNORE_NEW;
        }
    }
}

static void finish_def(struct task_def *def)
{
    DWORD i;

    for (i = 0; i < def->n_triggers; i++)
    {
        struct trigger *t = &def->triggers[i];

        if (t->type == TRIG_CALENDAR)
        {
            if (t->cal == CAL_DAILY && !t->days_interval) t->days_interval = 1;
            if (t->cal == CAL_WEEKLY && !t->weeks_interval) t->weeks_interval = 1;
            if (t->cal == CAL_MONTHLY && !t->months) t->months = 0xfff;
            if (t->cal == CAL_NONE || t->cal == CAL_MONTHLY_DOW) t->enabled = FALSE;
        }
        if (t->rep_interval && t->rep_interval < TICKS_PER_MIN)
        {
            WARN("repetition interval below one minute ignored\n");
            t->rep_interval = 0;
        }
        if (t->type == TRIG_EVENT)
            FIXME("EventTrigger is parsed but never fires: event log subscriptions (wevtapi) are not available yet\n");
        if (t->type == TRIG_SESSION || t->type == TRIG_UNKNOWN)
            t->enabled = FALSE;
    }
}

/* Parses a task XML document (any encoding xmllite detects). */
static HRESULT parse_task_xml(const void *data, SIZE_T size, struct task_def **out)
{
    WCHAR rel[512];
    const WCHAR *stack[MAX_TASK_DEPTH];
    WCHAR *names[MAX_TASK_DEPTH] = { NULL };
    IXmlReader *reader = NULL;
    IStream *stream = NULL;
    struct task_def *def;
    XmlNodeType type;
    HGLOBAL hmem;
    HRESULT hr;
    int depth = 0, task_depth = -1, i;
    BOOL in_principal = FALSE, skip_principal = FALSE, done = FALSE;
    void *buf;

    *out = NULL;
    if (!(def = calloc(1, sizeof(*def)))) return E_OUTOFMEMORY;
    def->ref = 1;
    def->enabled = TRUE;
    def->allow_demand_start = TRUE;
    def->disallow_on_batteries = TRUE;
    def->policy = POLICY_IGNORE_NEW;
    def->exec_limit = 72 * TICKS_PER_HOUR;
    def->logon_type = TASK_LOGON_NONE;

    if (!(hmem = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1)))
    {
        free_def(def);
        return E_OUTOFMEMORY;
    }
    buf = GlobalLock(hmem);
    memcpy(buf, data, size);
    GlobalUnlock(hmem);

    if (FAILED(hr = CreateStreamOnHGlobal(hmem, TRUE, &stream)))
    {
        GlobalFree(hmem);
        free_def(def);
        return hr;
    }
    {
        ULARGE_INTEGER sz;
        sz.QuadPart = size;
        IStream_SetSize(stream, sz);
    }

    if (FAILED(hr = CreateXmlReader(&IID_IXmlReader, (void **)&reader, NULL)))
        goto out;
    IXmlReader_SetProperty(reader, XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    if (FAILED(hr = IXmlReader_SetInput(reader, (IUnknown *)stream)))
        goto out;

    while (!done && (hr = IXmlReader_Read(reader, &type)) == S_OK)
    {
        const WCHAR *name, *value;
        BOOL empty;

        switch (type)
        {
        case XmlNodeType_Element:
            if (FAILED(hr = IXmlReader_GetLocalName(reader, &name, NULL))) goto out;
            empty = IXmlReader_IsEmptyElement(reader);
            if (depth >= MAX_TASK_DEPTH)
            {
                hr = SCHED_E_MALFORMEDXML;
                goto out;
            }
            names[depth] = wcsdup(name);
            if (!names[depth])
            {
                hr = E_OUTOFMEMORY;
                goto out;
            }
            stack[depth] = names[depth];
            depth++;

            if (task_depth < 0 && depth == 1 && !wcscmp(name, L"Task"))
                task_depth = 1;
            else if (task_depth > 0 && depth > task_depth)
            {
                rel[0] = 0;
                for (i = task_depth; i < depth; i++)
                {
                    if (wcslen(rel) + wcslen(stack[i]) + 2 >= ARRAYSIZE(rel)) break;
                    if (i > task_depth) wcscat(rel, L"/");
                    wcscat(rel, stack[i]);
                }
                parse_element_start(def, rel, &in_principal, &skip_principal);
            }

            if (empty)
            {
                depth--;
                free(names[depth]);
                names[depth] = NULL;
            }
            break;

        case XmlNodeType_Text:
        case XmlNodeType_CDATA:
            if (task_depth > 0 && depth > task_depth)
            {
                if (FAILED(hr = IXmlReader_GetValue(reader, &value, NULL))) goto out;
                rel[0] = 0;
                for (i = task_depth; i < depth; i++)
                {
                    if (wcslen(rel) + wcslen(stack[i]) + 2 >= ARRAYSIZE(rel)) break;
                    if (i > task_depth) wcscat(rel, L"/");
                    wcscat(rel, stack[i]);
                }
                parse_text(def, rel, value, in_principal, skip_principal);
            }
            break;

        case XmlNodeType_EndElement:
            if (depth > 0)
            {
                depth--;
                if (task_depth > 0 && depth == task_depth && !wcscmp(stack[depth], L"Principals"))
                    in_principal = skip_principal = FALSE;
                if (task_depth > 0 && depth == task_depth + 1 && !wcscmp(stack[depth], L"Principal"))
                    in_principal = FALSE;
                if (depth == task_depth - 1 || (task_depth > 0 && depth == 0)) done = TRUE;
                free(names[depth]);
                names[depth] = NULL;
            }
            break;

        default:
            break;
        }
    }
    if (hr == S_FALSE) hr = S_OK;
    if (SUCCEEDED(hr) && task_depth < 0) hr = SCHED_E_MALFORMEDXML;

out:
    for (i = 0; i < MAX_TASK_DEPTH; i++) free(names[i]);
    if (reader) IXmlReader_Release(reader);
    if (stream) IStream_Release(stream);
    if (FAILED(hr))
    {
        WARN("task XML rejected, hr %#lx\n", hr);
        free_def(def);
        return SCHED_E_MALFORMEDXML;
    }
    finish_def(def);
    *out = def;
    return S_OK;
}

/* An in-memory UTF-16 document gets a BOM so xmllite does not guess. */
static HRESULT parse_task_wstr(const WCHAR *xml, struct task_def **def)
{
    size_t len = wcslen(xml);
    WCHAR *buf;
    HRESULT hr;

    *def = NULL;
    if (len > MAX_TASK_FILE / sizeof(WCHAR)) return SCHED_E_MALFORMEDXML;
    if (!(buf = malloc((len + 1) * sizeof(WCHAR)))) return E_OUTOFMEMORY;
    buf[0] = 0xfeff;
    memcpy(buf + 1, xml, len * sizeof(WCHAR));
    hr = parse_task_xml(buf, (len + 1) * sizeof(WCHAR), def);
    free(buf);
    return hr;
}

/*
 * Only files owned by LocalSystem, Administrators or TrustedInstaller are
 * loaded: the service writes every registered task itself, so a file some
 * other account dropped into the folder is not a registered task.
 */
static BOOL is_trusted_task_file(const WCHAR *file)
{
    static const WCHAR trusted_installer[] = L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";
    SECURITY_DESCRIPTOR *sd;
    PSID owner = NULL, ti;
    DWORD size = 0;
    BOOL defaulted, ok = FALSE;

    /* FAT has no owners; whoever can write system32 there can write anything */
    if (!tasks_dir_has_acls) return TRUE;

    GetFileSecurityW(file, OWNER_SECURITY_INFORMATION, NULL, 0, &size);
    if (!size || !(sd = malloc(size))) return FALSE;
    if (GetFileSecurityW(file, OWNER_SECURITY_INFORMATION, sd, size, &size) &&
        GetSecurityDescriptorOwner(sd, &owner, &defaulted) && owner)
    {
        if (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid))
            ok = TRUE;
        else if (ConvertStringSidToSidW(trusted_installer, &ti))
        {
            ok = EqualSid(owner, ti);
            LocalFree(ti);
        }
    }
    free(sd);
    if (!ok) WARN("ignoring %s: not owned by SYSTEM, Administrators or TrustedInstaller\n", debugstr_w(file));
    return ok;
}

static HRESULT load_task_file(const WCHAR *file, struct task_def **def)
{
    HANDLE hfile;
    DWORD size, read;
    char *data;
    HRESULT hr;

    *def = NULL;
    hfile = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, 0, NULL);
    if (hfile == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());

    size = GetFileSize(hfile, NULL);
    if (size == INVALID_FILE_SIZE || size > MAX_TASK_FILE)
    {
        CloseHandle(hfile);
        return SCHED_E_MALFORMEDXML;
    }
    if (!(data = malloc(size ? size : 1)))
    {
        CloseHandle(hfile);
        return E_OUTOFMEMORY;
    }
    if (!ReadFile(hfile, data, size, &read, NULL) || read != size)
    {
        free(data);
        CloseHandle(hfile);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(hfile);

    hr = parse_task_xml(data, size, def);
    free(data);
    return hr;
}

/* --------------------------------------------------------------- paths */

BOOL engine_valid_path(const WCHAR *path)
{
    const WCHAR *p, *comp;
    size_t len;

    if (!path) return TRUE;
    if (wcslen(path) >= MAX_PATH) return FALSE;

    for (p = path; *p; p++)
    {
        if (*p == ':' || *p < 0x20 || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|')
            return FALSE;
    }

    comp = path;
    for (;;)
    {
        while (*comp == '\\' || *comp == '/') comp++;
        if (!*comp) break;
        for (p = comp; *p && *p != '\\' && *p != '/'; p++) ;
        len = p - comp;
        if ((len == 1 && comp[0] == '.') || (len == 2 && comp[0] == '.' && comp[1] == '.'))
            return FALSE;
        /* trailing dots and spaces are stripped by Win32, which could alias "..". */
        if (comp[len - 1] == '.' || comp[len - 1] == ' ')
            return FALSE;
        comp = p;
    }
    return TRUE;
}

/* "Folder/Name", "\\Folder\\Name\\" -> "\Folder\Name" */
static WCHAR *normalize_path(const WCHAR *path)
{
    WCHAR *out, *o;
    const WCHAR *p;

    if (!(out = malloc((wcslen(path) + 2) * sizeof(WCHAR)))) return NULL;
    o = out;
    for (p = path; *p; p++)
    {
        WCHAR c = (*p == '/') ? '\\' : *p;
        if (c == '\\' && o > out && o[-1] == '\\') continue;
        if (o == out && c != '\\') *o++ = '\\';
        *o++ = c;
    }
    if (o == out) *o++ = '\\';
    if (o - out > 1 && o[-1] == '\\') o--;
    *o = 0;
    return out;
}

static struct task *find_task(const WCHAR *path)
{
    struct task *task;
    WCHAR *norm;

    if (!(norm = normalize_path(path))) return NULL;
    LIST_FOR_EACH_ENTRY(task, &tasks, struct task, entry)
    {
        if (!wcsicmp(task->path, norm))
        {
            free(norm);
            return task;
        }
    }
    free(norm);
    return NULL;
}

/* --------------------------------------------------------------- state */

static HKEY open_state_key(const WCHAR *path, BOOL create)
{
    WCHAR *name, *norm;
    HKEY key = NULL;
    LONG err;

    if (!(norm = normalize_path(path))) return NULL;
    if (!(name = malloc((wcslen(state_root) + wcslen(norm) + 1) * sizeof(WCHAR))))
    {
        free(norm);
        return NULL;
    }
    wcscpy(name, state_root);
    wcscat(name, norm);
    free(norm);

    if (create)
        err = RegCreateKeyExW(HKEY_LOCAL_MACHINE, name, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL);
    else
        err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, name, 0, KEY_READ | KEY_WRITE, &key);
    free(name);
    return err == ERROR_SUCCESS ? key : NULL;
}

static void state_write_run(const WCHAR *path, ULONGLONG run_time, DWORD result)
{
    HKEY key = open_state_key(path, TRUE);

    if (!key) return;
    if (run_time != NO_TIME)
        RegSetValueExW(key, L"LastRunTime", 0, REG_QWORD, (const BYTE *)&run_time, sizeof(run_time));
    RegSetValueExW(key, L"LastTaskResult", 0, REG_DWORD, (const BYTE *)&result, sizeof(result));
    RegCloseKey(key);
}

static BOOL state_read_run(const WCHAR *path, ULONGLONG *run_time, DWORD *result)
{
    HKEY key = open_state_key(path, FALSE);
    DWORD size, type;
    BOOL ok = FALSE;

    *run_time = NO_TIME;
    *result = SCHED_S_TASK_HAS_NOT_RUN;
    if (!key) return FALSE;

    size = sizeof(*run_time);
    if (!RegQueryValueExW(key, L"LastRunTime", NULL, &type, (BYTE *)run_time, &size) && type == REG_QWORD)
        ok = TRUE;
    else
        *run_time = NO_TIME;
    size = sizeof(*result);
    if (RegQueryValueExW(key, L"LastTaskResult", NULL, &type, (BYTE *)result, &size) || type != REG_DWORD)
        *result = SCHED_S_TASK_HAS_NOT_RUN;
    RegCloseKey(key);
    return ok;
}

static void state_add_missed(const WCHAR *path)
{
    HKEY key = open_state_key(path, TRUE);
    DWORD runs = 0, size = sizeof(runs), type;

    if (!key) return;
    if (RegQueryValueExW(key, L"MissedRuns", NULL, &type, (BYTE *)&runs, &size) || type != REG_DWORD) runs = 0;
    runs++;
    RegSetValueExW(key, L"MissedRuns", 0, REG_DWORD, (const BYTE *)&runs, sizeof(runs));
    RegCloseKey(key);
}

static DWORD state_missed(const WCHAR *path)
{
    HKEY key = open_state_key(path, FALSE);
    DWORD runs = 0, size = sizeof(runs), type;

    if (!key) return 0;
    if (RegQueryValueExW(key, L"MissedRuns", NULL, &type, (BYTE *)&runs, &size) || type != REG_DWORD) runs = 0;
    RegCloseKey(key);
    return runs;
}

/* SID string of whoever registered the task through the RPC interface. */
static WCHAR *state_registrant(const WCHAR *path)
{
    HKEY key = open_state_key(path, FALSE);
    WCHAR buf[192];
    DWORD size = sizeof(buf) - sizeof(WCHAR), type;
    WCHAR *ret = NULL;

    if (!key) return NULL;
    memset(buf, 0, sizeof(buf));
    if (!RegQueryValueExW(key, L"Registrant", NULL, &type, (BYTE *)buf, &size) && type == REG_SZ)
        ret = wcsdup(buf);
    RegCloseKey(key);
    return ret;
}

static void state_delete(const WCHAR *path)
{
    WCHAR *name, *norm;

    if (!(norm = normalize_path(path))) return;
    if ((name = malloc((wcslen(state_root) + wcslen(norm) + 1) * sizeof(WCHAR))))
    {
        wcscpy(name, state_root);
        wcscat(name, norm);
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, name);
        free(name);
    }
    free(norm);
}

/* ---------------------------------------------------------- principals */

static PSID sid_from_account(const WCHAR *account)
{
    WCHAR domain[256];
    DWORD sid_size = 0, domain_size = ARRAYSIZE(domain);
    SID_NAME_USE use;
    PSID sid = NULL, tmp;

    if (!account || !*account) return NULL;

    if ((account[0] == 'S' || account[0] == 's') && account[1] == '-')
    {
        if (!ConvertStringSidToSidW(account, &tmp)) return NULL;
        sid_size = GetLengthSid(tmp);
        if ((sid = malloc(sid_size))) CopySid(sid_size, sid, tmp);
        LocalFree(tmp);
        return sid;
    }

    LookupAccountNameW(NULL, account, NULL, &sid_size, domain, &domain_size, &use);
    if (!sid_size || !(sid = malloc(sid_size))) return NULL;
    domain_size = ARRAYSIZE(domain);
    if (!LookupAccountNameW(NULL, account, sid, &sid_size, domain, &domain_size, &use))
    {
        free(sid);
        return NULL;
    }
    return sid;
}

static PSID copy_sid(PSID src)
{
    DWORD size = GetLengthSid(src);
    PSID sid = malloc(size);

    if (sid) CopySid(size, sid, src);
    return sid;
}

static PSID token_user_sid(HANDLE token)
{
    BYTE buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    DWORD size;

    if (!GetTokenInformation(token, TokenUser, buf, sizeof(buf), &size)) return NULL;
    return copy_sid(((TOKEN_USER *)buf)->User.Sid);
}

static BOOL token_has_group(HANDLE token, PSID group)
{
    TOKEN_GROUPS *groups;
    DWORD size = 0, i;
    BOOL found = FALSE;

    GetTokenInformation(token, TokenGroups, NULL, 0, &size);
    if (!size || !(groups = malloc(size))) return FALSE;
    if (GetTokenInformation(token, TokenGroups, groups, size, &size))
    {
        for (i = 0; i < groups->GroupCount && !found; i++)
        {
            if ((groups->Groups[i].Attributes & SE_GROUP_ENABLED) &&
                !(groups->Groups[i].Attributes & SE_GROUP_USE_FOR_DENY_ONLY) &&
                EqualSid(groups->Groups[i].Sid, group))
                found = TRUE;
        }
    }
    free(groups);
    return found;
}

/*
 * The SID a task runs as, for authorization: the principal UserId, else the
 * registering caller, else LocalSystem (a file an administrator copied into
 * the Tasks folder). A GroupId principal returns NULL with *is_group set.
 */
static PSID task_owner_sid(const WCHAR *path, const struct task_def *def, BOOL *is_group)
{
    WCHAR *registrant;
    PSID sid = NULL;

    *is_group = FALSE;
    if (def && def->user_id && *def->user_id)
        return sid_from_account(def->user_id);
    if (def && def->group_id && *def->group_id)
    {
        *is_group = TRUE;
        return NULL;
    }
    if (path && (registrant = state_registrant(path)))
    {
        sid = sid_from_account(registrant);
        free(registrant);
        if (sid) return sid;
    }
    {
        BYTE buf[SECURITY_MAX_SID_SIZE];
        DWORD size = sizeof(buf);
        if (CreateWellKnownSid(WinLocalSystemSid, NULL, buf, &size)) sid = copy_sid(buf);
    }
    return sid;
}

static HANDLE duplicate_primary(HANDLE token)
{
    HANDLE dup = NULL;

    if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &dup))
        return NULL;
    return dup;
}

static HANDLE service_account_token(const WCHAR *name)
{
    HANDLE token = NULL;

    if (!LogonUserW(name, L"NT AUTHORITY", NULL, LOGON32_LOGON_SERVICE, LOGON32_PROVIDER_DEFAULT, &token))
    {
        ERR("LogonUser(%s) failed, error %lu\n", debugstr_w(name), GetLastError());
        return NULL;
    }
    return token;
}

/* RunLevel HighestAvailable: a filtered admin token runs with its linked full token. */
static HANDLE apply_run_level(HANDLE token, BOOL highest)
{
    TOKEN_ELEVATION_TYPE type;
    TOKEN_LINKED_TOKEN linked;
    DWORD size;
    HANDLE full;

    if (!highest) return token;
    if (!GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size) ||
        type != TokenElevationTypeLimited)
        return token;
    if (!GetTokenInformation(token, TokenLinkedToken, &linked, sizeof(linked), &size))
    {
        WARN("no linked token (error %lu), running with the filtered token\n", GetLastError());
        return token;
    }
    full = duplicate_primary(linked.LinkedToken);
    CloseHandle(linked.LinkedToken);
    if (!full) return token;
    CloseHandle(token);
    return full;
}

/* Logged-on session token of 'user' (or of a member of 'group'); 'prefer' is tried first. */
static HANDLE session_token_for(PSID user, PSID group, DWORD prefer)
{
    WTS_SESSION_INFOW *info = NULL;
    DWORD count = 0, i, pass;
    HANDLE token;

    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &info, &count)) return NULL;

    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < count; i++)
        {
            PSID sid;
            BOOL match;

            if (!info[i].SessionId) continue;
            if (pass == 0 && info[i].SessionId != prefer) continue;
            if (pass == 1 && info[i].SessionId == prefer) continue;
            if (!WTSQueryUserToken(info[i].SessionId, &token)) continue;

            if (group)
                match = token_has_group(token, group);
            else
            {
                sid = token_user_sid(token);
                match = sid && EqualSid(sid, user);
                free(sid);
            }
            if (match)
            {
                WTSFreeMemory(info);
                return token;
            }
            CloseHandle(token);
        }
    }
    WTSFreeMemory(info);
    return NULL;
}

static HRESULT get_run_token(const struct instance *inst, DWORD prefer_session, BOOL force_session,
                             HANDLE *token, BOOL *interactive)
{
    const struct task_def *def = inst->def;
    PSID user = NULL, group = NULL;
    BOOL is_group;
    HANDLE t = NULL, self;
    HRESULT hr = S_OK;

    *token = NULL;
    *interactive = FALSE;

    user = task_owner_sid(inst->path, def, &is_group);
    if (is_group && !(group = sid_from_account(def->group_id)))
        return SCHED_E_ACCOUNT_NAME_NOT_FOUND;
    if (!is_group && !user)
        return SCHED_E_ACCOUNT_NAME_NOT_FOUND;

    if (user && IsWellKnownSid(user, WinLocalSystemSid))
    {
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &self))
        {
            t = duplicate_primary(self);
            CloseHandle(self);
        }
        if (t && force_session)
        {
            if (SetTokenInformation(t, TokenSessionId, &prefer_session, sizeof(prefer_session)))
                *interactive = TRUE;
            else
                WARN("cannot move the SYSTEM token to session %lu, error %lu\n", prefer_session, GetLastError());
        }
        if (!t) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    else if (user && IsWellKnownSid(user, WinLocalServiceSid))
    {
        if (!(t = service_account_token(L"LOCAL SERVICE"))) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    else if (user && IsWellKnownSid(user, WinNetworkServiceSid))
    {
        if (!(t = service_account_token(L"NETWORK SERVICE"))) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    else
    {
        t = session_token_for(user, group, prefer_session);
        if (t)
        {
            *interactive = TRUE;
            t = apply_run_level(t, def->highest);
        }
        else if (def->logon_type == TASK_LOGON_PASSWORD || def->logon_type == TASK_LOGON_S4U ||
                 def->logon_type == TASK_LOGON_INTERACTIVE_TOKEN_OR_PASSWORD)
        {
            FIXME("stored credentials / S4U are not supported; the user must be logged on\n");
            hr = SCHED_E_USER_NOT_LOGGED_ON;
        }
        else
            hr = SCHED_E_USER_NOT_LOGGED_ON;
    }

    free(user);
    free(group);
    if (SUCCEEDED(hr) && !t) hr = E_FAIL;
    *token = t;
    return hr;
}

/* ----------------------------------------------------------- instances */

static void CALLBACK process_exit_callback(void *context, BOOLEAN timeout)
{
    SetEvent(wake_event);
}

static WCHAR *substitute_args(const WCHAR *src, DWORD n_args, WCHAR **args)
{
    size_t len, extra = 0, i;
    const WCHAR *p;
    WCHAR *out, *o;

    if (!src) return NULL;
    for (i = 0; i < n_args; i++) extra += wcslen(args[i]);
    len = wcslen(src) + extra + 1;
    /* each "$(ArgN)" is at least 7 characters, so 'len' is an upper bound */
    if (!(out = malloc(len * sizeof(WCHAR)))) return NULL;

    for (p = src, o = out; *p; )
    {
        if (p[0] == '$' && p[1] == '(' && !wcsncmp(p + 2, L"Arg", 3) && p[5] >= '0' && p[5] <= '9')
        {
            const WCHAR *q = p + 5;
            DWORD n = 0;

            while (*q >= '0' && *q <= '9' && n < 100) n = n * 10 + (*q++ - '0');
            if (*q == ')')
            {
                if (n < n_args)
                {
                    wcscpy(o, args[n]);
                    o += wcslen(args[n]);
                }
                p = q + 1;
                continue;
            }
        }
        *o++ = *p++;
    }
    *o = 0;
    return out;
}

static WCHAR *expand_for_user(HANDLE token, const WCHAR *src)
{
    WCHAR *out;
    DWORD size = 32768;

    if (!src) return NULL;
    if (!(out = malloc(size * sizeof(WCHAR)))) return NULL;
    if (!ExpandEnvironmentStringsForUserW(token, src, out, size))
    {
        if (!ExpandEnvironmentStringsW(src, out, size)) wcscpy(out, src);
    }
    return out;
}

static HRESULT launch_action(struct instance *inst)
{
    const struct action *a = &inst->def->actions[inst->action];
    WCHAR *cmd = NULL, *args = NULL, *dir = NULL, *cmdline = NULL, *sub;
    WCHAR sysdir[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    void *env = NULL;
    size_t len;
    HRESULT hr = S_OK;
    BOOL quote;

    if (!a->command || !*a->command) return SCHED_E_INVALID_TASK;

    if ((sub = substitute_args(a->command, inst->n_args, inst->args)))
    {
        cmd = expand_for_user(inst->token, sub);
        free(sub);
    }
    if (a->arguments && (sub = substitute_args(a->arguments, inst->n_args, inst->args)))
    {
        args = expand_for_user(inst->token, sub);
        free(sub);
    }
    if (a->directory && *a->directory) dir = expand_for_user(inst->token, a->directory);
    if (!cmd)
    {
        hr = E_OUTOFMEMORY;
        goto out;
    }

    quote = (cmd[0] != '"' && wcschr(cmd, ' '));
    len = wcslen(cmd) + (args ? wcslen(args) : 0) + 4;
    if (!(cmdline = malloc(len * sizeof(WCHAR))))
    {
        hr = E_OUTOFMEMORY;
        goto out;
    }
    cmdline[0] = 0;
    if (quote) wcscat(cmdline, L"\"");
    wcscat(cmdline, cmd);
    if (quote) wcscat(cmdline, L"\"");
    if (args && *args)
    {
        wcscat(cmdline, L" ");
        wcscat(cmdline, args);
    }

    if (!dir)
    {
        GetSystemDirectoryW(sysdir, ARRAYSIZE(sysdir));
        dir = wcsdup(sysdir);
    }

    if (!CreateEnvironmentBlock(&env, inst->token, FALSE)) env = NULL;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (inst->interactive) si.lpDesktop = (WCHAR *)L"winsta0\\default";

    TRACE("%s: starting %s as %s\n", debugstr_w(inst->path), debugstr_w(cmdline),
          inst->interactive ? "interactive user" : "service account");

    if (!CreateProcessAsUserW(inst->token, NULL, cmdline, NULL, NULL, FALSE,
                              CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE | CREATE_DEFAULT_ERROR_MODE,
                              env, dir, &si, &pi))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        ERR("%s: CreateProcessAsUser(%s) failed, hr %#lx\n", debugstr_w(inst->path), debugstr_w(cmdline), hr);
        goto out;
    }

    CloseHandle(pi.hThread);
    inst->process = pi.hProcess;
    inst->pid = pi.dwProcessId;
    if (!RegisterWaitForSingleObject(&inst->wait, pi.hProcess, process_exit_callback, NULL, INFINITE,
                                     WT_EXECUTEONLYONCE))
        inst->wait = NULL;

out:
    if (env) DestroyEnvironmentBlock(env);
    free(cmd);
    free(args);
    free(dir);
    free(cmdline);
    return hr;
}

static void free_instance(struct instance *inst)
{
    DWORD i;

    list_remove(&inst->entry);
    if (inst->wait) UnregisterWait(inst->wait);
    if (inst->process) CloseHandle(inst->process);
    if (inst->token) CloseHandle(inst->token);
    for (i = 0; i < inst->n_args; i++) free(inst->args[i]);
    free(inst->args);
    def_release(inst->def);
    free(inst->path);
    free(inst);
}

static DWORD count_instances(const WCHAR *path)
{
    struct instance *inst;
    DWORD n = 0;

    LIST_FOR_EACH_ENTRY(inst, &instances, struct instance, entry)
        if (!wcsicmp(inst->path, path)) n++;
    return n;
}

static void stop_instance(struct instance *inst)
{
    if (inst->process && !inst->stopping)
    {
        TRACE("%s: terminating pid %lu\n", debugstr_w(inst->path), inst->pid);
        TerminateProcess(inst->process, SCHED_S_TASK_TERMINATED);
    }
    inst->stopping = TRUE;
}

/*
 * Starts one instance of 'task'. 'token' (optional) is an already resolved
 * primary token (RUN_FLAG_AS_SELF); it is consumed.
 */
static HRESULT start_instance(struct task *task, DWORD n_args, const WCHAR **args, DWORD session,
                              BOOL force_session, HANDLE token, GUID *guid)
{
    struct instance *inst;
    ULONGLONG now = utc_now();
    HRESULT hr;
    DWORD i;

    if (!task->def->n_actions)
    {
        if (token) CloseHandle(token);
        state_write_run(task->path, now, SCHED_E_INVALID_TASK);
        return SCHED_E_INVALID_TASK;
    }

    if (!(inst = calloc(1, sizeof(*inst))))
    {
        if (token) CloseHandle(token);
        return E_OUTOFMEMORY;
    }
    inst->path = wcsdup(task->path);
    inst->def = task->def;
    def_addref(inst->def);
    inst->start = now;
    inst->deadline = task->def->exec_limit ? now + task->def->exec_limit : NO_TIME;
    UuidCreate(&inst->guid);
    list_add_tail(&instances, &inst->entry);

    if (n_args)
    {
        if (!(inst->args = calloc(n_args, sizeof(WCHAR *))))
        {
            if (token) CloseHandle(token);
            free_instance(inst);
            return E_OUTOFMEMORY;
        }
        for (i = 0; i < n_args; i++)
        {
            if (!(inst->args[i] = wcsdup(args[i] ? args[i] : L"")))
            {
                inst->n_args = i;
                if (token) CloseHandle(token);
                free_instance(inst);
                return E_OUTOFMEMORY;
            }
        }
        inst->n_args = n_args;
    }

    if (token)
    {
        inst->token = token;
        inst->interactive = TRUE;
        hr = S_OK;
    }
    else
        hr = get_run_token(inst, session, force_session, &inst->token, &inst->interactive);

    if (SUCCEEDED(hr)) hr = launch_action(inst);

    if (FAILED(hr))
    {
        WARN("%s: run failed, hr %#lx\n", debugstr_w(task->path), hr);
        state_write_run(task->path, now, hr);
        free_instance(inst);
        return hr;
    }

    state_write_run(task->path, now, SCHED_S_TASK_RUNNING);
    if (guid) *guid = inst->guid;
    return S_OK;
}

static BOOL on_battery(void)
{
    SYSTEM_POWER_STATUS status;

    return GetSystemPowerStatus(&status) && status.ACLineStatus == 0;
}

/* A trigger fired: apply the task settings, then start or queue an instance. */
static void fire_task(struct task *task, const WCHAR *why, DWORD session)
{
    struct instance *inst, *next;
    DWORD running;

    if (!task->def->enabled) return;
    if (task->def->disallow_on_batteries && on_battery())
    {
        TRACE("%s: on batteries, not started\n", debugstr_w(task->path));
        return;
    }

    running = count_instances(task->path);
    TRACE("%s: %s trigger, %lu running\n", debugstr_w(task->path), debugstr_w(why), running);
    if (running)
    {
        switch (task->def->policy)
        {
        case POLICY_IGNORE_NEW:
            return;
        case POLICY_QUEUE:
            task->queued++;
            return;
        case POLICY_STOP_EXISTING:
            LIST_FOR_EACH_ENTRY_SAFE(inst, next, &instances, struct instance, entry)
                if (!wcsicmp(inst->path, task->path)) stop_instance(inst);
            break;
        case POLICY_PARALLEL:
            break;
        }
    }
    start_instance(task, 0, NULL, session, FALSE, NULL, NULL);
}

/* Reap finished processes, run the next action, enforce ExecutionTimeLimit. */
static void process_instances(ULONGLONG now)
{
    struct instance *inst, *next;
    struct task *task;
    DWORD code;
    HRESULT hr;

    LIST_FOR_EACH_ENTRY_SAFE(inst, next, &instances, struct instance, entry)
    {
        if (inst->deadline != NO_TIME && now >= inst->deadline && !inst->stopping)
        {
            WARN("%s: ExecutionTimeLimit reached\n", debugstr_w(inst->path));
            stop_instance(inst);
        }

        if (WaitForSingleObject(inst->process, 0) != WAIT_OBJECT_0) continue;

        if (!GetExitCodeProcess(inst->process, &code)) code = GetLastError();
        if (inst->wait)
        {
            UnregisterWait(inst->wait);
            inst->wait = NULL;
        }
        CloseHandle(inst->process);
        inst->process = NULL;

        if (!inst->stopping && inst->action + 1 < inst->def->n_actions)
        {
            inst->action++;
            hr = launch_action(inst);
            if (SUCCEEDED(hr)) continue;
            code = hr;
        }

        TRACE("%s: finished, result %#lx\n", debugstr_w(inst->path), code);
        state_write_run(inst->path, NO_TIME, inst->stopping ? (DWORD)SCHED_S_TASK_TERMINATED : code);

        task = find_task(inst->path);
        free_instance(inst);
        if (task && task->queued && !count_instances(task->path))
        {
            task->queued--;
            start_instance(task, 0, NULL, 0, FALSE, NULL, NULL);
        }
    }
}

/* ---------------------------------------------------------- task store */

static void free_task(struct task *task)
{
    list_remove(&task->entry);
    def_release(task->def);
    free(task->path);
    free(task->file);
    free(task);
}

static void init_trigger_times(struct task *task, ULONGLONG now, BOOL at_startup)
{
    ULONGLONG last_run, created = ft_to_u64(&task->create_time), prev, prev_utc;
    DWORD i, result;
    BOOL missed = FALSE;

    state_read_run(task->path, &last_run, &result);

    for (i = 0; i < task->def->n_triggers; i++)
    {
        struct trigger *t = &task->def->triggers[i];

        t->pending = t->activated = NO_TIME;
        t->next = NO_TIME;
        if (!t->enabled) continue;
        if (t->type != TRIG_TIME && t->type != TRIG_CALENDAR) continue;

        t->next = trigger_next_time(t, now);

        if (!at_startup) continue;
        prev = base_prev(t, utc_to_lt(now));
        if (prev == NO_TIME) continue;
        prev_utc = lt_to_utc(prev);
        if (prev_utc > created && (last_run == NO_TIME || prev_utc > last_run) &&
            (!t->has_end || prev_utc <= t->end_utc))
            missed = TRUE;
    }

    if (missed && task->def->enabled)
    {
        if (task->def->start_when_available)
            fire_task(task, L"StartWhenAvailable", 0);
        else
            state_add_missed(task->path);
    }
}

static void activate_event_triggers(struct task *task, enum trigger_type type, ULONGLONG now, PSID logon_user)
{
    DWORD i;

    for (i = 0; i < task->def->n_triggers; i++)
    {
        struct trigger *t = &task->def->triggers[i];

        if (t->type != type || !t->enabled) continue;
        if (t->has_start && now < t->start_utc) continue;
        if (t->has_end && now > t->end_utc) continue;
        if (type == TRIG_LOGON && t->user_id && *t->user_id)
        {
            PSID sid = sid_from_account(t->user_id);
            BOOL match = sid && logon_user && EqualSid(sid, logon_user);

            free(sid);
            if (!match) continue;
        }
        t->activated = now + t->delay;
        t->pending = now + t->delay;
        t->next = NO_TIME;
    }
}

static void scan_folder(const WCHAR *dir, const WCHAR *rel, int depth, ULONGLONG now, BOOL at_startup)
{
    WIN32_FIND_DATAW data;
    WCHAR *pattern, *child, *child_rel;
    HANDLE handle;
    struct task *task;
    size_t dlen = wcslen(dir), rlen = wcslen(rel);

    if (depth > MAX_TASK_DEPTH) return;
    if (!(pattern = malloc((dlen + 3) * sizeof(WCHAR)))) return;
    wcscpy(pattern, dir);
    wcscat(pattern, L"\\*");
    handle = FindFirstFileW(pattern, &data);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) return;

    do
    {
        size_t nlen = wcslen(data.cFileName);

        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        child = malloc((dlen + nlen + 2) * sizeof(WCHAR));
        child_rel = malloc((rlen + nlen + 2) * sizeof(WCHAR));
        if (!child || !child_rel)
        {
            free(child);
            free(child_rel);
            continue;
        }
        wcscpy(child, dir);
        wcscat(child, L"\\");
        wcscat(child, data.cFileName);
        wcscpy(child_rel, rel);
        wcscat(child_rel, L"\\");
        wcscat(child_rel, data.cFileName);

        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            scan_folder(child, child_rel, depth + 1, now, at_startup);
            free(child);
            free(child_rel);
            continue;
        }

        task = find_task(child_rel);
        if (task && !CompareFileTime(&task->write_time, &data.ftLastWriteTime) &&
            task->size == (((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow))
        {
            task->seen = TRUE;
            free(child);
            free(child_rel);
            continue;
        }

        {
            struct task_def *def;

            if (!is_trusted_task_file(child) || FAILED(load_task_file(child, &def)))
            {
                WARN("skipping unreadable task %s\n", debugstr_w(child));
                if (task) task->seen = TRUE;
                free(child);
                free(child_rel);
                continue;
            }

            if (!task)
            {
                if (!(task = calloc(1, sizeof(*task))))
                {
                    def_release(def);
                    free(child);
                    free(child_rel);
                    continue;
                }
                task->path = normalize_path(child_rel);
                task->file = child;
                child = NULL;
                list_add_tail(&tasks, &task->entry);
            }
            else
                def_release(task->def);

            task->def = def;
            task->write_time = data.ftLastWriteTime;
            task->create_time = data.ftCreationTime;
            task->size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
            task->seen = TRUE;
            TRACE("loaded task %s: %lu triggers, %lu actions\n",
                  debugstr_w(task->path), def->n_triggers, def->n_actions);

            init_trigger_times(task, now, at_startup);
            if (!at_startup) activate_event_triggers(task, TRIG_REGISTRATION, now, NULL);
            if (at_startup && GetTickCount64() < BOOT_WINDOW_MS)
                activate_event_triggers(task, TRIG_BOOT, now, NULL);
        }
        free(child);
        free(child_rel);
    } while (FindNextFileW(handle, &data));

    FindClose(handle);
}

static void rescan_tasks(ULONGLONG now, BOOL at_startup)
{
    struct task *task, *next;

    LIST_FOR_EACH_ENTRY(task, &tasks, struct task, entry)
        task->seen = FALSE;

    scan_folder(tasks_dir, L"", 0, now, at_startup);

    LIST_FOR_EACH_ENTRY_SAFE(task, next, &tasks, struct task, entry)
    {
        if (task->seen) continue;
        TRACE("task %s removed\n", debugstr_w(task->path));
        free_task(task);
    }
}

/* ------------------------------------------------------ logon and idle */

static void check_logons(ULONGLONG now, BOOL initial)
{
    WTS_SESSION_INFOW *info = NULL;
    struct seen_logon *seen, *next;
    struct task *task;
    DWORD count = 0, i;

    LIST_FOR_EACH_ENTRY(seen, &logons, struct seen_logon, entry)
        seen->present = FALSE;

    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &info, &count))
    {
        for (i = 0; i < count; i++)
        {
            TOKEN_STATISTICS stats;
            HANDLE token;
            DWORD size;
            BOOL known = FALSE;

            if (!info[i].SessionId) continue;
            if (!WTSQueryUserToken(info[i].SessionId, &token)) continue;
            if (!GetTokenInformation(token, TokenStatistics, &stats, sizeof(stats), &size))
            {
                CloseHandle(token);
                continue;
            }

            LIST_FOR_EACH_ENTRY(seen, &logons, struct seen_logon, entry)
            {
                if (seen->luid.LowPart == stats.AuthenticationId.LowPart &&
                    seen->luid.HighPart == stats.AuthenticationId.HighPart)
                {
                    seen->present = known = TRUE;
                    break;
                }
            }

            if (!known && (seen = calloc(1, sizeof(*seen))))
            {
                PSID user = token_user_sid(token);

                seen->luid = stats.AuthenticationId;
                seen->present = TRUE;
                list_add_tail(&logons, &seen->entry);
                if (!initial)
                {
                    TRACE("new logon in session %lu\n", info[i].SessionId);
                    LIST_FOR_EACH_ENTRY(task, &tasks, struct task, entry)
                        activate_event_triggers(task, TRIG_LOGON, now, user);
                }
                free(user);
            }
            CloseHandle(token);
        }
        WTSFreeMemory(info);
    }

    LIST_FOR_EACH_ENTRY_SAFE(seen, next, &logons, struct seen_logon, entry)
    {
        if (seen->present) continue;
        list_remove(&seen->entry);
        free(seen);
    }
}

static ULONGLONG ft_ticks(const FILETIME *ft)
{
    return ft_to_u64(ft);
}

/* Idle = no input for IDLE_INPUT_MS and an idle CPU since the last poll. */
static void check_idle(ULONGLONG now)
{
    LASTINPUTINFO lii;
    FILETIME idle, kernel, user;
    ULONGLONG idle_t, total_t, d_idle, d_total;
    BOOL input_idle, cpu_idle = FALSE;
    struct task *task;

    lii.cbSize = sizeof(lii);
    if (!GetLastInputInfo(&lii)) return;
    input_idle = (GetTickCount() - lii.dwTime) >= IDLE_INPUT_MS;

    if (GetSystemTimes(&idle, &kernel, &user))
    {
        idle_t = ft_ticks(&idle);
        total_t = ft_ticks(&kernel) + ft_ticks(&user);
        d_idle = idle_t - cpu_idle_prev;
        d_total = total_t - cpu_total_prev;
        if (cpu_total_prev && d_total) cpu_idle = (d_idle * 100 / d_total) >= IDLE_CPU_PCT;
        cpu_idle_prev = idle_t;
        cpu_total_prev = total_t;
    }

    if (!input_idle)
    {
        idle_fired = FALSE;
        return;
    }
    if (idle_fired || !cpu_idle) return;

    idle_fired = TRUE;
    TRACE("system is idle\n");
    LIST_FOR_EACH_ENTRY(task, &tasks, struct task, entry)
        activate_event_triggers(task, TRIG_IDLE, now, NULL);
}

/* ---------------------------------------------------------- main loop */

static void run_due_triggers(ULONGLONG now)
{
    struct task *task;
    DWORD i;

    LIST_FOR_EACH_ENTRY(task, &tasks, struct task, entry)
    {
        BOOL fire = FALSE;
        const WCHAR *why = NULL;

        for (i = 0; i < task->def->n_triggers; i++)
        {
            struct trigger *t = &task->def->triggers[i];

            if (!t->enabled) continue;
            if (t->type == TRIG_TIME || t->type == TRIG_CALENDAR)
            {
                if (t->next == NO_TIME || t->next > now) continue;
                t->next = trigger_next_time(t, now);
                fire = TRUE;
                why = L"time";
            }
            else if (t->pending != NO_TIME && t->pending <= now)
            {
                t->pending = NO_TIME;
                t->next = repetition_next(t, t->activated, now);
                fire = TRUE;
                why = L"event";
            }
            else if (t->pending == NO_TIME && t->next != NO_TIME && t->next <= now)
            {
                t->next = repetition_next(t, t->activated, now);
                fire = TRUE;
                why = L"repetition";
            }
            if (t->type != TRIG_TIME && t->type != TRIG_CALENDAR &&
                t->has_end && t->next != NO_TIME && t->next > t->end_utc)
                t->next = NO_TIME;
        }
        if (fire) fire_task(task, why, 0);
    }
}

static ULONGLONG next_wakeup(ULONGLONG now)
{
    struct task *task;
    struct instance *inst;
    ULONGLONG best = NO_TIME;
    DWORD i;

    LIST_FOR_EACH_ENTRY(task, &tasks, struct task, entry)
    {
        for (i = 0; i < task->def->n_triggers; i++)
        {
            const struct trigger *t = &task->def->triggers[i];

            if (!t->enabled) continue;
            if (t->type == TRIG_TIME || t->type == TRIG_CALENDAR)
                best = min_time(best, t->next);
            else
                best = min_time(best, trigger_next_time(t, now));
        }
    }
    LIST_FOR_EACH_ENTRY(inst, &instances, struct instance, entry)
        if (!inst->stopping) best = min_time(best, inst->deadline);
    return best;
}

static DWORD WINAPI engine_main(void *arg)
{
    HANDLE change, events[3];
    ULONGLONG now, next;
    DWORD timeout, n, ret;
    BOOL startup = TRUE;

    TRACE("engine started, tasks in %s\n", debugstr_w(tasks_dir));

    CreateDirectoryW(tasks_dir, NULL);
    change = FindFirstChangeNotificationW(tasks_dir, TRUE,
                                          FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                          FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    if (change == INVALID_HANDLE_VALUE)
    {
        WARN("no change notification on %s, polling\n", debugstr_w(tasks_dir));
        change = NULL;
    }

    for (;;)
    {
        now = utc_now();

        EnterCriticalSection(&engine_cs);
        if (rescan_needed || startup)
        {
            rescan_needed = FALSE;
            rescan_tasks(now, startup);
        }
        if (startup || logon_check_needed)
        {
            logon_check_needed = FALSE;
            /* a logon during the boot window predates the service: count it as new */
            check_logons(now, startup && GetTickCount64() >= BOOT_WINDOW_MS);
        }
        check_idle(now);
        process_instances(now);
        run_due_triggers(now);
        next = next_wakeup(utc_now());
        LeaveCriticalSection(&engine_cs);
        startup = FALSE;

        now = utc_now();
        if (next == NO_TIME || next <= now)
            timeout = (next == NO_TIME) ? POLL_MS : 0;
        else
            timeout = (DWORD)min((next - now) / 10000 + 1, (ULONGLONG)POLL_MS);

        n = 0;
        events[n++] = stop_event;
        events[n++] = wake_event;
        if (change) events[n++] = change;

        ret = WaitForMultipleObjects(n, events, FALSE, timeout);
        if (ret == WAIT_OBJECT_0) break;
        if (change && ret == WAIT_OBJECT_0 + 2)
        {
            EnterCriticalSection(&engine_cs);
            rescan_needed = TRUE;
            LeaveCriticalSection(&engine_cs);
            if (!FindNextChangeNotification(change))
            {
                FindCloseChangeNotification(change);
                change = NULL;
            }
        }
        if (ret == WAIT_TIMEOUT)
        {
            /* also catches logons that bypassed the Winlogon notification */
            EnterCriticalSection(&engine_cs);
            logon_check_needed = TRUE;
            if (!change) rescan_needed = TRUE;
            LeaveCriticalSection(&engine_cs);
        }
    }

    if (change) FindCloseChangeNotification(change);
    TRACE("engine stopped\n");
    return 0;
}

/* ------------------------------------------------------------- public */

void engine_start(void)
{
    DWORD tid;

    GetSystemDirectoryW(tasks_dir, ARRAYSIZE(tasks_dir) - 8);
    wcscat(tasks_dir, L"\\Tasks");

    {
        WCHAR root[4] = { tasks_dir[0], ':', '\\', 0 };
        DWORD fs_flags = 0;

        if (GetVolumeInformationW(root, NULL, 0, NULL, NULL, &fs_flags, NULL, 0) &&
            !(fs_flags & FS_PERSISTENT_ACLS))
        {
            WARN("%s has no ACLs: task file owners are not checked\n", debugstr_w(root));
            tasks_dir_has_acls = FALSE;
        }
    }

    wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!wake_event || !stop_event)
    {
        ERR("cannot create engine events\n");
        return;
    }
    engine_thread = CreateThread(NULL, 0, engine_main, NULL, 0, &tid);
    if (!engine_thread) ERR("cannot start the task engine, error %lu\n", GetLastError());
}

void engine_stop(void)
{
    struct instance *inst, *next_inst;
    struct task *task, *next_task;
    struct seen_logon *seen, *next_seen;

    if (engine_thread)
    {
        SetEvent(stop_event);
        WaitForSingleObject(engine_thread, INFINITE);
        CloseHandle(engine_thread);
        engine_thread = NULL;
    }

    EnterCriticalSection(&engine_cs);
    /* Running task processes outlive the service, as on Windows. */
    LIST_FOR_EACH_ENTRY_SAFE(inst, next_inst, &instances, struct instance, entry)
        free_instance(inst);
    LIST_FOR_EACH_ENTRY_SAFE(task, next_task, &tasks, struct task, entry)
        free_task(task);
    LIST_FOR_EACH_ENTRY_SAFE(seen, next_seen, &logons, struct seen_logon, entry)
    {
        list_remove(&seen->entry);
        free(seen);
    }
    LeaveCriticalSection(&engine_cs);
}

void engine_notify_logon(void)
{
    EnterCriticalSection(&engine_cs);
    logon_check_needed = TRUE;
    LeaveCriticalSection(&engine_cs);
    if (wake_event) SetEvent(wake_event);
}

void engine_notify_changed(void)
{
    EnterCriticalSection(&engine_cs);
    rescan_needed = TRUE;
    LeaveCriticalSection(&engine_cs);
    if (wake_event) SetEvent(wake_event);
}

/* ------------------------------------------------------- authorization */

struct caller
{
    PSID sid;
    BOOL admin;
    HANDLE token;   /* impersonation token */
};

static void free_caller(struct caller *c)
{
    free(c->sid);
    if (c->token) CloseHandle(c->token);
}

static HRESULT get_caller(struct caller *c)
{
    BYTE buf[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(buf);
    BOOL member = FALSE;
    RPC_STATUS status;

    memset(c, 0, sizeof(*c));
    status = RpcImpersonateClient(NULL);
    if (status != RPC_S_OK)
    {
        ERR("RpcImpersonateClient failed, status %ld\n", status);
        return E_ACCESSDENIED;
    }
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, &c->token))
        c->token = NULL;
    RpcRevertToSelf();
    if (!c->token) return E_ACCESSDENIED;

    if (!(c->sid = token_user_sid(c->token)))
    {
        free_caller(c);
        return E_ACCESSDENIED;
    }
    if (IsWellKnownSid(c->sid, WinLocalSystemSid))
        c->admin = TRUE;
    else if (CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, buf, &size) &&
             CheckTokenMembership(c->token, buf, &member) && member)
        c->admin = TRUE;
    return S_OK;
}

/* The caller must be an administrator or the account the task runs as. */
static HRESULT check_owner(const struct caller *c, const WCHAR *path, const struct task_def *def)
{
    BOOL is_group;
    PSID owner;
    HRESULT hr;

    if (c->admin) return S_OK;
    owner = task_owner_sid(path, def, &is_group);
    hr = (owner && EqualSid(owner, c->sid)) ? S_OK : E_ACCESSDENIED;
    free(owner);
    return hr;
}

static struct task_def *load_def_for(const WCHAR *path)
{
    struct task *task;
    struct task_def *def = NULL;
    WCHAR *file, *norm;

    EnterCriticalSection(&engine_cs);
    if ((task = find_task(path)))
    {
        def = task->def;
        def_addref(def);
    }
    LeaveCriticalSection(&engine_cs);
    if (def) return def;

    /* not loaded yet (just written, or the scan has not run): read the file */
    if (!(norm = normalize_path(path))) return NULL;
    if ((file = malloc((wcslen(tasks_dir) + wcslen(norm) + 1) * sizeof(WCHAR))))
    {
        wcscpy(file, tasks_dir);
        wcscat(file, norm);
        if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES &&
            !(GetFileAttributesW(file) & FILE_ATTRIBUTE_DIRECTORY))
            load_task_file(file, &def);
        free(file);
    }
    free(norm);
    return def;
}

/*
 * SchRpcRegisterTask: a non-administrator may only register (or replace) a
 * task that runs as itself. On success *registrant receives the caller SID
 * string for engine_task_registered.
 */
HRESULT engine_check_register(const WCHAR *path, const WCHAR *xml, DWORD flags, WCHAR **registrant)
{
    struct task_def *def = NULL, *old;
    struct caller c;
    HRESULT hr;
    BOOL is_group;
    PSID owner;

    *registrant = NULL;
    if (!engine_valid_path(path)) return E_INVALIDARG;
    if (FAILED(hr = get_caller(&c))) return hr;

    if (!ConvertSidToStringSidW(c.sid, registrant)) *registrant = NULL;

    if (!c.admin)
    {
        if (!xml || FAILED(parse_task_wstr(xml, &def)))
        {
            hr = E_ACCESSDENIED;
            goto out;
        }
        owner = task_owner_sid(NULL, def, &is_group);
        /*
         * HighestAvailable needs an elevated registration, as on Windows.
         * Without a UserId the task runs as its registrant, i.e. the caller.
         */
        if (def->highest)
            hr = E_ACCESSDENIED;
        else if (!def->user_id || !*def->user_id)
            hr = is_group ? E_ACCESSDENIED : S_OK;
        else
            hr = (owner && EqualSid(owner, c.sid)) ? S_OK : E_ACCESSDENIED;
        free(owner);

        if (hr == S_OK && path && (flags & TASK_UPDATE) && (old = load_def_for(path)))
        {
            hr = check_owner(&c, path, old);
            def_release(old);
        }
    }

out:
    if (def) def_release(def);
    free_caller(&c);
    if (FAILED(hr))
    {
        LocalFree(*registrant);
        *registrant = NULL;
        WARN("register %s denied\n", debugstr_w(path));
    }
    return hr;
}

void engine_task_registered(const WCHAR *path, WCHAR *registrant)
{
    HKEY key;

    if (registrant && (key = open_state_key(path, TRUE)))
    {
        RegSetValueExW(key, L"Registrant", 0, REG_SZ, (const BYTE *)registrant,
                       (wcslen(registrant) + 1) * sizeof(WCHAR));
        RegCloseKey(key);
    }
    LocalFree(registrant);
    engine_notify_changed();
}

/* Delete, enable/disable, run and stop need the administrator or the task owner. */
HRESULT engine_check_task_access(const WCHAR *path)
{
    struct task_def *def;
    struct caller c;
    HRESULT hr;

    if (!engine_valid_path(path)) return E_INVALIDARG;
    if (FAILED(hr = get_caller(&c))) return hr;
    if (c.admin)
    {
        free_caller(&c);
        return S_OK;
    }
    def = path ? load_def_for(path) : NULL;
    hr = def ? check_owner(&c, path, def) : E_ACCESSDENIED;
    if (def) def_release(def);
    free_caller(&c);
    return hr;
}

void engine_task_deleted(const WCHAR *path)
{
    state_delete(path);
    engine_notify_changed();
}

/* ----------------------------------------------------------- RPC hooks */

HRESULT engine_run(const WCHAR *path, DWORD n_args, const WCHAR **args, DWORD flags, DWORD session_id,
                   const WCHAR *user, GUID *guid)
{
    struct task *task;
    struct caller c;
    HANDLE token = NULL;
    HRESULT hr;

    memset(guid, 0, sizeof(*guid));
    if (!engine_valid_path(path)) return E_INVALIDARG;
    if (user && *user) FIXME("run as %s is not supported, ignored\n", debugstr_w(user));
    if (n_args > 32) return E_INVALIDARG;

    if (FAILED(hr = engine_check_task_access(path))) return hr;

    if (flags & RUN_FLAG_AS_SELF)
    {
        if (FAILED(hr = get_caller(&c))) return hr;
        token = duplicate_primary(c.token);
        free_caller(&c);
        if (!token) return HRESULT_FROM_WIN32(GetLastError());
    }

    EnterCriticalSection(&engine_cs);
    rescan_tasks(utc_now(), FALSE);
    if (!(task = find_task(path)))
        hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    else if (!(flags & RUN_FLAG_IGNORE_CONSTRAINTS) && !task->def->enabled)
        hr = SCHED_E_TASK_DISABLED;
    else if (!(flags & RUN_FLAG_IGNORE_CONSTRAINTS) && !task->def->allow_demand_start)
        hr = SCHED_E_START_ON_DEMAND;
    else if (!(flags & RUN_FLAG_IGNORE_CONSTRAINTS) && task->def->policy == POLICY_IGNORE_NEW &&
             count_instances(task->path))
        hr = SCHED_E_ALREADY_RUNNING;
    else
    {
        hr = start_instance(task, n_args, args, session_id, (flags & RUN_FLAG_USE_SESSION_ID) != 0, token, guid);
        token = NULL;
    }
    LeaveCriticalSection(&engine_cs);

    if (token) CloseHandle(token);
    if (wake_event) SetEvent(wake_event);
    return hr;
}

HRESULT engine_stop_task(const WCHAR *path)
{
    struct instance *inst;
    WCHAR *norm;
    HRESULT hr;

    /* a NULL path stops every task, which only an administrator may do */
    if (FAILED(hr = engine_check_task_access(path))) return hr;

    norm = path ? normalize_path(path) : NULL;
    if (path && !norm) return E_OUTOFMEMORY;

    EnterCriticalSection(&engine_cs);
    LIST_FOR_EACH_ENTRY(inst, &instances, struct instance, entry)
    {
        if (norm && wcsicmp(inst->path, norm)) continue;
        stop_instance(inst);
    }
    LeaveCriticalSection(&engine_cs);
    free(norm);

    if (wake_event) SetEvent(wake_event);
    return S_OK;
}

HRESULT engine_stop_instance(const GUID *guid)
{
    struct instance *inst;
    WCHAR *path = NULL;
    HRESULT hr;

    EnterCriticalSection(&engine_cs);
    LIST_FOR_EACH_ENTRY(inst, &instances, struct instance, entry)
    {
        if (IsEqualGUID(&inst->guid, guid))
        {
            path = wcsdup(inst->path);
            break;
        }
    }
    LeaveCriticalSection(&engine_cs);
    if (!path) return SCHED_E_TASK_NOT_RUNNING;

    hr = engine_check_task_access(path);
    if (SUCCEEDED(hr))
    {
        EnterCriticalSection(&engine_cs);
        LIST_FOR_EACH_ENTRY(inst, &instances, struct instance, entry)
        {
            if (IsEqualGUID(&inst->guid, guid))
            {
                stop_instance(inst);
                break;
            }
        }
        LeaveCriticalSection(&engine_cs);
        if (wake_event) SetEvent(wake_event);
    }
    free(path);
    return hr;
}

HRESULT engine_enum_instances(const WCHAR *path, DWORD *n_guids, GUID **guids)
{
    struct instance *inst;
    WCHAR *norm = NULL;
    DWORD n = 0;

    *n_guids = 0;
    *guids = NULL;
    if (!engine_valid_path(path)) return E_INVALIDARG;
    if (path && *path && !(norm = normalize_path(path))) return E_OUTOFMEMORY;
    /* "\" means every task */
    if (norm && !wcscmp(norm, L"\\"))
    {
        free(norm);
        norm = NULL;
    }

    EnterCriticalSection(&engine_cs);
    LIST_FOR_EACH_ENTRY(inst, &instances, struct instance, entry)
        if (!norm || !wcsicmp(inst->path, norm)) n++;
    if (n && (*guids = MIDL_user_allocate(n * sizeof(GUID))))
    {
        LIST_FOR_EACH_ENTRY(inst, &instances, struct instance, entry)
            if (!norm || !wcsicmp(inst->path, norm)) (*guids)[(*n_guids)++] = inst->guid;
    }
    LeaveCriticalSection(&engine_cs);
    free(norm);

    if (n && !*guids) return E_OUTOFMEMORY;
    return S_OK;
}

HRESULT engine_instance_info(const GUID *guid, WCHAR **path, DWORD *state, WCHAR **action, DWORD *pid)
{
    struct instance *inst;
    HRESULT hr = SCHED_E_TASK_NOT_RUNNING;

    *path = *action = NULL;
    *state = TASK_STATE_UNKNOWN;
    *pid = 0;

    EnterCriticalSection(&engine_cs);
    LIST_FOR_EACH_ENTRY(inst, &instances, struct instance, entry)
    {
        const WCHAR *cmd;

        if (!IsEqualGUID(&inst->guid, guid)) continue;
        cmd = inst->def->actions[inst->action].command;
        *path = MIDL_user_allocate((wcslen(inst->path) + 1) * sizeof(WCHAR));
        if (*path) wcscpy(*path, inst->path);
        if (cmd && (*action = MIDL_user_allocate((wcslen(cmd) + 1) * sizeof(WCHAR))))
            wcscpy(*action, cmd);
        *state = TASK_STATE_RUNNING;
        *pid = inst->pid;
        hr = *path ? S_OK : E_OUTOFMEMORY;
        break;
    }
    LeaveCriticalSection(&engine_cs);
    return hr;
}

HRESULT engine_last_run(const WCHAR *path, SYSTEMTIME *last, DWORD *code)
{
    ULONGLONG when;

    memset(last, 0, sizeof(*last));
    *code = SCHED_S_TASK_HAS_NOT_RUN;
    if (!engine_valid_path(path)) return E_INVALIDARG;

    if (!state_read_run(path, &when, code) || when == NO_TIME)
        return S_OK;
    utc_to_local_systemtime(when, last);
    return S_OK;
}

HRESULT engine_missed_runs(const WCHAR *path, DWORD *runs)
{
    if (!engine_valid_path(path)) return E_INVALIDARG;
    *runs = state_missed(path);
    return S_OK;
}

DWORD engine_task_state(const WCHAR *path, BOOL enabled)
{
    struct task *task;
    DWORD state;

    if (!enabled) return TASK_STATE_DISABLED;

    EnterCriticalSection(&engine_cs);
    task = find_task(path);
    if (task && count_instances(task->path))
        state = TASK_STATE_RUNNING;
    else if (task && task->queued)
        state = TASK_STATE_QUEUED;
    else
        state = TASK_STATE_READY;
    LeaveCriticalSection(&engine_cs);
    return state;
}

/* SchRpcScheduledRuntimes: time and calendar activations in [start, end]. */
HRESULT engine_runtimes(const WCHAR *path, const SYSTEMTIME *start, const SYSTEMTIME *end, DWORD n_requested,
                        DWORD *n_runtimes, SYSTEMTIME **runtimes)
{
    struct task_def *def;
    ULONGLONG from, to, cur, best, *last;
    DWORD i, count = 0, max;

    *n_runtimes = 0;
    *runtimes = NULL;
    if (!engine_valid_path(path)) return E_INVALIDARG;

    from = utc_now();
    if (start && !local_systemtime_to_utc(start, &from)) return E_INVALIDARG;
    to = ~(ULONGLONG)0;
    if (end && !local_systemtime_to_utc(end, &to)) return E_INVALIDARG;

    if (!(def = load_def_for(path))) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    max = n_requested ? min(n_requested, 1000u) : 1000u;
    if (!(*runtimes = MIDL_user_allocate(max * sizeof(SYSTEMTIME))) ||
        !(last = calloc(def->n_triggers ? def->n_triggers : 1, sizeof(*last))))
    {
        MIDL_user_free(*runtimes);
        *runtimes = NULL;
        def_release(def);
        return E_OUTOFMEMORY;
    }

    /* 'from' itself counts as a run time: search after from - 1 */
    for (i = 0; i < def->n_triggers; i++) last[i] = from ? from - 1 : 0;

    while (count < max)
    {
        best = NO_TIME;
        for (i = 0; i < def->n_triggers; i++)
        {
            const struct trigger *t = &def->triggers[i];

            if (t->type != TRIG_TIME && t->type != TRIG_CALENDAR) continue;
            if (last[i] == NO_TIME) continue;
            cur = trigger_next_time(t, last[i]);
            if (cur == NO_TIME) last[i] = NO_TIME;
            best = min_time(best, cur);
        }
        if (best == NO_TIME || best > to) break;
        utc_to_local_systemtime(best, &(*runtimes)[count++]);
        for (i = 0; i < def->n_triggers; i++)
            if (last[i] != NO_TIME && last[i] < best) last[i] = best;
    }

    free(last);
    def_release(def);
    *n_runtimes = count;
    if (!count)
    {
        MIDL_user_free(*runtimes);
        *runtimes = NULL;
        return S_FALSE;
    }
    return S_OK;
}
