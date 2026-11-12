/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test window: the Photo Viewer ribbon (RIBBON-APPS-SPEC.md §1) or, with "paint" on the
 *              command line, the Paint ribbon (§2) on the F1 framework
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 *
 * Viewer mode exercises: two tabs plus a contextual tab, every basic control
 * kind, collections, QAT with persistence, application menu, keytips,
 * tooltips with accelerator text, group variants, minimise, theme and DPI.
 * Paint mode adds the Paint gaps: contextual "Text" tab (G13), hosted font
 * combos (G14), colour wells and the palette with right-click (G15),
 * in-ribbon gallery strips for shapes and colours (G16), a dialog launcher
 * (G17) and check-box rows (G19).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <leewayui/ribbon.h>
#include <leewayui/theme.h>
#include <leewayui/dpi.h>

/* ---- Viewer command ids (shimgvw resource.h values; new ids per spec §1) ---- */
enum {
    IDC_PREV_PIC = 500, IDC_NEXT_PIC, IDC_BEST_FIT, IDC_REAL_SIZE, IDC_SLIDE_SHOW, IDC_ZOOM_IN, IDC_ZOOM_OUT,
    IDC_ROT_CLOCKW, IDC_ROT_COUNCW, IDC_ROT_CWSAVE, IDC_ROT_CCWSAVE, IDC_DELETE, IDC_PRINT, IDC_SAVEAS,
    IDC_MODIFY, IDC_HELP_TOC,
    IDC_TOGGLEFULLSCREEN = 600, IDC_INCTIMER, IDC_DECTIMER,
    ID_OPEN = 700, ID_WALLPAPER, ID_EXIT, ID_ZOOM_GALLERY = 710,
    ID_ZOOM_10 = 711, ID_ZOOM_25, ID_ZOOM_50, ID_ZOOM_75, ID_ZOOM_100, ID_ZOOM_150, ID_ZOOM_200, ID_ZOOM_400, ID_ZOOM_800, ID_ZOOM_1600,
    ID_ANIM_PLAY = 750, ID_ANIM_PREV, ID_ANIM_NEXT,
    ID_DEMO_ANIMATED = 790,     /* demo only: toggles the "Animation" context */
};
enum { TAB_HOME = 1, TAB_VIEW = 2, TAB_ANIMATION = 3, TAB_TEXT = 4 };

/* ---- Paint command ids (mspaint resource.h values; new ids 300-399 per spec §2) ---- */
enum {
    IDM_FILENEW = 201, IDM_FILEOPEN = 202, IDM_FILESAVE = 203, IDM_FILESAVEAS = 204, IDM_FILEFROMSCANNERORCAMERA = 205,
    IDM_FILEPAGEVIEW = 206, IDM_FILEPAGESETUP = 207, IDM_FILEPRINT = 208, IDM_FILESEND = 209, IDM_FILEASWALLPAPERPLANE = 210,
    IDM_FILEEXIT = 218, IDM_EDITUNDO = 220, IDM_EDITREDO = 221, IDM_EDITCUT = 222, IDM_EDITCOPY = 223, IDM_EDITPASTE = 224,
    IDM_EDITDELETESELECTION = 225, IDM_EDITINVERTSELECTION = 226, IDM_EDITSELECTALL = 227, IDM_EDITPASTEFROM = 229,
    IDM_VIEWSTATUSBAR = 232, IDM_VIEWZOOM125 = 234, IDM_VIEWZOOM25, IDM_VIEWZOOM50, IDM_VIEWZOOM100, IDM_VIEWZOOM200,
    IDM_VIEWZOOM400, IDM_VIEWZOOM800, IDM_VIEWFULLSCREEN = 241, IDM_VIEWSHOWGRID = 242, IDM_VIEWSHOWMINIATURE = 243,
    IDM_IMAGESTRETCHSKEW = 251, IDM_IMAGECROP = 252, IDM_IMAGEINVERTCOLORS = 253, IDM_IMAGEATTRIBUTES = 254,
    IDM_IMAGEDELETEIMAGE = 255, IDM_IMAGEDRAWOPAQUE = 256, IDM_COLORSEDITPALETTE = 260, IDM_COLORSMODERNPALETTE = 261,
    IDM_COLORSOLDPALETTE = 262, IDM_COLORSGRAYSCALE = 263, IDM_COLORSMONOCHROME = 264, IDM_HELPINFO = 271,
    IDM_BOLD = 280, IDM_ITALIC, IDM_UNDERLINE, IDM_VERTICAL, IDM_CTRL_PLUS = 290, IDM_CTRL_MINUS = 291,
    ID_ROTATE = 310, ID_ROT_R90, ID_ROT_L90, ID_ROT_180, ID_FLIP_V, ID_FLIP_H,
    ID_BRUSHES = 320, ID_SHAPES = 340, ID_OUTLINE = 345, ID_SHAPEFILL = 346, ID_OUTLINE_NONE = 347, ID_OUTLINE_SOLID, ID_OUTLINE_MARKER,
    ID_FILL_NONE = 350, ID_FILL_SOLID, ID_FILL_CRAYON, ID_SIZE = 360, ID_SIZE_1 = 361, ID_SIZE_2, ID_SIZE_3, ID_SIZE_4, ID_SIZE_5,
    ID_COLOR1 = 370, ID_COLOR2 = 371, ID_PALETTE = 372, ID_ZOOMLEVEL = 380, ID_RULERS = 381,
    ID_FONTFAMILY = 390, ID_FONTSIZE = 391, ID_BG_OPAQUE = 392, ID_BG_TRANSPARENT = 393,
    ID_FREESEL = 600, ID_RECTSEL, ID_RUBBER, ID_FILL, ID_COLOR, ID_ZOOM, ID_PEN, ID_BRUSH, ID_AIRBRUSH, ID_TEXT,
    ID_LINE, ID_BEZIER, ID_RECT, ID_SHAPE, ID_ELLIPSE, ID_RRECT,
    ID_PAL_FIRST = 3000, ID_PAL2_FIRST = 3100,      /* palette cells: left = Colour 1, right = Colour 2 */
};

struct AppState {
    HWND main_window;
    ILeewayRibbon *ribbon;
    HWND client_area;
    HACCEL accel;
    BOOL paint;
    /* viewer */
    int zoom;
    BOOL fit, actual, fullscreen, slideshow, animated, statusbar, has_image;
    int image_index, image_count;
    /* paint */
    UINT32 tool;
    COLORREF color1, color2;
    UINT32 active_well;
    BOOL bold, italic, underline, vertical, opaque, rulers, grid, thumbnail;
    WCHAR font_name[64], font_size[16];
    WCHAR last[200];
};

static AppState g_app;

/* ================================================================== */
/* Viewer mode (spec §1) */

static const LeewayUICommand g_viewer_commands[] = {
    /* id, label, tooltip, keytip, icon, enabled, checked, visible */
    {ID_OPEN, L"Open...", L"Open a picture", L"O", 1, TRUE, FALSE, TRUE},
    {IDC_SAVEAS, L"Save as...", L"Save a copy of the picture", L"A", 2, TRUE, FALSE, TRUE},
    {IDC_PRINT, L"Print", L"Print the picture", L"P", 3, TRUE, FALSE, TRUE},
    {IDC_MODIFY, L"Edit", L"Open the picture in an editor", L"E", 4, TRUE, FALSE, TRUE},
    {ID_WALLPAPER, L"Set as desktop background", NULL, L"B", 5, TRUE, FALSE, TRUE},
    {IDC_HELP_TOC, L"Help", L"Help topics", L"H", 6, TRUE, FALSE, TRUE},
    {ID_EXIT, L"Exit", L"Close the viewer", L"X", 7, TRUE, FALSE, TRUE},
    {IDC_PREV_PIC, L"Previous", L"Previous picture", L"P", 8, TRUE, FALSE, TRUE},
    {IDC_NEXT_PIC, L"Next", L"Next picture", L"N", 9, TRUE, FALSE, TRUE},
    {IDC_BEST_FIT, L"Fit", L"Fit the picture to the window", L"F", 10, TRUE, FALSE, TRUE},
    {IDC_REAL_SIZE, L"Actual size", L"Show the picture at 100 %", L"A", 11, TRUE, FALSE, TRUE},
    {IDC_ZOOM_IN, L"Zoom in", NULL, L"I", 12, TRUE, FALSE, TRUE},
    {IDC_ZOOM_OUT, L"Zoom out", NULL, L"O", 13, TRUE, FALSE, TRUE},
    {ID_ZOOM_GALLERY, L"Zoom level", L"Choose a zoom level", L"Z", 14, TRUE, FALSE, TRUE},
    {ID_ZOOM_10, L"10 %", NULL, NULL, 0, TRUE, FALSE, TRUE}, {ID_ZOOM_25, L"25 %", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_ZOOM_50, L"50 %", NULL, NULL, 0, TRUE, FALSE, TRUE}, {ID_ZOOM_75, L"75 %", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_ZOOM_100, L"100 %", NULL, NULL, 0, TRUE, FALSE, TRUE}, {ID_ZOOM_150, L"150 %", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_ZOOM_200, L"200 %", NULL, NULL, 0, TRUE, FALSE, TRUE}, {ID_ZOOM_400, L"400 %", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_ZOOM_800, L"800 %", NULL, NULL, 0, TRUE, FALSE, TRUE}, {ID_ZOOM_1600, L"1600 %", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {IDC_ROT_COUNCW, L"Rotate left", L"Rotate the view 90 degrees left", L"L", 15, TRUE, FALSE, TRUE},
    {IDC_ROT_CLOCKW, L"Rotate right", L"Rotate the view 90 degrees right", L"R", 16, TRUE, FALSE, TRUE},
    {IDC_ROT_CCWSAVE, L"Rotate left and save", NULL, L"L", 15, TRUE, FALSE, TRUE},
    {IDC_ROT_CWSAVE, L"Rotate right and save", NULL, L"R", 16, TRUE, FALSE, TRUE},
    {IDC_SLIDE_SHOW, L"Slideshow", L"Start or stop the slideshow", L"S", 17, TRUE, FALSE, TRUE},
    {IDC_INCTIMER, L"Faster", L"Shorter interval between pictures", L"F", 0, TRUE, FALSE, TRUE},
    {IDC_DECTIMER, L"Slower", L"Longer interval between pictures", L"S", 0, TRUE, FALSE, TRUE},
    {IDC_DELETE, L"Delete", L"Move the picture to the Recycle Bin", L"D", 18, TRUE, FALSE, TRUE},
    {IDC_TOGGLEFULLSCREEN, L"Full screen", L"Show the picture full screen", L"F", 19, TRUE, FALSE, TRUE},
    {LWUI_STATUSBAR_TOGGLE, L"Status bar", L"Show or hide the status bar", L"S", 20, TRUE, TRUE, TRUE},
    {LWUI_RIBBON_MINIMIZE, L"Minimize ribbon", L"Show only the tab names", L"M", 21, TRUE, FALSE, TRUE},
    {ID_ANIM_PLAY, L"Play", L"Play or pause the animation", L"P", 0, TRUE, FALSE, TRUE},
    {ID_ANIM_PREV, L"Previous frame", NULL, L"R", 0, TRUE, FALSE, TRUE},
    {ID_ANIM_NEXT, L"Next frame", NULL, L"N", 0, TRUE, FALSE, TRUE},
    {ID_DEMO_ANIMATED, L"Animated picture", L"Demo: pretend the picture is animated (shows the contextual tab)", L"G", LWUI_ICON_GLYPH | 22, TRUE, FALSE, TRUE},
};

static const LeewayUIPlacement g_v_navigate[] = {
    {IDC_PREV_PIC, LWUI_CONTROL_BUTTON_LARGE, NULL, 0},
    {IDC_NEXT_PIC, LWUI_CONTROL_BUTTON_LARGE, NULL, 0},
};
static const LeewayUIPlacement g_v_zoom[] = {
    {IDC_BEST_FIT, LWUI_CONTROL_TOGGLE_LARGE, NULL, 0},
    {IDC_REAL_SIZE, LWUI_CONTROL_TOGGLE_LARGE, NULL, 0},
    {IDC_ZOOM_IN, LWUI_CONTROL_BUTTON_SMALL, NULL, 0},
    {IDC_ZOOM_OUT, LWUI_CONTROL_BUTTON_SMALL, NULL, 0},
    {ID_ZOOM_GALLERY, LWUI_CONTROL_GALLERY, NULL, 0},
};
/* View tab copy of the Zoom group: Full screen takes F there (spec §1.6, Alt,V,F), so Fit gets B (Ctrl+B) */
static const LeewayUIPlacement g_v_zoom_view[] = {
    {IDC_BEST_FIT, LWUI_CONTROL_TOGGLE_LARGE, L"B", 0},
    {IDC_REAL_SIZE, LWUI_CONTROL_TOGGLE_LARGE, NULL, 0},
    {IDC_ZOOM_IN, LWUI_CONTROL_BUTTON_SMALL, NULL, 0},
    {IDC_ZOOM_OUT, LWUI_CONTROL_BUTTON_SMALL, NULL, 0},
    {ID_ZOOM_GALLERY, LWUI_CONTROL_GALLERY, NULL, 0},
};
static const LeewayUIPlacement g_v_rotate[] = {
    {IDC_ROT_COUNCW, LWUI_CONTROL_SPLIT_LARGE, NULL, 0},
    {IDC_ROT_CLOCKW, LWUI_CONTROL_SPLIT_LARGE, NULL, 0},
};
static const LeewayUIPlacement g_v_slideshow[] = {
    {IDC_SLIDE_SHOW, LWUI_CONTROL_SPLIT_LARGE, NULL, 0},
};
static const LeewayUIPlacement g_v_file[] = {
    {IDC_DELETE, LWUI_CONTROL_BUTTON_LARGE, NULL, 0},
    {IDC_SAVEAS, LWUI_CONTROL_BUTTON_SMALL, L"V", 0},    /* per-placement keytip: A in the app menu, V here */
    {IDC_PRINT, LWUI_CONTROL_BUTTON_SMALL, L"T", 0},
    {IDC_MODIFY, LWUI_CONTROL_BUTTON_SMALL, NULL, 0},
};
static const LeewayUIPlacement g_v_display[] = {
    {IDC_TOGGLEFULLSCREEN, LWUI_CONTROL_TOGGLE_LARGE, NULL, 0},
    {LWUI_STATUSBAR_TOGGLE, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
    {LWUI_RIBBON_MINIMIZE, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
    {ID_DEMO_ANIMATED, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
};
static const LeewayUIPlacement g_v_help[] = {
    {IDC_HELP_TOC, LWUI_CONTROL_BUTTON_SMALL, NULL, 0},
};
static const LeewayUIPlacement g_v_anim[] = {
    {ID_ANIM_PLAY, LWUI_CONTROL_TOGGLE_LARGE, NULL, 0},
    {ID_ANIM_PREV, LWUI_CONTROL_BUTTON_SMALL, NULL, 0},
    {ID_ANIM_NEXT, LWUI_CONTROL_BUTTON_SMALL, NULL, 0},
};

#define ORDER_ALL LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED)

static const LeewayUIGroup g_v_home_groups[] = {
    {L"Navigate", NULL, 0, 0, 2, g_v_navigate},
    {L"Zoom", NULL, ORDER_ALL, 0, 5, g_v_zoom},
    {L"Rotate", NULL, 0, 0, 2, g_v_rotate},
    {L"Slideshow", NULL, 0, 0, 1, g_v_slideshow},
    {L"File", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, 0), 0, 4, g_v_file},
};
static const LeewayUIGroup g_v_view_groups[] = {
    {L"Zoom", NULL, ORDER_ALL, 0, 5, g_v_zoom_view},
    {L"Display", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, 0), 0, 4, g_v_display},
    {L"Help", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_SMALL, 0, 0, 0), 0, 1, g_v_help},
};
static const LeewayUIGroup g_v_anim_groups[] = {
    {L"Frames", NULL, 0, 0, 3, g_v_anim},
};
static const LeewayUITab g_v_tabs[] = {
    {TAB_HOME, L"Home", L"H", NULL, 5, g_v_home_groups},
    {TAB_VIEW, L"View", L"V", NULL, 3, g_v_view_groups},
    {TAB_ANIMATION, L"Animation", L"A", L"IsAnimated", 1, g_v_anim_groups},
};
static const UINT32 g_v_appmenu[] = {ID_OPEN, IDC_SAVEAS, IDC_PRINT, IDC_MODIFY, ID_WALLPAPER, IDC_HELP_TOC, ID_EXIT};
static const UINT32 g_v_qat_default[] = {IDC_PREV_PIC, IDC_NEXT_PIC, IDC_ROT_COUNCW, IDC_ROT_CLOCKW};
static const LeewayUILayout g_viewer_layout = {
    L"LeewayUIRibbonTest", 3, g_v_tabs, 7, g_v_appmenu, 4, g_v_qat_default, IDC_HELP_TOC,
};

static const LeewayUICollectionItem g_v_zoom_levels[] = {
    {ID_ZOOM_10, L"10 %", 0, 0, 0}, {ID_ZOOM_25, L"25 %", 0, 0, 0}, {ID_ZOOM_50, L"50 %", 0, 0, 0}, {ID_ZOOM_75, L"75 %", 0, 0, 0},
    {ID_ZOOM_100, L"100 %", 0, 0, 0}, {ID_ZOOM_150, L"150 %", 0, 0, 0}, {ID_ZOOM_200, L"200 %", 0, 0, 0}, {ID_ZOOM_400, L"400 %", 0, 0, 0},
    {ID_ZOOM_800, L"800 %", 0, 0, 0}, {ID_ZOOM_1600, L"1600 %", 0, 0, 0}, {IDC_BEST_FIT, L"Fit", 10, 0, 0}, {IDC_REAL_SIZE, L"Actual size", 11, 0, 0},
};
static const LeewayUICollectionItem g_v_rot_left_menu[] = {{IDC_ROT_CCWSAVE, L"Rotate left and save", 15, 0, 0}};
static const LeewayUICollectionItem g_v_rot_right_menu[] = {{IDC_ROT_CWSAVE, L"Rotate right and save", 16, 0, 0}};
static const LeewayUICollectionItem g_v_slideshow_menu[] = {{IDC_INCTIMER, L"Faster", 0, 0, 0}, {IDC_DECTIMER, L"Slower", 0, 0, 0}};
static const int g_v_levels[] = {10, 25, 50, 75, 100, 150, 200, 400, 800, 1600};

/* ================================================================== */
/* Paint mode (spec §2) */

static const LeewayUICommand g_paint_commands[] = {
    {IDM_FILENEW, L"New", L"Create a new picture", L"N", 0, TRUE, FALSE, TRUE},
    {IDM_FILEOPEN, L"Open...", L"Open a picture", L"O", 0, TRUE, FALSE, TRUE},
    {IDM_FILESAVE, L"Save", L"Save the picture", L"S", 0, TRUE, FALSE, TRUE},
    {IDM_FILESAVEAS, L"Save as", L"Save a copy", L"A", 0, TRUE, FALSE, TRUE},
    {IDM_FILEPRINT, L"Print", L"Print the picture", L"P", 0, TRUE, FALSE, TRUE},
    {IDM_FILEPAGESETUP, L"Page setup", NULL, L"G", 0, TRUE, FALSE, TRUE},
    {IDM_FILEPAGEVIEW, L"Print preview", NULL, L"V", 0, TRUE, FALSE, TRUE},
    {IDM_FILEFROMSCANNERORCAMERA, L"From scanner or camera", NULL, L"M", 0, FALSE, FALSE, TRUE},
    {IDM_FILESEND, L"Send in e-mail", NULL, L"D", 0, TRUE, FALSE, TRUE},
    {IDM_FILEASWALLPAPERPLANE, L"Set as desktop background", NULL, L"B", 0, TRUE, FALSE, TRUE},
    {IDM_IMAGEATTRIBUTES, L"Properties", L"Image attributes", L"I", 0, TRUE, FALSE, TRUE},
    {IDM_HELPINFO, L"About", NULL, L"T", 0, TRUE, FALSE, TRUE},
    {IDM_FILEEXIT, L"Exit", NULL, L"X", 0, TRUE, FALSE, TRUE},
    {IDM_EDITUNDO, L"Undo", L"Undo the last action", NULL, 0, TRUE, FALSE, TRUE},
    {IDM_EDITREDO, L"Redo", L"Redo the undone action", NULL, 0, TRUE, FALSE, TRUE},
    {IDM_EDITPASTE, L"Paste", L"Paste from the clipboard", L"V", 0, TRUE, FALSE, TRUE},
    {IDM_EDITPASTEFROM, L"Paste from...", NULL, L"F", 0, TRUE, FALSE, TRUE},
    {IDM_EDITCUT, L"Cut", L"Cut the selection", L"X", 0, TRUE, FALSE, TRUE},
    {IDM_EDITCOPY, L"Copy", L"Copy the selection", L"CP", 0, TRUE, FALSE, TRUE},   /* spec said C: a prefix of C1/C2 */
    {ID_RECTSEL, L"Select", L"Rectangular selection", L"SE", 0, TRUE, FALSE, TRUE},
    {ID_FREESEL, L"Free-form selection", NULL, L"F", 0, TRUE, FALSE, TRUE},
    {IDM_EDITSELECTALL, L"Select all", NULL, L"A", 0, TRUE, FALSE, TRUE},
    {IDM_EDITINVERTSELECTION, L"Invert selection", NULL, L"I", 0, TRUE, FALSE, TRUE},
    {IDM_EDITDELETESELECTION, L"Delete", NULL, L"D", 0, TRUE, FALSE, TRUE},
    {IDM_IMAGEDRAWOPAQUE, L"Transparent selection", NULL, L"T", 0, TRUE, FALSE, TRUE},
    {IDM_IMAGECROP, L"Crop", L"Crop to the selection", L"RP", 0, TRUE, FALSE, TRUE},
    {IDM_IMAGESTRETCHSKEW, L"Resize", L"Resize and skew", L"RE", 0, TRUE, FALSE, TRUE},
    {ID_ROTATE, L"Rotate", L"Rotate or flip", L"RO", 0, TRUE, FALSE, TRUE},
    {ID_ROT_R90, L"Rotate right 90°", NULL, L"R", 0, TRUE, FALSE, TRUE},
    {ID_ROT_L90, L"Rotate left 90°", NULL, L"L", 0, TRUE, FALSE, TRUE},
    {ID_ROT_180, L"Rotate 180°", NULL, L"T", 0, TRUE, FALSE, TRUE},
    {ID_FLIP_V, L"Flip vertical", NULL, L"V", 0, TRUE, FALSE, TRUE},
    {ID_FLIP_H, L"Flip horizontal", NULL, L"H", 0, TRUE, FALSE, TRUE},
    {IDM_IMAGEINVERTCOLORS, L"Invert colours", NULL, L"I", 0, TRUE, FALSE, TRUE},
    {IDM_IMAGEDELETEIMAGE, L"Delete image", NULL, L"D", 0, TRUE, FALSE, TRUE},
    {IDM_COLORSGRAYSCALE, L"Greyscale", NULL, L"G", 0, TRUE, FALSE, TRUE},
    {IDM_COLORSMONOCHROME, L"Monochrome", NULL, L"M", 0, TRUE, FALSE, TRUE},
    {ID_PEN, L"Pencil", NULL, L"PE", 0, TRUE, FALSE, TRUE},        /* spec said P: a prefix of PA (Palette) */
    {ID_FILL, L"Fill with colour", NULL, L"K", 0, TRUE, FALSE, TRUE},
    {ID_TEXT, L"Text", L"Insert text (shows the Text tab)", L"T", 0, TRUE, FALSE, TRUE},
    {ID_RUBBER, L"Eraser", NULL, L"ER", 0, TRUE, FALSE, TRUE},     /* spec said E: a prefix of EC (Edit colours) */
    {ID_COLOR, L"Colour picker", NULL, L"I", 0, TRUE, FALSE, TRUE},
    {ID_ZOOM, L"Magnifier", NULL, L"M", 0, TRUE, FALSE, TRUE},
    {ID_BRUSHES, L"Brushes", L"Choose a brush", L"B", 0, TRUE, FALSE, TRUE},
    {ID_BRUSH, L"Brush", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_AIRBRUSH, L"Airbrush", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_SHAPES, L"Shapes", L"Choose a shape", L"SH", 0, TRUE, FALSE, TRUE},
    {ID_LINE, L"Line", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_BEZIER, L"Curve", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_RECT, L"Rectangle", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_SHAPE, L"Polygon", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_ELLIPSE, L"Ellipse", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_RRECT, L"Rounded rectangle", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_OUTLINE, L"Outline", NULL, L"O", 0, TRUE, FALSE, TRUE},
    {ID_OUTLINE_NONE, L"No outline", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_OUTLINE_SOLID, L"Solid colour", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_OUTLINE_MARKER, L"Marker", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_SHAPEFILL, L"Fill", NULL, L"F", 0, TRUE, FALSE, TRUE},
    {ID_FILL_NONE, L"No fill", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_FILL_SOLID, L"Solid colour", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_FILL_CRAYON, L"Crayon", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_SIZE, L"Size", L"Line width", L"SZ", 0, TRUE, FALSE, TRUE},
    {ID_SIZE_1, L"1 px", NULL, NULL, 0, TRUE, FALSE, TRUE}, {ID_SIZE_2, L"2 px", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_SIZE_3, L"3 px", NULL, NULL, 0, TRUE, FALSE, TRUE}, {ID_SIZE_4, L"5 px", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_SIZE_5, L"8 px", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_COLOR1, L"Colour 1", L"Foreground colour (left button)", L"C1", 0, TRUE, FALSE, TRUE},
    {ID_COLOR2, L"Colour 2", L"Background colour (right button)", L"C2", 0, TRUE, FALSE, TRUE},
    {ID_PALETTE, L"Palette", L"Left click: Colour 1, right click: Colour 2", L"PA", 0, TRUE, FALSE, TRUE},
    {IDM_COLORSMODERNPALETTE, L"Modern palette", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {IDM_COLORSOLDPALETTE, L"Classic palette", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {IDM_COLORSEDITPALETTE, L"Edit colours", L"Edit the palette", L"EC", 0, TRUE, FALSE, TRUE},
    {IDM_CTRL_PLUS, L"Zoom in", NULL, L"I", 0, TRUE, FALSE, TRUE},
    {IDM_CTRL_MINUS, L"Zoom out", NULL, L"O", 0, TRUE, FALSE, TRUE},
    {IDM_VIEWZOOM100, L"100 %", L"Actual size", L"1", 0, TRUE, FALSE, TRUE},
    {ID_ZOOMLEVEL, L"Zoom level", NULL, L"Z", 0, TRUE, FALSE, TRUE},
    {IDM_VIEWZOOM125, L"12.5 %", NULL, NULL, 0, TRUE, FALSE, TRUE}, {IDM_VIEWZOOM25, L"25 %", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {IDM_VIEWZOOM50, L"50 %", NULL, NULL, 0, TRUE, FALSE, TRUE}, {IDM_VIEWZOOM200, L"200 %", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {IDM_VIEWZOOM400, L"400 %", NULL, NULL, 0, TRUE, FALSE, TRUE}, {IDM_VIEWZOOM800, L"800 %", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_RULERS, L"Rulers", L"Show the rulers", L"R", 0, TRUE, FALSE, TRUE},
    {IDM_VIEWSHOWGRID, L"Gridlines", L"Show the grid", L"G", 0, TRUE, FALSE, TRUE},
    {IDM_VIEWSTATUSBAR, L"Status bar", NULL, L"S", 0, TRUE, TRUE, TRUE},
    {IDM_VIEWFULLSCREEN, L"Full screen", NULL, L"F", 0, TRUE, FALSE, TRUE},
    {IDM_VIEWSHOWMINIATURE, L"Thumbnail", NULL, L"T", 0, TRUE, FALSE, TRUE},
    {ID_FONTFAMILY, L"Font", L"Font family", L"FF", 0, TRUE, FALSE, TRUE},
    {ID_FONTSIZE, L"Size", L"Font size", L"FS", 0, TRUE, FALSE, TRUE},
    {IDM_BOLD, L"Bold", NULL, L"B", LWUI_ICON_GLYPH | 0, TRUE, FALSE, TRUE},
    {IDM_ITALIC, L"Italic", NULL, L"I", LWUI_ICON_GLYPH | 0, TRUE, FALSE, TRUE},
    {IDM_UNDERLINE, L"Underline", NULL, L"U", LWUI_ICON_GLYPH | 0, TRUE, FALSE, TRUE},
    {IDM_VERTICAL, L"Vertical", NULL, L"VT", LWUI_ICON_GLYPH | 0, TRUE, FALSE, TRUE},
    {ID_BG_OPAQUE, L"Opaque", L"Opaque background", L"O", 0, TRUE, TRUE, TRUE},
    {ID_BG_TRANSPARENT, L"Transparent", L"Transparent background", L"R", 0, TRUE, FALSE, TRUE},
};

static const LeewayUIPlacement g_p_clipboard[] = {
    {IDM_EDITPASTE, LWUI_CONTROL_SPLIT_LARGE, NULL, 0},
    {IDM_EDITCUT, LWUI_CONTROL_BUTTON_SMALL, NULL, 0},
    {IDM_EDITCOPY, LWUI_CONTROL_BUTTON_SMALL, NULL, 0},
};
static const LeewayUIPlacement g_p_image[] = {
    {ID_RECTSEL, LWUI_CONTROL_SPLIT_LARGE, NULL, 0},
    {IDM_IMAGECROP, LWUI_CONTROL_BUTTON_SMALL, NULL, 0},
    {IDM_IMAGESTRETCHSKEW, LWUI_CONTROL_BUTTON_SMALL, NULL, 0},
    {ID_ROTATE, LWUI_CONTROL_DROPDOWN, NULL, 0},
};
static const LeewayUIPlacement g_p_tools[] = {
    {ID_PEN, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
    {ID_FILL, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
    {ID_TEXT, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
    {ID_RUBBER, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
    {ID_COLOR, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
    {ID_ZOOM, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
};
static const LeewayUIPlacement g_p_brushes[] = {
    {ID_BRUSHES, LWUI_CONTROL_GALLERY, NULL, 0},
};
static const LeewayUIPlacement g_p_shapes[] = {
    {ID_SHAPES, LWUI_CONTROL_GALLERY_STRIP, NULL, 0},          /* in-ribbon strip, 3 cells wide (G16) */
    {ID_OUTLINE, LWUI_CONTROL_DROPDOWN, NULL, 0},
    {ID_SHAPEFILL, LWUI_CONTROL_DROPDOWN, NULL, 0},
};
static const LeewayUIPlacement g_p_size[] = {
    {ID_SIZE, LWUI_CONTROL_DROPDOWN, NULL, 0},
};
static const LeewayUIPlacement g_p_colours[] = {
    {ID_COLOR1, LWUI_CONTROL_COLORWELL, NULL, 0},
    {ID_COLOR2, LWUI_CONTROL_COLORWELL, NULL, 0},
    {ID_PALETTE, LWUI_CONTROL_GALLERY_STRIP, NULL, 234},       /* 10 cells + scroll column */
    {IDM_COLORSEDITPALETTE, LWUI_CONTROL_BUTTON_LARGE, NULL, 0},
};
static const LeewayUIPlacement g_p_zoom[] = {
    {IDM_CTRL_PLUS, LWUI_CONTROL_BUTTON_LARGE, NULL, 0},
    {IDM_CTRL_MINUS, LWUI_CONTROL_BUTTON_LARGE, NULL, 0},
    {IDM_VIEWZOOM100, LWUI_CONTROL_BUTTON_SMALL, NULL, 0},
    {ID_ZOOMLEVEL, LWUI_CONTROL_GALLERY, NULL, 0},
};
static const LeewayUIPlacement g_p_showhide[] = {
    {ID_RULERS, LWUI_CONTROL_CHECKBOX, NULL, 0},
    {IDM_VIEWSHOWGRID, LWUI_CONTROL_CHECKBOX, NULL, 0},
    {IDM_VIEWSTATUSBAR, LWUI_CONTROL_CHECKBOX, NULL, 0},
};
static const LeewayUIPlacement g_p_display[] = {
    {IDM_VIEWFULLSCREEN, LWUI_CONTROL_TOGGLE_LARGE, NULL, 0},
    {IDM_VIEWSHOWMINIATURE, LWUI_CONTROL_TOGGLE_LARGE, NULL, 0},
};
static const LeewayUIPlacement g_p_font[] = {
    {ID_FONTFAMILY, LWUI_CONTROL_COMBO, NULL, 130},
    {ID_FONTSIZE, LWUI_CONTROL_COMBO, NULL, 50},
    {IDM_BOLD, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
    {IDM_ITALIC, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
    {IDM_UNDERLINE, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
    {IDM_VERTICAL, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
};
static const LeewayUIPlacement g_p_background[] = {
    {ID_BG_OPAQUE, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
    {ID_BG_TRANSPARENT, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0},
};

static const LeewayUIGroup g_p_home_groups[] = {
    {L"Clipboard", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, 0), 0, 3, g_p_clipboard},
    {L"Image", NULL, ORDER_ALL, IDM_IMAGEATTRIBUTES, 4, g_p_image},           /* dialog launcher (G17) */
    {L"Tools", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, 0, 0), 0, 6, g_p_tools},
    {L"Brushes", NULL, 0, 0, 1, g_p_brushes},
    {L"Shapes", NULL, ORDER_ALL, 0, 3, g_p_shapes},
    {L"Size", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_SMALL, 0, 0, 0), 0, 1, g_p_size},
    {L"Colours", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, 0, 0), 0, 4, g_p_colours},
};
static const LeewayUIGroup g_p_view_groups[] = {
    {L"Zoom", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, 0), 0, 4, g_p_zoom},
    {L"Show or hide", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_SMALL, 0, 0, 0), 0, 3, g_p_showhide},
    {L"Display", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, 0), 0, 2, g_p_display},
};
static const LeewayUIGroup g_p_text_groups[] = {
    {L"Font", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, 0, 0), 0, 6, g_p_font},
    {L"Background", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_SMALL, 0, 0, 0), 0, 2, g_p_background},
    {L"Colours", NULL, LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_COLLAPSED, 0, 0), 0, 4, g_p_colours},
};
static const LeewayUITab g_p_tabs[] = {
    {TAB_HOME, L"Home", L"H", NULL, 7, g_p_home_groups},
    {TAB_VIEW, L"View", L"V", NULL, 3, g_p_view_groups},
    {TAB_TEXT, L"Text", L"T", L"IsTextTool", 3, g_p_text_groups},
};
static const UINT32 g_p_appmenu[] = {IDM_FILENEW, IDM_FILEOPEN, IDM_FILESAVE, IDM_FILESAVEAS, IDM_FILEPRINT, IDM_FILEFROMSCANNERORCAMERA,
                                     IDM_FILESEND, IDM_FILEASWALLPAPERPLANE, IDM_IMAGEATTRIBUTES, IDM_HELPINFO, IDM_FILEEXIT};
static const UINT32 g_p_qat_default[] = {IDM_FILESAVE, IDM_EDITUNDO, IDM_EDITREDO};
static const LeewayUILayout g_paint_layout = {
    L"LeewayUIPaintTest", 3, g_p_tabs, 11, g_p_appmenu, 3, g_p_qat_default, IDM_HELPINFO,
};

static const LeewayUICollectionItem g_p_paste_menu[] = {{IDM_EDITPASTE, L"Paste", 0, 0, 0}, {IDM_EDITPASTEFROM, L"Paste from...", 0, 0, 0}};
static const LeewayUICollectionItem g_p_select_menu[] = {
    {ID_RECTSEL, L"Rectangular selection", 0, 0, 0}, {ID_FREESEL, L"Free-form selection", 0, 0, 0},
    {IDM_EDITSELECTALL, L"Select all", 0, 0, 0}, {IDM_EDITINVERTSELECTION, L"Invert selection", 0, 0, 0},
    {IDM_EDITDELETESELECTION, L"Delete", 0, 0, 0}, {IDM_IMAGEDRAWOPAQUE, L"Transparent selection", 0, 0, 0},
};
static const LeewayUICollectionItem g_p_rotate_menu[] = {
    {ID_ROT_R90, L"Rotate right 90°", 0, 0, 0}, {ID_ROT_L90, L"Rotate left 90°", 0, 0, 0}, {ID_ROT_180, L"Rotate 180°", 0, 0, 0},
    {ID_FLIP_V, L"Flip vertical", 0, 0, 0}, {ID_FLIP_H, L"Flip horizontal", 0, 0, 0},
    {IDM_IMAGEINVERTCOLORS, L"Invert colours", 0, 0, 0}, {IDM_IMAGEDELETEIMAGE, L"Delete image", 0, 0, 0},
    {IDM_COLORSGRAYSCALE, L"Greyscale", 0, 0, 0}, {IDM_COLORSMONOCHROME, L"Monochrome", 0, 0, 0},
};
static const LeewayUICollectionItem g_p_brush_items[] = {
    {ID_BRUSH, L"Brush", 0, 0, 0}, {ID_AIRBRUSH, L"Airbrush", 0, 0, 0}, {321, L"Calligraphy 1", 0, 0, 0}, {322, L"Calligraphy 2", 0, 0, 0},
    {323, L"Oil brush", 0, 0, 0}, {324, L"Crayon", 0, 0, 0}, {325, L"Marker", 0, 0, 0}, {326, L"Watercolour", 0, 0, 0},
};
static const LeewayUICollectionItem g_p_shape_items[] = {
    {ID_LINE, L"Line", 0, 0, 0}, {ID_BEZIER, L"Curve", 0, 0, 0}, {ID_RECT, L"Rectangle", 0, 0, 0},
    {ID_SHAPE, L"Polygon", 0, 0, 0}, {ID_ELLIPSE, L"Ellipse", 0, 0, 0}, {ID_RRECT, L"Rounded rectangle", 0, 0, 0},
};
static const LeewayUICollectionItem g_p_outline_items[] = {{ID_OUTLINE_NONE, L"No outline", 0, 0, 0}, {ID_OUTLINE_SOLID, L"Solid colour", 0, 0, 0}, {ID_OUTLINE_MARKER, L"Marker", 0, 0, 0}};
static const LeewayUICollectionItem g_p_fill_items[] = {{ID_FILL_NONE, L"No fill", 0, 0, 0}, {ID_FILL_SOLID, L"Solid colour", 0, 0, 0}, {ID_FILL_CRAYON, L"Crayon", 0, 0, 0}};
static const LeewayUICollectionItem g_p_size_items[] = {{ID_SIZE_1, L"1 px", 0, 0, 0}, {ID_SIZE_2, L"2 px", 0, 0, 0}, {ID_SIZE_3, L"3 px", 0, 0, 0}, {ID_SIZE_4, L"5 px", 0, 0, 0}, {ID_SIZE_5, L"8 px", 0, 0, 0}};
static const LeewayUICollectionItem g_p_zoom_items[] = {
    {IDM_VIEWZOOM125, L"12.5 %", 0, 0, 0}, {IDM_VIEWZOOM25, L"25 %", 0, 0, 0}, {IDM_VIEWZOOM50, L"50 %", 0, 0, 0}, {IDM_VIEWZOOM100, L"100 %", 0, 0, 0},
    {IDM_VIEWZOOM200, L"200 %", 0, 0, 0}, {IDM_VIEWZOOM400, L"400 %", 0, 0, 0}, {IDM_VIEWZOOM800, L"800 %", 0, 0, 0},
};
static const LeewayUICollectionItem g_p_font_names[] = {
    {0, L"Arial", 0, 0, 0}, {0, L"Calibri", 0, 0, 0}, {0, L"Consolas", 0, 0, 0}, {0, L"Segoe UI", 0, 0, 0},
    {0, L"Tahoma", 0, 0, 0}, {0, L"Times New Roman", 0, 0, 0}, {0, L"Verdana", 0, 0, 0},
};
static const LeewayUICollectionItem g_p_font_sizes[] = {
    {0, L"8", 0, 0, 0}, {0, L"9", 0, 0, 0}, {0, L"10", 0, 0, 0}, {0, L"11", 0, 0, 0}, {0, L"12", 0, 0, 0}, {0, L"14", 0, 0, 0}, {0, L"16", 0, 0, 0},
    {0, L"18", 0, 0, 0}, {0, L"20", 0, 0, 0}, {0, L"24", 0, 0, 0}, {0, L"28", 0, 0, 0}, {0, L"36", 0, 0, 0}, {0, L"48", 0, 0, 0}, {0, L"72", 0, 0, 0},
};
/* the modern palette: 20 colours + the two palette switches as text rows below the grid */
static const COLORREF g_p_palette[20] = {
    RGB(0, 0, 0), RGB(127, 127, 127), RGB(136, 0, 21), RGB(237, 28, 36), RGB(255, 127, 39), RGB(255, 242, 0), RGB(34, 177, 76),
    RGB(0, 162, 232), RGB(63, 72, 204), RGB(163, 73, 164), RGB(255, 255, 255), RGB(195, 195, 195), RGB(185, 122, 87),
    RGB(255, 174, 201), RGB(255, 201, 14), RGB(239, 228, 176), RGB(181, 230, 29), RGB(153, 217, 234), RGB(112, 146, 190), RGB(200, 191, 231),
};
static const WCHAR *const g_p_palette_names[20] = {
    L"Black", L"Grey 50 %", L"Dark red", L"Red", L"Orange", L"Yellow", L"Green", L"Turquoise", L"Indigo", L"Purple",
    L"White", L"Grey 25 %", L"Brown", L"Rose", L"Gold", L"Light yellow", L"Lime", L"Light turquoise", L"Blue grey", L"Lavender",
};
static LeewayUICollectionItem g_p_palette_items[22];

static void Note(const WCHAR *what)
{
    lstrcpynW(g_app.last, what, 200);
    if (g_app.client_area) SetWindowTextW(g_app.client_area, g_app.last);
}

static void Relayout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int h = g_app.ribbon ? (int)LeewayRibbonGetHeight(g_app.ribbon) : 0;
    if (g_app.ribbon) SetWindowPos(LeewayRibbonGetHWND(g_app.ribbon), NULL, 0, 0, rc.right, h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_app.client_area) SetWindowPos(g_app.client_area, NULL, 0, h, rc.right, rc.bottom - h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static const WCHAR *LabelOf(UINT32 id)
{
    const LeewayUICommand *t = g_app.paint ? g_paint_commands : g_viewer_commands;
    UINT32 n = g_app.paint ? sizeof(g_paint_commands) / sizeof(g_paint_commands[0]) : sizeof(g_viewer_commands) / sizeof(g_viewer_commands[0]);
    for (UINT32 i = 0; i < n; i++) if (t[i].id == id) return t[i].label;
    return L"";
}

/* ---- the two callbacks, viewer ---- */

static HRESULT ViewerExecute(UINT32 id)
{
    switch (id) {
    case IDC_BEST_FIT: g_app.fit = TRUE; g_app.actual = FALSE; g_app.zoom = 100; break;
    case IDC_REAL_SIZE: g_app.actual = TRUE; g_app.fit = FALSE; g_app.zoom = 100; break;
    case IDC_ZOOM_IN: g_app.fit = g_app.actual = FALSE; if (g_app.zoom < 1600) g_app.zoom = g_app.zoom < 100 ? g_app.zoom + 25 : g_app.zoom * 2; break;
    case IDC_ZOOM_OUT: g_app.fit = g_app.actual = FALSE; if (g_app.zoom > 10) g_app.zoom = g_app.zoom <= 100 ? g_app.zoom - 25 : g_app.zoom / 2; if (g_app.zoom < 10) g_app.zoom = 10; break;
    case IDC_TOGGLEFULLSCREEN: g_app.fullscreen = !g_app.fullscreen; break;
    case IDC_SLIDE_SHOW: g_app.slideshow = !g_app.slideshow; break;
    case LWUI_STATUSBAR_TOGGLE: g_app.statusbar = !g_app.statusbar; break;
    case ID_DEMO_ANIMATED:
        g_app.animated = !g_app.animated;
        LeewayRibbonSetContext(g_app.ribbon, L"IsAnimated", g_app.animated);
        break;
    case IDC_PREV_PIC: if (g_app.image_index > 0) g_app.image_index--; break;
    case IDC_NEXT_PIC: if (g_app.image_index + 1 < g_app.image_count) g_app.image_index++; break;
    case ID_EXIT: PostMessageW(g_app.main_window, WM_CLOSE, 0, 0); break;
    default:
        if (id >= ID_ZOOM_10 && id <= ID_ZOOM_1600) {
            g_app.zoom = g_v_levels[id - ID_ZOOM_10];
            g_app.fit = g_app.actual = FALSE;
        }
        break;
    }
    WCHAR msg[200];
    wsprintfW(msg, L"execute(%u) %s   zoom %d %%, picture %d of %d", id, LabelOf(id), g_app.zoom, g_app.image_index + 1, g_app.image_count);
    Note(msg);
    return S_OK;
}

static HRESULT ViewerState(UINT32 id, LeewayUICommandState *st)
{
    static WCHAR zoom_label[16];
    BOOL has_list = g_app.image_count > 1;
    switch (id) {
    case IDC_PREV_PIC: st->enabled = has_list && g_app.image_index > 0; break;
    case IDC_NEXT_PIC: st->enabled = has_list && g_app.image_index + 1 < g_app.image_count; break;
    case IDC_BEST_FIT: st->checked = g_app.fit; break;
    case IDC_REAL_SIZE: st->checked = g_app.actual; break;
    case IDC_ZOOM_IN: st->enabled = g_app.zoom < 1600; break;
    case IDC_ZOOM_OUT: st->enabled = g_app.zoom > 10; break;
    case ID_ZOOM_GALLERY: wsprintfW(zoom_label, L"%d %%", g_app.zoom); st->label = zoom_label; break;
    case IDC_TOGGLEFULLSCREEN: st->checked = g_app.fullscreen; break;
    case IDC_SLIDE_SHOW: st->checked = g_app.slideshow; break;
    case LWUI_STATUSBAR_TOGGLE: st->checked = g_app.statusbar; break;
    case ID_DEMO_ANIMATED: st->checked = g_app.animated; break;
    case ID_ANIM_PLAY: st->checked = g_app.slideshow; break;
    default: break;
    }
    return S_OK;
}

/* ---- the two callbacks, paint ---- */

static BOOL IsTool(UINT32 id) { return id >= ID_FREESEL && id <= ID_RRECT; }
static BOOL IsShapeTool(UINT32 id) { return id >= ID_LINE && id <= ID_RRECT; }

static HRESULT PaintExecute(UINT32 id)
{
    WCHAR msg[200];
    if (IsTool(id) || (id >= 321 && id <= 326)) {
        g_app.tool = id;
        LeewayRibbonSetContext(g_app.ribbon, L"IsTextTool", id == ID_TEXT);   /* the Text tab (G13) */
    } else if (id >= ID_PAL_FIRST && id < ID_PAL_FIRST + 20) {
        g_app.color1 = g_p_palette[id - ID_PAL_FIRST];
    } else if (id >= ID_PAL2_FIRST && id < ID_PAL2_FIRST + 20) {
        g_app.color2 = g_p_palette[id - ID_PAL2_FIRST];
    } else {
        switch (id) {
        case ID_COLOR1: case ID_COLOR2: g_app.active_well = id; break;
        case ID_FONTFAMILY: { UINT32 n = 64; LeewayRibbonGetComboText(g_app.ribbon, id, g_app.font_name, &n); break; }
        case ID_FONTSIZE: { UINT32 n = 16; LeewayRibbonGetComboText(g_app.ribbon, id, g_app.font_size, &n); break; }
        case IDM_BOLD: g_app.bold = !g_app.bold; break;
        case IDM_ITALIC: g_app.italic = !g_app.italic; break;
        case IDM_UNDERLINE: g_app.underline = !g_app.underline; break;
        case IDM_VERTICAL: g_app.vertical = !g_app.vertical; break;
        case ID_BG_OPAQUE: g_app.opaque = TRUE; break;
        case ID_BG_TRANSPARENT: g_app.opaque = FALSE; break;
        case ID_RULERS: g_app.rulers = !g_app.rulers; break;
        case IDM_VIEWSHOWGRID: g_app.grid = !g_app.grid; break;
        case IDM_VIEWSTATUSBAR: g_app.statusbar = !g_app.statusbar; break;
        case IDM_VIEWFULLSCREEN: g_app.fullscreen = !g_app.fullscreen; break;
        case IDM_VIEWSHOWMINIATURE: g_app.thumbnail = !g_app.thumbnail; break;
        case IDM_CTRL_PLUS: if (g_app.zoom < 800) g_app.zoom *= 2; break;
        case IDM_CTRL_MINUS: if (g_app.zoom > 25) g_app.zoom /= 2; break;
        case IDM_VIEWZOOM100: g_app.zoom = 100; break;
        case IDM_VIEWZOOM125: g_app.zoom = 12; break;
        case IDM_VIEWZOOM25: g_app.zoom = 25; break;
        case IDM_VIEWZOOM50: g_app.zoom = 50; break;
        case IDM_VIEWZOOM200: g_app.zoom = 200; break;
        case IDM_VIEWZOOM400: g_app.zoom = 400; break;
        case IDM_VIEWZOOM800: g_app.zoom = 800; break;
        case IDM_FILEEXIT: PostMessageW(g_app.main_window, WM_CLOSE, 0, 0); break;
        default: break;
        }
    }
    wsprintfW(msg, L"execute(%u) %s   tool %u, colour 1 #%06X, colour 2 #%06X, font %s %s, zoom %d %%", id, LabelOf(id), g_app.tool,
              (unsigned)(((GetRValue(g_app.color1)) << 16) | (GetGValue(g_app.color1) << 8) | GetBValue(g_app.color1)),
              (unsigned)(((GetRValue(g_app.color2)) << 16) | (GetGValue(g_app.color2) << 8) | GetBValue(g_app.color2)),
              g_app.font_name, g_app.font_size, g_app.zoom);
    Note(msg);
    return S_OK;
}

static HRESULT PaintState(UINT32 id, LeewayUICommandState *st)
{
    static WCHAR zoom_label[16];
    if (IsTool(id) || (id >= 321 && id <= 326)) { st->checked = (g_app.tool == id); return S_OK; }
    switch (id) {
    case ID_RECTSEL: st->checked = g_app.tool == ID_RECTSEL || g_app.tool == ID_FREESEL; break;
    case IDM_EDITCUT: case IDM_EDITCOPY: case IDM_IMAGECROP: case IDM_EDITINVERTSELECTION: case IDM_EDITDELETESELECTION:
        st->enabled = TRUE; break;   /* HasSelection: always true in the demo */
    case ID_OUTLINE: case ID_SHAPEFILL: st->enabled = IsShapeTool(g_app.tool); break;
    case ID_SIZE: st->enabled = g_app.tool != ID_TEXT && g_app.tool != ID_FILL && g_app.tool != ID_COLOR && g_app.tool != ID_ZOOM; break;
    case ID_COLOR1: st->color = g_app.color1; st->checked = g_app.active_well == ID_COLOR1; break;
    case ID_COLOR2: st->color = g_app.color2; st->checked = g_app.active_well == ID_COLOR2; break;
    case ID_FONTFAMILY: st->label = g_app.font_name; break;
    case ID_FONTSIZE: st->label = g_app.font_size; break;
    case IDM_BOLD: st->checked = g_app.bold; break;
    case IDM_ITALIC: st->checked = g_app.italic; break;
    case IDM_UNDERLINE: st->checked = g_app.underline; break;
    case IDM_VERTICAL: st->checked = g_app.vertical; break;
    case ID_BG_OPAQUE: st->checked = g_app.opaque; break;
    case ID_BG_TRANSPARENT: st->checked = !g_app.opaque; break;
    case ID_RULERS: st->checked = g_app.rulers; break;
    case IDM_VIEWSHOWGRID: st->checked = g_app.grid; break;
    case IDM_VIEWSTATUSBAR: st->checked = g_app.statusbar; break;
    case IDM_VIEWFULLSCREEN: st->checked = g_app.fullscreen; break;
    case IDM_VIEWSHOWMINIATURE: st->checked = g_app.thumbnail; break;
    case IDM_VIEWZOOM100: st->enabled = g_app.zoom != 100; break;
    case ID_ZOOMLEVEL: wsprintfW(zoom_label, L"%d %%", g_app.zoom); st->label = zoom_label; break;
    default: break;
    }
    return S_OK;
}

static HRESULT OnExecute(UINT32 id, void *)
{
    HRESULT hr = g_app.paint ? PaintExecute(id) : ViewerExecute(id);
    /* the app owns state: tell the ribbon to re-read it (spec G3) */
    LeewayRibbonInvalidateAll(g_app.ribbon);
    return hr;
}

static HRESULT OnQueryState(UINT32 id, LeewayUICommandState *st, void *)
{
    return g_app.paint ? PaintState(id, st) : ViewerState(id, st);
}

/* Demo icon resolver (G10): slots 1-22 draw as coloured discs so the
 * placeholder path and the resolver path can both be seen; 0 = placeholder. */
static HICON ResolveIcon(UINT32 icon_id, UINT32 size, LeewayUIIconVariant variant, COLORREF fg, COLORREF bg, void *)
{
    if (icon_id == 0 || icon_id > 22 || (icon_id % 3) == 0) return NULL;   /* every third slot stays a placeholder */
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
    HGDIOBJ old = SelectObject(mem, color);
    RECT rc = {0, 0, (LONG)size, (LONG)size};
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(mem, &rc, b);
    DeleteObject(b);
    COLORREF tint = variant == LWUI_ICON_VARIANT_MONO ? fg : RGB(60 + (icon_id * 37) % 160, 110 + (icon_id * 53) % 120, 200);
    HBRUSH t = CreateSolidBrush(tint);
    HPEN p = CreatePen(PS_SOLID, 1, variant == LWUI_ICON_VARIANT_MONO ? fg : RGB(23, 63, 122));
    HGDIOBJ ob = SelectObject(mem, variant == LWUI_ICON_VARIANT_MONO ? GetStockObject(NULL_BRUSH) : t), op = SelectObject(mem, p);
    Ellipse(mem, 2, 2, size - 2, size - 2);
    SelectObject(mem, ob);
    SelectObject(mem, op);
    DeleteObject(t);
    DeleteObject(p);
    SelectObject(mem, mask);
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(mem, &rc, white);
    HGDIOBJ ob2 = SelectObject(mem, GetStockObject(BLACK_BRUSH)), op2 = SelectObject(mem, GetStockObject(BLACK_PEN));
    Ellipse(mem, 2, 2, size - 2, size - 2);
    SelectObject(mem, ob2);
    SelectObject(mem, op2);
    SelectObject(mem, old);
    ICONINFO ii;
    ii.fIcon = TRUE;
    ii.xHotspot = ii.yHotspot = 0;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return icon;
}

static void SetupRibbon(void)
{
    ILeewayRibbon *rb = g_app.ribbon;
    LeewayRibbonSetIconResolver(rb, ResolveIcon, NULL);
    LeewayRibbonSetAccelerators(rb, g_app.accel);
    HRESULT hr;
    if (g_app.paint) {
        for (int i = 0; i < 20; i++) {
            g_p_palette_items[i].id = ID_PAL_FIRST + i;
            g_p_palette_items[i].alt_id = ID_PAL2_FIRST + i;
            g_p_palette_items[i].label = g_p_palette_names[i];
            g_p_palette_items[i].color = g_p_palette[i];
        }
        g_p_palette_items[20].id = IDM_COLORSMODERNPALETTE; g_p_palette_items[20].label = L"Modern palette"; g_p_palette_items[20].color = LWUI_COLOR_NONE;
        g_p_palette_items[21].id = IDM_COLORSOLDPALETTE; g_p_palette_items[21].label = L"Classic palette"; g_p_palette_items[21].color = LWUI_COLOR_NONE;
        LeewayRibbonSetCollection(rb, IDM_EDITPASTE, g_p_paste_menu, 2, 0);
        LeewayRibbonSetCollection(rb, ID_RECTSEL, g_p_select_menu, 6, LWUI_COLLECTION_LAST_CHOICE_PRIMARY);
        LeewayRibbonSetCollection(rb, ID_ROTATE, g_p_rotate_menu, 9, 0);
        LeewayRibbonSetCollection(rb, ID_BRUSHES, g_p_brush_items, 8, 0);
        LeewayRibbonSetCollection(rb, ID_SHAPES, g_p_shape_items, 6, 0);
        LeewayRibbonSetCollection(rb, ID_OUTLINE, g_p_outline_items, 3, 0);
        LeewayRibbonSetCollection(rb, ID_SHAPEFILL, g_p_fill_items, 3, 0);
        LeewayRibbonSetCollection(rb, ID_SIZE, g_p_size_items, 5, 0);
        LeewayRibbonSetCollection(rb, ID_ZOOMLEVEL, g_p_zoom_items, 7, 0);
        LeewayRibbonSetCollection(rb, ID_FONTFAMILY, g_p_font_names, 7, 0);
        LeewayRibbonSetCollection(rb, ID_FONTSIZE, g_p_font_sizes, 14, 0);
        LeewayRibbonSetCollection(rb, ID_PALETTE, g_p_palette_items, 22, LWUI_COLLECTION_COLORS);
        hr = LeewayRibbonSetLayout(rb, &g_paint_layout);
    } else {
        LeewayRibbonSetCollection(rb, ID_ZOOM_GALLERY, g_v_zoom_levels, 12, 0);
        LeewayRibbonSetCollection(rb, IDC_ROT_COUNCW, g_v_rot_left_menu, 1, 0);     /* primary never changes (spec §1.5) */
        LeewayRibbonSetCollection(rb, IDC_ROT_CLOCKW, g_v_rot_right_menu, 1, 0);
        LeewayRibbonSetCollection(rb, IDC_SLIDE_SHOW, g_v_slideshow_menu, 2, 0);
        hr = LeewayRibbonSetLayout(rb, &g_viewer_layout);
    }
    if (FAILED(hr)) {
        WCHAR m[64];
        wsprintfW(m, L"SetLayout failed: 0x%08X", (unsigned)hr);
        Note(m);
    }
}

static LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CREATE: {
        g_app.main_window = hwnd;
        g_app.zoom = 100;
        g_app.fit = TRUE;
        g_app.statusbar = TRUE;
        g_app.has_image = TRUE;
        g_app.image_count = 5;
        g_app.tool = ID_PEN;
        g_app.color1 = RGB(0, 0, 0);
        g_app.color2 = RGB(255, 255, 255);
        g_app.active_well = ID_COLOR1;
        g_app.opaque = TRUE;
        lstrcpyW(g_app.font_name, L"Calibri");
        lstrcpyW(g_app.font_size, L"11");

        g_app.client_area = CreateWindowW(L"STATIC",
                                          g_app.paint ? L"LeewayUI F1: the Paint ribbon. Pick the Text tool for the contextual tab; left/right-click the palette; Alt shows keytips."
                                                      : L"LeewayUI F1: the Photo Viewer ribbon. Alt shows keytips; narrow the window to see groups collapse; Ctrl+F1 minimises.",
                                          WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 0, 0, 10, 10, hwnd, NULL, GetModuleHandleW(NULL), NULL);

        const LeewayUICommand *table = g_app.paint ? g_paint_commands : g_viewer_commands;
        UINT32 count = g_app.paint ? sizeof(g_paint_commands) / sizeof(g_paint_commands[0]) : sizeof(g_viewer_commands) / sizeof(g_viewer_commands[0]);
        if (FAILED(LeewayRibbonCreate(hwnd, table, count, OnExecute, OnQueryState, &g_app, &g_app.ribbon))) {
            Note(L"LeewayRibbonCreate failed");
            return 0;
        }
        SetupRibbon();
        Relayout(hwnd);
        return 0;
    }
    case WM_SIZE:
        Relayout(hwnd);
        return 0;
    case WM_NOTIFY: {
        const NMHDR *nm = (const NMHDR *)lparam;
        if (g_app.ribbon && nm->hwndFrom == LeewayRibbonGetHWND(g_app.ribbon)) {
            if (nm->code == LWUI_RBN_HEIGHTCHANGED) Relayout(hwnd);
            else if (nm->code == LWUI_RBN_QATCHANGED) Note(L"QAT customised and persisted");
        }
        return 0;
    }
    case WM_COMMAND:
        /* accelerators route through the same execute handler as the ribbon */
        if (HIWORD(wparam) == 1) OnExecute(LOWORD(wparam), &g_app);
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_KEYMENU && g_app.ribbon) return 0;   /* Alt handled by the ribbon's keytips */
        break;
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        if (g_app.ribbon) LeewayRibbonOnThemeChanged(g_app.ribbon);
        break;
    case WM_DPICHANGED: {
        if (g_app.ribbon) LeewayRibbonSetDPI(g_app.ribbon, HIWORD(wparam));
        const RECT *rect = (const RECT *)lparam;
        SetWindowPos(hwnd, NULL, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_F6 && g_app.ribbon) SetFocus(LeewayRibbonGetHWND(g_app.ribbon));   /* the ribbon is one Tab stop */
        return 0;
    case WM_DESTROY:
        if (g_app.ribbon) {
            LeewayRibbonDestroy(g_app.ribbon);
            g_app.ribbon = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR cmd_line, int)
{
    g_app.paint = cmd_line && *cmd_line && (wcsstr(cmd_line, L"paint") != NULL || wcsstr(cmd_line, L"Paint") != NULL);

    static const ACCEL viewer_accels[] = {
        {FCONTROL | FVIRTKEY, 'O', ID_OPEN}, {FCONTROL | FVIRTKEY, 'S', IDC_SAVEAS}, {FCONTROL | FVIRTKEY, 'P', IDC_PRINT},
        {FCONTROL | FVIRTKEY, 'E', IDC_MODIFY}, {FCONTROL | FVIRTKEY, 'B', IDC_BEST_FIT}, {FCONTROL | FVIRTKEY, 'A', IDC_REAL_SIZE},
        {FCONTROL | FVIRTKEY, 'K', IDC_ROT_COUNCW}, {FCONTROL | FVIRTKEY, 'L', IDC_ROT_CLOCKW}, {FVIRTKEY, VK_F11, IDC_TOGGLEFULLSCREEN},
        {FVIRTKEY, VK_LEFT, IDC_PREV_PIC}, {FVIRTKEY, VK_RIGHT, IDC_NEXT_PIC}, {FVIRTKEY, VK_DELETE, IDC_DELETE},
        {FVIRTKEY, VK_ADD, IDC_ZOOM_IN}, {FVIRTKEY, VK_SUBTRACT, IDC_ZOOM_OUT}, {FVIRTKEY, VK_F1, IDC_HELP_TOC},
    };
    static const ACCEL paint_accels[] = {
        {FCONTROL | FVIRTKEY, 'N', IDM_FILENEW}, {FCONTROL | FVIRTKEY, 'O', IDM_FILEOPEN}, {FCONTROL | FVIRTKEY, 'S', IDM_FILESAVE},
        {FCONTROL | FVIRTKEY, 'P', IDM_FILEPRINT}, {FCONTROL | FVIRTKEY, 'Z', IDM_EDITUNDO}, {FCONTROL | FVIRTKEY, 'Y', IDM_EDITREDO},
        {FCONTROL | FVIRTKEY, 'X', IDM_EDITCUT}, {FCONTROL | FVIRTKEY, 'C', IDM_EDITCOPY}, {FCONTROL | FVIRTKEY, 'V', IDM_EDITPASTE},
        {FCONTROL | FVIRTKEY, 'A', IDM_EDITSELECTALL}, {FCONTROL | FVIRTKEY, 'E', IDM_IMAGEATTRIBUTES}, {FCONTROL | FVIRTKEY, 'W', IDM_IMAGESTRETCHSKEW},
        {FCONTROL | FVIRTKEY, 'B', IDM_BOLD}, {FCONTROL | FVIRTKEY, 'I', IDM_ITALIC}, {FCONTROL | FVIRTKEY, 'U', IDM_UNDERLINE},
        {FCONTROL | FVIRTKEY, VK_ADD, IDM_CTRL_PLUS}, {FCONTROL | FVIRTKEY, VK_SUBTRACT, IDM_CTRL_MINUS},
    };
    g_app.accel = g_app.paint ? CreateAcceleratorTableW((LPACCEL)paint_accels, sizeof(paint_accels) / sizeof(paint_accels[0]))
                              : CreateAcceleratorTableW((LPACCEL)viewer_accels, sizeof(viewer_accels) / sizeof(viewer_accels[0]));

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = MainWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"LeewayUIRibbonTestWindow";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"LeewayUIRibbonTestWindow",
                                g_app.paint ? L"LeewayUI F1: Paint ribbon" : L"LeewayUI F1: Photo Viewer ribbon",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, g_app.paint ? 1180 : 900, 520,
                                NULL, NULL, instance, NULL);
    if (!hwnd) return 1;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_app.ribbon && LeewayRibbonPreTranslateMessage(g_app.ribbon, &msg)) continue;
        if (g_app.accel && TranslateAcceleratorW(hwnd, g_app.accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_app.accel) DestroyAcceleratorTable(g_app.accel);
    return 0;
}
