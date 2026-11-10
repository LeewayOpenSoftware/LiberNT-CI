/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Ribbon control header - F1 framework for tabs, groups, buttons, galleries, QAT, app menu
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 */

#pragma once

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Command and State Management ===== */

typedef struct {
    UINT32 id;
    WCHAR *label;
    WCHAR *tooltip;
    WCHAR *keytip;
    UINT32 icon_id;
    BOOL enabled;
    BOOL checked;
    BOOL visible;
} LeewayUICommand;

typedef HRESULT (*LeewayUICommandExecuteHandler)(UINT32 id, void *app_context);
typedef HRESULT (*LeewayUICommandStateHandler)(UINT32 id, LeewayUICommand *out, void *app_context);

/* ===== Control Kinds (F1 Controls) ===== */

typedef enum {
    LWUI_CONTROL_BUTTON_LARGE = 0,    /* 32x32 icon + label */
    LWUI_CONTROL_BUTTON_SMALL,         /* 16x16 icon, optional label */
    LWUI_CONTROL_BUTTON_SMALL_ICON,    /* 16x16 icon only */
    LWUI_CONTROL_BUTTON_TOGGLE,        /* Toggle state (checked/unchecked) */
    LWUI_CONTROL_BUTTON_SPLIT,         /* Primary action + dropdown arrow */
    LWUI_CONTROL_BUTTON_DROPDOWN,      /* Dropdown menu */
    LWUI_CONTROL_GALLERY,              /* Grid of items with scrolling */
    LWUI_CONTROL_COMBO,                /* Combo box (framework gap, not F1) */
    LWUI_CONTROL_COLORWELL,            /* Color picker (framework gap, not F1) */
    LWUI_CONTROL_CHECKBOX,             /* Checkbox (framework gap, not F1) */
    LWUI_CONTROL_LAUNCHER,             /* Dialog launcher (framework gap, not F1) */
} LeewayUIControlKind;

/* ===== Layout Tree: Placement with Per-Placement Keytip ===== */

typedef struct LeewayUIPlacement {
    UINT32 command_id;
    LeewayUIControlKind kind;
    WCHAR *keytip;              /* Per-placement keytip (may differ from command keytip) */
    struct LeewayUIPlacement *sibling;
} LeewayUIPlacement;

typedef struct LeewayUIGroup {
    WCHAR *label;
    UINT32 min_columns;         /* Minimum columns in "large" variant */
    UINT32 max_columns;         /* Maximum columns in "small" variant */
    LeewayUIPlacement *controls;
    struct LeewayUIGroup *sibling;
} LeewayUIGroup;

typedef struct LeewayUITab {
    UINT32 id;
    WCHAR *label;
    WCHAR *keytip;
    LeewayUIGroup *groups;
    struct LeewayUITab *sibling;
} LeewayUITab;

/* ===== Collections: Items for Galleries/Dropdowns ===== */

typedef struct LeewayUICollectionItem {
    UINT32 id;                  /* Command ID or item ID */
    WCHAR *label;
    UINT32 icon_id;
    struct LeewayUICollectionItem *sibling;
} LeewayUICollectionItem;

typedef struct LeewayUICollection {
    UINT32 id;                  /* Gallery or dropdown ID */
    LeewayUICollectionItem *items;
    BOOL allow_search;          /* For galleries */
    BOOL show_labels;           /* For galleries */
} LeewayUICollection;

/* ===== Gallery and Dropdown Support ===== */

typedef enum {
    LWUI_LAYOUT_GRID_ROWS,
    LWUI_LAYOUT_GRID_COLS,
    LWUI_LAYOUT_LIST,
} LeewayUIGalleryLayout;

typedef struct {
    UINT32 gallery_id;
    LeewayUIGalleryLayout layout;
    UINT32 grid_cols;           /* For grid layouts */
} LeewayUIGalleryConfig;

/* ===== Split Button Last-Choice Behavior ===== */

typedef struct {
    UINT32 command_id;
    BOOL remember_last_choice;  /* If true, pressing button repeats last dropdown selection */
} LeewayUISplitButtonConfig;

/* ===== Quick Access Toolbar (QAT) ===== */

typedef struct {
    UINT32 command_id;
    LeewayUIControlKind kind;   /* Usually BUTTON_SMALL */
} LeewayUIQATItem;

typedef struct {
    LeewayUIQATItem *items;
    UINT32 count;
} LeewayUIQAT;

/* ===== Application Menu ===== */

typedef struct LeewayUIAppMenuItem {
    UINT32 command_id;
    LeewayUIControlKind kind;
    WCHAR *label;               /* Override for menu display */
    struct LeewayUIAppMenuItem *sibling;
} LeewayUIAppMenuItem;

typedef struct {
    LeewayUIAppMenuItem *items;
} LeewayUIAppMenu;

/* ===== Icon Resolver with Glyph-Darkness Rule ===== */

typedef enum {
    LWUI_ICON_GLYPH = 0,        /* Text formatting/alignment/list glyphs - use @mono on dark */
    LWUI_ICON_PICTORIAL,        /* Pictorial/illustrative - keep color on dark */
} LeewayUIIconType;

typedef struct {
    UINT32 icon_id;
    WCHAR *slot_name;           /* e.g., "paint.bold", "viewer.saveas" */
    LeewayUIIconType type;      /* Determines rendering on dark band */
    WCHAR *icon_base_path;      /* e.g., "shared/libernt/shell/appframework/icons" */
} LeewayUIIconSlot;

typedef struct {
    UINT32 icon_id;
    HRESULT (*load_svg)(UINT32 icon_id, UINT32 size, BOOL is_dark_band, WCHAR **out_svg_path);
    HRESULT (*release_svg)(WCHAR *svg_path);
} LeewayUIIconResolver;

/* ===== Resize/Collapse Policy ===== */

typedef enum {
    LWUI_RESIZE_FULL = 0,       /* All controls in large form */
    LWUI_RESIZE_MEDIUM,         /* Mixed large/small */
    LWUI_RESIZE_SMALL,          /* All controls in small form */
    LWUI_RESIZE_COLLAPSED,      /* Group into dropdown/popup */
} LeewayUIResizeState;

typedef struct {
    LeewayUIResizeState state;
    struct {
        UINT32 width_px;        /* Ribbon available width in logical units */
        BOOL is_horizontal;     /* TRUE for side-by-side, FALSE for vertical stack */
    } context;
} LeewayUIResizePolicy;

/* ===== Keyboard Navigation and KeyTips ===== */

typedef enum {
    LWUI_KEYTIP_ALPHA = 0,      /* Single letter (A-Z, 0-9) */
    LWUI_KEYTIP_DIGIT,          /* Numeric sequence */
    LWUI_KEYTIP_MULTI,          /* Multi-key sequence (e.g., "OH" for Office Home) */
} LeewayUIKeytipStyle;

typedef struct {
    BOOL keytip_mode_active;
    WCHAR *current_prefix;      /* For multi-key sequences */
} LeewayUIKeyboardState;

/* ===== Accessibility (MSAA) ===== */

typedef enum {
    LWUI_ROLE_RIBBON = 0,
    LWUI_ROLE_TAB,
    LWUI_ROLE_TABLIST,
    LWUI_ROLE_GROUP,
    LWUI_ROLE_BUTTON,
    LWUI_ROLE_MENUITEM,
    LWUI_ROLE_GALLERY,
} LeewayUIAccessibleRole;

typedef struct {
    LeewayUIAccessibleRole role;
    WCHAR *name;
    WCHAR *description;
    BOOL is_enabled;
    HWND hwnd_self;
} LeewayUIAccessibleObject;

/* ===== Theme Integration ===== */

typedef struct {
    COLORREF text_color;        /* Foreground text */
    COLORREF bg_color;          /* Band/group background */
    COLORREF border_color;      /* Control borders */
    COLORREF hover_color;       /* Hover state */
    COLORREF pressed_color;     /* Active/pressed state */
    COLORREF disabled_color;    /* Disabled text */
    BOOL dark_mode;             /* TRUE if dark theme - affects icon rendering */
} LeewayUITheme;

/* ===== DPI Awareness ===== */

typedef struct {
    UINT32 dpi;                 /* Logical DPI (96, 120, 144, 192, etc.) */
    FLOAT scale_factor;         /* dpi / 96.0 */
} LeewayUIDPIContext;

/* ===== Main Ribbon Context ===== */

typedef struct ILeewayRibbon ILeewayRibbon;

/* ===== Ribbon Interface ===== */

typedef struct ILeewayRibbon {
    HRESULT (*QueryInterface)(ILeewayRibbon *self, REFIID iid, void **out);
    ULONG (*AddRef)(ILeewayRibbon *self);
    ULONG (*Release)(ILeewayRibbon *self);

    /* Basic ribbon setup */
    HRESULT (*SetCommandTable)(ILeewayRibbon *self,
                                const LeewayUICommand *commands, UINT32 count,
                                LeewayUICommandExecuteHandler exec,
                                LeewayUICommandStateHandler state,
                                void *app_context);

    /* Layout and structure (F1 gaps G1-G3) */
    HRESULT (*SetLayout)(ILeewayRibbon *self, const LeewayUITab *tabs);
    HRESULT (*SetQAT)(ILeewayRibbon *self, const LeewayUIQAT *qat);
    HRESULT (*SetAppMenu)(ILeewayRibbon *self, const LeewayUIAppMenu *app_menu);

    /* Control-specific operations (F1 gaps G4, G5, G6) */
    HRESULT (*SetCollectionItems)(ILeewayRibbon *self, UINT32 gallery_id,
                                   const LeewayUICollectionItem *items);
    HRESULT (*SetSplitButtonConfig)(ILeewayRibbon *self, UINT32 command_id,
                                     const LeewayUISplitButtonConfig *config);
    HRESULT (*GetQATItems)(ILeewayRibbon *self, LeewayUIQATItem **items, UINT32 *count);
    HRESULT (*PersistQAT)(ILeewayRibbon *self);
    HRESULT (*LoadQATFromRegistry)(ILeewayRibbon *self, HKEY hkey_root, const WCHAR *app_name);

    /* App menu and galleries */
    HRESULT (*ShowAppMenu)(ILeewayRibbon *self, POINT pt);
    HRESULT (*ShowGalleryPopup)(ILeewayRibbon *self, UINT32 gallery_id, POINT pt);

    /* Icon resolver (F1 gap G10) */
    HRESULT (*SetIconResolver)(ILeewayRibbon *self, const LeewayUIIconResolver *resolver);
    HRESULT (*ResolveIconSVG)(ILeewayRibbon *self, UINT32 icon_id, UINT32 size,
                               BOOL is_dark_band, WCHAR **out_path);

    /* Theme and appearance (F1 gap G8) */
    HRESULT (*SetTheme)(ILeewayRibbon *self, const LeewayUITheme *theme);

    /* DPI handling (F1 gap G9) */
    HRESULT (*SetDPI)(ILeewayRibbon *self, UINT32 dpi);
    HRESULT (*HandleDPIChanged)(ILeewayRibbon *self, UINT32 new_dpi);

    /* Resize and collapse policy (F1 gap G11) */
    HRESULT (*SetResizePolicy)(ILeewayRibbon *self, const LeewayUIResizePolicy *policy);
    HRESULT (*GetCurrentResizeState)(ILeewayRibbon *self, LeewayUIResizeState *out);

    /* Keyboard and keytips (F1 gaps G18) */
    HRESULT (*EnableKeyTips)(ILeewayRibbon *self, BOOL enable);
    HRESULT (*HandleKeyDown)(ILeewayRibbon *self, UINT vkey, const LeewayUIKeyboardState *state);

    /* Tooltips (F1 gap G22) */
    HRESULT (*ShowTooltip)(ILeewayRibbon *self, UINT32 command_id, POINT pt);
    HRESULT (*HideTooltip)(ILeewayRibbon *self);

    /* Minimize/collapse band (F1 gap G21) */
    HRESULT (*MinimizeRibbon)(ILeewayRibbon *self, BOOL minimize);

    /* Help button integration (F1 gap G24) */
    HRESULT (*SetHelpButtonHandler)(ILeewayRibbon *self,
                                     HRESULT (*handler)(void *app_context));

    /* Rendering and invalidation (F1 gap G3) */
    HRESULT (*InvalidateRegion)(ILeewayRibbon *self, const RECT *rc);
    HRESULT (*Render)(ILeewayRibbon *self, HDC hdc, const RECT *rc);

    /* Accessibility (F1 gap G20) */
    HRESULT (*GetAccessibleObject)(ILeewayRibbon *self, UINT32 child_id,
                                    LeewayUIAccessibleObject *out);
    HRESULT (*NotifyAccessibleChange)(ILeewayRibbon *self, UINT32 child_id, UINT32 event);

    /* Multi-placement support (F1 gap G12) */
    HRESULT (*SetPlacementKeytip)(ILeewayRibbon *self, UINT32 command_id,
                                   const WCHAR *tab_name, const WCHAR *keytip);

    /* Window and lifecycle */
    HWND (*GetHWND)(ILeewayRibbon *self);
    HRESULT (*Destroy)(ILeewayRibbon *self);
} ILeewayRibbon;

/* ===== Factory Functions ===== */

HRESULT WINAPI LeewayRibbonCreate(HWND parent, const LeewayUICommand *table, UINT32 count,
                                   LeewayUICommandExecuteHandler exec,
                                   LeewayUICommandStateHandler state,
                                   void *app_context, ILeewayRibbon **out);

#ifdef __cplusplus
}
#endif
