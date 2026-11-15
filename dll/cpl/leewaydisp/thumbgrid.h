/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Grouped thumbnail grid control (theme gallery, wallpaper picker)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#define TG_CLASS L"LeewayThumbGrid"

/* window style bits (class specific) */
#define TGS_CHECKBOXES 0x0001
#define TGS_LABELS     0x0002   /* caption under each thumbnail (theme gallery) */
#define TGS_SWATCH     0x0004   /* window colour square over the thumbnail corner */

#define TGM_CLEAR     (WM_USER + 1)
#define TGM_ADDGROUP  (WM_USER + 2)   /* lParam = group name; returns group index or -1 */
#define TGM_ADDITEM   (WM_USER + 3)   /* lParam = TGITEM*; returns item index or -1 */
#define TGM_SETSEL    (WM_USER + 4)   /* wParam = index or -1 */
#define TGM_GETSEL    (WM_USER + 5)
#define TGM_SETCHECK  (WM_USER + 6)   /* wParam = index, lParam = BOOL */
#define TGM_GETCHECK  (WM_USER + 7)   /* wParam = index */
#define TGM_GETCOUNT  (WM_USER + 8)
#define TGM_CHECKALL  (WM_USER + 9)   /* lParam = BOOL */
#define TGM_GETDATA   (WM_USER + 10)  /* wParam = index; returns TGITEM.Data */
#define TGM_SETCELL   (WM_USER + 11)  /* wParam = thumb width, lParam = thumb height */

#define TGM_GETIMAGE  (WM_USER + 12)  /* wParam = index, lParam = WCHAR[MAX_PATH] */

/* sent to the parent as WM_COMMAND(MAKEWPARAM(id, code), hwnd) */
#define TGN_SEL      1
#define TGN_CHECK    2
#define TGN_ACTIVATE 3

typedef struct TGITEM
{
    int Group;
    const WCHAR *Text;
    const WCHAR *Image;     /* picture path, may be NULL */
    COLORREF Fill;          /* thumbnail colour when there is no picture */
    BOOL HasSwatch;
    COLORREF Swatch;
    BOOL Checked;
    LPARAM Data;
} TGITEM;

BOOL TgRegister(HINSTANCE hInst);
/* Picture cropped to the thumbnail's aspect ratio as a top-down 32-bit DIB; NULL when the file cannot be read. */
HBITMAP TgMakeThumb(HDC ref, const WCHAR *path, int w, int h);
