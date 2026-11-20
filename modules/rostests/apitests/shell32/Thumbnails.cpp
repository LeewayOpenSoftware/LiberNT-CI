/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests shell item images, thumbnail handlers and the thumbnail cache
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "shelltest.h"
#include <shellutils.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <shlobj_undoc.h>
#include <shlguid_undoc.h>
#include <stdio.h>

static WCHAR g_Dir[MAX_PATH];
static WCHAR g_Bmp[MAX_PATH];
static WCHAR g_Png[MAX_PATH];
static WCHAR g_Txt[MAX_PATH];
static WCHAR g_Lnk[MAX_PATH];
static WCHAR g_Big[MAX_PATH];

#define RED   RGB(255, 0, 0)
#define BLUE  RGB(0, 0, 255)

static BOOL
WriteBytes(LPCWSTR Path, const void *Data, DWORD Size)
{
    HANDLE File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD Written = 0;
    BOOL Ok;

    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = WriteFile(File, Data, Size, &Written, NULL) && Written == Size;
    CloseHandle(File);
    return Ok;
}

static BOOL
WriteBmp(LPCWSTR Path, LONG Width, LONG Height)
{
    LONG Stride = (Width * 3 + 3) & ~3;
    DWORD Size = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + Stride * Height;
    BYTE *Data = (BYTE *)calloc(1, Size);
    BITMAPFILEHEADER *File = (BITMAPFILEHEADER *)Data;
    BITMAPINFOHEADER *Info = (BITMAPINFOHEADER *)(File + 1);
    BYTE *Bits = (BYTE *)(Info + 1);
    BOOL Ok;

    if (!Data)
        return FALSE;
    File->bfType = 0x4d42;
    File->bfSize = Size;
    File->bfOffBits = sizeof(*File) + sizeof(*Info);
    Info->biSize = sizeof(*Info);
    Info->biWidth = Width;
    Info->biHeight = Height;
    Info->biPlanes = 1;
    Info->biBitCount = 24;
    for (LONG y = 0; y < Height; y++)
    {
        for (LONG x = 0; x < Width; x++)
        {
            BYTE *Pixel = Bits + y * Stride + x * 3;
            COLORREF Color = x < Width / 2 ? RED : BLUE;
            Pixel[0] = GetBValue(Color);
            Pixel[1] = GetGValue(Color);
            Pixel[2] = GetRValue(Color);
        }
    }
    Ok = WriteBytes(Path, Data, Size);
    free(Data);
    return Ok;
}

static DWORD
Crc32(const BYTE *Data, SIZE_T Size, DWORD Crc)
{
    Crc = ~Crc;
    for (SIZE_T i = 0; i < Size; i++)
    {
        Crc ^= Data[i];
        for (int k = 0; k < 8; k++)
            Crc = (Crc >> 1) ^ (0xedb88320 & (0 - (Crc & 1)));
    }
    return ~Crc;
}

static void
PutBe32(BYTE *Out, DWORD Value)
{
    Out[0] = (BYTE)(Value >> 24);
    Out[1] = (BYTE)(Value >> 16);
    Out[2] = (BYTE)(Value >> 8);
    Out[3] = (BYTE)Value;
}

static SIZE_T
PutChunk(BYTE *Out, const char *Type, const BYTE *Data, DWORD Size)
{
    PutBe32(Out, Size);
    memcpy(Out + 4, Type, 4);
    if (Size)
        memcpy(Out + 8, Data, Size);
    PutBe32(Out + 8 + Size, Crc32(Out + 4, Size + 4, 0));
    return 12 + Size;
}

static BOOL
WritePng(LPCWSTR Path, LONG Width, LONG Height)
{
    DWORD RowSize = 1 + Width * 4;
    DWORD Raw = RowSize * Height;
    DWORD Blocks = (Raw + 65534) / 65535;
    DWORD ZSize = 2 + Raw + Blocks * 5 + 4;
    BYTE *Pixels = (BYTE *)calloc(1, Raw);
    BYTE *Z = (BYTE *)calloc(1, ZSize);
    BYTE *Out = (BYTE *)calloc(1, ZSize + 128);
    BYTE Header[13];
    DWORD A = 1, B = 0, Pos = 0, Done = 0;
    SIZE_T Size = 0;
    BOOL Ok;

    if (!Pixels || !Z || !Out)
        return FALSE;
    for (LONG y = 0; y < Height; y++)
    {
        BYTE *Row = Pixels + y * RowSize;
        for (LONG x = 0; x < Width; x++)
        {
            BYTE *Pixel = Row + 1 + x * 4;
            COLORREF Color = x < Width / 2 ? RED : BLUE;
            Pixel[0] = GetRValue(Color);
            Pixel[1] = GetGValue(Color);
            Pixel[2] = GetBValue(Color);
            Pixel[3] = y < Height / 2 ? 255 : 128;
        }
    }
    for (DWORD i = 0; i < Raw; i++)
    {
        A = (A + Pixels[i]) % 65521;
        B = (B + A) % 65521;
    }
    Z[Pos++] = 0x78;
    Z[Pos++] = 0x01;
    while (Done < Raw)
    {
        DWORD Chunk = min(65535, Raw - Done);
        Z[Pos++] = (Done + Chunk == Raw) ? 1 : 0;
        Z[Pos++] = (BYTE)Chunk;
        Z[Pos++] = (BYTE)(Chunk >> 8);
        Z[Pos++] = (BYTE)~Chunk;
        Z[Pos++] = (BYTE)(~Chunk >> 8);
        memcpy(Z + Pos, Pixels + Done, Chunk);
        Pos += Chunk;
        Done += Chunk;
    }
    PutBe32(Z + Pos, (B << 16) | A);
    Pos += 4;

    memcpy(Out, "\x89PNG\r\n\x1a\n", 8);
    Size = 8;
    PutBe32(Header, Width);
    PutBe32(Header + 4, Height);
    Header[8] = 8;
    Header[9] = 6;
    Header[10] = Header[11] = Header[12] = 0;
    Size += PutChunk(Out + Size, "IHDR", Header, sizeof(Header));
    Size += PutChunk(Out + Size, "IDAT", Z, Pos);
    Size += PutChunk(Out + Size, "IEND", NULL, 0);
    Ok = WriteBytes(Path, Out, (DWORD)Size);
    free(Pixels);
    free(Z);
    free(Out);
    return Ok;
}

static BOOL
BitmapSize(HBITMAP Bitmap, LONG *Width, LONG *Height, WORD *Bpp)
{
    BITMAP Info;

    if (!GetObjectW(Bitmap, sizeof(Info), &Info))
        return FALSE;
    *Width = Info.bmWidth;
    *Height = Info.bmHeight;
    *Bpp = Info.bmBitsPixel;
    return TRUE;
}

static COLORREF
SamplePixel(HBITMAP Bitmap, LONG x, LONG y)
{
    HDC Dc = CreateCompatibleDC(NULL);
    HGDIOBJ Old = SelectObject(Dc, Bitmap);
    COLORREF Color = GetPixel(Dc, x, y);

    SelectObject(Dc, Old);
    DeleteDC(Dc);
    return Color;
}

static BOOL
IsNear(COLORREF A, COLORREF B)
{
    return abs(GetRValue(A) - GetRValue(B)) < 40 &&
           abs(GetGValue(A) - GetGValue(B)) < 40 &&
           abs(GetBValue(A) - GetBValue(B)) < 40;
}

static void
TestImageFactory(void)
{
    CComPtr<IShellItemImageFactory> Factory;
    HBITMAP Bitmap = NULL;
    LONG Width, Height;
    WORD Bpp;
    HRESULT hr;
    SIZE Size;

    hr = SHCreateItemFromParsingName(g_Bmp, NULL, IID_PPV_ARG(IShellItemImageFactory, &Factory));
    ok_hr(hr, S_OK);
    if (FAILED(hr))
        return;

    Size.cx = Size.cy = 32;
    hr = Factory->GetImage(Size, SIIGBF_THUMBNAILONLY, &Bitmap);
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr) && Bitmap)
    {
        ok(BitmapSize(Bitmap, &Width, &Height, &Bpp), "GetObject failed\n");
        ok_long(Width, 32);
        ok_long(Height, 24);
        ok_int(Bpp, 32);
        ok(IsNear(SamplePixel(Bitmap, Width / 4, Height / 2), RED), "left %06lx\n", SamplePixel(Bitmap, Width / 4, Height / 2));
        ok(IsNear(SamplePixel(Bitmap, Width * 3 / 4, Height / 2), BLUE), "right %06lx\n", SamplePixel(Bitmap, Width * 3 / 4, Height / 2));
        DeleteObject(Bitmap);
        Bitmap = NULL;
    }

    Size.cx = Size.cy = 256;
    hr = Factory->GetImage(Size, SIIGBF_THUMBNAILONLY, &Bitmap);
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr) && Bitmap)
    {
        BitmapSize(Bitmap, &Width, &Height, &Bpp);
        trace("256 request on 64x48 gives %ldx%ld\n", Width, Height);
        ok(Width <= 256 && Height <= 256 && Width * 48 == Height * 64, "aspect kept: %ldx%ld\n", Width, Height);
        DeleteObject(Bitmap);
        Bitmap = NULL;
    }

    Size.cx = Size.cy = 32;
    hr = Factory->GetImage(Size, SIIGBF_ICONONLY, &Bitmap);
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr) && Bitmap)
    {
        BitmapSize(Bitmap, &Width, &Height, &Bpp);
        ok_long(Width, 32);
        ok_long(Height, 32);
        DeleteObject(Bitmap);
        Bitmap = NULL;
    }

    Factory.Release();
    hr = SHCreateItemFromParsingName(g_Txt, NULL, IID_PPV_ARG(IShellItemImageFactory, &Factory));
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        hr = Factory->GetImage(Size, SIIGBF_THUMBNAILONLY, &Bitmap);
        ok(FAILED(hr), "text thumbnail: %#lx\n", hr);
        trace("text thumbnail only: %#lx\n", hr);
        if (Bitmap)
            DeleteObject(Bitmap);
        Bitmap = NULL;
        hr = Factory->GetImage(Size, SIIGBF_RESIZETOFIT, &Bitmap);
        ok_hr(hr, S_OK);
        if (Bitmap)
        {
            BitmapSize(Bitmap, &Width, &Height, &Bpp);
            ok_long(Width, 32);
            ok_long(Height, 32);
            DeleteObject(Bitmap);
            Bitmap = NULL;
        }
        Factory.Release();
    }

    hr = SHCreateItemFromParsingName(g_Dir, NULL, IID_PPV_ARG(IShellItemImageFactory, &Factory));
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        hr = Factory->GetImage(Size, SIIGBF_ICONONLY, &Bitmap);
        ok_hr(hr, S_OK);
        if (Bitmap)
            DeleteObject(Bitmap);
    }
}

static void
TestThumbnailHandler(void)
{
    CComPtr<IShellItem> Item;
    CComPtr<IThumbnailProvider> Provider;
    WTS_ALPHATYPE Alpha = WTSAT_UNKNOWN;
    HBITMAP Bitmap = NULL;
    LONG Width, Height;
    WORD Bpp;
    HRESULT hr;

    hr = SHCreateItemFromParsingName(g_Bmp, NULL, IID_PPV_ARG(IShellItem, &Item));
    ok_hr(hr, S_OK);
    if (FAILED(hr))
        return;
    hr = Item->BindToHandler(NULL, BHID_ThumbnailHandler, IID_PPV_ARG(IThumbnailProvider, &Provider));
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        hr = Provider->GetThumbnail(32, &Bitmap, &Alpha);
        ok_hr(hr, S_OK);
        if (SUCCEEDED(hr) && Bitmap)
        {
            BitmapSize(Bitmap, &Width, &Height, &Bpp);
            ok(max(Width, Height) == 32, "provider size %ldx%ld\n", Width, Height);
            ok(Width * 48 == Height * 64, "provider aspect %ldx%ld\n", Width, Height);
            trace("bmp alpha type %d bpp %u\n", Alpha, Bpp);
            DeleteObject(Bitmap);
            Bitmap = NULL;
        }
        Provider.Release();
    }
    Item.Release();

    hr = SHCreateItemFromParsingName(g_Png, NULL, IID_PPV_ARG(IShellItem, &Item));
    if (SUCCEEDED(hr) &&
        SUCCEEDED(Item->BindToHandler(NULL, BHID_ThumbnailHandler, IID_PPV_ARG(IThumbnailProvider, &Provider))))
    {
        hr = Provider->GetThumbnail(64, &Bitmap, &Alpha);
        ok_hr(hr, S_OK);
        ok_int(Alpha, WTSAT_ARGB);
        if (Bitmap)
            DeleteObject(Bitmap);
        Provider.Release();
    }
    else
    {
        ok(FALSE, "png thumbnail handler: %#lx\n", hr);
    }
    Item.Release();

    hr = SHCreateItemFromParsingName(g_Txt, NULL, IID_PPV_ARG(IShellItem, &Item));
    if (SUCCEEDED(hr))
    {
        hr = Item->BindToHandler(NULL, BHID_ThumbnailHandler, IID_PPV_ARG(IThumbnailProvider, &Provider));
        ok(FAILED(hr), "text thumbnail handler: %#lx\n", hr);
        trace("text BHID_ThumbnailHandler: %#lx\n", hr);
    }
}

static void
TestThumbnailCache(void)
{
    CComPtr<IThumbnailCache> Cache;
    CComPtr<IShellItem> Item, Text, Png;
    CComPtr<ISharedBitmap> Shared;
    WTS_CACHEFLAGS Flags = WTS_DEFAULT;
    WTS_THUMBNAILID Id;
    WTS_ALPHATYPE Format;
    HBITMAP Bitmap = NULL;
    HRESULT hr;
    SIZE Size;

    hr = CoCreateInstance(CLSID_LocalThumbnailCache, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IThumbnailCache, &Cache));
    ok_hr(hr, S_OK);
    if (FAILED(hr))
        return;
    if (FAILED(SHCreateItemFromParsingName(g_Bmp, NULL, IID_PPV_ARG(IShellItem, &Item))) ||
        FAILED(SHCreateItemFromParsingName(g_Txt, NULL, IID_PPV_ARG(IShellItem, &Text))) ||
        FAILED(SHCreateItemFromParsingName(g_Png, NULL, IID_PPV_ARG(IShellItem, &Png))))
    {
        skip("no shell items\n");
        return;
    }

    hr = Cache->GetThumbnail(Png, 96, WTS_INCACHEONLY, &Shared, &Flags, &Id);
    ok(FAILED(hr), "uncached png in cache-only mode: %#lx\n", hr);
    trace("uncached WTS_INCACHEONLY: %#lx\n", hr);
    Shared.Release();

    ZeroMemory(&Id, sizeof(Id));
    hr = Cache->GetThumbnail(Item, 96, WTS_EXTRACT, &Shared, &Flags, &Id);
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr) && Shared)
    {
        ok_hr(Shared->GetSize(&Size), S_OK);
        trace("cache size for 96: %ldx%ld flags %#x\n", Size.cx, Size.cy, Flags);
        ok(Size.cx <= 96 && Size.cy <= 96 && Size.cx * 48 == Size.cy * 64, "cache size %ldx%ld\n", Size.cx, Size.cy);
        ok_hr(Shared->GetFormat(&Format), S_OK);
        ok_hr(Shared->GetSharedBitmap(&Bitmap), S_OK);
        ok(Bitmap != NULL, "no shared bitmap\n");
    }
    Shared.Release();

    Flags = WTS_DEFAULT;
    hr = Cache->GetThumbnail(Item, 96, WTS_INCACHEONLY, &Shared, &Flags, NULL);
    ok_hr(hr, S_OK);
    ok(Flags & WTS_CACHED, "second lookup flags %#x\n", Flags);
    Shared.Release();

    Flags = WTS_DEFAULT;
    hr = Cache->GetThumbnailByID(Id, 96, &Shared, &Flags);
    ok_hr(hr, S_OK);
    Shared.Release();

    hr = Cache->GetThumbnail(Text, 96, WTS_EXTRACT, &Shared, &Flags, NULL);
    ok(FAILED(hr), "text in thumbnail cache: %#lx\n", hr);
    trace("text WTS_EXTRACT: %#lx\n", hr);
}

static void
TestSharedBitmap(void)
{
    CComPtr<ISharedBitmap> Shared;
    BITMAPINFO Info = {{sizeof(BITMAPINFOHEADER), 20, -10, 1, 32, BI_RGB}};
    HBITMAP Bitmap, Out = NULL;
    WTS_ALPHATYPE Format = WTSAT_UNKNOWN;
    VOID *Bits;
    HRESULT hr;
    SIZE Size;

    hr = CoCreateInstance(CLSID_SharedBitmap, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(ISharedBitmap, &Shared));
    ok_hr(hr, S_OK);
    if (FAILED(hr))
        return;
    hr = Shared->GetSharedBitmap(&Out);
    ok(FAILED(hr), "empty shared bitmap: %#lx\n", hr);
    Bitmap = CreateDIBSection(NULL, &Info, DIB_RGB_COLORS, &Bits, NULL, 0);
    ok_hr(Shared->InitializeBitmap(Bitmap, WTSAT_ARGB), S_OK);
    ok_hr(Shared->GetSize(&Size), S_OK);
    ok_long(Size.cx, 20);
    ok_long(Size.cy, 10);
    ok_hr(Shared->GetFormat(&Format), S_OK);
    ok_int(Format, WTSAT_ARGB);
    ok_hr(Shared->GetSharedBitmap(&Out), S_OK);
    ok(Out == Bitmap, "shared %p vs %p\n", Out, Bitmap);
    Out = NULL;
    ok_hr(Shared->Detach(&Out), S_OK);
    ok(Out == Bitmap, "detached %p vs %p\n", Out, Bitmap);
    hr = Shared->GetSharedBitmap(&Out);
    ok(FAILED(hr), "after detach: %#lx\n", hr);
    DeleteObject(Bitmap);
}

/* Sink for the background generator (CLSID_Thumbnail) */
#define THUMB_SINK_MSG (WM_USER + 100)
#define THUMB_RESULT_MAX 16

struct ThumbResult
{
    DWORD Id;
    HBITMAP Bitmap;
};

static ThumbResult g_Results[THUMB_RESULT_MAX];
static int g_ResultCount;

static LRESULT CALLBACK
ThumbSinkProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == THUMB_SINK_MSG)
    {
        if (g_ResultCount < THUMB_RESULT_MAX)
        {
            g_Results[g_ResultCount].Id = (DWORD)lParam;
            g_Results[g_ResultCount].Bitmap = (HBITMAP)wParam;
            g_ResultCount++;
        }
        else if (wParam)
        {
            DeleteObject((HBITMAP)wParam);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void
PumpSink(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static BOOL
PumpUntil(int Count, DWORD TimeoutMs)
{
    DWORD Start = GetTickCount();

    for (;;)
    {
        PumpSink();
        if (g_ResultCount >= Count)
            return TRUE;
        if (GetTickCount() - Start > TimeoutMs)
            return FALSE;
        Sleep(10);
    }
}

static const ThumbResult *
LastResult(DWORD Id)
{
    for (int i = g_ResultCount - 1; i >= 0; i--)
        if (g_Results[i].Id == Id)
            return &g_Results[i];
    return NULL;
}

static void
ClearResults(void)
{
    for (int i = 0; i < g_ResultCount; i++)
        if (g_Results[i].Bitmap)
            DeleteObject(g_Results[i].Bitmap);
    g_ResultCount = 0;
}

static void
TestThumbnailGenerator(void)
{
    CComPtr<IThumbnail3> Gen;
    CComPtr<IShellItem> Text;
    CComHeapPtr<ITEMIDLIST> PngPidl;
    const ThumbResult *Result;
    WNDCLASSW wc = { 0 };
    HWND Sink;
    LONG Width, Height;
    WORD Bpp;
    HRESULT hr;

    wc.lpfnWndProc = ThumbSinkProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ThumbnailsApitestSink";
    RegisterClassW(&wc);
    Sink = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    ok(Sink != NULL, "sink window: %lu\n", GetLastError());
    if (!Sink)
        return;

    hr = CoCreateInstance(CLSID_Thumbnail, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IThumbnail3, &Gen));
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        /* Nowhere to deliver before Init() */
        hr = Gen->GetBitmap(g_Bmp, 1, 64, 64);
        ok(FAILED(hr), "request before Init: %#lx\n", hr);

        ok_hr(Gen->Init(Sink, THUMB_SINK_MSG), S_OK);
        ok_hr(Gen->GetBitmap(NULL, 1, 64, 64), E_INVALIDARG);
        hr = Gen->GetBitmap(L"Z:\\no\\such\\thumbnails-apitest.bmp", 1, 64, 64);
        ok(FAILED(hr), "missing file: %#lx\n", hr);
        ok_hr(Gen->GetBitmap(g_Bmp, 1, 0, 64), E_INVALIDARG);

        /* A bitmap arrives as a message, fitted into the requested box */
        ok_hr(Gen->GetBitmap(g_Bmp, 1, 64, 64), S_OK);
        ok(PumpUntil(1, 5000), "no thumbnail message within 5 s\n");
        Result = LastResult(1);
        ok(Result != NULL, "no result for item 1\n");
        if (Result && Result->Bitmap)
        {
            BitmapSize(Result->Bitmap, &Width, &Height, &Bpp);
            ok_long(Width, 64);
            ok_long(Height, 48);
            ok(IsNear(SamplePixel(Result->Bitmap, Width / 4, Height / 2), RED), "left %06lx\n", SamplePixel(Result->Bitmap, Width / 4, Height / 2));
            ok(IsNear(SamplePixel(Result->Bitmap, Width * 3 / 4, Height / 2), BLUE), "right %06lx\n", SamplePixel(Result->Bitmap, Width * 3 / 4, Height / 2));
        }
        else if (Result)
        {
            ok(FALSE, "bmp delivered without a bitmap\n");
        }
        ClearResults();

        /* No handler for text: nothing is posted, the view keeps its icon */
        hr = SHCreateItemFromParsingName(g_Txt, NULL, IID_PPV_ARG(IShellItem, &Text));
        ok_hr(hr, S_OK);
        if (SUCCEEDED(hr))
        {
            ok_hr(Gen->GetBitmapFromShellItem(Text, 2, 64, 64), S_OK);
            ok(!PumpUntil(1, 2000), "message for an item without thumbnail (bitmap %p)\n", g_ResultCount ? g_Results[0].Bitmap : NULL);
        }
        ClearResults();

        /* Pidl entry point; the newest request for an item wins */
        hr = SHParseDisplayName(g_Png, NULL, &PngPidl, 0, NULL);
        ok_hr(hr, S_OK);
        if (SUCCEEDED(hr))
        {
            ok_hr(Gen->GetBitmapFromIDList(PngPidl, 3, 32, 32), S_OK);
            ok_hr(Gen->GetBitmapFromIDList(PngPidl, 3, 16, 16), S_OK);
            ok(PumpUntil(1, 5000), "no message for the png item within 5 s\n");
            Sleep(300);
            PumpSink();
            Result = LastResult(3);
            ok(Result != NULL && Result->Bitmap != NULL, "no png thumbnail\n");
            if (Result && Result->Bitmap)
            {
                BitmapSize(Result->Bitmap, &Width, &Height, &Bpp);
                ok(max(Width, Height) <= 16, "last result for item 3 is %ldx%ld, expected the 16 request\n", Width, Height);
            }
        }
        ClearResults();

        /* Init(NULL, 0) cancels: nothing is posted after it returns */
        ok_hr(Gen->GetBitmap(g_Bmp, 4, 48, 48), S_OK);
        ok_hr(Gen->GetBitmap(g_Png, 5, 48, 48), S_OK);
        ok_hr(Gen->Init(NULL, 0), S_OK);
        PumpSink();
        ClearResults();
        ok(!PumpUntil(1, 500), "message after cancel (item %lu)\n", g_ResultCount ? g_Results[0].Id : 0);
        ClearResults();

        /* Release() must not wait for the extraction in flight: a slow provider
         * would freeze the view that navigates away. A big picture keeps the
         * worker busy; Release() has to come back at once. */
        ok_hr(Gen->Init(Sink, THUMB_SINK_MSG), S_OK);
        if (WriteBmp(g_Big, 2400, 2400))
        {
            DWORD Start, Elapsed;
            int Tries;

            ok_hr(Gen->GetBitmap(g_Big, 6, 256, 256), S_OK);
            Sleep(30);
            Start = GetTickCount();
            Gen.Release();
            Elapsed = GetTickCount() - Start;
            ok(Elapsed < 1000, "Release waited %lu ms for the extraction in flight\n", Elapsed);
            trace("Release with a 2400x2400 extraction in flight: %lu ms\n", Elapsed);
            PumpSink();
            ClearResults();
            /* the worker may still hold the file for a moment */
            for (Tries = 0; Tries < 100 && !DeleteFileW(g_Big) && GetLastError() == ERROR_SHARING_VIOLATION; Tries++)
                Sleep(50);
        }
        else
        {
            skip("cannot write the big bitmap: %lu\n", GetLastError());
            ok_hr(Gen->GetBitmap(g_Bmp, 6, 48, 48), S_OK);
            Gen.Release();
            PumpSink();
            ClearResults();
        }
    }

    DestroyWindow(Sink);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

static void
TestIconsOnly(void)
{
    CComPtr<IShellItemImageFactory> Factory;
    SHELLSTATE State;
    BOOL OldIconsOnly, NewIconsOnly;
    HBITMAP Bitmap = NULL;
    LONG Width, Height;
    WORD Bpp;
    HRESULT hr;
    SIZE Size;

    ZeroMemory(&State, sizeof(State));
    SHGetSetSettings(&State, SSF_ICONSONLY, FALSE);
    OldIconsOnly = State.fIconsOnly;
    NewIconsOnly = TRUE;
    State.fIconsOnly = NewIconsOnly;
    SHGetSetSettings(&State, SSF_ICONSONLY, TRUE);

    hr = SHCreateItemFromParsingName(g_Bmp, NULL, IID_PPV_ARG(IShellItemImageFactory, &Factory));
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        Size.cx = Size.cy = 32;
        hr = Factory->GetImage(Size, SIIGBF_THUMBNAILONLY, &Bitmap);
        ok(FAILED(hr), "thumbnail with IconsOnly set: %#lx\n", hr);
        if (Bitmap)
            DeleteObject(Bitmap);
        Bitmap = NULL;
        hr = Factory->GetImage(Size, SIIGBF_RESIZETOFIT, &Bitmap);
        ok_hr(hr, S_OK);
        if (Bitmap)
        {
            BitmapSize(Bitmap, &Width, &Height, &Bpp);
            ok_long(Width, 32);
            ok_long(Height, 32);
            DeleteObject(Bitmap);
        }
    }

    State.fIconsOnly = OldIconsOnly;
    SHGetSetSettings(&State, SSF_ICONSONLY, TRUE);
}

/* Shortcut thumbnails: lnkfile\shellex\{IThumbnailProvider} -> CLSID_ShellLink, which forwards to the target */
static void
TestLinkThumbnail(void)
{
    static const WCHAR szKey[] = L"lnkfile\\shellex\\{e357fccd-a995-4576-b01f-234630154e96}";
    static const WCHAR szClsid[] = L"{00021401-0000-0000-C000-000000000046}";
    CComPtr<IShellLinkW> Link;
    CComPtr<IPersistFile> File;
    CComPtr<IShellItem> Item;
    CComPtr<IThumbnailProvider> Provider;
    CComPtr<IShellItemImageFactory> Factory;
    WTS_ALPHATYPE Alpha = WTSAT_UNKNOWN;
    HBITMAP Bitmap = NULL;
    LONG Width, Height;
    WORD Bpp;
    HKEY hKey;
    BOOL CreatedKey = FALSE;
    HRESULT hr;
    SIZE Size;

    /* The install hive registers the handler; an image updated in place may lack it */
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, szKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
    }
    else if (RegCreateKeyExW(HKEY_CLASSES_ROOT, szKey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE *)szClsid, sizeof(szClsid));
        RegCloseKey(hKey);
        CreatedKey = TRUE;
        trace("registered lnkfile thumbnail handler for the test\n");
    }
    else
    {
        skip("cannot register the lnkfile thumbnail handler: %lu\n", GetLastError());
        return;
    }

    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IShellLinkW, &Link));
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        ok_hr(Link->SetPath(g_Bmp), S_OK);
        hr = Link->QueryInterface(IID_PPV_ARG(IPersistFile, &File));
        ok_hr(hr, S_OK);
        if (SUCCEEDED(hr))
            ok_hr(File->Save(g_Lnk, TRUE), S_OK);
        File.Release();
        Link.Release();
    }

    if (GetFileAttributesW(g_Lnk) != INVALID_FILE_ATTRIBUTES)
    {
        hr = SHCreateItemFromParsingName(g_Lnk, NULL, IID_PPV_ARG(IShellItem, &Item));
        ok_hr(hr, S_OK);
        if (SUCCEEDED(hr))
        {
            hr = Item->BindToHandler(NULL, BHID_ThumbnailHandler, IID_PPV_ARG(IThumbnailProvider, &Provider));
            ok_hr(hr, S_OK);
            if (SUCCEEDED(hr))
            {
                hr = Provider->GetThumbnail(32, &Bitmap, &Alpha);
                ok_hr(hr, S_OK);
                if (SUCCEEDED(hr) && Bitmap)
                {
                    BitmapSize(Bitmap, &Width, &Height, &Bpp);
                    ok(max(Width, Height) == 32, "link provider size %ldx%ld\n", Width, Height);
                    ok(Width * 48 == Height * 64, "link provider aspect %ldx%ld\n", Width, Height);
                    DeleteObject(Bitmap);
                    Bitmap = NULL;
                }
            }

            hr = Item->QueryInterface(IID_PPV_ARG(IShellItemImageFactory, &Factory));
            ok_hr(hr, S_OK);
            if (SUCCEEDED(hr))
            {
                Size.cx = Size.cy = 32;
                hr = Factory->GetImage(Size, SIIGBF_THUMBNAILONLY, &Bitmap);
                ok_hr(hr, S_OK);
                if (SUCCEEDED(hr) && Bitmap)
                {
                    BitmapSize(Bitmap, &Width, &Height, &Bpp);
                    ok_long(Width, 32);
                    ok_long(Height, 24);
                    ok(IsNear(SamplePixel(Bitmap, Width / 4, Height / 2), RED), "link left %06lx\n", SamplePixel(Bitmap, Width / 4, Height / 2));
                    ok(IsNear(SamplePixel(Bitmap, Width * 3 / 4, Height / 2), BLUE), "link right %06lx\n", SamplePixel(Bitmap, Width * 3 / 4, Height / 2));
                    DeleteObject(Bitmap);
                    Bitmap = NULL;
                }
            }
        }
        DeleteFileW(g_Lnk);
    }
    else
    {
        ok(FALSE, "shortcut was not saved: %lu\n", GetLastError());
    }

    if (CreatedKey)
        RegDeleteKeyW(HKEY_CLASSES_ROOT, szKey);
}

/* Font thumbnails: ttffile\shellex\{IThumbnailProvider} -> fontext CFontThumbnail ("Abc" in the face) */
static void
TestFontThumbnail(void)
{
    static const WCHAR szKey[] = L"ttffile\\shellex\\{e357fccd-a995-4576-b01f-234630154e96}";
    static const WCHAR szClsid[] = L"{E2537D60-39C7-4E40-9560-B3386D4BB207}";
    CComPtr<IShellItemImageFactory> Factory;
    CComPtr<IUnknown> Probe;
    WCHAR szFont[MAX_PATH], szCopy[MAX_PATH];
    HBITMAP Bitmap = NULL;
    LONG Width, Height, Dark = 0;
    WORD Bpp;
    HKEY hKey;
    BOOL CreatedKey = FALSE;
    CLSID clsid;
    HRESULT hr;
    SIZE Size;

    /* The Fonts folder is a namespace extension that parses font titles, not file names:
     * test a font file in a plain directory, as a downloaded font would be */
    GetWindowsDirectoryW(szFont, _countof(szFont));
    PathAppendW(szFont, L"Fonts\\arial.ttf");
    wsprintfW(szCopy, L"%s\\sample.ttf", g_Dir);
    if (GetFileAttributesW(szFont) == INVALID_FILE_ATTRIBUTES || !CopyFileW(szFont, szCopy, FALSE))
    {
        skip("no %ls (%lu)\n", szFont, GetLastError());
        return;
    }

    /* An image updated in place may predate the class registration: register fontext.dll */
    CLSIDFromString((LPOLESTR)szClsid, &clsid);
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IUnknown, &Probe));
    if (FAILED(hr))
    {
        HMODULE hFontExt = LoadLibraryW(L"fontext.dll");
        typedef HRESULT (STDAPICALLTYPE *REGPROC)(void);
        REGPROC pReg = hFontExt ? (REGPROC)GetProcAddress(hFontExt, "DllRegisterServer") : NULL;
        trace("font thumbnail class not registered (%#lx), DllRegisterServer: %#lx\n", hr, pReg ? pReg() : E_FAIL);
    }
    Probe.Release();

    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, szKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
    }
    else if (RegCreateKeyExW(HKEY_CLASSES_ROOT, szKey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE *)szClsid, sizeof(szClsid));
        RegCloseKey(hKey);
        CreatedKey = TRUE;
        trace("registered ttffile thumbnail handler for the test\n");
    }
    else
    {
        skip("cannot register the ttffile thumbnail handler: %lu\n", GetLastError());
        return;
    }

    hr = SHCreateItemFromParsingName(szCopy, NULL, IID_PPV_ARG(IShellItemImageFactory, &Factory));
    ok_hr(hr, S_OK);
    if (SUCCEEDED(hr))
    {
        Size.cx = Size.cy = 64;
        hr = Factory->GetImage(Size, SIIGBF_THUMBNAILONLY, &Bitmap);
        ok_hr(hr, S_OK);
        if (SUCCEEDED(hr) && Bitmap)
        {
            BitmapSize(Bitmap, &Width, &Height, &Bpp);
            ok_long(Width, 64);
            ok_long(Height, 64);
            ok(IsNear(SamplePixel(Bitmap, 1, 1), RGB(255, 255, 255)), "corner %06lx\n", SamplePixel(Bitmap, 1, 1));
            for (LONG y = 0; y < Height; y += 2)
                for (LONG x = 0; x < Width; x += 2)
                    if (GetRValue(SamplePixel(Bitmap, x, y)) < 128)
                        Dark++;
            ok(Dark > 0, "no glyph pixels in the font thumbnail\n");
            trace("font thumbnail dark samples: %ld\n", Dark);
            DeleteObject(Bitmap);
        }
    }

    Factory.Release();
    DeleteFileW(szCopy);
    if (CreatedKey)
        RegDeleteKeyW(HKEY_CLASSES_ROOT, szKey);
}

START_TEST(Thumbnails)
{
    WCHAR Temp[MAX_PATH];

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GetTempPathW(_countof(Temp), Temp);
    wsprintfW(g_Dir, L"%sthumbtest%lu", Temp, GetCurrentProcessId());
    CreateDirectoryW(g_Dir, NULL);
    wsprintfW(g_Bmp, L"%s\\image.bmp", g_Dir);
    wsprintfW(g_Png, L"%s\\image.png", g_Dir);
    wsprintfW(g_Txt, L"%s\\note.txt", g_Dir);
    wsprintfW(g_Lnk, L"%s\\image.lnk", g_Dir);
    wsprintfW(g_Big, L"%s\\big.bmp", g_Dir);
    if (!WriteBmp(g_Bmp, 64, 48) || !WritePng(g_Png, 64, 48) || !WriteBytes(g_Txt, "text", 4))
    {
        skip("cannot create test files: %lu\n", GetLastError());
        CoUninitialize();
        return;
    }

    TestImageFactory();
    TestThumbnailHandler();
    TestThumbnailCache();
    TestSharedBitmap();
    TestThumbnailGenerator();
    TestIconsOnly();
    TestLinkThumbnail();
    TestFontThumbnail();

    DeleteFileW(g_Bmp);
    DeleteFileW(g_Png);
    DeleteFileW(g_Txt);
    RemoveDirectoryW(g_Dir);
    CoUninitialize();
}
