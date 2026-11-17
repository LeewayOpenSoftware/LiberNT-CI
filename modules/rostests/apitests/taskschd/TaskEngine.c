/*
 * PROJECT:     LiberNT api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Task Scheduler 2.0: tasks really run, run state, Run/Stop,
 *              trigger XML round trip, task path validation
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The run tests register tasks as LocalSystem and need an elevated
 * administrator; they are skipped otherwise. Everything here also holds on
 * Windows.
 */

#define COBJMACROS
#include <apitest.h>
#include <objbase.h>
#include <oleauto.h>
#include <initguid.h>
#include <taskschd.h>
#include <strsafe.h>

#ifndef SCHED_S_TASK_RUNNING
#define SCHED_S_TASK_RUNNING        ((HRESULT)0x00041301)
#endif
#ifndef SCHED_S_TASK_HAS_NOT_RUN
#define SCHED_S_TASK_HAS_NOT_RUN    ((HRESULT)0x00041303)
#endif
#ifndef SCHED_S_TASK_TERMINATED
#define SCHED_S_TASK_TERMINATED     ((HRESULT)0x00041306)
#endif

static const WCHAR task_ns[] = L"http://schemas.microsoft.com/windows/2004/02/mit/task";

static VARIANT v_null, v_empty;

/* Task XML run as LocalSystem; 'triggers' is the inner text of <Triggers>. */
static void make_task_xml(WCHAR *buf, size_t cch, const WCHAR *triggers, const WCHAR *args)
{
    StringCchPrintfW(buf, cch,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
        L"<Task version=\"1.2\" xmlns=\"%s\">\n"
        L"  <RegistrationInfo><Description>LiberNT task engine apitest</Description></RegistrationInfo>\n"
        L"  <Triggers>%s</Triggers>\n"
        L"  <Principals><Principal id=\"Author\"><UserId>S-1-5-18</UserId>"
        L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>\n"
        L"  <Settings>\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\n"
        L"    <Enabled>true</Enabled>\n"
        L"    <ExecutionTimeLimit>PT5M</ExecutionTimeLimit>\n"
        L"  </Settings>\n"
        L"  <Actions Context=\"Author\"><Exec><Command>%%SystemRoot%%\\System32\\cmd.exe</Command>"
        L"<Arguments>%s</Arguments></Exec></Actions>\n"
        L"</Task>\n",
        task_ns, triggers, args);
}

static HRESULT register_system_task(ITaskFolder *root, const WCHAR *name, const WCHAR *xml, IRegisteredTask **task)
{
    BSTR bname = SysAllocString(name), bxml = SysAllocString(xml);
    HRESULT hr;

    ITaskFolder_DeleteTask(root, bname, 0);
    hr = ITaskFolder_RegisterTask(root, bname, bxml, TASK_CREATE, v_null, v_null,
                                  TASK_LOGON_SERVICE_ACCOUNT, v_null, task);
    SysFreeString(bname);
    SysFreeString(bxml);
    return hr;
}

static void delete_task(ITaskFolder *root, const WCHAR *name)
{
    BSTR bname = SysAllocString(name);
    ITaskFolder_DeleteTask(root, bname, 0);
    SysFreeString(bname);
}

/* Waits until LastTaskResult is neither "has not run" nor "running". */
static LONG wait_for_result(IRegisteredTask *task, DWORD timeout_ms)
{
    DWORD start = GetTickCount();
    LONG result = SCHED_S_TASK_HAS_NOT_RUN;

    while (GetTickCount() - start < timeout_ms)
    {
        if (IRegisteredTask_get_LastTaskResult(task, &result) == S_OK &&
            result != SCHED_S_TASK_HAS_NOT_RUN && result != SCHED_S_TASK_RUNNING)
            break;
        Sleep(500);
    }
    return result;
}

static void format_local(WCHAR *buf, size_t cch, ULONGLONG add_seconds)
{
    SYSTEMTIME st;
    FILETIME ft;
    ULARGE_INTEGER u;

    GetLocalTime(&st);
    SystemTimeToFileTime(&st, &ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += add_seconds * 10000000ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
    StringCchPrintfW(buf, cch, L"%04u-%02u-%02uT%02u:%02u:%02u",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

/* A TimeTrigger a few seconds ahead runs the action as SYSTEM and records its exit code. */
static BOOL test_time_trigger(ITaskFolder *root)
{
    static const WCHAR name[] = L"LiberNT TaskEngine TimeTrigger";
    WCHAR when[32], triggers[256], xml[4096];
    IRegisteredTask *task = NULL;
    SYSTEMTIME *times = NULL, from, to;
    DWORD count;
    DATE date;
    LONG result;
    HRESULT hr;

    format_local(when, ARRAYSIZE(when), 8);
    StringCchPrintfW(triggers, ARRAYSIZE(triggers),
                     L"<TimeTrigger><StartBoundary>%s</StartBoundary><Enabled>true</Enabled></TimeTrigger>", when);
    make_task_xml(xml, ARRAYSIZE(xml), triggers, L"/c exit 42");

    hr = register_system_task(root, name, xml, &task);
    if (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
    {
        skip("registering a LocalSystem task needs an elevated administrator\n");
        return FALSE;
    }
    ok(hr == S_OK, "RegisterTask failed: 0x%08lx\n", hr);
    if (hr != S_OK) return FALSE;

    result = 0;
    hr = IRegisteredTask_get_LastTaskResult(task, &result);
    ok(hr == S_OK, "get_LastTaskResult: 0x%08lx\n", hr);
    ok(result == SCHED_S_TASK_HAS_NOT_RUN, "fresh task result 0x%08lx\n", result);

    date = 0;
    hr = IRegisteredTask_get_NextRunTime(task, &date);
    ok(hr == S_OK, "get_NextRunTime: 0x%08lx\n", hr);
    ok(date != 0, "NextRunTime not set\n");

    GetLocalTime(&from);
    to = from;
    to.wYear++;
    count = 5;
    hr = IRegisteredTask_GetRunTimes(task, &from, &to, &count, &times);
    ok(hr == S_OK, "GetRunTimes: 0x%08lx\n", hr);
    ok(count == 1, "GetRunTimes: %lu run times, expected 1\n", count);
    CoTaskMemFree(times);

    result = wait_for_result(task, 90000);
    ok(result == 42, "TimeTrigger task: LastTaskResult 0x%08lx, expected 42 (did it run?)\n", result);

    date = 0;
    hr = IRegisteredTask_get_LastRunTime(task, &date);
    ok(hr == S_OK, "get_LastRunTime after run: 0x%08lx\n", hr);
    ok(date != 0, "LastRunTime not recorded\n");

    IRegisteredTask_Release(task);
    delete_task(root, name);
    return TRUE;
}

/* Run with $(Arg0), then a long run stopped through IRegisteredTask::Stop. */
static void test_run_stop(ITaskFolder *root)
{
    static const WCHAR name[] = L"LiberNT TaskEngine RunStop";
    WCHAR xml[4096];
    IRegisteredTask *task = NULL;
    IRunningTask *running = NULL;
    IRunningTaskCollection *instances = NULL;
    TASK_STATE state;
    VARIANT arg;
    LONG result, count;
    DWORD start;
    HRESULT hr;

    make_task_xml(xml, ARRAYSIZE(xml), L"", L"/c exit $(Arg0)");
    hr = register_system_task(root, name, xml, &task);
    ok(hr == S_OK, "RegisterTask failed: 0x%08lx\n", hr);
    if (hr != S_OK) return;

    V_VT(&arg) = VT_BSTR;
    V_BSTR(&arg) = SysAllocString(L"7");
    hr = IRegisteredTask_Run(task, arg, &running);
    VariantClear(&arg);
    ok(hr == S_OK, "Run: 0x%08lx\n", hr);
    if (running) IRunningTask_Release(running);
    result = wait_for_result(task, 30000);
    ok(result == 7, "Run with $(Arg0)=7: LastTaskResult 0x%08lx\n", result);

    IRegisteredTask_Release(task);
    task = NULL;

    make_task_xml(xml, ARRAYSIZE(xml), L"", L"/c ping -n 60 127.0.0.1 &gt; nul");
    hr = register_system_task(root, name, xml, &task);
    ok(hr == S_OK, "RegisterTask (long) failed: 0x%08lx\n", hr);
    if (hr != S_OK) return;

    running = NULL;
    hr = IRegisteredTask_Run(task, v_empty, &running);
    ok(hr == S_OK, "Run (long): 0x%08lx\n", hr);

    state = TASK_STATE_UNKNOWN;
    for (start = GetTickCount(); GetTickCount() - start < 10000; Sleep(250))
        if (IRegisteredTask_get_State(task, &state) == S_OK && state == TASK_STATE_RUNNING) break;
    ok(state == TASK_STATE_RUNNING, "state %d, expected running\n", state);

    hr = IRegisteredTask_GetInstances(task, 0, &instances);
    ok(hr == S_OK, "GetInstances: 0x%08lx\n", hr);
    if (instances)
    {
        count = 0;
        IRunningTaskCollection_get_Count(instances, &count);
        ok(count == 1, "%ld running instances, expected 1\n", count);
        IRunningTaskCollection_Release(instances);
    }

    hr = IRegisteredTask_Stop(task, 0);
    ok(hr == S_OK, "Stop: 0x%08lx\n", hr);
    result = wait_for_result(task, 30000);
    ok(result == SCHED_S_TASK_TERMINATED, "stopped task: LastTaskResult 0x%08lx\n", result);

    if (running) IRunningTask_Release(running);
    IRegisteredTask_Release(task);
    delete_task(root, name);
}

static ITrigger *add_trigger(ITriggerCollection *triggers, TASK_TRIGGER_TYPE2 type)
{
    ITrigger *trigger = NULL;
    HRESULT hr = ITriggerCollection_Create(triggers, type, &trigger);

    ok(hr == S_OK, "Create(type %d): 0x%08lx\n", type, hr);
    return hr == S_OK ? trigger : NULL;
}

/* The new trigger types build, serialize and read back. No registration. */
static void test_trigger_roundtrip(ITaskService *service)
{
    ITaskDefinition *def = NULL, *copy = NULL;
    ITriggerCollection *triggers = NULL;
    IActionCollection *actions = NULL;
    IAction *action = NULL;
    IExecAction *exec;
    ITrigger *trigger;
    IWeeklyTrigger *weekly;
    IMonthlyTrigger *monthly;
    IEventTrigger *event;
    IBootTrigger *boot;
    TASK_TRIGGER_TYPE2 type;
    LONG idx;
    BSTR xml = NULL, str;
    short sval;
    LONG lval, count;
    VARIANT_BOOL last;
    HRESULT hr;

    hr = ITaskService_NewTask(service, 0, &def);
    ok(hr == S_OK, "NewTask: 0x%08lx\n", hr);
    if (hr != S_OK) return;

    if (ITaskDefinition_get_Actions(def, &actions) == S_OK &&
        IActionCollection_Create(actions, TASK_ACTION_EXEC, &action) == S_OK &&
        IAction_QueryInterface(action, &IID_IExecAction, (void **)&exec) == S_OK)
    {
        str = SysAllocString(L"cmd.exe");
        IExecAction_put_Path(exec, str);
        SysFreeString(str);
        IExecAction_Release(exec);
    }
    if (action) IAction_Release(action);

    hr = ITaskDefinition_get_Triggers(def, &triggers);
    ok(hr == S_OK, "get_Triggers: 0x%08lx\n", hr);
    if (hr != S_OK) goto done;

    if ((trigger = add_trigger(triggers, TASK_TRIGGER_WEEKLY)))
    {
        str = SysAllocString(L"2030-01-07T09:00:00");
        ITrigger_put_StartBoundary(trigger, str);
        SysFreeString(str);
        ITrigger_QueryInterface(trigger, &IID_IWeeklyTrigger, (void **)&weekly);
        IWeeklyTrigger_put_DaysOfWeek(weekly, 0x22);    /* Monday | Friday */
        IWeeklyTrigger_put_WeeksInterval(weekly, 2);
        IWeeklyTrigger_Release(weekly);
        ITrigger_Release(trigger);
    }
    if ((trigger = add_trigger(triggers, TASK_TRIGGER_MONTHLY)))
    {
        str = SysAllocString(L"2030-01-01T06:30:00");
        ITrigger_put_StartBoundary(trigger, str);
        SysFreeString(str);
        ITrigger_QueryInterface(trigger, &IID_IMonthlyTrigger, (void **)&monthly);
        IMonthlyTrigger_put_DaysOfMonth(monthly, 0x40000001);   /* 1st and 31st */
        IMonthlyTrigger_put_MonthsOfYear(monthly, 0x801);       /* January | December */
        IMonthlyTrigger_put_RunOnLastDayOfMonth(monthly, VARIANT_TRUE);
        IMonthlyTrigger_Release(monthly);
        ITrigger_Release(trigger);
    }
    if ((trigger = add_trigger(triggers, TASK_TRIGGER_BOOT)))
    {
        ITrigger_QueryInterface(trigger, &IID_IBootTrigger, (void **)&boot);
        str = SysAllocString(L"PT30S");
        IBootTrigger_put_Delay(boot, str);
        SysFreeString(str);
        IBootTrigger_Release(boot);
        ITrigger_Release(trigger);
    }
    if ((trigger = add_trigger(triggers, TASK_TRIGGER_EVENT)))
    {
        ITrigger_QueryInterface(trigger, &IID_IEventTrigger, (void **)&event);
        str = SysAllocString(L"<QueryList><Query Id=\"0\"><Select Path=\"System\">*</Select></Query></QueryList>");
        IEventTrigger_put_Subscription(event, str);
        SysFreeString(str);
        IEventTrigger_Release(event);
        ITrigger_Release(trigger);
    }
    if ((trigger = add_trigger(triggers, TASK_TRIGGER_TIME)))
    {
        /* Windows rejects a TimeTrigger without StartBoundary on read (SCHED_E_MISSINGNODE) */
        str = SysAllocString(L"2030-06-01T12:00:00");
        ITrigger_put_StartBoundary(trigger, str);
        SysFreeString(str);
        ITrigger_Release(trigger);
    }
    if ((trigger = add_trigger(triggers, TASK_TRIGGER_IDLE))) ITrigger_Release(trigger);

    hr = ITaskDefinition_get_XmlText(def, &xml);
    ok(hr == S_OK, "get_XmlText: 0x%08lx\n", hr);
    if (hr != S_OK) goto done;

    hr = ITaskService_NewTask(service, 0, &copy);
    ok(hr == S_OK, "NewTask: 0x%08lx\n", hr);
    if (hr != S_OK) goto done;
    hr = ITaskDefinition_put_XmlText(copy, xml);
    ok(hr == S_OK, "put_XmlText: 0x%08lx\n", hr);
    if (hr != S_OK) goto done;

    ITriggerCollection_Release(triggers);
    triggers = NULL;
    hr = ITaskDefinition_get_Triggers(copy, &triggers);
    ok(hr == S_OK, "get_Triggers (copy): 0x%08lx\n", hr);
    if (hr != S_OK) goto done;

    count = 0;
    ITriggerCollection_get_Count(triggers, &count);
    ok(count == 6, "read back %ld triggers, expected 6\n", count);

    idx = 1;
    trigger = NULL;
    if (ITriggerCollection_get_Item(triggers, idx, &trigger) == S_OK)
    {
        ITrigger_get_Type(trigger, &type);
        ok(type == TASK_TRIGGER_WEEKLY, "trigger 1 type %d, expected weekly\n", type);
        if (type == TASK_TRIGGER_WEEKLY && ITrigger_QueryInterface(trigger, &IID_IWeeklyTrigger, (void **)&weekly) == S_OK)
        {
            sval = 0;
            IWeeklyTrigger_get_DaysOfWeek(weekly, &sval);
            ok(sval == 0x22, "DaysOfWeek 0x%x\n", sval);
            sval = 0;
            IWeeklyTrigger_get_WeeksInterval(weekly, &sval);
            ok(sval == 2, "WeeksInterval %d\n", sval);
            IWeeklyTrigger_Release(weekly);
        }
        ITrigger_Release(trigger);
    }

    idx = 2;
    trigger = NULL;
    if (ITriggerCollection_get_Item(triggers, idx, &trigger) == S_OK)
    {
        ITrigger_get_Type(trigger, &type);
        ok(type == TASK_TRIGGER_MONTHLY, "trigger 2 type %d, expected monthly\n", type);
        if (type == TASK_TRIGGER_MONTHLY && ITrigger_QueryInterface(trigger, &IID_IMonthlyTrigger, (void **)&monthly) == S_OK)
        {
            lval = 0;
            IMonthlyTrigger_get_DaysOfMonth(monthly, &lval);
            ok(lval == 0x40000001, "DaysOfMonth 0x%lx\n", lval);
            sval = 0;
            IMonthlyTrigger_get_MonthsOfYear(monthly, &sval);
            ok(sval == 0x801, "MonthsOfYear 0x%x\n", sval);
            last = VARIANT_FALSE;
            IMonthlyTrigger_get_RunOnLastDayOfMonth(monthly, &last);
            ok(last == VARIANT_TRUE, "RunOnLastDayOfMonth %d\n", last);
            IMonthlyTrigger_Release(monthly);
        }
        ITrigger_Release(trigger);
    }

    idx = 4;
    trigger = NULL;
    if (ITriggerCollection_get_Item(triggers, idx, &trigger) == S_OK)
    {
        ITrigger_get_Type(trigger, &type);
        ok(type == TASK_TRIGGER_EVENT, "trigger 4 type %d, expected event\n", type);
        if (type == TASK_TRIGGER_EVENT && ITrigger_QueryInterface(trigger, &IID_IEventTrigger, (void **)&event) == S_OK)
        {
            str = NULL;
            IEventTrigger_get_Subscription(event, &str);
            ok(str && !wcsncmp(str, L"<QueryList>", 11), "Subscription %s\n", wine_dbgstr_w(str));
            SysFreeString(str);
            IEventTrigger_Release(event);
        }
        ITrigger_Release(trigger);
    }

done:
    SysFreeString(xml);
    if (triggers) ITriggerCollection_Release(triggers);
    if (actions) IActionCollection_Release(actions);
    if (copy) ITaskDefinition_Release(copy);
    ITaskDefinition_Release(def);
}

/* A task name cannot leave the Tasks folder. */
static void test_path_escape(ITaskFolder *root)
{
    static const WCHAR *names[] = { L"..\\..\\lnt_ts_escape", L"..\\lnt_ts_escape", L"x\\..\\..\\..\\lnt_ts_escape" };
    WCHAR xml[4096], path[MAX_PATH];
    IRegisteredTask *task;
    BSTR bname, bxml;
    HRESULT hr;
    UINT i;

    make_task_xml(xml, ARRAYSIZE(xml), L"", L"/c exit 0");
    bxml = SysAllocString(xml);

    for (i = 0; i < ARRAYSIZE(names); i++)
    {
        task = NULL;
        bname = SysAllocString(names[i]);
        hr = ITaskFolder_RegisterTask(root, bname, bxml, TASK_CREATE, v_null, v_null,
                                      TASK_LOGON_SERVICE_ACCOUNT, v_null, &task);
        if (task)
        {
            IRegisteredTask_Release(task);
            ITaskFolder_DeleteTask(root, bname, 0);
        }
        SysFreeString(bname);
        trace("RegisterTask(%s): 0x%08lx\n", wine_dbgstr_w(names[i]), hr);

        GetWindowsDirectoryW(path, ARRAYSIZE(path));
        StringCchCatW(path, ARRAYSIZE(path), L"\\lnt_ts_escape");
        ok(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES, "%s escaped to %s\n",
           wine_dbgstr_w(names[i]), wine_dbgstr_w(path));
        GetSystemDirectoryW(path, ARRAYSIZE(path));
        StringCchCatW(path, ARRAYSIZE(path), L"\\lnt_ts_escape");
        ok(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES, "%s escaped to %s\n",
           wine_dbgstr_w(names[i]), wine_dbgstr_w(path));
    }
    SysFreeString(bxml);
}

START_TEST(TaskEngine)
{
    ITaskService *service = NULL;
    ITaskFolder *root = NULL;
    BSTR bpath;
    HRESULT hr;

    V_VT(&v_null) = VT_NULL;
    V_VT(&v_empty) = VT_EMPTY;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ok(SUCCEEDED(hr), "CoInitializeEx: 0x%08lx\n", hr);

    hr = CoCreateInstance(&CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskService, (void **)&service);
    if (FAILED(hr))
    {
        skip("TaskScheduler not available: 0x%08lx\n", hr);
        CoUninitialize();
        return;
    }

    hr = ITaskService_Connect(service, v_null, v_null, v_null, v_null);
    if (FAILED(hr))
    {
        skip("Connect failed (Schedule service not running?): 0x%08lx\n", hr);
        ITaskService_Release(service);
        CoUninitialize();
        return;
    }

    test_trigger_roundtrip(service);

    bpath = SysAllocString(L"\\");
    hr = ITaskService_GetFolder(service, bpath, &root);
    SysFreeString(bpath);
    ok(hr == S_OK, "GetFolder: 0x%08lx\n", hr);
    if (root)
    {
        test_path_escape(root);
        if (test_time_trigger(root))
            test_run_stop(root);
        ITaskFolder_Release(root);
    }

    ITaskService_Release(service);
    CoUninitialize();
}
