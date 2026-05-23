# Changelog

All notable changes to Bitdim are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

_Nothing yet._

## [0.1.0] — 2026-05-23

First public release.

### Added
- Per-monitor click-through layered overlay that dims everything except
  the active app.
- Per-process window union: multi-window apps (Chrome with three
  windows, IDE with side dialogs, …) stay bright together.
- Shell-process blacklist (`explorer.exe`, `ApplicationFrameHost.exe`,
  `ShellExperienceHost.exe`, `StartMenuExperienceHost.exe`,
  `SearchHost.exe`, `SearchApp.exe`, `SystemSettings.exe`) so clicking
  the tray overflow does not light up unrelated File Explorer windows.
- Maximized-window `rcWork` fallback so the bright region matches the
  visible window edge instead of leaking past the taskbar.
- PerMonitorV2 DPI awareness via both Win32 manifest and runtime
  `SetProcessDpiAwarenessContext`.
- Common-Controls 6 manifest for modern Win10/11 visual styles.
- 60 fps update throttle so a 1000 Hz polling mouse does not flood the
  GDI path during drag / resize.
- Win11 rounded dialog corners via `DWMWA_WINDOW_CORNER_PREFERENCE`
  (no-op on older Windows).
- Single floating slider dialog (dim %, Enabled, Exit) opened by either
  left- or right-click on the tray icon, anchored above the cursor.
- Global hotkey `Ctrl + Alt + D` for quick on/off without the dialog.
- Single-instance lock via named mutex.
- Multi-resolution `.ico` tray / Alt-Tab / Start-menu icon.
- Windows VERSIONINFO resource (FileVersion / ProductVersion /
  Description / Copyright) read by File Properties → Details and by
  standard packaging / signing tools.

### Build
- POSIX-style Makefile that forces `SHELL = cmd.exe` and works with
  both 32-bit and 64-bit w64devkit toolchains out of the box.
- `bitdim.exe` is 32-bit, ~120 KB, statically linked, no third-party
  dependencies.

[Unreleased]: https://github.com/alexzyp/bitdim/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/alexzyp/bitdim/releases/tag/v0.1.0
