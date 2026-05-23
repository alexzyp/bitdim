# Contributing to Bitdim

Thanks for your interest! Bitdim is intentionally tiny, so the bar for
new code is "it's worth the bytes." That said, real bug fixes and small,
well-scoped features are very welcome.

## Filing an issue

Use the bug-report or feature-request templates in the
[New Issue](https://github.com/alexzyp/bitdim/issues/new/choose) menu.
For bugs please include:

- Windows version (`winver` output)
- Display setup (single / multi-monitor, DPI scale per monitor)
- Reproduction steps
- Expected vs actual behaviour

For feature requests, please describe **the problem you're trying to
solve**, not just the implementation you have in mind — there may be a
simpler way.

## Building

See the [README — Build](README.md#build) section. In one sentence: put
any mingw-w64 toolchain (w64devkit recommended) on `PATH`, then `make`.

## Code style

- ANSI / C99, no GNU extensions.
- 4-space indent, no tabs.
- Pure Win32 — please do not introduce third-party dependencies.
- One source file (`bitdim.c`) is a feature, not an accident. If a
  change pushes the file past ~1500 lines or introduces unrelated
  subsystems, consider whether it belongs in Bitdim at all.
- Match the surrounding naming: `lower_snake_case` functions, `g_`
  prefix for globals, `WM_` / `IDC_` for Win32 constants.
- Keep the binary under ~200 KB. We're proud of being small.

## What is likely to be accepted

- Bug fixes with a clear repro
- Smarter handling of edge-case windows (cloaked, virtual desktops,
  exotic frames)
- Performance work (fewer GDI calls, less CPU during drag/resize)
- Compatibility fixes for older Windows / unusual DPI setups
- Polish to the slider dialog (a11y, keyboard nav, hi-DPI)
- New items from the README Roadmap, especially per-app rules and
  fullscreen detection

## What is unlikely to be accepted

- Telemetry / analytics / "phone home" of any kind
- Auto-update mechanisms — most users will update through their package channel of choice
- Web / cloud features
- Dependencies on heavy frameworks (Qt, .NET, Electron, etc.)
- Major UI redesigns without a discussion issue first
- Sweeping reformat / refactor commits that obscure real changes

## Pull request checklist

- [ ] Builds cleanly with `make` and `make debug`
- [ ] No new compiler warnings (we build with `-Wall -Wextra`)
- [ ] Tested on at least one Windows 10 or 11 machine, manually verified
- [ ] CHANGELOG.md entry under `[Unreleased]`
- [ ] Commit message in present tense ("Add per-monitor toggle", not
      "Added")

## License

By submitting a PR you agree your contribution will be released under
the MIT license that covers the rest of the project.
