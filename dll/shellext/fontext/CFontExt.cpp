/*
 * PROJECT:     ReactOS Font Shell Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     CFontExt implementation
 * COPYRIGHT:   Copyright 2019-2021 Mark Jansen <mark.jansen@reactos.org>
 *              Copyright 2019-2026 Katayama Hirofumi MZ (katayama.hirofumi.mz@gmail.com)
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(fontext);

#ifndef SHCIDS_ALLFIELDS
#define SHCIDS_ALLFIELDS 0x80000000L
#endif

struct FolderViewColumns
{
    int iResource;
    DWORD dwDefaultState;
    int cxChar;
    int fmt;
};

enum font_columns
{
    FONTEXT_COL_NAME,
    FONTEXT_COL_STYLE,
    FONTEXT_COL_SHOWHIDE,
    FONTEXT_COL_DESIGNEDFOR,
    FONTEXT_COL_CATEGORY,
    FONTEXT_COL_DESIGNER,
    FONTEXT_COL_EMBED,
    FONTEXT_COL_FILENAME,
    FONTEXT_COL_SIZE,
    FONTEXT_COL_MODIFIED,
    FONTEXT_COL_ATTR,
};

// {6B3A8C1E-2F0D-4E5A-9C47-5D1F0A7E2B94}: the property set of the Fonts folder columns, pid = column
static const GUID FMTID_FontExt =
    { 0x6b3a8c1e, 0x2f0d, 0x4e5a, { 0x9c, 0x47, 0x5d, 0x1f, 0x0a, 0x7e, 0x2b, 0x94 } };

static FolderViewColumns g_ColumnDefs[] =
{
    { IDS_COL_NAME,      SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT,   25, LVCFMT_LEFT },
    { IDS_COL_STYLE,     SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT,   15, LVCFMT_LEFT },
    { IDS_COL_SHOWHIDE,  SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT,   10, LVCFMT_LEFT },
    { IDS_COL_DESIGNEDFOR, SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT, 22, LVCFMT_LEFT },
    { IDS_COL_CATEGORY,  SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT,   10, LVCFMT_LEFT },
    { IDS_COL_DESIGNER,  SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT,   20, LVCFMT_LEFT },
    { IDS_COL_EMBED,     SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT,   18, LVCFMT_LEFT },
    { IDS_COL_FILENAME,  SHCOLSTATE_TYPE_STR,   20, LVCFMT_LEFT },
    { IDS_COL_SIZE,      SHCOLSTATE_TYPE_STR,   10, LVCFMT_RIGHT },
    { IDS_COL_MODIFIED,  SHCOLSTATE_TYPE_DATE,  15, LVCFMT_LEFT },
    { IDS_COL_ATTR,      SHCOLSTATE_TYPE_STR,   12, LVCFMT_RIGHT },
};

static const FontPidlEntry* _AnyFontEntry(PCITEMID_CHILD pidl, bool* pbFamily = nullptr)
{
    const FontPidlEntry* entry = _FontFromIL(pidl);
    if (pbFamily)
        *pbFamily = false;
    if (!entry)
    {
        entry = _FamilyFromIL(pidl);
        if (pbFamily)
            *pbFamily = (entry != nullptr);
    }
    return entry;
}

// Helper functions to translate a guid to a readable name
bool GetInterfaceName(const WCHAR* InterfaceString, WCHAR* buf, size_t size)
{
    WCHAR LocalBuf[100];
    DWORD dwType = 0, dwDataSize = size * sizeof(WCHAR);

    if (!SUCCEEDED(StringCchPrintfW(LocalBuf, _countof(LocalBuf), L"Interface\\%s", InterfaceString)))
        return false;

    return RegGetValueW(HKEY_CLASSES_ROOT, LocalBuf, NULL, RRF_RT_REG_SZ, &dwType, buf, &dwDataSize) == ERROR_SUCCESS;
}

WCHAR* g2s(REFCLSID iid)
{
    static WCHAR buf[2][300];
    static int idx = 0;

    idx ^= 1;

    LPOLESTR tmp;
    HRESULT hr = ProgIDFromCLSID(iid, &tmp);
    if (SUCCEEDED(hr))
    {
        wcscpy(buf[idx], tmp);
        CoTaskMemFree(tmp);
        return buf[idx];
    }
    StringFromGUID2(iid, buf[idx], _countof(buf[idx]));
    if (GetInterfaceName(buf[idx], buf[idx], _countof(buf[idx])))
    {
        return buf[idx];
    }
    StringFromGUID2(iid, buf[idx], _countof(buf[idx]));

    return buf[idx];
}

static HRESULT FONTEXT_GetAttributeString(DWORD dwAttributes, LPWSTR pszOut, UINT cchMax)
{
    PCWSTR AttrLetters = L"RHSAC"; // Read-only, Hidden, System, Archive, Compressed

    UINT ich = 0;
    if ((dwAttributes & FILE_ATTRIBUTE_READONLY) && ich < cchMax)
        pszOut[ich++] = AttrLetters[0];
    if ((dwAttributes & FILE_ATTRIBUTE_HIDDEN) && ich < cchMax)
        pszOut[ich++] = AttrLetters[1];
    if ((dwAttributes & FILE_ATTRIBUTE_SYSTEM) && ich < cchMax)
        pszOut[ich++] = AttrLetters[2];
    if ((dwAttributes & FILE_ATTRIBUTE_ARCHIVE) && ich < cchMax)
        pszOut[ich++] = AttrLetters[3];
    if ((dwAttributes & FILE_ATTRIBUTE_COMPRESSED) && ich < cchMax)
        pszOut[ich++] = AttrLetters[4];
    if (ich < cchMax)
    {
        pszOut[ich] = UNICODE_NULL;
        return S_OK;
    }
    ERR("Buffer too short: %u\n", cchMax);
    return E_FAIL;
}

CFontExt::CFontExt()
{
    InterlockedIncrement(&g_ModuleRefCnt);
}

CFontExt::~CFontExt()
{
    InterlockedDecrement(&g_ModuleRefCnt);
}

void CFontExt::SetViewWindow(HWND hwndView)
{
    m_hwndView = hwndView;
}

HRESULT CFontExt::InitFamily(PCIDLIST_ABSOLUTE rootFolder, PCIDLIST_ABSOLUTE self, PCWSTR familyKey)
{
    m_Folder.Attach(ILClone(rootFolder));
    m_Self.Attach(ILClone(self));
    m_FamilyKey = familyKey;
    return (m_Folder && m_Self) ? S_OK : E_OUTOFMEMORY;
}

CStringW CFontExt::ColumnText(PCUITEMID_CHILD pidl, UINT iColumn)
{
    CStringW text;
    bool bFamily;
    const FontPidlEntry* entry = _AnyFontEntry(pidl, &bFamily);
    if (!entry)
        return text;
    CFontInfo* info = g_FontCache->FindByName(entry->Name());
    if (!info)
        return text;
    CFontMeta& meta = info->Meta();

    switch (iColumn)
    {
    case FONTEXT_COL_NAME:
        return info->DisplayName();
    case FONTEXT_COL_STYLE:
        if (bFamily)
        {
            CAtlArray<FontViewItem> items;
            CStringW key(entry->FileName());
            g_FontCache->BuildView(&key, items);
            text.Format(CStringW(MAKEINTRESOURCEW(IDS_STYLES_FMT)), (UINT)items.GetCount());
            return text;
        }
        return (meta.valid && !meta.Style.IsEmpty()) ? meta.Style : CStringW(L"Regular");
    case FONTEXT_COL_SHOWHIDE:
        return CStringW(MAKEINTRESOURCEW(g_FontCache->IsHidden(info) ? IDS_HIDE : IDS_SHOW));
    case FONTEXT_COL_DESIGNEDFOR:
        return FontMeta_DesignedFor(meta);
    case FONTEXT_COL_CATEGORY:
    {
        static const UINT ids[] = { IDS_CAT_TEXT, IDS_CAT_DISPLAY, IDS_CAT_SYMBOL };
        return CStringW(MAKEINTRESOURCEW(ids[FontMeta_Category(meta)]));
    }
    case FONTEXT_COL_DESIGNER:
        return meta.Designer.IsEmpty() ? meta.Foundry : meta.Designer;
    case FONTEXT_COL_EMBED:
    {
        static const UINT ids[] = { IDS_EMB_INSTALLABLE, IDS_EMB_EDITABLE, IDS_EMB_PREVIEW, IDS_EMB_RESTRICTED };
        return CStringW(MAKEINTRESOURCEW(ids[FontMeta_Embeddability(meta)]));
    }
    }
    return text;
}

HRESULT CFontExt::ToggleHidden(IDataObject* pDataObj)
{
    CDataObjectHIDA cida(pDataObj);
    if (!cida || cida->cidl <= 0)
        return E_FAIL;

    bool bHide = true, bFirst = true;
    for (UINT iItem = 0; iItem < cida->cidl; ++iItem)
    {
        const FontPidlEntry* entry = _FontFromIL(HIDA_GetPIDLItem(cida, iItem));
        if (!entry)
            continue;
        CFontInfo* info = g_FontCache->FindByName(entry->Name());
        if (bFirst)
            bHide = !g_FontCache->IsHidden(info);
        bFirst = false;
        g_FontCache->SetHidden(info, bHide);
    }

    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSH, m_Folder, NULL);
    return S_OK;
}

// Opens the family stack of the first selected style in the current browser window
HRESULT CFontExt::OpenFamily(IDataObject* pDataObj)
{
    CDataObjectHIDA cida(pDataObj);
    if (!cida || cida->cidl <= 0)
        return E_FAIL;
    const FontPidlEntry* entry = _FontFromIL(HIDA_GetPIDLItem(cida, 0));
    CFontInfo* info = entry ? g_FontCache->FindByName(entry->Name()) : nullptr;
    if (!info)
        return E_FAIL;

    // The stack pidl carries the registry name of its representative: ask the view for it
    CStringW key = info->Family(), repName = info->Name();
    CAtlArray<FontViewItem> items;
    g_FontCache->BuildView(nullptr, items);
    for (size_t i = 0; i < items.GetCount(); ++i)
    {
        if (items[i].bFamily && items[i].Key.CompareNoCase(key) == 0)
            repName = items[i].RegName;
    }

    CComHeapPtr<ITEMIDLIST> family;
    family.Attach(_ILCreateFamily(repName, key));
    CComHeapPtr<ITEMIDLIST> abs;
    abs.Attach(ILCombine(m_Folder, family));
    if (!abs)
        return E_OUTOFMEMORY;

    IShellBrowser* psb = (IShellBrowser*)SendMessageW(GetParent(m_hwndView), WM_USER + 7 /* WM_GETISHELLBROWSER */, 0, 0);
    if (psb && SUCCEEDED(psb->BrowseObject(abs, SBSP_SAMEBROWSER | SBSP_ABSOLUTE)))
        return S_OK;

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_IDLIST;
    sei.hwnd = m_hwndView;
    sei.lpIDList = abs;
    sei.lpVerb = L"open";
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) ? S_OK : E_FAIL;
}

// A family stack in a selection stands for all of its styles
HRESULT CFontExt::ExpandedDataObject(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, REFIID riid, LPVOID* ppvOut)
{
    CAtlArray<PITEMID_CHILD> pidls;
    for (UINT i = 0; i < cidl; ++i)
    {
        const FontPidlEntry* family = _FamilyFromIL(apidl[i]);
        if (!family)
        {
            PITEMID_CHILD clone = ILClone(apidl[i]);
            if (clone)
                pidls.Add(clone);
            continue;
        }

        CAtlArray<FontViewItem> items;
        CStringW key(family->FileName());
        g_FontCache->BuildView(&key, items);
        for (size_t k = 0; k < items.GetCount(); ++k)
        {
            CFontInfo* info = g_FontCache->FindByName(items[k].RegName);
            PITEMID_CHILD pidl = info ? _ILCreate(info->Name(), info->File()) : NULL;
            if (pidl)
                pidls.Add(pidl);
        }
    }

    HRESULT hr = _CDataObject_CreateInstance(m_Folder, (UINT)pidls.GetCount(), (PCUITEMID_CHILD_ARRAY)pidls.GetData(),
                                             riid, ppvOut);
    for (size_t i = 0; i < pidls.GetCount(); ++i)
        ILFree(pidls[i]);
    return hr;
}

HRESULT CFontExt::DeleteItems(IDataObject* pDataObj)
{
    HRESULT hr = DoDeleteFontFiles(m_hwndView, pDataObj);
    FAILED_UNEXPECTEDLY(hr);
    return S_OK; // Override default action
}

HRESULT CFontExt::PreviewItems(IDataObject* pDataObj)
{
    HRESULT hr = DoPreviewFontFiles(m_hwndView, pDataObj);
    FAILED_UNEXPECTEDLY(hr);
    return S_OK; // Override default action
}

// *** IShellFolder2 methods ***
STDMETHODIMP CFontExt::GetDefaultSearchGUID(GUID *lpguid)
{
    ERR("%s() UNIMPLEMENTED\n", __FUNCTION__);
    return E_NOTIMPL;
}

STDMETHODIMP CFontExt::EnumSearches(IEnumExtraSearch **ppenum)
{
    ERR("%s() UNIMPLEMENTED\n", __FUNCTION__);
    return E_NOTIMPL;
}

STDMETHODIMP CFontExt::GetDefaultColumn(DWORD dwReserved, ULONG *pSort, ULONG *pDisplay)
{
    if (!pSort || !pDisplay)
        return E_POINTER;

    *pSort = FONTEXT_COL_NAME;
    *pDisplay = FONTEXT_COL_NAME;
    return S_OK;
}

STDMETHODIMP CFontExt::GetDefaultColumnState(UINT iColumn, SHCOLSTATEF *pcsFlags)
{
    if (!pcsFlags || iColumn >= _countof(g_ColumnDefs))
        return E_INVALIDARG;

    *pcsFlags = g_ColumnDefs[iColumn].dwDefaultState;
    return S_OK;
}

STDMETHODIMP CFontExt::GetDetailsEx(PCUITEMID_CHILD pidl, const SHCOLUMNID *pscid, VARIANT *pv)
{
    if (!pidl || !pscid || !pv)
        return E_INVALIDARG;

    bool bFamily;
    const FontPidlEntry* fontEntry = _AnyFontEntry(pidl, &bFamily);
    if (!fontEntry)
        return E_INVALIDARG;

    if (IsEqualGUID(pscid->fmtid, FMTID_FontExt))
    {
        V_BSTR(pv) = SysAllocString(ColumnText(pidl, pscid->pid));
        if (!V_BSTR(pv))
            return E_OUTOFMEMORY;
        V_VT(pv) = VT_BSTR;
        return S_OK;
    }

    if (IsEqualGUID(pscid->fmtid, FMTID_Storage) && pscid->pid == PID_STG_NAME)
    {
        STRRET str;
        HRESULT hr = GetDisplayNameOf(pidl, SHGDN_NORMAL | SHGDN_INFOLDER, &str);
        if (FAILED(hr))
            return hr;
        hr = StrRetToBSTR(&str, pidl, &V_BSTR(pv));
        if (SUCCEEDED(hr))
            V_VT(pv) = VT_BSTR;
        return hr;
    }

    if (bFamily)
        return E_INVALIDARG;

    auto info = g_FontCache->Find(fontEntry);
    if (info == nullptr)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    if (IsEqualGUID(pscid->fmtid, PKEY_FileName.fmtid) && pscid->pid == PKEY_FileName.pid)
    {
        V_BSTR(pv) = SysAllocString(PathFindFileNameW(info->File()));
        if (!V_BSTR(pv))
            return E_OUTOFMEMORY;
        V_VT(pv) = VT_BSTR;
        return S_OK;
    }

    if (IsEqualGUID(pscid->fmtid, FMTID_Storage))
    {
        switch (pscid->pid)
        {
        case PID_STG_SIZE:
            V_VT(pv) = VT_UI8;
            V_UI8(pv) = info->FileSize().QuadPart;
            return S_OK;
        case PID_STG_ATTRIBUTES:
            V_VT(pv) = VT_UI4;
            V_UI4(pv) = info->FileAttributes();
            return S_OK;
        case PID_STG_WRITETIME:
        {
            FILETIME local;
            SYSTEMTIME time;
            if (!FileTimeToLocalFileTime(&info->FileWriteTime(), &local) ||
                !FileTimeToSystemTime(&local, &time) ||
                !SystemTimeToVariantTime(&time, &V_DATE(pv)))
            {
                return E_FAIL;
            }
            V_VT(pv) = VT_DATE;
            return S_OK;
        }
        default:
            break;
        }
    }

    return E_INVALIDARG;
}

STDMETHODIMP CFontExt::GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn, SHELLDETAILS *psd)
{
    if (iColumn >= _countof(g_ColumnDefs))
        return E_FAIL;

    psd->cxChar = g_ColumnDefs[iColumn].cxChar;
    psd->fmt = g_ColumnDefs[iColumn].fmt;

    // No item requested, so return the column name
    if (pidl == NULL)
    {
        return SHSetStrRet(&psd->str, _AtlBaseModule.GetResourceInstance(), g_ColumnDefs[iColumn].iResource);
    }

    // Validate that this pidl is the last one
    PCUIDLIST_RELATIVE curpidl = ILGetNext(pidl);
    if (curpidl->mkid.cb != 0)
    {
        ERR("ERROR, unhandled PIDL!\n");
        return E_FAIL;
    }

    // Name, ReactOS specific?
    if (iColumn == FONTEXT_COL_NAME)
        return GetDisplayNameOf(pidl, 0, &psd->str);

    // The font specific columns come from the font file; a stack shows its representative
    if (iColumn < FONTEXT_COL_FILENAME)
        return SHSetStrRet(&psd->str, ColumnText(pidl, iColumn));
    if (_FamilyFromIL(pidl))
        return SHSetStrRet(&psd->str, L"");

    const FontPidlEntry* fontEntry = _FontFromIL(pidl);
    if (!fontEntry)
    {
        ERR("ERROR, not a font PIDL!\n");
        return E_FAIL;
    }

    // If we got here, we are in details view!
    auto info = g_FontCache->Find(fontEntry);
    if (info == nullptr)
    {
        ERR("Unable to query info about %S\n", fontEntry->Name());
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    int ret;
    SYSTEMTIME time;
    switch (iColumn)
    {
    case FONTEXT_COL_FILENAME:
        return SHSetStrRet(&psd->str, PathFindFileNameW(info->File()));
    case FONTEXT_COL_SIZE:
        psd->str.uType = STRRET_CSTR;
        StrFormatKBSizeA(info->FileSize().QuadPart, psd->str.cStr, MAX_PATH);
        return S_OK;
    case FONTEXT_COL_MODIFIED:
        psd->str.uType = STRRET_CSTR;
        FileTimeToSystemTime(&info->FileWriteTime(), &time);
        ret = GetDateFormatA(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &time, NULL, psd->str.cStr, MAX_PATH);
        if (ret < 1)
        {
            ERR("GetDateFormatA failed\n");
            return E_FAIL;
        }
        psd->str.cStr[ret-1] = ' ';
        GetTimeFormatA(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &time, NULL, &psd->str.cStr[ret], MAX_PATH - ret);
        return S_OK;
    case FONTEXT_COL_ATTR:
        {
            WCHAR szAttr[8];
            HRESULT hr = FONTEXT_GetAttributeString(info->FileAttributes(), szAttr, _countof(szAttr));
            if (FAILED_UNEXPECTEDLY(hr))
                return hr;
            return SHSetStrRet(&psd->str, szAttr);
        }
    default:
        break;
    }

    UNIMPLEMENTED;
    return E_NOTIMPL;
}

STDMETHODIMP CFontExt::MapColumnToSCID(UINT iColumn, SHCOLUMNID *pscid)
{
    if (!pscid)
        return E_POINTER;

    switch (iColumn)
    {
    case FONTEXT_COL_NAME:
        pscid->fmtid = FMTID_Storage;
        pscid->pid = PID_STG_NAME;
        return S_OK;
    case FONTEXT_COL_STYLE:
    case FONTEXT_COL_SHOWHIDE:
    case FONTEXT_COL_DESIGNEDFOR:
    case FONTEXT_COL_CATEGORY:
    case FONTEXT_COL_DESIGNER:
    case FONTEXT_COL_EMBED:
        pscid->fmtid = FMTID_FontExt;
        pscid->pid = iColumn;
        return S_OK;
    case FONTEXT_COL_FILENAME:
        pscid->fmtid = PKEY_FileName.fmtid;
        pscid->pid = PKEY_FileName.pid;
        return S_OK;
    case FONTEXT_COL_SIZE:
        pscid->fmtid = FMTID_Storage;
        pscid->pid = PID_STG_SIZE;
        return S_OK;
    case FONTEXT_COL_MODIFIED:
        pscid->fmtid = FMTID_Storage;
        pscid->pid = PID_STG_WRITETIME;
        return S_OK;
    case FONTEXT_COL_ATTR:
        pscid->fmtid = FMTID_Storage;
        pscid->pid = PID_STG_ATTRIBUTES;
        return S_OK;
    default:
        break;
    }
    return E_INVALIDARG;
}

// *** IShellFolder2 methods ***
STDMETHODIMP CFontExt::ParseDisplayName(HWND hwndOwner, LPBC pbc, LPOLESTR lpszDisplayName, DWORD *pchEaten, PIDLIST_RELATIVE *ppidl, DWORD *pdwAttributes)
{
    if (!lpszDisplayName || !lpszDisplayName[0] || !ppidl)
        return E_INVALIDARG;

    *ppidl = NULL;
    if (pchEaten)
        *pchEaten = 0;

    if (lpszDisplayName[0] == L':' && lpszDisplayName[1] == L':')
        return E_INVALIDARG;

    if (PathIsRelativeW(lpszDisplayName)) // Not full path?
        return E_INVALIDARG;

    // Load font cache
    if (g_FontCache->Size() == 0)
        g_FontCache->Read();

    for (SIZE_T iFont = 0; iFont < g_FontCache->Size(); ++iFont)
    {
        CStringW fileName = g_FontCache->File(iFont);
        if (fileName.IsEmpty())
            continue;

        CStringW filePath = g_FontCache->GetFontFilePath(fileName);
        if (filePath.CompareNoCase(lpszDisplayName) != 0)
            continue;

        CStringW fontName = g_FontCache->Name(iFont);
        if (fontName.IsEmpty())
            continue;

        // Create a PIDL
        *ppidl = _ILCreate(fontName, fileName);
        if (!*ppidl)
            return E_OUTOFMEMORY;

        if (pchEaten)
            *pchEaten = wcslen(lpszDisplayName);

        if (pdwAttributes)
            *pdwAttributes &= (SFGAO_CANDELETE | SFGAO_HASPROPSHEET | SFGAO_CANCOPY |
                               SFGAO_FILESYSTEM);

        return S_OK;
    }

    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); // Not found
}

STDMETHODIMP CFontExt::EnumObjects(HWND hwndOwner, DWORD dwFlags, LPENUMIDLIST *ppEnumIDList)
{
    return _CEnumFonts_CreateInstance(this, dwFlags, IID_PPV_ARG(IEnumIDList, ppEnumIDList));
}

STDMETHODIMP CFontExt::BindToObject(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut)
{
    if (!ppvOut)
        return E_POINTER;

    *ppvOut = NULL;
    if (!pidl)
        return E_INVALIDARG;

    const FontPidlEntry* family = _FamilyFromIL(pidl);
    if (family && m_FamilyKey.IsEmpty())
    {
        CComPtr<CFontExt> sub;
        HRESULT hr = ShellObjectCreator(sub);
        if (FAILED(hr))
            return hr;
        CComHeapPtr<ITEMIDLIST> self;
        self.Attach(ILCombine(m_Self ? m_Self : m_Folder, pidl));
        if (!self)
            return E_OUTOFMEMORY;
        hr = sub->InitFamily(m_Folder, self, family->FileName());
        if (FAILED(hr))
            return hr;
        return sub->QueryInterface(riid, ppvOut);
    }

    if (!_FontFromIL(pidl))
        return E_INVALIDARG;

    return E_NOINTERFACE;
}

STDMETHODIMP CFontExt::BindToStorage(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut)
{
    ERR("%s() UNIMPLEMENTED\n", __FUNCTION__);
    return E_NOTIMPL;
}

STDMETHODIMP CFontExt::CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2)
{
    bool bFamily1, bFamily2;
    const FontPidlEntry* fontEntry1 = _AnyFontEntry(pidl1, &bFamily1);
    const FontPidlEntry* fontEntry2 = _AnyFontEntry(pidl2, &bFamily2);
    if (!fontEntry1 || !fontEntry2)
        return E_INVALIDARG;

    int result;
    if (bFamily1 || bFamily2)
    {
        result = StrCmpIW(FontDisplayName(fontEntry1->Name()), FontDisplayName(fontEntry2->Name()));
        if (!result)
            result = (int)bFamily2 - (int)bFamily1;
        return MAKE_COMPARE_HRESULT(result);
    }

    DWORD sortMode = lParam & 0xFFFF0000;
    DWORD column = lParam & 0x0000FFFF;
    if (sortMode == SHCIDS_ALLFIELDS)
    {
        result = StrCmpIW(FontDisplayName(fontEntry1->Name()), FontDisplayName(fontEntry2->Name()));
    }
    else
    {
        auto info1 = g_FontCache->Find(fontEntry1);
        auto info2 = g_FontCache->Find(fontEntry2);

        if (!info1 || !info2)
        {
            ERR("Unable to find font %S or %S in cache!\n", fontEntry1->Name(), fontEntry2->Name());
            return E_INVALIDARG;
        }

        switch (column)
        {
        case 0xffff:
            /* ROS bug? */
        case FONTEXT_COL_NAME:
            result = StrCmpIW(FontDisplayName(fontEntry1->Name()), FontDisplayName(fontEntry2->Name()));
            break;
        case FONTEXT_COL_STYLE:
        case FONTEXT_COL_SHOWHIDE:
        case FONTEXT_COL_DESIGNEDFOR:
        case FONTEXT_COL_CATEGORY:
        case FONTEXT_COL_DESIGNER:
        case FONTEXT_COL_EMBED:
            result = StrCmpIW(ColumnText((PCUITEMID_CHILD)pidl1, column), ColumnText((PCUITEMID_CHILD)pidl2, column));
            break;
        case FONTEXT_COL_FILENAME:
            result = StrCmpIW(PathFindFileNameW(info1->File()), PathFindFileNameW(info2->File()));
            break;
        case FONTEXT_COL_SIZE:
            {
                ULONGLONG size1 = info1->FileSize().QuadPart, size2 = info2->FileSize().QuadPart;
                result = (size1 < size2) ? -1 : ((size1 > size2) ? 1 : 0);
            }
            break;
        case FONTEXT_COL_MODIFIED:
            result = CompareFileTime(&info1->FileWriteTime(), &info2->FileWriteTime());
            break;
        case FONTEXT_COL_ATTR:
            {
                HRESULT hr;
                WCHAR szAttr1[8], szAttr2[8];
                hr = FONTEXT_GetAttributeString(info1->FileAttributes(), szAttr1, _countof(szAttr1));
                if (FAILED_UNEXPECTEDLY(hr))
                    return hr;
                hr = FONTEXT_GetAttributeString(info2->FileAttributes(), szAttr2, _countof(szAttr2));
                if (FAILED_UNEXPECTEDLY(hr))
                    return hr;
                result = _wcsicmp(szAttr1, szAttr2);
            }
            break;
        default:
            ERR("Unimplemented column %u\n", column);
            return E_INVALIDARG;
        }
    }

    return MAKE_COMPARE_HRESULT(result);
}

STDMETHODIMP CFontExt::CreateViewObject(HWND hwndOwner, REFIID riid, LPVOID *ppvOut)
{
    HRESULT hr = E_NOINTERFACE;

    *ppvOut = NULL;

    if (IsEqualIID(riid, IID_IDropTarget))
    {
        *ppvOut = static_cast<IDropTarget *>(this);
        AddRef();
        hr = S_OK;
    }
    else if (IsEqualIID(riid, IID_IContextMenu))
    {
        TRACE("IContextMenu\n");
        return CFontBackgroundMenu_Create(this, hwndOwner, this, (IContextMenu**)ppvOut);
    }
    else if (IsEqualIID(riid, IID_IShellView))
    {
        CComPtr<CFontFolderViewCB> sfviewcb;
        if (SUCCEEDED(hr = ShellObjectCreator(sfviewcb)))
        {
            SFV_CREATE create = { sizeof(create), this, NULL, sfviewcb };
            hr = SHCreateShellFolderView(&create, (IShellView**)ppvOut);
            if (SUCCEEDED(hr))
                sfviewcb->Initialize(this, (IShellView*)*ppvOut, m_Folder);
        }
    }

    return hr;
}

STDMETHODIMP CFontExt::GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, DWORD *rgfInOut)
{
    if (!rgfInOut || !cidl || !apidl)
        return E_INVALIDARG;

    if (cidl <= 0)
    {
        *rgfInOut = 0;
        return S_OK;
    }

    DWORD rgf = (SFGAO_CANDELETE | SFGAO_HASPROPSHEET | SFGAO_CANCOPY | SFGAO_FILESYSTEM | SFGAO_FOLDER |
                 SFGAO_GHOSTED);
    while (cidl > 0 && *apidl)
    {
        bool bFamily;
        const FontPidlEntry* fontEntry = _AnyFontEntry(*apidl, &bFamily);
        if (!fontEntry)
        {
            rgf = 0;
            break;
        }

        if (bFamily)
            rgf &= (SFGAO_CANDELETE | SFGAO_CANCOPY | SFGAO_FOLDER);
        else
            rgf &= (SFGAO_CANDELETE | SFGAO_HASPROPSHEET | SFGAO_CANCOPY | SFGAO_FILESYSTEM | SFGAO_GHOSTED);

        if (!g_FontCache->IsHidden(g_FontCache->FindByName(fontEntry->Name())))
            rgf &= ~SFGAO_GHOSTED;

        apidl++;
        cidl--;
    }

    *rgfInOut &= rgf;
    *rgfInOut &= ~SFGAO_VALIDATE; // Make sure SFGAO_VALIDATE is cleared, some apps depend on that

    return S_OK;
}

HRESULT CALLBACK CFontExt::MenuCallback(
    IShellFolder *psf, HWND hwnd, IDataObject *pdtobj,
    UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    CFontExt* pThis = static_cast<CFontExt*>(psf);
    if (!pThis)
        return E_FAIL;

    switch (uMsg)
    {
        case DFM_MERGECONTEXTMENU:
        {
            QCMINFO* pqcminfo = (QCMINFO*)lParam;
            // Insert [Preview] menu item
            CString strPreview(MAKEINTRESOURCEW(IDS_FONT_PREVIEW));
            ::InsertMenuW(pqcminfo->hmenu, 0, MF_BYPOSITION | MF_STRING,
                          pqcminfo->idCmdFirst++, strPreview); // Command 0
            // Make it default
            ::SetMenuDefaultItem(pqcminfo->hmenu, pqcminfo->idCmdFirst - 1, FALSE);

            // Insert [Hide]/[Show] according to the state of the first selected font
            bool bHidden = false;
            CDataObjectHIDA cida(pdtobj);
            if (cida && cida->cidl > 0)
            {
                const FontPidlEntry* entry = _FontFromIL(HIDA_GetPIDLItem(cida, 0));
                if (entry)
                    bHidden = g_FontCache->IsHidden(g_FontCache->FindByName(entry->Name()));
            }
            CString strToggle(MAKEINTRESOURCEW(bHidden ? IDS_FONT_SHOW_VERB : IDS_FONT_HIDE_VERB));
            ::InsertMenuW(pqcminfo->hmenu, 1, MF_BYPOSITION | MF_STRING,
                          pqcminfo->idCmdFirst++, strToggle); // Command 1
            return S_OK;
        }
        case DFM_GETVERBA:
        case DFM_GETVERBW:
        {
            // Replace default "open" command action
            UINT idCmd = LOWORD(wParam), cchMax = HIWORD(wParam);
            if (idCmd == 0)
            {
                if (uMsg == DFM_GETVERBA)
                    lstrcpynA((PSTR)lParam, "open", cchMax);
                else
                    lstrcpynW((PWSTR)lParam, L"open", cchMax);
                return S_OK;
            }
            break;
        }
        case DFM_INVOKECOMMANDEX:
            return E_NOTIMPL;
        case DFM_INVOKECOMMAND:
        {
            if (wParam == 0)
                return pThis->PreviewItems(pdtobj);
            if (wParam == 1)
                return pThis->ToggleHidden(pdtobj);
            if (wParam == DFM_CMD_COPY)
                return S_FALSE;
            if (wParam == DFM_CMD_DELETE)
                return pThis->DeleteItems(pdtobj);
            if (wParam == DFM_CMD_PASTE)
                return S_FALSE;
            if (wParam == DFM_CMD_PROPERTIES)
                return S_FALSE;
            if (wParam == DFM_CMD_MOVE)
            {
                ERR("DFM_CMD_MOVE not supported\n");
                return E_NOTIMPL;
            }
            ERR("wParam: %p\n", wParam);
            return E_FAIL;
        }
        case DFM_GETDEFSTATICID: // Required for Windows 7 to pick a default
            return S_FALSE;
        case DFM_WM_INITMENUPOPUP:
        {
            HMENU hMenu = (HMENU)wParam;
            // Delete default [Open] menu item
            ::DeleteMenu(hMenu, FCIDM_SHVIEW_OPEN, MF_BYCOMMAND);
            // Disable [Paste link] menu item
            ::EnableMenuItem(hMenu, FCIDM_SHVIEW_INSERTLINK, MF_BYCOMMAND | MF_GRAYED);
            break;
        }
    }
    return E_NOTIMPL;
}

// A family stack opens like any folder item, so only the file operations are ours
HRESULT CALLBACK CFontExt::FamilyMenuCallback(
    IShellFolder *psf, HWND hwnd, IDataObject *pdtobj,
    UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    CFontExt* pThis = static_cast<CFontExt*>(psf);
    if (!pThis)
        return E_FAIL;

    if (uMsg == DFM_MERGECONTEXTMENU)
    {
        QCMINFO* pqcminfo = (QCMINFO*)lParam;
        CString strOpen(MAKEINTRESOURCEW(IDS_FONT_OPEN_VERB));
        ::InsertMenuW(pqcminfo->hmenu, 0, MF_BYPOSITION | MF_STRING, pqcminfo->idCmdFirst++, strOpen); // Command 0
        ::SetMenuDefaultItem(pqcminfo->hmenu, pqcminfo->idCmdFirst - 1, FALSE);
        return S_OK;
    }
    if (uMsg == DFM_GETVERBA || uMsg == DFM_GETVERBW)
    {
        UINT idCmd = LOWORD(wParam), cchMax = HIWORD(wParam);
        if (idCmd == 0)
        {
            if (uMsg == DFM_GETVERBA)
                lstrcpynA((PSTR)lParam, "open", cchMax);
            else
                lstrcpynW((PWSTR)lParam, L"open", cchMax);
            return S_OK;
        }
    }
    if (uMsg == DFM_GETDEFSTATICID)
        return S_FALSE;
    if (uMsg == DFM_WM_INITMENUPOPUP)
    {
        ::DeleteMenu((HMENU)wParam, FCIDM_SHVIEW_OPEN, MF_BYCOMMAND);
        return E_NOTIMPL;
    }
    if (uMsg == DFM_INVOKECOMMAND)
    {
        if (wParam == 0)
            return pThis->OpenFamily(pdtobj);
        if (wParam == DFM_CMD_COPY || wParam == DFM_CMD_PASTE || wParam == DFM_CMD_PROPERTIES)
            return S_FALSE;
        if (wParam == DFM_CMD_DELETE)
            return pThis->DeleteItems(pdtobj);
    }
    return E_NOTIMPL;
}

STDMETHODIMP CFontExt::GetUIObjectOf(HWND hwndOwner, UINT cidl, PCUITEMID_CHILD_ARRAY apidl, REFIID riid, UINT * prgfInOut, LPVOID * ppvOut)
{
    if (riid == IID_IContextMenu ||
        riid == IID_IContextMenu2 ||
        riid == IID_IContextMenu3)
    {
        if (cidl <= 0)
            return E_FAIL;

        bool bAllFamilies = true;
        for (UINT i = 0; i < cidl; ++i)
            bAllFamilies = bAllFamilies && _FamilyFromIL(apidl[i]);
        return CDefFolderMenu_Create2(NULL, hwndOwner, cidl, apidl, this,
                                      bAllFamilies ? FamilyMenuCallback : MenuCallback,
                                      0, NULL, (IContextMenu**)ppvOut);
    }
    else if (riid == IID_IExtractIconA || riid == IID_IExtractIconW)
    {
        if (cidl == 1)
        {
            bool bFamily;
            const FontPidlEntry* fontEntry = _AnyFontEntry(*apidl, &bFamily);
            CFontInfo* sampleInfo = fontEntry ? g_FontCache->FindByName(fontEntry->Name()) : nullptr;
            if (sampleInfo)
            {
                FontIconSpec spec;
                CFontMeta& meta = sampleInfo->Meta();
                spec.File = g_FontCache->Filename(sampleInfo, true);
                spec.Face = meta.valid ? meta.Family : sampleInfo->DisplayName();
                spec.Weight = (meta.hasOS2 && meta.weight >= 100 && meta.weight <= 900) ? meta.weight : 400;
                spec.Italic = meta.italic;
                spec.Stack = bFamily;
                HRESULT hrIcon = _CFontIcon_Create(&spec, riid, ppvOut);
                if (SUCCEEDED(hrIcon))
                    return hrIcon;
            }
            if (fontEntry && !bFamily)
            {
                DWORD dwAttributes = FILE_ATTRIBUTE_NORMAL;
                CStringW strFileName = g_FontCache->GetFontFilePath(fontEntry->FileName());
                return SHCreateFileExtractIconW(strFileName, dwAttributes, riid, ppvOut);
            }
        }
        else
        {
            ERR("IID_IExtractIcon with cidl != 1 UNIMPLEMENTED\n");
        }
    }
    else if (riid == IID_IDataObject)
    {
        if (cidl >= 1)
        {
            return ExpandedDataObject(cidl, apidl, riid, ppvOut);
        }
        else
        {
            CComPtr<IDataObject> pDataObj;
            HRESULT hr = OleGetClipboard(&pDataObj);
            if (FAILED_UNEXPECTEDLY(hr))
                return E_FAIL;
            *ppvOut = pDataObj.Detach();
            return S_OK;
        }
    }
    else if (riid == IID_IDropTarget)
    {
        *ppvOut = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }

    //ERR("%s(riid=%S) UNIMPLEMENTED\n", __FUNCTION__, g2s(riid));
    return E_NOTIMPL;
}

STDMETHODIMP CFontExt::GetDisplayNameOf(PCUITEMID_CHILD pidl, DWORD dwFlags, LPSTRRET strRet)
{
    if (!pidl || !strRet)
        return E_INVALIDARG;

    // Validate that this pidl is the last one
    PCUIDLIST_RELATIVE nextPidl = ILGetNext(pidl);
    if (nextPidl && nextPidl->mkid.cb != 0)
    {
        ERR("ERROR, unhandled PIDL!\n");
        return E_FAIL;
    }

    const FontPidlEntry* familyEntry = _FamilyFromIL(pidl);
    if (familyEntry)
    {
        if (dwFlags & SHGDN_FORPARSING)
            return SHSetStrRet(strRet, familyEntry->FileName());
        return SHSetStrRet(strRet, FontDisplayName(familyEntry->Name()));
    }

    const FontPidlEntry* fontEntry = _FontFromIL(pidl);
    if (!fontEntry)
        return E_FAIL;

    if (dwFlags & SHGDN_FORPARSING)
    {
        CStringW fileName = fontEntry->FileName();
        if (!(dwFlags & SHGDN_INFOLDER))
        {
            CStringW fullPath = g_FontCache->GetFontFilePath(fileName);
            if (!fullPath.IsEmpty())
                return SHSetStrRet(strRet, fullPath);
        }
        return SHSetStrRet(strRet, fileName);
    }

    return SHSetStrRet(strRet, FontDisplayName(fontEntry->Name()));
}

STDMETHODIMP CFontExt::SetNameOf(HWND hwndOwner, PCUITEMID_CHILD pidl, LPCOLESTR lpName, DWORD dwFlags, PITEMID_CHILD *pPidlOut)
{
    ERR("%s() UNIMPLEMENTED\n", __FUNCTION__);
    return E_NOTIMPL;
}

// *** IPersistFolder2 methods ***
STDMETHODIMP CFontExt::GetCurFolder(LPITEMIDLIST *ppidl)
{
    if (ppidl && m_Folder)
    {
        *ppidl = ILClone(m_Self ? m_Self : m_Folder);
        return S_OK;
    }

    return E_POINTER;
}

// *** IPersistFolder methods ***
STDMETHODIMP CFontExt::Initialize(LPCITEMIDLIST pidl)
{
    WCHAR PidlPath[MAX_PATH + 1] = {0}, FontsDir[MAX_PATH + 1];
    if (!SHGetPathFromIDListW(pidl, PidlPath))
    {
        ERR("Unable to extract path from pidl\n");
        return E_FAIL;
    }

    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_FONTS, NULL, 0, FontsDir);
    if (FAILED_UNEXPECTEDLY(hr))
    {
        ERR("Unable to get fonts path (0x%x)\n", hr);
        return hr;
    }

    if (StrCmpIW(PidlPath, FontsDir))
    {
        ERR("CFontExt View initializing on unexpected folder: '%S'\n", PidlPath);
        return E_FAIL;
    }

    m_Folder.Attach(ILCreateFromPathW(FontsDir));
    m_Self.Attach(ILClone(m_Folder));
    StringCchCatW(FontsDir, _countof(FontsDir), L"\\");
    g_FontCache->SetFontDir(FontsDir);

    return S_OK;
}

// *** IPersist methods ***
STDMETHODIMP CFontExt::GetClassID(CLSID *lpClassId)
{
    *lpClassId = CLSID_CFontExt;
    return S_OK;
}

// *** IDropTarget methods ***
STDMETHODIMP CFontExt::DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)
{
    m_bDragAccepted = CheckDataObject(pDataObj);
    if (!m_bDragAccepted)
    {
        *pdwEffect = DROPEFFECT_NONE;
        return E_FAIL;
    }

    *pdwEffect &= DROPEFFECT_COPY;
    return S_OK;
}

STDMETHODIMP CFontExt::DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)
{
    if (!m_bDragAccepted)
    {
        *pdwEffect = DROPEFFECT_NONE;
        return E_FAIL;
    }
    *pdwEffect &= DROPEFFECT_COPY;
    return S_OK;
}

STDMETHODIMP CFontExt::DragLeave()
{
    return S_OK;
}

STDMETHODIMP CFontExt::Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)
{
    ATLASSERT(m_hwndView);
    HRESULT hr = InstallFontsFromDataObject(m_hwndView, pDataObj);

    CStringW text, title;
    title.LoadStringW(IDS_REACTOS_FONTS_FOLDER);
    if (FAILED_UNEXPECTEDLY(hr))
    {
        // Show error message
        text.LoadStringW(IDS_INSTALL_FAILED);
        MessageBoxW(m_hwndView, text, title, MB_ICONERROR);
    }
    else if (hr == S_OK)
    {
        // Refresh font cache and notify the system about the font change
        if (g_FontCache)
            g_FontCache->Read();

        PostMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"fonts");
        PostMessageW(HWND_BROADCAST, WM_FONTCHANGE, 0, 0);

        // Show successful message
        text.LoadStringW(IDS_INSTALL_OK);
        MessageBoxW(m_hwndView, text, title, MB_ICONINFORMATION);
    }

    return hr;
}
