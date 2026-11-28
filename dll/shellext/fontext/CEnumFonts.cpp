/*
 * PROJECT:     ReactOS Font Shell Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     CEnumFonts implementation
 * COPYRIGHT:   Copyright 2019 Mark Jansen <mark.jansen@reactos.org>
 */

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(fontext);

class CEnumFonts :
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IEnumIDList
{
private:
    DWORD m_dwFlags;
    ULONG m_Index;
    CAtlArray<FontViewItem> m_Items;

    bool Wanted(const FontViewItem& item) const
    {
        if (!m_dwFlags)
            return true;
        return item.bFamily ? !!(m_dwFlags & SHCONTF_FOLDERS) : !!(m_dwFlags & SHCONTF_NONFOLDERS);
    }

public:
    CEnumFonts()
        : m_dwFlags(0)
        , m_Index(0)
    {
    }

    HRESULT Initialize(CFontExt* folder, DWORD flags)
    {
        m_dwFlags = flags;
        m_Index = 0;
        const CStringW& key = folder->GetFamilyKey();
        g_FontCache->BuildView(key.IsEmpty() ? nullptr : &key, m_Items);
        return S_OK;
    }

    // *** IEnumIDList methods ***
    STDMETHODIMP Next(ULONG celt, LPITEMIDLIST *rgelt, ULONG *pceltFetched) override
    {
        if (!rgelt || (!pceltFetched && celt != 1))
            return E_POINTER;

        HRESULT hr = S_OK;
        ULONG Fetched = 0;

        while (celt)
        {
            if (m_Index >= m_Items.GetCount())
            {
                hr = S_FALSE;
                break;
            }

            const FontViewItem& item = m_Items[m_Index];
            if (Wanted(item))
            {
                PITEMID_CHILD pidl = NULL;
                if (item.bFamily)
                {
                    pidl = _ILCreateFamily(item.RegName, item.Key);
                }
                else
                {
                    CFontInfo* info = g_FontCache->FindByName(item.RegName);
                    CStringW FileName = info ? info->File() : CStringW();
                    if (!FileName.IsEmpty())
                        pidl = _ILCreate(item.RegName, FileName);
                    else
                        ERR("Why is Name or FileName empty?\n");
                }
                if (pidl)
                {
                    rgelt[Fetched++] = pidl;
                    --celt;
                }
            }
            m_Index++;
        }

        if (pceltFetched)
            *pceltFetched = Fetched;
        return hr;
    }

    STDMETHODIMP Skip(ULONG celt) override
    {
        while (celt && m_Index < m_Items.GetCount())
        {
            if (Wanted(m_Items[m_Index]))
                --celt;
            ++m_Index;
        }
        return S_OK;
    }

    STDMETHODIMP Reset() override
    {
        m_Index = 0;
        return S_OK;
    }

    STDMETHODIMP Clone(IEnumIDList **ppenum) override
    {
        return E_NOTIMPL;
    }

public:
    DECLARE_NOT_AGGREGATABLE(CEnumFonts)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CEnumFonts)
        COM_INTERFACE_ENTRY_IID(IID_IEnumIDList, IEnumIDList)
    END_COM_MAP()
};

HRESULT _CEnumFonts_CreateInstance(CFontExt* zip, DWORD flags, REFIID riid, LPVOID * ppvOut)
{
    return ShellObjectCreatorInit<CEnumFonts>(zip, flags, riid, ppvOut);
}
