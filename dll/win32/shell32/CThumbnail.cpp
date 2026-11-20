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
 * comes from IShellItemImageFactory::GetImage,
 * so the thumbnail cache and every registered IThumbnailProvider are shared
 * with the other consumers. A request whose dwItemID is still queued replaces
 * the queued one; a request whose dwItemID is being extracted makes that
 * result stale, so only the newest request per item is delivered. Init(NULL,
 * 0), a new Init() and the final Release() drop all pending work and nothing
 * is posted for it afterwards.
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

class CThumbnail :
    public CComCoClass<CThumbnail, &CLSID_Thumbnail>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IThumbnail3,
    public CObjectWithSiteBase
{
    HWND m_hwnd;
    UINT m_uMsg;
    CRITICAL_SECTION m_cs;
    THUMBNAIL_REQUEST *m_pHead;
    THUMBNAIL_REQUEST *m_pTail;
    HANDLE m_hThread;
    HANDLE m_hWake;
    ULONG m_uGeneration;     /* bumped by Init(); results of an older generation are dropped */
    BOOL m_bQuit;
    BOOL m_bActive;          /* the worker is extracting m_dwActiveItemID */
    DWORD m_dwActiveItemID;
    BOOL m_bActiveStale;     /* a newer request for the active item arrived */

public:
    CThumbnail() :
        m_hwnd(NULL), m_uMsg(0), m_pHead(NULL), m_pTail(NULL), m_hThread(NULL), m_hWake(NULL),
        m_uGeneration(0), m_bQuit(FALSE), m_bActive(FALSE), m_dwActiveItemID(0), m_bActiveStale(FALSE)
    {
        InitializeCriticalSection(&m_cs);
    }

    HRESULT FinalConstruct()
    {
        m_hWake = CreateEventW(NULL, FALSE, FALSE, NULL);
        return m_hWake ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    void FinalRelease()
    {
        EnterCriticalSection(&m_cs);
        m_bQuit = TRUE;
        m_hwnd = NULL;
        DropQueue();
        LeaveCriticalSection(&m_cs);
        if (m_hThread)
        {
            /* The worker holds no reference; it only finishes the extraction in flight. */
            SetEvent(m_hWake);
            WaitForSingleObject(m_hThread, INFINITE);
            CloseHandle(m_hThread);
            m_hThread = NULL;
        }
        if (m_hWake)
        {
            CloseHandle(m_hWake);
            m_hWake = NULL;
        }
        DeleteCriticalSection(&m_cs);
    }

    STDMETHOD(Init)(HWND hwnd, UINT uMsg) override
    {
        EnterCriticalSection(&m_cs);
        m_hwnd = hwnd;
        m_uMsg = uMsg;
        m_uGeneration++;
        DropQueue();
        LeaveCriticalSection(&m_cs);
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
    static void FreeRequest(THUMBNAIL_REQUEST *pReq)
    {
        ILFree(pReq->pidl);
        HeapFree(GetProcessHeap(), 0, pReq);
    }

    /* m_cs held */
    void DropQueue()
    {
        while (m_pHead)
        {
            THUMBNAIL_REQUEST *pReq = m_pHead;
            m_pHead = pReq->pNext;
            FreeRequest(pReq);
        }
        m_pTail = NULL;
        if (m_bActive)
            m_bActiveStale = TRUE;
    }

    /* m_cs held */
    void RemoveQueued(DWORD dwItemID)
    {
        THUMBNAIL_REQUEST **ppLink = &m_pHead;
        THUMBNAIL_REQUEST *pPrev = NULL;

        while (*ppLink)
        {
            THUMBNAIL_REQUEST *pReq = *ppLink;
            if (pReq->dwItemID == dwItemID)
            {
                *ppLink = pReq->pNext;
                if (m_pTail == pReq)
                    m_pTail = pPrev;
                FreeRequest(pReq);
                continue;
            }
            pPrev = pReq;
            ppLink = &pReq->pNext;
        }
    }

    /* m_cs held */
    HRESULT StartWorker()
    {
        if (m_hThread)
            return S_OK;
        m_hThread = CreateThread(NULL, 0, ThreadProc, this, 0, NULL);
        if (!m_hThread)
            return HRESULT_FROM_WIN32(GetLastError());
        return S_OK;
    }

    HRESULT Queue(PCIDLIST_ABSOLUTE pidl, DWORD dwItemID, LONG cx, LONG cy)
    {
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

        EnterCriticalSection(&m_cs);
        if (!m_hwnd || m_bQuit)
        {
            /* Nowhere to deliver: Init() was not called or the sink was dropped. */
            LeaveCriticalSection(&m_cs);
            FreeRequest(pReq);
            return E_FAIL;
        }
        pReq->uGeneration = m_uGeneration;
        RemoveQueued(dwItemID);
        if (m_bActive && m_dwActiveItemID == dwItemID)
            m_bActiveStale = TRUE;
        if (m_pTail)
            m_pTail->pNext = pReq;
        else
            m_pHead = pReq;
        m_pTail = pReq;
        hr = StartWorker();
        if (FAILED(hr))
        {
            RemoveQueued(dwItemID);
            LeaveCriticalSection(&m_cs);
            return hr;
        }
        LeaveCriticalSection(&m_cs);
        SetEvent(m_hWake);
        return S_OK;
    }

    /* Worker side */
    THUMBNAIL_REQUEST *Take()
    {
        THUMBNAIL_REQUEST *pReq = NULL;

        EnterCriticalSection(&m_cs);
        if (!m_bQuit && m_pHead)
        {
            pReq = m_pHead;
            m_pHead = pReq->pNext;
            if (!m_pHead)
                m_pTail = NULL;
            pReq->pNext = NULL;
            m_bActive = TRUE;
            m_dwActiveItemID = pReq->dwItemID;
            m_bActiveStale = FALSE;
        }
        else
        {
            m_bActive = FALSE;
        }
        LeaveCriticalSection(&m_cs);
        return pReq;
    }

    HBITMAP Extract(const THUMBNAIL_REQUEST *pReq)
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
        return hbm;
    }

    void Deliver(const THUMBNAIL_REQUEST *pReq, HBITMAP hbm)
    {
        BOOL bPosted = FALSE;

        EnterCriticalSection(&m_cs);
        if (hbm && !m_bQuit && m_hwnd && !m_bActiveStale && pReq->uGeneration == m_uGeneration)
            bPosted = PostMessageW(m_hwnd, m_uMsg, (WPARAM)hbm, (LPARAM)pReq->dwItemID);
        m_bActive = FALSE;
        LeaveCriticalSection(&m_cs);
        /* Posted: the window owns the bitmap. Not posted (cancelled, stale, no window,
         * or PostMessage failed because the queue is full or the window is gone): ours to delete. */
        if (!bPosted && hbm)
            DeleteObject(hbm);
    }

    void Run()
    {
        for (;;)
        {
            THUMBNAIL_REQUEST *pReq;
            BOOL bQuit;

            WaitForSingleObject(m_hWake, INFINITE);
            while ((pReq = Take()) != NULL)
            {
                HBITMAP hbm = Extract(pReq);
                Deliver(pReq, hbm);
                FreeRequest(pReq);
            }
            EnterCriticalSection(&m_cs);
            bQuit = m_bQuit;
            LeaveCriticalSection(&m_cs);
            if (bQuit)
                break;
        }
    }

    static DWORD WINAPI ThreadProc(LPVOID pParam)
    {
        CThumbnail *pThis = static_cast<CThumbnail *>(pParam);
        HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

        pThis->Run();
        if (SUCCEEDED(hrInit))
            CoUninitialize();
        return 0;
    }
};

OBJECT_ENTRY_AUTO(CLSID_Thumbnail, CThumbnail)
