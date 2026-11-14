# Window Focus Highlighter: notes for agents

A tiny Windows tray app (plain C, Win32, ~50 KB exe). When focus moves to another window it flashes a border
and/or tint around it, then fades out. Runs in the background, can start with Windows, has a settings window.

Priorities from the user: small, fast, efficient. Keep it dependency-free C. No polling, no frameworks.

## Layout

| File | Purpose |
| --- | --- |
| `highlighter.c` | The whole app (single translation unit) |
| `app.manifest`, `app.rc` | Common Controls v6, PerMonitorV2 DPI awareness, Win10+ compat; embedded via windres |
| `build.bat` | Build script (MinGW-w64 gcc + windres) |

## Build and run

Needs MinGW-w64 (`gcc` and `windres`) on PATH. From Git Bash:

```bash
taskkill //f //im WindowFocusHighlighter.exe   # the running exe locks the file
windres app.rc -O coff -o app_res.o
gcc -Os -fno-math-errno -municode -mwindows -s -Wall -Wl,--gc-sections highlighter.c app_res.o \
    -o WindowFocusHighlighter.exe -lcomctl32 -lcomdlg32 -lshell32 -luser32 -lgdi32 -ladvapi32
```

Keep the build warning-free (`-Wall`). Add new link libs to `build.bat` too. `*.o` and `*.exe` are gitignored.

There is no test suite. Verify by running the app and looking at it (see Testing).

## Architecture (in file order)

- **Config**: `Config` struct, `LoadConfig` / `SaveConfig` against `%APPDATA%\WindowFocusHighlighter\settings.ini`
  (`[Highlight]` keys: Enabled, Mode, Opacity, TintOpacity, Thickness, Softness, HoldMs, FadeMs, Hotkey, Color (hex RRGGBB),
  Ignore (`;` separated exe names)). `Sanitize` clamps everything. Corner radius is intentionally NOT a setting.
- **Startup**: `HKCU\...\Run` value `WindowFocusHighlighter`.
- **Rendering**: `Render()` fills a premultiplied-ARGB DIB using a rounded-rect signed distance function, anti-aliased.
  Handles border band, interior tint, and the edge-fade (`Softness`) profile. The same function draws the overlay,
  the tray icon, and the settings preview, so keep it general.
- **Overlay**: a layered, topmost, click-through, no-activate window (`WHOverlay`). `GetGeom` decides geometry,
  `PresentOverlay` (re)renders, `ShowHighlight` starts a flash. Timers: `T_HOLD` then `T_FADE` (fade is just
  `UpdateLayeredWindow` constant alpha, no re-render). A `EVENT_OBJECT_LOCATIONCHANGE` hook is installed only while a
  highlight is visible so it follows moved/resized windows.
- **Focus handling**: `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`, 40 ms debounce timer, then `OnFocus` filters
  (own windows, minimized, cloaked, shell classes in `kIgnoredClasses`, user ignore list via `IgnoredProcess`).
- **Tray / hotkey**: tray icon is generated at runtime (no .ico), menu has Enabled / Settings / Start with Windows / Exit,
  global toggle hotkey via `RegisterHotKey` (default Ctrl+Alt+H).
- **Dark mode**: follows the system app theme. Uses undocumented uxtheme ordinals 133/135/136 for dark menus, plus
  `DwmSetWindowAttribute(20)` for the title bar, `SetWindowTheme` on controls, and `WM_CTLCOLOR*` painting.
  Re-themes on `WM_SETTINGCHANGE` ("ImmersiveColorSet"). The stock hotkey control stays white in dark mode (known).
- **Settings dialog** (`WHSettings`, one window, hand-built, no resource templates): custom-painted sidebar with four
  pages (Appearance, Timing, Behavior, Ignore list); a live preview panel (`WHPreview`) on top; custom-drawn sliders
  (`WHSlider`, replaces the stock trackbar, speaks `TBM_*` messages and sends `WM_HSCROLL`); an owner-drawn ignore
  list with hover states and per-row remove. Above the sidebar pages sits an "Enabled" card (switch + toggle hotkey
  hint) painted by `PaintEnabledCard`; its state lives in a hidden `IDC_ENABLE` checkbox so `ReadDialog`/`PutDialog`
  still work, and `SyncEnabledUI` refreshes it when the tray menu or hotkey flips Enabled. Controls are tagged with the page (`ctlPage`) and, on Appearance, a row
  (`ctlRow`), so `ApplyLayout` can show/hide pages and collapse rows that don't apply to the chosen Style.
- **Main window**: hidden top-level window (not message-only, it must receive `TaskbarCreated`). Single instance via
  a named mutex; a second launch posts `WM_OPEN` to the first, which opens the settings window.

## Behaviors worth preserving

- Windowed windows: border is drawn OUTSIDE the window edge (overlay grows by `thickness`), overlapping the window's own
  1 px DWM edge by ~2 px (DPI scaled) so no sliver shows. Maximized/full-screen windows: border stays INSIDE, square.
- Corner rounding follows Windows: 8 px (DPI scaled) on Windows 11 build 22000+, 0 on Windows 10 and for maximized.
- Edge fade fades outward for outside borders and inward for inside borders.
- Tint-only style has no outside band; Border-only has no tint. The tint opacity and border opacity are separate.
- Settings apply live. `LiveApply` updates `cfg` and the preview immediately; disk/registry/hotkey writes are
  debounced 250 ms (`FlushSave`) and flushed on close. `g_loading` guards against feedback while `PutDialog` fills controls.
- Efficiency: no polling anywhere. Timers only run while a highlight is visible, the Timing-page preview is animating,
  or a save is pending. Bitmaps are freed when the highlight hides.

## Testing tips

- Start the exe, then start it a second time: the second instance opens the settings window in the first.
- To keep a highlight on screen long enough to look at, temporarily set `HoldMs=6000` in the ini (restore it after,
  and restore any other test values; the user may also be tweaking settings while you work, so don't assume odd ini
  values are a bug).
- Screenshots: PowerShell with `SetProcessDPIAware()`, `GetWindowRect`, then `Graphics.CopyFromScreen` and view the PNG.
  Page-switching and hover tests can use `SetCursorPos` + `mouse_event`. A full-screen game on the user's machine can
  cover the dialog; if the capture is wrong, retry after relaunching.
- For rendering math, compile `Render` into a small standalone harness that writes a PPM/PNG instead of using the UI.
- Quote carefully when writing Windows paths or `\` strings via shell heredocs; writing patch scripts to files first is
  more reliable.

## Website screenshots

`docs/img/{appearance,timing,behavior}.png` (dark) and `*-light.png` are generated by `tools\screenshots.bat` (rebuilds, then
runs `tools/screenshots.ps1`, which sets default settings and the Windows theme, captures, and restores both). Run it
from cmd or PowerShell, not Git Bash. If a sidebar layout change breaks the clicks, edit the `$pages` offsets in the script.

When preparing a new version release (see `release.sh`): if the UI changed significantly since the last release, run the
script and commit the images as part of the release. If the changes were minor, list what looks different (new controls,
moved pages, restyled elements) and ask the user whether to update the screenshots. Do not silently skip this check.

## Workflow preferences (from the user)

- After each change: rebuild, restart the app, and leave the settings window open (launch the exe twice).
- Commit each completed change (small commits). Git warns about LF to CRLF conversion; that is harmless.
- When the user asks for visual redesigns, show a few options (mockups) and let them choose before building.
