# Changelog

Each release needs a `## vX.Y.Z` section here; its text becomes the GitHub Release notes.

## v1.1.0

- New "Enabled" switch card above the settings pages, with the toggle hotkey hint; it stays in sync with the tray menu and hotkey.
- Settings window now opens on first launch.

## v1.0.0

First release of Window Focus Highlighter, a small tray app that flashes a border and/or tint around the window that just got focus, then fades out.

- Three styles: border, tint, or both, with adjustable color, opacity, thickness and soft edge fade.
- Follows Windows corner rounding, handles maximized windows, high DPI and multiple monitors, and tracks the window if it moves.
- Settings window with live preview, light/dark theme support, timing controls, a toggle hotkey (Ctrl+Alt+H) and a per-program ignore list.
- Runs from the tray, can start with Windows, and uses no polling.
- Version number shown in the tray menu and exe properties.
