/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Continue the boot spinner in winlogon's first window, seamlessly from the kernel's
 *              boot spinner (ntoskrnl/inbv). The kernel renders its spinner at the interrupt time
 *              and hands the display over at the current phase; winlogon renders at the same clock
 *              (the shared user page) over the same screen rectangle, so the hand-over from the
 *              boot screen to the logon UI has no jump (owner fluidity rule; INVESTIGACION.md 6.3).
 *              Geometry and animation come from the shared header the kernel uses too (bootspin.h),
 *              so there is one source for both.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "winlogon.h"
#include <reactos/bootspin.h>
#include <mmsystem.h>  /* timeBeginPeriod */

#define WL_SPIN_CLASS   L"WinlogonBootSpinner"

typedef struct _WL_SPINNER
{
    HDESK Desktop;
    INBV_SPIN_RECT Rect;
    ULONG Dpi;                  /* the DPI the rect was built at; logged for Pam's 1:1 check */
    PINBV_SPIN_SAMPLE Samples;
    PULONG PixelStart;
    PULONG Ease;                /* INBV_SPIN_EASE_STEPS + 1 entries */
    PULONG Frame;               /* Rect.Width * Rect.Height pixels, 0x00RRGGBB */
    ULONG Lut[256];             /* stroke opacity -> packed grey-on-black */
    BITMAPINFOHEADER Info;       /* top-down 32bpp header for the frame */
    HANDLE Shown;               /* the window is up (or has failed) */
    HANDLE Stop;                /* WlBootSpinnerStop asks the pump to finish */
    HANDLE Thread;
    BOOL Ready;
} WL_SPINNER;

/* One spinner at a time: Show creates it, Stop tears it down. */
static WL_SPINNER *g_Spinner = NULL;

/* Milliseconds from the high-resolution counter (GetTickCount64 steps by about 15.6 ms). */
static
ULONGLONG
WlSpinNowMs(VOID)
{
    LARGE_INTEGER Count, Frequency;

    if (!QueryPerformanceFrequency(&Frequency) || Frequency.QuadPart == 0 || !QueryPerformanceCounter(&Count))
        return GetTickCount64();
    return (ULONGLONG)(Count.QuadPart / (Frequency.QuadPart / 1000 ? Frequency.QuadPart / 1000 : 1));
}

/* The boot clock: 100 ns since boot, the same value the kernel renders at (KeQueryInterruptTime),
 * read from the shared user page so the first user-mode frame continues where the kernel's last
 * frame stands. */
static
ULONGLONG
WlSpinInterruptTime(VOID)
{
    LARGE_INTEGER Time;

    do
    {
        Time.HighPart = (LONG)SharedUserData->InterruptTime.High1Time;
        Time.LowPart = SharedUserData->InterruptTime.LowPart;
    } while (Time.HighPart != (LONG)SharedUserData->InterruptTime.High2Time);

    return (ULONGLONG)Time.QuadPart;
}

/* Pumps the spinner thread's messages until Event is set or Ms pass. Returns TRUE if Event was set. */
static
BOOL
WlSpinWait(
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
LRESULT
CALLBACK
WlSpinProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    if (Message == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    return DefWindowProcW(Window, Message, wParam, lParam);
}

static
VOID
WlSpinFreeResources(
    _Inout_ WL_SPINNER *Spinner)
{
    if (Spinner->Frame)
        HeapFree(GetProcessHeap(), 0, Spinner->Frame);
    if (Spinner->Ease)
        HeapFree(GetProcessHeap(), 0, Spinner->Ease);
    if (Spinner->Samples)
        HeapFree(GetProcessHeap(), 0, Spinner->Samples);
    if (Spinner->PixelStart)
        HeapFree(GetProcessHeap(), 0, Spinner->PixelStart);
    Spinner->Frame = NULL;
    Spinner->Ease = NULL;
    Spinner->Samples = NULL;
    Spinner->PixelStart = NULL;
}

/*
 * Work out where the spinner sits and build what each frame needs: the sub-pixel samples, the
 * ease table, the colour table and the frame buffer. The rect comes from the desktop size, which
 * is the boot mode on an equal-size first commit (so it maps 1:1 onto the kernel's pixels and onto
 * the centred boot-screen wallpaper); when the first desktop mode differs from the boot mode the
 * spinner would shift by the centring offset, which would need the boot dimensions from win32k.
 * Returns FALSE if the spinner does not fit or an allocation fails.
 */
static
BOOL
WlSpinBuild(
    _Inout_ WL_SPINNER *Spinner)
{
    HDC Screen;
    ULONG Dpi = LOADER_PARAMETER_FRAMEBUFFER_DPI_DEFAULT;
    ULONG ScreenW = (ULONG)GetSystemMetrics(SM_CXSCREEN);
    ULONG ScreenH = (ULONG)GetSystemMetrics(SM_CYSCREEN);
    ULONG Count, Samples, i;

    Screen = GetDC(NULL);
    if (Screen != NULL)
    {
        int Logical = GetDeviceCaps(Screen, LOGPIXELSX);
        if (Logical > 0)
            Dpi = (ULONG)Logical;
        ReleaseDC(NULL, Screen);
    }

    Spinner->Rect = InbvSpinComputeRect(ScreenW, ScreenH, Dpi);
    Spinner->Dpi = Dpi;
    if (Spinner->Rect.Width == 0 || Spinner->Rect.Height == 0)
        return FALSE;

    Count = Spinner->Rect.Width * Spinner->Rect.Height;

    Spinner->PixelStart = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)(Count + 1) * sizeof(ULONG));
    if (!Spinner->PixelStart)
        return FALSE;

    Samples = InbvSpinBuildSamples(Spinner->Rect.Width, Spinner->Rect.Height, NULL, Spinner->PixelStart);
    Spinner->Samples = HeapAlloc(GetProcessHeap(), 0,
                                 (SIZE_T)(Samples ? Samples : 1) * sizeof(INBV_SPIN_SAMPLE));
    if (!Spinner->Samples)
        return FALSE;
    InbvSpinBuildSamples(Spinner->Rect.Width, Spinner->Rect.Height, Spinner->Samples, Spinner->PixelStart);

    Spinner->Ease = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)(INBV_SPIN_EASE_STEPS + 1) * sizeof(ULONG));
    if (!Spinner->Ease)
        return FALSE;
    InbvSpinEaseTable(Spinner->Ease);

    for (i = 0; i < 256; i++)
    {
        ULONG v = (ULONG)INBV_SPIN_RED * i / 255;   /* grey: R == G == B */
        Spinner->Lut[i] = (v << 16) | (v << 8) | v; /* 0x00RRGGBB, opaque over the black logo area */
    }

    Spinner->Frame = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)Count * sizeof(ULONG));
    if (!Spinner->Frame)
        return FALSE;

    ZeroMemory(&Spinner->Info, sizeof(Spinner->Info));
    Spinner->Info.biSize = sizeof(BITMAPINFOHEADER);
    Spinner->Info.biWidth = (LONG)Spinner->Rect.Width;
    Spinner->Info.biHeight = -(LONG)Spinner->Rect.Height;   /* top-down: Frame[0] is the top-left */
    Spinner->Info.biPlanes = 1;
    Spinner->Info.biBitCount = 32;
    Spinner->Info.biCompression = BI_RGB;
    return TRUE;
}

static
DWORD
WINAPI
WlSpinThread(
    _In_ PVOID Parameter)
{
    WL_SPINNER *Spinner = Parameter;
    HWND Window = NULL;
    HDC Window_DC = NULL;
    WNDCLASSEXW Class;
    ULONG Count = Spinner->Rect.Width * Spinner->Rect.Height;

    if (!SetThreadDesktop(Spinner->Desktop))
    {
        SetEvent(Spinner->Shown);
        return 1;
    }

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = WlSpinProc;
    Class.hInstance = hAppInstance;
    Class.lpszClassName = WL_SPIN_CLASS;
    RegisterClassExW(&Class);

    Window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             WL_SPIN_CLASS, NULL, WS_POPUP,
                             (int)Spinner->Rect.Left, (int)Spinner->Rect.Top,
                             (int)Spinner->Rect.Width, (int)Spinner->Rect.Height,
                             NULL, NULL, hAppInstance, NULL);
    if (Window != NULL)
    {
        Window_DC = GetDC(Window);
        if (Window_DC != NULL)
        {
            /* Draw the first frame before the window shows, so it appears already carrying the
             * spinner where the kernel left it - no black flash at the hand-over. */
            InbvSpinRenderFrame(Spinner->Samples, Spinner->PixelStart, Count, Spinner->Ease,
                                WlSpinInterruptTime(), Spinner->Lut, Spinner->Frame);
            SetDIBitsToDevice(Window_DC, 0, 0, Spinner->Rect.Width, Spinner->Rect.Height,
                              0, 0, 0, Spinner->Rect.Height, Spinner->Frame,
                              (const BITMAPINFO *)&Spinner->Info, DIB_RGB_COLORS);
            SetWindowPos(Window, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            Spinner->Ready = TRUE;
        }
    }
    SetEvent(Spinner->Shown);

    if (Spinner->Ready)
    {
        ULONGLONG Start = WlSpinNowMs(), Last = Start, Now, Next = Start;
        DWORD Frames = 0, Late = 0, Longest = 0, Interval;
        CHAR Line[96];

        /* Raise the timer resolution to 1 ms for the hand-over, so the 15 ms pump is not
         * quantised to the ~15.6 ms default tick (which would make every other frame late).
         * Bounded to the hand-over, so no idle cost. */
        timeBeginPeriod(1);

        StringCchPrintfA(Line, ARRAYSIZE(Line), "boot-spinner-shown dpi=%lu rect=%lu,%lu,%lu,%lu",
                         Spinner->Dpi, Spinner->Rect.Left, Spinner->Rect.Top,
                         Spinner->Rect.Width, Spinner->Rect.Height);
        LogonStage(Line);

        for (;;)
        {
            Now = WlSpinNowMs();
            Interval = (DWORD)(Now - Last);
            if (Frames > 0)
            {
                if (Interval > Longest)
                    Longest = Interval;
                if (Interval > 2 * INBV_SPIN_FRAME_MS)
                    Late++;
            }
            Last = Now;

            InbvSpinRenderFrame(Spinner->Samples, Spinner->PixelStart, Count, Spinner->Ease,
                                WlSpinInterruptTime(), Spinner->Lut, Spinner->Frame);
            SetDIBitsToDevice(Window_DC, 0, 0, Spinner->Rect.Width, Spinner->Rect.Height,
                              0, 0, 0, Spinner->Rect.Height, Spinner->Frame,
                              (const BITMAPINFO *)&Spinner->Info, DIB_RGB_COLORS);
            Frames++;

            /* Wait to a fixed cadence (deadline += 15 ms), not 15 ms after this frame, so the
             * render time does not accumulate; if we fell behind, resync to now and skip the
             * missed slots instead of bursting to catch up. */
            Next += INBV_SPIN_FRAME_MS;
            Now = WlSpinNowMs();
            if (Next <= Now)
                Next = Now;
            if (WlSpinWait(Spinner->Stop, (DWORD)(Next - Now)))
                break;
        }

        timeEndPeriod(1);

        /* Pam reads the hand-over from this line, in the cover-fade style: a late frame is an
         * interval over twice the target. */
        StringCchPrintfA(Line, ARRAYSIZE(Line), "boot-spinner frames=%lu longest-ms=%lu late=%lu target-ms=%u",
                         Frames, Longest, Late, (unsigned)INBV_SPIN_FRAME_MS);
        LogonStage(Line);
    }

    if (Window_DC != NULL)
        ReleaseDC(Window, Window_DC);
    if (Window != NULL)
        DestroyWindow(Window);
    UnregisterClassW(WL_SPIN_CLASS, hAppInstance);
    return 0;
}

/*
 * Show the boot spinner on the Winlogon desktop, continuing the kernel's spinner without a jump.
 * Idempotent: a second call while one is up does nothing. Returns once the window is up (or has
 * failed), so the caller can hand the screen over knowing the spinner is drawing.
 */
VOID
WlBootSpinnerShow(
    _Inout_ PWLSESSION Session)
{
    WL_SPINNER *Spinner;

    if (g_Spinner != NULL || Session == NULL)
        return;

    Spinner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Spinner));
    if (Spinner == NULL)
        return;

    Spinner->Desktop = Session->WinlogonDesktop;
    if (!WlSpinBuild(Spinner))
    {
        WlSpinFreeResources(Spinner);
        HeapFree(GetProcessHeap(), 0, Spinner);
        return;
    }

    Spinner->Shown = CreateEventW(NULL, TRUE, FALSE, NULL);
    Spinner->Stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Spinner->Shown == NULL || Spinner->Stop == NULL)
        goto Fail;

    Spinner->Thread = CreateThread(NULL, 0, WlSpinThread, Spinner, 0, NULL);
    if (Spinner->Thread == NULL)
        goto Fail;

    g_Spinner = Spinner;
    WaitForSingleObject(Spinner->Shown, INFINITE);
    return;

Fail:
    if (Spinner->Shown)
        CloseHandle(Spinner->Shown);
    if (Spinner->Stop)
        CloseHandle(Spinner->Stop);
    WlSpinFreeResources(Spinner);
    HeapFree(GetProcessHeap(), 0, Spinner);
}

/*
 * Stop the boot spinner and wait for its window to close, so the caller can bring up the logon UI
 * in its place. A no-op if no spinner is up.
 */
VOID
WlBootSpinnerStop(VOID)
{
    WL_SPINNER *Spinner = g_Spinner;

    if (Spinner == NULL)
        return;
    g_Spinner = NULL;

    SetEvent(Spinner->Stop);
    WaitForSingleObject(Spinner->Thread, INFINITE);

    CloseHandle(Spinner->Thread);
    CloseHandle(Spinner->Stop);
    CloseHandle(Spinner->Shown);
    WlSpinFreeResources(Spinner);
    HeapFree(GetProcessHeap(), 0, Spinner);
}
