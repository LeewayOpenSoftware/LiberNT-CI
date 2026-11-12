/*
 * PROJECT:    PAINT for ReactOS
 * LICENSE:    LGPL-2.0-or-later (https://spdx.org/licenses/LGPL-2.0-or-later)
 * PURPOSE:    The main window
 * COPYRIGHT:  Copyright 2015 Benedikt Freisen <b.freisen@gmx.net>
 *             Copyright 2023 Katayama Hirofumi MZ <katayama.hirofumi.mz@gmail.com>
 */

#pragma once

#include "paint_ribbon_api.h"
#include "../../../sdk/include/leewayui/dpi.h"

#define WM_RIBBONREFRESH (WM_APP + 21)

class CMainWindow : public CWindowImpl<CMainWindow>
{
public:
    DECLARE_WND_CLASS_EX(L"MSPaintApp", CS_DBLCLKS, COLOR_BTNFACE)

    BEGIN_MSG_MAP(CMainWindow)
        MESSAGE_HANDLER(WM_DROPFILES, OnDropFiles)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_INITMENUPOPUP, OnInitMenuPopup)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_GETMINMAXINFO, OnGetMinMaxInfo)
        MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
        MESSAGE_HANDLER(WM_SYSCOLORCHANGE, OnSysColorChange)
        MESSAGE_HANDLER(WM_COMMAND, OnCommand)
        MESSAGE_HANDLER(WM_NOTIFY, OnNotify)
        MESSAGE_HANDLER(0x02E0 /* WM_DPICHANGED */, OnDpiChanged)
        MESSAGE_HANDLER(WM_RIBBONREFRESH, OnRibbonRefresh)
        MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
    END_MSG_MAP()

    CMainWindow() : m_hMenu(NULL), m_pRibbon(NULL) { }

    HWND DoCreate();
    BOOL GetOpenFileName(IN OUT LPWSTR pszFile, INT cchMaxFile);
    BOOL GetSaveFileName(IN OUT LPWSTR pszFile, INT cchMaxFile);
    BOOL ChooseColor(IN OUT COLORREF *prgbColor);
    VOID TrackPopupMenu(POINT ptScreen, INT iSubMenu);
    BOOL CanUndo() const;
    BOOL CanRedo() const;
    BOOL CanPaste() const;
    void InvalidateRibbon();
    BOOL PreTranslateRibbon(MSG *pMsg);
    void SetRibbonAccelerators(HACCEL hAccel);
    void SaveAsFormat(PCWSTR pszExt);

private:
    HMENU m_hMenu;
    ILeewayRibbon *m_pRibbon;
    BOOL m_bRibbonDirty = FALSE;

    LRESULT OnDropFiles(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnCreate(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnDestroy(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnClose(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnInitMenuPopup(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnSize(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnGetMinMaxInfo(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnKeyDown(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnSysColorChange(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnCommand(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnNotify(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnDpiChanged(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnRibbonRefresh(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnMouseWheel(UINT nMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);

    void alignChildrenToMainWindow();
    void saveImage(BOOL overwrite);
    void InsertSelectionFromHBITMAP(HBITMAP bitmap, HWND window);
    BOOL ConfirmSave();
    void ProcessFileMenu(HMENU hPopupMenu);
};
