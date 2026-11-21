/*
 * PROJECT:     LiberNT Font Shell Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Thumbnail provider for font files
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

class CFontThumbnail :
    public CComCoClass<CFontThumbnail, &CLSID_CFontThumbnail>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IInitializeWithFile,
    public IThumbnailProvider
{
    CStringW m_Path;

public:
    // IInitializeWithFile
    STDMETHOD(Initialize)(LPCWSTR pszFilePath, DWORD grfMode) override;

    // IThumbnailProvider
    STDMETHOD(GetThumbnail)(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha) override;

    DECLARE_REGISTRY_RESOURCEID(IDR_FONTTHUMB)
    DECLARE_NOT_AGGREGATABLE(CFontThumbnail)

    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CFontThumbnail)
        COM_INTERFACE_ENTRY_IID(IID_IInitializeWithFile, IInitializeWithFile)
        COM_INTERFACE_ENTRY_IID(IID_IThumbnailProvider, IThumbnailProvider)
    END_COM_MAP()
};
