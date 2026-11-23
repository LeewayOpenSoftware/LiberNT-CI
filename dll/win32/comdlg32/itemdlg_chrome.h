/*
 * PROJECT:     LiberNT
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Windows 11 chrome of the item dialog
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#define IDC_NAVBACK             201
#define IDC_NAVFORWARD          202
#define IDC_NAVUP               203
#define IDC_NAVREFRESH          204
#define IDC_NEWFOLDER           205
#define IDC_ORGANIZE            206
#define IDC_VIEWMENU            207
#define IDC_HIDEFOLDERS         208

#define IDS_ORGANIZE            1208
#define IDS_NEW_FOLDER          1209
#define IDS_HIDE_FOLDERS        1210
#define IDS_BROWSE_FOLDERS      1211
#define IDS_SEARCH_IN           1212
#define IDS_ORG_CUT             1213
#define IDS_ORG_COPY            1214
#define IDS_ORG_PASTE           1215
#define IDS_ORG_DELETE          1216
#define IDS_ORG_RENAME          1217
#define IDS_ORG_SELECTALL       1218
#define IDS_ORG_PROPERTIES      1219
#define IDS_VIEW_DETAILS        1220
#define IDS_VIEW_LIST           1221
#define IDS_VIEW_SMALL          1222
#define IDS_VIEW_MEDIUM         1223
#define IDS_VIEW_LARGE          1224
#define IDS_NAV_REFRESH         1225
#define IDS_NAV_BACK            1226
#define IDS_NAV_FORWARD         1227
#define IDS_NAV_UP              1228

#ifndef RC_INVOKED

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ItemDlgChrome ItemDlgChrome;

typedef struct ItemDlgChromeHost
{
    void (*navigate)(void *context, PCIDLIST_ABSOLUTE pidl);
    void (*navigate_text)(void *context, LPCWSTR text);
    IShellView *(*get_view)(void *context);
    void (*collapse)(void *context, BOOL collapsed);
} ItemDlgChromeHost;

ItemDlgChrome *ItemDlgChrome_Create(HWND dialog, const ItemDlgChromeHost *host, void *context, BOOL save);
void ItemDlgChrome_Destroy(ItemDlgChrome *chrome);
void ItemDlgChrome_Layout(ItemDlgChrome *chrome, HDWP *hdwp, const RECT *client, int bottom, const RECT *buttons,
                          RECT *browser);
BOOL ItemDlgChrome_IsCollapsed(ItemDlgChrome *chrome);
int ItemDlgChrome_TopHeight(ItemDlgChrome *chrome);
BOOL ItemDlgChrome_OnColor(ItemDlgChrome *chrome, UINT msg, HDC dc, INT_PTR *result);
void ItemDlgChrome_SettingsChanged(ItemDlgChrome *chrome);
void ItemDlgChrome_SetLocation(ItemDlgChrome *chrome, PCIDLIST_ABSOLUTE pidl);
BOOL ItemDlgChrome_OnNotify(ItemDlgChrome *chrome, NMHDR *header, LRESULT *result);
BOOL ItemDlgChrome_OnCommand(ItemDlgChrome *chrome, UINT id);

#ifdef __cplusplus
}
#endif

#endif /* RC_INVOKED */
