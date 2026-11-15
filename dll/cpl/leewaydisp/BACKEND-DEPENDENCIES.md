# Leeway Display & Personalization: backend dependencies

Control Panel item `leewaydisp.cpl`; backend `sdk/lib/leewaydisplay` (`leewayui/display_backend.h`).
Each row is a capability the UI does not offer yet because the stack behind it is missing or unverified.
The UI never fakes these; `LDispCapabilities()` leaves the flag cleared.

| Feature | Needs | Owner | State |
|---|---|---|---|
| Resolution / refresh | `ChangeDisplaySettingsEx` (USER/win32k) with CDS_TEST | backend | Implemented, lab check pending |
| Keep/Revert (15 s) | Dynamic apply, registry commit only after Keep | backend | Implemented |
| Crash during the 15 s window | Persisted pending-revert marker read at logon | CPL + shell | Not done; nothing is persisted before Keep, so a crash returns to the old mode |
| Orientation | `DM_DISPLAYORIENTATION` honoured by the display driver | driver | Read-only; `LDISP_CAP_ORIENTATION` is cleared, rotation is not offered |
| Multi-display position and primary | `LDispGetLayout/TestLayout/ApplyLayout/CommitLayout`: per-display CDS_TEST, dynamic apply with CDS_SET_PRIMARY at 0,0, registry only after Keep (CDS_UPDATEREGISTRY \| CDS_NORESET + one commit) | backend | Backend done; `LDISP_CAP_MULTIMON_POSITION` is set only with 2+ attached displays and a passing CDS_TEST. No UI yet; lab check needs a two-head VM, otherwise untestable and unverified |
| Attach / detach a display | Registry attach state or a 0x0 DEVMODE, verified on a driver | backend | Refused (`DISP_CHANGE_BADPARAM`); `LDISP_CAP_MULTIMONITOR` never set |
| Wallpaper Span | Multi-monitor span in the desktop painter | shell | Not offered |
| Per-monitor scale | Per-monitor DPI in win32k and `GetDpiForMonitor` | win32k | Unsupported (`LDISP_CAP_PERMON_DPI`); system DPI is shown |
| Theme / accent colour | UxTheme theme switch + DWM colorization values | uxtheme/dwm | Not done |
| Pointer schemes | `SPI_SETCURSORS` + `HKCU\Control Panel\Cursors` | backend | Not done |
| Screensaver | `SPI_SETSCREENSAVE*` + `SCRNSAVE.EXE` read by winlogon | backend | Implemented (tab shown only when `LDISP_CAP_SCREENSAVER`); lab check pending. No "On resume, password protect" (ScreenSaverIsSecure left untouched), no preview pane |
| Desktop icons | `CLSID` desktop-icon visibility values read by the desktop view | shell | Not done |
| Localized strings, menu verbs | STRINGTABLE for other languages; MUIVerb support in the desktop-background menu | CPL / shell | Strings in en, es, ja, zh-CN; menu verbs are English literals, MUIVerb unverified |

Shared UI helpers: the CPL is plain Win32/commctrl for now. The leewayui DPI and theme helpers come in with the landing redesign.

## Control Panel window (2026-11-15 redesign)

One window (`cpwin.c`): back/forward, clickable breadcrumb, search over the page names, Win7-style task pane, theme
gallery and bottom strip on Personalization, a dedicated Desktop Background page, Window Color, Display and Screen
Resolution pages, Screen Saver as a dialog. Honest states, nothing faked:

| Item | State |
|---|---|
| Theme gallery | Shows the live "Unsaved Theme" only; installed themes come with the `.theme` model |
| Save theme, Get more themes online | Disabled (no theme writer, no online service) |
| Change desktop icons, Change your account picture | Disabled (no backend) |
| Slide show interval, Shuffle, Select all, Clear all | Disabled; the desktop holds one picture |
| Window Color swatches | Page links to the advanced appearance settings; swatches wait for a verified colour backend |
| Taskbar and Start Menu | Launches the shell option page; failure is reported |
