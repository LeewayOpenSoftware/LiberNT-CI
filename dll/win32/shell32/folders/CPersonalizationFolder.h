/*
 * PROJECT:     shell32
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Control Panel > Appearance and Personalization > Personalization,
 *              a shell folder shown in the Explorer view pane
 *
 * The folder has one root page (the theme gallery) and four child pages
 * (Desktop Background, Window Color, Display, Screen Resolution). Every page is
 * a child PIDL, so Explorer's address bar, Back/Forward and Up work on them.
 */

#ifndef _SHFLDR_PERSONALIZATION_H_
#define _SHFLDR_PERSONALIZATION_H_

/* {ED834ED6-4B5A-4bfe-8F11-A626DCB6A921}, the CLSID of Microsoft.Personalization */
EXTERN_C const CLSID CLSID_PersonalizationFolder;

/* Page ids, shared with the page component (leewaydisp.cpl) */
#define LWPAGE_MAIN         0
#define LWPAGE_BACKGROUND   1
#define LWPAGE_WINCOLOR     2
#define LWPAGE_DISPLAY      3
#define LWPAGE_RESOLUTION   4
#define LWPAGE_COUNT        5
#define LWNAV_CPHOME        0xFFFF

/* The "Appearance and Personalization" category: the Control Panel folder holds one id for it, and
 * binding it gives a folder whose only child is Personalization */
PCWSTR PersCategoryName(VOID);
PITEMID_CHILD CplCategoryCreatePidl(VOID);
BOOL CplIsCategoryPidl(PCUIDLIST_RELATIVE pidl);
int PersCompareChain(PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2);
BOOL CplIsCategoryName(PCWSTR name);
BOOL CplIsPersonalizationParseName(PCWSTR name);
HRESULT CplCategoryBind(PCIDLIST_ABSOLUTE pidlAbsCategory, REFIID riid, LPVOID *ppvOut);
/* Browses from a view without doing it inside the view's own window procedure; takes pidl */
HRESULT PersNavigateOutside(IShellBrowser *psb, PIDLIST_ABSOLUTE pidl);

class CPersonalizationFolder :
    public CComCoClass<CPersonalizationFolder, &CLSID_PersonalizationFolder>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public IShellFolder2,
    public IPersistFolder2
{
    private:
        LPITEMIDLIST m_pidlRoot;    /* absolute pidl of this folder */
        UINT m_page;                /* LWPAGE_* shown by this folder */

    public:
        CPersonalizationFolder();
        ~CPersonalizationFolder();

        // IShellFolder
        STDMETHOD(ParseDisplayName)(HWND hwndOwner, LPBC pbc, LPOLESTR lpszDisplayName, DWORD *pchEaten, PIDLIST_RELATIVE *ppidl, DWORD *pdwAttributes) override;
        STDMETHOD(EnumObjects)(HWND hwndOwner, DWORD dwFlags, LPENUMIDLIST *ppEnumIDList) override;
        STDMETHOD(BindToObject)(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut) override;
        STDMETHOD(BindToStorage)(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut) override;
        STDMETHOD(CompareIDs)(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2) override;
        STDMETHOD(CreateViewObject)(HWND hwndOwner, REFIID riid, LPVOID *ppvOut) override;
        STDMETHOD(GetAttributesOf)(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, DWORD *rgfInOut) override;
        STDMETHOD(GetUIObjectOf)(HWND hwndOwner, UINT cidl, PCUITEMID_CHILD_ARRAY apidl, REFIID riid, UINT *prgfInOut, LPVOID *ppvOut) override;
        STDMETHOD(GetDisplayNameOf)(PCUITEMID_CHILD pidl, DWORD dwFlags, LPSTRRET strRet) override;
        STDMETHOD(SetNameOf)(HWND hwndOwner, PCUITEMID_CHILD pidl, LPCOLESTR lpName, DWORD dwFlags, PITEMID_CHILD *pPidlOut) override;

        // IShellFolder2
        STDMETHOD(GetDefaultSearchGUID)(GUID *pguid) override;
        STDMETHOD(EnumSearches)(IEnumExtraSearch **ppenum) override;
        STDMETHOD(GetDefaultColumn)(DWORD dwRes, ULONG *pSort, ULONG *pDisplay) override;
        STDMETHOD(GetDefaultColumnState)(UINT iColumn, DWORD *pcsFlags) override;
        STDMETHOD(GetDetailsEx)(PCUITEMID_CHILD pidl, const SHCOLUMNID *pscid, VARIANT *pv) override;
        STDMETHOD(GetDetailsOf)(PCUITEMID_CHILD pidl, UINT iColumn, SHELLDETAILS *psd) override;
        STDMETHOD(MapColumnToSCID)(UINT column, SHCOLUMNID *pscid) override;

        // IPersist
        STDMETHOD(GetClassID)(CLSID *lpClassId) override;

        // IPersistFolder
        STDMETHOD(Initialize)(PCIDLIST_ABSOLUTE pidl) override;

        // IPersistFolder2
        STDMETHOD(GetCurFolder)(PIDLIST_ABSOLUTE *pidl) override;

        static HRESULT WINAPI UpdateRegistry(BOOL bRegister) { return S_OK; } // registered by comreg_code.inf
        DECLARE_NOT_AGGREGATABLE(CPersonalizationFolder)

        DECLARE_PROTECT_FINAL_CONSTRUCT()

        BEGIN_COM_MAP(CPersonalizationFolder)
        COM_INTERFACE_ENTRY_IID(IID_IShellFolder2, IShellFolder2)
        COM_INTERFACE_ENTRY_IID(IID_IShellFolder, IShellFolder)
        COM_INTERFACE_ENTRY_IID(IID_IPersistFolder, IPersistFolder)
        COM_INTERFACE_ENTRY_IID(IID_IPersistFolder2, IPersistFolder2)
        COM_INTERFACE_ENTRY_IID(IID_IPersist, IPersist)
        END_COM_MAP()
};

#endif /* _SHFLDR_PERSONALIZATION_H_ */
