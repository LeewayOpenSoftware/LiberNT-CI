/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Backend contract for display configuration and wallpaper state
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The Control Panel item (dll/cpl/leewaydisp) talks only to this API; Settings may link the same library later.
 * Every call reports the real result. A capability the stack cannot provide is cleared in LDispCapabilities and
 * is never faked. Implementation: sdk/lib/leewaydisplay (USER/GDI: EnumDisplayDevices, EnumDisplaySettingsEx,
 * ChangeDisplaySettingsEx, SystemParametersInfo).
 */

#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LDISP_MAX_MONITORS 16
#define LDISP_MAX_MODES    512
#define LWALL_MAX_ITEMS    128

typedef struct LDISP_MONITOR
{
    WCHAR Device[32];     /* \\.\DISPLAYn */
    WCHAR Adapter[128];   /* adapter description */
    WCHAR Monitor[128];   /* attached monitor description, may be empty */
    BOOL Attached;
    BOOL Primary;
    RECT Bounds;          /* desktop coordinates, valid when Attached */
} LDISP_MONITOR;

typedef struct LDISP_MODE
{
    DWORD Width;
    DWORD Height;
    DWORD Bits;
    DWORD Frequency;      /* 0 or 1 = hardware default */
    DWORD Orientation;    /* DMDO_* */
} LDISP_MODE;

#define LDISP_CAP_RESOLUTION   0x0001
#define LDISP_CAP_REFRESH      0x0002
#define LDISP_CAP_ORIENTATION  0x0004  /* rotation can be applied: NOT implemented; LDISP_MODE.Orientation is read-only */
#define LDISP_CAP_MULTIMONITOR 0x0008  /* attach and detach: NOT implemented, never set; see LDISP_CAP_MULTIMON_POSITION */
#define LDISP_CAP_PERMON_DPI   0x0010  /* per-monitor scale: NOT supported by the stack yet */
#define LDISP_CAP_MULTIMON_POSITION 0x0040 /* 2+ attached displays and a CDS_TEST of the current layout passes for each */
#define LDISP_CAP_SCREENSAVER  0x0020  /* SPI_*SCREENSAVE* answers on this system */

DWORD LDispCapabilities(void);
int LDispEnumMonitors(LDISP_MONITOR *out, int max);
int LDispEnumModes(const WCHAR *device, LDISP_MODE *out, int max);
BOOL LDispGetCurrent(const WCHAR *device, LDISP_MODE *mode);
/* DISP_CHANGE_* results; DISP_CHANGE_SUCCESSFUL == 0. */
LONG LDispTestMode(const WCHAR *device, const LDISP_MODE *mode);          /* CDS_TEST, changes nothing */
/* Dynamic change only: the mode is NEVER written to the registry here, so a hang, crash or power cut before the
 * user confirms comes back up in the previous mode. Use it to apply and to revert. */
LONG LDispApplyMode(const WCHAR *device, const LDISP_MODE *mode);
/* Persist the mode (CDS_UPDATEREGISTRY) after the user confirmed it. */
LONG LDispCommitMode(const WCHAR *device, const LDISP_MODE *mode);
UINT LDispGetSystemDpi(void);

/* Multi-display position and primary. Modes are NOT changed here (use LDispApplyMode per display) and attach/detach is
 * refused: a layout whose Attached flags differ from the current ones fails with DISP_CHANGE_BADPARAM. The set holds
 * the attached displays only. X/Y are desktop coordinates; exactly one entry has Primary. */
typedef struct LDISP_LAYOUT
{
    WCHAR Device[32];
    LONG X;
    LONG Y;
    BOOL Primary;
} LDISP_LAYOUT;

/* Current arrangement of the attached displays, for showing and for reverting. */
int LDispGetLayout(LDISP_LAYOUT *out, int max);
/* CDS_TEST for every display; nothing changes. */
LONG LDispTestLayout(const LDISP_LAYOUT *layout, int count);
/* Dynamic change only (never written to the registry), primary first. Positions are normalised so the primary is at 0,0.
 * Re-apply the LDispGetLayout snapshot to revert. */
LONG LDispApplyLayout(const LDISP_LAYOUT *layout, int count);
/* Persist the arrangement (CDS_UPDATEREGISTRY | CDS_NORESET, then one commit) after the user confirmed it. */
LONG LDispCommitLayout(const LDISP_LAYOUT *layout, int count);

/* Wallpaper: styles map to the WallpaperStyle/TileWallpaper values. Span is not offered (no multi-monitor span). */
typedef enum LWALL_STYLE
{
    LWALL_FILL,
    LWALL_FIT,
    LWALL_STRETCH,
    LWALL_TILE,
    LWALL_CENTER,
    LWALL_STYLE_COUNT
} LWALL_STYLE;

typedef struct LWALL_ITEM
{
    WCHAR Path[MAX_PATH];
    WCHAR Name[64];       /* file name without extension */
} LWALL_ITEM;

/* Scans %SystemRoot%\Web\Wallpaper (bmp, jpg, jpeg, png), at most max items, sorted by name. */
int LWallEnum(LWALL_ITEM *out, int max);
BOOL LWallGetCurrent(WCHAR *path, DWORD cch, LWALL_STYLE *style);
BOOL LWallSet(const WCHAR *path, LWALL_STYLE style);

/* Screen saver: SCRNSAVE.EXE (read by winlogon) plus SPI_SETSCREENSAVETIMEOUT/ACTIVE. Timeout is in seconds. */
typedef struct LSAVER_ITEM
{
    WCHAR Path[MAX_PATH];
    WCHAR Name[64];       /* file name without extension */
} LSAVER_ITEM;

#define LSAVER_MAX_ITEMS 64

BOOL LSaverIsAvailable(void);
/* Scans %SystemRoot%\System32\*.scr, sorted by name. */
int LSaverEnum(LSAVER_ITEM *out, int max);
/* path is empty when no saver is set. */
BOOL LSaverGet(WCHAR *path, DWORD cch, UINT *timeoutSeconds, BOOL *active);
/* path NULL or empty removes the saver (and turns it off). All values are restored if any step fails. */
BOOL LSaverSet(const WCHAR *path, UINT timeoutSeconds, BOOL active);
/* Starts the saver with /s as a separate process; any key or mouse movement ends it. */
BOOL LSaverTest(const WCHAR *path);

#ifdef __cplusplus
}
#endif
