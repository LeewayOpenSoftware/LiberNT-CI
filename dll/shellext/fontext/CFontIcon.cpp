/*
 * PROJECT:     ReactOS Font Shell Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     "Abc" sample icons for the items of the Fonts folder
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "precomp.h"


class CFontIcon :
    public CComCoClass<CFontIcon>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IExtractIconW,
    public IExtractIconA
{
    FontIconSpec m_Spec;

    HICON Render(int cx)
    {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = cx;
        bi.bmiHeader.biHeight = -cx;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC hdc = CreateCompatibleDC(NULL);
        if (!hdc)
            return NULL;
        PULONG bits = NULL;
        HBITMAP hbmColor = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (PVOID*)&bits, NULL, 0);
        HBITMAP hbmMask = CreateBitmap(cx, cx, 1, 1, NULL);
        if (!hbmColor || !hbmMask || !bits)
        {
            if (hbmColor)
                DeleteObject(hbmColor);
            if (hbmMask)
                DeleteObject(hbmMask);
            DeleteDC(hdc);
            return NULL;
        }
        ZeroMemory(bits, cx * cx * sizeof(ULONG));
        HGDIOBJ hOld = SelectObject(hdc, hbmColor);

        int margin = max(1, cx / 8), depth = m_Spec.Stack ? max(2, cx / 10) : 0;
        RECT front = { margin, margin + depth, cx - margin - depth, cx - 1 };
        RECT pages[3];
        int cPages = 0;
        if (m_Spec.Stack)
        {
            RECT p2 = { front.left + depth, front.top - depth, front.right + depth, front.bottom - depth };
            RECT p1 = { front.left + depth / 2, front.top - depth / 2, front.right + depth / 2, front.bottom - depth / 2 };
            pages[cPages++] = p2;
            pages[cPages++] = p1;
        }
        pages[cPages++] = front;

        HBRUSH hWhite = (HBRUSH)GetStockObject(WHITE_BRUSH);
        HBRUSH hBorder = CreateSolidBrush(RGB(120, 120, 120));
        for (int i = 0; i < cPages; ++i)
        {
            FillRect(hdc, &pages[i], hWhite);
            FrameRect(hdc, &pages[i], hBorder);
        }
        DeleteObject(hBorder);

        int height = max(8, (front.bottom - front.top) * 45 / 100);
        HFONT hFont = CreateFontW(-height, 0, 0, 0, m_Spec.Weight, m_Spec.Italic, 0, 0, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                                  m_Spec.Face);
        if (hFont)
        {
            HGDIOBJ hOldFont = SelectObject(hdc, hFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0, 0, 0));
            RECT text = front;
            InflateRect(&text, -1, -1);
            DrawTextW(hdc, cx < 24 ? L"A" : L"Abc", -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
            SelectObject(hdc, hOldFont);
            DeleteObject(hFont);
        }
        GdiFlush();

        // GDI does not write alpha: pages are opaque, the rest stays transparent
        for (int y = 0; y < cx; ++y)
        {
            for (int x = 0; x < cx; ++x)
            {
                bool inside = false;
                for (int i = 0; i < cPages && !inside; ++i)
                    inside = x >= pages[i].left && x < pages[i].right && y >= pages[i].top && y < pages[i].bottom;
                if (inside)
                    bits[y * cx + x] |= 0xFF000000;
            }
        }

        SelectObject(hdc, hOld);
        ICONINFO ii = {};
        ii.fIcon = TRUE;
        ii.hbmColor = hbmColor;
        ii.hbmMask = hbmMask;
        HICON hIcon = CreateIconIndirect(&ii);
        DeleteObject(hbmColor);
        DeleteObject(hbmMask);
        DeleteDC(hdc);
        return hIcon;
    }

public:
    HRESULT Initialize(const FontIconSpec* spec)
    {
        m_Spec = *spec;
        return S_OK;
    }

    // *** IExtractIconW methods ***
    STDMETHODIMP GetIconLocation(UINT uFlags, PWSTR pszIconFile, UINT cchMax, int* piIndex, UINT* pwFlags) override
    {
        if (!pszIconFile || !piIndex || !pwFlags)
            return E_POINTER;
        if (FAILED(StringCchCopyW(pszIconFile, cchMax, m_Spec.File)))
            return E_FAIL;
        *piIndex = m_Spec.Stack ? 1 : 0;
        *pwFlags = GIL_NOTFILENAME;
        return S_OK;
    }

    STDMETHODIMP Extract(PCWSTR pszFile, UINT nIconIndex, HICON* phiconLarge, HICON* phiconSmall, UINT nIconSize) override
    {
        int cxLarge = LOWORD(nIconSize), cxSmall = HIWORD(nIconSize);
        HICON hLarge = cxLarge ? Render(cxLarge) : NULL;
        HICON hSmall = cxSmall ? Render(cxSmall) : NULL;
        if (phiconLarge)
            *phiconLarge = hLarge;
        else if (hLarge)
            DestroyIcon(hLarge);
        if (phiconSmall)
            *phiconSmall = hSmall;
        else if (hSmall)
            DestroyIcon(hSmall);
        return (hLarge || hSmall) ? S_OK : E_FAIL;
    }

    // *** IExtractIconA methods ***
    STDMETHODIMP GetIconLocation(UINT uFlags, PSTR pszIconFile, UINT cchMax, int* piIndex, UINT* pwFlags) override
    {
        WCHAR wide[MAX_PATH];
        HRESULT hr = GetIconLocation(uFlags, wide, _countof(wide), piIndex, pwFlags);
        if (SUCCEEDED(hr) && !WideCharToMultiByte(CP_ACP, 0, wide, -1, pszIconFile, cchMax, NULL, NULL))
            hr = E_FAIL;
        return hr;
    }

    STDMETHODIMP Extract(PCSTR pszFile, UINT nIconIndex, HICON* phiconLarge, HICON* phiconSmall, UINT nIconSize) override
    {
        return Extract(L"", nIconIndex, phiconLarge, phiconSmall, nIconSize);
    }

    DECLARE_NO_REGISTRY()
    DECLARE_NOT_AGGREGATABLE(CFontIcon)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CFontIcon)
        COM_INTERFACE_ENTRY_IID(IID_IExtractIconW, IExtractIconW)
        COM_INTERFACE_ENTRY_IID(IID_IExtractIconA, IExtractIconA)
    END_COM_MAP()
};

HRESULT _CFontIcon_Create(const FontIconSpec* spec, REFIID riid, LPVOID* ppvOut)
{
    return ShellObjectCreatorInit<CFontIcon>(spec, riid, ppvOut);
}
