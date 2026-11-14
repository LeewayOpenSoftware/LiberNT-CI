# Scoping: theme, pointer and screensaver pages

Facts come from reading the tree at this branch. Nothing here is lab-verified; every page stays hidden or
disabled until its backend call is checked on an image. No page claims more than the row says.

## Screensaver (smallest, do first)
- Stack: `SPI_SETSCREENSAVEACTIVE`, `SPI_SETSCREENSAVETIMEOUT` exist in `win32ss/user/ntuser/sysparams.c`; the
  saver is `HKCU\Control Panel\Desktop\SCRNSAVE.EXE`. `desk.cpl` already has a saver page (reference only, GPL
  ReactOS code; ours is written fresh).
- Backend to add: `LSaverEnum` (scan `%SystemRoot%\System32\*.scr`), `LSaverGet/Set` (SCRNSAVE.EXE + timeout +
  active), `LSaverPreview` (`/p` hwnd) and `LSaverTest` (`/s`). Test launches a process: needs a close path.
- Not claimed: password-on-resume (needs the lock stack), power timers.

## Pointers
- Stack: `SPI_SETCURSORS` is handled in win32k (reloads the system cursors from the registry).
  `HKCU\Control Panel\Cursors` holds one file per role plus the scheme name.
- Backend to add: `LCursorGetScheme/SetScheme` (write the role values, then `SPI_SETCURSORS`), scheme list from
  `HKCU\Control Panel\Cursors\Schemes`, and our own `.cur` set (own art only).
- Not claimed: pointer size/colour scaling (no per-DPI cursor path), pointer trails/shadow until checked.

## Theme and accent colour (largest, last)
- Stack: `uxtheme` exports `OpenThemeFile`, `ApplyTheme`, `SetSystemVisualStyle`; `dwmapi` has
  `DwmGetColorizationColor` but `DwmpSetColorizationColor` is `E_NOTIMPL`. So a theme switch is possible,
  an accent change through DWM is not.
- Backend to add: `LThemeEnum` (scan `%SystemRoot%\Resources\Themes`), `LThemeApply`.
  Accent colour: offer only if the DWM or a documented registry value actually changes the frame in the lab;
  otherwise the capability flag stays cleared.
- Owner rule: never flip the owner OS theme while testing; theme lab checks run only inside the VM.

## Order and gating
1. Screensaver page. 2. Pointer page. 3. Theme page, accent only if proven.
Each step: backend call, capability flag, page enabled by the flag, lab check by pam-mu3j9l86, then bundle.
Needs `LDISP_CAP_*`-style flags per area (`LTHEME_CAP_*` etc.) so the UI never offers what is not there.
