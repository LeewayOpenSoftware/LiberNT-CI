/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Checks that a scroll bar arrow button does not draw the colour-key of its glyph
 */

#include <apitest.h>
#include <windows.h>
#include <uxtheme.h>
#include <vsstyle.h>

#define KEY_PIXEL 0x00FF00FF
#define BOX_SIZE 17

START_TEST(ScrollGlyphKey)
{
    BITMAPINFO Info = {{sizeof(BITMAPINFOHEADER), BOX_SIZE, -BOX_SIZE, 1, 32, BI_RGB}};
    HWND Window;
    HTHEME Theme;
    HDC Dc;
    HBITMAP Bitmap;
    DWORD *Bits = NULL;
    RECT Rect = {0, 0, BOX_SIZE, BOX_SIZE};
    HRESULT Result;
    int Index, Keyed = 0, State;

    if (!IsThemeActive())
    {
        skip("No visual style is active\n");
        return;
    }

    Window = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    ok(Window != NULL, "CreateWindowExW failed %lu\n", GetLastError());
    if (!Window)
        return;

    Theme = OpenThemeData(Window, L"SCROLLBAR");
    if (!Theme)
    {
        skip("No SCROLLBAR class\n");
        DestroyWindow(Window);
        return;
    }

    Dc = CreateCompatibleDC(NULL);
    Bitmap = CreateDIBSection(Dc, &Info, DIB_RGB_COLORS, (void **)&Bits, NULL, 0);
    ok(Bitmap != NULL, "CreateDIBSection failed\n");
    if (Bitmap)
    {
        SelectObject(Dc, Bitmap);
        for (State = ABS_UPNORMAL; State <= ABS_RIGHTNORMAL; State += 4)
        {
            ZeroMemory(Bits, BOX_SIZE * BOX_SIZE * sizeof(DWORD));
            Result = DrawThemeBackground(Theme, Dc, SBP_ARROWBTN, State, &Rect, NULL);
            ok(Result == S_OK, "DrawThemeBackground state %d returned 0x%lx\n", State, Result);
            for (Index = 0; Index < BOX_SIZE * BOX_SIZE; Index++)
            {
                if ((Bits[Index] & 0x00FFFFFF) == KEY_PIXEL)
                    Keyed++;
            }
        }
        ok(Keyed == 0, "The glyph colour-key was drawn on %d pixels\n", Keyed);
        DeleteObject(Bitmap);
    }
    DeleteDC(Dc);
    CloseThemeData(Theme);
    DestroyWindow(Window);
}
