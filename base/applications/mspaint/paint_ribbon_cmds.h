/*
 * PROJECT:    PAINT for LiberNT
 * LICENSE:    LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:    Paint ribbon: the ONE command table, layout and collections, shared by
 *             mspaint (paint_ribbon.cpp) and the standalone demo (ribbon_demo).
 *             ATL-free on purpose: only <windows.h>, ribbon.h and resource.h.
 * COPYRIGHT:  Copyright 2026 Leeway Open Software
 * PROVENANCE: sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 */

#pragma once

#include <windows.h>
#include <stdlib.h>
#include "../../../sdk/include/leewayui/ribbon.h"
#include "../../../sdk/include/leewayui/ribbon_icons.h"
#include "resource.h"

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* Paint command table. IDs from resource.h (201-292 menu, 600-615 tools).
 * New Paint commands: 300-399. Labels/tooltips from Paint's string resources. */

static const LeewayUICommand g_paintCommands[] = {
    /* File commands (201-218) */
    { IDM_FILENEW,                  L"New",                     L"New image",           L"N",   LWUI_AUR_COMMON_NEW, TRUE, FALSE, TRUE },
    { IDM_FILEOPEN,                 L"Open",                    L"Open image",          L"O",   LWUI_AUR_COMMON_OPEN, TRUE, FALSE, TRUE },
    { IDM_FILESAVE,                 L"Save",                    L"Save image",          L"S",   LWUI_AUR_COMMON_SAVE, TRUE, FALSE, TRUE },
    { IDM_FILESAVEAS,               L"Save as",                 L"Save image as",       L"A",   LWUI_AUR_COMMON_SAVEAS, TRUE, FALSE, TRUE },
    { IDM_FILEFROMSCANNERORCAMERA,  L"From scanner",            L"From scanner or camera", L"M",   0, FALSE, FALSE, TRUE },
    { IDM_FILEPRINT,                L"Print",                   L"Print image",         L"P",   LWUI_AUR_COMMON_PRINT, TRUE, FALSE, TRUE },
    { IDM_FILEPAGESETUP,            L"Page setup",              L"Page setup",          L"G",   LWUI_AUR_COMMON_PAGESETUP, TRUE, FALSE, TRUE },
    { IDM_FILEPAGEVIEW,             L"Print preview",           L"Print preview",       L"V",   0, TRUE, FALSE, TRUE },
    { IDM_FILESEND,                 L"Send in e-mail",          L"Send in e-mail",      L"D",   LWUI_AUR_COMMON_EMAIL, TRUE, FALSE, TRUE },
    { IDM_FILEASWALLPAPERPLANE,     L"Set as background",       L"Set as desktop background", L"B", 0, TRUE, FALSE, TRUE },
    { IDM_FILEASWALLPAPERCENTERED,  L"Centered",                NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { IDM_FILEASWALLPAPERSTRETCHED, L"Stretched",               NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { IDM_IMAGEATTRIBUTES,          L"Properties",              L"Image properties",    L"I",   LWUI_AUR_COMMON_PROPERTIES, TRUE, FALSE, TRUE },
    { IDM_HELPINFO,                 L"About Paint",             L"About Paint",         L"T",   LWUI_AUR_COMMON_ABOUT, TRUE, FALSE, TRUE },
    { IDM_FILEEXIT,                 L"Exit",                    L"Exit Paint",          L"X",   LWUI_AUR_COMMON_EXIT, TRUE, FALSE, TRUE },

    /* Edit commands (220-229) */
    { IDM_EDITUNDO,                 L"Undo",                    L"Undo action",         L"U",   LWUI_AUR_COMMON_UNDO, TRUE, FALSE, TRUE },
    { IDM_EDITREDO,                 L"Redo",                    L"Redo action",         L"Y",   LWUI_AUR_COMMON_REDO, TRUE, FALSE, TRUE },
    { IDM_EDITCUT,                  L"Cut",                     L"Cut to clipboard",    L"X",   LWUI_AUR_COMMON_CUT, TRUE, FALSE, TRUE },
    { IDM_EDITCOPY,                 L"Copy",                    L"Copy to clipboard",   L"CP",   LWUI_AUR_COMMON_COPY, TRUE, FALSE, TRUE },
    { IDM_EDITPASTE,                L"Paste",                   L"Paste from clipboard", L"V",   LWUI_AUR_COMMON_PASTE, TRUE, FALSE, TRUE },
    { IDM_EDITDELETESELECTION,      L"Delete",                  L"Delete selection",    NULL,   LWUI_AUR_COMMON_DELETE, TRUE, FALSE, TRUE },
    { IDM_EDITINVERTSELECTION,      L"Invert selection",        L"Invert selection",    NULL,   0, TRUE, FALSE, TRUE },
    { IDM_EDITSELECTALL,            L"Select all",              L"Select all",          NULL,   LWUI_AUR_COMMON_SELECT_ALL, TRUE, FALSE, TRUE },
    { IDM_EDITPASTEFROM,            L"Paste from...",           L"Paste from file",     NULL,   0, TRUE, FALSE, TRUE },

    /* Image commands (250-263) */
    { IDM_IMAGEROTATEMIRROR,        L"Rotate/Flip",             L"Rotate or flip",      NULL,   LWUI_AUR_COMMON_ROTATE_RIGHT, TRUE, FALSE, TRUE },
    { IDM_IMAGESTRETCHSKEW,         L"Resize",                  L"Resize or skew",      L"RE",  LWUI_AUR_PAINT_RESIZE, TRUE, FALSE, TRUE },
    { IDM_IMAGECROP,                L"Crop",                    L"Crop image",          L"RP",  LWUI_AUR_PAINT_CROP, TRUE, FALSE, TRUE },
    { IDM_IMAGEINVERTCOLORS,        L"Invert colors",           L"Invert colors",       NULL,   0, TRUE, FALSE, TRUE },
    { IDM_IMAGEDELETEIMAGE,         L"Delete image",            L"Delete image",        NULL,   0, TRUE, FALSE, TRUE },
    { IDM_IMAGEDRAWOPAQUE,          L"Transparent selection",   L"Toggle transparency", NULL,   LWUI_AUR_PAINT_BG_TRANSPARENT, TRUE, FALSE, TRUE },
    { IDM_COLORSEDITPALETTE,        L"Edit palette",            L"Edit palette",        L"EC",  LWUI_AUR_PAINT_EDIT_COLOURS, TRUE, FALSE, TRUE },
    { IDM_COLORSMODERNPALETTE,      L"Modern palette",          NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { IDM_COLORSOLDPALETTE,         L"Old palette",             NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { IDM_COLORSGRAYSCALE,          L"Grayscale",               NULL,                   NULL,   0, TRUE, FALSE, TRUE },
    { IDM_COLORSMONOCHROME,         L"Monochrome",              NULL,                   NULL,   0, TRUE, FALSE, TRUE },

    /* View commands (230-240) */
    { IDM_VIEWSTATUSBAR,            L"Status bar",              L"Show status bar",     L"S",   LWUI_AUR_COMMON_STATUSBAR, TRUE, FALSE, TRUE },
    { IDM_VIEWSHOWGRID,             L"Gridlines",               L"Show gridlines",      L"G",   0, TRUE, FALSE, TRUE },
    { IDM_VIEWSHOWMINIATURE,        L"Thumbnail",               L"Show thumbnail",      L"T",   LWUI_AUR_PAINT_THUMBNAIL, TRUE, FALSE, TRUE },
    { IDM_VIEWFULLSCREEN,           L"Full screen",             L"Toggle full screen",  L"F",   LWUI_AUR_COMMON_FULLSCREEN, TRUE, FALSE, TRUE },
    { IDM_VIEWZOOM100,              L"100 %",                   L"Zoom to 100 percent", L"1",   LWUI_AUR_COMMON_ZOOM, TRUE, FALSE, TRUE },
    { IDM_CTRL_PLUS,                L"Zoom in",                 L"Zoom in",             L"I",   LWUI_AUR_COMMON_ZOOM_IN, TRUE, FALSE, TRUE },
    { IDM_CTRL_MINUS,               L"Zoom out",                L"Zoom out",            L"O",   LWUI_AUR_COMMON_ZOOM_OUT, TRUE, FALSE, TRUE },

    /* Selection tools (600-615): radio set */
    { ID_FREESEL,                   L"Free form",               L"Free form selection", NULL,   LWUI_AUR_PAINT_SELECT_FREE, TRUE, FALSE, TRUE },
    { ID_RECTSEL,                   L"Select",                  L"Rectangle selection", L"SE",  LWUI_AUR_PAINT_SELECT_RECT, TRUE, FALSE, TRUE },
    { ID_RUBBER,                    L"Eraser",                  L"Eraser",              L"ER",   LWUI_AUR_PAINT_ERASER, TRUE, FALSE, TRUE },
    { ID_FILL,                      L"Fill with color",         L"Fill with color",     L"K",   LWUI_AUR_PAINT_FILL, TRUE, FALSE, TRUE },
    { ID_COLOR,                     L"Color picker",            L"Color picker",        L"I",   LWUI_AUR_PAINT_PICKER, TRUE, FALSE, TRUE },
    { ID_ZOOM,                      L"Magnifier",               L"Magnifier",           L"M",   LWUI_AUR_PAINT_MAGNIFIER, TRUE, FALSE, TRUE },
    { ID_PEN,                       L"Pencil",                  L"Pencil",              L"PE",   LWUI_AUR_PAINT_PENCIL, TRUE, FALSE, TRUE },
    { ID_BRUSH,                     L"Brush",                   L"Brush",               NULL,   LWUI_AUR_PAINT_BRUSH, TRUE, FALSE, TRUE },
    { ID_AIRBRUSH,                  L"Airbrush",                L"Airbrush",            NULL,   LWUI_AUR_PAINT_BRUSH, TRUE, FALSE, TRUE },
    { ID_TEXT,                      L"Text",                    L"Text tool",           L"T",   LWUI_AUR_PAINT_TEXT, TRUE, FALSE, TRUE },
    { ID_LINE,                      L"Line",                    L"Line",                NULL,   LWUI_AUR_PAINT_SHAPES, TRUE, FALSE, TRUE },
    { ID_BEZIER,                    L"Curve",                   L"Curve",               NULL,   LWUI_AUR_PAINT_SHAPES, TRUE, FALSE, TRUE },
    { ID_RECT,                      L"Rectangle",               L"Rectangle",           NULL,   LWUI_AUR_PAINT_SHAPES, TRUE, FALSE, TRUE },
    { ID_SHAPE,                     L"Polygon",                 L"Polygon",             NULL,   LWUI_AUR_PAINT_SHAPES, TRUE, FALSE, TRUE },
    { ID_ELLIPSE,                   L"Ellipse",                 L"Ellipse",             NULL,   LWUI_AUR_PAINT_SHAPES, TRUE, FALSE, TRUE },
    { ID_RRECT,                     L"Rounded rectangle",       L"Rounded rectangle",   NULL,   LWUI_AUR_PAINT_SHAPES, TRUE, FALSE, TRUE },

    /* Text formatting (280-283) */
    { IDM_BOLD,                     L"Bold",                    L"Bold",                L"B",   0, TRUE, FALSE, TRUE },
    { IDM_ITALIC,                   L"Italic",                  L"Italic",              L"I",   0, TRUE, FALSE, TRUE },
    { IDM_UNDERLINE,                L"Underline",               L"Underline",           L"U",   0, TRUE, FALSE, TRUE },
    { IDM_VERTICAL,                 L"Vertical",                L"Vertical text",       L"VT",  0, TRUE, FALSE, TRUE },

    /* Zoom gallery (380) */
    { 380,                          L"Zoom level",              L"Select zoom level",   L"Z",   LWUI_AUR_COMMON_ZOOM, TRUE, FALSE, TRUE },

    /* Paint new/collection commands (300-399) */
    { 301,                          L"PNG",                     L"Save as PNG",         NULL,   0, TRUE, FALSE, FALSE },
    { 302,                          L"JPEG",                    L"Save as JPEG",        NULL,   0, TRUE, FALSE, FALSE },
    { 303,                          L"BMP",                     L"Save as BMP",         NULL,   0, TRUE, FALSE, FALSE },
    { 304,                          L"GIF",                     L"Save as GIF",         NULL,   0, TRUE, FALSE, FALSE },
    { 305,                          L"Other formats",           L"Save as other format", NULL,   0, TRUE, FALSE, FALSE },

    { 310,                          L"Rotate",                  L"Rotate image",        L"RO",  LWUI_AUR_COMMON_ROTATE_RIGHT, TRUE, FALSE, TRUE },
    { 311,                          L"Rotate right 90\u00B0",        NULL,                   NULL,   LWUI_AUR_COMMON_ROTATE_RIGHT, TRUE, FALSE, FALSE },
    { 312,                          L"Rotate left 90\u00B0",         NULL,                   NULL,   LWUI_AUR_COMMON_ROTATE_LEFT, TRUE, FALSE, FALSE },
    { 313,                          L"Rotate 180\u00B0",             NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { 314,                          L"Flip vertical",           NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { 315,                          L"Flip horizontal",         NULL,                   NULL,   0, TRUE, FALSE, FALSE },

    { 320,                          L"Brushes",                 L"Select brush",        L"B",   LWUI_AUR_PAINT_BRUSH, TRUE, FALSE, TRUE },
    /* 321-339: brush styles (app-provided at runtime) */

    { 340,                          L"Shapes",                  L"Select shape",        L"SH",  LWUI_AUR_PAINT_SHAPES, TRUE, FALSE, TRUE },
    { 345,                          L"Outline",                 L"Shape outline",       L"O",   LWUI_AUR_PAINT_OUTLINE, TRUE, FALSE, TRUE },
    { 346,                          L"Fill",                    L"Shape fill",          L"F",   LWUI_AUR_PAINT_SHAPE_FILL, TRUE, FALSE, TRUE },
    /* 347-352: outline/fill options */

    { 360,                          L"Size",                    L"Line width",          L"SZ",  LWUI_AUR_PAINT_SIZE, TRUE, FALSE, TRUE },
    /* 361-365: line width options */

    { 370,                          L"Color 1",                 L"Foreground color",    L"C1",  0, TRUE, FALSE, TRUE },
    { 371,                          L"Color 2",                 L"Background color",    L"C2",  0, TRUE, FALSE, TRUE },
    { 372,                          L"Palette",                 L"Palette",             L"PA",  0, TRUE, FALSE, TRUE },


    { 390,                          L"Font family",             L"Select font",         L"FF",  0, TRUE, FALSE, TRUE },
    { 391,                          L"Font size",               L"Select font size",    L"FS",  0, TRUE, FALSE, TRUE },
    { 392,                          L"Opaque",                  L"Opaque text background", L"O", LWUI_AUR_PAINT_BG_OPAQUE, TRUE, FALSE, TRUE },
    { 393,                          L"Transparent",             L"Transparent text background", L"R", LWUI_AUR_PAINT_BG_TRANSPARENT, TRUE, FALSE, TRUE },
};

/* Keytips (spec 2): per tab level, groups and placements share one namespace and must be
 * prefix-free (no keytip equal to, or the start of, another). Copy is CP (C is a prefix of
 * C1/C2), Pencil PE (prefix of PA), Eraser ER (prefix of EC); groups carry no keytip.
 * LeewayRibbonSetLayout returns LWUI_E_KEYTIP_COLLISION otherwise. */

/* Placements for Home tab */
static const LeewayUIPlacement g_homeClipboardPlacement[] = {
    { IDM_EDITPASTE,       LWUI_CONTROL_SPLIT_LARGE, L"V", 0 },
    { IDM_EDITCUT,         LWUI_CONTROL_BUTTON_SMALL, NULL, 0 },
    { IDM_EDITCOPY,        LWUI_CONTROL_BUTTON_SMALL, NULL, 0 },
};

static const LeewayUIPlacement g_homeImagePlacement[] = {
    { ID_RECTSEL,          LWUI_CONTROL_SPLIT_LARGE, L"SE", 0 },
    { IDM_IMAGECROP,       LWUI_CONTROL_BUTTON_SMALL, L"RP", 0 },
    { IDM_IMAGESTRETCHSKEW, LWUI_CONTROL_BUTTON_SMALL, L"RE", 0 },
    { 310,                 LWUI_CONTROL_DROPDOWN,    L"RO", 0 },
};

static const LeewayUIPlacement g_homeToolsPlacement[] = {
    { ID_PEN,              LWUI_CONTROL_TOGGLE_SMALL, L"PE", 0 },
    { ID_FILL,             LWUI_CONTROL_TOGGLE_SMALL, L"K", 0 },
    { ID_TEXT,             LWUI_CONTROL_TOGGLE_SMALL, L"T", 0 },
    { ID_RUBBER,           LWUI_CONTROL_TOGGLE_SMALL, L"ER", 0 },
    { ID_COLOR,            LWUI_CONTROL_TOGGLE_SMALL, L"I", 0 },
    { ID_ZOOM,             LWUI_CONTROL_TOGGLE_SMALL, L"M", 0 },
};

static const LeewayUIPlacement g_homeBrushesPlacement[] = {
    { 320,                 LWUI_CONTROL_SPLIT_LARGE, L"B", 0 },
};

static const LeewayUIPlacement g_homeShapesPlacement[] = {
    { 340,                 LWUI_CONTROL_GALLERY_STRIP, L"SH", 0 },
    { 345,                 LWUI_CONTROL_DROPDOWN, L"O", 0 },
    { 346,                 LWUI_CONTROL_DROPDOWN, L"F", 0 },
};

static const LeewayUIPlacement g_homeSizePlacement[] = {
    { 360,                 LWUI_CONTROL_SPLIT_LARGE, L"SZ", 0 },
};

static const LeewayUIPlacement g_homeColoursPlacement[] = {
    { 370,                 LWUI_CONTROL_COLORWELL, L"C1", 0 },
    { 371,                 LWUI_CONTROL_COLORWELL, L"C2", 0 },
    { 372,                 LWUI_CONTROL_GALLERY_STRIP, L"PA", 234 },
    { IDM_COLORSEDITPALETTE, LWUI_CONTROL_BUTTON_LARGE, L"EC", 0 },
};

/* Groups for Home tab */
static const LeewayUIGroup g_homeGroups[] = {
    { L"Clipboard", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE), 0,
      _countof(g_homeClipboardPlacement), g_homeClipboardPlacement },

    { L"Image", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED), IDM_IMAGEATTRIBUTES,
      _countof(g_homeImagePlacement), g_homeImagePlacement },

    { L"Tools", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_homeToolsPlacement), g_homeToolsPlacement },

    { L"Brushes", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_homeBrushesPlacement), g_homeBrushesPlacement },

    { L"Shapes", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_homeShapesPlacement), g_homeShapesPlacement },

    { L"Size", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_homeSizePlacement), g_homeSizePlacement },

    { L"Colours", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_homeColoursPlacement), g_homeColoursPlacement },
};

/* Home tab */
static const LeewayUITab g_homeTab = {
    1,
    L"Home",
    L"H",
    NULL,
    _countof(g_homeGroups), g_homeGroups,
};

/* Placements for View tab */
static const LeewayUIPlacement g_viewZoomPlacement[] = {
    { IDM_CTRL_PLUS,       LWUI_CONTROL_BUTTON_LARGE, L"I", 0 },
    { IDM_CTRL_MINUS,      LWUI_CONTROL_BUTTON_LARGE, L"O", 0 },
    { IDM_VIEWZOOM100,     LWUI_CONTROL_BUTTON_SMALL, L"1", 0 },
    { 380,                 LWUI_CONTROL_GALLERY, L"Z", 0 },
};

static const LeewayUIPlacement g_viewShowPlacement[] = {
    { IDM_VIEWSHOWGRID,    LWUI_CONTROL_CHECKBOX, L"G", 0 },
    { IDM_VIEWSTATUSBAR,   LWUI_CONTROL_CHECKBOX, L"S", 0 },
};

static const LeewayUIPlacement g_viewDisplayPlacement[] = {
    { IDM_VIEWFULLSCREEN,  LWUI_CONTROL_TOGGLE_LARGE, L"F", 0 },
    { IDM_VIEWSHOWMINIATURE, LWUI_CONTROL_TOGGLE_LARGE, L"T", 0 },
};

/* Groups for View tab */
static const LeewayUIGroup g_viewGroups[] = {
    { L"Zoom", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE), 0,
      _countof(g_viewZoomPlacement), g_viewZoomPlacement },

    { L"Show or hide", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_SMALL, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_viewShowPlacement), g_viewShowPlacement },

    { L"Display", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE), 0,
      _countof(g_viewDisplayPlacement), g_viewDisplayPlacement },
};

/* View tab */
static const LeewayUITab g_viewTab = {
    2,
    L"View",
    L"V",
    NULL,
    _countof(g_viewGroups), g_viewGroups,
};

/* Placements for Text tab (contextual: IsTextTool) */
static const LeewayUIPlacement g_textFontPlacement[] = {
    { 390,                 LWUI_CONTROL_COMBO, L"FF", 130 },
    { 391,                 LWUI_CONTROL_COMBO, L"FS", 50 },
    { IDM_BOLD,            LWUI_CONTROL_TOGGLE_SMALL, L"B", 0 },
    { IDM_ITALIC,          LWUI_CONTROL_TOGGLE_SMALL, L"I", 0 },
    { IDM_UNDERLINE,       LWUI_CONTROL_TOGGLE_SMALL, L"U", 0 },
    { IDM_VERTICAL,        LWUI_CONTROL_TOGGLE_SMALL, L"VT", 0 },
};

static const LeewayUIPlacement g_textBackgroundPlacement[] = {
    { 392,                 LWUI_CONTROL_TOGGLE_SMALL, L"O", 0 },
    { 393,                 LWUI_CONTROL_TOGGLE_SMALL, L"R", 0 },
};

static const LeewayUIPlacement g_textColoursPlacement[] = {
    { 370,                 LWUI_CONTROL_COLORWELL, L"C1", 0 },
    { 371,                 LWUI_CONTROL_COLORWELL, L"C2", 0 },
    { 372,                 LWUI_CONTROL_GALLERY_STRIP, L"PA", 234 },
};

/* Groups for Text tab */
static const LeewayUIGroup g_textGroups[] = {
    { L"Font", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED), 0,
      _countof(g_textFontPlacement), g_textFontPlacement },

    { L"Background", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_textBackgroundPlacement), g_textBackgroundPlacement },

    { L"Colours", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_textColoursPlacement), g_textColoursPlacement },
};

/* Text tab (contextual) */
static const LeewayUITab g_textTab = {
    3,
    L"Text",
    L"T",
    L"IsTextTool",
    _countof(g_textGroups), g_textGroups,
};

/* All tabs */
static const LeewayUITab g_paintTabs[] = {
    g_homeTab,
    g_viewTab,
    g_textTab,
};

/* App menu ids (in order) */
static const UINT32 g_paintAppMenuIds[] = {
    IDM_FILENEW,
    IDM_FILEOPEN,
    IDM_FILESAVE,
    IDM_FILESAVEAS,
    IDM_FILEPRINT,
    IDM_FILESEND,
    IDM_FILEASWALLPAPERPLANE,
    IDM_IMAGEATTRIBUTES,
    IDM_HELPINFO,
    IDM_FILEEXIT,
};

/* QAT default ids */
static const UINT32 g_paintQATIds[] = {
    IDM_FILESAVE,
    IDM_EDITUNDO,
    IDM_EDITREDO,
};

/* Main layout */
static const LeewayUILayout g_paintLayout = {
    L"Paint",
    _countof(g_paintTabs), g_paintTabs,
    _countof(g_paintAppMenuIds), g_paintAppMenuIds,
    _countof(g_paintQATIds), g_paintQATIds,
    IDM_HELPINFO, /* help_command_id: the help button runs About */
};

/* ---- Collections (galleries, dropdowns, split menus) ---- */

/* Ids of the items. Tools are Paint's own (610-615 shapes); 321-328 brush styles,
 * 347-352 outline/fill, 361-368 sizes and 1001-1028 palette cells are new. */

static const LeewayUICollectionItem g_paintRotateItems[] = {
    { 311, L"Rotate right 90\u00B0", 0, LWUI_COLOR_NONE, 0 }, { 312, L"Rotate left 90\u00B0", 0, LWUI_COLOR_NONE, 0 },
    { 313, L"Rotate 180\u00B0", 0, LWUI_COLOR_NONE, 0 }, { 314, L"Flip vertical", 0, LWUI_COLOR_NONE, 0 },
    { 315, L"Flip horizontal", 0, LWUI_COLOR_NONE, 0 },
    { IDM_IMAGEINVERTCOLORS, L"Invert colors", 0, LWUI_COLOR_NONE, 0 }, { IDM_IMAGEDELETEIMAGE, L"Delete image", 0, LWUI_COLOR_NONE, 0 },
    { IDM_COLORSGRAYSCALE, L"Grayscale", 0, LWUI_COLOR_NONE, 0 }, { IDM_COLORSMONOCHROME, L"Monochrome", 0, LWUI_COLOR_NONE, 0 },
};

static const LeewayUICollectionItem g_paintPasteItems[] = {
    { IDM_EDITPASTE, L"Paste", 0, LWUI_COLOR_NONE, 0 }, { IDM_EDITPASTEFROM, L"Paste from...", 0, LWUI_COLOR_NONE, 0 },
};

static const LeewayUICollectionItem g_paintSelectItems[] = {
    { ID_RECTSEL, L"Rectangular selection", 0, LWUI_COLOR_NONE, 0 }, { ID_FREESEL, L"Free-form selection", 0, LWUI_COLOR_NONE, 0 },
    { IDM_EDITSELECTALL, L"Select all", 0, LWUI_COLOR_NONE, 0 },
    { IDM_EDITDELETESELECTION, L"Delete", 0, LWUI_COLOR_NONE, 0 }, { IDM_IMAGEDRAWOPAQUE, L"Transparent selection", 0, LWUI_COLOR_NONE, 0 },
};

static const LeewayUICollectionItem g_paintSaveAsItems[] = {
    { 301, L"PNG", 0, LWUI_COLOR_NONE, 0 }, { 302, L"JPEG", 0, LWUI_COLOR_NONE, 0 }, { 303, L"BMP", 0, LWUI_COLOR_NONE, 0 }, { 304, L"GIF", 0, LWUI_COLOR_NONE, 0 }, { 305, L"Other formats", 0, LWUI_COLOR_NONE, 0 },
};

static const LeewayUICollectionItem g_paintZoomItems[] = {
    { IDM_VIEWZOOM125, L"12.5 %", 0, LWUI_COLOR_NONE, 0 }, { IDM_VIEWZOOM25, L"25 %", 0, LWUI_COLOR_NONE, 0 }, { IDM_VIEWZOOM50, L"50 %", 0, LWUI_COLOR_NONE, 0 },
    { IDM_VIEWZOOM100, L"100 %", 0, LWUI_COLOR_NONE, 0 }, { IDM_VIEWZOOM200, L"200 %", 0, LWUI_COLOR_NONE, 0 }, { IDM_VIEWZOOM400, L"400 %", 0, LWUI_COLOR_NONE, 0 },
    { IDM_VIEWZOOM800, L"800 %", 0, LWUI_COLOR_NONE, 0 },
};

static const LeewayUICollectionItem g_paintBrushItems[] = {
    { ID_BRUSH, L"Brush", 0, LWUI_COLOR_NONE, 0 }, { ID_AIRBRUSH, L"Airbrush", 0, LWUI_COLOR_NONE, 0 },
    { 321, L"Square brush", 0, LWUI_COLOR_NONE, 0 }, { 322, L"Calligraphy 1", 0, LWUI_COLOR_NONE, 0 },
    { 323, L"Calligraphy 2", 0, LWUI_COLOR_NONE, 0 },
};

static const LeewayUICollectionItem g_paintShapeItems[] = {
    { ID_LINE, L"Line", 0, LWUI_COLOR_NONE, 0 }, { ID_BEZIER, L"Curve", 0, LWUI_COLOR_NONE, 0 }, { ID_RECT, L"Rectangle", 0, LWUI_COLOR_NONE, 0 },
    { ID_SHAPE, L"Polygon", 0, LWUI_COLOR_NONE, 0 }, { ID_ELLIPSE, L"Ellipse", 0, LWUI_COLOR_NONE, 0 }, { ID_RRECT, L"Rounded rectangle", 0, LWUI_COLOR_NONE, 0 },
};

static const LeewayUICollectionItem g_paintOutlineItems[] = {
    { 347, L"No outline", 0, LWUI_COLOR_NONE, 0 }, { 348, L"Solid color", 0, LWUI_COLOR_NONE, 0 },
};

static const LeewayUICollectionItem g_paintFillItems[] = {
    { 350, L"No fill", 0, LWUI_COLOR_NONE, 0 }, { 351, L"Solid color", 0, LWUI_COLOR_NONE, 0 }, { 352, L"Color 2", 0, LWUI_COLOR_NONE, 0 },
};

static const LeewayUICollectionItem g_paintSizeItems[] = {
    { 361, L"1 px", 0, LWUI_COLOR_NONE, 0 }, { 362, L"2 px", 0, LWUI_COLOR_NONE, 0 }, { 363, L"3 px", 0, LWUI_COLOR_NONE, 0 }, { 364, L"4 px", 0, LWUI_COLOR_NONE, 0 }, { 365, L"5 px", 0, LWUI_COLOR_NONE, 0 },
    { 366, L"8 px", 0, LWUI_COLOR_NONE, 0 }, { 367, L"12 px", 0, LWUI_COLOR_NONE, 0 }, { 368, L"16 px", 0, LWUI_COLOR_NONE, 0 },
};

#define PAINT_PALETTE_FIRST 1001
#define PAINT_PALETTE_COUNT 28

/* Cells are swatches (LWUI_COLLECTION_COLORS): left click executes id (Color 1), right click alt_id (Color 2). */
#define PAINT_PALETTE_ALT_FIRST 1101
#define PAL(n, name, r, g, b) { PAINT_PALETTE_FIRST + (n), name, 0, RGB(r, g, b), PAINT_PALETTE_ALT_FIRST + (n) }
static const LeewayUICollectionItem g_paintPaletteItems[PAINT_PALETTE_COUNT] = {
    PAL(0, L"Black", 0, 0, 0), PAL(1, L"Gray", 128, 128, 128), PAL(2, L"Dark red", 128, 0, 0), PAL(3, L"Olive", 128, 128, 0),
    PAL(4, L"Dark green", 0, 128, 0), PAL(5, L"Teal", 0, 128, 128), PAL(6, L"Navy", 0, 0, 128), PAL(7, L"Purple", 128, 0, 128),
    PAL(8, L"Khaki", 128, 128, 64), PAL(9, L"Dark teal", 0, 64, 64), PAL(10, L"Sky blue", 0, 128, 255), PAL(11, L"Dark blue", 0, 64, 128),
    PAL(12, L"Indigo", 64, 0, 255), PAL(13, L"Brown", 128, 64, 0),
    PAL(14, L"White", 255, 255, 255), PAL(15, L"Light gray", 192, 192, 192), PAL(16, L"Red", 255, 0, 0), PAL(17, L"Yellow", 255, 255, 0),
    PAL(18, L"Green", 0, 255, 0), PAL(19, L"Cyan", 0, 255, 255), PAL(20, L"Blue", 0, 0, 255), PAL(21, L"Magenta", 255, 0, 255),
    PAL(22, L"Light yellow", 255, 255, 128), PAL(23, L"Mint", 0, 255, 128), PAL(24, L"Light cyan", 128, 255, 255),
    PAL(25, L"Periwinkle", 128, 128, 255), PAL(26, L"Rose", 255, 0, 128), PAL(27, L"Orange", 255, 128, 64),
};
#undef PAL

/* Fills every collection. Call after LeewayRibbonSetLayout (collections are kept
 * across layouts, so the order does not matter to the ribbon). Returns the first
 * failure but tries all of them. */
static inline HRESULT PaintRibbonSetCollections(ILeewayRibbon *ribbon)
{
    HRESULT hr = S_OK, h;

#define PAINT_SETCOLL(cmd, arr, flags) \
    do { h = LeewayRibbonSetCollection(ribbon, (cmd), (arr), _countof(arr), (flags)); if (FAILED(h) && SUCCEEDED(hr)) hr = h; } while (0)

    PAINT_SETCOLL(IDM_EDITPASTE, g_paintPasteItems, 0);
    PAINT_SETCOLL(ID_RECTSEL, g_paintSelectItems, 0);
    PAINT_SETCOLL(310, g_paintRotateItems, 0);
    PAINT_SETCOLL(IDM_FILESAVEAS, g_paintSaveAsItems, 0);
    PAINT_SETCOLL(380, g_paintZoomItems, 0);
    PAINT_SETCOLL(320, g_paintBrushItems, 0);
    PAINT_SETCOLL(340, g_paintShapeItems, 0);
    PAINT_SETCOLL(345, g_paintOutlineItems, 0);
    PAINT_SETCOLL(346, g_paintFillItems, 0);
    PAINT_SETCOLL(360, g_paintSizeItems, 0);
    PAINT_SETCOLL(372, g_paintPaletteItems, LWUI_COLLECTION_COLORS);
#undef PAINT_SETCOLL
    return hr;
}
