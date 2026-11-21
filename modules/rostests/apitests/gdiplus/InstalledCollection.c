/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests that the GDI+ installed font collection is usable and resolves the shell font
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <apitest.h>
#include <windows.h>

typedef struct
{
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} STARTUPINPUT;

int WINAPI GdiplusStartup(ULONG_PTR *token, const STARTUPINPUT *input, void *output);
void WINAPI GdiplusShutdown(ULONG_PTR token);
int WINAPI GdipNewInstalledFontCollection(void **collection);
int WINAPI GdipGetFontCollectionFamilyCount(void *collection, INT *count);
int WINAPI GdipCreateFontFamilyFromName(const WCHAR *name, void *collection, void **family);
int WINAPI GdipDeleteFontFamily(void *family);
int WINAPI GdipCreateFontFromLogfontW(HDC hdc, const LOGFONTW *logfont, void **font);
int WINAPI GdipDeleteFont(void *font);

static void TestFamily(const WCHAR *name)
{
    void *family = NULL;
    int status = GdipCreateFontFamilyFromName(name, NULL, &family);

    ok(status == 0, "GdipCreateFontFamilyFromName(%ls) returned %d\n", name, status);
    if (family)
        GdipDeleteFontFamily(family);
}

START_TEST(InstalledCollection)
{
    STARTUPINPUT input = { 1, NULL, FALSE, FALSE };
    ULONG_PTR token;
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    void *collection = NULL, *font = NULL;
    INT count = 0;
    int status;
    HDC hdc;

    if (GdiplusStartup(&token, &input, NULL))
    {
        skip("GDI+ did not start\n");
        return;
    }

    status = GdipNewInstalledFontCollection(&collection);
    ok(status == 0, "GdipNewInstalledFontCollection returned %d\n", status);
    status = GdipGetFontCollectionFamilyCount(collection, &count);
    ok(status == 0 && count > 0, "Installed collection has %d families, status %d\n", count, status);

    TestFamily(L"Arial");
    TestFamily(L"Selawik");
    TestFamily(L"Segoe UI");

    /* The shell title font: the message font of the system metrics on a screen DC */
    ok(SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0), "No system metrics\n");
    trace("message font: '%ls' height %ld\n", ncm.lfMessageFont.lfFaceName, ncm.lfMessageFont.lfHeight);
    hdc = GetDC(NULL);
    status = GdipCreateFontFromLogfontW(hdc, &ncm.lfMessageFont, &font);
    ok(status == 0, "GdipCreateFontFromLogfontW(lfMessageFont) returned %d\n", status);
    if (font)
        GdipDeleteFont(font);
    ReleaseDC(NULL, hdc);

    GdiplusShutdown(token);
}
