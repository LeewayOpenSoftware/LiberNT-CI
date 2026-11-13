#pragma once

/* Icons */
#define IDI_APP_ICON 1
#define IDI_BMP_ICON 2
#define IDI_GIF_ICON 3
#define IDI_JPG_ICON 4
#define IDI_PNG_ICON 5

/* Cursors */
#define IDC_HANDDRAG 100

/* Control IDs */
#define IDC_TOOLBAR     40
#define IDC_STATUSBAR   41
#define IDC_ZOOMSLIDER  42

/* Toolbar images */
#define IDB_PREV_PIC    50
#define IDB_NEXT_PIC    51
#define IDB_BEST_FIT    52
#define IDB_REAL_SIZE   53
#define IDB_SLIDE_SHOW  54
#define IDB_ZOOM_IN     55
#define IDB_ZOOM_OUT    56
#define IDB_ROT_CLOCKW  57
#define IDB_ROT_COUNCW  58
#define IDB_ROT_CWSAVE  59
#define IDB_ROT_CCWSAVE 60
#define IDB_DELETE      61
#define IDB_PRINT       62
#define IDB_SAVEAS      63
#define IDB_MODIFY      64
#define IDB_HELP_TOC    65

/* ToolBar buttons */
#define IDC_TOOL_BASE   500
#define IDC_ACCEL_BASE (IDC_TOOL_BASE + 100) /* Accelerator commands not on the toolbar */

#define IDC_TOGGLEFULLSCREEN (IDC_ACCEL_BASE + 0)
#define IDC_INCTIMER    (IDC_ACCEL_BASE + 1)
#define IDC_DECTIMER    (IDC_ACCEL_BASE + 2)

#define IDC_PREV_PIC    (IDC_TOOL_BASE + 0)
#define IDC_NEXT_PIC    (IDC_TOOL_BASE + 1)
#define IDC_BEST_FIT    (IDC_TOOL_BASE + 2)
#define IDC_REAL_SIZE   (IDC_TOOL_BASE + 3)
#define IDC_SLIDE_SHOW  (IDC_TOOL_BASE + 4)
#define IDC_ZOOM_IN     (IDC_TOOL_BASE + 5)
#define IDC_ZOOM_OUT    (IDC_TOOL_BASE + 6)
#define IDC_ROT_CLOCKW  (IDC_TOOL_BASE + 7)
#define IDC_ROT_COUNCW  (IDC_TOOL_BASE + 8)
#define IDC_ROT_CWSAVE  (IDC_TOOL_BASE + 9)
#define IDC_ROT_CCWSAVE (IDC_TOOL_BASE + 10)
#define IDC_DELETE      (IDC_TOOL_BASE + 11)
#define IDC_PRINT       (IDC_TOOL_BASE + 12)
#define IDC_SAVEAS      (IDC_TOOL_BASE + 13)
#define IDC_MODIFY      (IDC_TOOL_BASE + 14)
#define IDC_HELP_TOC    (IDC_TOOL_BASE + 15)
#define IDC_ENDSLIDESHOW (IDC_TOOL_BASE + 16)

#define TBICON_PREV_PIC     0
#define TBICON_NEXT_PIC     1
#define TBICON_BEST_FIT     2
#define TBICON_REAL_SIZE    3
#define TBICON_SLIDE_SHOW   4
#define TBICON_ZOOM_IN      5
#define TBICON_ZOOM_OUT     6
#define TBICON_ROT_CLOCKW   7
#define TBICON_ROT_COUNCW   8
#define TBICON_ROT_CWSAVE   9
#define TBICON_ROT_CCWSAVE  10
#define TBICON_DELETE       11
#define TBICON_PRINT        12
#define TBICON_SAVEAS       13
#define TBICON_MODIFY       14
#define TBICON_HELP_TOC     15

/* Strings */
#define IDS_APPTITLE    100
#define IDS_SETASDESKBG 101
#define IDS_NOPREVIEW   102
#define IDS_PREVIEW     550
#define IDS_OPEN_ALLPICTURES 103

/* Friendly File Type Names */
#define IDS_EMF_FILE    301
#define IDS_GIF_FILE    302
#define IDS_JPG_FILE    303
#define IDS_BMP_FILE    304
#define IDS_PNG_FILE    305
#define IDS_TIF_FILE    306
#define IDS_WMF_FILE    307

/* Tooltips strings */
#define IDS_TOOLTIP_PREV_PIC   400
#define IDS_TOOLTIP_NEXT_PIC   401
#define IDS_TOOLTIP_BEST_FIT   402
#define IDS_TOOLTIP_REAL_SIZE  403
#define IDS_TOOLTIP_SLIDE_SHOW 404
#define IDS_TOOLTIP_ZOOM_IN    405
#define IDS_TOOLTIP_ZOOM_OUT   406
#define IDS_TOOLTIP_ROT_CLOCKW 407
#define IDS_TOOLTIP_ROT_COUNCW 408
#define IDS_TOOLTIP_DELETE     409
#define IDS_TOOLTIP_PRINT      410
#define IDS_TOOLTIP_SAVEAS     411
#define IDS_TOOLTIP_MODIFY     412
#define IDS_TOOLTIP_HELP_TOC   413
#define IDS_TOOLTIP_ROT_CWSAVE 414
#define IDS_TOOLTIP_ROT_CCWSAVE 415

/* Keyboard accelerators */
#define IDR_ACCELERATOR 600

/* Pill chrome: menu strip, commands and strings */
#define IDC_MENU_COPY      640
#define IDC_MENU_COPYPATH  641
#define IDC_MENU_EMAIL     642
#define IDC_MENU_BURN      643
#define IDC_MENU_OPENWITH  644
#define IDC_MENU_PROPS     645

/* Pill glyphs (Aureline), embedded */
#define IDI_PILL_ZOOM      200
#define IDI_PILL_FIT       201
#define IDI_PILL_PREV      202
#define IDI_PILL_PLAY      203
#define IDI_PILL_NEXT      204
#define IDI_PILL_ROT_CCW   205
#define IDI_PILL_ROT_CW    206
#define IDI_PILL_DELETE    207
#define IDI_PILL_ACTUAL    208

#define IDS_MENU_FILE      560
#define IDS_MENU_PRINT     561
#define IDS_MENU_EMAIL     562
#define IDS_MENU_BURN      563
#define IDS_MENU_OPEN      564
#define IDS_ITEM_OPEN      565
#define IDS_ITEM_SAVEAS    566
#define IDS_ITEM_COPY      567
#define IDS_ITEM_COPYPATH  568
#define IDS_ITEM_DELETE    569
#define IDS_ITEM_WALLPAPER 570
#define IDS_ITEM_PROPS     571
#define IDS_ITEM_EXIT      572
#define IDS_ITEM_PRINT     573
#define IDS_ITEM_BURN      574
#define IDS_ITEM_EDIT      575
#define IDS_ITEM_OPENWITH  576
#define IDS_TOOLTIP_DISPLAYSIZE 577
#define IDS_TOOLTIP_FIT_TOGGLE  578
#define IDS_EMAIL_FAILED   579
#define IDS_BURN_FAILED    580
#define IDS_ROTATE_LOSSY   581
#define IDS_DELETE_RECYCLE   582
#define IDS_DELETE_PERMANENT 583
