/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Icons whose frames are PNG (Vista+ .ico): LoadImage, CreateIconFromResourceEx, the mask, DrawIconEx
 */

#include "precomp.h"

/* 16x16 solid red, RGBA, 82 bytes */
static const BYTE s_png_rgba[82] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0xF3, 0xFF,
    0x61, 0x00, 0x00, 0x00, 0x19, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
    0x9F, 0x12, 0xCC, 0x30, 0x6A, 0xC0, 0xA8, 0x01, 0xA3, 0x06, 0x0C, 0x17, 0x03, 0x00, 0x30, 0xC4,
    0xFE, 0x10, 0x8A, 0xC6, 0x4C, 0xE5, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42,
    0x60, 0x82,
};
/* 16x16 solid red, RGB (no alpha channel), 79 bytes */
static const BYTE s_png_rgb[79] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x91, 0x68,
    0x36, 0x00, 0x00, 0x00, 0x16, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0x40,
    0x12, 0x62, 0x18, 0xD5, 0x30, 0xAA, 0x61, 0xF8, 0x6A, 0x00, 0x00, 0x90, 0xF9, 0xFF, 0x01, 0xF2,
    0xEE, 0xE8, 0x57, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

#include <pshpack2.h>
typedef struct
{
    BYTE bWidth;
    BYTE bHeight;
    BYTE bColorCount;
    BYTE bReserved;
    WORD wPlanes;
    WORD wBitCount;
    DWORD dwBytesInRes;
    DWORD dwImageOffset;
} TEST_ICONDIRENTRY;

typedef struct
{
    WORD idReserved;
    WORD idType;
    WORD idCount;
    TEST_ICONDIRENTRY idEntries[1];
} TEST_ICONDIR;
#include <poppack.h>

static BOOL WriteIco(LPCWSTR path, const BYTE *png, DWORD cbPng, WORD bpp)
{
    TEST_ICONDIR dir = { 0, 1, 1 };
    HANDLE h;
    DWORD written;
    BOOL ok1, ok2;

    dir.idEntries[0].bWidth = 16;
    dir.idEntries[0].bHeight = 16;
    dir.idEntries[0].wPlanes = 1;
    dir.idEntries[0].wBitCount = bpp;
    dir.idEntries[0].dwBytesInRes = cbPng;
    dir.idEntries[0].dwImageOffset = sizeof(dir);

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    ok1 = WriteFile(h, &dir, sizeof(dir), &written, NULL) && written == sizeof(dir);
    ok2 = WriteFile(h, png, cbPng, &written, NULL) && written == cbPng;
    CloseHandle(h);
    return ok1 && ok2;
}

/* Returns the 32-bpp pixel (0xAARRGGBB) at (x, y) of a bitmap, or 0xDEADBEEF. */
static DWORD BitmapPixel(HBITMAP hbm, int cx, int cy, int x, int y)
{
    BITMAPINFO bmi = { 0 };
    DWORD *bits;
    HDC hdc;
    DWORD result = 0xDEADBEEF;

    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = cx;
    bmi.bmiHeader.biHeight = -cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bits = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)cx * cy * 4);
    hdc = GetDC(NULL);
    if (bits && hdc && GetDIBits(hdc, hbm, 0, cy, bits, &bmi, DIB_RGB_COLORS) == cy)
        result = bits[y * cx + x];
    if (hdc) ReleaseDC(NULL, hdc);
    if (bits) HeapFree(GetProcessHeap(), 0, bits);
    return result;
}

/* The AND-mask bit at (x, y) of a 1-bpp mask bitmap of cx x cy (icons: the
 * mask bitmap is cx x cy; cursors from CreateIcon may be twice as high). */
static int MaskBit(HBITMAP hbmMask, int cx, int cy, int x, int y)
{
    struct { BITMAPINFOHEADER bmih; RGBQUAD colors[2]; } bmi = { { 0 } };
    BYTE *bits;
    HDC hdc;
    int stride = ((cx + 31) / 32) * 4;
    int result = -1;

    bmi.bmih.biSize = sizeof(bmi.bmih);
    bmi.bmih.biWidth = cx;
    bmi.bmih.biHeight = -cy;
    bmi.bmih.biPlanes = 1;
    bmi.bmih.biBitCount = 1;
    bits = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)stride * cy);
    hdc = GetDC(NULL);
    if (bits && hdc && GetDIBits(hdc, hbmMask, 0, cy, bits, (BITMAPINFO *)&bmi, DIB_RGB_COLORS) == cy)
        result = (bits[y * stride + x / 8] >> (7 - (x % 8))) & 1;
    if (hdc) ReleaseDC(NULL, hdc);
    if (bits) HeapFree(GetProcessHeap(), 0, bits);
    return result;
}

/* Draw the icon on a white 32-bpp DIB and return the centre pixel. */
static DWORD DrawnCenterPixel(HICON hIcon)
{
    BITMAPINFO bmi = { 0 };
    DWORD *bits = NULL;
    HDC hdc = CreateCompatibleDC(NULL);
    HBITMAP hbm, hbmOld;
    DWORD result = 0xDEADBEEF;
    int i;

    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = 16;
    bmi.bmiHeader.biHeight = -16;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (hdc && hbm)
    {
        for (i = 0; i < 16 * 16; i++)
            bits[i] = 0xFFFFFFFF;
        hbmOld = SelectObject(hdc, hbm);
        if (DrawIconEx(hdc, 0, 0, hIcon, 16, 16, 0, NULL, DI_NORMAL))
        {
            GdiFlush();
            result = bits[8 * 16 + 8];
        }
        SelectObject(hdc, hbmOld);
    }
    if (hbm) DeleteObject(hbm);
    if (hdc) DeleteDC(hdc);
    return result;
}

static void CheckIcon(const char *Name, HICON hIcon, BOOL HasAlpha)
{
    ICONINFO ii = { 0 };
    BITMAP bm = { 0 };
    DWORD pixel;
    int bit;

    ok(hIcon != NULL, "%s: no icon, error %lu\n", Name, GetLastError());
    if (!hIcon)
        return;

    ok(GetIconInfo(hIcon, &ii), "%s: GetIconInfo failed, error %lu\n", Name, GetLastError());
    ok(ii.fIcon, "%s: not an icon\n", Name);
    ok(ii.hbmColor != NULL, "%s: no colour bitmap\n", Name);
    ok(ii.hbmMask != NULL, "%s: no mask bitmap\n", Name);
    if (ii.hbmColor)
    {
        GetObjectW(ii.hbmColor, sizeof(bm), &bm);
        ok(bm.bmWidth == 16 && bm.bmHeight == 16, "%s: colour bitmap is %ldx%ld, expected 16x16\n", Name, bm.bmWidth, bm.bmHeight);
        ok(bm.bmBitsPixel == 32, "%s: colour bitmap is %u bpp, expected 32\n", Name, bm.bmBitsPixel);
        pixel = BitmapPixel(ii.hbmColor, 16, 16, 8, 8);
        ok((pixel & 0x00FFFFFF) == 0x00FF0000, "%s: colour pixel is %#lx, expected red\n", Name, pixel);
        if (HasAlpha)
            ok((pixel >> 24) == 0xFF, "%s: alpha is %lu, expected 255\n", Name, pixel >> 24);
    }
    if (ii.hbmMask)
    {
        GetObjectW(ii.hbmMask, sizeof(bm), &bm);
        ok(bm.bmWidth == 16 && bm.bmHeight == 16, "%s: mask bitmap is %ldx%ld, expected 16x16\n", Name, bm.bmWidth, bm.bmHeight);
        bit = MaskBit(ii.hbmMask, 16, 16, 8, 8);
        ok(bit == 0, "%s: AND mask bit of an opaque pixel is %d, expected 0 (opaque)\n", Name, bit);
    }
    pixel = DrawnCenterPixel(hIcon);
    ok((pixel & 0x00FFFFFF) == 0x00FF0000, "%s: DrawIconEx centre pixel is %#lx, expected red\n", Name, pixel);

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
}

START_TEST(LoadImage_PngIcon)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    HICON hIcon;

    if (!GetTempPathW(_countof(dir), dir))
    {
        skip("GetTempPathW failed, error %lu\n", GetLastError());
        return;
    }

    /* LoadImage from a .ico file whose only frame is a PNG with alpha */
    wsprintfW(path, L"%spngicon_rgba.ico", dir);
    ok(WriteIco(path, s_png_rgba, sizeof(s_png_rgba), 32), "could not write %ls, error %lu\n", path, GetLastError());
    hIcon = LoadImageW(NULL, path, IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
    CheckIcon("LoadImage RGBA", hIcon, TRUE);
    if (hIcon) DestroyIcon(hIcon);
    hIcon = LoadImageW(NULL, path, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    ok(hIcon != NULL, "LoadImage RGBA (default size): no icon, error %lu\n", GetLastError());
    if (hIcon) DestroyIcon(hIcon);
    DeleteFileW(path);

    /* The same without an alpha channel: the mask must still say opaque */
    wsprintfW(path, L"%spngicon_rgb.ico", dir);
    ok(WriteIco(path, s_png_rgb, sizeof(s_png_rgb), 24), "could not write %ls, error %lu\n", path, GetLastError());
    hIcon = LoadImageW(NULL, path, IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
    CheckIcon("LoadImage RGB", hIcon, FALSE);
    if (hIcon) DestroyIcon(hIcon);
    DeleteFileW(path);

    /* The resource form: the PNG bytes as an RT_ICON image */
    hIcon = CreateIconFromResourceEx((PBYTE)s_png_rgba, sizeof(s_png_rgba), TRUE, 0x00030000, 16, 16, 0);
    CheckIcon("CreateIconFromResourceEx RGBA", hIcon, TRUE);
    if (hIcon) DestroyIcon(hIcon);
    hIcon = CreateIconFromResourceEx((PBYTE)s_png_rgb, sizeof(s_png_rgb), TRUE, 0x00030000, 16, 16, 0);
    CheckIcon("CreateIconFromResourceEx RGB", hIcon, FALSE);
    if (hIcon) DestroyIcon(hIcon);
}
