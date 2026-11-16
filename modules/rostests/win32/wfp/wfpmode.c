/*
 * PROJECT:     LiberNT user-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     WFP enforcement-mode / telemetry lab utility (M3 stage 0)
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 *
 * Raw-ioctl + WFP-management helper for the stage-0 lab gate (lab-harness/
 * m3_s0_gate.py). Every verb prints one machine-read record per line; values are
 * decimal unless prefixed 0x. The exit code is 0 on success, else the Win32 error.
 *
 *   get                              WFPMODE get mode=<0|1>
 *   set <off|on>                     WFPMODE pid=<pid> / WFPMODE set want=<0|1> rc=<err> mode=<read back>
 *                                    (direct ioctl with the caller token: as Administrator -> rc=5)
 *   svc set <off|on>                 runs set under a temporary LocalSystem service; WFPMODE svc rc=<n>
 *   events [--type <t>]              WFPEVENTS dropped/next/count then one WFPEVENT per record
 *   stats <filterId>                 WFPSTATS filter=<id> hits=<n>  (IOCTL_WFP_FILTER_GET_STATS)
 *   stats --from-events              WFPSTATS per filter id seen in the event ring
 *   filters hold <n> <permit|block-top> tcp <port> <seconds>
 *                                    WFPFILTERS count=<n> ids=<first>..<last> rc=<n>; holds the session open
 *   connect <port>                   WFPCONNECT port=<p> rc=<0|wsa error>
 *
 * Mode changes require the LocalSystem token (the driver enforces D-AUTH).
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <rpc.h>
#include <initguid.h>
#include <fwpmu.h>
#include <winioctl.h>
#include <stdio.h>
#include <drivers/wfp/wfpioctl.h>

#define SVC_NAME     L"WfpModeS0Svc"
#define SVC_OUT_PATH L"C:\\wfpmode-svc.out"

static ULONG g_SvcWant;
static SERVICE_STATUS_HANDLE g_SvcStatusHandle;

static
HANDLE
OpenEngineRaw(VOID)
{
    HANDLE Device = CreateFileW(WFP_ENGINE_WIN32_NAME, GENERIC_READ | GENERIC_WRITE,
                                0, NULL, OPEN_EXISTING, 0, NULL);
    if (Device == INVALID_HANDLE_VALUE)
    {
        fwprintf(stderr, L"wfpmode: cannot open %s (error %lu)\n",
                 WFP_ENGINE_WIN32_NAME, GetLastError());
    }
    return Device;
}

static
ULONG
QueryMode(HANDLE Device)
{
    ULONG Mode = 0xFFFFFFFF;
    DWORD Bytes = 0;

    if (!DeviceIoControl(Device, IOCTL_WFP_GET_MODE, NULL, 0,
                         &Mode, sizeof(Mode), &Bytes, NULL) || Bytes < sizeof(Mode))
    {
        return 0xFFFFFFFF;
    }
    return Mode;
}

static
DWORD
IoSetMode(HANDLE Device, ULONG Want)
{
    WFP_IOCTL_SET_MODE In;
    DWORD Bytes = 0;

    In.Mode = Want;
    if (DeviceIoControl(Device, IOCTL_WFP_SET_MODE, &In, sizeof(In), NULL, 0, &Bytes, NULL))
        return ERROR_SUCCESS;
    return GetLastError();
}

/* ---- get ---- */

static
int
CmdGet(VOID)
{
    HANDLE Device = OpenEngineRaw();
    ULONG Mode;

    if (Device == INVALID_HANDLE_VALUE)
        return 2;
    Mode = QueryMode(Device);
    CloseHandle(Device);
    wprintf(L"WFPMODE get mode=%lu\n", Mode);
    return (Mode == 0xFFFFFFFF) ? 2 : 0;
}

/* ---- set (direct, caller token) ---- */

static
int
CmdSet(ULONG Want)
{
    HANDLE Device = OpenEngineRaw();
    DWORD Rc;
    ULONG Mode;

    if (Device == INVALID_HANDLE_VALUE)
        return 2;
    wprintf(L"WFPMODE pid=%lu\n", GetCurrentProcessId());
    Rc = IoSetMode(Device, Want);
    Mode = QueryMode(Device);
    CloseHandle(Device);
    wprintf(L"WFPMODE set want=%lu rc=%lu mode=%lu\n", Want, Rc, Mode);
    return (int)Rc;
}

/* ---- set run inside the temporary LocalSystem service ---- */

static
VOID WINAPI
SvcCtrlHandler(DWORD Control)
{
    UNREFERENCED_PARAMETER(Control);
}

static
VOID WINAPI
SvcMain(DWORD Argc, LPWSTR *Argv)
{
    SERVICE_STATUS Status;
    HANDLE Device;
    DWORD Rc = ERROR_OPEN_FAILED;
    ULONG Mode = 0xFFFFFFFF;
    FILE *Out;

    UNREFERENCED_PARAMETER(Argc);
    UNREFERENCED_PARAMETER(Argv);

    ZeroMemory(&Status, sizeof(Status));
    Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    Status.dwCurrentState = SERVICE_RUNNING;

    g_SvcStatusHandle = RegisterServiceCtrlHandlerW(SVC_NAME, SvcCtrlHandler);
    if (g_SvcStatusHandle != NULL)
        SetServiceStatus(g_SvcStatusHandle, &Status);

    Device = OpenEngineRaw();
    if (Device != INVALID_HANDLE_VALUE)
    {
        Rc = IoSetMode(Device, g_SvcWant);
        Mode = QueryMode(Device);
        CloseHandle(Device);
    }

    Out = _wfopen(SVC_OUT_PATH, L"w, ccs=UTF-8");
    if (Out != NULL)
    {
        fwprintf(Out, L"WFPMODE pid=%lu\n", GetCurrentProcessId());
        fwprintf(Out, L"WFPMODE set want=%lu rc=%lu mode=%lu\n", g_SvcWant, Rc, Mode);
        fclose(Out);
    }

    Status.dwCurrentState = SERVICE_STOPPED;
    Status.dwWin32ExitCode = Rc;
    if (g_SvcStatusHandle != NULL)
        SetServiceStatus(g_SvcStatusHandle, &Status);
}

static
int
RunAsService(ULONG Want)
{
    SERVICE_TABLE_ENTRYW Table[2];

    g_SvcWant = Want;
    Table[0].lpServiceName = SVC_NAME;
    Table[0].lpServiceProc = SvcMain;
    Table[1].lpServiceName = NULL;
    Table[1].lpServiceProc = NULL;
    return StartServiceCtrlDispatcherW(Table) ? 0 : (int)GetLastError();
}

/* ---- svc set (parent: install, start, collect, delete) ---- */

static
int
CmdSvcSet(ULONG Want)
{
    SC_HANDLE Scm, Svc;
    SERVICE_STATUS Status;
    wchar_t Exe[MAX_PATH];
    wchar_t Bin[1024];
    DWORD Rc;
    int Tries;
    FILE *In;

    Scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (Scm == NULL)
    {
        wprintf(L"WFPMODE svc rc=%lu\n", GetLastError());
        return 2;
    }

    Svc = OpenServiceW(Scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (Svc != NULL)
    {
        DeleteService(Svc);
        CloseServiceHandle(Svc);
    }
    DeleteFileW(SVC_OUT_PATH);

    GetModuleFileNameW(NULL, Exe, MAX_PATH);
    _snwprintf(Bin, 1024, L"\"%s\" set %s --svc", Exe, Want ? L"on" : L"off");

    Svc = CreateServiceW(Scm, SVC_NAME, SVC_NAME, SERVICE_ALL_ACCESS,
                         SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
                         SERVICE_ERROR_IGNORE, Bin, NULL, NULL, NULL, NULL, NULL);
    if (Svc == NULL)
    {
        Rc = GetLastError();
        CloseServiceHandle(Scm);
        wprintf(L"WFPMODE svc rc=%lu\n", Rc);
        return 2;
    }

    ZeroMemory(&Status, sizeof(Status));
    if (!StartServiceW(Svc, 0, NULL))
    {
        Rc = GetLastError();
    }
    else
    {
        Rc = ERROR_SERVICE_REQUEST_TIMEOUT;
        for (Tries = 0; Tries < 100; Tries++)
        {
            if (QueryServiceStatus(Svc, &Status) &&
                Status.dwCurrentState == SERVICE_STOPPED)
            {
                Rc = Status.dwWin32ExitCode;
                break;
            }
            Sleep(100);
        }
    }

    In = _wfopen(SVC_OUT_PATH, L"r, ccs=UTF-8");
    if (In != NULL)
    {
        wchar_t Line[512];
        while (fgetws(Line, 512, In) != NULL)
            fputws(Line, stdout);
        fclose(In);
    }

    DeleteService(Svc);
    CloseServiceHandle(Svc);
    CloseServiceHandle(Scm);
    DeleteFileW(SVC_OUT_PATH);

    wprintf(L"WFPMODE svc rc=%lu\n", Rc);
    return (int)Rc;
}

/* ---- events ---- */

static
int
EnumEvents(HANDLE Device, PWFP_IOCTL_EVENT_HEADER *Header, PWFP_IOCTL_EVENT *Records, PBYTE Buffer, DWORD BufferSize)
{
    DWORD Bytes = 0;

    if (!DeviceIoControl(Device, IOCTL_WFP_EVENT_ENUM, NULL, 0,
                         Buffer, BufferSize, &Bytes, NULL) ||
        Bytes < sizeof(WFP_IOCTL_EVENT_HEADER))
    {
        return -1;
    }
    *Header = (PWFP_IOCTL_EVENT_HEADER)Buffer;
    *Records = (PWFP_IOCTL_EVENT)(Buffer + sizeof(WFP_IOCTL_EVENT_HEADER));
    return 0;
}

static
int
CmdEvents(LONG TypeFilter)
{
    static BYTE Buffer[256 * 1024];
    HANDLE Device = OpenEngineRaw();
    PWFP_IOCTL_EVENT_HEADER Header;
    PWFP_IOCTL_EVENT Event;
    ULONG Index;

    if (Device == INVALID_HANDLE_VALUE)
        return 2;
    if (EnumEvents(Device, &Header, &Event, Buffer, sizeof(Buffer)) != 0)
    {
        fwprintf(stderr, L"wfpmode: event-enum failed (error %lu)\n", GetLastError());
        CloseHandle(Device);
        return 2;
    }

    wprintf(L"WFPEVENTS dropped=%llu next=%llu count=%lu\n",
            (unsigned long long)Header->Dropped,
            (unsigned long long)Header->NextSequence, Header->Count);

    for (Index = 0; Index < Header->Count; Index++)
    {
        if (TypeFilter >= 0 && (LONG)Event[Index].Type != TypeFilter)
            continue;
        wprintf(L"WFPEVENT seq=%llu type=%lu mode=%lu layer=%lu pid=%lu filter=%llu "
                L"status=0x%08lx la=%lu ra=%lu lp=%u rp=%u proto=%u dir=%u image=%s\n",
                (unsigned long long)Event[Index].Sequence, Event[Index].Type,
                Event[Index].Mode, Event[Index].LayerId, Event[Index].ProcessId,
                (unsigned long long)Event[Index].FilterId, (unsigned long)Event[Index].Status,
                Event[Index].LocalAddress, Event[Index].RemoteAddress,
                Event[Index].LocalPort, Event[Index].RemotePort,
                Event[Index].Protocol, Event[Index].Direction,
                Event[Index].Image[0] ? Event[Index].Image : L"-");
    }
    CloseHandle(Device);
    return 0;
}

/* ---- stats ---- */

static
HANDLE
OpenEngineSession(VOID)
{
    HANDLE Device = OpenEngineRaw();
    WFP_IOCTL_OPEN Open;
    DWORD Bytes = 0;

    if (Device == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;
    ZeroMemory(&Open, sizeof(Open));
    if (!DeviceIoControl(Device, IOCTL_WFP_OPEN, &Open, sizeof(Open), NULL, 0, &Bytes, NULL))
    {
        fwprintf(stderr, L"wfpmode: engine open failed (error %lu)\n", GetLastError());
        CloseHandle(Device);
        return INVALID_HANDLE_VALUE;
    }
    return Device;
}

static
int
PrintStats(HANDLE Device, UINT64 FilterId)
{
    WFP_IOCTL_FILTER_STATS Stats;
    UINT64 Id = FilterId;
    DWORD Bytes = 0;

    if (!DeviceIoControl(Device, IOCTL_WFP_FILTER_GET_STATS, &Id, sizeof(Id),
                         &Stats, sizeof(Stats), &Bytes, NULL) || Bytes < sizeof(Stats))
    {
        wprintf(L"WFPSTATS filter=%llu hits=-1\n", (unsigned long long)FilterId);
        return (int)GetLastError();
    }
    wprintf(L"WFPSTATS filter=%llu hits=%llu\n",
            (unsigned long long)Stats.FilterId, (unsigned long long)Stats.Hits);
    return 0;
}

static
int
CmdStatsId(UINT64 FilterId)
{
    HANDLE Device = OpenEngineSession();
    int Rc;

    if (Device == INVALID_HANDLE_VALUE)
        return 2;
    Rc = PrintStats(Device, FilterId);
    CloseHandle(Device);
    return Rc;
}

static
int
CmdStatsFromEvents(VOID)
{
    static BYTE Buffer[256 * 1024];
    static UINT64 Seen[4096];
    HANDLE Device = OpenEngineSession();
    PWFP_IOCTL_EVENT_HEADER Header;
    PWFP_IOCTL_EVENT Event;
    ULONG Index, SeenCount = 0, I;

    if (Device == INVALID_HANDLE_VALUE)
        return 2;
    if (EnumEvents(Device, &Header, &Event, Buffer, sizeof(Buffer)) != 0)
    {
        CloseHandle(Device);
        return 2;
    }
    for (Index = 0; Index < Header->Count && SeenCount < 4096; Index++)
    {
        UINT64 Id = Event[Index].FilterId;
        BOOL Dup = FALSE;

        if (Id == 0)
            continue;
        for (I = 0; I < SeenCount; I++)
            if (Seen[I] == Id) { Dup = TRUE; break; }
        if (Dup)
            continue;
        Seen[SeenCount++] = Id;
        PrintStats(Device, Id);
    }
    CloseHandle(Device);
    return 0;
}

/* ---- filters hold ---- */

static
int
CmdFiltersHold(ULONG Count, BOOL BlockTop, USHORT Port, ULONG Seconds)
{
    FWPM_SESSION0 Session;
    FWPM_FILTER0 Filter;
    FWPM_FILTER_CONDITION0 Condition;
    HANDLE Engine = NULL;
    UINT64 First = 0, Last = 0, Id;
    UINT64 Weight;
    DWORD Error, Rc = ERROR_SUCCESS;
    ULONG Index;

    ZeroMemory(&Session, sizeof(Session));
    Session.displayData.name = L"wfpmode stage-0 hold session";
    Session.flags = FWPM_SESSION_FLAG_DYNAMIC;
    Error = FwpmEngineOpen0(NULL, RPC_C_AUTHN_WINNT, NULL, &Session, &Engine);
    if (Error != ERROR_SUCCESS)
    {
        wprintf(L"WFPFILTERS count=0 ids=0..0 rc=%lu\n", Error);
        return (int)Error;
    }

    ZeroMemory(&Condition, sizeof(Condition));
    Condition.fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
    Condition.matchType = FWP_MATCH_EQUAL;
    Condition.conditionValue.type = FWP_UINT16;
    Condition.conditionValue.uint16 = Port;

    for (Index = 1; Index <= Count; Index++)
    {
        ZeroMemory(&Filter, sizeof(Filter));
        Filter.layerKey = FWPM_LAYER_ALE_AUTH_CONNECT_V4;
        Filter.displayData.name = L"wfpmode stage-0 hold filter";
        /* FWP_UINT8 weight is a 0..15 index; use an absolute 64-bit weight so n>15 (over65) works */
        Weight = (UINT64)Index;
        Filter.weight.type = FWP_UINT64;
        Filter.weight.uint64 = &Weight;
        Filter.filterCondition = &Condition;
        Filter.numFilterConditions = 1;
        if (BlockTop && Index == Count)
            Filter.action.type = FWP_ACTION_BLOCK;
        else
            Filter.action.type = FWP_ACTION_PERMIT;

        Id = 0;
        Error = FwpmFilterAdd0(Engine, &Filter, NULL, &Id);
        if (Error != ERROR_SUCCESS)
        {
            Rc = Error;
            break;
        }
        if (Index == 1)
            First = Id;
        Last = Id;
    }

    wprintf(L"WFPFILTERS count=%lu ids=%llu..%llu rc=%lu\n",
            (BlockTop || Rc == ERROR_SUCCESS) ? Count : (Index - 1),
            (unsigned long long)First, (unsigned long long)Last, Rc);
    fflush(stdout);

    if (Rc == ERROR_SUCCESS && Seconds > 0)
        Sleep(Seconds * 1000);

    FwpmEngineClose0(Engine);
    return (int)Rc;
}

/* ---- connect ---- */

static
int
CmdConnect(USHORT Port)
{
    WSADATA WsaData;
    SOCKET Listener, Client;
    struct sockaddr_in Addr;
    int AddrLen = sizeof(Addr);
    int Rc = 0;

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    {
        wprintf(L"WFPCONNECT port=%u rc=%d\n", Port, WSAGetLastError());
        return 2;
    }

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Addr.sin_port = htons(Port);

    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Listener != INVALID_SOCKET &&
        bind(Listener, (struct sockaddr *)&Addr, sizeof(Addr)) == 0 &&
        listen(Listener, 1) == 0)
    {
        Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (Client != INVALID_SOCKET)
        {
            if (connect(Client, (struct sockaddr *)&Addr, AddrLen) != 0)
                Rc = WSAGetLastError();
            closesocket(Client);
        }
        else
        {
            Rc = WSAGetLastError();
        }
    }
    else
    {
        Rc = WSAGetLastError();
    }

    if (Listener != INVALID_SOCKET)
        closesocket(Listener);
    WSACleanup();

    wprintf(L"WFPCONNECT port=%u rc=%d\n", Port, Rc);
    return Rc;
}

/* ---- entry ---- */

static
BOOL
ArgIs(const wchar_t *A, const wchar_t *B)
{
    return _wcsicmp(A, B) == 0;
}

int
wmain(int argc, wchar_t **argv)
{
    int i;
    BOOL IsSvc = FALSE;

    for (i = 1; i < argc; i++)
        if (ArgIs(argv[i], L"--svc"))
            IsSvc = TRUE;

    if (argc >= 3 && ArgIs(argv[1], L"set"))
    {
        ULONG Want = ArgIs(argv[2], L"on") ? WfpModeOn : WfpModeOff;
        if (IsSvc)
            return RunAsService(Want);
        return CmdSet(Want);
    }
    if (argc >= 2 && ArgIs(argv[1], L"get"))
        return CmdGet();
    if (argc >= 4 && ArgIs(argv[1], L"svc") && ArgIs(argv[2], L"set"))
        return CmdSvcSet(ArgIs(argv[3], L"on") ? WfpModeOn : WfpModeOff);
    if (argc >= 2 && ArgIs(argv[1], L"events"))
    {
        LONG Type = -1;
        if (argc >= 4 && ArgIs(argv[2], L"--type"))
            Type = (LONG)_wtoi(argv[3]);
        return CmdEvents(Type);
    }
    if (argc >= 3 && ArgIs(argv[1], L"stats"))
    {
        if (ArgIs(argv[2], L"--from-events"))
            return CmdStatsFromEvents();
        return CmdStatsId(_wcstoui64(argv[2], NULL, 0));
    }
    if (argc >= 7 && ArgIs(argv[1], L"filters") && ArgIs(argv[2], L"hold") && ArgIs(argv[5], L"tcp"))
    {
        ULONG N = (ULONG)_wtoi(argv[3]);
        BOOL BlockTop = ArgIs(argv[4], L"block-top");
        USHORT Port = (USHORT)_wtoi(argv[6]);
        ULONG Seconds = (argc >= 8) ? (ULONG)_wtoi(argv[7]) : 0;
        return CmdFiltersHold(N, BlockTop, Port, Seconds);
    }
    if (argc >= 3 && ArgIs(argv[1], L"connect"))
        return CmdConnect((USHORT)_wtoi(argv[2]));

    wprintf(L"usage: wfpmode get | set <off|on> | svc set <off|on> | events [--type <t>] |\n"
            L"               stats <filterId> | stats --from-events |\n"
            L"               filters hold <n> <permit|block-top> tcp <port> <seconds> | connect <port>\n");
    return 1;
}
