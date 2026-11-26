/*
 * Native Win32 Launcher UI (ISO Picker, XISO Extractor, Settings)
 * for The Sims: Bustin' Out
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher_ui.h"
#include "xiso_extract.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

/* Enable modern Common Controls v6 visual styles via linker manifest directive */
#if defined(_MSC_VER)
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

/* ── Control IDs ───────────────────────────────────────────── */

enum {
    ID_TAB            = 100,

    /* Tab 0: Game & ISO */
    ID_GRP_STATUS     = 200,
    ID_LBL_GAME_STAT  = 201,
    ID_GRP_ISO        = 202,
    ID_LBL_ISO        = 203,
    ID_EDT_ISO        = 204,
    ID_BTN_BROWSE_ISO = 205,
    ID_LBL_ISO_INFO   = 206,
    ID_BTN_EXTRACT    = 207,
    ID_PRG_EXTRACT    = 208,
    ID_LBL_EXTRACT    = 209,
    ID_GRP_PATHS      = 210,
    ID_LBL_GAMEDIR    = 211,
    ID_EDT_GAMEDIR    = 212,
    ID_BTN_GAMEDIR    = 213,
    ID_LBL_SAVEDIR    = 214,
    ID_EDT_SAVEDIR    = 215,
    ID_BTN_SAVEDIR    = 216,
    ID_BTN_OPENSAVES  = 217,

    /* Tab 1: Video & Graphics */
    ID_GRP_DISPLAY    = 300,
    ID_LBL_RES        = 301,
    ID_CMB_RES        = 302,
    ID_LBL_DISPMODE   = 303,
    ID_CMB_DISPMODE   = 304,
    ID_LBL_ASPECT     = 305,
    ID_CMB_ASPECT     = 306,
    ID_CHK_VSYNC      = 307,
    ID_GRP_RENDER     = 308,
    ID_CHK_HW_D3D11   = 309,
    ID_CHK_GPU_VSH    = 315,
    ID_CHK_CULL       = 310,
    ID_CHK_PERF       = 311,
    ID_LBL_FPS        = 316,
    ID_CMB_FPS        = 317,
    ID_LBL_THREADS    = 312,
    ID_CMB_THREADS    = 313,
    ID_LBL_GFX_NOTE   = 314,

    /* Tab 2: Audio & Controls */
    ID_GRP_AUDIO_IN   = 400,
    ID_CHK_AUDIO      = 401,
    ID_CHK_KEYBOARD   = 402,
    ID_CHK_SKIP       = 403,
    ID_GRP_CONTROLS   = 404,
    ID_EDT_CONTROLS   = 405,

    /* Bottom bar */
    ID_BTN_DEFAULTS   = 500,
    ID_BTN_SAVE       = 501,
    ID_BTN_PLAY       = 502,

    /* Custom messages */
    WM_EXTRACT_PROG   = WM_APP + 1,
    WM_EXTRACT_DONE   = WM_APP + 2,
    WM_DO_CAPTURE     = WM_APP + 3
};

typedef struct {
    int width, height;
    const char *label;
} ResPreset;

static const ResPreset k_res_presets[] = {
    {  640,  480, "640 x 480  (1x Original Xbox)" },
    { 1280,  960, "1280 x 960  (2x HD - Recommended)" },
    { 1920, 1440, "1920 x 1440  (3x Ultra HD)" },
    { 1280,  720, "1280 x 720  (720p Widescreen Window)" },
    { 1920, 1080, "1920 x 1080  (1080p Full HD Window)" },
    { 2560, 1440, "2560 x 1440  (1440p QHD Window)" }
};

static const int k_thread_vals[] = { 0, 1, 2, 4, 8 };
static const char *k_thread_labels[] = {
    "Auto (Up to 8 Threads - Recommended)",
    "1 Thread (Single-Threaded)",
    "2 Threads",
    "4 Threads",
    "8 Threads"
};

/* ── Launcher Window State ─────────────────────────────────── */

typedef struct {
    HWND            hwnd;
    HINSTANCE       hinst;
    LauncherConfig *cfg;
    const char     *capture_bmp_path;

    HFONT           fnt_ui;
    HFONT           fnt_bold;
    HFONT           fnt_title;
    HFONT           fnt_sub;
    HFONT           fnt_mono;

    int             cur_tab;
    int             game_ready;
    int             result_play;

    /* Background XISO extraction state */
    int             extracting;
    volatile long   cancel_extract;
    HANDLE          extract_thread;
    CRITICAL_SECTION prog_cs;
    uint32_t        prog_files_done;
    uint32_t        prog_files_total;
    uint64_t        prog_bytes_done;
    uint64_t        prog_bytes_total;
    char            prog_file[260];
    int             extract_rc;
    char            extract_err[256];
} LauncherState;

static LauncherState *g_ls = NULL;

/* ── Helpers ───────────────────────────────────────────────── */

static HWND make_ctrl(HWND parent, const char *cls, const char *text,
                      DWORD style, DWORD exstyle,
                      int x, int y, int w, int h, int id, HFONT font)
{
    HWND hw = CreateWindowExA(exstyle, cls, text, style,
                              x, y, w, h, parent, (HMENU)(intptr_t)id,
                              GetModuleHandleA(NULL), NULL);
    if (hw && font)
        SendMessageA(hw, WM_SETFONT, (WPARAM)font, TRUE);
    return hw;
}

static void show_tab_controls(HWND hwnd, int tab)
{
    static const int tab0[] = {
        ID_GRP_STATUS, ID_LBL_GAME_STAT,
        ID_GRP_ISO, ID_LBL_ISO, ID_EDT_ISO, ID_BTN_BROWSE_ISO, ID_LBL_ISO_INFO,
        ID_BTN_EXTRACT, ID_PRG_EXTRACT, ID_LBL_EXTRACT,
        ID_GRP_PATHS, ID_LBL_GAMEDIR, ID_EDT_GAMEDIR, ID_BTN_GAMEDIR,
        ID_LBL_SAVEDIR, ID_EDT_SAVEDIR, ID_BTN_SAVEDIR, ID_BTN_OPENSAVES
    };
    static const int tab1[] = {
        ID_GRP_DISPLAY, ID_LBL_RES, ID_CMB_RES, ID_LBL_DISPMODE, ID_CMB_DISPMODE,
        ID_LBL_ASPECT, ID_CMB_ASPECT, ID_CHK_VSYNC,
        ID_GRP_RENDER, ID_CHK_HW_D3D11, ID_CHK_GPU_VSH, ID_CHK_CULL, ID_CHK_PERF,
        ID_LBL_FPS, ID_CMB_FPS, ID_LBL_THREADS, ID_CMB_THREADS, ID_LBL_GFX_NOTE
    };
    static const int tab2[] = {
        ID_GRP_AUDIO_IN, ID_CHK_AUDIO, ID_CHK_KEYBOARD, ID_CHK_SKIP,
        ID_GRP_CONTROLS, ID_EDT_CONTROLS
    };
    size_t i;

    for (i = 0; i < sizeof(tab0)/sizeof(tab0[0]); i++)
        ShowWindow(GetDlgItem(hwnd, tab0[i]), tab == 0 ? SW_SHOW : SW_HIDE);
    for (i = 0; i < sizeof(tab1)/sizeof(tab1[0]); i++)
        ShowWindow(GetDlgItem(hwnd, tab1[i]), tab == 1 ? SW_SHOW : SW_HIDE);
    for (i = 0; i < sizeof(tab2)/sizeof(tab2[0]); i++)
        ShowWindow(GetDlgItem(hwnd, tab2[i]), tab == 2 ? SW_SHOW : SW_HIDE);
}

static void format_bytes(uint64_t b, char *out, size_t out_sz)
{
    if (b >= 1024ULL * 1024ULL * 1024ULL)
        snprintf(out, out_sz, "%.2f GB", (double)b / (1024.0 * 1024.0 * 1024.0));
    else if (b >= 1024ULL * 1024ULL)
        snprintf(out, out_sz, "%.1f MB", (double)b / (1024.0 * 1024.0));
    else
        snprintf(out, out_sz, "%.1f KB", (double)b / 1024.0);
}

static void refresh_iso_status(LauncherState *ls)
{
    char iso_path[MAX_PATH];
    char msg[512];
    XisoInfo info;

    GetDlgItemTextA(ls->hwnd, ID_EDT_ISO, iso_path, sizeof(iso_path));
    if (!iso_path[0]) {
        SetDlgItemTextA(ls->hwnd, ID_LBL_ISO_INFO,
                        "Select your Original Xbox .iso / .xiso.iso dump to inspect or extract.");
        EnableWindow(GetDlgItem(ls->hwnd, ID_BTN_EXTRACT), FALSE);
        return;
    }

    if (xiso_inspect(iso_path, &info)) {
        char szbuf[64];
        format_bytes(info.total_bytes, szbuf, sizeof(szbuf));
        if (info.has_default_xbe) {
            snprintf(msg, sizeof(msg),
                     "%s%s  |  Title ID: 0x%08X  |  %u files (%s)",
                     info.title_id_matches ? "Verified XISO: " : "Other Xbox XISO: ",
                     info.title_name[0] ? info.title_name : "Unknown",
                     info.title_id, info.file_count, szbuf);
        } else {
            snprintf(msg, sizeof(msg),
                     "Valid XISO (%u files, %s) -- warning: default.xbe not found in root.",
                     info.file_count, szbuf);
        }
        SetDlgItemTextA(ls->hwnd, ID_LBL_ISO_INFO, msg);
        EnableWindow(GetDlgItem(ls->hwnd, ID_BTN_EXTRACT), !ls->extracting);
    } else {
        SetDlgItemTextA(ls->hwnd, ID_LBL_ISO_INFO, info.error);
        EnableWindow(GetDlgItem(ls->hwnd, ID_BTN_EXTRACT), FALSE);
    }
}

static void refresh_game_status(LauncherState *ls)
{
    char game_dir[MAX_PATH];
    GameDirInfo ginfo;
    RECT banner = { 0, 0, 680, 86 };

    GetDlgItemTextA(ls->hwnd, ID_EDT_GAMEDIR, game_dir, sizeof(game_dir));
    ls->game_ready = xiso_validate_game_dir(game_dir, &ginfo);
    SetDlgItemTextA(ls->hwnd, ID_LBL_GAME_STAT, ginfo.status);
    EnableWindow(GetDlgItem(ls->hwnd, ID_BTN_PLAY), ls->game_ready && !ls->extracting);
    InvalidateRect(ls->hwnd, &banner, FALSE);
}

static void sync_gfx_enable_states(HWND hw)
{
    int hw_d3d11 = (IsDlgButtonChecked(hw, ID_CHK_HW_D3D11) == BST_CHECKED);
    int gpu_vsh  = (IsDlgButtonChecked(hw, ID_CHK_GPU_VSH) == BST_CHECKED);
    EnableWindow(GetDlgItem(hw, ID_CHK_GPU_VSH), hw_d3d11);
    EnableWindow(GetDlgItem(hw, ID_CHK_CULL),    hw_d3d11);
    /* CPU VSH worker threads only run when GPU VSH is unchecked or software GPU is active */
    EnableWindow(GetDlgItem(hw, ID_CMB_THREADS), !(hw_d3d11 && gpu_vsh));
}

static void load_ui_from_cfg(LauncherState *ls)
{
    const LauncherConfig *c = ls->cfg;
    HWND hw = ls->hwnd;
    int i, res_idx = 1, thr_idx = 0;

    SetDlgItemTextA(hw, ID_EDT_ISO, c->iso_path);
    SetDlgItemTextA(hw, ID_EDT_GAMEDIR, c->game_dir);
    SetDlgItemTextA(hw, ID_EDT_SAVEDIR, c->save_dir);

    for (i = 0; i < (int)(sizeof(k_res_presets)/sizeof(k_res_presets[0])); i++) {
        if (k_res_presets[i].width == c->win_width && k_res_presets[i].height == c->win_height) {
            res_idx = i;
            break;
        }
    }
    SendDlgItemMessageA(hw, ID_CMB_RES, CB_SETCURSEL, (WPARAM)res_idx, 0);
    SendDlgItemMessageA(hw, ID_CMB_DISPMODE, CB_SETCURSEL, (WPARAM)(c->fullscreen ? 1 : 0), 0);
    SendDlgItemMessageA(hw, ID_CMB_ASPECT, CB_SETCURSEL, (WPARAM)(c->aspect_stretch ? 1 : 0), 0);

    CheckDlgButton(hw, ID_CHK_VSYNC,    c->vsync ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, ID_CHK_HW_D3D11, !c->software_gpu ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, ID_CHK_GPU_VSH,  c->gpu_vsh ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, ID_CHK_CULL,     c->backface_cull ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, ID_CHK_PERF,     c->gpu_profile ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageA(hw, ID_CMB_FPS, CB_SETCURSEL, (WPARAM)(c->target_fps == 60 ? 1 : 0), 0);

    for (i = 0; i < (int)(sizeof(k_thread_vals)/sizeof(k_thread_vals[0])); i++) {
        if (k_thread_vals[i] == c->vsh_threads) { thr_idx = i; break; }
    }
    SendDlgItemMessageA(hw, ID_CMB_THREADS, CB_SETCURSEL, (WPARAM)thr_idx, 0);

    CheckDlgButton(hw, ID_CHK_AUDIO,    c->audio_enable ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, ID_CHK_KEYBOARD, c->keyboard_enable ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, ID_CHK_SKIP,     c->skip_launcher ? BST_CHECKED : BST_UNCHECKED);

    refresh_iso_status(ls);
    refresh_game_status(ls);
    sync_gfx_enable_states(hw);
}

static void save_ui_to_cfg(LauncherState *ls)
{
    LauncherConfig *c = ls->cfg;
    HWND hw = ls->hwnd;
    int idx;

    GetDlgItemTextA(hw, ID_EDT_ISO, c->iso_path, sizeof(c->iso_path));
    GetDlgItemTextA(hw, ID_EDT_GAMEDIR, c->game_dir, sizeof(c->game_dir));
    GetDlgItemTextA(hw, ID_EDT_SAVEDIR, c->save_dir, sizeof(c->save_dir));

    idx = (int)SendDlgItemMessageA(hw, ID_CMB_RES, CB_GETCURSEL, 0, 0);
    if (idx >= 0 && idx < (int)(sizeof(k_res_presets)/sizeof(k_res_presets[0]))) {
        c->win_width  = k_res_presets[idx].width;
        c->win_height = k_res_presets[idx].height;
    }
    c->fullscreen     = (SendDlgItemMessageA(hw, ID_CMB_DISPMODE, CB_GETCURSEL, 0, 0) == 1) ? 1 : 0;
    c->aspect_stretch = (SendDlgItemMessageA(hw, ID_CMB_ASPECT, CB_GETCURSEL, 0, 0) == 1) ? 1 : 0;
    c->vsync          = (IsDlgButtonChecked(hw, ID_CHK_VSYNC) == BST_CHECKED) ? 1 : 0;

    c->software_gpu   = (IsDlgButtonChecked(hw, ID_CHK_HW_D3D11) == BST_CHECKED) ? 0 : 1;
    c->gpu_vsh        = (IsDlgButtonChecked(hw, ID_CHK_GPU_VSH) == BST_CHECKED) ? 1 : 0;
    c->target_fps     = (SendDlgItemMessageA(hw, ID_CMB_FPS, CB_GETCURSEL, 0, 0) == 1) ? 60 : 30;
    c->backface_cull  = (IsDlgButtonChecked(hw, ID_CHK_CULL) == BST_CHECKED) ? 1 : 0;
    c->gpu_profile    = (IsDlgButtonChecked(hw, ID_CHK_PERF) == BST_CHECKED) ? 1 : 0;

    idx = (int)SendDlgItemMessageA(hw, ID_CMB_THREADS, CB_GETCURSEL, 0, 0);
    if (idx >= 0 && idx < (int)(sizeof(k_thread_vals)/sizeof(k_thread_vals[0])))
        c->vsh_threads = k_thread_vals[idx];

    c->audio_enable    = (IsDlgButtonChecked(hw, ID_CHK_AUDIO) == BST_CHECKED) ? 1 : 0;
    c->keyboard_enable = (IsDlgButtonChecked(hw, ID_CHK_KEYBOARD) == BST_CHECKED) ? 1 : 0;
    c->skip_launcher   = (IsDlgButtonChecked(hw, ID_CHK_SKIP) == BST_CHECKED) ? 1 : 0;
}

/* ── Browse Dialogs ────────────────────────────────────────── */

static int browse_iso_file(HWND owner, char *path_io, size_t path_sz)
{
    OPENFILENAMEA ofn;
    char buf[MAX_PATH];

    memset(&ofn, 0, sizeof(ofn));
    snprintf(buf, sizeof(buf), "%s", path_io ? path_io : "");
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = "Xbox ISO Images (*.iso;*.xiso)\0*.iso;*.xiso\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = sizeof(buf);
    ofn.lpstrTitle  = "Select The Sims: Bustin' Out Xbox ISO";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        snprintf(path_io, path_sz, "%s", buf);
        return 1;
    }
    return 0;
}

static int CALLBACK browse_folder_cb(HWND hwnd, UINT uMsg, LPARAM lParam, LPARAM lpData)
{
    (void)lParam;
    if (uMsg == BFFM_INITIALIZED && lpData)
        SendMessageA(hwnd, BFFM_SETSELECTIONA, TRUE, lpData);
    return 0;
}

static int browse_folder(HWND owner, const char *title, char *path_io, size_t path_sz)
{
    BROWSEINFOA bi;
    LPITEMIDLIST pidl;
    char out[MAX_PATH];

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn      = browse_folder_cb;
    bi.lParam    = (LPARAM)path_io;

    pidl = SHBrowseForFolderA(&bi);
    if (pidl) {
        int ok = SHGetPathFromIDListA(pidl, out) ? 1 : 0;
        CoTaskMemFree(pidl);
        if (ok) {
            snprintf(path_io, path_sz, "%s", out);
            return 1;
        }
    }
    return 0;
}

/* ── Background Extraction Worker ──────────────────────────── */

static void extract_progress_cb(uint32_t files_done, uint32_t files_total,
                                uint64_t bytes_done, uint64_t bytes_total,
                                const char *current_file, void *user_data)
{
    LauncherState *ls = (LauncherState *)user_data;
    static DWORD last_post = 0;
    DWORD now = GetTickCount();

    EnterCriticalSection(&ls->prog_cs);
    ls->prog_files_done  = files_done;
    ls->prog_files_total = files_total;
    ls->prog_bytes_done  = bytes_done;
    ls->prog_bytes_total = bytes_total;
    snprintf(ls->prog_file, sizeof(ls->prog_file), "%s", current_file ? current_file : "");
    LeaveCriticalSection(&ls->prog_cs);

    if (now - last_post >= 50 || files_done == files_total) {
        last_post = now;
        PostMessageA(ls->hwnd, WM_EXTRACT_PROG, 0, 0);
    }
}

static DWORD WINAPI extract_worker_proc(LPVOID arg)
{
    LauncherState *ls = (LauncherState *)arg;
    char iso[MAX_PATH], out_dir[MAX_PATH];

    GetDlgItemTextA(ls->hwnd, ID_EDT_ISO, iso, sizeof(iso));
    GetDlgItemTextA(ls->hwnd, ID_EDT_GAMEDIR, out_dir, sizeof(out_dir));

    ls->extract_rc = xiso_extract_all(iso, out_dir,
                                      extract_progress_cb, ls,
                                      &ls->cancel_extract,
                                      ls->extract_err, sizeof(ls->extract_err));
    PostMessageA(ls->hwnd, WM_EXTRACT_DONE, 0, 0);
    return 0;
}

static void start_or_cancel_extract(LauncherState *ls)
{
    if (ls->extracting) {
        InterlockedExchange(&ls->cancel_extract, 1);
        SetDlgItemTextA(ls->hwnd, ID_LBL_EXTRACT, "Cancelling extraction...");
        EnableWindow(GetDlgItem(ls->hwnd, ID_BTN_EXTRACT), FALSE);
        return;
    }

    {
        char iso[MAX_PATH], out_dir[MAX_PATH];
        XisoInfo info;
        GetDlgItemTextA(ls->hwnd, ID_EDT_ISO, iso, sizeof(iso));
        GetDlgItemTextA(ls->hwnd, ID_EDT_GAMEDIR, out_dir, sizeof(out_dir));
        if (!out_dir[0]) {
            MessageBoxA(ls->hwnd, "Please choose a destination Game Folder first.",
                        "Extract ISO", MB_ICONWARNING);
            return;
        }
        if (!xiso_inspect(iso, &info)) {
            MessageBoxA(ls->hwnd, info.error, "Invalid ISO", MB_ICONERROR);
            return;
        }
        if (info.has_default_xbe && !info.title_id_matches) {
            int ans = MessageBoxA(
                ls->hwnd,
                "This Xbox ISO does not have Title ID 0x45410043 (The Sims: Bustin' Out).\n"
                "Extract its files anyway?",
                "Different Xbox Title", MB_YESNO | MB_ICONWARNING);
            if (ans != IDYES) return;
        }
    }

    ls->extracting = 1;
    ls->cancel_extract = 0;
    SetDlgItemTextA(ls->hwnd, ID_BTN_EXTRACT, "Cancel Extraction");
    SetDlgItemTextA(ls->hwnd, ID_LBL_EXTRACT, "Starting extraction...");
    SendDlgItemMessageA(ls->hwnd, ID_PRG_EXTRACT, PBM_SETPOS, 0, 0);
    EnableWindow(GetDlgItem(ls->hwnd, ID_BTN_BROWSE_ISO), FALSE);
    EnableWindow(GetDlgItem(ls->hwnd, ID_BTN_GAMEDIR), FALSE);
    EnableWindow(GetDlgItem(ls->hwnd, ID_BTN_PLAY), FALSE);
    EnableWindow(GetDlgItem(ls->hwnd, ID_BTN_SAVE), FALSE);
    EnableWindow(GetDlgItem(ls->hwnd, ID_BTN_DEFAULTS), FALSE);

    ls->extract_thread = CreateThread(NULL, 0, extract_worker_proc, ls, 0, NULL);
}

/* ── Header Banner Painting (Sims Theme + Plumbob) ─────────── */

static void paint_banner(LauncherState *ls, HDC hdc, int w)
{
    const int bh = 82;
    int y;
    HPEN old_pen, pen;
    HBRUSH br, old_br;

    /* Smooth vertical gradient: deep Sims royal blue -> teal-blue */
    for (y = 0; y < bh; y++) {
        int r = 16 + (y * 14) / bh;
        int g = 46 + (y * 54) / bh;
        int b = 118 + (y * 46) / bh;
        RECT row = { 0, y, w, y + 1 };
        HBRUSH rbr = CreateSolidBrush(RGB(r, g, b));
        FillRect(hdc, &row, rbr);
        DeleteObject(rbr);
    }
    /* Crisp bottom accent line */
    {
        RECT line = { 0, bh, w, bh + 3 };
        HBRUSH lbr = CreateSolidBrush(RGB(72, 214, 92));
        FillRect(hdc, &line, lbr);
        DeleteObject(lbr);
    }

    /* Draw faceted Sims emerald Plumbob diamond on the left */
    {
        int cx = 36, cy = 41;
        POINT left_top[3]    = { {cx, cy - 26}, {cx - 15, cy}, {cx, cy} };
        POINT right_top[3]   = { {cx, cy - 26}, {cx + 15, cy}, {cx, cy} };
        POINT left_bot[3]    = { {cx, cy + 26}, {cx - 15, cy}, {cx, cy} };
        POINT right_bot[3]   = { {cx, cy + 26}, {cx + 15, cy}, {cx, cy} };
        POINT center_hi[4]   = { {cx, cy - 26}, {cx + 6, cy}, {cx, cy + 26}, {cx - 6, cy} };

        pen = CreatePen(PS_SOLID, 1, RGB(28, 120, 42));
        old_pen = (HPEN)SelectObject(hdc, pen);

        br = CreateSolidBrush(RGB(110, 245, 118)); old_br = (HBRUSH)SelectObject(hdc, br);
        Polygon(hdc, left_top, 3); SelectObject(hdc, old_br); DeleteObject(br);

        br = CreateSolidBrush(RGB(56, 206, 74)); old_br = (HBRUSH)SelectObject(hdc, br);
        Polygon(hdc, right_top, 3); SelectObject(hdc, old_br); DeleteObject(br);

        br = CreateSolidBrush(RGB(40, 174, 58)); old_br = (HBRUSH)SelectObject(hdc, br);
        Polygon(hdc, left_bot, 3); SelectObject(hdc, old_br); DeleteObject(br);

        br = CreateSolidBrush(RGB(26, 138, 44)); old_br = (HBRUSH)SelectObject(hdc, br);
        Polygon(hdc, right_bot, 3); SelectObject(hdc, old_br); DeleteObject(br);

        br = CreateSolidBrush(RGB(148, 255, 152)); old_br = (HBRUSH)SelectObject(hdc, br);
        Polygon(hdc, center_hi, 4); SelectObject(hdc, old_br); DeleteObject(br);

        SelectObject(hdc, old_pen);
        DeleteObject(pen);
    }

    /* Title & Subtitle */
    SetBkMode(hdc, TRANSPARENT);
    {
        HFONT of = (HFONT)SelectObject(hdc, ls->fnt_title);
        SetTextColor(hdc, RGB(12, 24, 54));
        TextOutA(hdc, 67, 15, "The Sims: Bustin' Out", 21);
        SetTextColor(hdc, RGB(255, 255, 255));
        TextOutA(hdc, 66, 14, "The Sims: Bustin' Out", 21);
        SelectObject(hdc, ls->fnt_sub);
        SetTextColor(hdc, RGB(205, 232, 255));
        TextOutA(hdc, 67, 48, "Native Windows x64 Static Recompilation  |  Direct3D 11 & XAudio2", 65);
        SelectObject(hdc, of);
    }

    /* Status Pill on top-right */
    {
        RECT pill = { w - 166, 24, w - 18, 54 };
        HBRUSH pbr = CreateSolidBrush(ls->game_ready ? RGB(34, 156, 66) : RGB(196, 126, 22));
        HPEN ppen = CreatePen(PS_SOLID, 1, ls->game_ready ? RGB(130, 240, 152) : RGB(255, 210, 110));
        HFONT of = (HFONT)SelectObject(hdc, ls->fnt_bold);
        old_br = (HBRUSH)SelectObject(hdc, pbr);
        old_pen = (HPEN)SelectObject(hdc, ppen);
        RoundRect(hdc, pill.left, pill.top, pill.right, pill.bottom, 14, 14);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextA(hdc, ls->game_ready ? "Ready to Play" : "Setup Required",
                  -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old_br);
        SelectObject(hdc, old_pen);
        SelectObject(hdc, of);
        DeleteObject(pbr);
        DeleteObject(ppen);
    }
}

/* ── Optional UI Screenshot Capture (for automated visual verification) ── */

static void capture_window_to_bmp(HWND hwnd, const char *path)
{
    RECT rc;
    int w, h;
    HDC hdc_win, hdc_mem;
    HBITMAP hbm, old_bm;
    BITMAPINFO bi;
    uint8_t *bits = NULL;
    FILE *f;

    GetWindowRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    hdc_win = GetWindowDC(hwnd);
    hdc_mem = CreateCompatibleDC(hdc_win);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = h; /* bottom-up BMP */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    hbm = CreateDIBSection(hdc_mem, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (hbm && bits) {
        old_bm = (HBITMAP)SelectObject(hdc_mem, hbm);
        PrintWindow(hwnd, hdc_mem, 0);
        SelectObject(hdc_mem, old_bm);

        f = fopen(path, "wb");
        if (f) {
            uint32_t img_sz = (uint32_t)w * (uint32_t)h * 4u;
            BITMAPFILEHEADER bfh;
            memset(&bfh, 0, sizeof(bfh));
            bfh.bfType    = 0x4D42;
            bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            bfh.bfSize    = bfh.bfOffBits + img_sz;
            fwrite(&bfh, 1, sizeof(bfh), f);
            fwrite(&bi.bmiHeader, 1, sizeof(BITMAPINFOHEADER), f);
            fwrite(bits, 1, img_sz, f);
            fclose(f);
        }
        DeleteObject(hbm);
    }
    DeleteDC(hdc_mem);
    ReleaseDC(hwnd, hdc_win);
}

/* ── Window Procedure ──────────────────────────────────────── */

static LRESULT CALLBACK launcher_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    LauncherState *ls = g_ls;

    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND ctrl = (HWND)lParam;
        int cid = GetDlgCtrlID(ctrl);
        if (cid == ID_EDT_CONTROLS) {
            SetBkColor(hdc, RGB(248, 250, 252));
            SetTextColor(hdc, RGB(24, 34, 48));
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(24, 34, 48));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }

    case WM_CTLCOLORBTN: {
        HWND ctrl = (HWND)lParam;
        int cid = GetDlgCtrlID(ctrl);
        if (cid == ID_CHK_VSYNC || cid == ID_CHK_HW_D3D11 || cid == ID_CHK_GPU_VSH
         || cid == ID_CHK_CULL || cid == ID_CHK_PERF || cid == ID_CHK_AUDIO
         || cid == ID_CHK_KEYBOARD || cid == ID_CHK_SKIP) {
            SetBkMode((HDC)wParam, TRANSPARENT);
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (ls) paint_banner(ls, hdc, rc.right);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lParam;
        if (ls && nm && nm->idFrom == ID_TAB && nm->code == TCN_SELCHANGE) {
            ls->cur_tab = TabCtrl_GetCurSel(nm->hwndFrom);
            show_tab_controls(hwnd, ls->cur_tab);
        }
        return 0;
    }

    case WM_EXTRACT_PROG:
        if (ls) {
            uint32_t fd, ft;
            uint64_t bd, bt;
            char fn[260], s1[64], s2[64], line[512];
            int pct = 0;

            EnterCriticalSection(&ls->prog_cs);
            fd = ls->prog_files_done;  ft = ls->prog_files_total;
            bd = ls->prog_bytes_done;  bt = ls->prog_bytes_total;
            snprintf(fn, sizeof(fn), "%s", ls->prog_file);
            LeaveCriticalSection(&ls->prog_cs);

            if (bt > 0) pct = (int)((bd * 1000ULL) / bt);
            if (pct > 1000) pct = 1000;
            SendDlgItemMessageA(hwnd, ID_PRG_EXTRACT, PBM_SETPOS, (WPARAM)pct, 0);
            format_bytes(bd, s1, sizeof(s1));
            format_bytes(bt, s2, sizeof(s2));
            snprintf(line, sizeof(line), "Extracting %u / %u files (%s / %s): %s",
                     fd, ft, s1, s2, fn);
            SetDlgItemTextA(hwnd, ID_LBL_EXTRACT, line);
        }
        return 0;

    case WM_EXTRACT_DONE:
        if (ls) {
            if (ls->extract_thread) {
                WaitForSingleObject(ls->extract_thread, 2000);
                CloseHandle(ls->extract_thread);
                ls->extract_thread = NULL;
            }
            ls->extracting = 0;
            SetDlgItemTextA(hwnd, ID_BTN_EXTRACT, "Extract ISO -> Game Folder");
            EnableWindow(GetDlgItem(hwnd, ID_BTN_EXTRACT), TRUE);
            EnableWindow(GetDlgItem(hwnd, ID_BTN_BROWSE_ISO), TRUE);
            EnableWindow(GetDlgItem(hwnd, ID_BTN_GAMEDIR), TRUE);
            EnableWindow(GetDlgItem(hwnd, ID_BTN_SAVE), TRUE);
            EnableWindow(GetDlgItem(hwnd, ID_BTN_DEFAULTS), TRUE);

            if (ls->extract_rc == 1) {
                SendDlgItemMessageA(hwnd, ID_PRG_EXTRACT, PBM_SETPOS, 1000, 0);
                SetDlgItemTextA(hwnd, ID_LBL_EXTRACT, "Extraction complete! Ready to play.");
                save_ui_to_cfg(ls);
                launcher_config_save(ls->cfg);
            } else if (ls->extract_rc == -1) {
                SetDlgItemTextA(hwnd, ID_LBL_EXTRACT, "Extraction cancelled.");
            } else {
                SetDlgItemTextA(hwnd, ID_LBL_EXTRACT, ls->extract_err);
                MessageBoxA(hwnd, ls->extract_err, "Extraction Failed", MB_ICONERROR);
            }
            refresh_game_status(ls);
        }
        return 0;

    case WM_DO_CAPTURE:
        if (ls && ls->capture_bmp_path && *ls->capture_bmp_path) {
            UpdateWindow(hwnd);
            capture_window_to_bmp(hwnd, ls->capture_bmp_path);
            ls->result_play = 0;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (!ls) break;

        if (code == EN_KILLFOCUS && id == ID_EDT_ISO) {
            refresh_iso_status(ls);
            return 0;
        }
        if (code == EN_KILLFOCUS && id == ID_EDT_GAMEDIR) {
            refresh_game_status(ls);
            return 0;
        }

        switch (id) {
        case ID_CHK_HW_D3D11:
        case ID_CHK_GPU_VSH:
            sync_gfx_enable_states(hwnd);
            return 0;

        case ID_BTN_BROWSE_ISO: {
            char p[MAX_PATH];
            GetDlgItemTextA(hwnd, ID_EDT_ISO, p, sizeof(p));
            if (browse_iso_file(hwnd, p, sizeof(p))) {
                SetDlgItemTextA(hwnd, ID_EDT_ISO, p);
                refresh_iso_status(ls);
            }
            return 0;
        }
        case ID_BTN_EXTRACT:
            start_or_cancel_extract(ls);
            return 0;

        case ID_BTN_GAMEDIR: {
            char p[MAX_PATH];
            GetDlgItemTextA(hwnd, ID_EDT_GAMEDIR, p, sizeof(p));
            if (browse_folder(hwnd, "Select Extracted Game Folder (containing default.xbe)", p, sizeof(p))) {
                SetDlgItemTextA(hwnd, ID_EDT_GAMEDIR, p);
                refresh_game_status(ls);
            }
            return 0;
        }
        case ID_BTN_SAVEDIR: {
            char p[MAX_PATH];
            GetDlgItemTextA(hwnd, ID_EDT_SAVEDIR, p, sizeof(p));
            if (browse_folder(hwnd, "Select Save Data Folder", p, sizeof(p))) {
                SetDlgItemTextA(hwnd, ID_EDT_SAVEDIR, p);
            }
            return 0;
        }
        case ID_BTN_OPENSAVES: {
            char p[MAX_PATH];
            GetDlgItemTextA(hwnd, ID_EDT_SAVEDIR, p, sizeof(p));
            if (p[0]) {
                CreateDirectoryA(p, NULL);
                ShellExecuteA(hwnd, "explore", p, NULL, NULL, SW_SHOWNORMAL);
            }
            return 0;
        }
        case ID_BTN_DEFAULTS: {
            char ini_keep[MAX_PATH];
            snprintf(ini_keep, sizeof(ini_keep), "%s", ls->cfg->config_path);
            launcher_config_defaults(ls->cfg, ini_keep);
            load_ui_from_cfg(ls);
            return 0;
        }
        case ID_BTN_SAVE: {
            char msg[MAX_PATH + 64];
            save_ui_to_cfg(ls);
            if (launcher_config_save(ls->cfg)) {
                snprintf(msg, sizeof(msg), "Settings saved to:\n%s", ls->cfg->config_path);
                SetDlgItemTextA(hwnd, ID_LBL_EXTRACT, "Settings saved.");
                MessageBoxA(hwnd, msg, "The Sims: Bustin' Out Launcher", MB_ICONINFORMATION);
            } else {
                snprintf(msg, sizeof(msg), "Failed to write settings to:\n%s", ls->cfg->config_path);
                MessageBoxA(hwnd, msg, "Save Settings", MB_ICONERROR);
            }
            return 0;
        }

        case ID_BTN_PLAY:
            save_ui_to_cfg(ls);
            refresh_game_status(ls);
            if (!ls->game_ready) {
                MessageBoxA(hwnd,
                            "No valid extracted game files found.\n"
                            "Select your Xbox ISO and click 'Extract ISO -> Game Folder', or browse to an existing game folder.",
                            "Game Files Required", MB_ICONWARNING);
                return 0;
            }
            launcher_config_save(ls->cfg);
            ls->result_play = 1;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        if (ls && ls->extracting) {
            int ans = MessageBoxA(hwnd, "ISO extraction is in progress. Cancel and close?",
                                  "Extraction Running", MB_YESNO | MB_ICONQUESTION);
            if (ans != IDYES) return 0;
            InterlockedExchange(&ls->cancel_extract, 1);
            if (ls->extract_thread) {
                WaitForSingleObject(ls->extract_thread, 5000);
                CloseHandle(ls->extract_thread);
                ls->extract_thread = NULL;
            }
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ── Public Entry Point ────────────────────────────────────── */

int launcher_ui_show(HINSTANCE hInst, LauncherConfig *cfg,
                     const char *capture_bmp_path, int initial_tab)
{
    INITCOMMONCONTROLSEX icc;
    WNDCLASSA wc;
    LauncherState ls;
    RECT rc;
    int win_w = 680, win_h = 540;
    int scr_w, scr_h;
    HWND tab, hw;
    TCITEMA tci;
    MSG msg;
    size_t i;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    memset(&icc, 0, sizeof(icc));
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    memset(&ls, 0, sizeof(ls));
    ls.hinst            = hInst;
    ls.cfg              = cfg;
    ls.capture_bmp_path = capture_bmp_path;
    InitializeCriticalSection(&ls.prog_cs);
    g_ls = &ls;

    ls.fnt_ui    = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    ls.fnt_bold  = CreateFontA(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    ls.fnt_title = CreateFontA(-24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    ls.fnt_sub   = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    ls.fnt_mono  = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = launcher_wndproc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "SimsBustinOutLauncherWnd";
    RegisterClassA(&wc);

    rc.left = 0; rc.top = 0; rc.right = win_w; rc.bottom = win_h;
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    scr_w = GetSystemMetrics(SM_CXSCREEN);
    scr_h = GetSystemMetrics(SM_CYSCREEN);

    ls.hwnd = CreateWindowExA(
        0, wc.lpszClassName, "The Sims: Bustin' Out - Launcher & Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (scr_w - (rc.right - rc.left)) / 2,
        (scr_h - (rc.bottom - rc.top)) / 2,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInst, NULL);
    if (!ls.hwnd) {
        DeleteCriticalSection(&ls.prog_cs);
        CoUninitialize();
        return 0;
    }

    /* Tab control */
    tab = make_ctrl(ls.hwnd, WC_TABCONTROLA, "",
                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0,
                    14, 96, win_w - 28, 386, ID_TAB, ls.fnt_bold);
    memset(&tci, 0, sizeof(tci));
    tci.mask = TCIF_TEXT;
    tci.pszText = "  Game && ISO  ";     TabCtrl_InsertItem(tab, 0, &tci);
    tci.pszText = "  Video && Graphics  "; TabCtrl_InsertItem(tab, 1, &tci);
    tci.pszText = "  Audio && Controls  "; TabCtrl_InsertItem(tab, 2, &tci);

    /* ── Tab 0: Game & ISO ── */
    make_ctrl(ls.hwnd, "BUTTON", "Game Installation Status",
              WS_CHILD | BS_GROUPBOX, 0, 28, 132, 624, 58, ID_GRP_STATUS, ls.fnt_bold);
    make_ctrl(ls.hwnd, "STATIC", "Checking game directory...",
              WS_CHILD | SS_LEFT, 0, 42, 156, 596, 24, ID_LBL_GAME_STAT, ls.fnt_ui);

    make_ctrl(ls.hwnd, "BUTTON", "Xbox ISO Picker && Automatic Extractor",
              WS_CHILD | BS_GROUPBOX, 0, 28, 200, 624, 134, ID_GRP_ISO, ls.fnt_bold);
    make_ctrl(ls.hwnd, "STATIC", "ISO File:",
              WS_CHILD, 0, 42, 224, 62, 20, ID_LBL_ISO, ls.fnt_ui);
    make_ctrl(ls.hwnd, "EDIT", "",
              WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
              108, 221, 422, 24, ID_EDT_ISO, ls.fnt_ui);
    make_ctrl(ls.hwnd, "BUTTON", "Browse ISO...",
              WS_CHILD | WS_TABSTOP, 0, 538, 220, 100, 26, ID_BTN_BROWSE_ISO, ls.fnt_ui);
    make_ctrl(ls.hwnd, "STATIC", "",
              WS_CHILD | SS_LEFT, 0, 42, 252, 596, 20, ID_LBL_ISO_INFO, ls.fnt_ui);
    make_ctrl(ls.hwnd, "BUTTON", "Extract ISO -> Game Folder",
              WS_CHILD | WS_TABSTOP, 0, 42, 276, 210, 26, ID_BTN_EXTRACT, ls.fnt_bold);
    hw = make_ctrl(ls.hwnd, PROGRESS_CLASSA, "",
                   WS_CHILD, 0, 260, 278, 378, 22, ID_PRG_EXTRACT, NULL);
    SendMessageA(hw, PBM_SETRANGE32, 0, 1000);
    make_ctrl(ls.hwnd, "STATIC", "Original ISO is never modified during extraction.",
              WS_CHILD | SS_PATHELLIPSIS, 0, 42, 308, 596, 18, ID_LBL_EXTRACT, ls.fnt_ui);

    make_ctrl(ls.hwnd, "BUTTON", "Directories",
              WS_CHILD | BS_GROUPBOX, 0, 28, 344, 624, 124, ID_GRP_PATHS, ls.fnt_bold);
    make_ctrl(ls.hwnd, "STATIC", "Game Folder:",
              WS_CHILD, 0, 42, 372, 86, 20, ID_LBL_GAMEDIR, ls.fnt_ui);
    make_ctrl(ls.hwnd, "EDIT", "",
              WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
              132, 369, 398, 24, ID_EDT_GAMEDIR, ls.fnt_ui);
    make_ctrl(ls.hwnd, "BUTTON", "Browse...",
              WS_CHILD | WS_TABSTOP, 0, 538, 368, 100, 26, ID_BTN_GAMEDIR, ls.fnt_ui);
    make_ctrl(ls.hwnd, "STATIC", "Save Folder:",
              WS_CHILD, 0, 42, 408, 86, 20, ID_LBL_SAVEDIR, ls.fnt_ui);
    make_ctrl(ls.hwnd, "EDIT", "",
              WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
              132, 405, 314, 24, ID_EDT_SAVEDIR, ls.fnt_ui);
    make_ctrl(ls.hwnd, "BUTTON", "Browse...",
              WS_CHILD | WS_TABSTOP, 0, 452, 404, 84, 26, ID_BTN_SAVEDIR, ls.fnt_ui);
    make_ctrl(ls.hwnd, "BUTTON", "Open Saves",
              WS_CHILD | WS_TABSTOP, 0, 542, 404, 96, 26, ID_BTN_OPENSAVES, ls.fnt_ui);

    /* ── Tab 1: Video & Graphics ── */
    make_ctrl(ls.hwnd, "BUTTON", "Display",
              WS_CHILD | BS_GROUPBOX, 0, 28, 132, 624, 152, ID_GRP_DISPLAY, ls.fnt_bold);
    make_ctrl(ls.hwnd, "STATIC", "Window Resolution:",
              WS_CHILD, 0, 44, 160, 135, 20, ID_LBL_RES, ls.fnt_ui);
    hw = make_ctrl(ls.hwnd, "COMBOBOX", "",
                   WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0,
                   186, 156, 300, 200, ID_CMB_RES, ls.fnt_ui);
    for (i = 0; i < sizeof(k_res_presets)/sizeof(k_res_presets[0]); i++)
        SendMessageA(hw, CB_ADDSTRING, 0, (LPARAM)k_res_presets[i].label);

    make_ctrl(ls.hwnd, "STATIC", "Display Mode:",
              WS_CHILD, 0, 44, 192, 135, 20, ID_LBL_DISPMODE, ls.fnt_ui);
    hw = make_ctrl(ls.hwnd, "COMBOBOX", "",
                   WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
                   186, 188, 300, 120, ID_CMB_DISPMODE, ls.fnt_ui);
    SendMessageA(hw, CB_ADDSTRING, 0, (LPARAM)"Windowed  (Alt+Enter / F11 toggles fullscreen)");
    SendMessageA(hw, CB_ADDSTRING, 0, (LPARAM)"Borderless Fullscreen");

    make_ctrl(ls.hwnd, "STATIC", "Aspect Ratio:",
              WS_CHILD, 0, 44, 224, 135, 20, ID_LBL_ASPECT, ls.fnt_ui);
    hw = make_ctrl(ls.hwnd, "COMBOBOX", "",
                   WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
                   186, 220, 300, 120, ID_CMB_ASPECT, ls.fnt_ui);
    SendMessageA(hw, CB_ADDSTRING, 0, (LPARAM)"4:3 Pillarbox (Original Aspect Ratio)");
    SendMessageA(hw, CB_ADDSTRING, 0, (LPARAM)"Stretch to Fill Window");

    make_ctrl(ls.hwnd, "BUTTON", "Enable VSync (reduces screen tearing)",
              WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
              186, 254, 300, 22, ID_CHK_VSYNC, ls.fnt_ui);

    make_ctrl(ls.hwnd, "BUTTON", "Direct3D 11 && Performance",
              WS_CHILD | BS_GROUPBOX, 0, 28, 294, 624, 174, ID_GRP_RENDER, ls.fnt_bold);
    make_ctrl(ls.hwnd, "BUTTON", "Hardware Direct3D 11 GPU Renderer",
              WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
              44, 316, 275, 22, ID_CHK_HW_D3D11, ls.fnt_ui);
    make_ctrl(ls.hwnd, "BUTTON", "GPU Vertex Shaders (HLSL - Fast)",
              WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
              338, 316, 290, 22, ID_CHK_GPU_VSH, ls.fnt_ui);
    make_ctrl(ls.hwnd, "BUTTON", "Projected Back-Face Culling (optional)",
              WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
              44, 342, 275, 22, ID_CHK_CULL, ls.fnt_ui);
    make_ctrl(ls.hwnd, "BUTTON", "Log GPU / VSH Telemetry to Console",
              WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
              338, 342, 290, 22, ID_CHK_PERF, ls.fnt_ui);

    make_ctrl(ls.hwnd, "STATIC", "Target Framerate:",
              WS_CHILD, 0, 44, 374, 150, 20, ID_LBL_FPS, ls.fnt_ui);
    hw = make_ctrl(ls.hwnd, "COMBOBOX", "",
                   WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
                   200, 370, 300, 120, ID_CMB_FPS, ls.fnt_ui);
    SendMessageA(hw, CB_ADDSTRING, 0, (LPARAM)"30 FPS  (Original Xbox - Recommended)");
    SendMessageA(hw, CB_ADDSTRING, 0, (LPARAM)"60 FPS  (Unlocked 60 Hz)");

    make_ctrl(ls.hwnd, "STATIC", "CPU VSH Threads:",
              WS_CHILD, 0, 44, 404, 150, 20, ID_LBL_THREADS, ls.fnt_ui);
    hw = make_ctrl(ls.hwnd, "COMBOBOX", "",
                   WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
                   200, 400, 300, 160, ID_CMB_THREADS, ls.fnt_ui);
    for (i = 0; i < sizeof(k_thread_labels)/sizeof(k_thread_labels[0]); i++)
        SendMessageA(hw, CB_ADDSTRING, 0, (LPARAM)k_thread_labels[i]);
    make_ctrl(ls.hwnd, "STATIC",
              "Internal 1280x960 (Xbox 2x2 FSAA), HLSL vs_4_0 GPU shaders, BC1-BC3 && stage-1 multitex.",
              WS_CHILD, 0, 44, 438, 594, 22, ID_LBL_GFX_NOTE, ls.fnt_ui);

    /* ── Tab 2: Audio & Controls ── */
    make_ctrl(ls.hwnd, "BUTTON", "Audio, Input && Startup",
              WS_CHILD | BS_GROUPBOX, 0, 28, 132, 624, 114, ID_GRP_AUDIO_IN, ls.fnt_bold);
    make_ctrl(ls.hwnd, "BUTTON", "Enable Xbox MCPX Audio (XAudio2 48 kHz Stereo)",
              WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
              44, 158, 420, 22, ID_CHK_AUDIO, ls.fnt_ui);
    make_ctrl(ls.hwnd, "BUTTON", "Enable Keyboard Controls alongside XInput Gamepad",
              WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
              44, 184, 420, 22, ID_CHK_KEYBOARD, ls.fnt_ui);
    make_ctrl(ls.hwnd, "BUTTON", "Skip launcher on future launches (hold Shift or pass --launcher to reopen)",
              WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
              44, 210, 560, 22, ID_CHK_SKIP, ls.fnt_ui);

    make_ctrl(ls.hwnd, "BUTTON", "Controls Quick Reference (XInput Gamepad + Keyboard)",
              WS_CHILD | BS_GROUPBOX, 0, 28, 256, 624, 212, ID_GRP_CONTROLS, ls.fnt_bold);
    make_ctrl(ls.hwnd, "EDIT",
              "  Action / Xbox Control       Keyboard Binding             Gamepad (XInput)\r\n"
              "  ----------------------------------------------------------------------------\r\n"
              "  Move Cursor / Sim           W / A / S / D                Left Stick\r\n"
              "  Rotate / Zoom Camera        I / J / K / L                Right Stick\r\n"
              "  Menus / Motives / D-Pad     Arrow Keys                   D-Pad\r\n"
              "  Select / Confirm (A)        Z  (or Space)                A Button\r\n"
              "  Cancel / Back (B)           X                            B Button\r\n"
              "  Action / Sell (X)           C                            X Button\r\n"
              "  Walls / Switch Sim (Y)      V                            Y Button\r\n"
              "  Speed / Rotate (LT / RT)    Q / E                        LT / RT\r\n"
              "  Black / White Buttons       1 / 2                        LB / RB\r\n"
              "  Pause / Buy / Build / Save  Enter (Start) / Backspace    Start / Back\r\n"
              "  Toggle Fullscreen           Alt + Enter  or  F11         --",
              WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL, WS_EX_CLIENTEDGE,
              42, 280, 596, 174, ID_EDT_CONTROLS, ls.fnt_mono);

    /* ── Bottom Action Bar ── */
    make_ctrl(ls.hwnd, "BUTTON", "Reset Defaults",
              WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,
              14, 494, 120, 32, ID_BTN_DEFAULTS, ls.fnt_ui);
    make_ctrl(ls.hwnd, "BUTTON", "Save Settings",
              WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,
              144, 494, 120, 32, ID_BTN_SAVE, ls.fnt_ui);
    make_ctrl(ls.hwnd, "BUTTON", "Play The Sims: Bustin' Out",
              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0,
              466, 494, 200, 32, ID_BTN_PLAY, ls.fnt_bold);

    if (initial_tab < 0 || initial_tab > 2) initial_tab = 0;
    ls.cur_tab = initial_tab;
    TabCtrl_SetCurSel(tab, initial_tab);
    show_tab_controls(ls.hwnd, initial_tab);
    load_ui_from_cfg(&ls);

    ShowWindow(ls.hwnd, SW_SHOW);
    UpdateWindow(ls.hwnd);

    if (capture_bmp_path && *capture_bmp_path)
        PostMessageA(ls.hwnd, WM_DO_CAPTURE, 0, 0);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(ls.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    DeleteObject(ls.fnt_ui);
    DeleteObject(ls.fnt_bold);
    DeleteObject(ls.fnt_title);
    DeleteObject(ls.fnt_sub);
    DeleteObject(ls.fnt_mono);
    DeleteCriticalSection(&ls.prog_cs);
    g_ls = NULL;
    CoUninitialize();

    return ls.result_play;
}
