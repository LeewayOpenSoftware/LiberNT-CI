/*
 * PROJECT:     ReactOS Font Shell Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tasks of the Fonts folder (Font settings, ClearType, Find a character) and
 *              the hand-out of the font thumbnail provider
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include "precomp.h"
#include <propsys.h>
#include <thumbcache.h>


// {E2537D60-39C7-4E40-9560-B3386D4BB207}: the font thumbnail provider class
static const GUID kFontThumbnailClass =
    { 0xe2537d60, 0x39c7, 0x4e40, { 0x95, 0x60, 0xb3, 0x38, 0x6d, 0x4b, 0xb2, 0x07 } };

struct FontTask
{
    UINT idTitle;
    PCWSTR pszFile;     // program or URI handed to ShellExecute
    PCWSTR pszExe;      // file that must exist under system32 for the task to be offered (or NULL)
    GUID guid;
};

static const FontTask g_Tasks[] =
{
    { IDS_TASK_SETTINGS, L"ms-settings:fonts", NULL,
      { 0x3c1a7e50, 0x5b2d, 0x4a0e, { 0x8f, 0x13, 0x2d, 0x6e, 0x91, 0xb4, 0x07, 0xa1 } } },
    { IDS_TASK_FONTINFO, L"https://learn.microsoft.com/typography/", NULL,
      { 0x3c1a7e53, 0x5b2d, 0x4a0e, { 0x8f, 0x13, 0x2d, 0x6e, 0x91, 0xb4, 0x07, 0xa1 } } },
    { IDS_TASK_CLEARTYPE, L"%SystemRoot%\\System32\\cttune.exe", L"cttune.exe",
      { 0x3c1a7e51, 0x5b2d, 0x4a0e, { 0x8f, 0x13, 0x2d, 0x6e, 0x91, 0xb4, 0x07, 0xa1 } } },
    { IDS_TASK_CHARMAP, L"%SystemRoot%\\System32\\charmap.exe", L"charmap.exe",
      { 0x3c1a7e52, 0x5b2d, 0x4a0e, { 0x8f, 0x13, 0x2d, 0x6e, 0x91, 0xb4, 0x07, 0xa1 } } },
    { IDS_TASK_FONTSIZE, L"ms-settings:easeofaccess-display", NULL,
      { 0x3c1a7e54, 0x5b2d, 0x4a0e, { 0x8f, 0x13, 0x2d, 0x6e, 0x91, 0xb4, 0x07, 0xa1 } } },
};

UINT FontTask_Count()
{
    return _countof(g_Tasks);
}

CStringW FontTask_Title(UINT iTask)
{
    return CStringW(MAKEINTRESOURCEW(g_Tasks[iTask].idTitle));
}

bool FontTask_Available(UINT iTask)
{
    if (!g_Tasks[iTask].pszExe)
        return true;
    WCHAR path[MAX_PATH];
    if (!GetSystemDirectoryW(path, _countof(path)))
        return false;
    PathAppendW(path, g_Tasks[iTask].pszExe);
    return PathFileExistsW(path);
}

HRESULT FontTask_Run(HWND hwnd, UINT iTask)
{
    if (iTask >= _countof(g_Tasks))
        return E_INVALIDARG;
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_DOENVSUBST;
    sei.hwnd = hwnd;
    sei.lpFile = g_Tasks[iTask].pszFile;
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

class CFontCommand :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IExplorerCommand
{
    UINT m_iTask = 0;

public:
    HRESULT Initialize(UINT iTask)
    {
        m_iTask = iTask;
        return S_OK;
    }

    STDMETHODIMP GetTitle(IShellItemArray *psiItemArray, PWSTR *ppszName) override
    {
        return SHStrDup(FontTask_Title(m_iTask), ppszName);
    }
    STDMETHODIMP GetIcon(IShellItemArray *psiItemArray, PWSTR *ppszIcon) override
    {
        *ppszIcon = NULL;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetToolTip(IShellItemArray *psiItemArray, PWSTR *ppszInfotip) override
    {
        *ppszInfotip = NULL;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetCanonicalName(GUID *pguidCommandName) override
    {
        *pguidCommandName = g_Tasks[m_iTask].guid;
        return S_OK;
    }
    STDMETHODIMP GetState(IShellItemArray *psiItemArray, BOOL fOkToBeSlow, EXPCMDSTATE *pCmdState) override
    {
        *pCmdState = FontTask_Available(m_iTask) ? ECS_ENABLED : ECS_HIDDEN;
        return S_OK;
    }
    STDMETHODIMP Invoke(IShellItemArray *psiItemArray, IBindCtx *pbc) override
    {
        return FontTask_Run(NULL, m_iTask);
    }
    STDMETHODIMP GetFlags(EXPCMDFLAGS *pFlags) override
    {
        *pFlags = ECF_DEFAULT;
        return S_OK;
    }
    STDMETHODIMP EnumSubCommands(IEnumExplorerCommand **ppEnum) override
    {
        *ppEnum = NULL;
        return E_NOTIMPL;
    }

    DECLARE_NOT_AGGREGATABLE(CFontCommand)
    DECLARE_PROTECT_FINAL_CONSTRUCT()
    BEGIN_COM_MAP(CFontCommand)
        COM_INTERFACE_ENTRY_IID(IID_IExplorerCommand, IExplorerCommand)
    END_COM_MAP()
};

class CFontCommandEnum :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IEnumExplorerCommand
{
    UINT m_Index = 0;

public:
    STDMETHODIMP Next(ULONG celt, IExplorerCommand **pUICommand, ULONG *pceltFetched) override
    {
        if (!pUICommand)
            return E_POINTER;
        ULONG fetched = 0;
        while (fetched < celt && m_Index < FontTask_Count())
        {
            HRESULT hr = ShellObjectCreatorInit<CFontCommand>(m_Index++, IID_PPV_ARG(IExplorerCommand, &pUICommand[fetched]));
            if (FAILED(hr))
                return hr;
            ++fetched;
        }
        if (pceltFetched)
            *pceltFetched = fetched;
        return fetched == celt ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG celt) override
    {
        m_Index = min(m_Index + celt, FontTask_Count());
        return m_Index < FontTask_Count() ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override
    {
        m_Index = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumExplorerCommand **ppenum) override
    {
        return E_NOTIMPL;
    }

    DECLARE_NOT_AGGREGATABLE(CFontCommandEnum)
    DECLARE_PROTECT_FINAL_CONSTRUCT()
    BEGIN_COM_MAP(CFontCommandEnum)
        COM_INTERFACE_ENTRY_IID(IID_IEnumExplorerCommand, IEnumExplorerCommand)
    END_COM_MAP()
};

class CFontCommandProvider :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IExplorerCommandProvider
{
public:
    STDMETHODIMP GetCommands(IUnknown *punkSite, REFIID riid, void **ppv) override
    {
        return ShellObjectCreator<CFontCommandEnum>(riid, ppv);
    }
    STDMETHODIMP GetCommand(REFGUID rguidCommandId, REFIID riid, void **ppv) override
    {
        for (UINT i = 0; i < FontTask_Count(); ++i)
        {
            if (IsEqualGUID(rguidCommandId, g_Tasks[i].guid))
                return ShellObjectCreatorInit<CFontCommand>(i, riid, ppv);
        }
        *ppv = NULL;
        return E_INVALIDARG;
    }

    DECLARE_NOT_AGGREGATABLE(CFontCommandProvider)
    DECLARE_PROTECT_FINAL_CONSTRUCT()
    BEGIN_COM_MAP(CFontCommandProvider)
        COM_INTERFACE_ENTRY_IID(IID_IExplorerCommandProvider, IExplorerCommandProvider)
    END_COM_MAP()
};

HRESULT _CFontCommandProvider_Create(REFIID riid, LPVOID* ppvOut)
{
    return ShellObjectCreator<CFontCommandProvider>(riid, ppvOut);
}

// The thumbnail provider lives in its own class (registered for the font file types);
// the folder hands it out for its items.
HRESULT _FontThumbnail_Create(PCWSTR pszPath, REFIID riid, LPVOID* ppvOut)
{
    CComPtr<IInitializeWithFile> init;
    HRESULT hr = CoCreateInstance(kFontThumbnailClass, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IInitializeWithFile, &init));
    if (FAILED(hr))
        return hr;
    hr = init->Initialize(pszPath, STGM_READ);
    if (FAILED(hr))
        return hr;
    return init->QueryInterface(riid, ppvOut);
}
