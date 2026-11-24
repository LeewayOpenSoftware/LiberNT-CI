/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The host window: status, Welcome, spinner, fade-out
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "logonuihost.h"
#include <math.h>

WINE_DEFAULT_DEBUG_CHANNEL(logonuihost);

#define LUH_CLASS           L"LiberNT_LogonUIHost"
#define LUH_TIMER_HEARTBEAT 1
#define LUH_TIMER_SPINNER   2
#define LUH_TIMER_FADE      3
#define LUH_SPINNER_MS      50

typedef struct _LUH_UI
{
    HFONT TitleFont;
    HFONT TextFont;
    WCHAR Title[256];       /* "Welcome" user or status text */
    WCHAR Detail[256];      /* result text, when any */
    BOOL Spinner;
    int SpinnerAngle;
    BOOL FirstPaintDone;
    BOOL Fading;
    int FadeStep;
    WLUI_EXIT_REASON ExitReason;
    int ExitCode;
    BOOL Exiting;
} LUH_UI;

static LUH_UI g_Ui;

static void
LuhBeginExit(
    _In_ HWND Window,
    _In_ WLUI_EXIT_REASON Reason,
    _In_ int ExitCode)
{
    if (g_Ui.Exiting)
        return;
    g_Ui.Exiting = TRUE;
    g_Ui.ExitReason = Reason;
    g_Ui.ExitCode = ExitCode;
    LuhIpcExiting(Reason);
    DestroyWindow(Window);
}

static void
LuhSetSpinner(
    _In_ HWND Window,
    _In_ BOOL On)
{
    if (On && !g_Ui.Spinner)
        SetTimer(Window, LUH_TIMER_SPINNER, LUH_SPINNER_MS, NULL);
    else if (!On && g_Ui.Spinner)
        KillTimer(Window, LUH_TIMER_SPINNER);
    g_Ui.Spinner = On;
}

static void
LuhHandleCommand(
    _In_ HWND Window,
    _In_ const WLUI_COMMAND *Command)
{
    WCHAR Num[16];

    StringCchPrintfW(Num, ARRAYSIZE(Num), L"%u", Command->Type);
    LuhTrace("command", Num);

    switch (Command->Type)
    {
        case WluiCmdDisplayStatus:
            StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), Command->Text);
            g_Ui.Detail[0] = UNICODE_NULL;
            LuhSetSpinner(Window, (Command->Flags & WLUI_FLAG_SPINNER) != 0);
            LuhTrace("status", g_Ui.Title);
            InvalidateRect(Window, NULL, TRUE);
            break;

        case WluiCmdDisplayWelcome:
            StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), L"Welcome");
            StringCchCopyW(g_Ui.Detail, ARRAYSIZE(g_Ui.Detail), Command->User);
            LuhSetSpinner(Window, TRUE);
            LuhTrace("welcome", g_Ui.Detail);
            InvalidateRect(Window, NULL, TRUE);
            break;

        case WluiCmdReportResult:
            StringCchCopyW(g_Ui.Detail, ARRAYSIZE(g_Ui.Detail), Command->Text);
            LuhSetSpinner(Window, FALSE);
            LuhTrace("result", g_Ui.Detail);
            InvalidateRect(Window, NULL, TRUE);
            break;

        case WluiCmdReadyForDesktopSwitch:
            /* The desktop behind is painted: fade out, then leave (§6.3). */
            if (!g_Ui.Fading)
            {
                g_Ui.Fading = TRUE;
                g_Ui.FadeStep = 0;
                LuhSetSpinner(Window, FALSE);
                SetTimer(Window, LUH_TIMER_FADE, LUH_FADE_MS / LUH_FADE_STEPS, NULL);
            }
            break;

        case WluiCmdDismiss:
            LuhBeginExit(Window, WluiExitDismissed, LUH_EXIT_OK);
            break;

        case WluiCmdRequestCredentials:
            /* Providers and credential fields arrive with the next commit; until then
             * the honest answer is a provider-init failure, never a fake credential page. */
            LuhBeginExit(Window, WluiExitProviderInitFailed, LUH_EXIT_NOT_SUPPORTED);
            break;

        default:
            WARN("unknown command %u ignored\n", Command->Type);
            break;
    }
}

static void
LuhPaint(
    _In_ HWND Window)
{
    PAINTSTRUCT Ps;
    HDC Dc;
    RECT Client, Rect;
    HGDIOBJ OldFont;

    Dc = BeginPaint(Window, &Ps);
    GetClientRect(Window, &Client);

    FillRect(Dc, &Client, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(Dc, TRANSPARENT);
    SetTextColor(Dc, RGB(255, 255, 255));

    Rect = Client;
    Rect.top = Client.bottom / 2 - 40;
    Rect.bottom = Rect.top + 40;
    OldFont = SelectObject(Dc, g_Ui.TitleFont);
    DrawTextW(Dc, g_Ui.Title, -1, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (g_Ui.Detail[0] != UNICODE_NULL)
    {
        Rect.top = Rect.bottom + 8;
        Rect.bottom = Rect.top + 28;
        SelectObject(Dc, g_Ui.TextFont);
        DrawTextW(Dc, g_Ui.Detail, -1, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (g_Ui.Spinner)
    {
        /* Eight dots on a circle, one highlighted per tick. */
        int Cx = Client.right / 2, Cy = Rect.bottom + 48, i;
        for (i = 0; i < 8; i++)
        {
            double A = (g_Ui.SpinnerAngle + i * 45) * 3.14159265 / 180.0;
            int X = Cx + (int)(18.0 * cos(A)), Y = Cy + (int)(18.0 * sin(A));
            int Gray = 60 + i * 24;
            HBRUSH B = CreateSolidBrush(RGB(Gray, Gray, Gray));
            RECT Dot = { X - 3, Y - 3, X + 3, Y + 3 };
            FillRect(Dc, &Dot, B);
            DeleteObject(B);
        }
    }

    SelectObject(Dc, OldFont);
    EndPaint(Window, &Ps);

    if (!g_Ui.FirstPaintDone)
    {
        /* Ready goes out only after the first frame is on screen (§6.3). */
        g_Ui.FirstPaintDone = TRUE;
        GdiFlush();
        LuhIpcReady();
    }
}

static LRESULT CALLBACK
LuhWindowProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (Message)
    {
        case WM_CREATE:
            SetTimer(Window, LUH_TIMER_HEARTBEAT, LUH_HEARTBEAT_MS, NULL);
            return 0;

        case WM_PAINT:
            LuhPaint(Window);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_TIMER:
            if (wParam == LUH_TIMER_HEARTBEAT)
            {
                LuhIpcHeartbeat();
            }
            else if (wParam == LUH_TIMER_SPINNER)
            {
                g_Ui.SpinnerAngle = (g_Ui.SpinnerAngle + 45) % 360;
                InvalidateRect(Window, NULL, FALSE);
            }
            else if (wParam == LUH_TIMER_FADE)
            {
                g_Ui.FadeStep++;
                if (g_Ui.FadeStep >= LUH_FADE_STEPS)
                {
                    KillTimer(Window, LUH_TIMER_FADE);
                    LuhBeginExit(Window, WluiExitAfterDesktopSwitch, LUH_EXIT_OK);
                }
                else
                {
                    SetLayeredWindowAttributes(Window, 0,
                        (BYTE)(255 - (255 * g_Ui.FadeStep) / LUH_FADE_STEPS), LWA_ALPHA);
                }
            }
            return 0;

        case WM_LUH_COMMAND:
        {
            WLUI_COMMAND *Command = (WLUI_COMMAND *)lParam;
            if (Command)
            {
                LuhHandleCommand(Window, Command);
                SecureZeroMemory(Command, sizeof(*Command));
                HeapFree(GetProcessHeap(), 0, Command);
            }
            return 0;
        }

        case WM_LUH_IPC_LOST:
            /* 995 means winlogon ended this launch; anything else is a dead channel. */
            if (!g_Ui.Exiting)
            {
                g_Ui.Exiting = TRUE;
                g_Ui.ExitCode = (wParam == ERROR_OPERATION_ABORTED) ? LUH_EXIT_OK : LUH_EXIT_IPC_FAILED;
                DestroyWindow(Window);
            }
            return 0;

        case WM_CLOSE:
            /* Only winlogon closes this window, through Dismiss. */
            return 0;

        case WM_DESTROY:
            KillTimer(Window, LUH_TIMER_HEARTBEAT);
            KillTimer(Window, LUH_TIMER_SPINNER);
            KillTimer(Window, LUH_TIMER_FADE);
            PostQuitMessage(g_Ui.ExitCode);
            return 0;
    }
    return DefWindowProcW(Window, Message, wParam, lParam);
}

HWND
LuhUiCreate(
    _In_ HINSTANCE Instance)
{
    WNDCLASSEXW Class;
    HWND Window;
    int Width = GetSystemMetrics(SM_CXSCREEN), Height = GetSystemMetrics(SM_CYSCREEN);

    ZeroMemory(&g_Ui, sizeof(g_Ui));
    StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), L"");
    g_Ui.TitleFont = CreateFontW(-32, 0, 0, 0, FW_LIGHT, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_Ui.TextFont = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = LuhWindowProc;
    Class.hInstance = Instance;
    Class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    Class.lpszClassName = LUH_CLASS;
    if (!RegisterClassExW(&Class))
        return NULL;

    /* Layered so the desktop-switch fade can drop the alpha; top-most on its desktop. */
    Window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE, LUH_CLASS, L"",
                             WS_POPUP | WS_VISIBLE, 0, 0, Width, Height, NULL, NULL, Instance, NULL);
    if (Window == NULL)
        return NULL;
    SetLayeredWindowAttributes(Window, 0, 255, LWA_ALPHA);
    UpdateWindow(Window);
    return Window;
}

int
LuhUiRun(
    _In_ HWND Window)
{
    MSG Msg;

    UNREFERENCED_PARAMETER(Window);

    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }

    if (g_Ui.TitleFont) DeleteObject(g_Ui.TitleFont);
    if (g_Ui.TextFont) DeleteObject(g_Ui.TextFont);
    return (int)Msg.wParam;
}
