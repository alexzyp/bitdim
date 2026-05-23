/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 alexzyp
 *
 * Bitdim - spotlight focus dimmer for Windows.
 *
 * For every monitor we create a click-through, no-activate, top-most
 * layered window painted solid black at the configured alpha. The hole
 * we punch in each overlay is the union of every visible top-level
 * window that belongs to the foreground window's process — so a
 * multi-window app (Chrome, an IDE with side dialogs) stays bright as
 * one cohesive surface. Shell processes (explorer et al.) are degraded
 * to just the foreground window's own rect; see is_shell_process.
 *
 *   Left- / right-click tray icon -> small floating dialog
 *                                    (slider + Enabled checkbox + Exit)
 *   Ctrl + Alt + D                -> global on/off hotkey
 *
 * Pure Win32 / C99, single .exe, no dependencies.
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <commctrl.h>

#include <string.h>
#include <wchar.h>
#include <wctype.h>

/* ---------------------------------------------------------------- constants */

#define APP_NAME      L"Bitdim"
#define APP_MUTEX     L"Bitdim.SingleInstance.v1"
#define OVL_CLASS     L"BitdimOverlayCls"
#define MAIN_CLASS    L"BitdimMainCls"
#define CUSTOM_CLASS  L"BitdimCustomCls"

#define WM_TRAYICON   (WM_USER + 1)
#define HOTKEY_TOGGLE 1
#define TRAY_ID       1

#define MAX_MONITORS  16

#define UPDATE_TIMER_ID    200
#define UPDATE_THROTTLE_MS 16  /* ~60 fps cap on overlay recomputes */

/* Resource IDs (icon + dialog controls). */
#define IDI_MAIN          101
#define IDC_TRACKBAR      300
#define IDC_ENABLE_CHECK  301
#define IDC_EXIT_BTN      302

/* Dialog geometry (logical px @ 96 dpi; OS does per-monitor scaling). */
#define DLG_W   340
#define DLG_H   180
#define DLG_PAD 18

/* Defined inline so we don't depend on the very latest SDK headers. */
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2_VALUE ((HANDLE)-4)

typedef struct {
    HWND hwnd;
    RECT rc;       /* monitor rect in virtual-screen coords */
} overlay_t;

static HINSTANCE  g_hinst = NULL;
static HWND       g_main  = NULL;
static overlay_t  g_ovl[MAX_MONITORS];
static int        g_ovl_count = 0;
static BYTE       g_alpha = 130;          /* 0..255; 130 ~= 50% */
static BOOL       g_enabled = TRUE;
static HBRUSH     g_brush = NULL;
static HWINEVENTHOOK g_hook = NULL;
static NOTIFYICONDATAW g_tray = {0};
static BOOL       g_update_pending  = FALSE;
static DWORD      g_last_update_tick = 0;
static HWND       g_custom_dlg = NULL;
static HWND       g_custom_trackbar = NULL;
static HWND       g_custom_label = NULL;
static HWND       g_custom_caption = NULL;
static HWND       g_custom_check = NULL;
static HWND       g_custom_exit = NULL;
static HFONT      g_dlg_font = NULL;
static HFONT      g_big_font = NULL;
static POINT      g_menu_anchor = {0, 0};

/* ---------------------------------------------------------------- helpers */

static BOOL is_skip_window(HWND fg) {
    if (!fg) return TRUE;
    /* ignore our own overlays / main */
    for (int i = 0; i < g_ovl_count; ++i)
        if (g_ovl[i].hwnd == fg) return TRUE;
    if (fg == g_main) return TRUE;

    wchar_t cls[64] = {0};
    GetClassNameW(fg, cls, 64);
    /* shell surfaces — leave dim alone */
    if (wcscmp(cls, L"Shell_TrayWnd")    == 0) return TRUE;
    if (wcscmp(cls, L"Progman")          == 0) return TRUE;
    if (wcscmp(cls, L"WorkerW")          == 0) return TRUE;
    if (wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) return TRUE;
    return FALSE;
}

/* Returns the rect that visually represents the window on screen.
 *
 * Three cases:
 *  1) Maximized -> use the host monitor's work area. DWM frame bounds for
 *     maximized windows can return the pre-maximize size or an inset content
 *     rect depending on the Windows version / app; rcWork is unambiguous.
 *  2) Normal/snapped -> DWMWA_EXTENDED_FRAME_BOUNDS, which excludes the
 *     invisible resize border that GetWindowRect would include.
 *  3) Fallback -> GetWindowRect. */
static BOOL get_window_rect_visible(HWND h, RECT *out) {
    WINDOWPLACEMENT wp;
    wp.length = sizeof wp;
    if (GetWindowPlacement(h, &wp) && wp.showCmd == SW_SHOWMAXIMIZED) {
        HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        if (mon && GetMonitorInfoW(mon, &mi)) {
            *out = mi.rcWork;
            return TRUE;
        }
    }
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, out, sizeof *out)))
        return TRUE;
    return GetWindowRect(h, out);
}

/* Returns the base image name of pid in lowercase, or empty on failure. */
static BOOL proc_base_name(DWORD pid, wchar_t *out, DWORD cap) {
    out[0] = 0;
    if (!pid) return FALSE;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return FALSE;
    wchar_t path[MAX_PATH];
    DWORD plen = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(p, 0, path, &plen);
    CloseHandle(p);
    if (!ok) return FALSE;
    const wchar_t *base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    DWORD i = 0;
    for (; base[i] && i + 1 < cap; ++i) out[i] = (wchar_t)towlower(base[i]);
    out[i] = 0;
    return TRUE;
}

/* explorer.exe and friends host dozens of unrelated top-level windows
 * (taskbar, every File Explorer window, NotifyIconOverflowWindow, ...).
 * Per-process expansion makes the algorithm light up File Explorer
 * windows even when the user is just opening the tray overflow.
 * For these processes we fall back to "just the fg window itself". */
static BOOL is_shell_process(DWORD pid) {
    wchar_t name[64];
    if (!proc_base_name(pid, name, 64)) return FALSE;
    return (wcscmp(name, L"explorer.exe")                == 0 ||
            wcscmp(name, L"applicationframehost.exe")    == 0 ||
            wcscmp(name, L"shellexperiencehost.exe")     == 0 ||
            wcscmp(name, L"startmenuexperiencehost.exe") == 0 ||
            wcscmp(name, L"searchhost.exe")              == 0 ||
            wcscmp(name, L"searchapp.exe")               == 0 ||
            wcscmp(name, L"systemsettings.exe")          == 0);
}

/* True iff h is a real on-screen window we should keep bright. */
static BOOL is_visible_for_dim(HWND h) {
    if (!h)                  return FALSE;
    if (!IsWindowVisible(h)) return FALSE;
    if (IsIconic(h))         return FALSE;
    BOOL cloaked = FALSE;
    /* DWMWA_CLOAKED catches invisible UWP host windows, virtual-desktop'd
     * windows, and shell-cloaked ones — those should never punch a hole. */
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked)
        return FALSE;
    if (is_skip_window(h))   return FALSE;
    return TRUE;
}

/* ---------------------------------------------------------------- region update */

typedef struct {
    DWORD pid;       /* process whose windows count as "active" */
    HRGN  rgn;       /* accumulator, screen coords */
} fg_enum_ctx_t;

static BOOL CALLBACK fg_enum_proc(HWND h, LPARAM lp) {
    fg_enum_ctx_t *ctx = (fg_enum_ctx_t *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != ctx->pid)        return TRUE;
    if (!is_visible_for_dim(h)) return TRUE;

    RECT r;
    if (!get_window_rect_visible(h, &r)) return TRUE;
    if (r.right <= r.left || r.bottom <= r.top) return TRUE;

    HRGN add = CreateRectRgnIndirect(&r);
    CombineRgn(ctx->rgn, ctx->rgn, add, RGN_OR);
    DeleteObject(add);
    return TRUE;
}

/* Build a screen-space region for the "active app". Normally this is the
 * union of every visible top-level window of the foreground window's process,
 * so a Chrome window with three other Chrome windows stays bright as one
 * coherent app. For shell processes (explorer et al.) we degrade to the
 * single fg window — see is_shell_process for why. */
static HRGN build_active_rgn(HWND fg) {
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    if (!is_visible_for_dim(fg)) return rgn;   /* leave empty -> full dim */
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return rgn;

    if (is_shell_process(pid)) {
        RECT r;
        if (get_window_rect_visible(fg, &r) && r.right > r.left && r.bottom > r.top) {
            HRGN add = CreateRectRgnIndirect(&r);
            CombineRgn(rgn, rgn, add, RGN_OR);
            DeleteObject(add);
        }
        return rgn;
    }

    fg_enum_ctx_t ctx = { pid, rgn };
    EnumWindows(fg_enum_proc, (LPARAM)&ctx);
    return rgn;
}

static void update_overlays_for(HWND fg) {
    HRGN active = build_active_rgn(fg);  /* screen coords; possibly empty */

    for (int i = 0; i < g_ovl_count; ++i) {
        overlay_t *o = &g_ovl[i];
        int w = o->rc.right  - o->rc.left;
        int h = o->rc.bottom - o->rc.top;

        HRGN full = CreateRectRgn(0, 0, w, h);
        /* translate the active rgn into this monitor's window-local coords */
        HRGN local = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(local, active, NULL, RGN_COPY);
        OffsetRgn(local, -o->rc.left, -o->rc.top);
        CombineRgn(full, full, local, RGN_DIFF);
        DeleteObject(local);

        /* SetWindowRgn takes ownership of `full` */
        SetWindowRgn(o->hwnd, full, TRUE);
    }

    DeleteObject(active);
}

static void run_update_now(void) {
    g_last_update_tick = GetTickCount();
    update_overlays_for(GetForegroundWindow());
}

/* Coalesce bursts of events (drag/resize fires hundreds per second) into
 * at most one update every UPDATE_THROTTLE_MS. First event in a quiet
 * period runs immediately; subsequent events arm a one-shot timer. */
static void schedule_update(void) {
    if (g_update_pending) return;
    DWORD now   = GetTickCount();
    DWORD since = now - g_last_update_tick;
    if (since >= UPDATE_THROTTLE_MS) {
        run_update_now();
    } else {
        g_update_pending = TRUE;
        SetTimer(g_main, UPDATE_TIMER_ID, UPDATE_THROTTLE_MS - since, NULL);
    }
}

/* ---------------------------------------------------------------- overlays */

static LRESULT CALLBACK ovl_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc; GetClientRect(h, &rc);
        FillRect((HDC)wp, &rc, g_brush);
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static BOOL CALLBACK mon_enum(HMONITOR mon, HDC dc, LPRECT prc, LPARAM lp) {
    (void)dc; (void)lp;
    if (g_ovl_count >= MAX_MONITORS) return TRUE;

    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);

    overlay_t *o = &g_ovl[g_ovl_count];
    o->rc = mi.rcMonitor;

    DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST
             | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    o->hwnd = CreateWindowExW(
        ex, OVL_CLASS, L"",
        WS_POPUP,
        o->rc.left, o->rc.top,
        o->rc.right - o->rc.left, o->rc.bottom - o->rc.top,
        NULL, NULL, g_hinst, NULL);
    if (!o->hwnd) return TRUE;

    SetLayeredWindowAttributes(o->hwnd, 0, g_alpha, LWA_ALPHA);
    ShowWindow(o->hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(o->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ++g_ovl_count;
    return TRUE;
}

static void overlays_create(void) {
    g_ovl_count = 0;
    EnumDisplayMonitors(NULL, NULL, mon_enum, 0);
    run_update_now();
}

static void overlays_destroy(void) {
    for (int i = 0; i < g_ovl_count; ++i) {
        if (g_ovl[i].hwnd) DestroyWindow(g_ovl[i].hwnd);
    }
    g_ovl_count = 0;
}

static void overlays_set_alpha(BYTE a) {
    g_alpha = a;
    for (int i = 0; i < g_ovl_count; ++i)
        SetLayeredWindowAttributes(g_ovl[i].hwnd, 0, g_alpha, LWA_ALPHA);
}

static void toggle_enabled(void) {
    g_enabled = !g_enabled;
    if (g_enabled) overlays_create();
    else           overlays_destroy();
}

/* ---------------------------------------------------------------- WinEvent hook */

static void CALLBACK win_event(HWINEVENTHOOK hook, DWORD event,
                               HWND h, LONG idObject, LONG idChild,
                               DWORD thread, DWORD time) {
    (void)hook; (void)idChild; (void)thread; (void)time;
    if (!g_enabled) return;
    if (idObject != OBJID_WINDOW) return;
    if (event != EVENT_SYSTEM_FOREGROUND &&
        event != EVENT_SYSTEM_MINIMIZESTART &&
        event != EVENT_SYSTEM_MINIMIZEEND &&
        event != EVENT_OBJECT_LOCATIONCHANGE) {
        return;
    }

    /* For LOCATIONCHANGE only care about windows belonging to the
     * foreground process — most movement events come from other apps
     * (background animations, mouse cursor sub-objects, etc.) and don't
     * affect our region. */
    if (event == EVENT_OBJECT_LOCATIONCHANGE && h) {
        HWND fg = GetForegroundWindow();
        if (fg) {
            DWORD fg_pid = 0, h_pid = 0;
            GetWindowThreadProcessId(fg, &fg_pid);
            GetWindowThreadProcessId(h, &h_pid);
            if (h_pid != fg_pid) return;
        }
    }

    schedule_update();
}

/* ---------------------------------------------------------------- tray */

static void tray_add(void) {
    g_tray.cbSize           = sizeof g_tray;
    g_tray.hWnd             = g_main;
    g_tray.uID              = TRAY_ID;
    g_tray.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAYICON;
    g_tray.hIcon            = LoadIconW(g_hinst, MAKEINTRESOURCEW(IDI_MAIN));
    if (!g_tray.hIcon) g_tray.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcsncpy(g_tray.szTip, APP_NAME L" - Ctrl+Alt+D to toggle",
            sizeof g_tray.szTip / sizeof(wchar_t) - 1);
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}

static void tray_remove(void) { Shell_NotifyIconW(NIM_DELETE, &g_tray); }

/* ---------------------------------------------------------------- custom dim dialog */

static void custom_set_label(int pct) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%d%%", pct);
    SetWindowTextW(g_custom_label, buf);
}

static HFONT make_font(int pt, int weight) {
    HDC dc = GetDC(NULL);
    int h = -MulDiv(pt, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(NULL, dc);
    return CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static LRESULT CALLBACK custom_dlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        if ((HWND)lp == g_custom_label)        SetTextColor(dc, RGB( 30, 110, 220));
        else if ((HWND)lp == g_custom_caption) SetTextColor(dc, RGB( 90,  90,  90));
        else                                   SetTextColor(dc, RGB(110, 110, 110));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_CTLCOLORBTN:
    case WM_ERASEBKGND: {
        HDC dc = (msg == WM_ERASEBKGND) ? (HDC)wp : NULL;
        if (msg == WM_ERASEBKGND) {
            RECT rc; GetClientRect(h, &rc);
            FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
            return 1;
        }
        return 0;
    }
    case WM_HSCROLL:
        if ((HWND)lp == g_custom_trackbar) {
            int pos = (int)SendMessageW(g_custom_trackbar, TBM_GETPOS, 0, 0);
            g_alpha = (BYTE)((pos * 255) / 100);
            if (g_enabled) overlays_set_alpha(g_alpha);
            custom_set_label(pos);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_ENABLE_CHECK && HIWORD(wp) == BN_CLICKED) {
            BOOL want = (SendMessageW(g_custom_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (want != g_enabled) toggle_enabled();
        } else if (LOWORD(wp) == IDC_EXIT_BTN && HIWORD(wp) == BN_CLICKED) {
            DestroyWindow(g_main);   /* quits the whole app */
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == VK_RETURN) { DestroyWindow(h); return 0; }
        break;
    case WM_ACTIVATE:
        /* close when the user clicks outside the dialog */
        if (LOWORD(wp) == WA_INACTIVE) DestroyWindow(h);
        return 0;
    case WM_CLOSE:   DestroyWindow(h); return 0;
    case WM_DESTROY:
        g_custom_dlg = g_custom_trackbar = g_custom_label = g_custom_caption = NULL;
        g_custom_check = g_custom_exit = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void show_custom_dlg(void) {
    if (g_custom_dlg) { SetForegroundWindow(g_custom_dlg); return; }

    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = custom_dlg_proc;
        wc.hInstance     = g_hinst;
        wc.lpszClassName = CUSTOM_CLASS;
        wc.hbrBackground = NULL;   /* we paint background ourselves */
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hIcon         = LoadIconW(g_hinst, MAKEINTRESOURCEW(IDI_MAIN));
        RegisterClassW(&wc);
        registered = TRUE;
    }

    if (!g_dlg_font) g_dlg_font = make_font(10, FW_NORMAL);
    if (!g_big_font) g_big_font = make_font(22, FW_SEMIBOLD);

    /* Anchor: above-left of the right-click point (typically tray = bottom-right).
     * Clamp into the host monitor's work area. */
    int W = DLG_W, H = DLG_H;
    HMONITOR mon = MonitorFromPoint(g_menu_anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);

    int x = g_menu_anchor.x - W + 24;     /* tail of dialog peeks at click */
    int y = g_menu_anchor.y - H - 12;
    if (x < mi.rcWork.left)        x = mi.rcWork.left + 8;
    if (y < mi.rcWork.top)         y = mi.rcWork.top  + 8;
    if (x + W > mi.rcWork.right)   x = mi.rcWork.right  - W - 8;
    if (y + H > mi.rcWork.bottom)  y = mi.rcWork.bottom - H - 8;

    g_custom_dlg = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CUSTOM_CLASS, L"Bitdim",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, W, H,
        NULL, NULL, g_hinst, NULL);
    if (!g_custom_dlg) return;

    /* Win11 rounded corners (no-op on Win10). */
    {
        int corner = 2; /* DWMWCP_ROUND */
        DwmSetWindowAttribute(g_custom_dlg, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                              &corner, sizeof corner);
    }

    /* Caption / label row */
    g_custom_caption = CreateWindowExW(
        0, L"STATIC", L"Dim amount",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        DLG_PAD, DLG_PAD, 140, 28,
        g_custom_dlg, NULL, g_hinst, NULL);
    SendMessageW(g_custom_caption, WM_SETFONT, (WPARAM)g_dlg_font, TRUE);

    /* Big % readout on the right */
    g_custom_label = CreateWindowExW(
        0, L"STATIC", L"50%",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        W - DLG_PAD - 110, DLG_PAD - 6, 110, 36,
        g_custom_dlg, NULL, g_hinst, NULL);
    SendMessageW(g_custom_label, WM_SETFONT, (WPARAM)g_big_font, TRUE);

    /* Trackbar full width */
    g_custom_trackbar = CreateWindowExW(
        0, TRACKBAR_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_BOTTOM | TBS_AUTOTICKS,
        DLG_PAD - 4, DLG_PAD + 44, W - 2 * (DLG_PAD - 4), 36,
        g_custom_dlg, (HMENU)(INT_PTR)IDC_TRACKBAR, g_hinst, NULL);
    SendMessageW(g_custom_trackbar, TBM_SETRANGE,    TRUE, MAKELONG(5, 95));
    SendMessageW(g_custom_trackbar, TBM_SETTICFREQ,  10,  0);
    SendMessageW(g_custom_trackbar, TBM_SETPAGESIZE,  0,  5);
    SendMessageW(g_custom_trackbar, TBM_SETPOS,     TRUE, (g_alpha * 100) / 255);

    custom_set_label((int)SendMessageW(g_custom_trackbar, TBM_GETPOS, 0, 0));

    /* Enable checkbox at the bottom-left — single source of truth for on/off */
    g_custom_check = CreateWindowExW(
        0, L"BUTTON", L"Enabled",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        DLG_PAD, DLG_PAD + 94, 140, 26,
        g_custom_dlg, (HMENU)(INT_PTR)IDC_ENABLE_CHECK, g_hinst, NULL);
    SendMessageW(g_custom_check, WM_SETFONT, (WPARAM)g_dlg_font, TRUE);
    SendMessageW(g_custom_check, BM_SETCHECK, g_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

    /* Exit button at the bottom-right — only path to quit the app now */
    g_custom_exit = CreateWindowExW(
        0, L"BUTTON", L"Exit",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        W - DLG_PAD - 84, DLG_PAD + 92, 84, 28,
        g_custom_dlg, (HMENU)(INT_PTR)IDC_EXIT_BTN, g_hinst, NULL);
    SendMessageW(g_custom_exit, WM_SETFONT, (WPARAM)g_dlg_font, TRUE);

    ShowWindow(g_custom_dlg, SW_SHOWNA);
    SetForegroundWindow(g_custom_dlg);
    SetFocus(g_custom_trackbar);
}

/* ---------------------------------------------------------------- main wnd */

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        if (!RegisterHotKey(h, HOTKEY_TOGGLE, MOD_CONTROL | MOD_ALT, 'D')) {
            MessageBoxW(NULL, L"Could not register Ctrl+Alt+D hotkey.",
                        APP_NAME, MB_ICONWARNING);
        }
        return 0;

    case WM_HOTKEY:
        if (wp == HOTKEY_TOGGLE) toggle_enabled();
        return 0;

    case WM_TIMER:
        if (wp == UPDATE_TIMER_ID) {
            KillTimer(h, UPDATE_TIMER_ID);
            g_update_pending = FALSE;
            run_update_now();
        }
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_RBUTTONUP) {
            GetCursorPos(&g_menu_anchor);
            show_custom_dlg();
        }
        return 0;

    case WM_DISPLAYCHANGE:
        if (g_enabled) { overlays_destroy(); overlays_create(); }
        return 0;

    case WM_DESTROY:
        UnregisterHotKey(h, HOTKEY_TOGGLE);
        if (g_hook) UnhookWinEvent(g_hook);
        overlays_destroy();
        tray_remove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---------------------------------------------------------------- entry */

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)cmdline; (void)show;
    g_hinst = hi;

    /* Make the whole process per-monitor DPI aware so rcMonitor / rcWork /
     * window rects are all in real pixels. Eliminates 1-2px taskbar bleed
     * on scaled displays. Defined via GetProcAddress so we still link with
     * older w64devkit user32 import libs. */
    HMODULE u32 = GetModuleHandleW(L"user32");
    if (u32) {
        typedef BOOL (WINAPI *spdac_t)(HANDLE);
        spdac_t fn = (spdac_t)(void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2_VALUE);
    }

    HANDLE mtx = CreateMutexW(NULL, TRUE, APP_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    g_brush = CreateSolidBrush(RGB(0, 0, 0));

    WNDCLASSW oc = {0};
    oc.lpfnWndProc   = ovl_proc;
    oc.hInstance     = hi;
    oc.lpszClassName = OVL_CLASS;
    oc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&oc);

    WNDCLASSW mc = {0};
    mc.lpfnWndProc   = wnd_proc;
    mc.hInstance     = hi;
    mc.lpszClassName = MAIN_CLASS;
    RegisterClassW(&mc);

    g_main = CreateWindowExW(0, MAIN_CLASS, APP_NAME,
                             0, 0, 0, 0, 0,
                             HWND_MESSAGE, NULL, hi, NULL);
    if (!g_main) return 1;

    tray_add();
    overlays_create();

    g_hook = SetWinEventHook(
        EVENT_MIN, EVENT_MAX,
        NULL, win_event, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_brush) DeleteObject(g_brush);
    if (mtx)     ReleaseMutex(mtx);
    return (int)msg.wParam;
}
