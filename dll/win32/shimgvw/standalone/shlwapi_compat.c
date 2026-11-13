/* Win11 standalone: shlwapi's ordinal-only SHForwardContextMenuMsg, reimplemented over IContextMenu2/3. */
#include <windows.h>
#include <shlobj.h>
HRESULT WINAPI SHForwardContextMenuMsg(IUnknown* pUnk, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* pResult, BOOL useIContextMenu2)
{
    IContextMenu3 *cm3 = NULL;
    IContextMenu2 *cm2 = NULL;
    HRESULT hr = E_FAIL;
    LRESULT res = 0;
    if (!pUnk)
        return E_INVALIDARG;
    if (!useIContextMenu2 && SUCCEEDED(pUnk->lpVtbl->QueryInterface(pUnk, &IID_IContextMenu3, (void **)&cm3)))
    {
        hr = cm3->lpVtbl->HandleMenuMsg2(cm3, uMsg, wParam, lParam, &res);
        cm3->lpVtbl->Release(cm3);
    }
    else if (SUCCEEDED(pUnk->lpVtbl->QueryInterface(pUnk, &IID_IContextMenu2, (void **)&cm2)))
    {
        hr = cm2->lpVtbl->HandleMenuMsg(cm2, uMsg, wParam, lParam);
        cm2->lpVtbl->Release(cm2);
    }
    if (pResult)
        *pResult = res;
    return hr;
}
