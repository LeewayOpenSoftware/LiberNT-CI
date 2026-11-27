/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Crossfade across a desktop switch: the last frame of the old desktop covers the
 *              new one and fades out over it (INVESTIGACION.md 6.3, 6.4).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "winlogon.h"

#define WL_COVER_CLASS      L"WinlogonTransitionCover"
#define WL_COVER_FADE_MS    500
#define WL_COVER_STEPS      20
#define WL_COVER_SHOWN_MS   1000
#define WL_COVER_HOLD_MS    5000    /* a cover nobody releases fades by itself */

struct _WL_COVER
{
    HDESK Desktop;
    HBITMAP Frame;
    RECT Screen;
    HANDLE Shown;
    HANDLE Release;
    HANDLE Thread;
    BOOL Ready;
};

typedef struct _WL_CAPTURE
{
    HDESK Desktop;
    RECT Screen;
    HBITMAP Frame;
} WL_CAPTURE;

static
VOID
WlCoverScreenRect(
    _Out_ PRECT Screen)
{
    Screen->left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    Screen->top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    Screen->right = Screen->left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    Screen->bottom = Screen->top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (Screen->right <= Screen->left || Screen->bottom <= Screen->top)
        SetRect(Screen, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}

/* The screen can only be read from a thread on the input desktop. */
static
DWORD
WINAPI
WlCaptureThread(
    _In_ PVOID Parameter)
{
    WL_CAPTURE *Capture = Parameter;
    HDC Screen, Memory;
    HGDIOBJ Old;
    int Width = Capture->Screen.right - Capture->Screen.left;
    int Height = Capture->Screen.bottom - Capture->Screen.top;

    if (!SetThreadDesktop(Capture->Desktop))
        return 1;

    Screen = GetDC(NULL);
    if (Screen == NULL)
        return 1;
    Memory = CreateCompatibleDC(Screen);
    if (Memory != NULL)
    {
        Capture->Frame = CreateCompatibleBitmap(Screen, Width, Height);
        if (Capture->Frame != NULL)
        {
            Old = SelectObject(Memory, Capture->Frame);
            if (!BitBlt(Memory, 0, 0, Width, Height, Screen,
                        Capture->Screen.left, Capture->Screen.top, SRCCOPY | CAPTUREBLT))
            {
                SelectObject(Memory, Old);
                DeleteObject(Capture->Frame);
                Capture->Frame = NULL;
            }
            else
            {
                SelectObject(Memory, Old);
            }
        }
        DeleteDC(Memory);
    }
    ReleaseDC(NULL, Screen);
    return 0;
}

static
LRESULT
CALLBACK
WlCoverProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    PWL_COVER Cover = (PWL_COVER)GetWindowLongPtrW(Window, GWLP_USERDATA);

    switch (Message)
    {
        case WM_NCCREATE:
            SetWindowLongPtrW(Window, GWLP_USERDATA,
                              (LONG_PTR)((LPCREATESTRUCTW)lParam)->lpCreateParams);
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_PAINT:
        {
            PAINTSTRUCT Paint;
            HDC Dc = BeginPaint(Window, &Paint);
            HDC Memory = CreateCompatibleDC(Dc);

            if (Memory != NULL && Cover != NULL)
            {
                HGDIOBJ Old = SelectObject(Memory, Cover->Frame);
                BitBlt(Dc, 0, 0, Cover->Screen.right - Cover->Screen.left,
                       Cover->Screen.bottom - Cover->Screen.top, Memory, 0, 0, SRCCOPY);
                SelectObject(Memory, Old);
            }
            if (Memory != NULL)
                DeleteDC(Memory);
            EndPaint(Window, &Paint);
            return 0;
        }
    }
    return DefWindowProcW(Window, Message, wParam, lParam);
}

/* Pumps the cover thread's messages until Event is set or Ms pass. */
static
BOOL
WlCoverWait(
    _In_opt_ HANDLE Event,
    _In_ DWORD Ms)
{
    ULONGLONG End = GetTickCount64() + Ms;
    MSG Msg;

    for (;;)
    {
        ULONGLONG Now = GetTickCount64();
        DWORD Left = (Now < End) ? (DWORD)(End - Now) : 0;
        DWORD Result = MsgWaitForMultipleObjects(Event ? 1 : 0, Event ? &Event : NULL, FALSE, Left, QS_ALLINPUT);

        if (Event != NULL && Result == WAIT_OBJECT_0)
            return TRUE;
        if (Result == WAIT_TIMEOUT || Result == WAIT_FAILED)
            return FALSE;
        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
    }
}

static
DWORD
WINAPI
WlCoverThread(
    _In_ PVOID Parameter)
{
    PWL_COVER Cover = Parameter;
    WNDCLASSEXW Class;
    HWND Window = NULL;
    int Step;

    if (!SetThreadDesktop(Cover->Desktop))
    {
        SetEvent(Cover->Shown);
        return 1;
    }

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = WlCoverProc;
    Class.hInstance = hAppInstance;
    Class.lpszClassName = WL_COVER_CLASS;
    RegisterClassExW(&Class);

    Window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                             WL_COVER_CLASS, NULL, WS_POPUP,
                             Cover->Screen.left, Cover->Screen.top,
                             Cover->Screen.right - Cover->Screen.left,
                             Cover->Screen.bottom - Cover->Screen.top,
                             NULL, NULL, hAppInstance, Cover);
    if (Window != NULL && SetLayeredWindowAttributes(Window, 0, 255, LWA_ALPHA))
    {
        SetWindowPos(Window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateWindow(Window);
        Cover->Ready = TRUE;
    }
    SetEvent(Cover->Shown);

    if (Cover->Ready)
    {
        WlCoverWait(Cover->Release, WL_COVER_HOLD_MS);
        for (Step = 1; Step <= WL_COVER_STEPS; Step++)
        {
            SetLayeredWindowAttributes(Window, 0, (BYTE)(255 - (255 * Step) / WL_COVER_STEPS), LWA_ALPHA);
            WlCoverWait(NULL, WL_COVER_FADE_MS / WL_COVER_STEPS);
        }
    }

    if (Window != NULL)
        DestroyWindow(Window);
    UnregisterClassW(WL_COVER_CLASS, hAppInstance);
    return 0;
}

static
VOID
WlCoverFree(
    _In_ PWL_COVER Cover)
{
    if (Cover->Thread)
        CloseHandle(Cover->Thread);
    if (Cover->Shown)
        CloseHandle(Cover->Shown);
    if (Cover->Release)
        CloseHandle(Cover->Release);
    if (Cover->Frame)
        DeleteObject(Cover->Frame);
    HeapFree(GetProcessHeap(), 0, Cover);
}

/*
 * Switches the input desktop from From to To behind a cover that shows the last frame of From.
 * The screen does not change at the switch; WlCoverRelease then fades the cover out. On any
 * failure the switch happens plainly and NULL is returned.
 */
PWL_COVER
WlSwitchDesktopCovered(
    _In_ HDESK From,
    _In_ HDESK To)
{
    WL_CAPTURE Capture;
    PWL_COVER Cover;
    HANDLE Thread;

    ZeroMemory(&Capture, sizeof(Capture));
    Capture.Desktop = From;
    WlCoverScreenRect(&Capture.Screen);
    Thread = CreateThread(NULL, 0, WlCaptureThread, &Capture, 0, NULL);
    if (Thread != NULL)
    {
        WaitForSingleObject(Thread, WL_COVER_SHOWN_MS);
        CloseHandle(Thread);
    }

    Cover = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Cover));
    if (Cover == NULL || Capture.Frame == NULL)
        goto Plain;

    Cover->Desktop = To;
    Cover->Frame = Capture.Frame;
    Capture.Frame = NULL;
    Cover->Screen = Capture.Screen;
    Cover->Shown = CreateEventW(NULL, TRUE, FALSE, NULL);
    Cover->Release = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Cover->Shown == NULL || Cover->Release == NULL)
        goto Plain;
    Cover->Thread = CreateThread(NULL, 0, WlCoverThread, Cover, 0, NULL);
    if (Cover->Thread == NULL)
        goto Plain;
    if (WaitForSingleObject(Cover->Shown, WL_COVER_SHOWN_MS) != WAIT_OBJECT_0 || !Cover->Ready)
    {
        SetEvent(Cover->Release);
        WaitForSingleObject(Cover->Thread, WL_COVER_SHOWN_MS + WL_COVER_FADE_MS);
        goto Plain;
    }

    SwitchDesktop(To);
    LogonStage("desktop-switch-covered");
    return Cover;

Plain:
    if (Capture.Frame)
        DeleteObject(Capture.Frame);
    if (Cover)
    {
        if (Cover->Thread && WaitForSingleObject(Cover->Thread, 0) == WAIT_TIMEOUT)
        {
            /* Still alive: it owns the cover memory now and must not be freed under it. */
            SwitchDesktop(To);
            return NULL;
        }
        WlCoverFree(Cover);
    }
    SwitchDesktop(To);
    LogonStage("desktop-switch-plain");
    return NULL;
}

/* Fades the cover out over the new desktop and waits until it is gone. */
VOID
WlCoverRelease(
    _In_opt_ PWL_COVER Cover)
{
    if (Cover == NULL)
        return;
    SetEvent(Cover->Release);
    if (WaitForSingleObject(Cover->Thread, WL_COVER_FADE_MS * 2) == WAIT_OBJECT_0)
        WlCoverFree(Cover);
    /* else: the thread still uses it; leaking one cover beats freeing it under the thread */
}
