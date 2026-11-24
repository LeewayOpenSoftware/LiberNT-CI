/*
 * Copyright 2012 Detlef Riekenberg
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

#define COBJMACROS

#include <stdio.h>
#include <stdlib.h>

#include "initguid.h"
#include "taskschd.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(schtasks);

static ITaskFolder *get_tasks_root_folder(void)
{
    ITaskService *service;
    ITaskFolder *root;
    VARIANT empty;
    HRESULT hres;

    hres = CoCreateInstance(&CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER,
                            &IID_ITaskService, (void**)&service);
    if (FAILED(hres))
        return NULL;

    V_VT(&empty) = VT_EMPTY;
    hres = ITaskService_Connect(service, empty, empty, empty, empty);
    if (FAILED(hres)) {
        FIXME("Connect failed: %08lx\n", hres);
        return NULL;
    }

    hres = ITaskService_GetFolder(service, NULL, &root);
    ITaskService_Release(service);
    if (FAILED(hres)) {
        FIXME("GetFolder failed: %08lx\n", hres);
        return NULL;
    }

    return root;
}

static IRegisteredTask *get_registered_task(const WCHAR *name)
{
    IRegisteredTask *registered_task;
    ITaskFolder *root;
    BSTR str;
    HRESULT hres;

    root = get_tasks_root_folder();
    if (!root)
        return NULL;

    str = SysAllocString(name);
    hres = ITaskFolder_GetTask(root, str, &registered_task);
    SysFreeString(str);
    ITaskFolder_Release(root);
    if (FAILED(hres)) {
        FIXME("GetTask failed: %08lx\n", hres);
        return NULL;
    }

    return registered_task;
}

static BSTR read_file_to_bstr(const WCHAR *file_name)
{
    LARGE_INTEGER file_size;
    DWORD read_size, size;
    unsigned char *data;
    HANDLE file;
    BOOL r = FALSE;
    BSTR ret;

    file = CreateFileW(file_name, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        FIXME("Could not open file\n");
        return NULL;
    }

    if (!GetFileSizeEx(file, &file_size) || !file_size.QuadPart) {
        FIXME("Empty file\n");
        CloseHandle(file);
        return NULL;
    }

    data = HeapAlloc(GetProcessHeap(), 0, file_size.QuadPart);
    if (data)
        r = ReadFile(file, data, file_size.QuadPart, &read_size, NULL);
    CloseHandle(file);
    if (!r) {
        FIXME("Read failed\n");
        HeapFree(GetProcessHeap(), 0, data);
        return NULL;
    }

    if (read_size > 2 && data[0] == 0xff && data[1] == 0xfe) { /* UTF-16 BOM */
        ret = SysAllocStringLen((const WCHAR *)(data + 2), (read_size - 2) / sizeof(WCHAR));
    }else {
        size = MultiByteToWideChar(CP_ACP, 0, (const char *)data, read_size, NULL, 0);
        ret = SysAllocStringLen(NULL, size);
        if (ret)
            MultiByteToWideChar(CP_ACP, 0, (const char *)data, read_size, ret, size);
    }
    HeapFree(GetProcessHeap(), 0, data);

    return ret;
}

static int change_command(int argc, WCHAR *argv[])
{
    BOOL have_option = FALSE, enable = FALSE;
    const WCHAR *task_name = NULL;
    IRegisteredTask *task;
    HRESULT hres;

    while (argc) {
        if (!wcsicmp(argv[0], L"/tn")) {
            if (argc < 2) {
                FIXME("Missing /tn value\n");
                return 1;
            }

            if (task_name) {
                FIXME("Duplicated /tn argument\n");
                return 1;
            }

            task_name = argv[1];
            argc -= 2;
            argv += 2;
        } else if (!wcsicmp(argv[0], L"/enable")) {
            enable = TRUE;
            have_option = TRUE;
            argc--;
            argv++;
        } else if (!wcsicmp(argv[0], L"/tr")) {
            if (argc < 2) {
                FIXME("Missing /tr value\n");
                return 1;
            }

            FIXME("Unsupported /tr option %s\n", debugstr_w(argv[1]));
            have_option = TRUE;
            argc -= 2;
            argv += 2;
        }else {
            FIXME("Unsupported arguments %s\n", debugstr_w(argv[0]));
            return 1;
        }
    }

    if (!task_name) {
        FIXME("Missing /tn option\n");
        return 1;
    }

    if (!have_option) {
        FIXME("Missing change options\n");
        return 1;
    }

    task = get_registered_task(task_name);
    if (!task)
        return 1;

    if (enable) {
        hres = IRegisteredTask_put_Enabled(task, VARIANT_TRUE);
        if (FAILED(hres)) {
            IRegisteredTask_Release(task);
            FIXME("put_Enabled failed: %08lx\n", hres);
            return 1;
        }
    }

    IRegisteredTask_Release(task);
    return 0;
}

/* Minimal XML text escape; the caller frees the result with HeapFree. */
static WCHAR *xml_escape(const WCHAR *s)
{
    size_t i, n = 0;
    WCHAR *out, *o;

    for (i = 0; s && s[i]; i++)
        switch (s[i]) {
        case '&': n += 5; break;
        case '<': case '>': n += 4; break;
        case '"': case '\'': n += 6; break;
        default: n++;
        }
    out = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR));
    if (!out) return NULL;
    o = out;
    for (i = 0; s && s[i]; i++)
        switch (s[i]) {
        case '&': lstrcpyW(o, L"&amp;"); o += 5; break;
        case '<': lstrcpyW(o, L"&lt;"); o += 4; break;
        case '>': lstrcpyW(o, L"&gt;"); o += 4; break;
        case '"': lstrcpyW(o, L"&quot;"); o += 6; break;
        case '\'': lstrcpyW(o, L"&apos;"); o += 6; break;
        default: *o++ = s[i];
        }
    *o = 0;
    return out;
}

/* ISO "YYYY-MM-DDTHH:MM:SS" from the current local time, overridden by /sd (MM/DD/YYYY) and /st (HH:MM[:SS]). */
static void build_boundary(WCHAR *out, size_t count, const WCHAR *sd, const WCHAR *st)
{
    SYSTEMTIME lt;
    unsigned int yy, mo, dd, hh, mm, ss = 0;
    const WCHAR *b, *c;

    GetLocalTime(&lt);
    yy = lt.wYear; mo = lt.wMonth; dd = lt.wDay; hh = lt.wHour; mm = lt.wMinute;
    if (sd && (b = wcschr(sd, '/'))) {
        mo = wcstoul(sd, NULL, 10);
        dd = wcstoul(b + 1, NULL, 10);
        if ((c = wcschr(b + 1, '/'))) yy = wcstoul(c + 1, NULL, 10);
    }
    if (st) {
        hh = wcstoul(st, NULL, 10);
        mm = ss = 0;
        if ((b = wcschr(st, ':'))) {
            mm = wcstoul(b + 1, NULL, 10);
            if ((c = wcschr(b + 1, ':'))) ss = wcstoul(c + 1, NULL, 10);
        }
    }
    _snwprintf(out, count, L"%04u-%02u-%02uT%02u:%02u:%02u", yy, mo, dd, hh, mm, ss);
}

/* Emit <Day/> elements for /d (comma list of SUN..SAT); defaults to every day. */
static void build_days_of_week(WCHAR *out, size_t count, const WCHAR *d)
{
    static const WCHAR *full[] = { L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday" };
    static const WCHAR *abbr[] = { L"SUN", L"MON", L"TUE", L"WED", L"THU", L"FRI", L"SAT" };
    int mask = 0, i;
    WCHAR *p = out;
    size_t rem = count;

    out[0] = 0;
    if (d) {
        const WCHAR *s = d;
        while (*s) {
            WCHAR tok[8];
            int n = 0;
            while (*s == ',' || *s == ' ') s++;
            while (*s && *s != ',' && n < 7) tok[n++] = *s++;
            tok[n] = 0;
            if (n) for (i = 0; i < 7; i++) if (!wcsicmp(tok, abbr[i])) mask |= 1 << i;
        }
    }
    if (!mask) mask = 0x7f;
    for (i = 0; i < 7; i++)
        if (mask & (1 << i)) {
            int w = _snwprintf(p, rem, L"<%s/>", full[i]);
            if (w < 0 || (size_t)w >= rem) break;
            p += w; rem -= w;
        }
}

static int create_command(int argc, WCHAR *argv[])
{
    const WCHAR *task_name = NULL, *xml_file = NULL, *tr = NULL, *sc = NULL;
    const WCHAR *mo = NULL, *st = NULL, *sd = NULL, *ru = NULL, *rp = NULL, *rl = NULL, *days = NULL;
    const WCHAR *sid = NULL, *logon = NULL, *runlevel, *cmd_for_xml, *args_for_xml = NULL;
    WCHAR *ru_esc = NULL, *tn_esc = NULL, *cmd_esc = NULL, *args_esc = NULL, *cmd_only = NULL, *xmlbuf = NULL, *p;
    TASK_LOGON_TYPE logon_param = TASK_LOGON_NONE;
    ITaskFolder *root = NULL;
    LONG flags = TASK_CREATE;
    IRegisteredTask *task;
    VARIANT v_user, v_pwd, v_sddl;
    WCHAR boundary[32];
    unsigned int interval;
    size_t cap, rem;
    BSTR str, xml;
    HRESULT hres;
    int ret = 1;

    while (argc) {
        if (!wcsicmp(argv[0], L"/xml")) {
            if (argc < 2) { FIXME("Missing /xml value\n"); return 1; }
            if (xml_file) { FIXME("Duplicated /xml argument\n"); return 1; }
            xml_file = argv[1]; argc -= 2; argv += 2;
        } else if (!wcsicmp(argv[0], L"/tn")) {
            if (argc < 2) { FIXME("Missing /tn value\n"); return 1; }
            if (task_name) { FIXME("Duplicated /tn argument\n"); return 1; }
            task_name = argv[1]; argc -= 2; argv += 2;
        } else if (!wcsicmp(argv[0], L"/tr")) {
            if (argc < 2) { FIXME("Missing /tr value\n"); return 1; }
            tr = argv[1]; argc -= 2; argv += 2;
        } else if (!wcsicmp(argv[0], L"/sc")) {
            if (argc < 2) { FIXME("Missing /sc value\n"); return 1; }
            sc = argv[1]; argc -= 2; argv += 2;
        } else if (!wcsicmp(argv[0], L"/mo")) {
            if (argc < 2) { FIXME("Missing /mo value\n"); return 1; }
            mo = argv[1]; argc -= 2; argv += 2;
        } else if (!wcsicmp(argv[0], L"/st")) {
            if (argc < 2) { FIXME("Missing /st value\n"); return 1; }
            st = argv[1]; argc -= 2; argv += 2;
        } else if (!wcsicmp(argv[0], L"/sd")) {
            if (argc < 2) { FIXME("Missing /sd value\n"); return 1; }
            sd = argv[1]; argc -= 2; argv += 2;
        } else if (!wcsicmp(argv[0], L"/d")) {
            if (argc < 2) { FIXME("Missing /d value\n"); return 1; }
            days = argv[1]; argc -= 2; argv += 2;
        } else if (!wcsicmp(argv[0], L"/ru")) {
            if (argc < 2) { FIXME("Missing /ru value\n"); return 1; }
            ru = argv[1]; argc -= 2; argv += 2;
        } else if (!wcsicmp(argv[0], L"/rp")) {
            if (argc < 2) { FIXME("Missing /rp value\n"); return 1; }
            rp = argv[1]; argc -= 2; argv += 2;
        } else if (!wcsicmp(argv[0], L"/rl")) {
            if (argc < 2) { FIXME("Missing /rl value\n"); return 1; }
            rl = argv[1]; argc -= 2; argv += 2;
        } else if (!wcsicmp(argv[0], L"/f")) {
            flags = TASK_CREATE_OR_UPDATE; argc--; argv++;
        } else {
            FIXME("Unsupported argument %s\n", debugstr_w(argv[0]));
            return 1;
        }
    }

    if (!task_name) { FIXME("Missing /tn argument\n"); return 1; }

    /* /xml: register the task definition from the file verbatim. */
    if (xml_file) {
        xml = read_file_to_bstr(xml_file);
        if (!xml) return 1;
        root = get_tasks_root_folder();
        if (!root) { SysFreeString(xml); return 1; }
        V_VT(&v_user) = VT_EMPTY; V_VT(&v_pwd) = VT_EMPTY; V_VT(&v_sddl) = VT_EMPTY;
        str = SysAllocString(task_name);
        hres = ITaskFolder_RegisterTask(root, str, xml, flags, v_user, v_pwd,
                                        TASK_LOGON_NONE, v_sddl, &task);
        SysFreeString(str);
        SysFreeString(xml);
        ITaskFolder_Release(root);
        if (FAILED(hres)) return 1;
        IRegisteredTask_Release(task);
        return 0;
    }

    /* Option form: build a task definition from /sc, /tr, /ru, ... as Win11 schtasks does. */
    if (!sc) { FIXME("Missing /sc or /xml argument\n"); return 1; }
    if (!tr) { FIXME("Missing /tr argument\n"); return 1; }

    interval = mo ? wcstoul(mo, NULL, 10) : 0;
    if (!interval) interval = 1;

    /* Split /tr into command + arguments for the Exec action (the service quotes Command separately). */
    if (tr[0] == '"') {
        const WCHAR *end = wcschr(tr + 1, '"');
        if (end) {
            size_t cl = end - (tr + 1);
            if (!(cmd_only = HeapAlloc(GetProcessHeap(), 0, (cl + 1) * sizeof(WCHAR)))) goto done;
            memcpy(cmd_only, tr + 1, cl * sizeof(WCHAR)); cmd_only[cl] = 0;
            args_for_xml = end + 1;
            while (*args_for_xml == ' ') args_for_xml++;
        }
    } else {
        const WCHAR *sp = wcschr(tr, ' ');
        if (sp) {
            size_t cl = sp - tr;
            if (!(cmd_only = HeapAlloc(GetProcessHeap(), 0, (cl + 1) * sizeof(WCHAR)))) goto done;
            memcpy(cmd_only, tr, cl * sizeof(WCHAR)); cmd_only[cl] = 0;
            args_for_xml = sp + 1;
            while (*args_for_xml == ' ') args_for_xml++;
        }
    }
    cmd_for_xml = cmd_only ? cmd_only : tr;

    tn_esc = xml_escape(task_name);
    cmd_esc = xml_escape(cmd_for_xml);
    if (args_for_xml && *args_for_xml) args_esc = xml_escape(args_for_xml);
    if (!tn_esc || !cmd_esc) goto done;

    if (ru) {
        if (!wcsicmp(ru, L"SYSTEM") || !wcsicmp(ru, L"NT AUTHORITY\\SYSTEM"))
            { sid = L"S-1-5-18"; logon = L"ServiceAccount"; logon_param = TASK_LOGON_SERVICE_ACCOUNT; }
        else if (!wcsicmp(ru, L"LOCAL SERVICE") || !wcsicmp(ru, L"NT AUTHORITY\\LOCAL SERVICE"))
            { sid = L"S-1-5-19"; logon = L"ServiceAccount"; logon_param = TASK_LOGON_SERVICE_ACCOUNT; }
        else if (!wcsicmp(ru, L"NETWORK SERVICE") || !wcsicmp(ru, L"NT AUTHORITY\\NETWORK SERVICE"))
            { sid = L"S-1-5-20"; logon = L"ServiceAccount"; logon_param = TASK_LOGON_SERVICE_ACCOUNT; }
        else {
            if (!(ru_esc = xml_escape(ru))) goto done;
            if (rp) { logon = L"Password"; logon_param = TASK_LOGON_PASSWORD; }
            else { logon = L"InteractiveToken"; logon_param = TASK_LOGON_INTERACTIVE_TOKEN; }
        }
    }
    runlevel = (rl && !wcsicmp(rl, L"HIGHEST")) ? L"HighestAvailable" : L"LeastPrivilege";

    cap = 4096 + 8 * (wcslen(tn_esc) + wcslen(cmd_esc) + (args_esc ? wcslen(args_esc) : 0) + (ru_esc ? wcslen(ru_esc) : 0));
    if (!(xmlbuf = HeapAlloc(GetProcessHeap(), 0, cap * sizeof(WCHAR)))) goto done;
    p = xmlbuf; rem = cap;

#define APP(...) do { int _n = _snwprintf(p, rem, __VA_ARGS__); \
    if (_n < 0 || (size_t)_n >= rem) goto done; p += _n; rem -= _n; } while (0)

    APP(L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo/>\r\n<Triggers>");

    if (!wcsicmp(sc, L"ONSTART")) APP(L"<BootTrigger><Enabled>true</Enabled></BootTrigger>");
    else if (!wcsicmp(sc, L"ONLOGON")) APP(L"<LogonTrigger><Enabled>true</Enabled></LogonTrigger>");
    else if (!wcsicmp(sc, L"ONIDLE")) APP(L"<IdleTrigger><Enabled>true</Enabled></IdleTrigger>");
    else {
        build_boundary(boundary, 32, sd, st);
        if (!wcsicmp(sc, L"ONCE"))
            APP(L"<TimeTrigger><StartBoundary>%s</StartBoundary><Enabled>true</Enabled></TimeTrigger>", boundary);
        else if (!wcsicmp(sc, L"MINUTE"))
            APP(L"<TimeTrigger><StartBoundary>%s</StartBoundary><Enabled>true</Enabled>"
                L"<Repetition><Interval>PT%uM</Interval></Repetition></TimeTrigger>", boundary, interval);
        else if (!wcsicmp(sc, L"HOURLY"))
            APP(L"<TimeTrigger><StartBoundary>%s</StartBoundary><Enabled>true</Enabled>"
                L"<Repetition><Interval>PT%uH</Interval></Repetition></TimeTrigger>", boundary, interval);
        else if (!wcsicmp(sc, L"DAILY"))
            APP(L"<CalendarTrigger><StartBoundary>%s</StartBoundary><Enabled>true</Enabled>"
                L"<ScheduleByDay><DaysInterval>%u</DaysInterval></ScheduleByDay></CalendarTrigger>", boundary, interval);
        else if (!wcsicmp(sc, L"WEEKLY")) {
            WCHAR dow[256];
            build_days_of_week(dow, 256, days);
            APP(L"<CalendarTrigger><StartBoundary>%s</StartBoundary><Enabled>true</Enabled>"
                L"<ScheduleByWeek><WeeksInterval>%u</WeeksInterval><DaysOfWeek>%s</DaysOfWeek></ScheduleByWeek></CalendarTrigger>",
                boundary, interval, dow);
        }
        else if (!wcsicmp(sc, L"MONTHLY")) {
            unsigned int mday = days ? wcstoul(days, NULL, 10) : 1;
            if (mday < 1 || mday > 31) mday = 1;
            APP(L"<CalendarTrigger><StartBoundary>%s</StartBoundary><Enabled>true</Enabled>"
                L"<ScheduleByMonth><DaysOfMonth><Day>%u</Day></DaysOfMonth><Months>"
                L"<January/><February/><March/><April/><May/><June/><July/><August/>"
                L"<September/><October/><November/><December/></Months></ScheduleByMonth></CalendarTrigger>",
                boundary, mday);
        }
        else { FIXME("Unsupported /sc value %s\n", debugstr_w(sc)); goto done; }
    }

    APP(L"</Triggers>");

    if (ru)
        APP(L"<Principals><Principal id=\"Author\"><UserId>%s</UserId>"
            L"<RunLevel>%s</RunLevel><LogonType>%s</LogonType></Principal></Principals>",
            sid ? sid : ru_esc, runlevel, logon);

    APP(L"<Settings><Enabled>true</Enabled><AllowStartOnDemand>true</AllowStartOnDemand>"
        L"<StartWhenAvailable>true</StartWhenAvailable>"
        L"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy></Settings>");

    APP(ru ? L"<Actions Context=\"Author\"><Exec><Command>%s</Command>"
           : L"<Actions><Exec><Command>%s</Command>", cmd_esc);
    if (args_esc) APP(L"<Arguments>%s</Arguments>", args_esc);
    APP(L"</Exec></Actions>\r\n</Task>\r\n");

#undef APP

    root = get_tasks_root_folder();
    if (!root) goto done;

    V_VT(&v_user) = VT_EMPTY; V_VT(&v_pwd) = VT_EMPTY; V_VT(&v_sddl) = VT_EMPTY;
    if (logon_param == TASK_LOGON_PASSWORD) {
        V_VT(&v_user) = VT_BSTR; V_BSTR(&v_user) = SysAllocString(ru);
        V_VT(&v_pwd) = VT_BSTR; V_BSTR(&v_pwd) = SysAllocString(rp);
    }

    str = SysAllocString(task_name);
    xml = SysAllocString(xmlbuf);
    hres = ITaskFolder_RegisterTask(root, str, xml, flags, v_user, v_pwd, logon_param, v_sddl, &task);
    SysFreeString(str);
    SysFreeString(xml);
    if (V_VT(&v_user) == VT_BSTR) SysFreeString(V_BSTR(&v_user));
    if (V_VT(&v_pwd) == VT_BSTR) SysFreeString(V_BSTR(&v_pwd));
    ITaskFolder_Release(root);
    if (SUCCEEDED(hres)) {
        IRegisteredTask_Release(task);
        ret = 0;
    }

done:
    if (cmd_only) HeapFree(GetProcessHeap(), 0, cmd_only);
    if (tn_esc) HeapFree(GetProcessHeap(), 0, tn_esc);
    if (cmd_esc) HeapFree(GetProcessHeap(), 0, cmd_esc);
    if (args_esc) HeapFree(GetProcessHeap(), 0, args_esc);
    if (ru_esc) HeapFree(GetProcessHeap(), 0, ru_esc);
    if (xmlbuf) HeapFree(GetProcessHeap(), 0, xmlbuf);
    return ret;
}

static int delete_command(int argc, WCHAR *argv[])
{
    const WCHAR *task_name = NULL;
    ITaskFolder *root = NULL;
    BSTR str;
    HRESULT hres;

    while (argc) {
        if (!wcsicmp(argv[0], L"/f")) {
            TRACE("force opt\n");
            argc--;
            argv++;
        } else if (!wcsicmp(argv[0], L"/tn")) {
            if (argc < 2) {
                FIXME("Missing /tn value\n");
                return 1;
            }

            if (task_name) {
                FIXME("Duplicated /tn argument\n");
                return 1;
            }

            task_name = argv[1];
            argc -= 2;
            argv += 2;
        }else {
            FIXME("Unsupported argument %s\n", debugstr_w(argv[0]));
            return 1;
        }
    }

    if (!task_name) {
        FIXME("Missing /tn argument\n");
        return 1;
    }

    root = get_tasks_root_folder();
    if (!root)
        return 1;

    str = SysAllocString(task_name);
    hres = ITaskFolder_DeleteTask(root, str, 0);
    SysFreeString(str);
    ITaskFolder_Release(root);
    if (FAILED(hres))
        return 1;

    return 0;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    int i, ret = 0;

    for (i = 0; i < argc; i++)
        TRACE(" %s", wine_dbgstr_w(argv[i]));
    TRACE("\n");

    CoInitialize(NULL);

    if (argc < 2)
        FIXME("Print current tasks state\n");
    else if (!wcsicmp(argv[1], L"/change"))
        ret = change_command(argc - 2, argv + 2);
    else if (!wcsicmp(argv[1], L"/create"))
        ret = create_command(argc - 2, argv + 2);
    else if (!wcsicmp(argv[1], L"/delete"))
        ret = delete_command(argc - 2, argv + 2);
    else
        FIXME("Unsupported command %s\n", debugstr_w(argv[1]));

    CoUninitialize();
    return ret;
}
