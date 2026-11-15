/*
 * PROJECT:     LiberNT uxtheme
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Version-4 style adapter over the Theme Loader 4 core (I1b, docs/design/I1B-V4-PATH.md in TL4)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * A version-4 THEME_FILE carries a V4_FILE: the checked mapping of the style (and of its strings module), the TL4
 * model built on it, and one THEME_PROPERTY per model property (the shadow array). The rest of uxtheme reaches a v4
 * file only through the dispatch lines in msstyles.c, property.c and draw.c, each of which tests tf->v4 / tp->v4
 * first. Every value a getter returns passes the range function of the loader library (REQUIREMENTS section 6.1);
 * out of range is E_PROP_ID_UNSUPPORTED. Nothing here opens a file or reads a setting: the trust gate (trust.c) does
 * that and hands the mapping over on a version-4 fork.
 */
#ifndef _MSSTYLES_V4_H_
#define _MSSTYLES_V4_H_

#include <tl4/loader/pe.h>
#include <tl4/loader/strings.h>
#include <tl4/loader/v4.h>
#include <tl4/loader/model.h>
#include <tl4/loader/png.h>
#include <tl4/loader/range.h>

#define V4_MODEL_BUFFER_CAP (2u * 1024u * 1024u)   /* above the 1,581,080 bytes of a style at the section-6 limits */

typedef struct _V4_FILE {
    /* the checked style: handle (deny-write share kept), section and read-only view; owned until CloseThemeFile */
    HANDLE hFile, hSection;
    const BYTE *base;
    SIZE_T size;
    /* the strings module, or zero */
    HANDLE hMui, hMuiSection;
    const BYTE *muiBase;
    SIZE_T muiSize;
    tl4_pe pe, peMui;
    BOOL hasMui;
    tl4_strings strings;          /* pointers to pe / peMui */
    tl4_model model;
    void *modelBuffer;
    SIZE_T modelBytes;
    THEME_PROPERTY *shadow;       /* one per model property, lpValue NULL, v4 set */
    UINT nshadow;
    WCHAR *names;                 /* the colour and size lists as v3 keeps them: name NUL NUL; the size list at namesSizeOff */
    UINT namesSizeOff;
    tl4_pixel_budget budget;      /* R-RES-4: decoded pixels of this style, taken by the image loader before each decode */
    THEME_PARTSTATE psSentinel;   /* what FindPart / FindPartState return for an existing v4 part-state: never dereferenced */
} V4_FILE, *PV4_FILE;

/* R-RES-4 / section 6: the per-style pixel budget for the life of the process (the image loader takes from it) */
#define V4_PIXEL_BUDGET (64ull * 1024ull * 1024ull)

/* Build a THEME_FILE from the checked mapping(s). On success the handles, sections and views belong to the new
 * THEME_FILE; on failure nothing of them is touched or closed, nothing allocated here stays, and *ptf is NULL. */
HRESULT MSSTYLES_V4_Open(HANDLE hFile, HANDLE hSection, const BYTE *base, SIZE_T size,
                         HANDLE hMui, HANDLE hMuiSection, const BYTE *muiBase, SIZE_T muiSize,
                         LPCWSTR pszPath, LPCWSTR pszColorName, LPCWSTR pszSizeName, PTHEME_FILE *ptf);
/* Release what V4_FILE owns, in order: shadow, model buffer, views, sections, handles. The classes list of the
 * THEME_FILE is freed by MSSTYLES_CloseThemeFile as for v3 (a v4 class has no part-states of its own). */
void MSSTYLES_V4_Close(PTHEME_FILE tf);

PTHEME_PROPERTY MSSTYLES_V4_FindProperty(PTHEME_CLASS tc, int iPartId, int iStateId, int iPropertyPrimitive, int iPropertyId);
PTHEME_PARTSTATE MSSTYLES_V4_FindPartState(PTHEME_CLASS tc, int iPartId, int iStateId);   /* iStateId 0: the part */

HRESULT MSSTYLES_V4_GetBool(PTHEME_PROPERTY tp, BOOL *pfVal);
HRESULT MSSTYLES_V4_GetColor(PTHEME_PROPERTY tp, COLORREF *pColor);
HRESULT MSSTYLES_V4_GetEnum(PTHEME_PROPERTY tp, int *piVal);
HRESULT MSSTYLES_V4_GetFont(PTHEME_PROPERTY tp, HDC hdc, LOGFONTW *pFont);
HRESULT MSSTYLES_V4_GetInt(PTHEME_PROPERTY tp, int *piVal);
HRESULT MSSTYLES_V4_GetIntList(PTHEME_PROPERTY tp, INTLIST *pIntList);
HRESULT MSSTYLES_V4_GetPosition(PTHEME_PROPERTY tp, POINT *pPoint);
HRESULT MSSTYLES_V4_GetRect(PTHEME_PROPERTY tp, RECT *pRect);
HRESULT MSSTYLES_V4_GetMargins(PTHEME_PROPERTY tp, MARGINS *pMargins);
HRESULT MSSTYLES_V4_GetString(PTHEME_PROPERTY tp, LPWSTR pszBuff, int cchMaxBuffChars);
/* An IMAGE / STREAM reference as the name the image cache keys on: "#<id>". */
HRESULT MSSTYLES_V4_GetImageName(PTHEME_PROPERTY tp, LPWSTR pszBuff, int cchMaxBuffChars);
/* The bytes of IMAGE resource `id` inside the style's view (for the PNG decoder of the image loader). */
BOOL MSSTYLES_V4_ImageData(PTHEME_FILE tf, UINT id, const BYTE **ppData, UINT *pcbData);

#endif
