/*
 * PROJECT:    PAINT for LiberNT
 * LICENSE:    LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:    Paint ribbon layout and integration
 * COPYRIGHT:  Copyright 2026 Leeway Open Software
 * PROVENANCE: sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 */

#pragma once

#include <windows.h>
#include "../../../sdk/include/leewayui/ribbon.h"
#include "resource.h"

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
    { 311,                          L"Rotate right 90°",        NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { 312,                          L"Rotate left 90°",         NULL,                   NULL,   0, TRUE, FALSE, FALSE },
    { 313,                          L"Rotate 180°",             NULL,                   NULL,   0, TRUE, FALSE, FALSE },
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

    { L"Shapes", L"SH", LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
      _countof(g_homeShapesPlacement), g_homeShapesPlacement },

    { L"Size", L"SZ", LWUI_VARIANT_ORDER(LWUI_VARIANT_SMALL, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE, LWUI_VARIANT_NONE), 0,
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
