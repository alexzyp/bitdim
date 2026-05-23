# Bitdim

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/alexzyp/bitdim?include_prereleases&sort=semver)](https://github.com/alexzyp/bitdim/releases)
[![Downloads](https://img.shields.io/github/downloads/alexzyp/bitdim/total.svg)](https://github.com/alexzyp/bitdim/releases)
[![Platform: Windows 7+](https://img.shields.io/badge/Windows-7%20%7C%208%20%7C%2010%20%7C%2011-0078D6.svg)](#compatibility)

A "spotlight" focus dimmer for Windows. Whatever app is in front stays
bright; everything else is dimmed. **~120 KB single .exe, pure C, no
install.**

Think of it as a vignette around your active app — close all the cosmetic
windows of 2025 and just *see* the one you're using.

## Usage

```cmd
bitdim.exe
```

- A tray icon appears near the clock.
- **Left- or right-click** the tray icon → opens a small floating dialog
  near the cursor:
  - Slider sets dim amount (5 %–95 %)
  - `☑ Enabled` toggles dim on/off
  - `Exit` quits the app
- **`Ctrl + Alt + D`** = global hotkey for quick on/off without the dialog.
- Click anywhere outside the dialog (or press `Esc`) to dismiss it; the
  dim state is kept.
- Multi-monitor: each display gets its own overlay; the active region is
  cut out wherever it falls. `WM_DISPLAYCHANGE` rebuilds overlays when
  monitors are plugged or unplugged.

## Build

Put any mingw-w64 toolchain on `PATH` — easiest option is
[w64devkit](https://github.com/skeeto/w64devkit) (32-bit or 64-bit).
Then from a `cmd` prompt in the repo root:

```cmd
make            :: release -> bitdim.exe
make debug      :: bitdim_debug.exe (-g -O0)
make clean
```

If `gcc.exe`, `windres.exe`, and `make.exe` are not yet on `PATH`,
prepend the toolchain's `bin\` first, for example:

```cmd
set PATH=C:\path\to\w64devkit\bin;%PATH%
```

The Makefile is POSIX-style and forces `SHELL = cmd.exe`, so it never
invokes `sh`. The project has no third-party dependencies — only the
Win32 SDK headers shipped by w64devkit.

## How it works

For each monitor a click-through layered top-most window is created
(`WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`)
and painted solid black with `SetLayeredWindowAttributes(LWA_ALPHA)`.
To leave the active app visible we call `SetWindowRgn` with
`(monitor_rect MINUS active_region)`, literally punching a hole through
the dim layer.

`active_region` is built as follows:

- The foreground window's owning process is identified.
- **Regular apps**: region = union of every visible top-level window that
  belongs to that process. Three Chrome windows therefore all stay bright
  as one app, and modal dialogs stay bright together with their owners.
- **Shell processes** (`explorer.exe`, `ApplicationFrameHost.exe`,
  `ShellExperienceHost.exe`, `StartMenuExperienceHost.exe`,
  `SearchHost.exe`, `SearchApp.exe`, `SystemSettings.exe`): region falls
  back to the foreground window's own rect. Otherwise opening the tray
  overflow would light up unrelated File Explorer windows.
- **Maximized windows**: the rect is taken from the host monitor's
  `rcWork`, because `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)`
  can return the pre-maximize bounds on some Windows / app combinations.
- **Normal / snapped**: DWM extended frame bounds (excludes the invisible
  Win10/11 resize border).
- Hidden / minimized / DWM-cloaked / shell-surface (`Shell_TrayWnd`,
  `Progman`, `WorkerW`, `Shell_SecondaryTrayWnd`) windows are filtered
  out and never contribute to the region.

A `SetWinEventHook(WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)`
listening for `EVENT_SYSTEM_FOREGROUND`,
`EVENT_SYSTEM_MINIMIZE{START,END}` and `EVENT_OBJECT_LOCATIONCHANGE`
schedules a region rebuild whenever the user switches / moves / resizes
windows. Updates are coalesced to ~60 fps with a single one-shot
`WM_TIMER` so a 1000 Hz polling mouse can't flood the GDI path while a
window is being dragged.

## Compatibility

- 32-bit exe (i686 mingw-w64) → Windows **7 / 8 / 8.1 / 10 / 11**, both
  32-bit and 64-bit (the latter via WoW64).
- Manifest declares `PerMonitorV2` DPI awareness and Common-Controls 6.0
  visual styles, so the dialog renders in the modern Win10/11 theme.
- Win11-only nice-to-have: rounded dialog corners via
  `DWMWA_WINDOW_CORNER_PREFERENCE` (silent no-op on older Windows).

## Roadmap

Ideas welcome — open an issue or send a PR.

- [ ] Per-app rules: never dim Discord / always dim Slack.
- [ ] Fullscreen detection: auto-disable for games / video.
- [ ] Smooth alpha animation on focus change.
- [ ] Custom dim colors (warm sepia, blueish night).
- [ ] Dim only inactive *monitors* (not just inactive windows).
- [ ] Persist alpha + enabled state across restarts.
- [ ] Configurable hotkey (Ctrl+Alt+D collides with some IDEs).
- [ ] Localization (currently English-only strings).

## Known limitations

- `Ctrl + Alt + D` may collide with an app that has bound it (some IDEs
  use this combo for "duplicate line"). Quick workaround: turn dim off
  with the tray dialog before launching that app.
- Fullscreen apps still get an overlay on top — toggle dim off before
  launching games / fullscreen video.
- Some custom-chrome apps (WSL graphical apps, certain Electron variants)
  report exotic frame bounds and may produce a hole that's a couple of
  pixels off the visible window edge.

## File map

| File              | Purpose                                                |
| ---               | ---                                                    |
| `bitdim.c`        | All program logic.                                     |
| `bitdim.rc`       | Resource script (icon, manifest, VERSIONINFO).         |
| `bitdim.manifest` | Visual styles, DPI awareness, supportedOS.             |
| `bitdim.ico`      | Tray / window / shortcut icon (multi-resolution).     |
| `Makefile`        | POSIX-style, forces `SHELL = cmd.exe`.                 |
| `CHANGELOG.md`    | Per-release human-readable changes.                    |
| `CONTRIBUTING.md` | How to file issues and send pull requests.             |
| `LICENSE`         | MIT.                                                   |

## Contributing

PRs and issues welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) for the
short version (build, code style, what kinds of changes are likely to be
accepted).

## License

MIT — see [LICENSE](LICENSE). You can do almost anything with this code
as long as the copyright notice ships with it.
