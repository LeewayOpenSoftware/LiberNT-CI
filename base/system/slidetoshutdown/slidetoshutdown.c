/*
 * PROJECT:     Orosi SlideToShutDown
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Slide the lock picture down over the dimmed desktop to shut the PC down
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS
#include <windows.h>
#include <strsafe.h>
#include <wine/debug.h>
#include "gfx.h"
#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(slidetoshutdown);

#define STS_CLASS           L"Orosi_SlideToShutDown"
#define STS_MUTEX           L"Local\\Orosi_SlideToShutDown"
#define STS_COVER_PERCENT   52      /* the curtain at rest covers the top 52 % (Win11 shot 16) */
#define STS_DIM             153     /* the desktop under it is dimmed by about 60 % */
#define STS_IN_MS           350     /* comes down with an ease-out (Win11 slide frames) */
#define STS_MOVE_MS         300     /* any later move over the whole screen height */
#define STS_MIN_MS          100
#define STS_FLICK           0.6     /* pixels per millisecond at release that decide by themselves */
#define WM_STS_START        (WM_APP + 1)
#define STS_LOCK_POLICY_KEY L"SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization"
#define STS_LOCK_DEFAULT    L"%SystemRoot%\\Web\\Screen\\Midnight Glass Waves.png"

typedef struct _STS
{
    HWND Window;
    LONG Width;
    LONG Height;
    LONG Rest;              /* the curtain's bottom edge at rest */
    LONG Y;                 /* the curtain's bottom edge now */
    LUH_SURFACE Curtain;    /* the lock picture with the two lines and the arrow near its bottom */
    LUH_SURFACE Desk;       /* the desktop as it was, dimmed */
    BOOL Dragging;
    LONG DragStartY;
    LONG DragStartEdge;
    LONG DragLastY;
    DWORD DragLastTime;
    double Velocity;        /* pixels per millisecond, positive downwards */
    LUH_ANIM DragAnim;
    BOOL Leaving;
} STS;

static STS g_Sts;

VOID
LuhStage(
    _In_ PCSTR Line)
{
    static LONG Enabled = -1;
    DWORD Value = 0, Size = sizeof(Value);
    CHAR Text[192];

    /* The LogonUI lab switch: the frame lines of this curtain go to the same trace. */
    if (Enabled < 0)
    {
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                         L"LiberNTStageTrace", RRF_RT_REG_DWORD, NULL, &Value, &Size) != ERROR_SUCCESS)
        {
            Value = 0;
        }
        Enabled = (Value != 0);
    }
    TRACE("%s\n", Line);
    if (!Enabled)
        return;
    StringCchPrintfA(Text, ARRAYSIZE(Text), "LOGONSTAGE %lu slidetoshutdown %s\n", GetTickCount(), Line);
    OutputDebugStringA(Text);
}

/* The machine's lock picture, as the lock screen shows it: the policy image, else the default. */
static BOOL
StsLoadPicture(
    _Out_ PLUH_SURFACE Picture)
{
    WCHAR Path[MAX_PATH];
    DWORD Size = sizeof(Path);
    HANDLE File;
    BOOL Ok;

    if (RegGetValueW(HKEY_LOCAL_MACHINE, STS_LOCK_POLICY_KEY, L"LockScreenImage",
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, Path, &Size) != ERROR_SUCCESS ||
        Path[0] == UNICODE_NULL)
    {
        Size = ExpandEnvironmentStringsW(STS_LOCK_DEFAULT, Path, ARRAYSIZE(Path));
        if (Size == 0 || Size > ARRAYSIZE(Path))
            return FALSE;
    }
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = LuhSurfaceLoad(Picture, File, 2 * g_Sts.Width, 2 * g_Sts.Height);
    CloseHandle(File);
    return Ok;
}

/* Built once: the curtain picture and the dimmed desktop under it. Nothing is drawn per frame. */
static BOOL
StsBuild(VOID)
{
    RECT All = { 0, 0, g_Sts.Width, g_Sts.Height }, Rect;
    LUH_SURFACE Picture;
    HFONT Small, Large;
    POINT Arrow[3];
    HGDIOBJ OldBrush, OldPen;
    HDC Screen;
    LONG Bottom = g_Sts.Height, Cx = g_Sts.Width / 2;
    WCHAR Text[256];

    if (!LuhSurfaceCreate(&g_Sts.Curtain, g_Sts.Width, g_Sts.Height) ||
        !LuhSurfaceCreate(&g_Sts.Desk, g_Sts.Width, g_Sts.Height))
        return FALSE;

    Screen = GetDC(NULL);
    if (Screen == NULL)
        return FALSE;
    BitBlt(g_Sts.Desk.Dc, 0, 0, g_Sts.Width, g_Sts.Height, Screen, 0, 0, SRCCOPY);
    ReleaseDC(NULL, Screen);
    LuhFillRect(&g_Sts.Desk, &All, RGB(0, 0, 0), STS_DIM);

    if (StsLoadPicture(&Picture))
    {
        LuhSurfaceScaleFrom(&g_Sts.Curtain, &Picture);
        LuhSurfaceDestroy(&Picture);
    }
    else
    {
        LuhFillRect(&g_Sts.Curtain, &All, RGB(18, 58, 112), 255);
    }

    /* Measured at 1024x768 against the curtain's bottom edge at rest (y 400): warning y 293,
     * prompt y 330, arrow y 368; the curtain is drawn bottom-aligned, so these follow it. */
    Small = LuhFontCreate(14, FW_NORMAL);
    Large = LuhFontCreate(24, FW_NORMAL);
    LoadStringW(GetModuleHandleW(NULL), IDS_STS_WARNING, Text, ARRAYSIZE(Text));
    SetRect(&Rect, 0, Bottom - LuhPx(107) - LuhPx(12), g_Sts.Width, Bottom - LuhPx(107) + LuhPx(12));
    LuhDrawText(&g_Sts.Curtain, Small, Text, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                RGB(255, 255, 255), 255);
    LoadStringW(GetModuleHandleW(NULL), IDS_STS_SLIDE, Text, ARRAYSIZE(Text));
    SetRect(&Rect, 0, Bottom - LuhPx(70) - LuhPx(20), g_Sts.Width, Bottom - LuhPx(70) + LuhPx(20));
    LuhDrawText(&g_Sts.Curtain, Large, Text, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                RGB(255, 255, 255), 255);
    if (Small) DeleteObject(Small);
    if (Large) DeleteObject(Large);

    /* The down arrow: a solid triangle; the curtain is presented opaque, so plain GDI fill is enough. */
    Arrow[0].x = Cx - LuhPx(11); Arrow[0].y = Bottom - LuhPx(41);
    Arrow[1].x = Cx + LuhPx(11); Arrow[1].y = Bottom - LuhPx(41);
    Arrow[2].x = Cx;             Arrow[2].y = Bottom - LuhPx(22);
    GdiFlush();
    OldBrush = SelectObject(g_Sts.Curtain.Dc, GetStockObject(WHITE_BRUSH));
    OldPen = SelectObject(g_Sts.Curtain.Dc, GetStockObject(WHITE_PEN));
    Polygon(g_Sts.Curtain.Dc, Arrow, 3);
    SelectObject(g_Sts.Curtain.Dc, OldPen);
    SelectObject(g_Sts.Curtain.Dc, OldBrush);
    GdiFlush();
    return TRUE;
}

/* One frame: the curtain's lower part above its bottom edge Y, the dimmed desktop under it. */
static VOID
StsBlit(
    _In_ HDC Dc,
    _In_ LONG Y)
{
    if (Y > 0)
        BitBlt(Dc, 0, 0, g_Sts.Width, Y, g_Sts.Curtain.Dc, 0, g_Sts.Height - Y, SRCCOPY);
    if (Y < g_Sts.Height)
        BitBlt(Dc, 0, Y, g_Sts.Width, g_Sts.Height - Y, g_Sts.Desk.Dc, 0, Y, SRCCOPY);
}

static VOID
StsPresent(
    _In_ LONG Y)
{
    HDC Dc;

    g_Sts.Y = min(max(Y, 0), g_Sts.Height);
    Dc = GetDC(g_Sts.Window);
    if (Dc == NULL)
        return;
    StsBlit(Dc, g_Sts.Y);
    GdiFlush();
    ReleaseDC(g_Sts.Window, Dc);
}

/* Moves the curtain's edge to To on the clock with an ease-out, matched to a release speed. */
static VOID
StsSlide(
    _In_ LONG To,
    _In_ DWORD FullMs,
    _In_ double Velocity,
    _In_ PCSTR Name)
{
    LONG From = g_Sts.Y, Distance = To > From ? To - From : From - To;
    LUH_ANIM Anim;
    DWORD Ms;
    double T;

    if (Distance == 0)
        return;
    Ms = (DWORD)(((LONGLONG)FullMs * Distance) / max(g_Sts.Height, 1));
    if (Velocity > 0.0 && (DWORD)(3.0 * Distance / Velocity) < Ms)
        Ms = (DWORD)(3.0 * Distance / Velocity);
    LuhAnimStart(&Anim, Name, max(Ms, STS_MIN_MS));
    for (;;)
    {
        T = LuhAnimFrame(&Anim);
        StsPresent(LuhLerp(From, To, LuhEaseOut(T)));
        if (T >= 1.0)
            break;
        LuhAnimWait(&Anim);
    }
    LuhAnimEnd(&Anim);
}

/* One line above the warning, in the curtain picture itself, so it moves with the curtain. */
static VOID
StsShowFailure(VOID)
{
    WCHAR Text[256];
    HFONT Font;
    RECT Rect;

    if (!LoadStringW(GetModuleHandleW(NULL), IDS_STS_FAILED, Text, ARRAYSIZE(Text)))
        return;
    Font = LuhFontCreate(14, FW_SEMIBOLD);
    SetRect(&Rect, 0, g_Sts.Height - LuhPx(145) - LuhPx(12), g_Sts.Width, g_Sts.Height - LuhPx(145) + LuhPx(12));
    LuhDrawText(&g_Sts.Curtain, Font, Text, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                RGB(255, 255, 255), 255);
    if (Font)
        DeleteObject(Font);
    LuhStage("sts-shutdown-refused");
}

static VOID
StsShutDown(VOID)
{
    HANDLE Token;
    TOKEN_PRIVILEGES Privileges;

    StsSlide(g_Sts.Height, STS_MOVE_MS, 0.0, "sts-shutdown");
    g_Sts.Leaving = TRUE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token))
    {
        Privileges.PrivilegeCount = 1;
        Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &Privileges.Privileges[0].Luid))
            AdjustTokenPrivileges(Token, FALSE, &Privileges, 0, NULL, NULL);
        CloseHandle(Token);
    }
    if (ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF,
                      SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
    {
        /* Asked; the session ends from here, or an app or the user cancels it. Either way this
         * window must not stay over the desktop or the end-session prompts: leave now, as
         * Windows' SlideToShutDown does. */
        DestroyWindow(g_Sts.Window);
        return;
    }

    /* Refused (no privilege, a policy): say so on the curtain and bring it back to rest. */
    ERR("ExitWindowsEx failed (%lu)\n", GetLastError());
    StsShowFailure();
    g_Sts.Leaving = FALSE;
    StsSlide(g_Sts.Rest, STS_MOVE_MS, 0.0, "sts-return");
}

/* Back up and away: Esc, a click on the desktop, the window losing the focus, a flick up. */
static VOID
StsDismiss(
    _In_ double Velocity)
{
    if (g_Sts.Leaving)
        return;
    g_Sts.Leaving = TRUE;
    StsSlide(0, STS_MOVE_MS, Velocity, "sts-dismiss");
    DestroyWindow(g_Sts.Window);
}

static VOID
StsDragEnd(VOID)
{
    double Velocity = g_Sts.Velocity;

    g_Sts.Dragging = FALSE;
    ReleaseCapture();
    LuhAnimEnd(&g_Sts.DragAnim);
    if (GetMessageTime() - g_Sts.DragLastTime > 100)
        Velocity = 0.0;

    /* Down past a third of the way to the bottom, or a flick down: shut down. Up past half the
     * rest position, or a flick up: dismiss. Otherwise back to rest. */
    if (g_Sts.Y > g_Sts.Rest + (g_Sts.Height - g_Sts.Rest) / 3 || Velocity > STS_FLICK)
        StsShutDown();
    else if (g_Sts.Y < g_Sts.Rest / 2 || Velocity < -STS_FLICK)
        StsDismiss(-Velocity);
    else
        StsSlide(g_Sts.Rest, STS_MOVE_MS, 0.0, "sts-return");
}

static LRESULT CALLBACK
StsWindowProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    PAINTSTRUCT Ps;
    LONG Y;

    switch (Message)
    {
        case WM_PAINT:
            BeginPaint(Window, &Ps);
            StsBlit(Ps.hdc, g_Sts.Y);
            EndPaint(Window, &Ps);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_STS_START:
            StsSlide(g_Sts.Rest, STS_IN_MS * g_Sts.Height / max(g_Sts.Rest, 1), 0.0, "sts-in");
            return 0;

        case WM_LBUTTONDOWN:
            if (g_Sts.Leaving)
                return 0;
            Y = (SHORT)HIWORD(lParam);
            if (Y >= g_Sts.Y)
            {
                StsDismiss(0.0);
                return 0;
            }
            g_Sts.Dragging = TRUE;
            g_Sts.DragStartY = Y;
            g_Sts.DragStartEdge = g_Sts.Y;
            g_Sts.DragLastY = Y;
            g_Sts.DragLastTime = GetMessageTime();
            g_Sts.Velocity = 0.0;
            SetCapture(Window);
            LuhAnimStart(&g_Sts.DragAnim, "sts-drag", 0);
            return 0;

        case WM_MOUSEMOVE:
            if (g_Sts.Dragging)
            {
                DWORD Now = GetMessageTime(), Elapsed = Now - g_Sts.DragLastTime;

                Y = (SHORT)HIWORD(lParam);
                if (Elapsed > 0)
                {
                    g_Sts.Velocity = 0.6 * ((double)(Y - g_Sts.DragLastY) / Elapsed) + 0.4 * g_Sts.Velocity;
                    g_Sts.DragLastY = Y;
                    g_Sts.DragLastTime = Now;
                }
                LuhAnimFrame(&g_Sts.DragAnim);
                StsPresent(g_Sts.DragStartEdge + (Y - g_Sts.DragStartY));
            }
            return 0;

        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (g_Sts.Dragging)
                StsDragEnd();
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
                StsDismiss(0.0);
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && !g_Sts.Dragging)
                StsDismiss(0.0);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(Window, Message, wParam, lParam);
}

int WINAPI
wWinMain(
    _In_ HINSTANCE Instance,
    _In_opt_ HINSTANCE Previous,
    _In_ LPWSTR CommandLine,
    _In_ int Show)
{
    WNDCLASSEXW Class;
    HANDLE Mutex;
    MSG Msg;

    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CommandLine);
    UNREFERENCED_PARAMETER(Show);

    /* One curtain at a time. */
    Mutex = CreateMutexW(NULL, TRUE, STS_MUTEX);
    if (Mutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    LuhGfxInit();
    ZeroMemory(&g_Sts, sizeof(g_Sts));
    g_Sts.Width = GetSystemMetrics(SM_CXSCREEN);
    g_Sts.Height = GetSystemMetrics(SM_CYSCREEN);
    g_Sts.Rest = MulDiv(g_Sts.Height, STS_COVER_PERCENT, 100);
    if (!StsBuild())
        return 1;

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = StsWindowProc;
    Class.hInstance = Instance;
    Class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    Class.lpszClassName = STS_CLASS;
    if (!RegisterClassExW(&Class))
        return 1;
    g_Sts.Window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, STS_CLASS, L"", WS_POPUP | WS_VISIBLE,
                                   0, 0, g_Sts.Width, g_Sts.Height, NULL, NULL, Instance, NULL);
    if (g_Sts.Window == NULL)
        return 1;
    SetForegroundWindow(g_Sts.Window);
    UpdateWindow(g_Sts.Window);
    /* The first frame is the dimmed desktop; the curtain comes down once it is on screen. */
    PostMessageW(g_Sts.Window, WM_STS_START, 0, 0);

    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }

    LuhSurfaceDestroy(&g_Sts.Curtain);
    LuhSurfaceDestroy(&g_Sts.Desk);
    LuhGfxShutdown();
    CloseHandle(Mutex);
    return (int)Msg.wParam;
}
