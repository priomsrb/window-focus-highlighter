# Features

Window Highlighter is a small Windows tray app that briefly highlights whichever window just received focus.

## Highlighting

- Flashes a highlight around the newly focused window, holds it briefly, then fades out.
- Three styles: **Border**, **Tint** (a translucent wash over the window), or **Border + tint**.
- The border is drawn just outside the edge of normal windows so it never covers window content.
  For maximized and full-screen windows it is drawn inside, with square corners.
- Follows Windows' own corner rounding (rounded on Windows 11, square on Windows 10 and when maximized).
- Soft glow edge: the border can fade out across its thickness.
- The highlight follows the window if you move or resize it while it is showing.
- Ignores things that shouldn't be highlighted: the taskbar, Alt-Tab, the Start menu, the desktop,
  minimized windows and cloaked (hidden) windows.
- Anti-aliased rendering, correct on high-DPI and multi-monitor setups.

## Settings

Open from the tray icon (double-click, or right-click then Settings). All changes apply immediately.

- **Appearance**
  - Style (border, tint, border + tint)
  - Color (color picker)
  - Border opacity, tint opacity
  - Border thickness
  - Edge fade
  - Controls that don't apply to the chosen style are hidden.
- **Timing**: how long the highlight shows and how long it takes to fade out. The preview animates to match.
- **Behavior**: turn highlighting on or off, start with Windows, and a global toggle hotkey (default Ctrl+Alt+H).
- **Ignore list**: programs that are never highlighted.
  - Add a program with a file picker.
  - Add from a menu of currently open programs.
  - Remove with the × on each row (or the Delete key).
- **Live preview**: a mock window at the top of the settings shows exactly how the highlight will look.
  Click it to flash the real highlight on the settings window.
- Follows the system light/dark theme, including the title bar and tray menu.
- Settings are stored in `%APPDATA%\WindowHighlighter\settings.ini`.

## Background app

- Lives in the system tray; no main window.
- Tray menu: Enabled, Settings, Start with Windows, Exit.
- Optional start with Windows (per-user, no admin needed).
- Single instance: launching the app again opens the settings window of the running copy.
- Global hotkey to pause and resume highlighting. The tray tooltip shows when it is paused.
- Restores its tray icon if Explorer restarts.

## Small and efficient

- One native executable, about 40 KB, written in plain C with the Win32 API. No runtime or installer needed.
- Event driven: it waits for Windows to report a focus change instead of polling.
- Timers and window-movement tracking only run while a highlight is visible.
- Memory for the highlight is released as soon as it disappears.
