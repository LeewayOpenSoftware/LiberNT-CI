/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     End-to-end test of the indicator the shell shows for the volume keys
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <apitest.h>

#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#define OSD_WIDTH        192
#define OSD_HEIGHT       47
#define OSD_GAP          14
#define CAPTURE_HALF     160
#define CAPTURE_HEIGHT   120
#define MAX_SAMPLES      1500

typedef struct _OSD_SAMPLE
{
    DWORD Time;
    LONG Top;
    LONG Bottom;
    LONG Left;
    LONG Right;
    COLORREF Body;
} OSD_SAMPLE;

typedef struct _OSD_CAPTURE
{
    HDC hdcScreen;
    HDC hdcMem;
    HBITMAP hbm;
    HGDIOBJ hbmOld;
    const BYTE *Bits;
    BYTE *Baseline;
    RECT rc;
    LONG cx;
    LONG cy;
} OSD_CAPTURE;

static OSD_SAMPLE g_Samples[MAX_SAMPLES];
static int g_Dpi = 96;

static LONG
Scale(LONG Value)
{
    return MulDiv(Value, g_Dpi, 96);
}

static LONG
Tolerance(VOID)
{
    return (g_Dpi == 96) ? 0 : 1;
}

static void
PumpMessages(DWORD Timeout)
{
    DWORD Deadline = GetTickCount() + Timeout;
    MSG Message;

    do
    {
        while (PeekMessageW(&Message, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Message);
            DispatchMessageW(&Message);
        }
        if ((LONG)(Deadline - GetTickCount()) <= 0)
            break;
        MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT);
    } while (TRUE);
}

static void
PressKey(WORD Vk)
{
    INPUT Input[2];

    ZeroMemory(Input, sizeof(Input));
    Input[0].type = Input[1].type = INPUT_KEYBOARD;
    Input[0].ki.wVk = Input[1].ki.wVk = Vk;
    Input[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    Input[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    ok_int(SendInput(2, Input, sizeof(INPUT)), 2);
}

static COLORREF
CapturePixel(const OSD_CAPTURE *Capture, const BYTE *Bits, LONG x, LONG y)
{
    const BYTE *Pixel = Bits + ((SIZE_T)y * Capture->cx + x) * 4;

    return RGB(Pixel[2], Pixel[1], Pixel[0]);
}

static BOOL
ColorsDiffer(COLORREF a, COLORREF b, int Threshold)
{
    return abs(GetRValue(a) - GetRValue(b)) + abs(GetGValue(a) - GetGValue(b)) +
           abs(GetBValue(a) - GetBValue(b)) > Threshold;
}

static BOOL
PixelChanged(const OSD_CAPTURE *Capture, LONG x, LONG y)
{
    return ColorsDiffer(CapturePixel(Capture, Capture->Bits, x, y),
                        CapturePixel(Capture, Capture->Baseline, x, y), 48);
}

static BOOL
CaptureInit(OSD_CAPTURE *Capture, const RECT *prcMonitor, const RECT *prcTray)
{
    BITMAPINFO bmi;
    void *pvBits = NULL;
    LONG xCenter = (prcMonitor->left + prcMonitor->right) / 2;

    ZeroMemory(Capture, sizeof(*Capture));
    SetRect(&Capture->rc, xCenter - Scale(CAPTURE_HALF), prcTray->top - Scale(CAPTURE_HEIGHT),
            xCenter + Scale(CAPTURE_HALF), prcTray->top);
    Capture->cx = Capture->rc.right - Capture->rc.left;
    Capture->cy = Capture->rc.bottom - Capture->rc.top;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = Capture->cx;
    bmi.bmiHeader.biHeight = -Capture->cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    Capture->hdcScreen = GetDC(NULL);
    Capture->hdcMem = CreateCompatibleDC(Capture->hdcScreen);
    Capture->hbm = CreateDIBSection(Capture->hdcMem, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    Capture->Baseline = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)Capture->cx * Capture->cy * 4);
    if (!Capture->hdcScreen || !Capture->hdcMem || !Capture->hbm || !pvBits || !Capture->Baseline)
        return FALSE;
    Capture->Bits = pvBits;
    Capture->hbmOld = SelectObject(Capture->hdcMem, Capture->hbm);
    return TRUE;
}

static void
CaptureFree(OSD_CAPTURE *Capture)
{
    if (Capture->hbmOld)
        SelectObject(Capture->hdcMem, Capture->hbmOld);
    if (Capture->hbm)
        DeleteObject(Capture->hbm);
    if (Capture->hdcMem)
        DeleteDC(Capture->hdcMem);
    if (Capture->hdcScreen)
        ReleaseDC(NULL, Capture->hdcScreen);
    if (Capture->Baseline)
        HeapFree(GetProcessHeap(), 0, Capture->Baseline);
}

static void
CaptureGrab(OSD_CAPTURE *Capture)
{
    BitBlt(Capture->hdcMem, 0, 0, Capture->cx, Capture->cy, Capture->hdcScreen, Capture->rc.left, Capture->rc.top,
           SRCCOPY | CAPTUREBLT);
    GdiFlush();
}

static void
CaptureBaseline(OSD_CAPTURE *Capture)
{
    CaptureGrab(Capture);
    CopyMemory(Capture->Baseline, Capture->Bits, (SIZE_T)Capture->cx * Capture->cy * 4);
}

static void
CaptureSample(OSD_CAPTURE *Capture, DWORD Time, OSD_SAMPLE *Sample)
{
    LONG x = Capture->cx / 2, y, yMiddle;

    CaptureGrab(Capture);
    Sample->Time = Time;
    Sample->Top = Sample->Bottom = Sample->Left = Sample->Right = -1;
    Sample->Body = 0;
    for (y = 0; y < Capture->cy; y++)
    {
        if (!PixelChanged(Capture, x, y))
            continue;
        if (Sample->Top < 0)
            Sample->Top = y;
        Sample->Bottom = y;
    }
    if (Sample->Top < 0)
        return;
    Sample->Body = CapturePixel(Capture, Capture->Bits, x, min(Sample->Top + Scale(3), Sample->Bottom));
    yMiddle = (Sample->Top + Sample->Bottom) / 2;
    for (x = 0; x < Capture->cx; x++)
    {
        if (!PixelChanged(Capture, x, yMiddle))
            continue;
        if (Sample->Left < 0)
            Sample->Left = x + Capture->rc.left;
        Sample->Right = x + Capture->rc.left;
    }
    Sample->Top += Capture->rc.top;
    Sample->Bottom += Capture->rc.top;
}

static int
Observe(OSD_CAPTURE *Capture, WORD Vk, DWORD SecondKeyAt, WORD SecondVk, DWORD Duration)
{
    DWORD Start, Now;
    int Count = 0;
    BOOL bSecondSent = (SecondKeyAt == 0);

    CaptureBaseline(Capture);
    Start = GetTickCount();
    PressKey(Vk);
    while (Count < MAX_SAMPLES && (Now = GetTickCount() - Start) < Duration)
    {
        if (!bSecondSent && Now >= SecondKeyAt)
        {
            PressKey(SecondVk);
            bSecondSent = TRUE;
        }
        CaptureSample(Capture, Now, &g_Samples[Count++]);
        PumpMessages(5);
    }
    return Count;
}

static int
SampleAt(int Count, DWORD Time)
{
    int i;

    for (i = 0; i < Count; i++)
    {
        if (g_Samples[i].Time >= Time)
            return i;
    }
    return Count - 1;
}

static void
CheckSingleKey(OSD_CAPTURE *Capture, const RECT *prcMonitor, const RECT *prcTray)
{
    const OSD_SAMPLE *Rest, *Late, *First = NULL, *Leaving = NULL;
    LONG xCenter = (prcMonitor->left + prcMonitor->right) / 2;
    int Count, i, iRest, iGone = -1;

    trace("Observing one volume key\n");
    Count = Observe(Capture, VK_VOLUME_UP, 0, 0, 4000);
    for (i = 0; i < Count && !First; i++)
    {
        if (g_Samples[i].Top >= 0)
            First = &g_Samples[i];
    }
    trace("%d samples, first frame at %lu ms\n", Count, First ? First->Time : 0);
    ok(First != NULL && First->Time < 1500, "Indicator did not appear within 1.5 s (%lu)\n", First ? First->Time : 0);
    if (!First)
        return;

    iRest = SampleAt(Count, 1200);
    Rest = &g_Samples[iRest];
    ok(Rest->Top >= 0, "Indicator not shown 1.2 s after the key\n");
    if (Rest->Top < 0)
        return;
    ok(abs(prcTray->top - (Rest->Bottom + 1) - Scale(OSD_GAP)) <= Tolerance(),
       "Gap above the taskbar is %ld, expected %ld\n", prcTray->top - (Rest->Bottom + 1), Scale(OSD_GAP));
    ok(abs(Rest->Bottom - Rest->Top + 1 - Scale(OSD_HEIGHT)) <= Tolerance(), "Height is %ld, expected %ld\n",
       Rest->Bottom - Rest->Top + 1, Scale(OSD_HEIGHT));
    ok(abs(Rest->Right - Rest->Left + 1 - Scale(OSD_WIDTH)) <= Tolerance(), "Width is %ld, expected %ld\n",
       Rest->Right - Rest->Left + 1, Scale(OSD_WIDTH));
    ok(abs((Rest->Left + Rest->Right + 1) / 2 - xCenter) <= Tolerance(), "Center is %ld, expected %ld\n",
       (Rest->Left + Rest->Right + 1) / 2, xCenter);

    ok(First->Bottom == prcTray->top - 1, "First frame ends at %ld, expected the taskbar edge %ld\n", First->Bottom,
       prcTray->top - 1);
    ok(First->Top > Rest->Top, "First frame top %ld is not below the resting top %ld\n", First->Top, Rest->Top);
    for (i = 0; i < iRest; i++)
    {
        if (g_Samples[i].Top >= 0 && g_Samples[i].Bottom - g_Samples[i].Top >= Scale(8))
        {
            ok(!ColorsDiffer(g_Samples[i].Body, Rest->Body, 24), "Entering frame %d color %06lx, resting %06lx\n", i,
               g_Samples[i].Body, Rest->Body);
            break;
        }
    }

    Late = &g_Samples[SampleAt(Count, 1500)];
    ok(Late->Top == Rest->Top, "Indicator top at 1.5 s is %ld, expected %ld\n", Late->Top, Rest->Top);
    for (i = iRest + 1; i < Count; i++)
    {
        if (g_Samples[i].Top < 0)
        {
            iGone = i;
            break;
        }
        if (!Leaving && g_Samples[i].Top > Rest->Top + Scale(4) && g_Samples[i].Bottom - g_Samples[i].Top >= Scale(8))
            Leaving = &g_Samples[i];
    }
    ok(iGone >= 0 && g_Samples[iGone].Time < 3500, "Indicator still shown at %lu ms\n",
       g_Samples[iGone >= 0 ? iGone : Count - 1].Time);
    ok(Leaving != NULL, "No frame of the indicator moving down before it disappeared\n");
    if (Leaving)
    {
        ok(!ColorsDiffer(Leaving->Body, Rest->Body, 24), "Leaving frame color %06lx, resting %06lx\n", Leaving->Body,
           Rest->Body);
    }
    if (iGone > 0)
    {
        ok(g_Samples[iGone - 1].Bottom == prcTray->top - 1, "Last frame ends at %ld, expected the taskbar edge %ld\n",
           g_Samples[iGone - 1].Bottom, prcTray->top - 1);
    }
}

static void
CheckSecondKeyRestartsTimeout(OSD_CAPTURE *Capture)
{
    int Count;

    trace("Observing two volume keys 1 s apart\n");
    Count = Observe(Capture, VK_VOLUME_UP, 1000, VK_VOLUME_DOWN, 5000);
    trace("%d samples\n", Count);
    ok(g_Samples[SampleAt(Count, 2600)].Top >= 0, "Indicator hidden 1.6 s after the second key\n");
    ok(g_Samples[SampleAt(Count, 4900)].Top < 0, "Indicator still shown 3.9 s after the second key\n");
}

static BOOL
GetEndpointVolume(IAudioEndpointVolume **ppVolume)
{
    IMMDeviceEnumerator *pEnum = NULL;
    IMMDevice *pDevice = NULL;
    HRESULT hr;

    *ppVolume = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator,
                          (void **)&pEnum);
    if (SUCCEEDED(hr))
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnum, eRender, eMultimedia, &pDevice);
    if (SUCCEEDED(hr))
        hr = IMMDevice_Activate(pDevice, &IID_IAudioEndpointVolume, CLSCTX_INPROC_SERVER, NULL, (void **)ppVolume);
    if (pDevice)
        IMMDevice_Release(pDevice);
    if (pEnum)
        IMMDeviceEnumerator_Release(pEnum);
    return SUCCEEDED(hr) && *ppVolume;
}

START_TEST(VolumeOsd)
{
    IAudioEndpointVolume *pVolume;
    MONITORINFO mi = { sizeof(mi) };
    WNDCLASSW wc = { 0 };
    OSD_CAPTURE Capture;
    float fLevel = 0.0f;
    BOOL bMute = FALSE;
    HWND hwndTray, hwnd;
    RECT rcTray;
    HDC hdc;

    SetProcessDPIAware();
    hdc = GetDC(NULL);
    g_Dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(NULL, hdc);

    hwndTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!hwndTray || !GetWindowRect(hwndTray, &rcTray) ||
        !GetMonitorInfoW(MonitorFromWindow(hwndTray, MONITOR_DEFAULTTOPRIMARY), &mi))
    {
        skip("No taskbar\n");
        return;
    }
    if (rcTray.top <= (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2)
    {
        skip("Taskbar is not at the bottom of the screen\n");
        return;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (!GetEndpointVolume(&pVolume))
    {
        skip("No default audio render endpoint\n");
        CoUninitialize();
        return;
    }
    ok_hr(IAudioEndpointVolume_GetMasterVolumeLevelScalar(pVolume, &fLevel), S_OK);
    ok_hr(IAudioEndpointVolume_GetMute(pVolume, &bMute), S_OK);

    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"VolumeOsdTest";
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"VolumeOsd", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           mi.rcMonitor.left + 40, mi.rcMonitor.top + 40, 320, 160, NULL, NULL, wc.hInstance, NULL);
    ok(hwnd != NULL, "CreateWindowExW failed: %lu\n", GetLastError());
    SetForegroundWindow(hwnd);
    SetCursorPos(mi.rcMonitor.left + 8, mi.rcMonitor.top + 8);
    PumpMessages(500);

    if (!CaptureInit(&Capture, &mi.rcMonitor, &rcTray))
    {
        skip("Screen capture setup failed\n");
    }
    else
    {
        /* The first indicator of a session can take seconds to come up; let one come and go first. */
        PressKey(VK_VOLUME_UP);
        PumpMessages(3500);
        CheckSingleKey(&Capture, &mi.rcMonitor, &rcTray);
        PumpMessages(1000);
        CheckSecondKeyRestartsTimeout(&Capture);
    }
    CaptureFree(&Capture);

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    IAudioEndpointVolume_SetMasterVolumeLevelScalar(pVolume, fLevel, NULL);
    IAudioEndpointVolume_SetMute(pVolume, bMute, NULL);
    IAudioEndpointVolume_Release(pVolume);
    CoUninitialize();
}
