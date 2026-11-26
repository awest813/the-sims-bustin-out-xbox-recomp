/*
 * Launcher Configuration & INI Persistence for The Sims: Bustin' Out
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher_config.h"

static int dir_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

static int file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

static void path_parent(char *path)
{
    char *s = strrchr(path, '\\');
    char *f = strrchr(path, '/');
    if (f && (!s || f > s)) s = f;
    if (s) *s = '\0';
}

/* Determine a sensible project root:
 * If running from build\Release\sims_bustin_out_recomp.exe and ..\..\game or ..\..\CMakeLists.txt
 * exists, use that project root; otherwise use the executable's own directory. */
static void detect_base_dir(char *out, size_t out_sz)
{
    char exe_dir[MAX_PATH];
    char cand[MAX_PATH];
    char probe[MAX_PATH * 2];

    GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
    path_parent(exe_dir);

    /* 1) Executable directory itself already has game\default.xbe or sims_launcher.ini */
    snprintf(probe, sizeof(probe), "%s\\game\\default.xbe", exe_dir);
    if (file_exists(probe)) {
        snprintf(out, out_sz, "%s", exe_dir);
        return;
    }
    snprintf(probe, sizeof(probe), "%s\\sims_launcher.ini", exe_dir);
    if (file_exists(probe)) {
        snprintf(out, out_sz, "%s", exe_dir);
        return;
    }

    /* 2) Check 2 levels up (typical build\Release\ layout) */
    snprintf(cand, sizeof(cand), "%s", exe_dir);
    path_parent(cand);
    path_parent(cand);
    snprintf(probe, sizeof(probe), "%s\\game\\default.xbe", cand);
    if (file_exists(probe)) {
        snprintf(out, out_sz, "%s", cand);
        return;
    }
    snprintf(probe, sizeof(probe), "%s\\CMakeLists.txt", cand);
    if (file_exists(probe)) {
        snprintf(out, out_sz, "%s", cand);
        return;
    }

    /* 3) Current working directory if it has game\default.xbe */
    if (GetCurrentDirectoryA(sizeof(cand), cand) > 0) {
        snprintf(probe, sizeof(probe), "%s\\game\\default.xbe", cand);
        if (file_exists(probe)) {
            snprintf(out, out_sz, "%s", cand);
            return;
        }
    }

    /* Fallback: exe directory */
    snprintf(out, out_sz, "%s", exe_dir);
}

static int find_first_iso_in_dir(const char *dir, char *out, size_t out_sz)
{
    char pattern[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pattern, sizeof(pattern), "%s\\*.iso", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                snprintf(out, out_sz, "%s\\%s", dir, fd.cFileName);
                FindClose(h);
                return 1;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return 0;
}

static void detect_nearby_iso(const char *base_dir, char *out, size_t out_sz)
{
    char parent[MAX_PATH];
    char sub[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    out[0] = '\0';
    if (find_first_iso_in_dir(base_dir, out, out_sz))
        return;

    snprintf(parent, sizeof(parent), "%s", base_dir);
    path_parent(parent);
    if (!parent[0]) return;

    if (find_first_iso_in_dir(parent, out, out_sz))
        return;

    /* Check immediate subdirectories of parent (e.g. "..\Sims, The - Bustin' Out (USA)\*.iso") */
    snprintf(sub, sizeof(sub), "%s\\*", parent);
    h = FindFirstFileA(sub, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
             && strcmp(fd.cFileName, ".") != 0
             && strcmp(fd.cFileName, "..") != 0) {
                char child[MAX_PATH * 2];
                snprintf(child, sizeof(child), "%s\\%s", parent, fd.cFileName);
                if (find_first_iso_in_dir(child, out, out_sz)) {
                    FindClose(h);
                    return;
                }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

void launcher_config_defaults(LauncherConfig *cfg, const char *override_ini_path)
{
    char base[MAX_PATH];

    memset(cfg, 0, sizeof(*cfg));
    detect_base_dir(base, sizeof(base));

    if (override_ini_path && *override_ini_path)
        snprintf(cfg->config_path, sizeof(cfg->config_path), "%s", override_ini_path);
    else
        snprintf(cfg->config_path, sizeof(cfg->config_path), "%s\\sims_launcher.ini", base);

    snprintf(cfg->game_dir, sizeof(cfg->game_dir), "%s\\game", base);
    snprintf(cfg->save_dir, sizeof(cfg->save_dir), "%s\\saves", base);
    detect_nearby_iso(base, cfg->iso_path, sizeof(cfg->iso_path));

    cfg->win_width       = 1280;
    cfg->win_height      = 960;
    cfg->fullscreen      = 0;
    cfg->aspect_stretch  = 0;
    cfg->vsync           = 1;

    cfg->software_gpu    = 0;
    cfg->gpu_vsh         = 1;
    cfg->target_fps      = 30;
    cfg->backface_cull   = 0;
    cfg->vsh_threads     = 0;
    cfg->gpu_profile     = 0;

    cfg->audio_enable    = 1;
    cfg->keyboard_enable = 1;
    cfg->skip_launcher   = 0;
}

void launcher_config_load(LauncherConfig *cfg)
{
    char buf[MAX_PATH];
    const char *ini = cfg->config_path;

    if (file_exists(ini)) {
        GetPrivateProfileStringA("Paths", "game_dir", cfg->game_dir, buf, sizeof(buf), ini);
        if (buf[0]) snprintf(cfg->game_dir, sizeof(cfg->game_dir), "%s", buf);

        GetPrivateProfileStringA("Paths", "iso_path", cfg->iso_path, buf, sizeof(buf), ini);
        if (buf[0]) snprintf(cfg->iso_path, sizeof(cfg->iso_path), "%s", buf);

        GetPrivateProfileStringA("Paths", "save_dir", cfg->save_dir, buf, sizeof(buf), ini);
        if (buf[0]) snprintf(cfg->save_dir, sizeof(cfg->save_dir), "%s", buf);

        cfg->win_width       = (int)GetPrivateProfileIntA("Display", "width", cfg->win_width, ini);
        cfg->win_height      = (int)GetPrivateProfileIntA("Display", "height", cfg->win_height, ini);
        if (cfg->win_width < 320 || cfg->win_width > 7680)   cfg->win_width  = 1280;
        if (cfg->win_height < 240 || cfg->win_height > 4320) cfg->win_height = 960;

        cfg->fullscreen      = GetPrivateProfileIntA("Display", "fullscreen", cfg->fullscreen, ini) ? 1 : 0;
        cfg->aspect_stretch  = GetPrivateProfileIntA("Display", "aspect_stretch", cfg->aspect_stretch, ini) ? 1 : 0;
        cfg->vsync           = GetPrivateProfileIntA("Display", "vsync", cfg->vsync, ini) ? 1 : 0;

        cfg->software_gpu    = GetPrivateProfileIntA("Graphics", "software_gpu", cfg->software_gpu, ini) ? 1 : 0;
        cfg->gpu_vsh         = GetPrivateProfileIntA("Graphics", "gpu_vsh", cfg->gpu_vsh, ini) ? 1 : 0;
        cfg->target_fps      = (int)GetPrivateProfileIntA("Graphics", "target_fps", cfg->target_fps, ini);
        if (cfg->target_fps != 30 && cfg->target_fps != 60) cfg->target_fps = 30;
        cfg->backface_cull   = GetPrivateProfileIntA("Graphics", "backface_cull", cfg->backface_cull, ini) ? 1 : 0;
        cfg->vsh_threads     = (int)GetPrivateProfileIntA("Graphics", "vsh_threads", cfg->vsh_threads, ini);
        if (cfg->vsh_threads < 0 || cfg->vsh_threads > 8) cfg->vsh_threads = 0;
        cfg->gpu_profile     = GetPrivateProfileIntA("Graphics", "gpu_profile", cfg->gpu_profile, ini) ? 1 : 0;

        cfg->audio_enable    = GetPrivateProfileIntA("Audio", "audio_enable", cfg->audio_enable, ini) ? 1 : 0;
        cfg->keyboard_enable = GetPrivateProfileIntA("Input", "keyboard_enable", cfg->keyboard_enable, ini) ? 1 : 0;
        cfg->skip_launcher   = GetPrivateProfileIntA("Launcher", "skip_launcher", cfg->skip_launcher, ini) ? 1 : 0;
    }

    /* Ensure save_dir exists or is sensible */
    if (!dir_exists(cfg->save_dir))
        CreateDirectoryA(cfg->save_dir, NULL);
}

static int write_ini_int(const char *sec, const char *key, int val, const char *ini)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    return WritePrivateProfileStringA(sec, key, buf, ini) ? 1 : 0;
}

int launcher_config_save(const LauncherConfig *cfg)
{
    const char *ini = cfg->config_path;
    int ok = 1;

    ok &= WritePrivateProfileStringA("Paths", "game_dir", cfg->game_dir, ini) ? 1 : 0;
    ok &= WritePrivateProfileStringA("Paths", "iso_path", cfg->iso_path, ini) ? 1 : 0;
    ok &= WritePrivateProfileStringA("Paths", "save_dir", cfg->save_dir, ini) ? 1 : 0;

    ok &= write_ini_int("Display", "width", cfg->win_width, ini);
    ok &= write_ini_int("Display", "height", cfg->win_height, ini);
    ok &= write_ini_int("Display", "fullscreen", cfg->fullscreen, ini);
    ok &= write_ini_int("Display", "aspect_stretch", cfg->aspect_stretch, ini);
    ok &= write_ini_int("Display", "vsync", cfg->vsync, ini);

    ok &= write_ini_int("Graphics", "software_gpu", cfg->software_gpu, ini);
    ok &= write_ini_int("Graphics", "gpu_vsh", cfg->gpu_vsh, ini);
    ok &= write_ini_int("Graphics", "target_fps", cfg->target_fps, ini);
    ok &= write_ini_int("Graphics", "backface_cull", cfg->backface_cull, ini);
    ok &= write_ini_int("Graphics", "vsh_threads", cfg->vsh_threads, ini);
    ok &= write_ini_int("Graphics", "gpu_profile", cfg->gpu_profile, ini);

    ok &= write_ini_int("Audio", "audio_enable", cfg->audio_enable, ini);
    ok &= write_ini_int("Input", "keyboard_enable", cfg->keyboard_enable, ini);
    ok &= write_ini_int("Launcher", "skip_launcher", cfg->skip_launcher, ini);

    return ok;
}

static void set_env_if_unset(const char *name, const char *val)
{
    if (!getenv(name))
        _putenv_s(name, val);
}

void launcher_config_apply_env(const LauncherConfig *cfg)
{
    char buf[32];

    if (cfg->save_dir[0])
        set_env_if_unset("RECOMP_SAVE_DIR", cfg->save_dir);

    snprintf(buf, sizeof(buf), "%d", cfg->win_width);
    set_env_if_unset("RECOMP_WINDOW_WIDTH", buf);
    snprintf(buf, sizeof(buf), "%d", cfg->win_height);
    set_env_if_unset("RECOMP_WINDOW_HEIGHT", buf);

    set_env_if_unset("RECOMP_FULLSCREEN",     cfg->fullscreen ? "1" : "0");
    set_env_if_unset("RECOMP_ASPECT_STRETCH", cfg->aspect_stretch ? "1" : "0");
    set_env_if_unset("RECOMP_VSYNC",          cfg->vsync ? "1" : "0");

    if (cfg->software_gpu)
        set_env_if_unset("RECOMP_SOFTWARE_GPU", "1");
    if (!cfg->gpu_vsh)
        set_env_if_unset("RECOMP_CPU_VSH", "1");
    if (cfg->target_fps == 60)
        set_env_if_unset("RECOMP_MAX_SWAP_INTERVAL", "1");
    set_env_if_unset("RECOMP_D3D11_CULL", cfg->backface_cull ? "1" : "0");
    if (cfg->vsh_threads > 0) {
        snprintf(buf, sizeof(buf), "%d", cfg->vsh_threads);
        set_env_if_unset("RECOMP_VSH_THREADS", buf);
    }
    if (cfg->gpu_profile)
        set_env_if_unset("RECOMP_GPU_PROFILE", "1");

    /* Keep the emulated MCPX APU / DirectSound handshake active so the title
     * never crashes or stalls on audio init; when disabled, mute host output. */
    set_env_if_unset("RECOMP_AC97_READY", "1");
    if (!cfg->audio_enable)
        set_env_if_unset("RECOMP_AUDIO_MUTE", "1");

    if (cfg->keyboard_enable)
        set_env_if_unset("RECOMP_KEYBOARD", "1");
    else
        _putenv_s("RECOMP_KEYBOARD", "0");
}
