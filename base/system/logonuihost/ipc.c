/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     wlui client side: connection, command pull thread, heartbeat
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "logonuihost.h"

WINE_DEFAULT_DEBUG_CHANNEL(logonuihost);

static HANDLE g_PullThread;
static HANDLE g_StopEvent;
static HWND g_Window;
static LONG g_IpcLostPosted;

BOOL
LuhIpcConnect(VOID)
{
    RPC_STATUS Status;

    Status = WluiConnect(g_Options.Endpoint, (PSID)g_Options.ServerSid, &g_Options.LaunchId, &g_Connection);
    if (Status != RPC_S_OK)
    {
        ERR("WluiConnect failed %lu\n", Status);
        return FALSE;
    }
    return TRUE;
}

static VOID
LuhIpcLost(
    _In_ DWORD Error)
{
    WCHAR Text[16];

    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu", Error);
    LuhTrace("ipc-lost", Text);
    if (InterlockedExchange(&g_IpcLostPosted, 1) == 0 && g_Window)
        PostMessageW(g_Window, WM_LUH_IPC_LOST, Error, 0);
}

/* Pulls commands until the stop event is set or the channel fails. */
static DWORD WINAPI
LuhPullThread(
    _In_ LPVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);

    while (WaitForSingleObject(g_StopEvent, 0) != WAIT_OBJECT_0)
    {
        WLUI_COMMAND *Command = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Command));
        DWORD Error;

        if (Command == NULL)
        {
            LuhIpcLost(ERROR_OUTOFMEMORY);
            break;
        }

        Error = WluiClientWaitForCommand(&g_Connection, Command);
        if (Error != ERROR_SUCCESS)
        {
            HeapFree(GetProcessHeap(), 0, Command);
            if (Error == ERROR_BUSY)
            {
                Sleep(100);     /* another wait is outstanding; never spin */
                continue;
            }
            LuhIpcLost(Error);  /* 995: the launch ended; anything else: channel gone */
            break;
        }

        if (Command->Type == WluiCmdNone)
        {
            HeapFree(GetProcessHeap(), 0, Command);
            continue;
        }

        /* Fixed-size wire strings are not guaranteed terminated. */
        Command->Text[ARRAYSIZE(Command->Text) - 1] = UNICODE_NULL;
        Command->User[ARRAYSIZE(Command->User) - 1] = UNICODE_NULL;

        if (!PostMessageW(g_Window, WM_LUH_COMMAND, 0, (LPARAM)Command))
        {
            SecureZeroMemory(Command, sizeof(*Command));
            HeapFree(GetProcessHeap(), 0, Command);
            break;
        }
    }
    return 0;
}

BOOL
LuhIpcStart(
    _In_ HWND Window)
{
    g_Window = Window;
    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_StopEvent == NULL)
        return FALSE;
    g_PullThread = CreateThread(NULL, 0, LuhPullThread, NULL, 0, NULL);
    return g_PullThread != NULL;
}

VOID
LuhIpcStop(VOID)
{
    if (g_StopEvent)
        SetEvent(g_StopEvent);
    if (g_PullThread)
    {
        /* The thread may sit inside a 30 s server wait; the process is leaving anyway. */
        WaitForSingleObject(g_PullThread, 500);
        CloseHandle(g_PullThread);
        g_PullThread = NULL;
    }
    if (g_StopEvent)
    {
        CloseHandle(g_StopEvent);
        g_StopEvent = NULL;
    }
    WluiDisconnect(&g_Connection);
}

VOID
LuhIpcHeartbeat(VOID)
{
    DWORD Error = WluiClientHeartbeat(&g_Connection);
    if (Error != ERROR_SUCCESS)
        LuhIpcLost(Error);
}

VOID
LuhIpcReady(VOID)
{
    DWORD Error = WluiClientReady(&g_Connection);
    LuhTrace("ready", NULL);
    if (Error != ERROR_SUCCESS)
        LuhIpcLost(Error);
}

VOID
LuhIpcExiting(
    _In_ WLUI_EXIT_REASON Reason)
{
    WCHAR Text[16];

    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%u", Reason);
    LuhTrace("exiting", Text);
    if (g_Connection.Binding != NULL)
        WluiClientExiting(&g_Connection, Reason);
}
