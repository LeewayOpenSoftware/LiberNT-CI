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
#include "resource.h"

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* Paint command table. IDs from resource.h (201-292 menu, 600-615 tools).
 * New Paint commands: 300-399. Labels/tooltips from Paint's string resources. */

static const LeewayUICommand g_paintCommands[] = {
    /* File commands (201-218) */
    { IDM_FILENEW,                  L"New",                     L"New image",           L"N",   0, TRUE, FALSE, TRUE },
    { IDM_FILEOPEN,                 L"Open",                    L"Open image",          L"O",   0, TRUE, FALSE, TRUE },
    { IDM_FILESAVE,                 L"Save",                    L"Save image",          L"S",   0, TRUE, FALSE, TRUE },
    { IDM_FILESAVEAS,               L"Save as",                 L"Save image as",       L"A",   0, TRUE, FALSE, TRUE },
    { IDM_FILEFROMSCANNERORCAMERA,  L"From scanner",            L"From scanner or camera", L"M",   0, FALSE, FALSE, TRUE },
    { IDM_FILEPRINT,                L"Print",                   L"Print image",         L"P",   0, TRUE, FALSE, TRUE },
    { IDM_FILEPAGESETUP,            L"Page setup",              L"Page setup",          L"G",   0, TRUE, FALSE, TRUE },
    { IDM_FILEPAGEVIEW,             L"Print preview",           L"Print preview",       L"V",   0, TRUE, FALSE, TRUE },
    { IDM_FILESEND,                 L"Send in e-mail",          L"Send in e-mail",      L"D",   0, TRUE, FALSE, TRUE },
    { IDM_FILEASWALLPAPERPLANE,     L"Set as background",       L"Set as desktop background", L"B", 0, TRUE, FALSE, TRUE },
    { IDM_FILEASWALLPAPERCENTERED,  L"Centered",                NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { IDM_FILEASWALLPAPERSTRETCHED, L"Stretched",               NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { IDM_IMAGEATTRIBUTES,          L"Properties",              L"Image properties",    L"I",   0, TRUE, FALSE, TRUE },
    { IDM_HELPINFO,                 L"About Paint",             L"About Paint",         L"T",   0, TRUE, FALSE, TRUE },
    { IDM_FILEEXIT,                 L"Exit",                    L"Exit Paint",          L"X",   0, TRUE, FALSE, TRUE },

    /* Edit commands (220-229) */
    { IDM_EDITUNDO,                 L"Undo",                    L"Undo action",         L"U",   0, TRUE, FALSE, TRUE },
    { IDM_EDITREDO,                 L"Redo",                    L"Redo action",         L"Y",   0, TRUE, FALSE, TRUE },
    { IDM_EDITCUT,                  L"Cut",                     L"Cut to clipboard",    L"X",   0, TRUE, FALSE, TRUE },
    { IDM_EDITCOPY,                 L"Copy",                    L"Copy to clipboard",   L"C",   0, TRUE, FALSE, TRUE },
    { IDM_EDITPASTE,                L"Paste",                   L"Paste from clipboard", L"V",   0, TRUE, FALSE, TRUE },
    { IDM_EDITDELETESELECTION,      L"Delete",                  L"Delete selection",    NULL,   0, TRUE, FALSE, TRUE },
    { IDM_EDITINVERTSELECTION,      L"Invert selection",        L"Invert selection",    NULL,   0, TRUE, FALSE, TRUE },
    { IDM_EDITSELECTALL,            L"Select all",              L"Select all",          NULL,   0, TRUE, FALSE, TRUE },
    { IDM_EDITPASTEFROM,            L"Paste from...",           L"Paste from file",     NULL,   0, TRUE, FALSE, TRUE },

    /* Image commands (250-263) */
    { IDM_IMAGEROTATEMIRROR,        L"Rotate/Flip",             L"Rotate or flip",      NULL,   0, TRUE, FALSE, TRUE },
    { IDM_IMAGESTRETCHSKEW,         L"Resize",                  L"Resize or skew",      L"RE",  0, TRUE, FALSE, TRUE },
    { IDM_IMAGECROP,                L"Crop",                    L"Crop image",          L"RP",  0, TRUE, FALSE, TRUE },
    { IDM_IMAGEINVERTCOLORS,        L"Invert colors",           L"Invert colors",       NULL,   0, TRUE, FALSE, TRUE },
    { IDM_IMAGEDELETEIMAGE,         L"Delete image",            L"Delete image",        NULL,   0, TRUE, FALSE, TRUE },
    { IDM_IMAGEDRAWOPAQUE,          L"Transparent selection",   L"Toggle transparency", NULL,   0, TRUE, FALSE, TRUE },
    { IDM_COLORSEDITPALETTE,        L"Edit palette",            L"Edit palette",        L"EC",  0, TRUE, FALSE, TRUE },
    { IDM_COLORSMODERNPALETTE,      L"Modern palette",          NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { IDM_COLORSOLDPALETTE,         L"Old palette",             NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { IDM_COLORSGRAYSCALE,          L"Grayscale",               NULL,                   NULL,   0, TRUE, FALSE, TRUE },
    { IDM_COLORSMONOCHROME,         L"Monochrome",              NULL,                   NULL,   0, TRUE, FALSE, TRUE },

    /* View commands (230-240) */
    { IDM_VIEWSTATUSBAR,            L"Status bar",              L"Show status bar",     L"S",   0, TRUE, FALSE, TRUE },
    { IDM_VIEWSHOWGRID,             L"Gridlines",               L"Show gridlines",      L"G",   0, TRUE, FALSE, TRUE },
    { IDM_VIEWSHOWMINIATURE,        L"Thumbnail",               L"Show thumbnail",      L"T",   0, TRUE, FALSE, TRUE },
    { IDM_VIEWFULLSCREEN,           L"Full screen",             L"Toggle full screen",  L"F",   0, TRUE, FALSE, TRUE },
    { IDM_VIEWZOOM100,              L"100 %",                   L"Zoom to 100 percent", L"1",   0, TRUE, FALSE, TRUE },
    { IDM_CTRL_PLUS,                L"Zoom in",                 L"Zoom in",             L"I",   0, TRUE, FALSE, TRUE },
    { IDM_CTRL_MINUS,               L"Zoom out",                L"Zoom out",            L"O",   0, TRUE, FALSE, TRUE },

    /* Selection tools (600-615): radio set */
    { ID_FREESEL,                   L"Free form",               L"Free form selection", NULL,   0, TRUE, FALSE, TRUE },
    { ID_RECTSEL,                   L"Rectangle",               L"Rectangle selection", L"SE",  0, TRUE, FALSE, TRUE },
    { ID_RUBBER,                    L"Eraser",                  L"Eraser",              L"E",   0, TRUE, FALSE, TRUE },
    { ID_FILL,                      L"Fill with color",         L"Fill with color",     L"K",   0, TRUE, FALSE, TRUE },
    { ID_COLOR,                     L"Color picker",            L"Color picker",        L"I",   0, TRUE, FALSE, TRUE },
    { ID_ZOOM,                      L"Magnifier",               L"Magnifier",           L"M",   0, TRUE, FALSE, TRUE },
    { ID_PEN,                       L"Pencil",                  L"Pencil",              L"P",   0, TRUE, FALSE, TRUE },
    { ID_BRUSH,                     L"Brush",                   L"Brush",               NULL,   0, TRUE, FALSE, TRUE },
    { ID_AIRBRUSH,                  L"Airbrush",                L"Airbrush",            NULL,   0, TRUE, FALSE, TRUE },
    { ID_TEXT,                      L"Text",                    L"Text tool",           L"T",   0, TRUE, FALSE, TRUE },
    { ID_LINE,                      L"Line",                    L"Line",                NULL,   0, TRUE, FALSE, TRUE },
    { ID_BEZIER,                    L"Curve",                   L"Curve",               NULL,   0, TRUE, FALSE, TRUE },
    { ID_RECT,                      L"Rectangle",               L"Rectangle",           NULL,   0, TRUE, FALSE, TRUE },
    { ID_SHAPE,                     L"Polygon",                 L"Polygon",             NULL,   0, TRUE, FALSE, TRUE },
    { ID_ELLIPSE,                   L"Ellipse",                 L"Ellipse",             NULL,   0, TRUE, FALSE, TRUE },
    { ID_RRECT,                     L"Rounded rectangle",       L"Rounded rectangle",   NULL,   0, TRUE, FALSE, TRUE },

    /* Text formatting (280-283) */
    { IDM_BOLD,                     L"Bold",                    L"Bold",                L"B",   0, TRUE, FALSE, TRUE },
    { IDM_ITALIC,                   L"Italic",                  L"Italic",              L"I",   0, TRUE, FALSE, TRUE },
    { IDM_UNDERLINE,                L"Underline",               L"Underline",           L"U",   0, TRUE, FALSE, TRUE },
    { IDM_VERTICAL,                 L"Vertical",                L"Vertical text",       L"VT",  0, TRUE, FALSE, TRUE },

    /* Zoom gallery (380) */
    { 380,                          L"Zoom level",              L"Select zoom level",   L"Z",   0, TRUE, FALSE, TRUE },

    /* Paint new/collection commands (300-399) */
    { 301,                          L"PNG",                     L"Save as PNG",         NULL,   0, TRUE, FALSE, FALSE },
    { 302,                          L"JPEG",                    L"Save as JPEG",        NULL,   0, TRUE, FALSE, FALSE },
    { 303,                          L"BMP",                     L"Save as BMP",         NULL,   0, TRUE, FALSE, FALSE },
    { 304,                          L"GIF",                     L"Save as GIF",         NULL,   0, TRUE, FALSE, FALSE },
    { 305,                          L"Other formats",           L"Save as other format", NULL,   0, TRUE, FALSE, FALSE },

    { 310,                          L"Rotate",                  L"Rotate image",        L"RO",  0, TRUE, FALSE, TRUE },
    { 311,                          L"Rotate right 90\u00B0",        NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { 312,                          L"Rotate left 90\u00B0",         NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { 313,                          L"Rotate 180\u00B0",             NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { 314,                          L"Flip vertical",           NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { 315,                          L"Flip horizontal",         NULL,                   NULL,   0, TRUE, FALSE, FALSE },

    { 320,                          L"Brushes",                 L"Select brush",        L"B",   0, TRUE, FALSE, TRUE },
    /* 321-339: brush styles (app-provided at runtime) */

    { 340,                          L"Shapes",                  L"Select shape",        L"SH",  0, TRUE, FALSE, TRUE },
    { 345,                          L"Outline",                 L"Shape outline",       L"O",   0, TRUE, FALSE, TRUE },
    { 346,                          L"Fill",                    L"Shape fill",          L"F",   0, TRUE, FALSE, TRUE },
    /* 347-352: outline/fill options */

    { 360,                          L"Size",                    L"Line width",          L"SZ",  0, TRUE, FALSE, TRUE },
    /* 361-365: line width options */

    { 370,                          L"Color 1",                 L"Foreground color",    L"C1",  0, TRUE, FALSE, TRUE },
    { 371,                          L"Color 2",                 L"Background color",    L"C2",  0, TRUE, FALSE, TRUE },
    { 372,                          L"Palette",                 L"Palette",             L"PA",  0, TRUE, FALSE, TRUE },

    { 381,                          L"Rulers",                  L"Show rulers",         L"R",   0, TRUE, FALSE, TRUE },

    { 390,                          L"Font family",             L"Select font",         L"FF",  0, TRUE, FALSE, TRUE },
    { 391,                          L"Font size",               L"Select font size",    L"FS",  0, TRUE, FALSE, TRUE },
    { 392,                          L"Opaque",                  L"Opaque text background", L"O", 0, TRUE, FALSE, TRUE },
    { 393,                          L"Transparent",             L"Transparent text background", L"R", 0, TRUE, FALSE, TRUE },
};

/* Keytips are unique per tab level: the groups' and the placements' share one namespace
 * (LeewayRibbonSetLayout returns LWUI_E_KEYTIP_COLLISION otherwise). */

/* Placements for Home tab */
static const LeewayUIPlacement g_homeClipboardPlacement[] = {
    { IDM_EDITPASTE,       LWUI_CONTROL_SPLIT_LARGE, L"V" },
    { IDM_EDITCUT,         LWUI_CONTROL_BUTTON_SMALL, NULL },
    { IDM_EDITCOPY,        LWUI_CONTROL_BUTTON_SMALL, NULL },
};

static const LeewayUIPlacement g_homeImagePlacement[] = {
    { ID_RECTSEL,          LWUI_CONTROL_SPLIT_LARGE, L"SE" },
    { IDM_IMAGECROP,       LWUI_CONTROL_BUTTON_SMALL, L"RP" },
    { IDM_IMAGESTRETCHSKEW, LWUI_CONTROL_BUTTON_SMALL, L"RE" },
    { 310,                 LWUI_CONTROL_DROPDOWN,    L"RO" },
};

static const LeewayUIPlacement g_homeToolsPlacement[] = {
    { ID_PEN,              LWUI_CONTROL_BUTTON_SMALL_ICON, L"P" },
    { ID_FILL,             LWUI_CONTROL_BUTTON_SMALL_ICON, L"K" },
    { ID_TEXT,             LWUI_CONTROL_BUTTON_SMALL_ICON, L"T" },
    { ID_RUBBER,           LWUI_CONTROL_BUTTON_SMALL_ICON, L"E" },
    { ID_COLOR,            LWUI_CONTROL_BUTTON_SMALL_ICON, L"I" },
    { ID_ZOOM,             LWUI_CONTROL_BUTTON_SMALL_ICON, L"M" },
};

static const LeewayUIPlacement g_homeBrushesPlacement[] = {
    { 320,                 LWUI_CONTROL_GALLERY, L"B" },
};

static const LeewayUIPlacement g_homeShapesPlacement[] = {
    { 340,                 LWUI_CONTROL_GALLERY, L"SH" },
    { 345,                 LWUI_CONTROL_DROPDOWN, L"O" },
    { 346,                 LWUI_CONTROL_DROPDOWN, L"F" },
};

static const LeewayUIPlacement g_homeSizePlacement[] = {
    { 360,                 LWUI_CONTROL_DROPDOWN, L"SZ" },
};

static const LeewayUIPlacement g_homeColoursPlacement[] = {
    { 370,                 LWUI_CONTROL_COLORWELL, L"C1" },
    { 371,                 LWUI_CONTROL_COLORWELL, L"C2" },
    { 372,                 LWUI_CONTROL_GALLERY, L"PA" },
    { IDM_COLORSEDITPALETTE, LWUI_CONTROL_BUTTON_LARGE, L"EC" },
};

/* Groups for Home tab */
static const LeewayUIGroup g_homeGroups[] = {
    { L"Clipboard", L"CB", LWUI_VARIANT_ORDER(LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE), 0,
      _countof(g_homeClipboardPlacement), g_homeClipboardPlacement },

    { L"Image", L"IM", LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED), 0,
      _countof(g_homeImagePlacement), g_homeImagePlacement },

    { L"Tools", L"TL", LWUI_VARIANT_ORDER(LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_homeToolsPlacement), g_homeToolsPlacement },

    { L"Brushes", L"BR", LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_homeBrushesPlacement), g_homeBrushesPlacement },

    { L"Shapes", L"SG", LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_homeShapesPlacement), g_homeShapesPlacement },

    { L"Size", L"SI", LWUI_VARIANT_ORDER(LWUI_VARIANT_SMALL, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_homeSizePlacement), g_homeSizePlacement },

    { L"Colours", L"CL", LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
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
    { IDM_CTRL_PLUS,       LWUI_CONTROL_BUTTON_LARGE, L"I" },
    { IDM_CTRL_MINUS,      LWUI_CONTROL_BUTTON_LARGE, L"O" },
    { IDM_VIEWZOOM100,     LWUI_CONTROL_BUTTON_SMALL, L"1" },
    { 380,                 LWUI_CONTROL_GALLERY, L"Z" },
};

static const LeewayUIPlacement g_viewShowPlacement[] = {
    { 381,                 LWUI_CONTROL_BUTTON_SMALL_ICON, L"R" },
    { IDM_VIEWSHOWGRID,    LWUI_CONTROL_BUTTON_SMALL_ICON, L"G" },
    { IDM_VIEWSTATUSBAR,   LWUI_CONTROL_BUTTON_SMALL_ICON, L"S" },
};

static const LeewayUIPlacement g_viewDisplayPlacement[] = {
    { IDM_VIEWFULLSCREEN,  LWUI_CONTROL_TOGGLE_LARGE, L"F" },
    { IDM_VIEWSHOWMINIATURE, LWUI_CONTROL_TOGGLE_LARGE, L"T" },
};

/* Groups for View tab */
static const LeewayUIGroup g_viewGroups[] = {
    { L"Zoom", L"ZM", LWUI_VARIANT_ORDER(LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE), 0,
      _countof(g_viewZoomPlacement), g_viewZoomPlacement },

    { L"Show or hide", L"SH", LWUI_VARIANT_ORDER(LWUI_VARIANT_SMALL, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_viewShowPlacement), g_viewShowPlacement },

    { L"Display", L"DI", LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE), 0,
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
    { 390,                 LWUI_CONTROL_COMBO, L"FF" },
    { 391,                 LWUI_CONTROL_COMBO, L"FS" },
    { IDM_BOLD,            LWUI_CONTROL_TOGGLE_SMALL, L"B" },
    { IDM_ITALIC,          LWUI_CONTROL_TOGGLE_SMALL, L"I" },
    { IDM_UNDERLINE,       LWUI_CONTROL_TOGGLE_SMALL, L"U" },
    { IDM_VERTICAL,        LWUI_CONTROL_TOGGLE_SMALL, L"VT" },
};

static const LeewayUIPlacement g_textBackgroundPlacement[] = {
    { 392,                 LWUI_CONTROL_TOGGLE_SMALL, L"O" },
    { 393,                 LWUI_CONTROL_TOGGLE_SMALL, L"R" },
};

static const LeewayUIPlacement g_textColoursPlacement[] = {
    { 370,                 LWUI_CONTROL_COLORWELL, L"C1" },
    { 371,                 LWUI_CONTROL_COLORWELL, L"C2" },
    { 372,                 LWUI_CONTROL_GALLERY, L"PA" },
};

/* Groups for Text tab */
static const LeewayUIGroup g_textGroups[] = {
    { L"Font", L"FT", LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED), 0,
      _countof(g_textFontPlacement), g_textFontPlacement },

    { L"Background", L"BG", LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_textBackgroundPlacement), g_textBackgroundPlacement },

    { L"Colours", L"CL", LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
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
    IDM_FILEFROMSCANNERORCAMERA,
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
    0, /* help_command_id: execute(LWUI_HELP) */
};

/* ---- Collections (galleries, dropdowns, split menus) ---- */

/* Ids of the items. Tools are Paint's own (610-615 shapes); 321-328 brush styles,
 * 347-352 outline/fill, 361-368 sizes and 1001-1028 palette cells are new. */

static const LeewayUICollectionItem g_paintRotateItems[] = {
    { 311, L"Rotate right 90\u00B0", 0 }, { 312, L"Rotate left 90\u00B0", 0 }, { 313, L"Rotate 180\u00B0", 0 },
    { 314, L"Flip vertical", 0 }, { 315, L"Flip horizontal", 0 },
};

static const LeewayUICollectionItem g_paintSaveAsItems[] = {
    { 301, L"PNG", 0 }, { 302, L"JPEG", 0 }, { 303, L"BMP", 0 }, { 304, L"GIF", 0 }, { 305, L"Other formats", 0 },
};

static const LeewayUICollectionItem g_paintZoomItems[] = {
    { IDM_VIEWZOOM125, L"12.5 %", 0 }, { IDM_VIEWZOOM25, L"25 %", 0 }, { IDM_VIEWZOOM50, L"50 %", 0 },
    { IDM_VIEWZOOM100, L"100 %", 0 }, { IDM_VIEWZOOM200, L"200 %", 0 }, { IDM_VIEWZOOM400, L"400 %", 0 },
    { IDM_VIEWZOOM800, L"800 %", 0 },
};

static const LeewayUICollectionItem g_paintBrushItems[] = {
    { 321, L"Brush", 0 }, { 322, L"Airbrush", 0 }, { 323, L"Calligraphy brush", 0 }, { 324, L"Oil brush", 0 },
    { 325, L"Crayon", 0 }, { 326, L"Marker", 0 }, { 327, L"Natural pencil", 0 }, { 328, L"Watercolor brush", 0 },
};

static const LeewayUICollectionItem g_paintShapeItems[] = {
    { ID_LINE, L"Line", 0 }, { ID_BEZIER, L"Curve", 0 }, { ID_RECT, L"Rectangle", 0 },
    { ID_SHAPE, L"Polygon", 0 }, { ID_ELLIPSE, L"Ellipse", 0 }, { ID_RRECT, L"Rounded rectangle", 0 },
};

static const LeewayUICollectionItem g_paintOutlineItems[] = {
    { 347, L"No outline", 0 }, { 348, L"Solid color", 0 }, { 349, L"Dashed", 0 },
};

static const LeewayUICollectionItem g_paintFillItems[] = {
    { 350, L"No fill", 0 }, { 351, L"Solid color", 0 }, { 352, L"Color 2", 0 },
};

static const LeewayUICollectionItem g_paintSizeItems[] = {
    { 361, L"1 px", 0 }, { 362, L"2 px", 0 }, { 363, L"3 px", 0 }, { 364, L"4 px", 0 }, { 365, L"5 px", 0 },
    { 366, L"8 px", 0 }, { 367, L"12 px", 0 }, { 368, L"16 px", 0 },
};

#define PAINT_PALETTE_FIRST 1001
#define PAINT_PALETTE_COUNT 28

/* The 28 colors of Paint's classic palette, in its order (two rows of 14). */
static const COLORREF g_paintPaletteColors[PAINT_PALETTE_COUNT] = {
    RGB(0, 0, 0), RGB(128, 128, 128), RGB(128, 0, 0), RGB(128, 128, 0), RGB(0, 128, 0), RGB(0, 128, 128), RGB(0, 0, 128),
    RGB(128, 0, 128), RGB(128, 128, 64), RGB(0, 64, 64), RGB(0, 128, 255), RGB(0, 64, 128), RGB(64, 0, 255), RGB(128, 64, 0),
    RGB(255, 255, 255), RGB(192, 192, 192), RGB(255, 0, 0), RGB(255, 255, 0), RGB(0, 255, 0), RGB(0, 255, 255), RGB(0, 0, 255),
    RGB(255, 0, 255), RGB(255, 255, 128), RGB(0, 255, 128), RGB(128, 255, 255), RGB(128, 128, 255), RGB(255, 0, 128), RGB(255, 128, 64),
};

static const LeewayUICollectionItem g_paintPaletteItems[PAINT_PALETTE_COUNT] = {
    { 1001, L"Black", 0 }, { 1002, L"Gray", 0 }, { 1003, L"Dark red", 0 }, { 1004, L"Olive", 0 }, { 1005, L"Dark green", 0 },
    { 1006, L"Teal", 0 }, { 1007, L"Navy", 0 }, { 1008, L"Purple", 0 }, { 1009, L"Khaki", 0 }, { 1010, L"Dark teal", 0 },
    { 1011, L"Sky blue", 0 }, { 1012, L"Dark blue", 0 }, { 1013, L"Indigo", 0 }, { 1014, L"Brown", 0 },
    { 1015, L"White", 0 }, { 1016, L"Light gray", 0 }, { 1017, L"Red", 0 }, { 1018, L"Yellow", 0 }, { 1019, L"Green", 0 },
    { 1020, L"Cyan", 0 }, { 1021, L"Blue", 0 }, { 1022, L"Magenta", 0 }, { 1023, L"Light yellow", 0 }, { 1024, L"Mint", 0 },
    { 1025, L"Light cyan", 0 }, { 1026, L"Periwinkle", 0 }, { 1027, L"Rose", 0 }, { 1028, L"Orange", 0 },
};

/* Fills every collection. Call after LeewayRibbonSetLayout (collections are kept
 * across layouts, so the order does not matter to the ribbon). Returns the first
 * failure but tries all of them. */
static inline HRESULT PaintRibbonSetCollections(ILeewayRibbon *ribbon)
{
    HRESULT hr = S_OK, h;

#define PAINT_SETCOLL(cmd, arr) \
    do { h = LeewayRibbonSetCollection(ribbon, (cmd), (arr), _countof(arr), 0); if (FAILED(h) && SUCCEEDED(hr)) hr = h; } while (0)

    PAINT_SETCOLL(310, g_paintRotateItems);
    PAINT_SETCOLL(IDM_FILESAVEAS, g_paintSaveAsItems);
    PAINT_SETCOLL(380, g_paintZoomItems);
    PAINT_SETCOLL(320, g_paintBrushItems);
    PAINT_SETCOLL(340, g_paintShapeItems);
    PAINT_SETCOLL(345, g_paintOutlineItems);
    PAINT_SETCOLL(346, g_paintFillItems);
    PAINT_SETCOLL(360, g_paintSizeItems);
    PAINT_SETCOLL(372, g_paintPaletteItems);
#undef PAINT_SETCOLL
    return hr;
}
