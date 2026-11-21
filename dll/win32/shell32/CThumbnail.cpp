/*
 * PROJECT:     LiberNT shell32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Background thumbnail generator
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/*
 * CThumbnail extracts thumbnails on its own worker thread and hands each one
 * to the window given to Init():
 *
 *     PostMessage(hwnd, uMsg, (WPARAM)hbmThumbnail, (LPARAM)dwItemID)
 *
 * The receiver owns hbmThumbnail. Nothing is posted when the item has no
 * thumbnail (no handler for the type, extraction failed, or "Always show
 * icons, never thumbnails" is on), so the view keeps its icon. The image
 * comes from IShellItemImageFactory::GetImage, so the thumbnail cache and
 * every registered IThumbnailProvider are shared with the other consumers.
 * A request whose dwItemID is still queued replaces the queued one; a request
 * whose dwItemID is being extracted makes that result stale, so only the
 * newest request per item is delivered. Init(NULL, 0), a new Init() and the
 * final Release() drop all pending work and nothing is posted for it
 * afterwards.
 *
 * The object and its worker thread share a reference-counted state block:
 * the final Release() only signals the worker and returns, it never waits for
 * the extraction in flight (a slow provider must not freeze the view that
 * navigates away). The worker finishes that extraction, drops the result,
 * frees the block if it holds the last reference, and leaves through
 * FreeLibraryAndExitThread so shell32 stays loaded as long as it runs.
 */

struct THUMBNAIL_REQUEST
{
    THUMBNAIL_REQUEST *pNext;
    LPITEMIDLIST pidl;
    DWORD dwItemID;
    LONG cx;
    LONG cy;
    ULONG uGeneration;
};

struct THUMBNAIL_WORKER
{
    LONG cRef;               /* the object and, once started, the worker thread */
    CRITICAL_SECTION cs;
    HANDLE hWake;
    HMODULE hModule;         /* shell32, held by the worker thread */
    BOOL bThread;            /* worker thread started */
    HWND hwnd;
    UINT uMsg;
    THUMBNAIL_REQUEST *pHead;
    THUMBNAIL_REQUEST *pTail;
    ULONG uGeneration;       /* bumped by Init(); results of an older generation are dropped */
    BOOL bQuit;
    BOOL bActive;            /* the worker is extracting dwActiveItemID */
    DWORD dwActiveItemID;
    BOOL bActiveStale;       /* a newer request for the active item arrived */
};

static void FreeRequest(THUMBNAIL_REQUEST *pReq)
{
    ILFree(pReq->pidl);
    HeapFree(GetProcessHeap(), 0, pReq);
}

/* cs held */
static void DropQueue(THUMBNAIL_WORKER *pW)
{
    while (pW->pHead)
    {
        THUMBNAIL_REQUEST *pReq = pW->pHead;
        pW->pHead = pReq->pNext;
        FreeRequest(pReq);
    }
    pW->pTail = NULL;
    if (pW->bActive)
        pW->bActiveStale = TRUE;
}

/* cs held */
static void RemoveQueued(THUMBNAIL_WORKER *pW, DWORD dwItemID)
{
    THUMBNAIL_REQUEST **ppLink = &pW->pHead;
    THUMBNAIL_REQUEST *pPrev = NULL;

    while (*ppLink)
    {
        THUMBNAIL_REQUEST *pReq = *ppLink;
        if (pReq->dwItemID == dwItemID)
        {
            *ppLink = pReq->pNext;
            if (pW->pTail == pReq)
                pW->pTail = pPrev;
            FreeRequest(pReq);
            continue;
        }
        pPrev = pReq;
        ppLink = &pReq->pNext;
    }
}

static void WorkerRelease(THUMBNAIL_WORKER *pW)
{
    if (InterlockedDecrement(&pW->cRef) != 0)
        return;
    DropQueue(pW);
    DeleteCriticalSection(&pW->cs);
    CloseHandle(pW->hWake);
    HeapFree(GetProcessHeap(), 0, pW);
}

/* Worker side */
static THUMBNAIL_REQUEST *Take(THUMBNAIL_WORKER *pW)
{
    THUMBNAIL_REQUEST *pReq = NULL;

    EnterCriticalSection(&pW->cs);
    if (!pW->bQuit && pW->pHead)
    {
        pReq = pW->pHead;
        pW->pHead = pReq->pNext;
        if (!pW->pHead)
            pW->pTail = NULL;
        pReq->pNext = NULL;
        pW->bActive = TRUE;
        pW->dwActiveItemID = pReq->dwItemID;
        pW->bActiveStale = FALSE;
    }
    else
    {
        pW->bActive = FALSE;
    }
    LeaveCriticalSection(&pW->cs);
    return pReq;
}

/*
 * The contract promises a 32bpp top-down DIB section with premultiplied alpha
 * (opaque pixels 0xFF). The cache and the in-tree providers deliver that, but
 * a third-party provider may hand back anything: convert the rest, and treat
 * a 32bpp bitmap whose alpha bytes are all zero as opaque.
 */
static HBITMAP Normalize(HBITMAP hbm)
{
    DIBSECTION ds;
    BITMAP bm;
    BITMAPINFO bmi;
    HBITMAP hbmNew;
    RGBQUAD *pBits = NULL;
    HDC hdcSrc, hdcDst;
    HGDIOBJ hOldSrc, hOldDst;
    UINT i, cPixels;
    BOOL bOk;

    if (GetObjectW(hbm, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBitsPixel == 32 &&
        ds.dsBmih.biHeight < 0 && ds.dsBmih.biCompression == BI_RGB && ds.dsBm.bmBits)
    {
        pBits = (RGBQUAD *)ds.dsBm.bmBits;
        cPixels = ds.dsBm.bmWidth * -ds.dsBmih.biHeight;
        GdiFlush();
        for (i = 0; i < cPixels; i++)
            if (pBits[i].rgbReserved)
                return hbm;
        for (i = 0; i < cPixels; i++)
            pBits[i].rgbReserved = 0xFF;
        return hbm;
    }

    if (!GetObjectW(hbm, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0)
        return hbm;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = -abs(bm.bmHeight);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    hbmNew = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void **)&pBits, NULL, 0);
    if (!hbmNew || !pBits)
    {
        if (hbmNew)
            DeleteObject(hbmNew);
        return hbm;
    }
    hdcSrc = CreateCompatibleDC(NULL);
    hdcDst = CreateCompatibleDC(NULL);
    bOk = hdcSrc && hdcDst;
    if (bOk)
    {
        hOldSrc = SelectObject(hdcSrc, hbm);
        hOldDst = SelectObject(hdcDst, hbmNew);
        bOk = BitBlt(hdcDst, 0, 0, bm.bmWidth, abs(bm.bmHeight), hdcSrc, 0, 0, SRCCOPY);
        SelectObject(hdcDst, hOldDst);
        SelectObject(hdcSrc, hOldSrc);
        GdiFlush();
    }
    if (hdcSrc)
        DeleteDC(hdcSrc);
    if (hdcDst)
        DeleteDC(hdcDst);
    if (!bOk)
    {
        DeleteObject(hbmNew);
        return hbm;
    }
    /* A 32bpp source keeps its alpha through the copy; a source without alpha
     * (or with none set) is opaque */
    cPixels = bm.bmWidth * abs(bm.bmHeight);
    for (i = 0; i < cPixels; i++)
        if (pBits[i].rgbReserved)
            break;
    if (i == cPixels)
        for (i = 0; i < cPixels; i++)
            pBits[i].rgbReserved = 0xFF;
    DeleteObject(hbm);
    return hbmNew;
}

static HBITMAP Extract(const THUMBNAIL_REQUEST *pReq)
{
    CComPtr<IShellItemImageFactory> pFactory;
    HBITMAP hbm = NULL;
    SIZE size;
    HRESULT hr;

    size.cx = pReq->cx;
    size.cy = pReq->cy;
    hr = SHCreateItemFromIDList(pReq->pidl, IID_PPV_ARG(IShellItemImageFactory, &pFactory));
    if (SUCCEEDED(hr))
        hr = pFactory->GetImage(size, SIIGBF_THUMBNAILONLY, &hbm);
    if (FAILED(hr))
    {
        TRACE("No thumbnail for item %lu: 0x%08lx\n", pReq->dwItemID, hr);
        if (hbm)
            DeleteObject(hbm);
        return NULL;
    }
    return Normalize(hbm);
}

static void Deliver(THUMBNAIL_WORKER *pW, const THUMBNAIL_REQUEST *pReq, HBITMAP hbm)
{
    BOOL bPosted = FALSE;

    EnterCriticalSection(&pW->cs);
    if (hbm && !pW->bQuit && pW->hwnd && !pW->bActiveStale && pReq->uGeneration == pW->uGeneration)
        bPosted = PostMessageW(pW->hwnd, pW->uMsg, (WPARAM)hbm, (LPARAM)pReq->dwItemID);
    pW->bActive = FALSE;
    LeaveCriticalSection(&pW->cs);
    /* Posted: the window owns the bitmap. Not posted (cancelled, stale, no window,
     * or PostMessage failed because the queue is full or the window is gone): ours to delete. */
    if (!bPosted && hbm)
        DeleteObject(hbm);
}

static DWORD WINAPI ThumbnailThreadProc(LPVOID pParam)
{
    THUMBNAIL_WORKER *pW = static_cast<THUMBNAIL_WORKER *>(pParam);
    HMODULE hModule = pW->hModule;
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    for (;;)
    {
        THUMBNAIL_REQUEST *pReq;
        BOOL bQuit;

        WaitForSingleObject(pW->hWake, INFINITE);
        while ((pReq = Take(pW)) != NULL)
        {
            HBITMAP hbm = Extract(pReq);
            Deliver(pW, pReq, hbm);
            FreeRequest(pReq);
        }
        EnterCriticalSection(&pW->cs);
        bQuit = pW->bQuit;
        LeaveCriticalSection(&pW->cs);
        if (bQuit)
            break;
    }
    if (SUCCEEDED(hrInit))
        CoUninitialize();
    WorkerRelease(pW);
    FreeLibraryAndExitThread(hModule, 0);
    return 0;
}

class CThumbnail :
    public CComCoClass<CThumbnail, &CLSID_Thumbnail>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IThumbnail3,
    public CObjectWithSiteBase
{
    THUMBNAIL_WORKER *m_pWorker;

public:
    CThumbnail() : m_pWorker(NULL) {}

    HRESULT FinalConstruct()
    {
        THUMBNAIL_WORKER *pW;

        pW = (THUMBNAIL_WORKER *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pW));
        if (!pW)
            return E_OUTOFMEMORY;
        pW->hWake = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!pW->hWake)
        {
            DWORD dwError = GetLastError();
            HeapFree(GetProcessHeap(), 0, pW);
            return HRESULT_FROM_WIN32(dwError);
        }
        InitializeCriticalSection(&pW->cs);
        pW->cRef = 1;
        m_pWorker = pW;
        return S_OK;
    }

    void FinalRelease()
    {
        THUMBNAIL_WORKER *pW = m_pWorker;

        if (!pW)
            return;
        m_pWorker = NULL;
        EnterCriticalSection(&pW->cs);
        pW->bQuit = TRUE;
        pW->hwnd = NULL;
        DropQueue(pW);
        LeaveCriticalSection(&pW->cs);
        /* The worker finishes its current extraction on its own time and drops the result. */
        SetEvent(pW->hWake);
        WorkerRelease(pW);
    }

    STDMETHOD(Init)(HWND hwnd, UINT uMsg) override
    {
        THUMBNAIL_WORKER *pW = m_pWorker;

        EnterCriticalSection(&pW->cs);
        pW->hwnd = hwnd;
        pW->uMsg = uMsg;
        pW->uGeneration++;
        DropQueue(pW);
        LeaveCriticalSection(&pW->cs);
        return S_OK;
    }

    STDMETHOD(GetBitmap)(LPCWSTR pszFile, DWORD dwItemID, LONG lWidth, LONG lHeight) override
    {
        CComHeapPtr<ITEMIDLIST> pidl;
        HRESULT hr;

        if (!pszFile)
            return E_INVALIDARG;
        if (GetFileAttributesW(pszFile) == INVALID_FILE_ATTRIBUTES)
            return HRESULT_FROM_WIN32(GetLastError());
        hr = SHParseDisplayName(pszFile, NULL, &pidl, 0, NULL);
        if (FAILED(hr))
            return hr;
        return GetBitmapFromIDList(pidl, dwItemID, lWidth, lHeight);
    }

    STDMETHOD(GetBitmapFromIDList)(PCIDLIST_ABSOLUTE pidl, DWORD dwItemID, LONG lWidth, LONG lHeight) override
    {
        if (!pidl)
            return E_INVALIDARG;
        if (lWidth <= 0 || lHeight <= 0)
            return E_INVALIDARG;
        return Queue(pidl, dwItemID, lWidth, lHeight);
    }

    STDMETHOD(GetBitmapFromShellItem)(IShellItem *psi, DWORD dwItemID, LONG lWidth, LONG lHeight) override
    {
        CComHeapPtr<ITEMIDLIST> pidl;
        HRESULT hr;

        if (!psi)
            return E_INVALIDARG;
        hr = SHGetIDListFromObject(psi, &pidl);
        if (FAILED(hr))
            return hr;
        return GetBitmapFromIDList(pidl, dwItemID, lWidth, lHeight);
    }

    DECLARE_REGISTRY_RESOURCEID(IDR_THUMBNAIL)
    DECLARE_NOT_AGGREGATABLE(CThumbnail)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CThumbnail)
        COM_INTERFACE_ENTRY_IID(IID_IThumbnail, IThumbnail3)
        COM_INTERFACE_ENTRY_IID(IID_IThumbnail2, IThumbnail3)
        COM_INTERFACE_ENTRY_IID(IID_IThumbnail3, IThumbnail3)
        COM_INTERFACE_ENTRY_IID(IID_IObjectWithSite, IObjectWithSite)
    END_COM_MAP()

private:
    /* cs held */
    HRESULT StartWorker(THUMBNAIL_WORKER *pW)
    {
        HANDLE hThread;

        if (pW->bThread)
            return S_OK;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)ThumbnailThreadProc, &pW->hModule))
            return HRESULT_FROM_WIN32(GetLastError());
        InterlockedIncrement(&pW->cRef);
        hThread = CreateThread(NULL, 0, ThumbnailThreadProc, pW, 0, NULL);
        if (!hThread)
        {
            DWORD dwError = GetLastError();
            InterlockedDecrement(&pW->cRef);
            FreeLibrary(pW->hModule);
            pW->hModule = NULL;
            return HRESULT_FROM_WIN32(dwError);
        }
        CloseHandle(hThread);
        pW->bThread = TRUE;
        return S_OK;
    }

    HRESULT Queue(PCIDLIST_ABSOLUTE pidl, DWORD dwItemID, LONG cx, LONG cy)
    {
        THUMBNAIL_WORKER *pW = m_pWorker;
        THUMBNAIL_REQUEST *pReq;
        HRESULT hr;

        pReq = (THUMBNAIL_REQUEST *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pReq));
        if (!pReq)
            return E_OUTOFMEMORY;
        pReq->pidl = ILClone(pidl);
        if (!pReq->pidl)
        {
            HeapFree(GetProcessHeap(), 0, pReq);
            return E_OUTOFMEMORY;
        }
        pReq->dwItemID = dwItemID;
        pReq->cx = cx;
        pReq->cy = cy;

        EnterCriticalSection(&pW->cs);
        if (!pW->hwnd || pW->bQuit)
        {
            /* Nowhere to deliver: Init() was not called or the sink was dropped. */
            LeaveCriticalSection(&pW->cs);
            FreeRequest(pReq);
            return E_FAIL;
        }
        pReq->uGeneration = pW->uGeneration;
        RemoveQueued(pW, dwItemID);
        if (pW->bActive && pW->dwActiveItemID == dwItemID)
            pW->bActiveStale = TRUE;
        if (pW->pTail)
            pW->pTail->pNext = pReq;
        else
            pW->pHead = pReq;
        pW->pTail = pReq;
        hr = StartWorker(pW);
        if (FAILED(hr))
        {
            RemoveQueued(pW, dwItemID);
            LeaveCriticalSection(&pW->cs);
            return hr;
        }
        LeaveCriticalSection(&pW->cs);
        SetEvent(pW->hWake);
        return S_OK;
    }
};

OBJECT_ENTRY_AUTO(CLSID_Thumbnail, CThumbnail)
