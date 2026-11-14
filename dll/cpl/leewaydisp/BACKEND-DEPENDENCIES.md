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
| Multi-display layout, primary display, attach/detach | `ApplyLayout`: positions (`DM_POSITION`) + `CDS_NORESET` per device + one commit; `CDS_SET_PRIMARY` | Toby | Not done (`LDISP_CAP_MULTIMONITOR`) |
| Wallpaper Span | Multi-monitor span in the desktop painter | shell | Not offered |
| Per-monitor scale | Per-monitor DPI in win32k and `GetDpiForMonitor` | win32k | Unsupported (`LDISP_CAP_PERMON_DPI`); system DPI is shown |
| Theme / accent colour | UxTheme theme switch + DWM colorization values | uxtheme/dwm | Not done |
| Pointer schemes | `SPI_SETCURSORS` + `HKCU\Control Panel\Cursors` | backend | Not done |
| Screensaver | `SPI_SETSCREENSAVE*` + `SCRNSAVE.EXE` read by winlogon | backend | Implemented (tab shown only when `LDISP_CAP_SCREENSAVER`); lab check pending. No "On resume, password protect" (ScreenSaverIsSecure left untouched), no preview pane |
| Desktop icons | `CLSID` desktop-icon visibility values read by the desktop view | shell | Not done |
| Localized strings, menu verbs | STRINGTABLE for other languages; MUIVerb support in the desktop-background menu | CPL / shell | Strings in en, es, ja, zh-CN; menu verbs are English literals, MUIVerb unverified |

Shared UI helpers: the CPL is plain Win32/commctrl for now. The leewayui DPI and theme helpers come in with the landing redesign.
