/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Ribbon control F1: layout tree, group variants, collections, QAT, app menu, keytips, tooltips
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 */

#include <windows.h>
#include <windowsx.h>
#include <oleacc.h>
#include <new>
#include <leewayui/ribbon.h>
#include <leewayui/theme.h>
#include <leewayui/dpi.h>
#include <string.h>

#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif

#define RIBBON_CLASS    L"LeewayUIRibbon"
#define POPUP_CLASS     L"LeewayUIRibbonPopup"
#define TIP_CLASS       L"LeewayUIRibbonTip"
#define RIBBON_CTRL_ID  0x4C57
#define TIMER_TIP       1
#define MAX_KEYTIP      4
#define MAX_CONTEXTS    16
#define MAX_QAT         32
#define MAX_RECENT      16
#define MAX_GROUPS      32

/* Logical metrics at 96 dpi; everything is scaled through LeewayDpiScale. */
enum {
    M_QAT_H = 24, M_STRIP_H = 26, M_BAND_H = 92, M_PAD = 3,
    M_ICON_L = 32, M_ICON_S = 16, M_LARGE_H = 66, M_LARGE_MIN_W = 46, M_SMALL_H = 22,
    M_GROUP_LABEL_H = 16, M_TAB_PAD = 11, M_APPBTN_W = 52, M_ARROW_W = 12, M_SEP_W = 6,
    M_MENU_ROW_H = 24, M_APPMENU_ROW_H = 40, M_APPMENU_W = 240, M_RECENT_W = 200, M_GALLERY_CELL_W = 76, M_GALLERY_CELL_H = 26,
    M_STRIP_BTN = 20, M_KEYTIP_H = 14, M_TIP_PAD = 6, M_LAUNCHER = 12,
    M_CELL = 22, M_COMBO_W = 110, M_STRIP_SCROLL_W = 14, M_CHECK = 13, M_COMBO_LIST_H = 200, M_SWATCH_L = 26,
};
#define MAX_HOSTED 16
#define MAX_STRIPS 16

/* ------------------------------------------------------------------ */
/* Small allocation arena: every Set* copies its input into one of these */

struct Arena {
    void **blocks;
    UINT32 n, cap;
};

static void *ArenaAlloc(Arena *a, SIZE_T size)
{
    if (a->n == a->cap) {
        UINT32 cap = a->cap ? a->cap * 2 : 16;
        void **nb = (void **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(void *));
        if (!nb) return NULL;
        if (a->blocks) {
            CopyMemory(nb, a->blocks, a->n * sizeof(void *));
            HeapFree(GetProcessHeap(), 0, a->blocks);
        }
        a->blocks = nb;
        a->cap = cap;
    }
    void *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
    if (p) a->blocks[a->n++] = p;
    return p;
}

static WCHAR *ArenaStr(Arena *a, const WCHAR *s)
{
    if (!s) return NULL;
    SIZE_T n = (lstrlenW(s) + 1) * sizeof(WCHAR);
    WCHAR *d = (WCHAR *)ArenaAlloc(a, n);
    if (d) CopyMemory(d, s, n);
    return d;
}

static void ArenaFree(Arena *a)
{
    for (UINT32 i = 0; i < a->n; i++) HeapFree(GetProcessHeap(), 0, a->blocks[i]);
    if (a->blocks) HeapFree(GetProcessHeap(), 0, a->blocks);
    a->blocks = NULL;
    a->n = a->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Internal model */

struct Layout {
    Arena arena;
    WCHAR *app_name;
    UINT32 tab_count;
    LeewayUITab *tabs;
    UINT32 appmenu_count;
    UINT32 *appmenu_ids;
    UINT32 qat_count;
    UINT32 *qat_default_ids;
    UINT32 help_command_id;
};

struct Collection {
    Arena arena;
    UINT32 cmd;
    UINT32 flags;
    UINT32 count;
    LeewayUICollectionItem *items;
    UINT32 last_choice;
};

struct Context {
    WCHAR name[32];
    BOOL active;
};

enum ItemKind {
    IT_GROUPFRAME, IT_APPBUTTON, IT_TAB, IT_QAT, IT_QATMORE, IT_HELP, IT_MINBTN,
    IT_CONTROL, IT_SPLITARROW, IT_GROUPBTN, IT_LAUNCHER, IT_MENUROW, IT_SEPARATOR,
    IT_GALCELL, IT_GALUP, IT_GALDOWN, IT_GALEXPAND
};

enum ItemFlags {
    IF_LARGE = 1,       /* large form: 32 px icon, label below */
    IF_LABEL = 2,       /* label drawn */
    IF_ARROW = 4,       /* dropdown arrow drawn */
    IF_CHECKMARK = 8,   /* menu row with a check column */
    IF_ICON32 = 16,     /* menu row with a 32 px icon (app menu) */
    IF_CHECKED = 32,    /* menu row currently checked */
    IF_CONTEXT = 64,    /* contextual tab */
    IF_DISABLED = 128,  /* gallery scroll button at its end */
    IF_RECENT = 256,    /* app-menu recent-files row (cmd = index; +IF_DISABLED = header) */
};

struct Item {
    ItemKind kind;
    UINT32 cmd;
    RECT rc;
    UINT32 flags;
    LeewayUIControlKind ckind;
    const LeewayUIGroup *grp;
    UINT32 tab;
    const WCHAR *keytip;
    const WCHAR *label;     /* menu rows, gallery cells and group buttons: fixed text */
    UINT32 icon_id;         /* menu rows and gallery cells: fixed icon */
    UINT32 alt_cmd;         /* gallery cells: right-click command */
    COLORREF color;         /* gallery cells and colour wells; LWUI_COLOR_NONE = none */
};

struct Hosted {             /* a COMBO hosted in the band (G14) */
    UINT32 cmd;
    HWND hwnd;
    UINT32 gen;             /* collection generation the list was filled from */
    BOOL used;
    WCHAR text[128];
};

struct StripState {         /* scroll position of an in-ribbon gallery (G16), in rows */
    UINT32 cmd;
    int offset;
};

struct Surface {
    Item *items;
    UINT32 n, cap;
    int hover, pressed, focus;
};

enum PopupKind { PK_MENU, PK_GALLERY, PK_GROUP, PK_TAB, PK_APPMENU, PK_QATMENU };

struct Popup {
    HWND hwnd;
    PopupKind kind;
    Surface s;
    UINT32 owner_cmd;
    Collection *coll;
    const LeewayUIGroup *grp;
    UINT32 tab;
    ILeewayRibbon *r;
    BOOL keytips;
    BOOL closing;
    WCHAR kt_buf[MAX_KEYTIP];
};

struct IconEntry {
    UINT32 id, size;
    LeewayUIIconVariant variant;
    COLORREF fg, bg;
    HICON icon;
};

struct ILeewayRibbon {
    HWND hwnd;
    HWND parent;
    LeewayUICommand *table;
    UINT32 count;
    Arena table_arena;
    LeewayUICommandExecuteHandler execute;
    LeewayUICommandStateHandler state;
    void *ctx;

    LeewayUIRecentOpenHandler recent_open;
    void *recent_ctx;
    UINT32 recent_max;
    UINT32 recent_count;
    WCHAR recent_paths[MAX_RECENT][MAX_PATH];
    const WCHAR *recent_names[MAX_RECENT];

    ILeewayTheme *theme;
    UINT32 dpi;
    HFONT font, font_bold, font_big;
    HBRUSH brush[LWUI_COLOR_COUNT];

    Layout *layout;
    UINT32 qat_ids[MAX_QAT];
    UINT32 qat_count;
    BOOL qat_below;

    Collection **colls;
    UINT32 coll_count, coll_cap;
    Context contexts[MAX_CONTEXTS];
    UINT32 context_count;

    UINT32 active_tab;      /* index; 0xFFFFFFFF = none */
    UINT32 restore_tab;
    BOOL minimized;

    Surface s;
    BOOL dirty;
    UINT32 variants[MAX_GROUPS];

    Popup *popup;

    int kt_level;           /* 0 off, 1 strip level, 2 active tab's controls */
    WCHAR kt_buf[MAX_KEYTIP];
    BOOL alt_pending;
    BOOL tracking;

    HWND tip;
    int tip_item;
    WCHAR tip_text[256];

    ACCEL *accels;
    UINT32 accel_count;

    LeewayUIIconResolver resolver;
    void *resolver_ctx;
    LeewayUIGalleryDrawHandler gallery_draw;
    void *gallery_draw_ctx;
    IconEntry *icons;
    UINT32 icon_count, icon_cap;

    Hosted hosted[MAX_HOSTED];
    UINT32 hosted_count;
    UINT32 coll_gen;
    StripState strips[MAX_STRIPS];
    UINT32 strip_count;
};

/* ------------------------------------------------------------------ */
/* Helpers */

static void Exec(ILeewayRibbon *r, UINT32 id);
static void Relayout(ILeewayRibbon *r);
static LRESULT AccessibleFromWindow(HWND hwnd, BOOL is_popup, WPARAM wp);

static inline int Sc(ILeewayRibbon *r, int v) { return LeewayDpiScale(r->dpi, v); }
static inline COLORREF Col(ILeewayRibbon *r, LeewayThemeColor c) { return LeewayThemeGetColor(r->theme, c); }

static HBRUSH Br(ILeewayRibbon *r, LeewayThemeColor c)
{
    if (!r->brush[c]) r->brush[c] = CreateSolidBrush(Col(r, c));
    return r->brush[c];
}

static void DropGdi(ILeewayRibbon *r)
{
    for (int i = 0; i < LWUI_COLOR_COUNT; i++) {
        if (r->brush[i]) { DeleteObject(r->brush[i]); r->brush[i] = NULL; }
    }
    if (r->font) { DeleteObject(r->font); r->font = NULL; }
    if (r->font_bold) { DeleteObject(r->font_bold); r->font_bold = NULL; }
    if (r->font_big) { DeleteObject(r->font_big); r->font_big = NULL; }
}

static void ClearIcons(ILeewayRibbon *r)
{
    for (UINT32 i = 0; i < r->icon_count; i++) {
        if (r->icons[i].icon) DestroyIcon(r->icons[i].icon);
    }
    r->icon_count = 0;
}

static void CreateFonts(ILeewayRibbon *r)
{
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        lf = ncm.lfMessageFont;
    } else {
        lstrcpyW(lf.lfFaceName, L"MS Shell Dlg 2");
    }
    lf.lfHeight = -MulDiv(9, (int)r->dpi, 72);
    lf.lfWidth = 0;
    lf.lfWeight = FW_NORMAL;
    r->font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_BOLD;
    r->font_bold = CreateFontIndirectW(&lf);
    lf.lfHeight = -MulDiv(14, (int)r->dpi, 72);
    r->font_big = CreateFontIndirectW(&lf);
}

static const LeewayUICommand *FindCommand(ILeewayRibbon *r, UINT32 id)
{
    for (UINT32 i = 0; i < r->count; i++) {
        if (r->table[i].id == id) return &r->table[i];
    }
    return NULL;
}

static BOOL IsFrameworkId(UINT32 id)
{
    return id >= LWUI_APPMENU && id <= LWUI_STATUSBAR_TOGGLE;
}

static const WCHAR *FrameworkLabel(UINT32 id)
{
    switch (id) {
    case LWUI_APPMENU: return L"File";
    case LWUI_QAT_CUSTOMIZE: return L"Customize Quick Access Toolbar";
    case LWUI_RIBBON_MINIMIZE: return L"Minimize the Ribbon";
    case LWUI_HELP: return L"Help";
    case LWUI_STATUSBAR_TOGGLE: return L"Status Bar";
    }
    return L"";
}

/* Fill a state from the table (or the framework defaults), then let the app override. */
static void QueryState(ILeewayRibbon *r, UINT32 id, LeewayUICommandState *st, const WCHAR **label, const WCHAR **tooltip, UINT32 *icon_id)
{
    const LeewayUICommand *c = FindCommand(r, id);
    st->enabled = TRUE;
    st->checked = FALSE;
    st->visible = TRUE;
    st->label = NULL;
    st->tooltip = NULL;
    st->color = LWUI_COLOR_NONE;
    if (c) {
        st->enabled = c->enabled;
        st->checked = c->checked;
        st->visible = c->visible;
    }
    if (id == LWUI_RIBBON_MINIMIZE) {
        st->checked = r->minimized;
        st->enabled = TRUE;
        st->visible = TRUE;
    } else if (id == LWUI_APPMENU || id == LWUI_QAT_CUSTOMIZE) {
        st->enabled = TRUE;
        st->visible = TRUE;
    } else if (r->state) {
        LeewayUICommandState tmp = *st;
        if (r->state(id, &tmp, r->ctx) == S_OK) *st = tmp;
    }
    if (label) *label = st->label ? st->label : (c && c->label ? c->label : FrameworkLabel(id));
    if (tooltip) *tooltip = st->tooltip ? st->tooltip : (c && c->tooltip ? c->tooltip : (st->label ? st->label : (c && c->label ? c->label : FrameworkLabel(id))));
    if (icon_id) *icon_id = c ? c->icon_id : LWUI_ICON_NONE;
}

static int TextWidth(HDC hdc, const WCHAR *s)
{
    SIZE sz = {0, 0};
    if (!s || !*s) return 0;
    GetTextExtentPoint32W(hdc, s, lstrlenW(s), &sz);
    return sz.cx;
}

static Collection *FindCollection(ILeewayRibbon *r, UINT32 cmd)
{
    for (UINT32 i = 0; i < r->coll_count; i++) {
        if (r->colls[i]->cmd == cmd) return r->colls[i];
    }
    return NULL;
}

static BOOL TabVisible(ILeewayRibbon *r, UINT32 index)
{
    if (!r->layout || index >= r->layout->tab_count) return FALSE;
    const WCHAR *ctx = r->layout->tabs[index].context;
    if (!ctx) return TRUE;
    for (UINT32 i = 0; i < r->context_count; i++) {
        if (lstrcmpiW(r->contexts[i].name, ctx) == 0) return r->contexts[i].active;
    }
    return FALSE;
}

static UINT32 FirstVisibleTab(ILeewayRibbon *r)
{
    if (!r->layout) return 0xFFFFFFFF;
    for (UINT32 i = 0; i < r->layout->tab_count; i++) {
        if (TabVisible(r, i)) return i;
    }
    return 0xFFFFFFFF;
}

static BOOL IsLargeKind(LeewayUIControlKind k)
{
    return k == LWUI_CONTROL_BUTTON_LARGE || k == LWUI_CONTROL_TOGGLE_LARGE || k == LWUI_CONTROL_SPLIT_LARGE;
}

static BOOL HasArrow(LeewayUIControlKind k)
{
    return k == LWUI_CONTROL_DROPDOWN || k == LWUI_CONTROL_GALLERY || k == LWUI_CONTROL_COMBO ||
           k == LWUI_CONTROL_SPLIT_LARGE || k == LWUI_CONTROL_SPLIT_SMALL || k == LWUI_CONTROL_SPLIT_SMALL_ICON;
}

static BOOL IsSplitKind(LeewayUIControlKind k)
{
    return k == LWUI_CONTROL_SPLIT_LARGE || k == LWUI_CONTROL_SPLIT_SMALL || k == LWUI_CONTROL_SPLIT_SMALL_ICON;
}

static BOOL IsToggleKind(LeewayUIControlKind k)
{
    return k == LWUI_CONTROL_TOGGLE_LARGE || k == LWUI_CONTROL_TOGGLE_SMALL || k == LWUI_CONTROL_CHECKBOX ||
           k == LWUI_CONTROL_COLORWELL;
}

static int StripOffset(ILeewayRibbon *r, UINT32 cmd)
{
    for (UINT32 i = 0; i < r->strip_count; i++) if (r->strips[i].cmd == cmd) return r->strips[i].offset;
    return 0;
}

static void SetStripOffset(ILeewayRibbon *r, UINT32 cmd, int offset)
{
    for (UINT32 i = 0; i < r->strip_count; i++) {
        if (r->strips[i].cmd == cmd) { r->strips[i].offset = offset; return; }
    }
    if (r->strip_count < MAX_STRIPS) {
        r->strips[r->strip_count].cmd = cmd;
        r->strips[r->strip_count].offset = offset;
        r->strip_count++;
    }
}

static Hosted *FindHosted(ILeewayRibbon *r, UINT32 cmd)
{
    for (UINT32 i = 0; i < r->hosted_count; i++) if (r->hosted[i].cmd == cmd) return &r->hosted[i];
    return NULL;
}

static Hosted *FindHostedByWindow(ILeewayRibbon *r, HWND hwnd)
{
    for (UINT32 i = 0; i < r->hosted_count; i++) {
        if (r->hosted[i].hwnd && (r->hosted[i].hwnd == hwnd || IsChild(r->hosted[i].hwnd, hwnd))) return &r->hosted[i];
    }
    return NULL;
}

static void SetComboText(ILeewayRibbon *r, UINT32 cmd, const WCHAR *text)
{
    Hosted *h = FindHosted(r, cmd);
    if (!h) {
        if (r->hosted_count == MAX_HOSTED) return;
        h = &r->hosted[r->hosted_count++];
        ZeroMemory(h, sizeof(*h));
        h->cmd = cmd;
    }
    lstrcpynW(h->text, text ? text : L"", 128);
    if (h->hwnd) SetWindowTextW(h->hwnd, h->text);
}

/* Variant walk per group (G11) */
static UINT32 VariantOrder(const LeewayUIGroup *g)
{
    if (g->variant_order) return g->variant_order;
    BOOL all_large = TRUE;
    for (UINT32 i = 0; i < g->control_count; i++) {
        if (!IsLargeKind(g->controls[i].kind)) { all_large = FALSE; break; }
    }
    return all_large ? LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, 0, 0)
                     : LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED);
}

static UINT32 VariantAt(const LeewayUIGroup *g, UINT32 step)
{
    UINT32 order = VariantOrder(g);
    if (step > 3) return LWUI_VARIANT_NONE;
    return (order >> (8 * step)) & 0xFF;
}

static void NotifyParent(ILeewayRibbon *r, UINT code)
{
    NMHDR nm;
    nm.hwndFrom = r->hwnd;
    nm.idFrom = RIBBON_CTRL_ID;
    nm.code = code;
    if (r->parent) SendMessageW(r->parent, WM_NOTIFY, (WPARAM)RIBBON_CTRL_ID, (LPARAM)&nm);
}

/* ------------------------------------------------------------------ */
/* Surfaces (item lists) */

static Item *SurfaceAdd(Surface *s)
{
    if (s->n == s->cap) {
        UINT32 cap = s->cap ? s->cap * 2 : 64;
        Item *ni = (Item *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(Item));
        if (!ni) return NULL;
        if (s->items) {
            CopyMemory(ni, s->items, s->n * sizeof(Item));
            HeapFree(GetProcessHeap(), 0, s->items);
        }
        s->items = ni;
        s->cap = cap;
    }
    Item *it = &s->items[s->n++];
    ZeroMemory(it, sizeof(*it));
    return it;
}

static void SurfaceReset(Surface *s)
{
    s->n = 0;
    s->hover = s->pressed = -1;
}

static void SurfaceFree(Surface *s)
{
    if (s->items) HeapFree(GetProcessHeap(), 0, s->items);
    ZeroMemory(s, sizeof(*s));
    s->hover = s->pressed = s->focus = -1;
}

static int HitTest(Surface *s, int x, int y)
{
    POINT pt = {x, y};
    for (int i = (int)s->n - 1; i >= 0; i--) {
        if (s->items[i].kind == IT_GROUPFRAME || s->items[i].kind == IT_SEPARATOR) continue;
        if (PtInRect(&s->items[i].rc, pt)) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Band layout: groups, columns, variants */

static int LargeWidth(ILeewayRibbon *r, HDC hdc, const WCHAR *label)
{
    int w = TextWidth(hdc, label) + Sc(r, 12);
    int m = Sc(r, M_LARGE_MIN_W);
    return w > m ? w : m;
}

static int SmallWidth(ILeewayRibbon *r, HDC hdc, const WCHAR *label, BOOL with_label, BOOL arrow)
{
    int w = Sc(r, M_ICON_S) + Sc(r, 6);
    if (with_label) w += TextWidth(hdc, label) + Sc(r, 6);
    if (arrow) w += Sc(r, M_ARROW_W);
    return w;
}

/* In-ribbon gallery strip (G16): three rows of cells plus a scroll/expand column. Returns the width. */
static int PackStrip(ILeewayRibbon *r, Surface *surf, const LeewayUIPlacement *p, int x, int top, int large_h, const LeewayUIGroup *g, UINT32 tab)
{
    int cell = Sc(r, M_CELL);
    const int rows = 3;
    int row_h = large_h / rows;
    int scroll_w = Sc(r, M_STRIP_SCROLL_W);
    int width = p->width ? Sc(r, (int)p->width) : cell * 3 + scroll_w;
    int cols = (width - scroll_w) / cell;
    if (cols < 1) cols = 1;
    width = cols * cell + scroll_w;
    Collection *c = FindCollection(r, p->command_id);
    BOOL colors = c && (c->flags & LWUI_COLLECTION_COLORS);
    UINT32 grid_n = 0;
    for (UINT32 i = 0; c && i < c->count; i++) {
        if (!(colors && c->items[i].color == LWUI_COLOR_NONE)) grid_n++;
    }
    int total_rows = ((int)grid_n + cols - 1) / cols;
    int off = StripOffset(r, p->command_id);
    if (off > total_rows - rows) off = total_rows - rows;
    if (off < 0) off = 0;
    if (!surf) return width;

    UINT32 gi = 0;
    for (UINT32 i = 0; c && i < c->count; i++) {
        const LeewayUICollectionItem *ci = &c->items[i];
        if (colors && ci->color == LWUI_COLOR_NONE) continue;
        int row = (int)(gi / cols), col = (int)(gi % cols);
        gi++;
        int vr = row - off;
        if (vr < 0 || vr >= rows) continue;
        Item *it = SurfaceAdd(surf);
        if (!it) return width;
        it->kind = IT_GALCELL;
        it->cmd = ci->id;
        it->alt_cmd = ci->alt_id;
        it->color = colors ? ci->color : LWUI_COLOR_NONE;
        it->icon_id = ci->icon_id;
        it->label = ci->label;
        it->grp = g;
        it->tab = tab;
        SetRect(&it->rc, x + col * cell, top + vr * row_h, x + (col + 1) * cell, top + (vr + 1) * row_h);
    }
    int sx = x + cols * cell;
    Item *up = SurfaceAdd(surf);
    if (up) {
        up->kind = IT_GALUP;
        up->cmd = p->command_id;
        up->grp = g;
        up->tab = tab;
        up->flags = off > 0 ? 0 : IF_DISABLED;
        SetRect(&up->rc, sx, top, sx + scroll_w, top + row_h);
    }
    Item *down = SurfaceAdd(surf);
    if (down) {
        down->kind = IT_GALDOWN;
        down->cmd = p->command_id;
        down->grp = g;
        down->tab = tab;
        down->flags = (off + rows < total_rows) ? 0 : IF_DISABLED;
        SetRect(&down->rc, sx, top + row_h, sx + scroll_w, top + 2 * row_h);
    }
    Item *ex = SurfaceAdd(surf);
    if (ex) {
        ex->kind = IT_GALEXPAND;
        ex->cmd = p->command_id;
        ex->grp = g;
        ex->tab = tab;
        ex->keytip = p->keytip;
        SetRect(&ex->rc, sx, top + 2 * row_h, sx + scroll_w, top + large_h);
    }
    return width;
}

/* Lays out one group at a variant. surf == NULL measures only. Returns the group width. */
static int PackGroup(ILeewayRibbon *r, HDC hdc, const LeewayUIGroup *g, UINT32 variant, Surface *surf, int ox, int oy, UINT32 tab, int band_h)
{
    int pad = Sc(r, M_PAD);
    int large_h = Sc(r, M_LARGE_H);
    int rowcap = (g->rows >= 2 && g->rows <= 3) ? (int)g->rows : 3;
    int small_h = large_h / rowcap;   /* 3 rows -> M_SMALL_H (unchanged); 2 rows -> taller (Win7) */
    int x = ox + pad;
    int top = oy + pad;
    int arrow_w = Sc(r, M_ARROW_W);
    int frame_index = -1;

    if (surf) {
        Item *f = SurfaceAdd(surf);
        if (f) {
            f->kind = IT_GROUPFRAME;
            f->grp = g;
            f->tab = tab;
            f->label = g->label;
            frame_index = (int)surf->n - 1;
        }
    }

    if (variant == LWUI_VARIANT_COLLAPSED) {
        int w = LargeWidth(r, hdc, g->label) + arrow_w;
        if (surf) {
            Item *it = SurfaceAdd(surf);
            if (it) {
                it->kind = IT_GROUPBTN;
                it->grp = g;
                it->tab = tab;
                it->label = g->label;
                it->keytip = g->keytip;
                it->flags = IF_LARGE | IF_LABEL | IF_ARROW;
                SetRect(&it->rc, x, top, x + w, top + large_h);
            }
        }
        x += w;
    } else {
        int col_x = x, col_w = 0, rows = 0;
        for (UINT32 i = 0; i < g->control_count; i++) {
            const LeewayUIPlacement *p = &g->controls[i];
            LeewayUICommandState st;
            const WCHAR *label;
            QueryState(r, p->command_id, &st, &label, NULL, NULL);
            if (!st.visible) continue;
            LeewayUIControlKind kind = p->kind;
            BOOL strip = (kind == LWUI_CONTROL_GALLERY_STRIP) && variant == LWUI_VARIANT_LARGE;
            if (kind == LWUI_CONTROL_GALLERY_STRIP && !strip) kind = LWUI_CONTROL_GALLERY;   /* degrade to the dropdown form */
            BOOL large = (variant == LWUI_VARIANT_LARGE) && (IsLargeKind(kind) || kind == LWUI_CONTROL_COLORWELL);
            if (large || strip) {
                if (rows) { x = col_x + col_w + Sc(r, 2); rows = 0; col_w = 0; }
                int w;
                if (strip) {
                    w = PackStrip(r, surf, p, x, top, large_h, g, tab);
                    x += w + Sc(r, 2);
                    col_x = x;
                    continue;
                }
                w = LargeWidth(r, hdc, label);
                if (HasArrow(kind) && !IsSplitKind(kind)) w += arrow_w;
                if (surf) {
                    Item *it = SurfaceAdd(surf);
                    if (it) {
                        it->kind = IT_CONTROL;
                        it->cmd = p->command_id;
                        it->ckind = kind;
                        it->grp = g;
                        it->tab = tab;
                        it->keytip = p->keytip;
                        it->color = st.color;
                        it->flags = IF_LARGE | IF_LABEL | (HasArrow(kind) ? IF_ARROW : 0);
                        SetRect(&it->rc, x, top, x + w, top + large_h);
                        if (IsSplitKind(kind)) {
                            /* primary = icon area, menu = label + arrow area */
                            int split_y = top + Sc(r, M_ICON_L) + Sc(r, 6);
                            it->rc.bottom = split_y;
                            it->flags = IF_LARGE;
                            Item *ar = SurfaceAdd(surf);
                            if (ar) {
                                ar->kind = IT_SPLITARROW;
                                ar->cmd = p->command_id;
                                ar->ckind = kind;
                                ar->grp = g;
                                ar->tab = tab;
                                ar->flags = IF_LARGE | IF_LABEL | IF_ARROW;
                                SetRect(&ar->rc, x, split_y, x + w, top + large_h);
                            }
                        }
                    }
                }
                x += w + Sc(r, 2);
                col_x = x;
            } else {
                BOOL with_label = (variant != LWUI_VARIANT_SMALL) && kind != LWUI_CONTROL_BUTTON_SMALL_ICON && kind != LWUI_CONTROL_SPLIT_SMALL_ICON;
                if (kind == LWUI_CONTROL_CHECKBOX) with_label = TRUE;
                BOOL arrow = HasArrow(kind);
                int w;
                if (kind == LWUI_CONTROL_COMBO) w = Sc(r, p->width ? (int)p->width : M_COMBO_W);
                else if (kind == LWUI_CONTROL_CHECKBOX) w = Sc(r, M_CHECK) + Sc(r, 10) + TextWidth(hdc, label) + Sc(r, 4);
                else w = SmallWidth(r, hdc, label, with_label, arrow);
                if (rows == rowcap) { x = col_x + col_w + Sc(r, 2); col_x = x; rows = 0; col_w = 0; }
                if (rows == 0) col_x = x;
                if (surf) {
                    Item *it = SurfaceAdd(surf);
                    if (it) {
                        it->kind = IT_CONTROL;
                        it->cmd = p->command_id;
                        it->ckind = kind;
                        it->grp = g;
                        it->tab = tab;
                        it->keytip = p->keytip;
                        it->color = st.color;
                        it->flags = (with_label ? IF_LABEL : 0) | (arrow ? IF_ARROW : 0);
                        if (kind == LWUI_CONTROL_COMBO) it->flags = IF_LABEL | IF_ARROW;
                        SetRect(&it->rc, col_x, top + rows * small_h, col_x + w, top + (rows + 1) * small_h);
                        if (IsSplitKind(kind)) {
                            it->rc.right -= arrow_w;
                            it->flags &= ~IF_ARROW;
                            Item *ar = SurfaceAdd(surf);
                            if (ar) {
                                ar->kind = IT_SPLITARROW;
                                ar->cmd = p->command_id;
                                ar->ckind = kind;
                                ar->grp = g;
                                ar->tab = tab;
                                ar->flags = IF_ARROW;
                                SetRect(&ar->rc, col_x + w - arrow_w, it->rc.top, col_x + w, it->rc.bottom);
                            }
                        }
                    }
                }
                if (w > col_w) col_w = w;
                rows++;
            }
        }
        if (rows) x = col_x + col_w + Sc(r, 2);
        if (variant != LWUI_VARIANT_COLLAPSED && g->launcher_id && surf) {
            Item *it = SurfaceAdd(surf);
            if (it) {
                int l = Sc(r, M_LAUNCHER);
                it->kind = IT_LAUNCHER;
                it->cmd = g->launcher_id;
                it->grp = g;
                it->tab = tab;
                SetRect(&it->rc, x - l - 1 + pad, oy + band_h - pad - l - 1, x - 1 + pad, oy + band_h - pad - 1);
            }
        }
    }

    int width = x - ox + pad;
    int min_w = TextWidth(hdc, g->label) + Sc(r, 10);
    if (width < min_w) width = min_w;
    if (surf && frame_index >= 0) {
        SetRect(&surf->items[frame_index].rc, ox, oy, ox + width, oy + band_h);
    }
    return width;
}

/* Lays out a whole tab into surf within rc, choosing group variants to fit (G11). */
static void LayoutBand(ILeewayRibbon *r, HDC hdc, Surface *surf, UINT32 tab, const RECT *rc, UINT32 *variants)
{
    if (!r->layout || tab >= r->layout->tab_count) return;
    const LeewayUITab *t = &r->layout->tabs[tab];
    UINT32 n = t->group_count;
    if (n > MAX_GROUPS) n = MAX_GROUPS;
    UINT32 step[MAX_GROUPS];
    int widths[MAX_GROUPS];
    int sep = Sc(r, M_SEP_W);
    int pad = Sc(r, M_PAD);
    int avail = (rc->right - rc->left) - 2 * pad;
    int band_h = (rc->bottom - rc->top) - 2 * pad;

    for (UINT32 i = 0; i < n; i++) step[i] = 0;
    for (;;) {
        int total = 0;
        for (UINT32 i = 0; i < n; i++) {
            widths[i] = PackGroup(r, hdc, &t->groups[i], VariantAt(&t->groups[i], step[i]), NULL, 0, 0, tab, band_h);
            total += widths[i] + (i ? sep : 0);
        }
        if (total <= avail) break;
        int pick = -1, best = -1;
        for (UINT32 i = 0; i < n; i++) {
            if (VariantAt(&t->groups[i], step[i] + 1) != LWUI_VARIANT_NONE && widths[i] > best) {
                best = widths[i];
                pick = (int)i;
            }
        }
        if (pick < 0) break;
        step[pick]++;
    }

    int x = rc->left + pad;
    for (UINT32 i = 0; i < n; i++) {
        UINT32 v = VariantAt(&t->groups[i], step[i]);
        if (variants) variants[i] = v;
        int w = PackGroup(r, hdc, &t->groups[i], v, surf, x, rc->top + pad, tab, band_h);
        x += w + sep;
    }
}

static void LayoutQatRow(ILeewayRibbon *r, Surface *surf, int y, int width)
{
    int h = Sc(r, M_QAT_H);
    int b = Sc(r, M_STRIP_BTN);
    int x = Sc(r, M_PAD) + Sc(r, 2);
    static const WCHAR *const digits[9] = {L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9"};
    for (UINT32 i = 0; i < r->qat_count; i++) {
        LeewayUICommandState st;
        QueryState(r, r->qat_ids[i], &st, NULL, NULL, NULL);
        if (!st.visible) continue;
        Item *it = SurfaceAdd(surf);
        if (!it) return;
        it->kind = IT_QAT;
        it->cmd = r->qat_ids[i];
        it->ckind = LWUI_CONTROL_BUTTON_SMALL_ICON;
        it->keytip = i < 9 ? digits[i] : NULL;
        SetRect(&it->rc, x, y + (h - b) / 2, x + b, y + (h - b) / 2 + b);
        x += b + Sc(r, 1);
    }
    Item *more = SurfaceAdd(surf);
    if (more) {
        more->kind = IT_QATMORE;
        more->cmd = LWUI_QAT_CUSTOMIZE;
        more->flags = IF_ARROW;
        SetRect(&more->rc, x + Sc(r, 2), y + (h - b) / 2, x + Sc(r, 2) + Sc(r, 14), y + (h - b) / 2 + b);
    }
    (void)width;
}

/* Hosted combo boxes (G14): the typing part is a plain EDIT child (coloured
 * through WM_CTLCOLOREDIT, so it follows the theme in every mode); the frame
 * and the arrow are drawn by the ribbon and the list is the ribbon's own menu
 * popup. Nothing depends on the system combo's theme. */
static void ComboEditRect(ILeewayRibbon *r, const RECT *rc, RECT *out)
{
    SetRect(out, rc->left + Sc(r, 3), rc->top + Sc(r, 3), rc->right - Sc(r, M_ARROW_W) - Sc(r, 2), rc->bottom - Sc(r, 3));
}

static void SyncHosted(ILeewayRibbon *r)
{
    for (UINT32 i = 0; i < r->hosted_count; i++) r->hosted[i].used = FALSE;
    for (UINT32 i = 0; i < r->s.n; i++) {
        Item *it = &r->s.items[i];
        if (it->kind != IT_CONTROL || it->ckind != LWUI_CONTROL_COMBO) continue;
        Hosted *h = FindHosted(r, it->cmd);
        if (!h) {
            if (r->hosted_count == MAX_HOSTED) continue;
            h = &r->hosted[r->hosted_count++];
            ZeroMemory(h, sizeof(*h));
            h->cmd = it->cmd;
        }
        h->used = TRUE;
        if (!h->hwnd) {
            h->hwnd = CreateWindowExW(0, L"EDIT", NULL, WS_CHILD | WS_CLIPSIBLINGS | ES_LEFT | ES_AUTOHSCROLL,
                                      0, 0, 10, 10, r->hwnd, (HMENU)(UINT_PTR)it->cmd, GetModuleHandleW(NULL), NULL);
            if (!h->hwnd) continue;
            SendMessageW(h->hwnd, EM_SETLIMITTEXT, 127, 0);
        }
        SendMessageW(h->hwnd, WM_SETFONT, (WPARAM)r->font, FALSE);
        h->gen = r->coll_gen;
        if (GetFocus() != h->hwnd) {
            LeewayUICommandState st;
            QueryState(r, it->cmd, &st, NULL, NULL, NULL);
            if (st.label) lstrcpynW(h->text, st.label, 128);
            WCHAR cur[128];
            GetWindowTextW(h->hwnd, cur, 128);
            if (lstrcmpW(cur, h->text) != 0) {
                SetWindowTextW(h->hwnd, h->text);
                SendMessageW(h->hwnd, EM_SETSEL, (WPARAM)-1, 0);   /* no selection highlight while unfocused */
            }
            EnableWindow(h->hwnd, st.enabled);
        }
        RECT er;
        ComboEditRect(r, &it->rc, &er);
        SetWindowPos(h->hwnd, NULL, er.left, er.top, er.right - er.left, er.bottom - er.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    for (UINT32 i = 0; i < r->hosted_count; i++) {
        if (!r->hosted[i].used && r->hosted[i].hwnd) ShowWindow(r->hosted[i].hwnd, SW_HIDE);
    }
}

static void DestroyHosted(ILeewayRibbon *r)
{
    for (UINT32 i = 0; i < r->hosted_count; i++) {
        if (r->hosted[i].hwnd) { DestroyWindow(r->hosted[i].hwnd); r->hosted[i].hwnd = NULL; }
    }
}

/* The combo's text changed by the user: remember it and execute the command. */
static void ComboCommitted(ILeewayRibbon *r, Hosted *h)
{
    WCHAR cur[128];
    GetWindowTextW(h->hwnd, cur, 128);
    lstrcpynW(h->text, cur, 128);
    Exec(r, h->cmd);
}

static UINT32 ComputeHeight(ILeewayRibbon *r)
{
    UINT32 h = Sc(r, M_STRIP_H);
    if (r->layout) h += Sc(r, M_QAT_H);
    if (!r->minimized && r->layout) h += Sc(r, M_BAND_H);
    return h;
}

static void LayoutMain(ILeewayRibbon *r)
{
    RECT client;
    GetClientRect(r->hwnd, &client);
    int w = client.right;
    Surface *s = &r->s;
    int keep_hover = s->hover, keep_pressed = s->pressed;
    SurfaceReset(s);
    HDC hdc = GetDC(r->hwnd);
    HFONT old = (HFONT)SelectObject(hdc, r->font);
    int y = 0;

    if (r->layout && !r->qat_below) {
        LayoutQatRow(r, s, y, w);
        y += Sc(r, M_QAT_H);
    }

    /* tab strip */
    int strip_h = Sc(r, M_STRIP_H);
    int pad = Sc(r, M_PAD);
    int x = pad;
    Item *app = SurfaceAdd(s);
    if (app) {
        app->kind = IT_APPBUTTON;
        app->cmd = LWUI_APPMENU;
        app->keytip = L"F";
        const LeewayUICommand *c = FindCommand(r, LWUI_APPMENU);
        if (c && c->keytip) app->keytip = c->keytip;
        SetRect(&app->rc, x, y + Sc(r, 2), x + Sc(r, M_APPBTN_W), y + strip_h);
        x = app->rc.right + Sc(r, 6);
    }
    if (r->layout) {
        SelectObject(hdc, r->font);
        for (UINT32 i = 0; i < r->layout->tab_count; i++) {
            if (!TabVisible(r, i)) continue;
            const LeewayUITab *t = &r->layout->tabs[i];
            int tw = TextWidth(hdc, t->label) + 2 * Sc(r, M_TAB_PAD);
            Item *it = SurfaceAdd(s);
            if (!it) break;
            it->kind = IT_TAB;
            it->tab = i;
            it->keytip = t->keytip;
            it->label = t->label;
            it->flags = t->context ? IF_CONTEXT : 0;
            SetRect(&it->rc, x, y + Sc(r, 3), x + tw, y + strip_h);
            x += tw + Sc(r, 2);
        }
    }
    int b = Sc(r, M_STRIP_BTN);
    int rx = w - pad - b;
    Item *mn = SurfaceAdd(s);
    if (mn) {
        mn->kind = IT_MINBTN;
        mn->cmd = LWUI_RIBBON_MINIMIZE;
        SetRect(&mn->rc, rx, y + (strip_h - b) / 2 + 1, rx + b, y + (strip_h - b) / 2 + 1 + b);
        rx -= b + Sc(r, 2);
    }
    Item *hp = SurfaceAdd(s);
    if (hp) {
        hp->kind = IT_HELP;
        hp->cmd = (r->layout && r->layout->help_command_id) ? r->layout->help_command_id : LWUI_HELP;
        SetRect(&hp->rc, rx, y + (strip_h - b) / 2 + 1, rx + b, y + (strip_h - b) / 2 + 1 + b);
    }
    y += strip_h;

    if (r->layout && !r->minimized && r->active_tab != 0xFFFFFFFF) {
        RECT band = {0, y, w, y + Sc(r, M_BAND_H)};
        LayoutBand(r, hdc, s, r->active_tab, &band, r->variants);
        y += Sc(r, M_BAND_H);
    }

    if (r->layout && r->qat_below) {
        LayoutQatRow(r, s, y, w);
        y += Sc(r, M_QAT_H);
    }

    SelectObject(hdc, old);
    ReleaseDC(r->hwnd, hdc);
    r->dirty = FALSE;
    SyncHosted(r);
    NotifyWinEvent(EVENT_OBJECT_REORDER, r->hwnd, OBJID_CLIENT, CHILDID_SELF);
    /* keep hover/pressed only if the indices still point at the same thing */
    s->hover = (keep_hover >= 0 && keep_hover < (int)s->n) ? keep_hover : -1;
    s->pressed = (keep_pressed >= 0 && keep_pressed < (int)s->n) ? keep_pressed : -1;
    if (s->focus >= (int)s->n) s->focus = -1;
}

static void Relayout(ILeewayRibbon *r)
{
    r->dirty = TRUE;
    if (r->hwnd) InvalidateRect(r->hwnd, NULL, FALSE);
}

static void EnsureLayout(ILeewayRibbon *r)
{
    if (r->dirty) LayoutMain(r);
}

/* ------------------------------------------------------------------ */
/* Icons */

static HICON GetIcon(ILeewayRibbon *r, UINT32 icon_id, UINT32 size, COLORREF fg, COLORREF bg)
{
    if (icon_id == LWUI_ICON_NONE || !r->resolver) return NULL;
    LeewayThemeMode mode = LeewayThemeGetMode(r->theme);
    LeewayUIIconVariant v = LWUI_ICON_VARIANT_COLOUR;
    if (mode == LEEWAY_THEME_MODE_HIGH_CONTRAST) v = LWUI_ICON_VARIANT_MONO;
    else if (mode == LEEWAY_THEME_MODE_DARK && (icon_id & LWUI_ICON_GLYPH)) v = LWUI_ICON_VARIANT_MONO;
    for (UINT32 i = 0; i < r->icon_count; i++) {
        IconEntry *e = &r->icons[i];
        if (e->id == icon_id && e->size == size && e->variant == v && e->fg == fg && e->bg == bg) return e->icon;
    }
    if (r->icon_count == r->icon_cap) {
        UINT32 cap = r->icon_cap ? r->icon_cap * 2 : 64;
        IconEntry *ni = (IconEntry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(IconEntry));
        if (!ni) return NULL;
        if (r->icons) {
            CopyMemory(ni, r->icons, r->icon_count * sizeof(IconEntry));
            HeapFree(GetProcessHeap(), 0, r->icons);
        }
        r->icons = ni;
        r->icon_cap = cap;
    }
    IconEntry *e = &r->icons[r->icon_count++];
    e->id = icon_id;
    e->size = size;
    e->variant = v;
    e->fg = fg;
    e->bg = bg;
    e->icon = r->resolver(icon_id & LWUI_ICON_ID_MASK, size, v, fg, bg, r->resolver_ctx);
    return e->icon;
}

static void DrawIconSlot(ILeewayRibbon *r, HDC hdc, int x, int y, int size, UINT32 icon_id, const WCHAR *label, BOOL enabled, COLORREF bg)
{
    HICON h = GetIcon(r, icon_id, (UINT32)size, Col(r, LWUI_COLOR_TEXT), bg);
    if (h) {
        if (enabled) DrawIconEx(hdc, x, y, h, size, size, 0, NULL, DI_NORMAL);
        else DrawStateW(hdc, NULL, NULL, (LPARAM)h, 0, x, y, size, size, DST_ICON | DSS_DISABLED);
        return;
    }
    /* placeholder glyph: rounded tile with the label's initial (G10) */
    RECT rc = {x + 1, y + 1, x + size - 1, y + size - 1};
    HBRUSH fill = Br(r, enabled ? LWUI_COLOR_PLACEHOLDER_BG : LWUI_COLOR_SEPARATOR);
    HPEN pen = CreatePen(PS_SOLID, 1, Col(r, LWUI_COLOR_GROUP_BORDER));
    HGDIOBJ ob = SelectObject(hdc, fill), op = SelectObject(hdc, pen);
    int rr = size >= 32 ? Sc(r, 6) : Sc(r, 3);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, rr, rr);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(pen);
    WCHAR initial[2] = {L'?', 0};
    if (label) {
        for (const WCHAR *p = label; *p; p++) {
            if ((*p >= L'A' && *p <= L'Z') || (*p >= L'a' && *p <= L'z') || (*p >= L'0' && *p <= L'9')) {
                initial[0] = (*p >= L'a' && *p <= L'z') ? (WCHAR)(*p - 32) : *p;
                break;
            }
        }
    }
    HFONT of = (HFONT)SelectObject(hdc, size >= 32 ? r->font_big : r->font_bold);
    SetTextColor(hdc, enabled ? Col(r, LWUI_COLOR_PLACEHOLDER_TEXT) : Col(r, LWUI_COLOR_TEXT_DISABLED));
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, initial, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, of);
}

/* ------------------------------------------------------------------ */
/* Drawing primitives */

static void VGradient(ILeewayRibbon *r, HDC hdc, const RECT *rc, COLORREF c0, COLORREF c1)
{
    int h = rc->bottom - rc->top;
    if (h <= 0) return;
    if (LeewayThemeIsFlat(r->theme) || c0 == c1) {
        HBRUSH b = CreateSolidBrush(c0);
        FillRect(hdc, rc, b);
        DeleteObject(b);
        return;
    }
    int step = h > 24 ? 2 : 1;
    for (int y = 0; y < h; y += step) {
        int t = y * 255 / (h > 1 ? h - 1 : 1);
        COLORREF c = RGB(GetRValue(c0) + (GetRValue(c1) - GetRValue(c0)) * t / 255,
                         GetGValue(c0) + (GetGValue(c1) - GetGValue(c0)) * t / 255,
                         GetBValue(c0) + (GetBValue(c1) - GetBValue(c0)) * t / 255);
        RECT row = {rc->left, rc->top + y, rc->right, rc->top + y + step};
        if (row.bottom > rc->bottom) row.bottom = rc->bottom;
        HBRUSH b = CreateSolidBrush(c);
        FillRect(hdc, &row, b);
        DeleteObject(b);
    }
}

static void FrameRound(ILeewayRibbon *r, HDC hdc, const RECT *rc, COLORREF fill, COLORREF border, int radius, BOOL has_fill)
{
    HBRUSH b = has_fill ? CreateSolidBrush(fill) : (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(hdc, b), op = SelectObject(hdc, p);
    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom, radius, radius);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(p);
    if (has_fill) DeleteObject(b);
    (void)r;
}

static void DrawArrowDown(ILeewayRibbon *r, HDC hdc, int cx, int cy, COLORREF c)
{
    int a = Sc(r, 3);
    POINT pts[3] = {{cx - a, cy - a / 2}, {cx + a + 1, cy - a / 2}, {cx, cy + a / 2 + 1}};
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(hdc, b), op = SelectObject(hdc, p);
    Polygon(hdc, pts, 3);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void DrawChevron(ILeewayRibbon *r, HDC hdc, int cx, int cy, BOOL up, COLORREF c)
{
    int a = Sc(r, 4);
    HPEN p = CreatePen(PS_SOLID, Sc(r, 2), c);
    HGDIOBJ op = SelectObject(hdc, p);
    if (up) {
        MoveToEx(hdc, cx - a, cy + a / 2, NULL); LineTo(hdc, cx, cy - a / 2); LineTo(hdc, cx + a + 1, cy + a / 2 + 1);
    } else {
        MoveToEx(hdc, cx - a, cy - a / 2, NULL); LineTo(hdc, cx, cy + a / 2); LineTo(hdc, cx + a + 1, cy - a / 2 - 1);
    }
    SelectObject(hdc, op);
    DeleteObject(p);
}

static void DrawCheck(ILeewayRibbon *r, HDC hdc, int cx, int cy, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, Sc(r, 2), c);
    HGDIOBJ op = SelectObject(hdc, p);
    int a = Sc(r, 4);
    MoveToEx(hdc, cx - a, cy, NULL);
    LineTo(hdc, cx - a / 3, cy + a * 2 / 3);
    LineTo(hdc, cx + a, cy - a * 2 / 3);
    SelectObject(hdc, op);
    DeleteObject(p);
}

static void DrawKeytip(ILeewayRibbon *r, HDC hdc, const WCHAR *tip, int cx, int cy)
{
    if (!tip || !*tip) return;
    HFONT of = (HFONT)SelectObject(hdc, r->font_bold);
    int w = TextWidth(hdc, tip) + Sc(r, 8);
    int h = Sc(r, M_KEYTIP_H);
    RECT rc = {cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h};
    FrameRound(r, hdc, &rc, Col(r, LWUI_COLOR_KEYTIP_BG), Col(r, LWUI_COLOR_KEYTIP_BORDER), Sc(r, 3), TRUE);
    SetTextColor(hdc, Col(r, LWUI_COLOR_KEYTIP_TEXT));
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, tip, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, of);
}

static void ButtonFace(ILeewayRibbon *r, HDC hdc, const RECT *rc, BOOL hover, BOOL pressed, BOOL checked)
{
    if (!hover && !pressed && !checked) return;
    LeewayThemeColor bg = pressed ? LWUI_COLOR_PRESSED_BG : (checked ? LWUI_COLOR_CHECKED_BG : LWUI_COLOR_HOVER_BG);
    LeewayThemeColor bd = pressed ? LWUI_COLOR_PRESSED_BORDER : (checked ? LWUI_COLOR_CHECKED_BORDER : LWUI_COLOR_HOVER_BORDER);
    FrameRound(r, hdc, rc, Col(r, bg), Col(r, bd), Sc(r, 4), TRUE);
}

/* ------------------------------------------------------------------ */
/* Painting */

static const WCHAR *EffectiveKeytip(ILeewayRibbon *r, const Item *it)
{
    if (it->keytip) return it->keytip;
    if (it->kind == IT_CONTROL || it->kind == IT_MENUROW || it->kind == IT_GALEXPAND) {
        const LeewayUICommand *c = FindCommand(r, it->cmd);
        if (c) return c->keytip;
    }
    return NULL;
}

static BOOL KeytipShownFor(ILeewayRibbon *r, const Item *it, BOOL popup_keytips)
{
    if (popup_keytips) return it->kind == IT_MENUROW || it->kind == IT_CONTROL || it->kind == IT_GROUPBTN || it->kind == IT_GALEXPAND;
    if (r->kt_level == 1) return it->kind == IT_APPBUTTON || it->kind == IT_TAB || it->kind == IT_QAT;
    if (r->kt_level == 2) return it->kind == IT_CONTROL || it->kind == IT_GROUPBTN || it->kind == IT_GALEXPAND;
    return FALSE;
}

static void DrawSwatch(ILeewayRibbon *r, HDC hdc, const RECT *rc, COLORREF color, BOOL enabled)
{
    HBRUSH b = CreateSolidBrush(color == LWUI_COLOR_NONE ? Col(r, LWUI_COLOR_SEPARATOR) : color);
    HPEN p = CreatePen(PS_SOLID, 1, enabled ? Col(r, LWUI_COLOR_GROUP_LABEL_TEXT) : Col(r, LWUI_COLOR_TEXT_DISABLED));
    HGDIOBJ ob = SelectObject(hdc, b), op = SelectObject(hdc, p);
    Rectangle(hdc, rc->left, rc->top, rc->right, rc->bottom);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void DrawCheckBox(ILeewayRibbon *r, HDC hdc, int x, int y, BOOL checked, BOOL enabled)
{
    int s = Sc(r, M_CHECK);
    RECT box = {x, y, x + s, y + s};
    HBRUSH b = Br(r, LWUI_COLOR_POPUP_BG);
    HPEN p = CreatePen(PS_SOLID, 1, enabled ? Col(r, LWUI_COLOR_TEXT) : Col(r, LWUI_COLOR_TEXT_DISABLED));
    HGDIOBJ ob = SelectObject(hdc, b), op = SelectObject(hdc, p);
    Rectangle(hdc, box.left, box.top, box.right, box.bottom);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(p);
    if (checked) DrawCheck(r, hdc, x + s / 2, y + s / 2, enabled ? Col(r, LWUI_COLOR_FOCUS) : Col(r, LWUI_COLOR_TEXT_DISABLED));
}

static void PaintItem(ILeewayRibbon *r, HDC hdc, Surface *s, int index, COLORREF surface_bg)
{
    Item *it = &s->items[index];
    BOOL hover = s->hover == index, pressed = s->pressed == index;
    RECT rc = it->rc;
    LeewayUICommandState st;
    const WCHAR *label = it->label;
    UINT32 icon_id = it->icon_id;
    BOOL enabled = TRUE, checked = FALSE;

    COLORREF color = it->color;
    if (it->kind == IT_CONTROL || it->kind == IT_SPLITARROW || it->kind == IT_QAT || it->kind == IT_HELP ||
        it->kind == IT_MINBTN || it->kind == IT_LAUNCHER || it->kind == IT_GALCELL ||
        (it->kind == IT_MENUROW && !(it->flags & (IF_CHECKMARK | IF_RECENT)))) {
        QueryState(r, it->cmd, &st, &label, NULL, &icon_id);
        enabled = st.enabled;
        checked = st.checked;
        if (it->label) label = it->label;
        if ((it->kind == IT_MENUROW || it->kind == IT_GALCELL) && it->icon_id) icon_id = it->icon_id;
        if (it->kind == IT_CONTROL && it->ckind == LWUI_CONTROL_COLORWELL) color = st.color;
        if (it->kind == IT_GALCELL && it->cmd == 0) { enabled = TRUE; checked = FALSE; }
    }
    if (it->kind == IT_MENUROW && (it->flags & IF_CHECKMARK)) checked = (it->flags & IF_CHECKED) != 0;

    /* a split button highlights both halves together */
    if (it->kind == IT_CONTROL || it->kind == IT_SPLITARROW) {
        for (UINT32 j = 0; j < s->n; j++) {
            if ((int)j == index) continue;
            Item *o = &s->items[j];
            if ((o->kind == IT_CONTROL || o->kind == IT_SPLITARROW) && o->cmd == it->cmd && o->grp == it->grp && IsSplitKind(it->ckind)) {
                if (s->hover == (int)j) hover = TRUE;
            }
        }
    }

    COLORREF text = enabled ? Col(r, LWUI_COLOR_TEXT) : Col(r, LWUI_COLOR_TEXT_DISABLED);
    if (pressed && enabled) text = Col(r, LWUI_COLOR_PRESSED_TEXT);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, r->font);

    switch (it->kind) {
    case IT_GROUPFRAME: {
        RECT f = rc;
        FrameRound(r, hdc, &f, 0, Col(r, LWUI_COLOR_GROUP_BORDER), Sc(r, 4), FALSE);
        RECT lb = {rc.left + 1, rc.bottom - Sc(r, M_GROUP_LABEL_H) - 1, rc.right - 1, rc.bottom - 1};
        FillRect(hdc, &lb, Br(r, LWUI_COLOR_GROUP_LABEL_BG));
        SetTextColor(hdc, Col(r, LWUI_COLOR_GROUP_LABEL_TEXT));
        DrawTextW(hdc, it->label ? it->label : L"", -1, &lb, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case IT_APPBUTTON: {
        COLORREF bg = Col(r, LWUI_COLOR_APPBUTTON_BG);
        if (hover || pressed) bg = Col(r, pressed ? LWUI_COLOR_PRESSED_BORDER : LWUI_COLOR_HOVER_BORDER);
        FrameRound(r, hdc, &rc, bg, bg, Sc(r, 4), TRUE);
        const LeewayUICommand *c = FindCommand(r, LWUI_APPMENU);
        SelectObject(hdc, r->font_bold);
        SetTextColor(hdc, Col(r, LWUI_COLOR_APPBUTTON_TEXT));
        DrawTextW(hdc, c && c->label ? c->label : L"File", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case IT_TAB: {
        BOOL active = (it->tab == r->active_tab);
        if (it->flags & IF_CONTEXT) {
            RECT tint = {rc.left, rc.top - Sc(r, 3), rc.right, rc.top};
            FillRect(hdc, &tint, Br(r, LWUI_COLOR_CONTEXT_TINT));
        }
        if (active && !r->minimized) {
            RECT a = {rc.left, rc.top, rc.right, rc.bottom + Sc(r, 2)};
            FrameRound(r, hdc, &a, Col(r, LWUI_COLOR_BAND_BG), Col(r, LWUI_COLOR_TAB_BORDER), Sc(r, 4), TRUE);
            RECT cover = {rc.left + 1, rc.bottom - 1, rc.right - 1, rc.bottom + Sc(r, 2)};
            FillRect(hdc, &cover, Br(r, LWUI_COLOR_BAND_BG));
        } else if (active || hover) {
            FrameRound(r, hdc, &rc, Col(r, LWUI_COLOR_HOVER_BG), Col(r, LWUI_COLOR_HOVER_BORDER), Sc(r, 4), TRUE);
        }
        SetTextColor(hdc, Col(r, LWUI_COLOR_TAB_TEXT));
        DrawTextW(hdc, it->label ? it->label : L"", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case IT_QAT: {
        ButtonFace(r, hdc, &rc, hover, pressed, checked);
        int sz = Sc(r, M_ICON_S);
        DrawIconSlot(r, hdc, rc.left + (rc.right - rc.left - sz) / 2, rc.top + (rc.bottom - rc.top - sz) / 2, sz, icon_id, label, enabled, surface_bg);
        break;
    }
    case IT_QATMORE: {
        ButtonFace(r, hdc, &rc, hover, pressed, FALSE);
        DrawArrowDown(r, hdc, (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2, Col(r, LWUI_COLOR_TEXT));
        break;
    }
    case IT_HELP: {
        ButtonFace(r, hdc, &rc, hover, pressed, FALSE);
        SelectObject(hdc, r->font_bold);
        SetTextColor(hdc, text);
        DrawTextW(hdc, L"?", 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case IT_MINBTN: {
        ButtonFace(r, hdc, &rc, hover, pressed, FALSE);
        DrawChevron(r, hdc, (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2, !r->minimized, Col(r, LWUI_COLOR_TEXT));
        break;
    }
    case IT_LAUNCHER: {
        ButtonFace(r, hdc, &rc, hover, pressed, FALSE);
        HPEN p = CreatePen(PS_SOLID, 1, text);
        HGDIOBJ op = SelectObject(hdc, p);
        int m = Sc(r, 2);
        MoveToEx(hdc, rc.left + m, rc.top + m, NULL);
        LineTo(hdc, rc.right - m, rc.bottom - m);
        MoveToEx(hdc, rc.right - m - Sc(r, 4), rc.bottom - m, NULL);
        LineTo(hdc, rc.right - m, rc.bottom - m);
        LineTo(hdc, rc.right - m, rc.bottom - m - Sc(r, 4));
        SelectObject(hdc, op);
        DeleteObject(p);
        break;
    }
    case IT_GALCELL: {
        RECT cell = rc;
        InflateRect(&cell, -1, -1);
        ButtonFace(r, hdc, &cell, hover, pressed, checked);
        if (r->gallery_draw) {
            RECT cc = cell;
            if (r->gallery_draw(0, it->cmd, hdc, &cc, text, r->dpi, r->gallery_draw_ctx) == S_OK)
                break;
        }
        if (it->color != LWUI_COLOR_NONE) {
            RECT sw = rc;
            InflateRect(&sw, -Sc(r, 4), -Sc(r, 4));
            DrawSwatch(r, hdc, &sw, it->color, enabled);
        } else {
            int sz = Sc(r, M_ICON_S);
            DrawIconSlot(r, hdc, rc.left + (rc.right - rc.left - sz) / 2, rc.top + (rc.bottom - rc.top - sz) / 2, sz, icon_id, label, enabled, surface_bg);
        }
        break;
    }
    case IT_GALUP:
    case IT_GALDOWN:
    case IT_GALEXPAND: {
        BOOL off = (it->flags & IF_DISABLED) != 0;
        RECT b = rc;
        InflateRect(&b, -1, -1);
        FrameRound(r, hdc, &b, Col(r, LWUI_COLOR_GROUP_LABEL_BG), Col(r, LWUI_COLOR_GROUP_BORDER), Sc(r, 2), TRUE);
        if (!off) ButtonFace(r, hdc, &b, hover, pressed, FALSE);
        COLORREF c = off ? Col(r, LWUI_COLOR_TEXT_DISABLED) : text;
        int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
        if (it->kind == IT_GALUP) DrawChevron(r, hdc, cx, cy, TRUE, c);
        else if (it->kind == IT_GALDOWN) DrawChevron(r, hdc, cx, cy, FALSE, c);
        else {
            RECT line = {cx - Sc(r, 4), cy - Sc(r, 5), cx + Sc(r, 5), cy - Sc(r, 4)};
            HBRUSH lb = CreateSolidBrush(c);
            FillRect(hdc, &line, lb);
            DeleteObject(lb);
            DrawArrowDown(r, hdc, cx, cy + Sc(r, 2), c);
        }
        break;
    }
    case IT_GROUPBTN:
    case IT_CONTROL:
    case IT_SPLITARROW: {
        if (it->kind == IT_CONTROL && it->ckind == LWUI_CONTROL_COMBO && s == &r->s) {
            /* hosted combo: frame and arrow here, the EDIT child draws the text */
            RECT box = rc;
            InflateRect(&box, -1, -1);
            FrameRound(r, hdc, &box, Col(r, LWUI_COLOR_POPUP_BG), Col(r, (hover || pressed) ? LWUI_COLOR_HOVER_BORDER : LWUI_COLOR_GROUP_BORDER), Sc(r, 3), TRUE);
            if (hover || pressed) {
                RECT ar = {rc.right - Sc(r, M_ARROW_W) - Sc(r, 2), box.top + 1, box.right - 1, box.bottom - 1};
                ButtonFace(r, hdc, &ar, hover, pressed, FALSE);
            }
            DrawArrowDown(r, hdc, rc.right - Sc(r, M_ARROW_W) / 2 - Sc(r, 1), (rc.top + rc.bottom) / 2, text);
            break;
        }
        if (it->kind == IT_CONTROL && it->ckind == LWUI_CONTROL_CHECKBOX) {
            ButtonFace(r, hdc, &rc, hover, pressed, FALSE);
            int cb = Sc(r, M_CHECK);
            DrawCheckBox(r, hdc, rc.left + Sc(r, 4), rc.top + (rc.bottom - rc.top - cb) / 2, checked, enabled);
            RECT lr = {rc.left + Sc(r, 4) + cb + Sc(r, 6), rc.top, rc.right - Sc(r, 2), rc.bottom};
            SetTextColor(hdc, text);
            DrawTextW(hdc, label ? label : L"", -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            break;
        }
        if (it->kind == IT_CONTROL && it->ckind == LWUI_CONTROL_COLORWELL) {
            ButtonFace(r, hdc, &rc, hover, pressed, checked);
            SetTextColor(hdc, text);
            if (it->flags & IF_LARGE) {
                int sw = Sc(r, M_SWATCH_L);
                RECT swr = {rc.left + (rc.right - rc.left - sw) / 2, rc.top + Sc(r, 6), 0, 0};
                swr.right = swr.left + sw;
                swr.bottom = swr.top + sw;
                DrawSwatch(r, hdc, &swr, color, enabled);
                RECT lr = {rc.left, swr.bottom + Sc(r, 4), rc.right, rc.bottom};
                DrawTextW(hdc, label ? label : L"", -1, &lr, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else {
                int sw = Sc(r, M_ICON_S);
                RECT swr = {rc.left + Sc(r, 3), rc.top + (rc.bottom - rc.top - sw) / 2, 0, 0};
                swr.right = swr.left + sw;
                swr.bottom = swr.top + sw;
                DrawSwatch(r, hdc, &swr, color, enabled);
                if (it->flags & IF_LABEL) {
                    RECT lr = {swr.right + Sc(r, 4), rc.top, rc.right - Sc(r, 2), rc.bottom};
                    DrawTextW(hdc, label ? label : L"", -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }
            break;
        }
        BOOL is_checked = (it->kind == IT_CONTROL && IsToggleKind(it->ckind)) ? checked : FALSE;
        if (it->kind == IT_CONTROL && IsSplitKind(it->ckind)) {
            /* face spans both halves; the other half draws its own part */
            RECT whole = rc;
            for (UINT32 j = 0; j < s->n; j++) {
                Item *o = &s->items[j];
                if (o->kind == IT_SPLITARROW && o->cmd == it->cmd && o->grp == it->grp) UnionRect(&whole, &whole, &o->rc);
            }
            ButtonFace(r, hdc, &whole, hover, pressed || (s->pressed >= 0 && s->items[s->pressed].kind == IT_SPLITARROW && s->items[s->pressed].cmd == it->cmd), is_checked);
            if (hover || pressed) {
                HPEN p = CreatePen(PS_SOLID, 1, Col(r, LWUI_COLOR_HOVER_BORDER));
                HGDIOBJ op = SelectObject(hdc, p);
                if (it->flags & IF_LARGE) { MoveToEx(hdc, rc.left + 1, rc.bottom, NULL); LineTo(hdc, rc.right - 1, rc.bottom); }
                else { MoveToEx(hdc, rc.right, rc.top + 1, NULL); LineTo(hdc, rc.right, rc.bottom - 1); }
                SelectObject(hdc, op);
                DeleteObject(p);
            }
        } else if (it->kind == IT_SPLITARROW) {
            /* face drawn by the primary half */
        } else {
            ButtonFace(r, hdc, &rc, hover, pressed, is_checked);
        }
        SetTextColor(hdc, text);
        if (it->flags & IF_LARGE) {
            int sz = Sc(r, M_ICON_L);
            if (it->kind != IT_SPLITARROW) {
                int ix = rc.left + (rc.right - rc.left - sz) / 2;
                DrawIconSlot(r, hdc, ix, rc.top + Sc(r, 3), sz, it->kind == IT_GROUPBTN ? LWUI_ICON_NONE : icon_id, label, enabled, surface_bg);
            }
            if (it->flags & IF_LABEL) {
                RECT lr = rc;
                lr.top = (it->kind == IT_SPLITARROW) ? rc.top : rc.top + Sc(r, 3) + sz + Sc(r, 2);
                if (it->flags & IF_ARROW) lr.bottom -= Sc(r, 10);
                DrawTextW(hdc, label ? label : L"", -1, &lr, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (it->flags & IF_ARROW) DrawArrowDown(r, hdc, (rc.left + rc.right) / 2, rc.bottom - Sc(r, 6), text);
            }
        } else {
            int sz = Sc(r, M_ICON_S);
            int x = rc.left + Sc(r, 3);
            if (it->kind != IT_SPLITARROW) {
                DrawIconSlot(r, hdc, x, rc.top + (rc.bottom - rc.top - sz) / 2, sz, icon_id, label, enabled, surface_bg);
                x += sz + Sc(r, 4);
                if (it->flags & IF_LABEL) {
                    RECT lr = {x, rc.top, rc.right - ((it->flags & IF_ARROW) ? Sc(r, M_ARROW_W) : Sc(r, 2)), rc.bottom};
                    DrawTextW(hdc, label ? label : L"", -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }
            if (it->flags & IF_ARROW) DrawArrowDown(r, hdc, rc.right - Sc(r, M_ARROW_W) / 2, (rc.top + rc.bottom) / 2, text);
        }
        break;
    }
    case IT_MENUROW: {
        if (hover && enabled) {
            RECT h = {rc.left + 1, rc.top, rc.right - 1, rc.bottom};
            FrameRound(r, hdc, &h, Col(r, LWUI_COLOR_HOVER_BG), Col(r, LWUI_COLOR_HOVER_BORDER), Sc(r, 3), TRUE);
        }
        int x = rc.left + Sc(r, 6);
        if (it->flags & IF_CHECKMARK) {
            if (checked) DrawCheck(r, hdc, x + Sc(r, 8), (rc.top + rc.bottom) / 2, text);
            x += Sc(r, 20);
        } else if (it->flags & IF_RECENT) {
            x += Sc(r, 4);
        } else {
            int sz = (it->flags & IF_ICON32) ? Sc(r, M_ICON_L) : Sc(r, M_ICON_S);
            DrawIconSlot(r, hdc, x, rc.top + (rc.bottom - rc.top - sz) / 2, sz, icon_id, label, enabled, surface_bg);
            x += sz + Sc(r, 8);
        }
        RECT lr = {x, rc.top, rc.right - Sc(r, 6), rc.bottom};
        if ((it->flags & IF_RECENT) && (it->flags & IF_DISABLED)) { SelectObject(hdc, r->font_bold); text = Col(r, LWUI_COLOR_TEXT_DISABLED); }
        SetTextColor(hdc, text);
        DrawTextW(hdc, label ? label : L"", -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case IT_SEPARATOR: {
        RECT l = {rc.left + Sc(r, 6), (rc.top + rc.bottom) / 2, rc.right - Sc(r, 6), (rc.top + rc.bottom) / 2 + 1};
        FillRect(hdc, &l, Br(r, LWUI_COLOR_SEPARATOR));
        break;
    }
    }

    if (s->focus == index && GetFocus() == r->hwnd && s == &r->s) {
        RECT f = rc;
        InflateRect(&f, -1, -1);
        SetTextColor(hdc, Col(r, LWUI_COLOR_FOCUS));
        SetBkColor(hdc, surface_bg);
        DrawFocusRect(hdc, &f);
    }
}

static void PaintKeytips(ILeewayRibbon *r, HDC hdc, Surface *s, BOOL popup_keytips)
{
    for (UINT32 i = 0; i < s->n; i++) {
        Item *it = &s->items[i];
        if (!KeytipShownFor(r, it, popup_keytips)) continue;
        const WCHAR *k = EffectiveKeytip(r, it);
        if (!k) continue;
        /* Badges sit on the control's lower edge so the icon and label stay readable:
         * tabs hang into the band's top edge (or stay inside the strip when minimised),
         * QAT icons get theirs just below, large buttons straddle the edge under the
         * label, small buttons straddle it centred on their icon, gallery expanders
         * take the badge on themselves, menu rows at the left of the row. */
        int cx, cy;
        if (it->kind == IT_TAB || it->kind == IT_APPBUTTON) {
            cx = (it->rc.left + it->rc.right) / 2;
            cy = (r->minimized || popup_keytips) ? it->rc.bottom - Sc(r, 7) : it->rc.bottom + Sc(r, 5);
        } else if (it->kind == IT_QAT) {
            cx = (it->rc.left + it->rc.right) / 2;
            cy = it->rc.bottom + Sc(r, 3);
        } else if (it->kind == IT_GALEXPAND) {
            cx = (it->rc.left + it->rc.right) / 2;
            cy = (it->rc.top + it->rc.bottom) / 2;
        } else if (it->kind == IT_MENUROW) {
            cx = it->rc.left + Sc(r, 14);
            cy = it->rc.bottom - Sc(r, 3);
        } else if (it->flags & IF_LARGE) {
            cx = (it->rc.left + it->rc.right) / 2;
            cy = it->rc.bottom + Sc(r, 1);
            /* a large split's primary half ends at the split line: use the whole control's bottom */
            if (it->kind == IT_CONTROL && IsSplitKind(it->ckind)) cy += Sc(r, M_LARGE_H - M_ICON_L - 6);
        } else {
            cx = it->rc.left + Sc(r, 3) + Sc(r, M_ICON_S) / 2;
            cy = it->rc.bottom - Sc(r, 2);
        }
        DrawKeytip(r, hdc, k, cx, cy);
    }
}

static void PaintMain(ILeewayRibbon *r, HDC hdc, const RECT *client)
{
    EnsureLayout(r);
    FillRect(hdc, client, Br(r, LWUI_COLOR_STRIP_BG));
    int y = 0;
    if (r->layout && !r->qat_below) y += Sc(r, M_QAT_H);
    y += Sc(r, M_STRIP_H);
    COLORREF band_bg = Col(r, LWUI_COLOR_STRIP_BG);
    if (r->layout && !r->minimized && r->active_tab != 0xFFFFFFFF) {
        RECT band = {0, y, client->right, y + Sc(r, M_BAND_H)};
        VGradient(r, hdc, &band, Col(r, LWUI_COLOR_BAND_BG), Col(r, LWUI_COLOR_BAND_BG2));
        RECT line = {0, y, client->right, y + 1};
        FillRect(hdc, &line, Br(r, LWUI_COLOR_TAB_BORDER));
        band_bg = Col(r, LWUI_COLOR_BAND_BG);
    } else {
        RECT line = {0, y - 1, client->right, y};
        FillRect(hdc, &line, Br(r, LWUI_COLOR_TAB_BORDER));
    }
    /* frames first, then everything else, tabs last so the active tab joins the band */
    for (UINT32 i = 0; i < r->s.n; i++) if (r->s.items[i].kind == IT_GROUPFRAME) PaintItem(r, hdc, &r->s, (int)i, band_bg);
    for (UINT32 i = 0; i < r->s.n; i++) if (r->s.items[i].kind != IT_GROUPFRAME && r->s.items[i].kind != IT_TAB) PaintItem(r, hdc, &r->s, (int)i, r->s.items[i].kind == IT_CONTROL || r->s.items[i].kind == IT_SPLITARROW || r->s.items[i].kind == IT_GROUPBTN ? band_bg : Col(r, LWUI_COLOR_STRIP_BG));
    for (UINT32 i = 0; i < r->s.n; i++) if (r->s.items[i].kind == IT_TAB) PaintItem(r, hdc, &r->s, (int)i, Col(r, LWUI_COLOR_STRIP_BG));
    if (r->kt_level) PaintKeytips(r, hdc, &r->s, FALSE);
}

static void PaintPopup(Popup *p, HDC hdc, const RECT *client)
{
    ILeewayRibbon *r = p->r;
    COLORREF bg;
    if (p->kind == PK_GROUP || p->kind == PK_TAB) {
        VGradient(r, hdc, client, Col(r, LWUI_COLOR_BAND_BG), Col(r, LWUI_COLOR_BAND_BG2));
        bg = Col(r, LWUI_COLOR_BAND_BG);
    } else {
        FillRect(hdc, client, Br(r, LWUI_COLOR_POPUP_BG));
        bg = Col(r, LWUI_COLOR_POPUP_BG);
    }
    for (UINT32 i = 0; i < p->s.n; i++) if (p->s.items[i].kind == IT_GROUPFRAME) PaintItem(r, hdc, &p->s, (int)i, bg);
    for (UINT32 i = 0; i < p->s.n; i++) if (p->s.items[i].kind != IT_GROUPFRAME) PaintItem(r, hdc, &p->s, (int)i, bg);
    if (p->keytips) PaintKeytips(r, hdc, &p->s, TRUE);
}

/* ------------------------------------------------------------------ */
/* Tooltips (G22) */

static void AccelText(ILeewayRibbon *r, UINT32 cmd, WCHAR *out, int cch)
{
    out[0] = 0;
    for (UINT32 i = 0; i < r->accel_count; i++) {
        const ACCEL *a = &r->accels[i];
        if (a->cmd != (WORD)cmd) continue;
        WCHAR key[64] = {0};
        if (a->fVirt & FVIRTKEY) {
            UINT sc = MapVirtualKeyW(a->key, MAPVK_VK_TO_VSC);
            LONG lp = (LONG)(sc << 16);
            switch (a->key) {
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_INSERT: case VK_DELETE:
            case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_DIVIDE: case VK_NUMLOCK:
                lp |= 1 << 24;
                break;
            }
            if (!GetKeyNameTextW(lp, key, 64)) wsprintfW(key, L"0x%02X", a->key);
        } else {
            key[0] = (WCHAR)a->key;
            key[1] = 0;
        }
        wsprintfW(out, L"%s%s%s%s", (a->fVirt & FCONTROL) ? L"Ctrl+" : L"", (a->fVirt & FSHIFT) ? L"Shift+" : L"",
                  (a->fVirt & FALT) ? L"Alt+" : L"", key);
        (void)cch;
        return;
    }
}

static void HideTip(ILeewayRibbon *r)
{
    KillTimer(r->hwnd, TIMER_TIP);
    if (r->tip) ShowWindow(r->tip, SW_HIDE);
    r->tip_item = -1;
}

static void ShowTip(ILeewayRibbon *r, int index)
{
    if (index < 0 || index >= (int)r->s.n || r->popup) return;
    Item *it = &r->s.items[index];
    const WCHAR *tooltip = NULL;
    WCHAR accel[96];
    if (it->kind == IT_CONTROL || it->kind == IT_SPLITARROW || it->kind == IT_QAT || it->kind == IT_HELP || it->kind == IT_MINBTN || it->kind == IT_LAUNCHER) {
        LeewayUICommandState st;
        QueryState(r, it->cmd, &st, NULL, &tooltip, NULL);
        AccelText(r, it->cmd, accel, 96);
    } else if (it->kind == IT_GROUPBTN || it->kind == IT_GALCELL) {
        tooltip = it->label;
        accel[0] = 0;
    } else if (it->kind == IT_QATMORE) {
        tooltip = FrameworkLabel(LWUI_QAT_CUSTOMIZE);
        accel[0] = 0;
    } else {
        return;
    }
    if (it->kind == IT_MINBTN && !accel[0]) lstrcpyW(accel, L"Ctrl+F1");
    if (!tooltip || !*tooltip) return;
    if (accel[0]) wsprintfW(r->tip_text, L"%s (%s)", tooltip, accel);
    else lstrcpynW(r->tip_text, tooltip, 255);

    if (!r->tip) {
        r->tip = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, TIP_CLASS, NULL, WS_POPUP | WS_BORDER,
                                 0, 0, 10, 10, r->hwnd, NULL, GetModuleHandleW(NULL), r);
        if (!r->tip) return;
    }
    HDC hdc = GetDC(r->tip);
    HFONT of = (HFONT)SelectObject(hdc, r->font);
    int w = TextWidth(hdc, r->tip_text) + 2 * Sc(r, M_TIP_PAD) + 2;
    int h = Sc(r, 20) + 2;
    SelectObject(hdc, of);
    ReleaseDC(r->tip, hdc);
    POINT pt = {it->rc.left, it->rc.bottom + Sc(r, 4)};
    ClientToScreen(r->hwnd, &pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        if (pt.x + w > mi.rcWork.right) pt.x = mi.rcWork.right - w;
        if (pt.y + h > mi.rcWork.bottom) pt.y = it->rc.top - h - Sc(r, 4);
    }
    SetWindowPos(r->tip, HWND_TOPMOST, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(r->tip, NULL, TRUE);
    r->tip_item = index;
}

static LRESULT CALLBACK TipWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ILeewayRibbon *r = (ILeewayRibbon *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        return 0;
    }
    if (msg == WM_PAINT && r) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, Br(r, LWUI_COLOR_POPUP_BG));
        HFONT of = (HFONT)SelectObject(hdc, r->font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, Col(r, LWUI_COLOR_TEXT));
        rc.left += Sc(r, M_TIP_PAD);
        DrawTextW(hdc, r->tip_text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, of);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Popups: menus, galleries, collapsed groups, minimised tab, app menu, QAT menu */

static void ClosePopup(ILeewayRibbon *r);
static void Exec(ILeewayRibbon *r, UINT32 id);
static void OpenPopup(ILeewayRibbon *r, PopupKind kind, const RECT *anchor_client, HWND anchor_hwnd, UINT32 owner_cmd, const LeewayUIGroup *grp, UINT32 tab, BOOL keytips);
static void ReadRecent(ILeewayRibbon *r);

static void PopupSetRect(Popup *p, const RECT *anchor_screen, int w, int h)
{
    int x = anchor_screen->left, y = anchor_screen->bottom;
    HMONITOR mon = MonitorFromRect(anchor_screen, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y + h > mi.rcWork.bottom) y = anchor_screen->top - h;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }
    SetWindowPos(p->hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

static void AddMenuRow(ILeewayRibbon *r, Surface *s, UINT32 id, const WCHAR *label, UINT32 icon_id, const WCHAR *keytip, UINT32 flags, int x, int y, int w, int h)
{
    Item *it = SurfaceAdd(s);
    if (!it) return;
    it->kind = IT_MENUROW;
    it->cmd = id;
    it->label = label;
    it->icon_id = icon_id;
    it->keytip = keytip;
    it->flags = flags;
    SetRect(&it->rc, x, y, x + w, y + h);
    (void)r;
}

/* Lays out the popup's surface and returns its size. */
static void BuildPopup(Popup *p, HDC hdc, int ribbon_width, int *out_w, int *out_h)
{
    ILeewayRibbon *r = p->r;
    Surface *s = &p->s;
    SurfaceReset(s);
    int pad = Sc(r, 4);
    SelectObject(hdc, r->font);

    switch (p->kind) {
    case PK_MENU: {
        Collection *c = p->coll;
        int row = Sc(r, M_MENU_ROW_H);
        int w = Sc(r, 120);
        for (UINT32 i = 0; c && i < c->count; i++) {
            int tw = TextWidth(hdc, c->items[i].label) + Sc(r, M_ICON_S) + Sc(r, 30);
            if (tw > w) w = tw;
        }
        int y = pad;
        for (UINT32 i = 0; c && i < c->count; i++) {
            const LeewayUICommand *cmd = FindCommand(r, c->items[i].id);
            AddMenuRow(r, s, c->items[i].id, c->items[i].label, c->items[i].icon_id, cmd ? cmd->keytip : NULL, 0, pad, y, w, row);
            y += row;
        }
        *out_w = w + 2 * pad;
        *out_h = y + pad;
        break;
    }
    case PK_GALLERY: {
        Collection *c = p->coll;
        if (c && (c->flags & LWUI_COLLECTION_COLORS)) {
            /* colour grid: 8 swatches per row, text rows (LWUI_COLOR_NONE items) below */
            int cell = Sc(r, M_CELL) + Sc(r, 4);
            int row = Sc(r, M_MENU_ROW_H);
            UINT32 cols = 8, gi = 0;
            int w = (int)cols * cell;
            for (UINT32 i = 0; i < c->count; i++) {
                if (c->items[i].color == LWUI_COLOR_NONE) {
                    int tw = TextWidth(hdc, c->items[i].label) + Sc(r, 30);
                    if (tw > w) w = tw;
                }
            }
            for (UINT32 i = 0; i < c->count; i++) {
                const LeewayUICollectionItem *ci = &c->items[i];
                if (ci->color == LWUI_COLOR_NONE) continue;
                Item *it = SurfaceAdd(s);
                if (!it) break;
                it->kind = IT_GALCELL;
                it->cmd = ci->id;
                it->alt_cmd = ci->alt_id;
                it->color = ci->color;
                it->label = ci->label;
                SetRect(&it->rc, pad + (int)(gi % cols) * cell, pad + (int)(gi / cols) * cell, pad + (int)(gi % cols + 1) * cell, pad + (int)(gi / cols + 1) * cell);
                gi++;
            }
            int y = pad + (int)((gi + cols - 1) / cols) * cell;
            BOOL first = TRUE;
            for (UINT32 i = 0; i < c->count; i++) {
                const LeewayUICollectionItem *ci = &c->items[i];
                if (ci->color != LWUI_COLOR_NONE) continue;
                if (first) {
                    Item *sep = SurfaceAdd(s);
                    if (sep) { sep->kind = IT_SEPARATOR; SetRect(&sep->rc, pad, y, pad + w, y + Sc(r, 8)); }
                    y += Sc(r, 8);
                    first = FALSE;
                }
                const LeewayUICommand *cmd = FindCommand(r, ci->id);
                AddMenuRow(r, s, ci->id, ci->label, ci->icon_id, cmd ? cmd->keytip : NULL, 0, pad, y, w, row);
                y += row;
            }
            *out_w = w + 2 * pad;
            *out_h = y + pad;
            break;
        }
        int cw = Sc(r, M_GALLERY_CELL_W), ch = Sc(r, M_GALLERY_CELL_H);
        UINT32 n = c ? c->count : 0;
        UINT32 cols = n < 4 ? (n ? n : 1) : 4;
        for (UINT32 i = 0; i < n; i++) {
            int tw = TextWidth(hdc, c->items[i].label) + Sc(r, M_ICON_S) + Sc(r, 16);
            if (tw > cw) cw = tw;
        }
        for (UINT32 i = 0; i < n; i++) {
            const LeewayUICommand *cmd = FindCommand(r, c->items[i].id);
            AddMenuRow(r, s, c->items[i].id, c->items[i].label, c->items[i].icon_id, cmd ? cmd->keytip : NULL, 0,
                       pad + (int)(i % cols) * cw, pad + (int)(i / cols) * ch, cw, ch);
        }
        *out_w = (int)cols * cw + 2 * pad;
        *out_h = (int)((n + cols - 1) / cols) * ch + 2 * pad;
        break;
    }
    case PK_APPMENU: {
        int row = Sc(r, M_APPMENU_ROW_H);
        int w = Sc(r, M_APPMENU_W);
        int y = pad;
        if (r->layout) {
            for (UINT32 i = 0; i < r->layout->appmenu_count; i++) {
                UINT32 id = r->layout->appmenu_ids[i];
                LeewayUICommandState st;
                QueryState(r, id, &st, NULL, NULL, NULL);
                if (!st.visible) continue;
                const LeewayUICommand *cmd = FindCommand(r, id);
                AddMenuRow(r, s, id, NULL, 0, cmd ? cmd->keytip : NULL, IF_ICON32, pad, y, w, row);
                y += row;
            }
        }
        int left_h = y;
        int total_w = w + 2 * pad;
        if (r->recent_open && r->recent_max > 0) {
            ReadRecent(r);
            if (r->recent_count > 0) {
                int rw = Sc(r, M_RECENT_W);
                int rx = w + 2 * pad;
                int ry = pad;
                Item *hdr = SurfaceAdd(s);
                if (hdr) { hdr->kind = IT_MENUROW; hdr->cmd = 0; hdr->label = L"Recent"; hdr->icon_id = 0; hdr->flags = IF_RECENT | IF_DISABLED; SetRect(&hdr->rc, rx, ry, rx + rw, ry + row); }
                ry += row;
                for (UINT32 i = 0; i < r->recent_count; i++) {
                    Item *ri = SurfaceAdd(s);
                    if (!ri) break;
                    ri->kind = IT_MENUROW; ri->cmd = i; ri->label = r->recent_names[i];
                    ri->icon_id = 0; ri->flags = IF_RECENT;
                    SetRect(&ri->rc, rx, ry, rx + rw, ry + row);
                    ry += row;
                }
                total_w = rx + rw + pad;
                if (ry > left_h) left_h = ry;
            }
        }
        *out_w = total_w;
        *out_h = left_h + pad;
        break;
    }
    case PK_QATMENU: {
        int row = Sc(r, M_MENU_ROW_H);
        int w = Sc(r, 200);
        int y = pad;
        UINT32 listed[128];
        UINT32 nl = 0;
        if (r->layout) {
            for (UINT32 t = 0; t < r->layout->tab_count && nl < 128; t++) {
                for (UINT32 g = 0; g < r->layout->tabs[t].group_count && nl < 128; g++) {
                    const LeewayUIGroup *grp = &r->layout->tabs[t].groups[g];
                    for (UINT32 k = 0; k < grp->control_count && nl < 128; k++) {
                        UINT32 id = grp->controls[k].command_id;
                        BOOL dup = FALSE;
                        for (UINT32 j = 0; j < nl; j++) if (listed[j] == id) { dup = TRUE; break; }
                        if (!dup && !IsFrameworkId(id)) listed[nl++] = id;
                    }
                }
            }
        }
        for (UINT32 i = 0; i < nl; i++) {
            const LeewayUICommand *cmd = FindCommand(r, listed[i]);
            BOOL in_qat = FALSE;
            for (UINT32 q = 0; q < r->qat_count; q++) if (r->qat_ids[q] == listed[i]) in_qat = TRUE;
            int tw = TextWidth(hdc, cmd ? cmd->label : L"") + Sc(r, 40);
            if (tw > w) w = tw;
            AddMenuRow(r, s, listed[i], cmd ? cmd->label : L"", 0, NULL, IF_CHECKMARK | (in_qat ? IF_CHECKED : 0), pad, y, w, row);
            y += row;
        }
        Item *sep = SurfaceAdd(s);
        if (sep) { sep->kind = IT_SEPARATOR; SetRect(&sep->rc, pad, y, pad + w, y + Sc(r, 8)); }
        y += Sc(r, 8);
        AddMenuRow(r, s, LWUI_QAT_CUSTOMIZE, r->qat_below ? L"Show Above the Ribbon" : L"Show Below the Ribbon", 0, NULL, IF_CHECKMARK, pad, y, w, row);
        y += row;
        AddMenuRow(r, s, LWUI_RIBBON_MINIMIZE, FrameworkLabel(LWUI_RIBBON_MINIMIZE), 0, NULL, IF_CHECKMARK | (r->minimized ? IF_CHECKED : 0), pad, y, w, row);
        y += row;
        for (UINT32 i = 0; i < s->n; i++) s->items[i].rc.right = pad + w;
        *out_w = w + 2 * pad;
        *out_h = y + pad;
        break;
    }
    case PK_GROUP: {
        int band_h = Sc(r, M_BAND_H) - 2 * Sc(r, M_PAD);
        int w = PackGroup(r, hdc, p->grp, LWUI_VARIANT_LARGE, s, pad, pad, p->tab, band_h);
        *out_w = w + 2 * pad;
        *out_h = band_h + 2 * pad;
        break;
    }
    case PK_TAB: {
        RECT band = {0, 0, ribbon_width, Sc(r, M_BAND_H)};
        UINT32 variants[MAX_GROUPS];
        LayoutBand(r, hdc, s, p->tab, &band, variants);
        *out_w = ribbon_width;
        *out_h = Sc(r, M_BAND_H);
        break;
    }
    }
}

static void PopupActivate(Popup *p, int index)
{
    ILeewayRibbon *r = p->r;
    if (index < 0 || index >= (int)p->s.n) return;
    Item it = p->s.items[index];
    switch (it.kind) {
    case IT_MENUROW: {
        if (it.flags & IF_RECENT) {
            if (!(it.flags & IF_DISABLED) && r->recent_open && it.cmd < r->recent_count) {
                WCHAR path[MAX_PATH];
                lstrcpynW(path, r->recent_paths[it.cmd], MAX_PATH);
                ClosePopup(r);
                r->recent_open(path, r->recent_ctx);
            }
            return;
        }
        if (p->kind == PK_QATMENU) {
            if (it.cmd == LWUI_QAT_CUSTOMIZE) {
                r->qat_below = !r->qat_below;
                ClosePopup(r);
                Relayout(r);
                NotifyParent(r, LWUI_RBN_HEIGHTCHANGED);
                /* persist position with the ids */
                UINT32 ids[MAX_QAT];
                UINT32 n = r->qat_count;
                CopyMemory(ids, r->qat_ids, n * sizeof(UINT32));
                LeewayRibbonSetQAT(r, ids, n);
            } else if (it.cmd == LWUI_RIBBON_MINIMIZE) {
                ClosePopup(r);
                LeewayRibbonMinimizeRibbon(r, !r->minimized);
            } else {
                UINT32 ids[MAX_QAT];
                UINT32 n = 0;
                BOOL found = FALSE;
                for (UINT32 i = 0; i < r->qat_count; i++) {
                    if (r->qat_ids[i] == it.cmd) { found = TRUE; continue; }
                    ids[n++] = r->qat_ids[i];
                }
                if (!found && n < MAX_QAT) ids[n++] = it.cmd;
                ClosePopup(r);
                LeewayRibbonSetQAT(r, ids, n);
            }
            return;
        }
        if (it.cmd == 0) {
            /* text-only choice of a combo shown as a menu: set the text, execute the combo's command */
            UINT32 owner = p->owner_cmd;
            SetComboText(r, owner, it.label);
            ClosePopup(r);
            Exec(r, owner);
            return;
        }
        LeewayUICommandState st;
        QueryState(r, it.cmd, &st, NULL, NULL, NULL);
        if (!st.enabled) return;
        Collection *c = p->coll;
        if (c && (c->flags & LWUI_COLLECTION_LAST_CHOICE_PRIMARY)) c->last_choice = it.cmd;
        ClosePopup(r);
        Exec(r, it.cmd);
        return;
    }
    case IT_GALCELL: {
        UINT32 id = it.cmd;
        ClosePopup(r);
        if (id) Exec(r, id);
        return;
    }
    case IT_GALUP:
    case IT_GALDOWN: {
        if (it.flags & IF_DISABLED) return;
        SetStripOffset(r, it.cmd, StripOffset(r, it.cmd) + (it.kind == IT_GALDOWN ? 1 : -1));
        RECT rc;
        GetClientRect(r->hwnd, &rc);
        HDC hdc = GetDC(p->hwnd);
        int w, h;
        BuildPopup(p, hdc, rc.right, &w, &h);
        ReleaseDC(p->hwnd, hdc);
        InvalidateRect(p->hwnd, NULL, FALSE);
        return;
    }
    case IT_GALEXPAND: {
        RECT anchor = it.rc;
        HWND from = p->hwnd;
        ClosePopup(r);
        OpenPopup(r, PK_GALLERY, &anchor, from, it.cmd, NULL, it.tab, FALSE);
        return;
    }
    case IT_CONTROL:
    case IT_SPLITARROW:
    case IT_LAUNCHER:
    case IT_GROUPBTN: {
        /* controls inside a group/tab popup behave as on the band */
        LeewayUICommandState st;
        if (it.kind != IT_GROUPBTN) {
            QueryState(r, it.cmd, &st, NULL, NULL, NULL);
            if (!st.enabled) return;
        }
        RECT anchor = it.rc;
        HWND from = p->hwnd;
        if (it.kind == IT_SPLITARROW || (it.kind == IT_CONTROL && (it.ckind == LWUI_CONTROL_DROPDOWN || it.ckind == LWUI_CONTROL_COMBO))) {
            ClosePopup(r);
            OpenPopup(r, PK_MENU, &anchor, from, it.cmd, NULL, it.tab, FALSE);
            return;
        }
        if (it.kind == IT_CONTROL && it.ckind == LWUI_CONTROL_GALLERY) {
            ClosePopup(r);
            OpenPopup(r, PK_GALLERY, &anchor, from, it.cmd, NULL, it.tab, FALSE);
            return;
        }
        if (it.kind == IT_GROUPBTN) {
            ClosePopup(r);
            OpenPopup(r, PK_GROUP, &anchor, from, 0, it.grp, it.tab, FALSE);
            return;
        }
        UINT32 id = it.cmd;
        if (IsSplitKind(it.ckind)) {
            Collection *c = FindCollection(r, it.cmd);
            if (c && (c->flags & LWUI_COLLECTION_LAST_CHOICE_PRIMARY) && c->last_choice) id = c->last_choice;
        }
        ClosePopup(r);
        Exec(r, id);
        return;
    }
    default:
        return;
    }
}

static LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Popup *p = (Popup *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        return 0;
    }
    if (!p) return DefWindowProcW(hwnd, msg, wp, lp);
    ILeewayRibbon *r = p->r;

    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_GETOBJECT:
        if ((LONG)lp == OBJID_CLIENT) return AccessibleFromWindow(hwnd, TRUE, wp);
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        HFONT of = (HFONT)SelectObject(mem, r->font);
        PaintPopup(p, mem, &rc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, of);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        int h = HitTest(&p->s, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h != p->s.hover) {
            p->s.hover = h;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (!PtInRect(&rc, pt)) {
            ClosePopup(r);
            return 0;
        }
        p->s.pressed = HitTest(&p->s, pt.x, pt.y);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        int h = HitTest(&p->s, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        int pressed = p->s.pressed;
        p->s.pressed = -1;
        if (h >= 0 && h == pressed) PopupActivate(p, h);
        else InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (!PtInRect(&rc, pt)) ClosePopup(r);
        return 0;
    }
    case WM_RBUTTONUP: {
        int h = HitTest(&p->s, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h >= 0 && p->s.items[h].kind == IT_GALCELL && p->s.items[h].alt_cmd) {
            UINT32 alt = p->s.items[h].alt_cmd;
            ClosePopup(r);
            Exec(r, alt);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (!p->closing && (HWND)lp != hwnd) ClosePopup(r);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenPopup(ILeewayRibbon *r, PopupKind kind, const RECT *anchor_client, HWND anchor_hwnd, UINT32 owner_cmd, const LeewayUIGroup *grp, UINT32 tab, BOOL keytips)
{
    ClosePopup(r);
    HideTip(r);
    Popup *p = (Popup *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Popup));
    if (!p) return;
    p->r = r;
    p->kind = kind;
    p->owner_cmd = owner_cmd;
    p->coll = owner_cmd ? FindCollection(r, owner_cmd) : NULL;
    p->grp = grp;
    p->tab = tab;
    p->keytips = keytips;
    p->s.hover = p->s.pressed = p->s.focus = -1;
    if ((kind == PK_MENU || kind == PK_GALLERY) && (!p->coll || p->coll->count == 0)) {
        HeapFree(GetProcessHeap(), 0, p);
        return;
    }
    HWND root = GetAncestor(r->hwnd, GA_ROOT);
    p->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, POPUP_CLASS, NULL, WS_POPUP | WS_BORDER,
                              0, 0, 10, 10, root ? root : r->hwnd, NULL, GetModuleHandleW(NULL), p);
    if (!p->hwnd) {
        HeapFree(GetProcessHeap(), 0, p);
        return;
    }
    r->popup = p;
    RECT client;
    GetClientRect(r->hwnd, &client);
    HDC hdc = GetDC(p->hwnd);
    int w = 10, h = 10;
    BuildPopup(p, hdc, client.right, &w, &h);
    ReleaseDC(p->hwnd, hdc);
    RECT anchor = *anchor_client;
    if (kind == PK_TAB) {
        /* the band of a minimised ribbon drops below the whole tab strip */
        SetRect(&anchor, 0, anchor_client->bottom + Sc(r, 2), client.right, anchor_client->bottom + Sc(r, 2));
        anchor_hwnd = r->hwnd;
    }
    MapWindowPoints(anchor_hwnd, NULL, (POINT *)&anchor, 2);
    int border = 2 * GetSystemMetrics(SM_CXBORDER);
    PopupSetRect(p, &anchor, w + border, h + border);
    ShowWindow(p->hwnd, SW_SHOWNOACTIVATE);
    SetCapture(p->hwnd);
}

static void ClosePopup(ILeewayRibbon *r)
{
    Popup *p = r->popup;
    if (!p || p->closing) return;
    p->closing = TRUE;
    r->popup = NULL;
    if (GetCapture() == p->hwnd) ReleaseCapture();
    DestroyWindow(p->hwnd);
    SurfaceFree(&p->s);
    HeapFree(GetProcessHeap(), 0, p);
    if (r->kt_level && r->hwnd) InvalidateRect(r->hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Commands */

static void KeytipExit(ILeewayRibbon *r)
{
    if (!r->kt_level) return;
    r->kt_level = 0;
    r->kt_buf[0] = 0;
    InvalidateRect(r->hwnd, NULL, FALSE);
}

static void Exec(ILeewayRibbon *r, UINT32 id)
{
    KeytipExit(r);
    HideTip(r);
    if (id == LWUI_RIBBON_MINIMIZE) {
        LeewayRibbonMinimizeRibbon(r, !r->minimized);
        return;
    }
    if (id == LWUI_APPMENU || id == LWUI_QAT_CUSTOMIZE) return;
    if (r->execute) r->execute(id, r->ctx);
    /* state may have changed anywhere (zoom, tool, selection): re-read at the next paint */
    Relayout(r);
}

static void SetActiveTab(ILeewayRibbon *r, UINT32 index)
{
    if (!r->layout || index >= r->layout->tab_count || !TabVisible(r, index)) return;
    if (!r->layout->tabs[index].context) r->restore_tab = index;
    r->active_tab = index;
    Relayout(r);
}

static void MainActivate(ILeewayRibbon *r, int index, BOOL from_keytip)
{
    if (index < 0 || index >= (int)r->s.n) return;
    Item it = r->s.items[index];
    HideTip(r);
    switch (it.kind) {
    case IT_APPBUTTON:
        OpenPopup(r, PK_APPMENU, &it.rc, r->hwnd, LWUI_APPMENU, NULL, 0, from_keytip);
        return;
    case IT_TAB:
        SetActiveTab(r, it.tab);
        if (r->minimized) {
            RECT a = it.rc;
            OpenPopup(r, PK_TAB, &a, r->hwnd, 0, NULL, it.tab, from_keytip);
        } else if (from_keytip) {
            r->kt_level = 2;
            r->kt_buf[0] = 0;
            InvalidateRect(r->hwnd, NULL, FALSE);
        }
        return;
    case IT_QATMORE:
        OpenPopup(r, PK_QATMENU, &it.rc, r->hwnd, LWUI_QAT_CUSTOMIZE, NULL, 0, FALSE);
        return;
    case IT_MINBTN:
        Exec(r, LWUI_RIBBON_MINIMIZE);
        return;
    case IT_GROUPBTN:
        OpenPopup(r, PK_GROUP, &it.rc, r->hwnd, 0, it.grp, it.tab, from_keytip);
        return;
    case IT_GALCELL:
        if (it.cmd) Exec(r, it.cmd);
        return;
    case IT_GALUP:
    case IT_GALDOWN:
        if (it.flags & IF_DISABLED) return;
        SetStripOffset(r, it.cmd, StripOffset(r, it.cmd) + (it.kind == IT_GALDOWN ? 1 : -1));
        Relayout(r);
        return;
    case IT_GALEXPAND:
        OpenPopup(r, PK_GALLERY, &it.rc, r->hwnd, it.cmd, NULL, it.tab, from_keytip);
        return;
    case IT_QAT:
    case IT_HELP:
    case IT_LAUNCHER:
    case IT_CONTROL:
    case IT_SPLITARROW: {
        LeewayUICommandState st;
        QueryState(r, it.cmd, &st, NULL, NULL, NULL);
        if (!st.enabled) return;
        if (it.kind == IT_CONTROL && it.ckind == LWUI_CONTROL_COMBO) {
            Hosted *h = FindHosted(r, it.cmd);
            if (h && h->hwnd && from_keytip) {
                SetFocus(h->hwnd);   /* the keytip puts the caret in the box */
                SendMessageW(h->hwnd, EM_SETSEL, 0, -1);
                return;
            }
            /* a click lands on the arrow (the EDIT child takes the text area): open the list */
            OpenPopup(r, PK_MENU, &it.rc, r->hwnd, it.cmd, NULL, it.tab, from_keytip);
            return;
        }
        if (it.kind == IT_SPLITARROW || (it.kind == IT_CONTROL && it.ckind == LWUI_CONTROL_DROPDOWN)) {
            RECT a = it.rc;
            if (it.kind == IT_SPLITARROW) {
                for (UINT32 j = 0; j < r->s.n; j++) {
                    Item *o = &r->s.items[j];
                    if (o->kind == IT_CONTROL && o->cmd == it.cmd && o->grp == it.grp) UnionRect(&a, &a, &o->rc);
                }
            }
            OpenPopup(r, PK_MENU, &a, r->hwnd, it.cmd, NULL, it.tab, from_keytip);
            return;
        }
        if (it.kind == IT_CONTROL && it.ckind == LWUI_CONTROL_GALLERY) {
            OpenPopup(r, PK_GALLERY, &it.rc, r->hwnd, it.cmd, NULL, it.tab, from_keytip);
            return;
        }
        UINT32 id = it.cmd;
        if (it.kind == IT_CONTROL && IsSplitKind(it.ckind)) {
            Collection *c = FindCollection(r, it.cmd);
            if (c && (c->flags & LWUI_COLLECTION_LAST_CHOICE_PRIMARY) && c->last_choice) id = c->last_choice;
        }
        Exec(r, id);
        return;
    }
    default:
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Accessibility (G20): one IAccessible per ribbon or popup window whose
 * simple children are the window's items. oleacc and oleaut32 are loaded
 * at run time so the library adds no link dependency. */

static const GUID LWUI_IID_IUnknown    = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID LWUI_IID_IDispatch   = {0x00020400, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID LWUI_IID_IAccessible = {0x618736E0, 0x3C3D, 0x11CF, {0x81, 0x0C, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

typedef LRESULT (STDAPICALLTYPE *PFN_LresultFromObject)(REFIID, WPARAM, LPUNKNOWN);
typedef HRESULT (STDAPICALLTYPE *PFN_AccessibleObjectFromWindow)(HWND, DWORD, REFIID, void **);
typedef BSTR (STDAPICALLTYPE *PFN_SysAllocString)(const OLECHAR *);

static PFN_LresultFromObject g_LresultFromObject;
static PFN_AccessibleObjectFromWindow g_AccessibleObjectFromWindow;
static PFN_SysAllocString g_SysAllocString;
static BOOL g_acc_loaded;

static void LoadAccessibility(void)
{
    if (g_acc_loaded) return;
    g_acc_loaded = TRUE;
    HMODULE oleacc = LoadLibraryW(L"oleacc.dll");
    HMODULE oleaut = LoadLibraryW(L"oleaut32.dll");
    if (oleacc) {
        g_LresultFromObject = (PFN_LresultFromObject)(void *)GetProcAddress(oleacc, "LresultFromObject");
        g_AccessibleObjectFromWindow = (PFN_AccessibleObjectFromWindow)(void *)GetProcAddress(oleacc, "AccessibleObjectFromWindow");
    }
    if (oleaut) g_SysAllocString = (PFN_SysAllocString)(void *)GetProcAddress(oleaut, "SysAllocString");
}

static HRESULT AccString(const WCHAR *s, BSTR *out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!s || !*s) return S_FALSE;
    if (!g_SysAllocString) return E_FAIL;
    *out = g_SysAllocString(s);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static const WCHAR *AccNameOf(ILeewayRibbon *r, const Item *it, WCHAR *buf, int cch)
{
    LeewayUICommandState st;
    const WCHAR *label = NULL;
    switch (it->kind) {
    case IT_GROUPFRAME: case IT_TAB: case IT_GROUPBTN: return it->label ? it->label : L"";
    case IT_APPBUTTON: { const LeewayUICommand *c = FindCommand(r, LWUI_APPMENU); return c && c->label ? c->label : L"File"; }
    case IT_QATMORE: return FrameworkLabel(LWUI_QAT_CUSTOMIZE);
    case IT_MINBTN: return FrameworkLabel(LWUI_RIBBON_MINIMIZE);
    case IT_SEPARATOR: return L"";
    case IT_GALUP: return L"Scroll up";
    case IT_GALDOWN: return L"Scroll down";
    case IT_GALEXPAND:
        QueryState(r, it->cmd, &st, &label, NULL, NULL);
        wsprintfW(buf, L"%s: more", label ? label : L"");
        return buf;
    case IT_SPLITARROW:
        QueryState(r, it->cmd, &st, &label, NULL, NULL);
        wsprintfW(buf, L"%s menu", label ? label : L"");
        return buf;
    default:
        if (it->label) return it->label;
        QueryState(r, it->cmd, &st, &label, NULL, NULL);
        return label ? label : L"";
    }
    (void)cch;
}

static LONG AccRoleOf(const Item *it)
{
    switch (it->kind) {
    case IT_GROUPFRAME: return ROLE_SYSTEM_GROUPING;
    case IT_APPBUTTON: return ROLE_SYSTEM_BUTTONMENU;
    case IT_TAB: return ROLE_SYSTEM_PAGETAB;
    case IT_QATMORE: case IT_SPLITARROW: case IT_GROUPBTN: case IT_GALEXPAND: return ROLE_SYSTEM_BUTTONDROPDOWN;
    case IT_MINBTN: return ROLE_SYSTEM_CHECKBUTTON;
    case IT_MENUROW: return ROLE_SYSTEM_MENUITEM;
    case IT_SEPARATOR: return ROLE_SYSTEM_SEPARATOR;
    case IT_GALCELL: return ROLE_SYSTEM_LISTITEM;
    case IT_CONTROL:
        if (IsToggleKind(it->ckind)) return ROLE_SYSTEM_CHECKBUTTON;
        if (IsSplitKind(it->ckind)) return ROLE_SYSTEM_SPLITBUTTON;
        if (it->ckind == LWUI_CONTROL_COMBO) return ROLE_SYSTEM_COMBOBOX;
        if (HasArrow(it->ckind)) return ROLE_SYSTEM_BUTTONDROPDOWN;
        return ROLE_SYSTEM_PUSHBUTTON;
    default: return ROLE_SYSTEM_PUSHBUTTON;
    }
}

static BOOL AccOpensPopup(const Item *it)
{
    switch (it->kind) {
    case IT_APPBUTTON: case IT_QATMORE: case IT_SPLITARROW: case IT_GROUPBTN: case IT_GALEXPAND: return TRUE;
    case IT_CONTROL: return HasArrow(it->ckind) && !IsSplitKind(it->ckind);
    default: return FALSE;
    }
}

static LONG AccStateOf(ILeewayRibbon *r, HWND hwnd, Surface *s, int index)
{
    const Item *it = &s->items[index];
    if (it->kind == IT_GROUPFRAME || it->kind == IT_SEPARATOR) return STATE_SYSTEM_READONLY;
    LONG state = STATE_SYSTEM_FOCUSABLE;
    LeewayUICommandState st;
    BOOL enabled = TRUE, checked = FALSE;
    if (it->kind == IT_CONTROL || it->kind == IT_SPLITARROW || it->kind == IT_QAT || it->kind == IT_HELP ||
        it->kind == IT_MINBTN || it->kind == IT_LAUNCHER || it->kind == IT_GALCELL || it->kind == IT_MENUROW) {
        QueryState(r, it->cmd, &st, NULL, NULL, NULL);
        enabled = st.enabled;
        checked = st.checked;
        if (it->kind == IT_MENUROW && (it->flags & IF_CHECKMARK)) checked = (it->flags & IF_CHECKED) != 0;
        if (it->kind == IT_GALCELL && it->cmd == 0) { enabled = TRUE; checked = FALSE; }
    }
    if (it->flags & IF_DISABLED) enabled = FALSE;
    if (!enabled) state |= STATE_SYSTEM_UNAVAILABLE;
    if (checked && (it->kind == IT_MINBTN || it->kind == IT_GALCELL || it->kind == IT_MENUROW || (it->kind == IT_CONTROL && IsToggleKind(it->ckind)))) state |= STATE_SYSTEM_CHECKED;
    if (it->kind == IT_TAB) {
        state |= STATE_SYSTEM_SELECTABLE;
        if (it->tab == r->active_tab) state |= STATE_SYSTEM_SELECTED;
    }
    if (AccOpensPopup(it)) state |= STATE_SYSTEM_HASPOPUP;
    if (s->pressed == index) state |= STATE_SYSTEM_PRESSED;
    if (s->hover == index) state |= STATE_SYSTEM_HOTTRACKED;
    if (s->focus == index && GetFocus() == hwnd) state |= STATE_SYSTEM_FOCUSED;
    return state;
}

static const WCHAR *AccDefaultActionOf(const Item *it)
{
    if (it->kind == IT_TAB) return L"Switch";
    if (AccOpensPopup(it)) return L"Open";
    if (it->kind == IT_MINBTN || (it->kind == IT_CONTROL && IsToggleKind(it->ckind))) return L"Toggle";
    if (it->kind == IT_GROUPFRAME || it->kind == IT_SEPARATOR) return NULL;
    return L"Press";
}

class RibbonAccessible : public IAccessible {
public:
    RibbonAccessible(HWND hwnd, BOOL is_popup) : m_ref(1), m_hwnd(hwnd), m_popup(is_popup) {}

    /* IUnknown */
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, LWUI_IID_IUnknown) || IsEqualGUID(riid, LWUI_IID_IDispatch) || IsEqualGUID(riid, LWUI_IID_IAccessible)) {
            *ppv = static_cast<IAccessible *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG n = InterlockedDecrement(&m_ref);
        if (n == 0) {
            this->RibbonAccessible::~RibbonAccessible();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return (ULONG)n;
    }

    /* IDispatch: not scriptable; IAccessible is reached through the vtable */
    STDMETHODIMP GetTypeInfoCount(UINT *pctinfo) { if (pctinfo) *pctinfo = 0; return S_OK; }
    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo **ppTInfo) { if (ppTInfo) *ppTInfo = NULL; return E_NOTIMPL; }
    STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR *, UINT, LCID, DISPID *) { return E_NOTIMPL; }
    STDMETHODIMP Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS *, VARIANT *, EXCEPINFO *, UINT *) { return E_NOTIMPL; }

    /* IAccessible */
    STDMETHODIMP get_accParent(IDispatch **ppdispParent)
    {
        if (!ppdispParent) return E_POINTER;
        *ppdispParent = NULL;
        if (!IsWindow(m_hwnd) || !g_AccessibleObjectFromWindow) return S_FALSE;
        HWND parent = m_popup ? GetWindow(m_hwnd, GW_OWNER) : GetParent(m_hwnd);
        if (!parent) return S_FALSE;
        return SUCCEEDED(g_AccessibleObjectFromWindow(parent, OBJID_WINDOW, LWUI_IID_IDispatch, (void **)ppdispParent)) ? S_OK : S_FALSE;
    }
    STDMETHODIMP get_accChildCount(long *pcountChildren)
    {
        if (!pcountChildren) return E_POINTER;
        ILeewayRibbon *r; Surface *s;
        *pcountChildren = Resolve(&r, &s) ? (long)s->n : 0;
        return S_OK;
    }
    STDMETHODIMP get_accChild(VARIANT, IDispatch **ppdispChild)
    {
        if (ppdispChild) *ppdispChild = NULL;
        return S_FALSE;   /* children are simple elements */
    }
    STDMETHODIMP get_accName(VARIANT varChild, BSTR *pszName)
    {
        ILeewayRibbon *r; Surface *s;
        int i = Child(varChild, &r, &s);
        if (i == -2) return E_INVALIDARG;
        if (i == -1) return AccString(m_popup ? L"Ribbon popup" : L"Ribbon", pszName);
        WCHAR buf[160];
        return AccString(AccNameOf(r, &s->items[i], buf, 160), pszName);
    }
    STDMETHODIMP get_accValue(VARIANT varChild, BSTR *pszValue)
    {
        ILeewayRibbon *r; Surface *s;
        int i = Child(varChild, &r, &s);
        if (i == -2) return E_INVALIDARG;
        if (pszValue) *pszValue = NULL;
        if (i < 0) return S_FALSE;
        const Item *it = &s->items[i];
        WCHAR buf[64];
        if (it->kind == IT_CONTROL && it->ckind == LWUI_CONTROL_COMBO) {
            Hosted *h = FindHosted(r, it->cmd);
            return AccString(h ? h->text : L"", pszValue);
        }
        COLORREF c = LWUI_COLOR_NONE;
        if (it->kind == IT_GALCELL) c = it->color;
        if (it->kind == IT_CONTROL && it->ckind == LWUI_CONTROL_COLORWELL) {
            LeewayUICommandState st;
            QueryState(r, it->cmd, &st, NULL, NULL, NULL);
            c = st.color;
        }
        if (c != LWUI_COLOR_NONE) {
            wsprintfW(buf, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
            return AccString(buf, pszValue);
        }
        return S_FALSE;
    }
    STDMETHODIMP get_accDescription(VARIANT varChild, BSTR *pszDescription)
    {
        ILeewayRibbon *r; Surface *s;
        int i = Child(varChild, &r, &s);
        if (i == -2) return E_INVALIDARG;
        if (pszDescription) *pszDescription = NULL;
        if (i < 0) return S_FALSE;
        const Item *it = &s->items[i];
        if (it->kind != IT_CONTROL && it->kind != IT_QAT && it->kind != IT_HELP && it->kind != IT_LAUNCHER) return S_FALSE;
        LeewayUICommandState st;
        const WCHAR *tip = NULL;
        QueryState(r, it->cmd, &st, NULL, &tip, NULL);
        return AccString(tip, pszDescription);
    }
    STDMETHODIMP get_accRole(VARIANT varChild, VARIANT *pvarRole)
    {
        if (!pvarRole) return E_POINTER;
        ILeewayRibbon *r; Surface *s;
        int i = Child(varChild, &r, &s);
        if (i == -2) return E_INVALIDARG;
        pvarRole->vt = VT_I4;
        pvarRole->lVal = i < 0 ? ROLE_SYSTEM_TOOLBAR : AccRoleOf(&s->items[i]);
        return S_OK;
    }
    STDMETHODIMP get_accState(VARIANT varChild, VARIANT *pvarState)
    {
        if (!pvarState) return E_POINTER;
        ILeewayRibbon *r; Surface *s;
        int i = Child(varChild, &r, &s);
        if (i == -2) return E_INVALIDARG;
        pvarState->vt = VT_I4;
        if (i < 0) pvarState->lVal = IsWindow(m_hwnd) && GetFocus() == m_hwnd ? STATE_SYSTEM_FOCUSED : 0;
        else pvarState->lVal = AccStateOf(r, m_hwnd, s, i);
        return S_OK;
    }
    STDMETHODIMP get_accHelp(VARIANT, BSTR *pszHelp) { if (pszHelp) *pszHelp = NULL; return S_FALSE; }
    STDMETHODIMP get_accHelpTopic(BSTR *pszHelpFile, VARIANT, long *pidTopic)
    {
        if (pszHelpFile) *pszHelpFile = NULL;
        if (pidTopic) *pidTopic = 0;
        return E_NOTIMPL;
    }
    STDMETHODIMP get_accKeyboardShortcut(VARIANT varChild, BSTR *pszKeyboardShortcut)
    {
        ILeewayRibbon *r; Surface *s;
        int i = Child(varChild, &r, &s);
        if (i == -2) return E_INVALIDARG;
        if (pszKeyboardShortcut) *pszKeyboardShortcut = NULL;
        if (i < 0) return S_FALSE;
        const Item *it = &s->items[i];
        WCHAR accel[96] = {0};
        if (it->kind == IT_CONTROL || it->kind == IT_QAT || it->kind == IT_HELP || it->kind == IT_LAUNCHER) AccelText(r, it->cmd, accel, 96);
        if (accel[0]) return AccString(accel, pszKeyboardShortcut);
        const WCHAR *k = EffectiveKeytip(r, it);
        if (!k) return S_FALSE;
        WCHAR buf[64];
        BOOL band = it->kind == IT_CONTROL || it->kind == IT_SPLITARROW || it->kind == IT_GROUPBTN || it->kind == IT_GALEXPAND || it->kind == IT_LAUNCHER;
        const WCHAR *tab_keytip = (band && !m_popup && r->layout && it->tab < r->layout->tab_count) ? r->layout->tabs[it->tab].keytip : NULL;
        if (tab_keytip) wsprintfW(buf, L"Alt, %s, %s", tab_keytip, k);
        else wsprintfW(buf, L"Alt, %s", k);
        return AccString(buf, pszKeyboardShortcut);
    }
    STDMETHODIMP get_accFocus(VARIANT *pvarChild)
    {
        if (!pvarChild) return E_POINTER;
        pvarChild->vt = VT_EMPTY;
        ILeewayRibbon *r; Surface *s;
        if (!Resolve(&r, &s) || GetFocus() != m_hwnd) return S_FALSE;
        pvarChild->vt = VT_I4;
        pvarChild->lVal = s->focus >= 0 ? s->focus + 1 : CHILDID_SELF;
        return S_OK;
    }
    STDMETHODIMP get_accSelection(VARIANT *pvarChildren)
    {
        if (pvarChildren) pvarChildren->vt = VT_EMPTY;
        return S_FALSE;
    }
    STDMETHODIMP get_accDefaultAction(VARIANT varChild, BSTR *pszDefaultAction)
    {
        ILeewayRibbon *r; Surface *s;
        int i = Child(varChild, &r, &s);
        if (i == -2) return E_INVALIDARG;
        if (pszDefaultAction) *pszDefaultAction = NULL;
        if (i < 0) return S_FALSE;
        return AccString(AccDefaultActionOf(&s->items[i]), pszDefaultAction);
    }
    STDMETHODIMP accSelect(long flagsSelect, VARIANT varChild)
    {
        ILeewayRibbon *r; Surface *s;
        int i = Child(varChild, &r, &s);
        if (i == -2) return E_INVALIDARG;
        if (flagsSelect & SELFLAG_TAKEFOCUS) {
            if (i >= 0 && !m_popup) r->s.focus = i;
            SetFocus(m_hwnd);
            InvalidateRect(m_hwnd, NULL, FALSE);
            return S_OK;
        }
        return E_INVALIDARG;
    }
    STDMETHODIMP accLocation(long *pxLeft, long *pyTop, long *pcxWidth, long *pcyHeight, VARIANT varChild)
    {
        if (!pxLeft || !pyTop || !pcxWidth || !pcyHeight) return E_POINTER;
        ILeewayRibbon *r; Surface *s;
        int i = Child(varChild, &r, &s);
        if (i == -2) return E_INVALIDARG;
        RECT rc;
        if (i < 0) GetClientRect(m_hwnd, &rc);
        else rc = s->items[i].rc;
        MapWindowPoints(m_hwnd, NULL, (POINT *)&rc, 2);
        *pxLeft = rc.left;
        *pyTop = rc.top;
        *pcxWidth = rc.right - rc.left;
        *pcyHeight = rc.bottom - rc.top;
        return S_OK;
    }
    STDMETHODIMP accNavigate(long navDir, VARIANT varStart, VARIANT *pvarEndUpAt)
    {
        if (!pvarEndUpAt) return E_POINTER;
        pvarEndUpAt->vt = VT_EMPTY;
        ILeewayRibbon *r; Surface *s;
        int i = Child(varStart, &r, &s);
        if (i == -2) return E_INVALIDARG;
        int n = (int)s->n;
        int target = -1;
        switch (navDir) {
        case NAVDIR_FIRSTCHILD: if (i < 0 && n) target = 0; break;
        case NAVDIR_LASTCHILD: if (i < 0 && n) target = n - 1; break;
        case NAVDIR_NEXT: if (i >= 0 && i + 1 < n) target = i + 1; break;
        case NAVDIR_PREVIOUS: if (i > 0) target = i - 1; break;
        default: break;
        }
        if (target < 0) return S_FALSE;
        pvarEndUpAt->vt = VT_I4;
        pvarEndUpAt->lVal = target + 1;
        return S_OK;
    }
    STDMETHODIMP accHitTest(long xLeft, long yTop, VARIANT *pvarChild)
    {
        if (!pvarChild) return E_POINTER;
        pvarChild->vt = VT_EMPTY;
        ILeewayRibbon *r; Surface *s;
        if (!Resolve(&r, &s)) return S_FALSE;
        POINT pt = {xLeft, yTop};
        ScreenToClient(m_hwnd, &pt);
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        if (!PtInRect(&rc, pt)) return S_FALSE;
        int h = HitTest(s, pt.x, pt.y);
        pvarChild->vt = VT_I4;
        pvarChild->lVal = h >= 0 ? h + 1 : CHILDID_SELF;
        return S_OK;
    }
    STDMETHODIMP accDoDefaultAction(VARIANT varChild)
    {
        ILeewayRibbon *r; Surface *s;
        int i = Child(varChild, &r, &s);
        if (i == -2) return E_INVALIDARG;
        if (i < 0) return S_FALSE;
        if (!AccDefaultActionOf(&s->items[i])) return S_FALSE;
        if (m_popup) PopupActivate((Popup *)GetWindowLongPtrW(m_hwnd, GWLP_USERDATA), i);
        else MainActivate(r, i, FALSE);
        return S_OK;
    }
    STDMETHODIMP put_accName(VARIANT, BSTR) { return E_NOTIMPL; }
    STDMETHODIMP put_accValue(VARIANT, BSTR) { return E_NOTIMPL; }

private:
    BOOL Resolve(ILeewayRibbon **r, Surface **s)
    {
        *r = NULL;
        *s = NULL;
        if (!IsWindow(m_hwnd)) return FALSE;
        if (m_popup) {
            Popup *p = (Popup *)GetWindowLongPtrW(m_hwnd, GWLP_USERDATA);
            if (!p) return FALSE;
            *r = p->r;
            *s = &p->s;
            return TRUE;
        }
        ILeewayRibbon *rb = (ILeewayRibbon *)GetWindowLongPtrW(m_hwnd, GWLP_USERDATA);
        if (!rb) return FALSE;
        EnsureLayout(rb);
        *r = rb;
        *s = &rb->s;
        return TRUE;
    }
    /* -1 = self, -2 = invalid, else item index */
    int Child(VARIANT v, ILeewayRibbon **r, Surface **s)
    {
        if (!Resolve(r, s)) return -2;
        if (v.vt == VT_EMPTY || (v.vt == VT_I4 && v.lVal == CHILDID_SELF)) return -1;
        if (v.vt != VT_I4 || v.lVal < 1 || v.lVal > (long)(*s)->n) return -2;
        return (int)v.lVal - 1;
    }

    LONG m_ref;
    HWND m_hwnd;
    BOOL m_popup;
};

static LRESULT AccessibleFromWindow(HWND hwnd, BOOL is_popup, WPARAM wp)
{
    LoadAccessibility();
    if (!g_LresultFromObject) return 0;
    void *mem = HeapAlloc(GetProcessHeap(), 0, sizeof(RibbonAccessible));
    if (!mem) return 0;
    RibbonAccessible *acc = new (mem) RibbonAccessible(hwnd, is_popup);
    LRESULT res = g_LresultFromObject(LWUI_IID_IAccessible, wp, static_cast<IUnknown *>(static_cast<IAccessible *>(acc)));
    acc->Release();
    return res;
}

static void AccFocusEvent(ILeewayRibbon *r)
{
    if (r->hwnd && GetFocus() == r->hwnd) NotifyWinEvent(EVENT_OBJECT_FOCUS, r->hwnd, OBJID_CLIENT, r->s.focus >= 0 ? r->s.focus + 1 : CHILDID_SELF);
}

/* ------------------------------------------------------------------ */
/* KeyTips (G18) */

static WCHAR UpperChar(WCHAR c)
{
    return (c >= L'a' && c <= L'z') ? (WCHAR)(c - 32) : c;
}

static BOOL KeytipEquals(const WCHAR *k, const WCHAR *buf)
{
    if (!k) return FALSE;
    for (; *k && *buf; k++, buf++) if (UpperChar(*k) != UpperChar(*buf)) return FALSE;
    return *k == 0 && *buf == 0;
}

static BOOL KeytipStartsWith(const WCHAR *k, const WCHAR *buf)
{
    if (!k) return FALSE;
    for (; *buf; k++, buf++) if (!*k || UpperChar(*k) != UpperChar(*buf)) return FALSE;
    return TRUE;
}

/* Returns TRUE when the character was consumed by the surface's keytips. */
static BOOL KeytipFeed(ILeewayRibbon *r, Surface *s, WCHAR *buf, BOOL popup_keytips, WCHAR c, Popup *p)
{
    int len = lstrlenW(buf);
    if (len >= MAX_KEYTIP - 1) len = 0;
    buf[len] = UpperChar(c);
    buf[len + 1] = 0;
    int exact = -1;
    BOOL partial = FALSE;
    for (UINT32 i = 0; i < s->n; i++) {
        Item *it = &s->items[i];
        if (!KeytipShownFor(r, it, popup_keytips)) continue;
        const WCHAR *k = EffectiveKeytip(r, it);
        if (!k) continue;
        if (KeytipEquals(k, buf)) { exact = (int)i; break; }
        if (KeytipStartsWith(k, buf)) partial = TRUE;
    }
    if (exact >= 0) {
        buf[0] = 0;
        if (p) PopupActivate(p, exact);
        else MainActivate(r, exact, TRUE);
        return TRUE;
    }
    if (!partial) {
        buf[0] = 0;
        MessageBeep(MB_OK);
    }
    return TRUE;
}

static void KeytipEnter(ILeewayRibbon *r)
{
    if (!r->layout) return;
    HideTip(r);
    r->kt_level = 1;
    r->kt_buf[0] = 0;
    InvalidateRect(r->hwnd, NULL, FALSE);
}

static void KeytipBack(ILeewayRibbon *r)
{
    if (r->kt_level == 2) {
        r->kt_level = 1;
        r->kt_buf[0] = 0;
        InvalidateRect(r->hwnd, NULL, FALSE);
    } else {
        KeytipExit(r);
    }
}

static BOOL IsKeytipKey(UINT vk)
{
    return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z');
}

/* ------------------------------------------------------------------ */
/* Ribbon window procedure */

static LRESULT CALLBACK RibbonWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ILeewayRibbon *r = (ILeewayRibbon *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        r = (ILeewayRibbon *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)r);
        r->hwnd = hwnd;
        return 0;
    }
    if (!r) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_GETOBJECT:
        if ((LONG)lp == OBJID_CLIENT) return AccessibleFromWindow(hwnd, FALSE, wp);
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HGDIOBJ ob = SelectObject(mem, bmp);
            HFONT of = (HFONT)SelectObject(mem, r->font);
            PaintMain(r, mem, &rc);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, of);
            SelectObject(mem, ob);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        Relayout(r);
        return 0;
    case WM_MOUSEMOVE: {
        EnsureLayout(r);
        if (!r->tracking) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
            r->tracking = TRUE;
        }
        int h = HitTest(&r->s, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h != r->s.hover) {
            r->s.hover = h;
            InvalidateRect(hwnd, NULL, FALSE);
            HideTip(r);
            if (h >= 0) SetTimer(hwnd, TIMER_TIP, 600, NULL);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        r->tracking = FALSE;
        if (r->s.hover != -1) {
            r->s.hover = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        HideTip(r);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_TIP) {
            KillTimer(hwnd, TIMER_TIP);
            if (r->s.hover >= 0 && r->s.pressed < 0) ShowTip(r, r->s.hover);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        EnsureLayout(r);
        KeytipExit(r);
        HideTip(r);
        r->s.pressed = HitTest(&r->s, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (r->s.pressed >= 0) SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        int h = HitTest(&r->s, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        int pressed = r->s.pressed;
        r->s.pressed = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        InvalidateRect(hwnd, NULL, FALSE);
        if (h >= 0 && h == pressed) MainActivate(r, h, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        EnsureLayout(r);
        int h = HitTest(&r->s, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h >= 0 && r->s.items[h].kind == IT_TAB) {
            ClosePopup(r);
            LeewayRibbonMinimizeRibbon(r, !r->minimized);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        EnsureLayout(r);
        int h = HitTest(&r->s, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h >= 0 && r->s.items[h].kind == IT_GALCELL && r->s.items[h].alt_cmd) Exec(r, r->s.items[h].alt_cmd);
        return 0;
    }
    case WM_COMMAND: {
        Hosted *h = FindHostedByWindow(r, (HWND)lp);
        if (!h) return 0;
        if (HIWORD(wp) == EN_KILLFOCUS) {
            WCHAR cur[128];
            GetWindowTextW(h->hwnd, cur, 128);
            SendMessageW(h->hwnd, EM_SETSEL, (WPARAM)-1, 0);
            if (lstrcmpW(cur, h->text) != 0) ComboCommitted(r, h);
        }
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        /* the hosted combo's edit takes the theme's popup colours (dark mode included) */
        HDC dc = (HDC)wp;
        SetTextColor(dc, Col(r, LWUI_COLOR_TEXT));
        SetBkColor(dc, Col(r, LWUI_COLOR_POPUP_BG));
        return (LRESULT)Br(r, LWUI_COLOR_POPUP_BG);
    }
    case WM_MOUSEWHEEL: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        EnsureLayout(r);
        int strip_top = (r->layout && !r->qat_below) ? Sc(r, M_QAT_H) : 0;
        if (r->layout && pt.y >= strip_top && pt.y < strip_top + Sc(r, M_STRIP_H) && r->active_tab != 0xFFFFFFFF) {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            UINT32 n = r->layout->tab_count;
            UINT32 i = r->active_tab;
            for (UINT32 step = 0; step < n; step++) {
                i = delta < 0 ? (i + 1) % n : (i + n - 1) % n;
                if (TabVisible(r, i)) { SetActiveTab(r, i); break; }
            }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (r->s.pressed >= 0) {
            r->s.pressed = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
        EnsureLayout(r);
        if (r->s.focus < 0) {
            for (UINT32 i = 0; i < r->s.n; i++) {
                if (r->s.items[i].kind == IT_TAB && r->s.items[i].tab == r->active_tab) { r->s.focus = (int)i; break; }
            }
        }
        InvalidateRect(hwnd, NULL, FALSE);
        AccFocusEvent(r);
        return 0;
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_KEYDOWN: {
        EnsureLayout(r);
        int n = (int)r->s.n;
        int f = r->s.focus;
        switch (wp) {
        case VK_LEFT:
        case VK_RIGHT: {
            int dir = wp == VK_RIGHT ? 1 : -1;
            for (int step = 0; step < n; step++) {
                f = (f + dir + n) % n;
                ItemKind k = r->s.items[f].kind;
                if (k != IT_GROUPFRAME && k != IT_SEPARATOR) break;
            }
            r->s.focus = f;
            InvalidateRect(hwnd, NULL, FALSE);
            AccFocusEvent(r);
            return 0;
        }
        case VK_UP:
        case VK_DOWN: {
            /* move to the item nearest vertically inside the same group column */
            if (f < 0) return 0;
            RECT cur = r->s.items[f].rc;
            int best = -1, bestd = 0x7FFFFFFF;
            for (int i = 0; i < n; i++) {
                if (i == f) continue;
                Item *o = &r->s.items[i];
                if (o->kind == IT_GROUPFRAME || o->kind == IT_SEPARATOR) continue;
                if (wp == VK_DOWN && o->rc.top < cur.bottom) continue;
                if (wp == VK_UP && o->rc.bottom > cur.top) continue;
                int dx = (o->rc.left + o->rc.right) / 2 - (cur.left + cur.right) / 2;
                int dy = (o->rc.top + o->rc.bottom) / 2 - (cur.top + cur.bottom) / 2;
                int d = dx * dx + dy * dy;
                if (d < bestd) { bestd = d; best = i; }
            }
            if (best >= 0) { r->s.focus = best; InvalidateRect(hwnd, NULL, FALSE); AccFocusEvent(r); }
            return 0;
        }
        case VK_SPACE:
        case VK_RETURN:
            if (f >= 0) MainActivate(r, f, FALSE);
            return 0;
        case VK_ESCAPE:
            if (r->parent) SetFocus(r->parent);
            return 0;
        }
        return 0;
    }
    case WM_DPICHANGED_AFTERPARENT: {
        UINT32 dpi = LeewayDpiGetForWindow(hwnd);
        if (dpi && dpi != r->dpi) LeewayRibbonSetDPI(r, dpi);
        return 0;
    }
    case WM_DESTROY:
        ClosePopup(r);
        if (r->tip) { DestroyWindow(r->tip); r->tip = NULL; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void RegisterClasses(void)
{
    static BOOL done = FALSE;
    if (done) return;
    done = TRUE;
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = RibbonWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = RIBBON_CLASS;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    wc.style = CS_DROPSHADOW | CS_SAVEBITS;
    wc.lpfnWndProc = PopupWndProc;
    wc.lpszClassName = POPUP_CLASS;
    RegisterClassW(&wc);
    wc.style = 0;
    wc.lpfnWndProc = TipWndProc;
    wc.lpszClassName = TIP_CLASS;
    RegisterClassW(&wc);
}

/* ------------------------------------------------------------------ */
/* QAT persistence: HKCU\Software\Leeway\<app>\QAT  Items (REG_BINARY UINT32[]), Below (REG_DWORD) */

static BOOL QatKeyPath(ILeewayRibbon *r, WCHAR *path, int cch)
{
    if (!r->layout || !r->layout->app_name || !*r->layout->app_name) return FALSE;
    wsprintfW(path, L"Software\\Leeway\\%s\\QAT", r->layout->app_name);
    (void)cch;
    return TRUE;
}

static void QatApplyDefaults(ILeewayRibbon *r)
{
    r->qat_count = 0;
    if (!r->layout) return;
    for (UINT32 i = 0; i < r->layout->qat_count && r->qat_count < MAX_QAT; i++) {
        UINT32 id = r->layout->qat_default_ids[i];
        if (FindCommand(r, id) || IsFrameworkId(id)) r->qat_ids[r->qat_count++] = id;
    }
}

static void QatLoad(ILeewayRibbon *r)
{
    WCHAR path[256];
    QatApplyDefaults(r);
    r->qat_below = FALSE;
    if (!QatKeyPath(r, path, 256)) return;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    UINT32 ids[MAX_QAT];
    DWORD size = sizeof(ids), type = 0;
    if (RegQueryValueExW(key, L"Items", NULL, &type, (LPBYTE)ids, &size) == ERROR_SUCCESS && type == REG_BINARY) {
        r->qat_count = 0;
        for (UINT32 i = 0; i < size / sizeof(UINT32) && r->qat_count < MAX_QAT; i++) {
            if (FindCommand(r, ids[i]) || IsFrameworkId(ids[i])) r->qat_ids[r->qat_count++] = ids[i];   /* stale ids dropped */
        }
    }
    DWORD below = 0;
    size = sizeof(below);
    if (RegQueryValueExW(key, L"Below", NULL, &type, (LPBYTE)&below, &size) == ERROR_SUCCESS && type == REG_DWORD) r->qat_below = below != 0;
    RegCloseKey(key);
}

static void QatSave(ILeewayRibbon *r)
{
    WCHAR path[256];
    if (!QatKeyPath(r, path, 256)) return;
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(key, L"Items", 0, REG_BINARY, (const BYTE *)r->qat_ids, r->qat_count * sizeof(UINT32));
    DWORD below = r->qat_below ? 1 : 0;
    RegSetValueExW(key, L"Below", 0, REG_DWORD, (const BYTE *)&below, sizeof(below));
    RegCloseKey(key);
}

/* ------------------------------------------------------------------ */
/* Layout copy and validation */

static void FreeLayout(Layout *l)
{
    if (!l) return;
    ArenaFree(&l->arena);
    HeapFree(GetProcessHeap(), 0, l);
}

static BOOL KnownId(ILeewayRibbon *r, UINT32 id)
{
    return FindCommand(r, id) != NULL || IsFrameworkId(id);
}

static void LogUnknownId(const WCHAR *where, UINT32 id)
{
    WCHAR msg[200];
    wsprintfW(msg, L"LeewayUI: SetLayout rejected %s command id 0x%04X: not in the command table -> LWUI_E_UNKNOWN_COMMAND. Add it to the table or remove it from the layout.\n", where, id);
    OutputDebugStringW(msg);
}

static HRESULT ReportCollision(const WCHAR *where, const WCHAR *keytip, UINT32 a, UINT32 b)
{
    WCHAR msg[200];
    wsprintfW(msg, L"LeewayUI: keytip '%s' collides in %s (ids %u and %u): equal or one is a prefix of the other\n", keytip, where, a, b);
    OutputDebugStringW(msg);
    return LWUI_E_KEYTIP_COLLISION;
}

/* Two keytips collide when equal or when one is a prefix of the other (the
 * shorter one would fire before the longer could be typed). */
static BOOL KeytipsCollide(const WCHAR *a, const WCHAR *b)
{
    return KeytipStartsWith(a, b) || KeytipStartsWith(b, a);
}

static HRESULT CheckKeytips(ILeewayRibbon *r, Layout *l)
{
    /* strip level: app button, QAT digits, tab keytips */
    const WCHAR *tips[128];
    UINT32 ids[128];
    UINT32 n = 0;
    tips[n] = L"F"; ids[n++] = LWUI_APPMENU;
    static const WCHAR *const digits[9] = {L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9"};
    for (UINT32 i = 0; i < 9 && n < 128; i++) { tips[n] = digits[i]; ids[n++] = 0; }
    for (UINT32 i = 0; i < l->tab_count && n < 128; i++) {
        if (!l->tabs[i].keytip) continue;
        for (UINT32 j = 0; j < n; j++) {
            if (KeytipsCollide(tips[j], l->tabs[i].keytip)) return ReportCollision(L"the tab strip", l->tabs[i].keytip, ids[j], l->tabs[i].id);
        }
        tips[n] = l->tabs[i].keytip; ids[n++] = l->tabs[i].id;
    }
    /* within each tab */
    for (UINT32 t = 0; t < l->tab_count; t++) {
        n = 0;
        for (UINT32 g = 0; g < l->tabs[t].group_count; g++) {
            const LeewayUIGroup *grp = &l->tabs[t].groups[g];
            if (grp->keytip && n < 128) {
                for (UINT32 j = 0; j < n; j++) if (KeytipsCollide(tips[j], grp->keytip)) return ReportCollision(l->tabs[t].label, grp->keytip, ids[j], 0);
                tips[n] = grp->keytip; ids[n++] = 0;
            }
            for (UINT32 k = 0; k < grp->control_count && n < 128; k++) {
                const LeewayUIPlacement *p = &grp->controls[k];
                const WCHAR *kt = p->keytip;
                if (!kt) {
                    const LeewayUICommand *c = FindCommand(r, p->command_id);
                    kt = c ? c->keytip : NULL;
                }
                if (!kt) continue;
                for (UINT32 j = 0; j < n; j++) {
                    if (ids[j] == p->command_id) continue;   /* the same id placed twice shares its keytip */
                    if (KeytipsCollide(tips[j], kt)) return ReportCollision(l->tabs[t].label, kt, ids[j], p->command_id);
                }
                tips[n] = kt; ids[n++] = p->command_id;
            }
        }
    }
    /* app menu rows */
    n = 0;
    for (UINT32 i = 0; i < l->appmenu_count && n < 128; i++) {
        const LeewayUICommand *c = FindCommand(r, l->appmenu_ids[i]);
        if (!c || !c->keytip) continue;
        for (UINT32 j = 0; j < n; j++) if (KeytipsCollide(tips[j], c->keytip)) return ReportCollision(L"the application menu", c->keytip, ids[j], c->id);
        tips[n] = c->keytip; ids[n++] = c->id;
    }
    return S_OK;
}

static Layout *CopyLayout(ILeewayRibbon *r, const LeewayUILayout *src, HRESULT *hr)
{
    Layout *l = (Layout *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Layout));
    *hr = E_OUTOFMEMORY;
    if (!l) return NULL;
    Arena *a = &l->arena;
    l->app_name = ArenaStr(a, src->app_name);
    l->help_command_id = src->help_command_id;
    l->tab_count = src->tab_count;
    l->tabs = (LeewayUITab *)ArenaAlloc(a, sizeof(LeewayUITab) * (src->tab_count ? src->tab_count : 1));
    if (!l->tabs) goto fail;
    for (UINT32 t = 0; t < src->tab_count; t++) {
        const LeewayUITab *st = &src->tabs[t];
        LeewayUITab *dt = &l->tabs[t];
        dt->id = st->id;
        dt->label = ArenaStr(a, st->label);
        dt->keytip = ArenaStr(a, st->keytip);
        dt->context = ArenaStr(a, st->context);
        dt->group_count = st->group_count;
        LeewayUIGroup *groups = (LeewayUIGroup *)ArenaAlloc(a, sizeof(LeewayUIGroup) * (st->group_count ? st->group_count : 1));
        if (!groups) goto fail;
        dt->groups = groups;
        for (UINT32 g = 0; g < st->group_count; g++) {
            const LeewayUIGroup *sg = &st->groups[g];
            LeewayUIGroup *dg = &groups[g];
            dg->label = ArenaStr(a, sg->label);
            dg->keytip = ArenaStr(a, sg->keytip);
            dg->variant_order = sg->variant_order;
            dg->launcher_id = sg->launcher_id;
            dg->control_count = sg->control_count;
            dg->rows = sg->rows;
            LeewayUIPlacement *pl = (LeewayUIPlacement *)ArenaAlloc(a, sizeof(LeewayUIPlacement) * (sg->control_count ? sg->control_count : 1));
            if (!pl) goto fail;
            dg->controls = pl;
            for (UINT32 k = 0; k < sg->control_count; k++) {
                pl[k].command_id = sg->controls[k].command_id;
                pl[k].kind = sg->controls[k].kind;
                pl[k].keytip = ArenaStr(a, sg->controls[k].keytip);
                pl[k].width = sg->controls[k].width;
                if (!KnownId(r, pl[k].command_id)) { *hr = LWUI_E_UNKNOWN_COMMAND; LogUnknownId(L"tab/group", pl[k].command_id); goto fail; }
            }
            if (dg->launcher_id && !KnownId(r, dg->launcher_id)) { *hr = LWUI_E_UNKNOWN_COMMAND; LogUnknownId(L"group launcher", dg->launcher_id); goto fail; }
        }
    }
    l->appmenu_count = src->appmenu_count;
    l->appmenu_ids = (UINT32 *)ArenaAlloc(a, sizeof(UINT32) * (src->appmenu_count ? src->appmenu_count : 1));
    if (!l->appmenu_ids) goto fail;
    for (UINT32 i = 0; i < src->appmenu_count; i++) {
        l->appmenu_ids[i] = src->appmenu_ids[i];
        if (!KnownId(r, src->appmenu_ids[i])) { *hr = LWUI_E_UNKNOWN_COMMAND; LogUnknownId(L"app-menu", src->appmenu_ids[i]); goto fail; }
    }
    l->qat_count = src->qat_count;
    l->qat_default_ids = (UINT32 *)ArenaAlloc(a, sizeof(UINT32) * (src->qat_count ? src->qat_count : 1));
    if (!l->qat_default_ids) goto fail;
    for (UINT32 i = 0; i < src->qat_count; i++) {
        l->qat_default_ids[i] = src->qat_default_ids[i];
        if (!KnownId(r, src->qat_default_ids[i])) { *hr = LWUI_E_UNKNOWN_COMMAND; LogUnknownId(L"QAT", src->qat_default_ids[i]); goto fail; }
    }
    if (l->help_command_id && !KnownId(r, l->help_command_id)) { *hr = LWUI_E_UNKNOWN_COMMAND; LogUnknownId(L"help button", l->help_command_id); goto fail; }
    *hr = CheckKeytips(r, l);
    if (FAILED(*hr)) goto fail;
    return l;
fail:
    if (*hr == LWUI_E_UNKNOWN_COMMAND) OutputDebugStringW(L"LeewayUI: layout references a command id that is not in the table\n");
    FreeLayout(l);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API */

HRESULT WINAPI LeewayRibbonCreate(HWND parent, const LeewayUICommand *table, UINT32 count,
                                  LeewayUICommandExecuteHandler execute,
                                  LeewayUICommandStateHandler state,
                                  void *app_context, ILeewayRibbon **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!parent || (!table && count)) {
        OutputDebugStringW(L"LeewayUI: LeewayRibbonCreate E_INVALIDARG (null parent, or null table with count>0)\n");
        return E_INVALIDARG;
    }

    ILeewayRibbon *r = (ILeewayRibbon *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ILeewayRibbon));
    if (!r) return E_OUTOFMEMORY;
    r->parent = parent;
    r->execute = execute;
    r->state = state;
    r->ctx = app_context;
    r->active_tab = 0xFFFFFFFF;
    r->restore_tab = 0;
    r->s.hover = r->s.pressed = r->s.focus = -1;
    r->tip_item = -1;
    r->dirty = TRUE;

    /* copy the table (structures and strings) */
    r->count = count;
    r->table = (LeewayUICommand *)ArenaAlloc(&r->table_arena, sizeof(LeewayUICommand) * (count ? count : 1));
    if (!r->table) { LeewayRibbonDestroy(r); return E_OUTOFMEMORY; }
    for (UINT32 i = 0; i < count; i++) {
        r->table[i] = table[i];
        r->table[i].label = ArenaStr(&r->table_arena, table[i].label);
        r->table[i].tooltip = ArenaStr(&r->table_arena, table[i].tooltip);
        r->table[i].keytip = ArenaStr(&r->table_arena, table[i].keytip);
    }

    if (FAILED(LeewayThemeCreate(&r->theme))) { LeewayRibbonDestroy(r); return E_OUTOFMEMORY; }
    r->dpi = LeewayDpiGetForWindow(parent);
    if (!r->dpi) r->dpi = 96;
    CreateFonts(r);

    RegisterClasses();
    RECT prc;
    GetClientRect(parent, &prc);
    HWND hwnd = CreateWindowExW(0, RIBBON_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                0, 0, prc.right, (int)ComputeHeight(r), parent, (HMENU)(UINT_PTR)RIBBON_CTRL_ID,
                                GetModuleHandleW(NULL), r);
    if (!hwnd) {
        WCHAR m[96]; wsprintfW(m, L"LeewayUI: LeewayRibbonCreate CreateWindowEx failed, gle=%lu\n", GetLastError()); OutputDebugStringW(m);
        LeewayRibbonDestroy(r); return HRESULT_FROM_WIN32(GetLastError());
    }
    r->hwnd = hwnd;
    *out = r;
    return S_OK;
}

HRESULT WINAPI LeewayRibbonDestroy(ILeewayRibbon *r)
{
    if (!r) return E_INVALIDARG;
    ClosePopup(r);
    if (r->tip) { DestroyWindow(r->tip); r->tip = NULL; }
    if (r->hwnd) {
        HWND h = r->hwnd;
        r->hwnd = NULL;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }
    ClearIcons(r);
    if (r->icons) HeapFree(GetProcessHeap(), 0, r->icons);
    for (UINT32 i = 0; i < r->coll_count; i++) {
        ArenaFree(&r->colls[i]->arena);
        HeapFree(GetProcessHeap(), 0, r->colls[i]);
    }
    if (r->colls) HeapFree(GetProcessHeap(), 0, r->colls);
    if (r->accels) HeapFree(GetProcessHeap(), 0, r->accels);
    FreeLayout(r->layout);
    SurfaceFree(&r->s);
    DropGdi(r);
    if (r->theme) LeewayThemeDestroy(r->theme);
    ArenaFree(&r->table_arena);
    HeapFree(GetProcessHeap(), 0, r);
    return S_OK;
}

HWND WINAPI LeewayRibbonGetHWND(ILeewayRibbon *r)
{
    return r ? r->hwnd : NULL;
}

HWND WINAPI LeewayRibbonGetWindowHandle(ILeewayRibbon *r)
{
    return r ? r->hwnd : NULL;
}

UINT32 WINAPI LeewayRibbonGetHeight(ILeewayRibbon *r)
{
    return r ? ComputeHeight(r) : 0;
}

HRESULT WINAPI LeewayRibbonSetLayout(ILeewayRibbon *r, const LeewayUILayout *layout)
{
    if (!r || !layout) return E_INVALIDARG;
    HRESULT hr;
    Layout *l = CopyLayout(r, layout, &hr);
    if (!l) return hr;
    ClosePopup(r);
    DestroyHosted(r);
    r->hosted_count = 0;
    r->strip_count = 0;
    FreeLayout(r->layout);
    r->layout = l;
    if (l->app_name) LeewayThemeApplyAppOverride(r->theme, l->app_name);
    r->active_tab = FirstVisibleTab(r);
    r->restore_tab = r->active_tab == 0xFFFFFFFF ? 0 : r->active_tab;
    r->s.focus = -1;
    QatLoad(r);
    Relayout(r);
    NotifyParent(r, LWUI_RBN_HEIGHTCHANGED);
    return S_OK;
}

HRESULT WINAPI LeewayRibbonSetCollection(ILeewayRibbon *r, UINT32 command_id, const LeewayUICollectionItem *items, UINT32 count, UINT32 flags)
{
    if (!r || (!items && count)) return E_INVALIDARG;
    for (UINT32 i = 0; i < r->coll_count; i++) {
        if (r->colls[i]->cmd == command_id) {
            ArenaFree(&r->colls[i]->arena);
            HeapFree(GetProcessHeap(), 0, r->colls[i]);
            r->colls[i] = r->colls[--r->coll_count];
            break;
        }
    }
    if (count == 0) return S_OK;
    if (r->coll_count == r->coll_cap) {
        UINT32 cap = r->coll_cap ? r->coll_cap * 2 : 8;
        Collection **nc = (Collection **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(Collection *));
        if (!nc) return E_OUTOFMEMORY;
        if (r->colls) {
            CopyMemory(nc, r->colls, r->coll_count * sizeof(Collection *));
            HeapFree(GetProcessHeap(), 0, r->colls);
        }
        r->colls = nc;
        r->coll_cap = cap;
    }
    Collection *c = (Collection *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Collection));
    if (!c) return E_OUTOFMEMORY;
    c->cmd = command_id;
    c->flags = flags;
    c->count = count;
    c->items = (LeewayUICollectionItem *)ArenaAlloc(&c->arena, sizeof(LeewayUICollectionItem) * count);
    if (!c->items) { HeapFree(GetProcessHeap(), 0, c); return E_OUTOFMEMORY; }
    for (UINT32 i = 0; i < count; i++) {
        c->items[i].id = items[i].id;
        c->items[i].icon_id = items[i].icon_id;
        c->items[i].label = ArenaStr(&c->arena, items[i].label);
        c->items[i].color = items[i].color;
        c->items[i].alt_id = items[i].alt_id;
    }
    r->colls[r->coll_count++] = c;
    r->coll_gen++;
    Relayout(r);
    return S_OK;
}

HRESULT WINAPI LeewayRibbonGetComboText(ILeewayRibbon *r, UINT32 command_id, WCHAR *buffer, UINT32 *cch)
{
    if (!r || !cch) return E_INVALIDARG;
    Hosted *h = FindHosted(r, command_id);
    const WCHAR *text = h ? h->text : L"";
    UINT32 need = (UINT32)lstrlenW(text) + 1;
    if (!buffer || *cch < need) {
        *cch = need;
        return buffer ? E_NOT_SUFFICIENT_BUFFER : S_OK;
    }
    lstrcpynW(buffer, text, (int)*cch);
    *cch = need;
    return S_OK;
}

HRESULT WINAPI LeewayRibbonSetContext(ILeewayRibbon *r, const WCHAR *name, BOOL active)
{
    if (!r || !name || !*name) return E_INVALIDARG;
    Context *c = NULL;
    for (UINT32 i = 0; i < r->context_count; i++) {
        if (lstrcmpiW(r->contexts[i].name, name) == 0) { c = &r->contexts[i]; break; }
    }
    if (!c) {
        if (r->context_count == MAX_CONTEXTS) return E_OUTOFMEMORY;
        c = &r->contexts[r->context_count++];
        lstrcpynW(c->name, name, 32);
        c->active = FALSE;
    }
    if (c->active == (active ? TRUE : FALSE)) return S_OK;
    c->active = active ? TRUE : FALSE;
    ClosePopup(r);
    if (r->layout) {
        if (active) {
            for (UINT32 i = 0; i < r->layout->tab_count; i++) {
                if (r->layout->tabs[i].context && lstrcmpiW(r->layout->tabs[i].context, name) == 0) {
                    if (r->active_tab != 0xFFFFFFFF && !r->layout->tabs[r->active_tab].context) r->restore_tab = r->active_tab;
                    r->active_tab = i;   /* auto-select on appear */
                    break;
                }
            }
        } else if (r->active_tab != 0xFFFFFFFF && r->layout->tabs[r->active_tab].context &&
                   lstrcmpiW(r->layout->tabs[r->active_tab].context, name) == 0) {
            r->active_tab = TabVisible(r, r->restore_tab) ? r->restore_tab : FirstVisibleTab(r);
        }
    }
    Relayout(r);
    return S_OK;
}

/* ---- recent files (G5) ---- */
static void RecentRegBase(ILeewayRibbon *r, WCHAR *out, int cch)
{
    const WCHAR *app = (r->layout && r->layout->app_name) ? r->layout->app_name : NULL;
    if (!app) { out[0] = 0; return; }
    wsprintfW(out, L"Software\\Leeway\\%s\\Recent", app); (void)cch;
    out[cch - 1] = 0;
}

static const WCHAR *BaseName(const WCHAR *path)
{
    const WCHAR *b = path;
    for (const WCHAR *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') b = p + 1;
    return b;
}

static void ReadRecent(ILeewayRibbon *r)
{
    r->recent_count = 0;
    WCHAR key[256]; RecentRegBase(r, key, 256);
    if (!key[0]) return;
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return;
    UINT32 cap = (r->recent_max && r->recent_max < MAX_RECENT) ? r->recent_max : MAX_RECENT;
    for (UINT32 i = 0; i < cap; i++) {
        WCHAR name[16]; wsprintfW(name, L"%u", i);
        DWORD type = 0, cb = sizeof(r->recent_paths[0]);
        if (RegQueryValueExW(h, name, NULL, &type, (BYTE *)r->recent_paths[r->recent_count], &cb) != ERROR_SUCCESS || type != REG_SZ)
            break;
        r->recent_paths[r->recent_count][MAX_PATH - 1] = 0;
        if (!r->recent_paths[r->recent_count][0]) break;
        r->recent_names[r->recent_count] = BaseName(r->recent_paths[r->recent_count]);
        r->recent_count++;
    }
    RegCloseKey(h);
}

static void WriteRecent(ILeewayRibbon *r)
{
    WCHAR key[256]; RecentRegBase(r, key, 256);
    if (!key[0]) return;
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &h, NULL) != ERROR_SUCCESS) return;
    for (UINT32 i = 0; i < MAX_RECENT; i++) { WCHAR nm[16]; wsprintfW(nm, L"%u", i); RegDeleteValueW(h, nm); }
    for (UINT32 i = 0; i < r->recent_count; i++) {
        WCHAR nm[16]; wsprintfW(nm, L"%u", i);
        RegSetValueExW(h, nm, 0, REG_SZ, (const BYTE *)r->recent_paths[i],
                       (DWORD)((lstrlenW(r->recent_paths[i]) + 1) * sizeof(WCHAR)));
    }
    RegCloseKey(h);
}

HRESULT WINAPI LeewayRibbonSetRecentProvider(ILeewayRibbon *r, LeewayUIRecentOpenHandler on_open, void *context, UINT32 max_items)
{
    if (!r) return E_INVALIDARG;
    r->recent_open = on_open;
    r->recent_ctx = context;
    r->recent_max = (max_items > MAX_RECENT) ? MAX_RECENT : max_items;
    return S_OK;
}

HRESULT WINAPI LeewayRibbonAddRecentFile(ILeewayRibbon *r, const WCHAR *path)
{
    if (!r || !path || !path[0]) return E_INVALIDARG;
    ReadRecent(r);
    WCHAR tmp[MAX_RECENT][MAX_PATH]; UINT32 n = 0;
    lstrcpynW(tmp[n++], path, MAX_PATH);
    for (UINT32 i = 0; i < r->recent_count && n < MAX_RECENT; i++)
        if (lstrcmpiW(r->recent_paths[i], path) != 0) lstrcpynW(tmp[n++], r->recent_paths[i], MAX_PATH);
    UINT32 cap = r->recent_max ? ((r->recent_max < MAX_RECENT) ? r->recent_max : MAX_RECENT) : MAX_RECENT;
    if (n > cap) n = cap;
    for (UINT32 i = 0; i < n; i++) { lstrcpynW(r->recent_paths[i], tmp[i], MAX_PATH); r->recent_names[i] = BaseName(r->recent_paths[i]); }
    r->recent_count = n;
    WriteRecent(r);
    return S_OK;
}

HRESULT WINAPI LeewayRibbonSetGalleryDrawCallback(ILeewayRibbon *r, LeewayUIGalleryDrawHandler handler, void *context)
{
    if (!r) return E_INVALIDARG;
    r->gallery_draw = handler;
    r->gallery_draw_ctx = context;
    return S_OK;
}

HRESULT WINAPI LeewayRibbonSetThemePreference(ILeewayRibbon *r, LeewayThemePref pref)
{
    if (!r) return E_INVALIDARG;
    LeewayThemeSetPreference(r->theme, pref);
    return LeewayRibbonOnThemeChanged(r);
}

HRESULT WINAPI LeewayRibbonSetIconResolver(ILeewayRibbon *r, LeewayUIIconResolver resolver, void *resolver_context)
{
    if (!r) return E_INVALIDARG;
    ClearIcons(r);
    r->resolver = resolver;
    r->resolver_ctx = resolver_context;
    Relayout(r);
    return S_OK;
}

HRESULT WINAPI LeewayRibbonSetAccelerators(ILeewayRibbon *r, HACCEL accelerators)
{
    if (!r) return E_INVALIDARG;
    if (r->accels) { HeapFree(GetProcessHeap(), 0, r->accels); r->accels = NULL; }
    r->accel_count = 0;
    if (!accelerators) return S_OK;
    int n = CopyAcceleratorTableW(accelerators, NULL, 0);
    if (n <= 0) return S_OK;
    r->accels = (ACCEL *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ACCEL) * n);
    if (!r->accels) return E_OUTOFMEMORY;
    r->accel_count = (UINT32)CopyAcceleratorTableW(accelerators, r->accels, n);
    return S_OK;
}

HRESULT WINAPI LeewayRibbonInvalidateCommand(ILeewayRibbon *r, UINT32 command_id)
{
    if (!r) return E_INVALIDARG;
    /* visibility or a label override may change widths: relayout, not just repaint */
    (void)command_id;
    Relayout(r);
    if (r->popup) InvalidateRect(r->popup->hwnd, NULL, FALSE);
    return S_OK;
}

HRESULT WINAPI LeewayRibbonInvalidateAll(ILeewayRibbon *r)
{
    if (!r) return E_INVALIDARG;
    Relayout(r);
    if (r->popup) InvalidateRect(r->popup->hwnd, NULL, FALSE);
    return S_OK;
}

HRESULT WINAPI LeewayRibbonSetActiveTab(ILeewayRibbon *r, UINT32 tab_id)
{
    if (!r || !r->layout) return E_INVALIDARG;
    for (UINT32 i = 0; i < r->layout->tab_count; i++) {
        if (r->layout->tabs[i].id == tab_id) {
            if (!TabVisible(r, i)) return E_ACCESSDENIED;
            ClosePopup(r);
            SetActiveTab(r, i);
            return S_OK;
        }
    }
    return E_INVALIDARG;
}

UINT32 WINAPI LeewayRibbonGetActiveTab(ILeewayRibbon *r)
{
    if (!r || !r->layout || r->active_tab == 0xFFFFFFFF) return 0;
    return r->layout->tabs[r->active_tab].id;
}

HRESULT WINAPI LeewayRibbonMinimizeRibbon(ILeewayRibbon *r, BOOL minimize)
{
    if (!r) return E_INVALIDARG;
    minimize = minimize ? TRUE : FALSE;
    if (r->minimized == minimize) return S_OK;
    ClosePopup(r);
    r->minimized = minimize;
    Relayout(r);
    NotifyParent(r, LWUI_RBN_MINIMIZED);
    NotifyParent(r, LWUI_RBN_HEIGHTCHANGED);
    return S_OK;
}

BOOL WINAPI LeewayRibbonIsMinimized(ILeewayRibbon *r)
{
    return r ? r->minimized : FALSE;
}

HRESULT WINAPI LeewayRibbonGetQAT(ILeewayRibbon *r, UINT32 *ids, UINT32 *count)
{
    if (!r || !count) return E_INVALIDARG;
    if (!ids || *count < r->qat_count) {
        *count = r->qat_count;
        return ids ? E_NOT_SUFFICIENT_BUFFER : S_OK;
    }
    CopyMemory(ids, r->qat_ids, r->qat_count * sizeof(UINT32));
    *count = r->qat_count;
    return S_OK;
}

HRESULT WINAPI LeewayRibbonSetQAT(ILeewayRibbon *r, const UINT32 *ids, UINT32 count)
{
    if (!r || (!ids && count)) return E_INVALIDARG;
    r->qat_count = 0;
    for (UINT32 i = 0; i < count && r->qat_count < MAX_QAT; i++) {
        if (FindCommand(r, ids[i]) || IsFrameworkId(ids[i])) r->qat_ids[r->qat_count++] = ids[i];
    }
    QatSave(r);
    Relayout(r);
    NotifyParent(r, LWUI_RBN_QATCHANGED);
    return S_OK;
}

HRESULT WINAPI LeewayRibbonOnThemeChanged(ILeewayRibbon *r)
{
    if (!r) return E_INVALIDARG;
    ClosePopup(r);
    LeewayThemeOnThemeChange(r->theme);
    DropGdi(r);
    CreateFonts(r);
    ClearIcons(r);
    for (UINT32 i = 0; i < r->hosted_count; i++) {
        if (r->hosted[i].hwnd) InvalidateRect(r->hosted[i].hwnd, NULL, TRUE);
    }
    Relayout(r);
    return S_OK;
}

HRESULT WINAPI LeewayRibbonSetDPI(ILeewayRibbon *r, UINT32 dpi)
{
    if (!r || !dpi) return E_INVALIDARG;
    if (r->dpi == dpi) return S_OK;
    ClosePopup(r);
    r->dpi = dpi;
    DropGdi(r);
    CreateFonts(r);
    ClearIcons(r);
    Relayout(r);
    NotifyParent(r, LWUI_RBN_HEIGHTCHANGED);
    return S_OK;
}

BOOL WINAPI LeewayRibbonPreTranslateMessage(ILeewayRibbon *r, MSG *msg)
{
    if (!r || !msg || !r->hwnd) return FALSE;
    /* only messages for our top-level window's thread matter */
    switch (msg->message) {
    case WM_SYSKEYDOWN:
        if (msg->wParam == VK_MENU) {
            if (!(msg->lParam & (1 << 30))) r->alt_pending = TRUE;   /* not a repeat */
            return FALSE;
        }
        r->alt_pending = FALSE;
        if (msg->wParam == VK_F10 && !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            if (r->popup) ClosePopup(r);
            if (r->kt_level) KeytipExit(r); else KeytipEnter(r);
            return TRUE;
        }
        if (r->kt_level && IsKeytipKey((UINT)msg->wParam)) {
            KeytipFeed(r, &r->s, r->kt_buf, FALSE, (WCHAR)msg->wParam, NULL);
            return TRUE;
        }
        return FALSE;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (msg->wParam == VK_MENU) {
            BOOL enter = r->alt_pending;
            r->alt_pending = FALSE;
            if (enter) {
                if (r->popup) ClosePopup(r);
                if (r->kt_level) KeytipExit(r); else KeytipEnter(r);
                return TRUE;
            }
        }
        return FALSE;
    case WM_KEYDOWN:
        r->alt_pending = FALSE;
        if (msg->wParam == VK_F1 && (GetKeyState(VK_CONTROL) & 0x8000)) {
            LeewayRibbonMinimizeRibbon(r, !r->minimized);
            return TRUE;
        }
        {
            /* Enter commits, Esc reverts, inside a hosted combo box */
            Hosted *h = FindHostedByWindow(r, msg->hwnd);
            if (h) {
                if (msg->wParam == VK_RETURN) {
                    ComboCommitted(r, h);
                    if (r->parent) SetFocus(r->parent);
                    return TRUE;
                }
                if (msg->wParam == VK_ESCAPE) {
                    SetWindowTextW(h->hwnd, h->text);
                    if (r->parent) SetFocus(r->parent);
                    return TRUE;
                }
                if (msg->wParam == VK_DOWN || msg->wParam == VK_F4) {
                    EnsureLayout(r);
                    for (UINT32 i = 0; i < r->s.n; i++) {
                        Item *it = &r->s.items[i];
                        if (it->kind == IT_CONTROL && it->ckind == LWUI_CONTROL_COMBO && it->cmd == h->cmd) {
                            OpenPopup(r, PK_MENU, &it->rc, r->hwnd, it->cmd, NULL, it->tab, TRUE);
                            break;
                        }
                    }
                    return TRUE;
                }
                return FALSE;
            }
        }
        if (r->popup) {
            Popup *p = r->popup;
            if (msg->wParam == VK_ESCAPE) {
                ClosePopup(r);
                if (r->kt_level == 2) r->kt_level = 1;
                InvalidateRect(r->hwnd, NULL, FALSE);
                return TRUE;
            }
            if (p->keytips && IsKeytipKey((UINT)msg->wParam)) {
                KeytipFeed(r, &p->s, p->kt_buf, TRUE, (WCHAR)msg->wParam, p);
                return TRUE;
            }
            return FALSE;
        }
        if (r->kt_level) {
            if (msg->wParam == VK_ESCAPE) { KeytipBack(r); return TRUE; }
            if (IsKeytipKey((UINT)msg->wParam)) {
                KeytipFeed(r, &r->s, r->kt_buf, FALSE, (WCHAR)msg->wParam, NULL);
                return TRUE;
            }
            KeytipExit(r);
            return FALSE;
        }
        return FALSE;
    case WM_CHAR:
    case WM_SYSCHAR:
        /* swallow the character that a consumed keytip key would otherwise produce */
        return (r->kt_level || (r->popup && r->popup->keytips)) ? TRUE : FALSE;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (r->kt_level && msg->hwnd != r->hwnd) KeytipExit(r);
        return FALSE;
    }
    return FALSE;
}
