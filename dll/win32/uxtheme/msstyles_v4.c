/*
 * PROJECT:     LiberNT uxtheme
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Version-4 style adapter over the Theme Loader 4 core (I1b)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "wingdi.h"
#include "vfwmsgs.h"
#include "uxtheme.h"
#include "vssym32.h"

#include "msstyles.h"
#include "msstyles_v4.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(uxtheme);

#define E_V4_BAD_FORMAT HRESULT_FROM_WIN32(ERROR_BAD_FORMAT)

static PV4_FILE v4_of(PTHEME_PROPERTY tp) { return tp ? (PV4_FILE)tp->v4file : NULL; }
static const tl4_prop *prop_of(PTHEME_PROPERTY tp) { return tp ? (const tl4_prop *)tp->v4 : NULL; }

/* ---- open / close ------------------------------------------------------------------------------------------- */

/* "App::Class" from the CMAP into the two v3 name fields; a half longer than the field is a reject (R-V4-2 bounds it) */
static BOOL split_class_name(const uint8_t *utf16, uint32_t chars, LPWSTR app, LPWSTR cls)
{
    uint32_t i, sep = chars;
    for (i = 0; i + 1 < chars; i++)
        if (utf16[2 * i] == ':' && utf16[2 * i + 1] == 0 && utf16[2 * i + 2] == ':' && utf16[2 * i + 3] == 0) { sep = i; break; }
    if (sep == chars)
    {
        if (chars >= MAX_THEME_CLASS_NAME) return FALSE;
        app[0] = 0;
        for (i = 0; i < chars; i++) cls[i] = (WCHAR)(utf16[2 * i] | (utf16[2 * i + 1] << 8));
        cls[chars] = 0;
        return TRUE;
    }
    if (sep >= MAX_THEME_APP_NAME || chars - sep - 2 >= MAX_THEME_CLASS_NAME) return FALSE;
    for (i = 0; i < sep; i++) app[i] = (WCHAR)(utf16[2 * i] | (utf16[2 * i + 1] << 8));
    app[sep] = 0;
    for (i = sep + 2; i < chars; i++) cls[i - sep - 2] = (WCHAR)(utf16[2 * i] | (utf16[2 * i + 1] << 8));
    cls[chars - sep - 2] = 0;
    return TRUE;
}

static void free_classes(PTHEME_FILE tf)
{
    while (tf->classes)
    {
        PTHEME_CLASS c = tf->classes;
        tf->classes = c->next;
        c->signature = 0;
        free(c);
    }
}

static void v4_release(PV4_FILE v4, BOOL releaseHandles)
{
    if (!v4) return;
    if (v4->shadow) HeapFree(GetProcessHeap(), 0, v4->shadow);
    if (v4->modelBuffer) HeapFree(GetProcessHeap(), 0, v4->modelBuffer);
    if (v4->names) HeapFree(GetProcessHeap(), 0, v4->names);
    if (releaseHandles)
    {
        if (v4->base) UnmapViewOfFile(v4->base);
        if (v4->muiBase) UnmapViewOfFile(v4->muiBase);
        if (v4->hSection) CloseHandle(v4->hSection);
        if (v4->hMuiSection) CloseHandle(v4->hMuiSection);
        if (v4->hFile) CloseHandle(v4->hFile);
        if (v4->hMui) CloseHandle(v4->hMui);
    }
    HeapFree(GetProcessHeap(), 0, v4);
}

/* a VMAP name as a v3 list: name NUL NUL, at the given offset of the names buffer */
static UINT put_name(WCHAR *dst, const uint8_t *utf16, uint32_t chars)
{
    uint32_t i;
    for (i = 0; i < chars; i++) dst[i] = (WCHAR)(utf16[2 * i] | (utf16[2 * i + 1] << 8));
    dst[chars] = 0;
    dst[chars + 1] = 0;
    return chars + 2;
}

HRESULT MSSTYLES_V4_Open(HANDLE hFile, HANDLE hSection, const BYTE *base, SIZE_T size,
                         HANDLE hMui, HANDLE hMuiSection, const BYTE *muiBase, SIZE_T muiSize,
                         LPCWSTR pszPath, LPCWSTR pszColorName, LPCWSTR pszSizeName, PTHEME_FILE *ptf)
{
    PV4_FILE v4;
    PTHEME_FILE tf = NULL;
    tl4_v4_stats stats;
    tl4_res_name t, n;
    tl4_resource res;
    tl4_v4_vmap vmap;
    size_t need;
    uint32_t i, ordinal;
    UINT off;
    HRESULT hr = E_V4_BAD_FORMAT;

    TRACE("(%p, %Iu bytes, mui %p, %Iu bytes, %s, %s)\n", base, size, muiBase, muiSize, debugstr_w(pszColorName), debugstr_w(pszSizeName));
    if (!ptf) return E_INVALIDARG;
    *ptf = NULL;
    if (!base || !size || (muiBase && !muiSize)) return E_INVALIDARG;

    v4 = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*v4));
    if (!v4) return E_OUTOFMEMORY;
    v4->hFile = hFile; v4->hSection = hSection; v4->base = base; v4->size = size;
    v4->hMui = hMui; v4->hMuiSection = hMuiSection; v4->muiBase = muiBase; v4->muiSize = muiSize;
    v4->psSentinel.iPartId = -1;
    v4->psSentinel.iStateId = -1;

    /* 1. the bounded readers on both views, the validator on the whole container (R-GEN-10, R-RES-8) */
    if (tl4_pe_open(base, size, &v4->pe) != TL4_OK) goto fail;
    if (muiBase)
    {
        if (tl4_pe_open(muiBase, muiSize, &v4->peMui) != TL4_OK) goto fail;
        v4->hasMui = TRUE;
    }
    if (tl4_v4_validate(&v4->pe, v4->hasMui ? &v4->peMui : NULL, &stats) != TL4_OK) goto fail;

    /* 2. one buffer from the validated statistics, then the model with its second pass (R-GEN-11) */
    need = tl4_model_size(&stats);
    if (need == 0 || need > V4_MODEL_BUFFER_CAP) goto fail;
    v4->modelBuffer = HeapAlloc(GetProcessHeap(), 0, need);
    if (!v4->modelBuffer) { hr = E_OUTOFMEMORY; goto fail; }
    v4->modelBytes = need;
    if (tl4_model_build(&v4->pe, v4->hasMui ? &v4->peMui : NULL, v4->modelBuffer, need, &v4->model) != TL4_OK) goto fail;
    if (tl4_strings_open(&v4->pe, v4->hasMui ? &v4->peMui : NULL, &v4->strings) != TL4_OK) goto fail;

    /* 3. the colour and size lists from VMAP; a requested name must be the one the style lists (as v3) */
    t = tl4_res_ascii("VMAP"); n = tl4_res_ascii("VMAP");
    if (tl4_pe_find(&v4->pe, &t, &n, &res) != TL4_OK || tl4_v4_check_vmap(res.data, res.size, &vmap) != TL4_OK) goto fail;
    v4->names = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (vmap.color_chars + vmap.size_chars + 4) * sizeof(WCHAR));
    if (!v4->names) { hr = E_OUTOFMEMORY; goto fail; }
    off = put_name(v4->names, vmap.color, vmap.color_chars);
    put_name(v4->names + off, vmap.size_name, vmap.size_chars);

    tf = calloc(1, sizeof(THEME_FILE));
    if (!tf) { hr = E_OUTOFMEMORY; goto fail; }
    tf->refcount = 1;
    tf->hTheme = NULL;
    if (pszPath) lstrcpynW(tf->szThemeFile, pszPath, MAX_PATH);   /* the built store path, from the gate */
    tf->pszAvailColors = v4->names;
    tf->pszAvailSizes = v4->names + off;
    tf->pszSelectedColor = (pszColorName && lstrcmpiW(pszColorName, tf->pszAvailColors)) ? NULL : tf->pszAvailColors;
    tf->pszSelectedSize = (pszSizeName && lstrcmpiW(pszSizeName, tf->pszAvailSizes)) ? NULL : tf->pszAvailSizes;
    if (!tf->pszSelectedColor || !tf->pszSelectedSize) { hr = E_PROP_ID_UNSUPPORTED; goto fail; }
    tf->v4 = v4;

    /* 4. one THEME_CLASS per CMAP class (the descriptors before the first class are not classes) */
    for (ordinal = v4->model.counts.descriptors; ordinal < v4->model.nclasses; ordinal++)
    {
        const uint8_t *name;
        uint32_t chars;
        PTHEME_CLASS c;
        if (tl4_model_class_name(&v4->model, ordinal, &name, &chars) != TL4_OK) goto fail;
        c = malloc(sizeof(*c));
        if (!c) { hr = E_OUTOFMEMORY; goto fail; }
        memset(c, 0, sizeof(*c));
        if (!split_class_name(name, chars, c->szAppName, c->szClassName)) { free(c); goto fail; }
        c->signature = 0x12bc6d83;      /* THEME_CLASS_SIGNATURE of msstyles.c */
        c->refcount = 0;
        c->hTheme = NULL;
        c->tf = tf;
        c->v4Ordinal = ordinal;
        c->next = tf->classes;
        tf->classes = c;
    }

    /* 5. the shadow array: stable THEME_PROPERTY pointers into it, one per model property */
    v4->nshadow = v4->model.nprops;
    v4->shadow = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (v4->nshadow ? v4->nshadow : 1) * sizeof(THEME_PROPERTY));
    if (!v4->shadow) { hr = E_OUTOFMEMORY; goto fail; }
    for (i = 0; i < v4->nshadow; i++)
    {
        const tl4_prop *p = &v4->model.props[i];
        THEME_PROPERTY *s = &v4->shadow[i];
        s->iPrimitiveType = p->type;
        s->iPropertyId = p->name_id;
        s->origin = (PROPERTYORIGIN)p->origin;      /* TL4_PO_* equals PO_* by construction (model.h) */
        s->lpValue = NULL;
        s->dwValueLen = 0;
        s->next = NULL;
        s->v4 = p;
        s->v4file = v4;
    }
    *ptf = tf;
    TRACE("version-4 style: %u classes, %u properties, model %Iu bytes, %s module\n",
          v4->model.nclasses - v4->model.counts.descriptors, v4->nshadow, v4->modelBytes, v4->hasMui ? "with" : "no");
    return S_OK;

fail:
    TRACE("refused: 0x%08lx\n", hr);
    if (tf) { free_classes(tf); free(tf); }
    v4_release(v4, FALSE);
    return hr;
}

void MSSTYLES_V4_Close(PTHEME_FILE tf)
{
    TRACE("(%p)\n", tf);
    if (!tf || !tf->v4) return;
    v4_release((PV4_FILE)tf->v4, TRUE);
    tf->v4 = NULL;
    tf->pszAvailColors = tf->pszAvailSizes = tf->pszSelectedColor = tf->pszSelectedSize = NULL;
}

/* ---- lookups --------------------------------------------------------------------------------------------- */

PTHEME_PROPERTY MSSTYLES_V4_FindProperty(PTHEME_CLASS tc, int iPartId, int iStateId, int iPropertyPrimitive, int iPropertyId)
{
    PV4_FILE v4;
    const tl4_prop *p;
    if (!tc || !tc->tf || !tc->tf->v4 || iPartId < 0 || iStateId < 0 || iPropertyId < 0 || iPropertyId > 0xFFFF) return NULL;
    v4 = (PV4_FILE)tc->tf->v4;
    if (tl4_model_find(&v4->model, tc->v4Ordinal, (uint32_t)iPartId, (uint32_t)iStateId, (uint32_t)iPropertyId, &p) != TL4_OK) return NULL;
    if (iPropertyPrimitive && (int)p->type != iPropertyPrimitive) return NULL;   /* as v3: the id with another type is "not found" */
    if (p < v4->model.props || p >= v4->model.props + v4->model.nprops) return NULL;
    return &v4->shadow[p - v4->model.props];
}

/* a VARIANT part-state (part, state) in the class or its base chain (bounded by TL4_V4_MAX_CHAIN as the model is) */
PTHEME_PARTSTATE MSSTYLES_V4_FindPartState(PTHEME_CLASS tc, int iPartId, int iStateId)
{
    PV4_FILE v4;
    uint32_t ordinal, depth, i;
    if (!tc || !tc->tf || !tc->tf->v4 || iPartId < 0 || iStateId < 0) return NULL;
    v4 = (PV4_FILE)tc->tf->v4;
    if (iStateId == 0)
        return tl4_model_part_defined(&v4->model, tc->v4Ordinal, (uint32_t)iPartId) ? &v4->psSentinel : NULL;
    ordinal = tc->v4Ordinal;
    for (depth = 0; depth <= TL4_V4_MAX_CHAIN && ordinal < v4->model.nclasses; depth++)
    {
        const tl4_class *c = &v4->model.classes[ordinal];
        for (i = 0; i < c->nps; i++)
        {
            const tl4_partstate *ps = &v4->model.partstates[c->first_ps + i];
            if (ps->area == TL4_AREA_VARIANT && ps->part == (uint32_t)iPartId && ps->state == (uint32_t)iStateId) return &v4->psSentinel;
        }
        if (c->base == TL4_NO_BASE) break;
        ordinal = c->base;
    }
    return NULL;
}

/* ---- getters: one TL4 call, one range check, one result --------------------------------------------------- */

static HRESULT range_ok(const tl4_prop *p, const int32_t *values, uint32_t count)
{
    return tl4_range_check(p->type, p->name_id, values, count) == TL4_OK ? S_OK : E_PROP_ID_UNSUPPORTED;
}

HRESULT MSSTYLES_V4_GetBool(PTHEME_PROPERTY tp, BOOL *pfVal)
{
    PV4_FILE v4 = v4_of(tp); const tl4_prop *p = prop_of(tp); int32_t v = 0;
    if (!v4 || !p || !pfVal) return E_INVALIDARG;
    *pfVal = FALSE;
    if (tl4_get_bool(&v4->model, p, &v) != TL4_OK || FAILED(range_ok(p, &v, 1))) return E_PROP_ID_UNSUPPORTED;
    *pfVal = v != 0;
    return S_OK;
}

HRESULT MSSTYLES_V4_GetColor(PTHEME_PROPERTY tp, COLORREF *pColor)
{
    PV4_FILE v4 = v4_of(tp); const tl4_prop *p = prop_of(tp); uint32_t c = 0; int32_t v;
    if (!v4 || !p || !pColor) return E_INVALIDARG;
    *pColor = 0;
    if (tl4_get_color(&v4->model, p, &c) != TL4_OK) return E_PROP_ID_UNSUPPORTED;
    v = (int32_t)c;
    if (FAILED(range_ok(p, &v, 1))) return E_PROP_ID_UNSUPPORTED;
    *pColor = c;
    return S_OK;
}

HRESULT MSSTYLES_V4_GetEnum(PTHEME_PROPERTY tp, int *piVal)
{
    PV4_FILE v4 = v4_of(tp); const tl4_prop *p = prop_of(tp); int32_t v = 0;
    if (!v4 || !p || !piVal) return E_INVALIDARG;
    if (tl4_get_enum(&v4->model, p, &v) != TL4_OK || FAILED(range_ok(p, &v, 1))) return E_PROP_ID_UNSUPPORTED;
    *piVal = v;
    return S_OK;
}

HRESULT MSSTYLES_V4_GetInt(PTHEME_PROPERTY tp, int *piVal)
{
    PV4_FILE v4 = v4_of(tp); const tl4_prop *p = prop_of(tp); int32_t v = 0;
    if (!v4 || !p || !piVal) return E_INVALIDARG;
    if (tl4_get_int(&v4->model, p, &v) != TL4_OK || FAILED(range_ok(p, &v, 1))) return E_PROP_ID_UNSUPPORTED;
    *piVal = v;
    return S_OK;
}

HRESULT MSSTYLES_V4_GetIntList(PTHEME_PROPERTY tp, INTLIST *pIntList)
{
    PV4_FILE v4 = v4_of(tp); const tl4_prop *p = prop_of(tp);
    int32_t values[TL4_V4_MAX_INTLIST]; uint32_t count = 0, i;
    if (!v4 || !p || !pIntList) return E_INVALIDARG;
    pIntList->iValueCount = 0;
    if (tl4_get_intlist(&v4->model, p, values, TL4_V4_MAX_INTLIST, &count) != TL4_OK || FAILED(range_ok(p, values, count))) return E_PROP_ID_UNSUPPORTED;
    if (count > MAX_INTLIST_COUNT) return E_PROP_ID_UNSUPPORTED;
    for (i = 0; i < count; i++) pIntList->iValues[i] = values[i];
    pIntList->iValueCount = (int)count;
    return S_OK;
}

HRESULT MSSTYLES_V4_GetPosition(PTHEME_PROPERTY tp, POINT *pPoint)
{
    PV4_FILE v4 = v4_of(tp); const tl4_prop *p = prop_of(tp); int32_t xy[2] = { 0, 0 };
    if (!v4 || !p || !pPoint) return E_INVALIDARG;
    if (tl4_get_position(&v4->model, p, xy) != TL4_OK || FAILED(range_ok(p, xy, 2))) return E_PROP_ID_UNSUPPORTED;
    pPoint->x = xy[0]; pPoint->y = xy[1];
    return S_OK;
}

HRESULT MSSTYLES_V4_GetRect(PTHEME_PROPERTY tp, RECT *pRect)
{
    PV4_FILE v4 = v4_of(tp); const tl4_prop *p = prop_of(tp); int32_t r[4] = { 0, 0, 0, 0 };
    if (!v4 || !p || !pRect) return E_INVALIDARG;
    if (tl4_get_rect(&v4->model, p, r) != TL4_OK || FAILED(range_ok(p, r, 4))) return E_PROP_ID_UNSUPPORTED;
    pRect->left = r[0]; pRect->top = r[1]; pRect->right = r[2]; pRect->bottom = r[3];
    return S_OK;
}

HRESULT MSSTYLES_V4_GetMargins(PTHEME_PROPERTY tp, MARGINS *pMargins)
{
    PV4_FILE v4 = v4_of(tp); const tl4_prop *p = prop_of(tp); int32_t m[4] = { 0, 0, 0, 0 };
    if (!v4 || !p || !pMargins) return E_INVALIDARG;
    memset(pMargins, 0, sizeof(*pMargins));
    if (tl4_get_margins(&v4->model, p, m) != TL4_OK || FAILED(range_ok(p, m, 4))) return E_PROP_ID_UNSUPPORTED;
    pMargins->cxLeftWidth = m[0]; pMargins->cxRightWidth = m[1]; pMargins->cyTopHeight = m[2]; pMargins->cyBottomHeight = m[3];
    return S_OK;
}

HRESULT MSSTYLES_V4_GetString(PTHEME_PROPERTY tp, LPWSTR pszBuff, int cchMaxBuffChars)
{
    PV4_FILE v4 = v4_of(tp); const tl4_prop *p = prop_of(tp); uint32_t chars = 0; tl4_status st;
    if (!v4 || !p || !pszBuff || cchMaxBuffChars < 1) return E_INVALIDARG;
    pszBuff[0] = 0;
    st = tl4_get_string(&v4->model, p, (uint16_t *)pszBuff, (uint32_t)cchMaxBuffChars, &chars);
    if (st == TL4_E_BUFFER) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);   /* never truncated (R-GEN-5) */
    if (st != TL4_OK || tl4_range_check_string(chars) != TL4_OK) { pszBuff[0] = 0; return E_PROP_ID_UNSUPPORTED; }
    return S_OK;
}

HRESULT MSSTYLES_V4_GetFont(PTHEME_PROPERTY tp, HDC hdc, LOGFONTW *pFont)
{
    PV4_FILE v4 = v4_of(tp); const tl4_prop *p = prop_of(tp); tl4_font f; uint32_t i;
    if (!v4 || !p || !pFont) return E_INVALIDARG;
    ZeroMemory(pFont, sizeof(*pFont));
    if (tl4_get_font(&v4->model, p, &f) != TL4_OK || tl4_range_check_font(&f) != TL4_OK) return E_PROP_ID_UNSUPPORTED;
    for (i = 0; i < f.face_chars && i < LF_FACESIZE - 1; i++) pFont->lfFaceName[i] = f.face[i];
    pFont->lfFaceName[i] = 0;
    pFont->lfHeight = -MulDiv(f.points, hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96, 72);
    pFont->lfWeight = f.bold ? FW_BOLD : FW_NORMAL;
    pFont->lfItalic = f.italic ? TRUE : FALSE;
    pFont->lfUnderline = f.underline ? TRUE : FALSE;
    pFont->lfStrikeOut = f.strikeout ? TRUE : FALSE;
    pFont->lfCharSet = DEFAULT_CHARSET;
    pFont->lfQuality = (BYTE)f.quality;       /* the same numbering as LOGFONT (model.h) */
    return S_OK;
}

HRESULT MSSTYLES_V4_GetImageName(PTHEME_PROPERTY tp, LPWSTR pszBuff, int cchMaxBuffChars)
{
    PV4_FILE v4 = v4_of(tp); const tl4_prop *p = prop_of(tp); uint32_t id = 0, off = 0, size = 0; tl4_status st;
    if (!v4 || !p || !pszBuff || cchMaxBuffChars < 12) return E_INVALIDARG;
    pszBuff[0] = 0;
    if (p->type == TL4_V4_FILENAME) st = tl4_get_filename(&v4->model, p, &id, &off, &size);
    else if (p->type == TL4_V4_DISKSTREAM) st = tl4_get_diskstream(&v4->model, p, &id, &off, &size);
    else return E_PROP_ID_UNSUPPORTED;
    if (st != TL4_OK || id > 0xFFFF) return E_PROP_ID_UNSUPPORTED;
    swprintf(pszBuff, (size_t)cchMaxBuffChars, L"#%u", id);
    return S_OK;
}

BOOL MSSTYLES_V4_ImageData(PTHEME_FILE tf, UINT id, const BYTE **ppData, UINT *pcbData)
{
    PV4_FILE v4;
    tl4_res_name t, n;
    tl4_resource res;
    if (ppData) *ppData = NULL;
    if (pcbData) *pcbData = 0;
    if (!tf || !tf->v4 || !ppData || !pcbData || id > 0xFFFF) return FALSE;
    v4 = (PV4_FILE)tf->v4;
    t = tl4_res_ascii("IMAGE"); n = tl4_res_id(id);
    if (tl4_pe_find(&v4->pe, &t, &n, &res) != TL4_OK) return FALSE;
    *ppData = res.data;         /* inside the view: the reader re-checked the range (R-PE-9) */
    *pcbData = res.size;
    return TRUE;
}
