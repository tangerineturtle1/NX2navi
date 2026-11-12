// The Win32 back end of the window (gfx_wsys.h): the window and its message loop on a thread of their own, F11
// fullscreen, the settings NX2NAVI_WINDOW_SIZE, NX2NAVI_FULLSCREEN and NX2NAVI_MONITOR, the monitor's refresh rate, focus, the Vulkan surface.
// The swapchain and the present are gfx_present.c's. The in-game menu's keyboard and mouse come from the window
// procedure (ui/menu_win.cpp), and so do the game's (input_win.c).
//
// The process is per-monitor DPI aware (nx2navi.manifest): every size here is physical pixels, NX2NAVI_WINDOW_SIZE included,
// and the swapchain matches the screen pixel for pixel. A window moved to a monitor at another display scale keeps its
// client size (WM_GETDPISCALEDSIZE); the menu takes its text size floor from the window's DPI (gfx_window_dpi).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <imm.h>

#include "cfg.h"
#include "flight.h"
#include "fs.h"
#include "gfx.h"
#include "gfx_wsys.h"
#include "input.h"
#include "input_win.h"
#include "nvn_api.h"
#include "ui/menu.h"
#include "vk.h"
#include "vk_dlssg.h"
#include "platform/platform.h"
#include <vulkan/vulkan_win32.h>

// ---- window state
static HWND g_hwnd;
static PlatEvent g_wnd_ready;   // manual-reset: the window thread made the window or gave up
static int g_wnd_failed;
static int g_background;                   // NX2NAVI_BACKGROUND_WINDOW: the window stays behind the user's windows
static int g_fs_pending;                   // window thread: NX2NAVI_FULLSCREEN did not apply before the first show; retried at the first activation
static volatile int32_t g_dpi = USER_DEFAULT_SCREEN_DPI;   // the window's DPI, set by the window thread

HWND gfx_window_handle(void) { return g_hwnd; }

int gfx_window_vk_extensions(const char **out, int max) {
    if (max < 2) return 0;
    out[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    out[1] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    return 2;
}

int gfx_window_vk_presentable(VkInstance inst, VkPhysicalDevice pd, uint32_t family) {
    (void)inst;
    return vkGetPhysicalDeviceWin32PresentationSupportKHR(pd, family) != VK_FALSE;
}

// ---- window thread
#define WND_STYLE WS_OVERLAPPEDWINDOW
#define WM_APP_SETTINGS (WM_APP + 1)
#define WM_APP_FULLSCREEN (WM_APP + 2)   // the fullscreen setting, retried at the first activation
#define WM_APP_TEXT_INPUT (WM_APP + 3)   // the menu's text field takes text (wParam 1) or no longer (0)
#define WM_APP_PASTE (WM_APP + 4)        // the menu's Paste: the clipboard's text, read on the window thread

static WINDOWPLACEMENT g_saved_place = {sizeof g_saved_place};   // the window behind fullscreen (window thread)

static int is_fullscreen(HWND h) { return !(GetWindowLongW(h, GWL_STYLE) & WS_OVERLAPPEDWINDOW); }

#define cfg_fullscreen gfx_cfg_fullscreen
#define cfg_window_size gfx_cfg_window_size

unsigned gfx_window_dpi(void) { return (unsigned)plat_cas32(&g_dpi, 0, 0); }

// The outer size of a framed window whose client area is cw x ch, with the frame measured at dpi.
static void outer_size_at(int cw, int ch, UINT dpi, int *ow, int *oh) {
    RECT r = {0, 0, cw, ch};
    AdjustWindowRectExForDpi(&r, WND_STYLE, FALSE, 0, dpi);
    *ow = r.right - r.left;
    *oh = r.bottom - r.top;
}

// The same at the window's DPI, or the system DPI before the window exists: plain AdjustWindowRect always takes the
// system DPI, which is wrong on a monitor at another display scale.
static void outer_size(HWND h, int cw, int ch, int *ow, int *oh) {
    outer_size_at(cw, ch, h ? GetDpiForWindow(h) : GetDpiForSystem(), ow, oh);
}

// The client size cw x ch scaled down, in its own proportions, so the whole window fits a work area: the chosen
// monitor's (work, with the frame measured at dpi) or, with work NULL, the primary monitor's, where CW_USEDEFAULT opens
// it. The default 1920x1080 on a 1080p laptop put the bottom of the picture (the HUD) under the taskbar and past the
// screen. The setting itself stays; the menu's size list offers what fits.
static void fit_work_area(int *cw, int *ch, const RECT *work, UINT dpi) {
    RECT prim;
    int ow, oh;
    if (!work) {
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &prim, 0)) return;
        work = &prim;
    }
    outer_size_at(*cw, *ch, dpi, &ow, &oh);
    int aw = (work->right - work->left) - (ow - *cw), ah = (work->bottom - work->top) - (oh - *ch);
    if (aw <= 0 || ah <= 0 || (*cw <= aw && *ch <= ah)) return;
    double k = (double)aw / *cw < (double)ah / *ch ? (double)aw / *cw : (double)ah / *ch;
    int w = (int)(*cw * k) & ~1, hh = (int)(*ch * k) & ~1;
    if (w < 640 || hh < 360) return;
    rt_log("gfx: window %dx%d does not fit the screen's work area; opening at %dx%d", *cw, *ch, w, hh);
    *cw = w;
    *ch = hh;
}

// Windows turns the display off and sleeps after the idle timeout, and gamepad input does not count as activity. While
// the window is up and not minimized it holds the display and the system on, as other games do. The state belongs to
// the calling thread, so only the window thread (window_main and wnd_proc) calls this; the process exit releases it.
static void keep_awake(int on) {
    static int cur = -1;
    if (on == cur) return;
    cur = on;
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
}

// F11: borderless window over the whole monitor, and back to the saved window. The swapchain follows through WM_SIZE.
// Not Alt+Enter: Enter is the Plus button (input_win.c). A background window (test runs) keeps its place behind the
// user's windows and takes no focus.
static void toggle_fullscreen(HWND h) {
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = {sizeof mi};
        if (!GetWindowPlacement(h, &g_saved_place) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (g_background ? SWP_NOZORDER | SWP_NOACTIVATE : 0));
    } else {
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_POPUP) | WND_STYLE);
        if (g_background) g_saved_place.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(h, &g_saved_place);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED |
                         (g_background ? SWP_NOACTIVATE : 0));
    }
}

// Window thread, before the window is first shown: gives it the outer size ow x oh (the windowed size F11 returns to)
// and then makes it fullscreen on the monitor it was created on, so the first thing on screen is the fullscreen window.
// toggle_fullscreen saves the placement of a window that was never visible, which is its normal rectangle as just set.
// The window stays hidden here; the caller shows it. Returns whether the window is fullscreen now.
static int fullscreen_before_show(HWND h, int ow, int oh) {
    SetWindowPos(h, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    toggle_fullscreen(h);
    g_saved_place.showCmd = SW_SHOWNORMAL;   // a hidden window may report SW_HIDE, which F11 would restore as hidden
    return is_fullscreen(h);
}

// The window size and fullscreen state last taken from the settings (window thread). apply_settings acts on a change of
// either value only, so a window resized by hand keeps its size when another setting changes.
static char g_applied_size[32];
static int g_applied_fs;

static void note_applied(void) {
    snprintf(g_applied_size, sizeof g_applied_size, "%s", cfg_value("NX2NAVI_WINDOW_SIZE"));
    g_applied_fs = cfg_fullscreen();
}

// Window thread: the window takes the size and fullscreen state of the settings when they changed. In fullscreen the
// size goes into the placement restored when fullscreen ends.
static void apply_settings(HWND h) {
    int size_changed = strcmp(cfg_value("NX2NAVI_WINDOW_SIZE"), g_applied_size) != 0, fs = cfg_fullscreen(), fs_changed = fs != g_applied_fs;
    note_applied();
    if (size_changed) {
        int cw, ch, ow, oh;
        cfg_window_size(&cw, &ch);
        outer_size(h, cw, ch, &ow, &oh);
        if (is_fullscreen(h)) {
            RECT *n = &g_saved_place.rcNormalPosition;
            n->right = n->left + ow;
            n->bottom = n->top + oh;
        } else {
            RECT have;
            if (GetClientRect(h, &have) && (have.right != cw || have.bottom != ch)) {
                if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
                SetWindowPos(h, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    if (fs_changed && fs != is_fullscreen(h)) {
        input_cursor_suspend(1);
        toggle_fullscreen(h);
        input_cursor_suspend(0);
    }
}

void gfx_window_apply_settings(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_SETTINGS, 0, 0);
}


void gfx_window_request_close(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// The menu's text field lives in this window, which has no IME (detached at creation, below): while the field takes
// text the window gets the default one, so a player can type a name in Chinese, Japanese or Korean (Windows hands the
// composed characters over as WM_CHAR, which menu_window_msg passes on), and it is detached again after. Posted to
// the window thread, which owns the window's input context.
void gfx_window_text_input(int on) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_TEXT_INPUT, (WPARAM)(on != 0), 0);
}

void gfx_window_paste(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_PASTE, 0, 0);
}

// ---- refresh rate of the window's monitor (NX2NAVI_FPS_LIMIT=display), measured when the window opens, lands on another
// monitor or the display mode changes (gfx_window_set_refresh keeps it). QueryDisplayConfig gives the exact rational
// rate (59.94, 143.998); EnumDisplaySettings' whole hertz is the fallback.
static HMONITOR g_rate_mon;

static double monitor_rate_hz(HMONITOR mon) {
    MONITORINFOEXW mi = {.cbSize = sizeof mi};
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return 0;
    double hz = 0;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np && nm) {
        DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths);
        DISPLAYCONFIG_MODE_INFO *modes = calloc(nm, sizeof *modes);
        if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS)
            for (UINT32 i = 0; i < np && !hz; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                                                                   .size = sizeof src,
                                                                   .adapterId = paths[i].sourceInfo.adapterId,
                                                                   .id = paths[i].sourceInfo.id}};
                DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
                if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS && !wcscmp(src.viewGdiDeviceName, mi.szDevice) &&
                    r.Denominator)
                    hz = (double)r.Numerator / r.Denominator;
            }
        free(paths);
        free(modes);
    }
    DEVMODEW dm = {.dmSize = sizeof dm};
    if (!hz && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = dm.dmDisplayFrequency;   // 0 and 1 mean the hardware default
    return hz;
}

// Window thread. force: the display mode changed, measure even on the same monitor.
static void refresh_rate_update(HWND h, int force) {
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
    if (!force && mon == g_rate_mon) return;
    g_rate_mon = mon;
    gfx_window_set_refresh(monitor_rate_hz(mon));
}

// ---- monitor choice (NX2NAVI_MONITOR, --monitor, --list-monitors)
// The monitors in EnumDisplayMonitors order, which is the index the setting counts in (1-based) and --list-monitors
// prints. The order follows the display driver's adapter and output order, so it holds while the same monitors stay
// attached and enabled, but a monitor that appears or goes away (a virtual display) can move the ones after it: the GDI
// device name or the monitor's name pins one monitor more firmly than its index.
#define MON_MAX 32

typedef struct {
    HMONITOR h;
    RECT rc, work;                  // the monitor's rectangle and its work area (without the taskbar), desktop pixels
    int primary;
    UINT dpi;                       // effective DPI of the monitor, 96 at 100% display scale
    wchar_t device[CCHDEVICENAME];  // GDI device name, such as \\.\DISPLAY3
    wchar_t name[64];               // the monitor's friendly name from the display configuration; empty when it has none
} MonInfo;

typedef struct { MonInfo m[MON_MAX]; int n; } MonList;

static void to_utf8(const wchar_t *w, char *out, size_t n) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL)) out[0] = 0;
}

// GetDpiForMonitor lives in shcore.dll, which the build does not link; 0 when it is missing. The library is loaded once
// and kept: the monitor list asks for every monitor's DPI each time it is built.
static UINT monitor_dpi(HMONITOR mon) {
    typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);
    static GetDpiForMonitorFn fn;
    static int looked;
    if (!looked) {
        HMODULE lib = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        fn = lib ? (GetDpiForMonitorFn)(void *)GetProcAddress(lib, "GetDpiForMonitor") : NULL;
        looked = 1;
    }
    UINT x = 0, y = 0;
    if (!fn || fn(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y) != S_OK) x = 0;
    return x;
}

static BOOL CALLBACK enum_monitor(HMONITOR h, HDC dc, LPRECT rc, LPARAM lp) {
    (void)dc, (void)rc;
    MonList *l = (MonList *)lp;
    MONITORINFOEXW mi = {.cbSize = sizeof mi};
    if (l->n >= MON_MAX) return FALSE;
    if (!GetMonitorInfoW(h, (MONITORINFO *)&mi)) return TRUE;
    MonInfo *m = &l->m[l->n++];
    *m = (MonInfo){.h = h, .rc = mi.rcMonitor, .work = mi.rcWork, .primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0,
                   .dpi = monitor_dpi(h)};
    wcsncpy(m->device, mi.szDevice, CCHDEVICENAME - 1);
    return TRUE;
}

// Fills MonInfo.name: an active display path ties a GDI device (the source) to the monitor attached to it (the target),
// and the target knows the monitor's name. Cloned monitors share a source, and the first path with a name wins.
static void fill_monitor_names(MonList *l) {
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    for (int attempt = 0; attempt < 3; attempt++) {   // the topology can change between the size query and the query
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np || !nm) break;
        free(paths);
        free(modes);
        paths = calloc(np, sizeof *paths);
        modes = calloc(nm, sizeof *modes);
        if (!paths || !modes) break;
        LONG rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL);
        if (rc == ERROR_INSUFFICIENT_BUFFER) continue;
        if (rc != ERROR_SUCCESS) break;
        for (UINT32 i = 0; i < np; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                                                               .size = sizeof src,
                                                               .adapterId = paths[i].sourceInfo.adapterId,
                                                               .id = paths[i].sourceInfo.id}};
            DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
                                                               .size = sizeof tgt,
                                                               .adapterId = paths[i].targetInfo.adapterId,
                                                               .id = paths[i].targetInfo.id}};
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS ||
                DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS)
                continue;
            for (int k = 0; k < l->n; k++)
                if (!l->m[k].name[0] && !_wcsicmp(src.viewGdiDeviceName, l->m[k].device))
                    wcsncpy(l->m[k].name, tgt.monitorFriendlyDeviceName, (sizeof l->m[k].name / sizeof(wchar_t)) - 1);
        }
        break;
    }
    free(paths);
    free(modes);
}

static int enum_monitors(MonList *l) {
    *l = (MonList){0};
    EnumDisplayMonitors(NULL, NULL, enum_monitor, (LPARAM)l);
    fill_monitor_names(l);
    return l->n;
}

// "2  \\.\DISPLAY3  "Name"  1920x1080 at 2560,0  primary  dpi 144 (150%)": one monitor, for the log and --list-monitors.
static void monitor_line(const MonList *l, int i, char *out, size_t cap) {
    const MonInfo *m = &l->m[i];
    char dev[CCHDEVICENAME * 4], name[64 * 4];
    to_utf8(m->device, dev, sizeof dev);
    to_utf8(m->name, name, sizeof name);
    snprintf(out, cap, "%d  %s  \"%s\"  %ldx%ld at %ld,%ld%s  dpi %u (%u%%)", i + 1, dev, name, m->rc.right - m->rc.left,
             m->rc.bottom - m->rc.top, m->rc.left, m->rc.top, m->primary ? "  primary" : "", m->dpi,
             m->dpi * 100 / USER_DEFAULT_SCREEN_DPI);
}

static int monitor_unset(const char *v) { return !v || !*v || !_stricmp(v, "auto"); }

// Whether text contains needle_lower, which is already lower-cased; the comparison ignores case.
static int contains_nocase(const wchar_t *text, const wchar_t *needle_lower) {
    wchar_t buf[CCHDEVICENAME + 64];
    if (!*needle_lower) return 0;
    wcsncpy(buf, text, (sizeof buf / sizeof *buf) - 1);
    buf[(sizeof buf / sizeof *buf) - 1] = 0;
    CharLowerBuffW(buf, (DWORD)wcslen(buf));
    return wcsstr(buf, needle_lower) != NULL;
}

// Which monitor of the list a NX2NAVI_MONITOR value (not empty, not auto) names, as a 0-based index; -1 when none.
// Rules, in this order: "primary"; all digits, a 1-based index; any other text, a case-insensitive substring of a
// monitor's GDI device name or friendly name (the first monitor in list order when several match). why gets the
// rule that matched, or the reason nothing did.
static int resolve_monitor(const MonList *l, const char *want, char *why, size_t cap) {
    if (!_stricmp(want, "primary")) {
        for (int i = 0; i < l->n; i++)
            if (l->m[i].primary) {
                snprintf(why, cap, "the primary monitor");
                return i;
            }
        snprintf(why, cap, "no monitor is marked primary");
        return -1;
    }
    if (want[strspn(want, "0123456789")] == 0) {
        long idx = strtol(want, NULL, 10);
        if (idx >= 1 && idx <= l->n) {
            snprintf(why, cap, "index %ld of %d", idx, l->n);
            return (int)idx - 1;
        }
        snprintf(why, cap, "index %s is outside 1..%d", want, l->n);
        return -1;
    }
    wchar_t needle[CCHDEVICENAME + 64];
    if (!MultiByteToWideChar(CP_UTF8, 0, want, -1, needle, (int)(sizeof needle / sizeof *needle))) {
        snprintf(why, cap, "the text is not valid UTF-8 or is too long");
        return -1;
    }
    CharLowerBuffW(needle, (DWORD)wcslen(needle));
    int first = -1, hits = 0;
    for (int i = 0; i < l->n; i++)
        if (contains_nocase(l->m[i].device, needle) || contains_nocase(l->m[i].name, needle)) {
            if (first < 0) first = i;
            hits++;
        }
    if (first < 0) {
        snprintf(why, cap, "no device name or monitor name contains it");
        return -1;
    }
    if (hits > 1) snprintf(why, cap, "device or monitor name contains it; the first of %d matches", hits);
    else snprintf(why, cap, "device or monitor name contains it");
    return first;
}

// The monitor NX2NAVI_MONITOR picks, copied to *out; 0 when it is unset or auto (the system places the window), or when
// it matches nothing now (the monitors present are logged, and the default placement applies). log: say what was decided
// and why. The window thread asks with log set; the size query below, which widescreen.c makes before the window
// exists, stays quiet.
static int chosen_monitor(MonInfo *out, int log) {
    const char *want = cfg_get("NX2NAVI_MONITOR");
    if (monitor_unset(want)) return 0;
    MonList l;
    char why[256], line[512];
    int n = enum_monitors(&l), pick = -1;
    if (n) pick = resolve_monitor(&l, want, why, sizeof why);
    else snprintf(why, sizeof why, "no monitor was found");
    if (pick < 0) {
        if (log) {
            rt_log("gfx: NX2NAVI_MONITOR=%s picks no monitor (%s); the window gets the default placement. Monitors present:",
                   want, why);
            for (int i = 0; i < n; i++) {
                monitor_line(&l, i, line, sizeof line);
                rt_log("gfx:   %s", line);
            }
        }
        return 0;
    }
    if (log) {
        monitor_line(&l, pick, line, sizeof line);
        rt_log("gfx: NX2NAVI_MONITOR=%s picks monitor %s (%s)", want, line, why);
    }
    *out = l.m[pick];
    return 1;
}

// --list-monitors (main.c): one line per monitor, then what the current NX2NAVI_MONITOR would pick. No game files.
int list_monitors(void) {
    MonList l;
    char line[512], why[256];
    int n = enum_monitors(&l);
    if (!n) {
        fprintf(stderr, "list-monitors: no monitor found\n");
        return 1;
    }
    printf("index  device  name  desktop rectangle  primary  dpi\n");
    for (int i = 0; i < n; i++) {
        monitor_line(&l, i, line, sizeof line);
        printf("%s\n", line);
    }
    const char *want = cfg_get("NX2NAVI_MONITOR");
    if (monitor_unset(want)) {
        printf("NX2NAVI_MONITOR is not set (auto): the system places the window\n");
    } else {
        int pick = resolve_monitor(&l, want, why, sizeof why);
        if (pick >= 0) printf("NX2NAVI_MONITOR=%s picks monitor %d (%s)\n", want, pick + 1, why);
        else printf("NX2NAVI_MONITOR=%s picks no monitor (%s)\n", want, why);
    }
    return 0;
}

// The top-left that centres an ow x oh window in work; a window larger than the work area keeps its corner at the
// work area's corner, so its title bar stays reachable.
static void center_in(const RECT *work, int ow, int oh, int *x, int *y) {
    int ww = work->right - work->left, wh = work->bottom - work->top;
    *x = work->left + (ww > ow ? (ww - ow) / 2 : 0);
    *y = work->top + (wh > oh ? (wh - oh) / 2 : 0);
}

// Before the window exists (widescreen.c wide_init at start), the monitor a fullscreen window opens on: the one
// NX2NAVI_MONITOR picks (window_main places the window there), else the primary monitor.
int gfx_window_monitor_size(int *w, int *h) {
    MONITORINFO mi = {sizeof mi};
    MonInfo chosen;
    HMONITOR mon = g_hwnd                      ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY)
                   : chosen_monitor(&chosen, 0) ? chosen.h
                                                : MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(mon, &mi)) return 0;
    *w = mi.rcMonitor.right - mi.rcMonitor.left;
    *h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    return 1;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT input_res;
    if (input_window_msg(h, m, w, l, &input_res)) return input_res;
    if (menu_window_msg(h, m, w, l, &input_res)) return input_res;
    switch (m) {
    case WM_KEYDOWN:
        // bit 30: auto-repeat. Not with Ctrl or Shift held: overlays and capture tools take such F11 shortcuts for their
        // own (Alt+F11 comes as WM_SYSKEYDOWN)
        if (w == VK_F11 && !(l & (1 << 30)) && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_SHIFT) >= 0) {
            input_cursor_suspend(1);
            toggle_fullscreen(h);
            input_cursor_suspend(0);
            cfg_set("NX2NAVI_FULLSCREEN", is_fullscreen(h) ? "1" : "0");   // the menu shows it; the next start keeps it
            cfg_save();
            note_applied();   // what cfg_get reads now (the environment may still override it)
        }
        break;
    case WM_APP_SETTINGS:
        apply_settings(h);
        return 0;
    case WM_APP_FULLSCREEN:
        if (cfg_fullscreen() && !is_fullscreen(h)) {
            input_cursor_suspend(1);
            toggle_fullscreen(h);
            input_cursor_suspend(0);
        }
        return 0;
    case WM_APP_TEXT_INPUT:
        if (w) ImmAssociateContextEx(h, NULL, IACE_DEFAULT);
        else ImmAssociateContext(h, NULL);
        return 0;
    case WM_APP_PASTE: {
        char text[256];
        plat_clipboard_get(text, sizeof text);
        menu_event_paste(text);
        return 0;
    }
    case WM_SIZE:
        keep_awake(w != SIZE_MINIMIZED);
        if (w != SIZE_MINIMIZED) gfx_window_mark_dirty();
        refresh_rate_update(h, 0);
        return 0;
    case WM_EXITSIZEMOVE:   // dragged onto another monitor
        refresh_rate_update(h, 0);
        break;
    case WM_DISPLAYCHANGE:   // a resolution, layout or HDR change of the desktop
        refresh_rate_update(h, 1);
        if (is_fullscreen(h) && !IsIconic(h)) {   // keep covering the monitor at its new size (a same size sends no WM_SIZE)
            MONITORINFO mi = {sizeof mi};
            if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
                SetWindowPos(h, NULL, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        gfx_window_mark_dirty();   // the swapchain is made again: its size, and its colour space after an HDR switch
        break;
    case WM_GETDPISCALEDSIZE:   // a framed window keeps its client size in pixels on a monitor at another scale
        if (!is_fullscreen(h) && !IsZoomed(h)) {
            RECT c, r;
            if (GetClientRect(h, &c)) {
                r = (RECT){0, 0, c.right, c.bottom};
                if (AdjustWindowRectExForDpi(&r, WND_STYLE, FALSE, 0, LOWORD(w))) {
                    *(SIZE *)l = (SIZE){r.right - r.left, r.bottom - r.top};
                    return TRUE;
                }
            }
        }
        break;   // DefWindowProc: Windows scales the size with the DPI
    case WM_DPICHANGED: {   // display scale changed, or the window reached a monitor at another scale
        const RECT *s = (const RECT *)l;
        plat_xchg32(&g_dpi, LOWORD(w));
        rt_log("gfx: window DPI %u (%u%%)", LOWORD(w), LOWORD(w) * 100u / USER_DEFAULT_SCREEN_DPI);
        if (is_fullscreen(h)) {   // keep covering the monitor the suggested rect lies on
            MONITORINFO mi = {sizeof mi};
            if (GetMonitorInfoW(MonitorFromRect(s, MONITOR_DEFAULTTONEAREST), &mi))
                SetWindowPos(h, NULL, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPos(h, NULL, s->left, s->top, s->right - s->left, s->bottom - s->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_ACTIVATEAPP: {
        int in = w != 0;
        gfx_window_focus_changed(in);
        if (in && g_fs_pending) {
            g_fs_pending = 0;
            PostMessageW(h, WM_APP_FULLSCREEN, 0, 0);
        }
        return 0;
    }
    case WM_ENDSESSION:
        // Windows is shutting down, restarting or logging off: it ends the process after this message without a
        // WM_CLOSE, so the close path runs here (the settings menu's pending save, RTSS's profile, a save Commit in
        // flight); gfx_window_closed does not return
        if (w) gfx_window_closed();
        return 0;
    case WM_CLOSE:
        gfx_window_closed();
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

// Brings the game window to the front. Windows refuses SetForegroundWindow to a process that was not the last to get
// input, which is the case for a game started by a frontend; attaching this thread's input queue to the foreground
// window's thread for the call is the documented way around it. The window is not left topmost.
static void raise_window(HWND h) {
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId(), other = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    int attached = other && other != me && AttachThreadInput(me, other, TRUE);
    ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetActiveWindow(h);
    if (attached) AttachThreadInput(me, other, FALSE);
    if (GetForegroundWindow() != h) {   // still refused: a topmost toggle puts it above the others without keeping it there
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
}

// The window's pointer: a gold arrowhead in the menu's colours, its tip the hotspot at the top left, the half towards
// the tip's right edge a lighter facet, with a dark outline; sized for the system DPI (32 px at 96 DPI) and drawn with
// 4x4 samples per pixel into a 32-bit cursor with alpha. The arrow when anything fails.
static int in_tri(float px, float py, float ax, float ay, float bx, float by, float cx, float cy) {
    float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by), d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy),
          d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

static HCURSOR pointer_cursor(void) {
    UINT dpi = GetDpiForSystem();
    int n = (int)(32 * (dpi ? dpi : 96) / 96);
    if (n < 24) n = 24;
    if (n > 96) n = 96;
    BITMAPV5HEADER bh = {.bV5Size = sizeof bh, .bV5Width = n, .bV5Height = -n, .bV5Planes = 1, .bV5BitCount = 32,
                         .bV5Compression = BI_BITFIELDS, .bV5RedMask = 0x00ff0000, .bV5GreenMask = 0x0000ff00,
                         .bV5BlueMask = 0x000000ff, .bV5AlphaMask = 0xff000000};
    HDC dc = GetDC(NULL);
    uint32_t *px = NULL;
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    ReleaseDC(NULL, dc);
    HBITMAP mask = CreateBitmap(n, n, 1, 1, NULL);
    if (!color || !mask || !px) {
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        return LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    }
    // the arrowhead: tip (ax, ay), the right wing (bx, by), the lower wing (cx, cy), the notch (mx, my) between them
    float o = n * 0.07f, ax = o, ay = o, bx = o + n * 0.70f, by = o + n * 0.46f, cx = o + n * 0.30f, cy = o + n * 0.80f;
    float mx = o + n * 0.36f, my = o + n * 0.50f, e = n * 0.055f;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int light = 0, deep = 0, edge = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float fx = x + (sx + 0.5f) / 4, fy = y + (sy + 0.5f) / 4;
                    int l = in_tri(fx, fy, ax, ay, bx, by, mx, my), d = !l && in_tri(fx, fy, ax, ay, mx, my, cx, cy);
                    int out = in_tri(fx, fy, ax - e, ay - e * 1.3f, bx + e * 1.4f, by + e * 0.2f, mx, my + e * 1.2f) ||
                              in_tri(fx, fy, ax - e * 1.3f, ay - e, mx + e * 1.2f, my, cx + e * 0.2f, cy + e * 1.4f);
                    light += l, deep += d, edge += out && !l && !d;
                }
            int cov = light + deep + edge;
            uint32_t a = (uint32_t)(cov * 255 / 16), r = 0, g = 0, b = 0;
            if (cov) {
                r = (uint32_t)((255 * light + 214 * deep + 44 * edge) / cov);
                g = (uint32_t)((226 * light + 158 * deep + 28 * edge) / cov);
                b = (uint32_t)((138 * light + 58 * deep + 10 * edge) / cov);
            }
            // premultiplied by alpha, as 32-bit cursors want
            px[y * n + x] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);
        }
    ICONINFO ii = {.fIcon = FALSE, .xHotspot = (DWORD)o, .yHotspot = (DWORD)o, .hbmMask = mask, .hbmColor = color};
    HCURSOR cur = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return cur ? cur : LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
}

static void window_main(void *arg) {
    (void)arg;
    HINSTANCE inst = GetModuleHandleW(NULL);
    // Icon resource 1 of nx2navi.rc (runtime/nx2navi.ico): the title bar, the taskbar button and Alt+Tab.
    WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = wnd_proc, .hInstance = inst,
                      .hIcon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                          GetSystemMetrics(SM_CYICON), LR_SHARED),
                      .hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                            GetSystemMetrics(SM_CYSMICON), LR_SHARED),
                      .hCursor = pointer_cursor(), .lpszClassName = L"NX2naviRecompWindow"};
    RegisterClassExW(&wc);
    int cw, ch, ow, oh, x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    cfg_window_size(&cw, &ch);
    // NX2NAVI_MONITOR: the window is created centred in the chosen monitor's work area, with its frame measured at that
    // monitor's DPI, so it opens there at the scale it will have. Without a choice the system places it. Either way
    // the size is scaled down first if it does not fit the work area it opens in.
    MonInfo mon;
    int on_mon = chosen_monitor(&mon, 1);
    fit_work_area(&cw, &ch, on_mon ? &mon.work : NULL, on_mon && mon.dpi ? mon.dpi : GetDpiForSystem());
    outer_size(NULL, cw, ch, &ow, &oh);
    if (on_mon) {
        outer_size_at(cw, ch, mon.dpi ? mon.dpi : GetDpiForSystem(), &ow, &oh);
        center_in(&mon.work, ow, oh, &x, &y);
    }
    // The window opens behind the user's windows first and is then raised (raise_window): a game started by a frontend
    // has no other way to reach it. The game counts itself in focus until the first WM_ACTIVATEAPP, so loading goes on,
    // and keyboard input waits for the foreground (input_win.c). If Windows still refuses the foreground, the taskbar
    // button flashes. With NX2NAVI_FULLSCREEN the window is made fullscreen on the monitor it was created on before it
    // is first shown (fullscreen_before_show), so no framed window appears; the windowed rectangle stays in g_saved_place
    // for F11. If that fails, g_fs_pending retries at the first activation (wnd_proc, WM_ACTIVATEAPP).
    // NX2NAVI_BACKGROUND_WINDOW=1 (test runs): no flashing, never fullscreen at start, and F11 keeps the window's place.
    int background = g_background = cfg_get("NX2NAVI_BACKGROUND_WINDOW") && atoi(cfg_get("NX2NAVI_BACKGROUND_WINDOW"));
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"NX2navi", WND_STYLE, x, y, ow, oh, NULL, NULL,
                             inst, NULL);
    if (!g_hwnd) {
        rt_log("gfx: CreateWindowEx failed (%lu)", GetLastError());
        g_wnd_failed = 1;
    } else {
        // An IME switched on over the game attaches to this window and then shows its candidate window and eats the keys
        // it composes, although there is nothing here to type into (a Chinese IME takes Shift, the B button by default).
        // Detaching the game's window leaves the software keyboard alone: its edit box sits in a window of its own
        // (swkbd_win.c) and gets the IME it needs. The menu's text field attaches one while it takes text
        // (gfx_window_text_input).
        ImmAssociateContext(g_hwnd, NULL);
        // The frame was measured at the system DPI (or the chosen monitor's); the window may have opened on a monitor at
        // another scale. When the size changes on a chosen monitor, the window is centred again in its work area;
        // otherwise it keeps the place it was created at.
        int nw, nh, nx = 0, ny = 0;
        UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW;
        plat_xchg32(&g_dpi, (int32_t)GetDpiForWindow(g_hwnd));
        outer_size(g_hwnd, cw, ch, &nw, &nh);
        if (on_mon && (nw != ow || nh != oh)) center_in(&mon.work, nw, nh, &nx, &ny);
        else flags |= SWP_NOMOVE;
        // the fullscreen setting: the window turns fullscreen on the monitor it is on before it is shown, and keeps
        // that place and size (fullscreen_before_show); the windowed size stays for F11
        int fs = !background && cfg_fullscreen() && fullscreen_before_show(g_hwnd, nw, nh);
        SetWindowPos(g_hwnd, HWND_BOTTOM, nx, ny, nw, nh, flags | (fs ? SWP_NOSIZE | SWP_NOMOVE : 0));
        if (on_mon && MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST) != mon.h)
            rt_log("gfx: the window is not on the chosen monitor; F11 fullscreen covers the monitor it is on");
        if (!background) {
            // Pending before the raise: the activation it gets is the first WM_ACTIVATEAPP, which takes the fullscreen
            // setting; set after it, fullscreen waited for the next activation and the game started in a window. Only
            // a window that did not become fullscreen before it was shown still needs that retry.
            g_fs_pending = cfg_fullscreen() && !fs;
            raise_window(g_hwnd);   // a frontend's player may have no way to reach a window left behind
            FLASHWINFO fw = {.cbSize = sizeof fw, .hwnd = g_hwnd, .dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG};
            FlashWindowEx(&fw);
        }
        note_applied();
        refresh_rate_update(g_hwnd, 1);
        keep_awake(1);
    }
    plat_event_set(&g_wnd_ready);
    if (g_wnd_failed) return;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return;
}

// ---- gfx_wsys.h
// Makes the window on its thread and waits until it is there.
int wsys_open(void) {
    g_wnd_failed = 0;
    plat_event_init(&g_wnd_ready, 1, 0);
    if (!plat_thread_spawn(window_main, NULL, "window", 0)) rt_fatal("gfx: cannot start the window thread");
    plat_event_wait(&g_wnd_ready);
    return !g_wnd_failed;
}

VkSurfaceKHR wsys_surface(VkInstance inst) {
    VkWin32SurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                      .hinstance = GetModuleHandleW(NULL), .hwnd = g_hwnd};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    // through Streamline when DLSS Frame Generation is loaded: it maps swapchains to windows from this call
    PFN_vkCreateWin32SurfaceKHR create = (PFN_vkCreateWin32SurfaceKHR)vk_dlssg_instance_proc(inst, "vkCreateWin32SurfaceKHR");
    VkResult r = (create ? create : vkCreateWin32SurfaceKHR)(inst, &si, NULL, &surface);
    if (r != VK_SUCCESS) rt_log("gfx: vkCreateWin32SurfaceKHR failed (%d)", (int)r);
    return r == VK_SUCCESS ? surface : VK_NULL_HANDLE;
}

void wsys_client_size(int *w, int *h) {
    RECT rc = {0};
    GetClientRect(g_hwnd, &rc);
    *w = rc.right, *h = rc.bottom;
}

int wsys_minimized(void) { return IsIconic(g_hwnd) != 0; }

void wsys_resize(int cw, int ch) {
    int ow, oh;
    outer_size(g_hwnd, cw, ch, &ow, &oh);
    SetWindowPos(g_hwnd, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void wsys_probe_focus(int in) { SendMessageW(g_hwnd, WM_ACTIVATEAPP, in ? TRUE : FALSE, 0); }

void wsys_probe_escape(void) { SendMessageW(g_hwnd, WM_KEYDOWN, VK_ESCAPE, 0); }

int wsys_probe_click(int where) {
    if (where == 0) SendMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, 0);
    else SendMessageW(g_hwnd, WM_MOUSEACTIVATE, (WPARAM)g_hwnd, MAKELPARAM(where == 1 ? HTCLIENT : HTCAPTION, WM_LBUTTONDOWN));
    return 1;
}
