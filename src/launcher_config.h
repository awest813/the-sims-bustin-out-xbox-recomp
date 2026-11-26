/*
 * Launcher Configuration & INI Persistence for The Sims: Bustin' Out
 */
#ifndef SIMS_LAUNCHER_CONFIG_H
#define SIMS_LAUNCHER_CONFIG_H

#include <windows.h>

typedef struct {
    /* [Paths] */
    char config_path[MAX_PATH];
    char game_dir[MAX_PATH];
    char iso_path[MAX_PATH];
    char save_dir[MAX_PATH];

    /* [Display] */
    int  win_width;      /* e.g. 640, 1280, 1920, 2560 */
    int  win_height;     /* e.g. 480, 960, 1440, 720, 1080, 1440 */
    int  fullscreen;     /* 0 = Windowed, 1 = Borderless Fullscreen */
    int  aspect_stretch; /* 0 = 4:3 Pillarbox (Original), 1 = Stretch to Fill */
    int  vsync;          /* 1 = On, 0 = Off */

    /* [Graphics] */
    int  software_gpu;   /* 0 = Direct3D 11 Hardware, 1 = Software CPU Fallback */
    int  gpu_vsh;        /* 1 = GPU HLSL Vertex Shaders (Default), 0 = CPU VSH */
    int  target_fps;     /* 30 = Original Xbox (Default), 60 = Unlocked 60 FPS */
    int  backface_cull;  /* 0 = Off (Default), 1 = Projected Back-Face Cull */
    int  vsh_threads;    /* 0 = Auto (up to 8), 1, 2, 4, 8 (CPU VSH fallback) */
    int  gpu_profile;    /* 0 = Off, 1 = Log D3D11/VSH FPS telemetry */

    /* [Audio] */
    int  audio_enable;   /* 1 = XAudio2 On, 0 = Muted/Disabled */

    /* [Input] */
    int  keyboard_enable;/* 1 = Gamepad + Keyboard, 0 = Gamepad Only */

    /* [Launcher] */
    int  skip_launcher;  /* 0 = Show launcher on start, 1 = Launch directly */
} LauncherConfig;

/* Initialize defaults and auto-detect base dir, game/, saves/, and nearby ISO. */
void launcher_config_defaults(LauncherConfig *cfg, const char *override_ini_path);

/* Load settings from cfg->config_path (missing keys keep defaults). */
void launcher_config_load(LauncherConfig *cfg);

/* Save settings to cfg->config_path. Returns 1 on success. */
int  launcher_config_save(const LauncherConfig *cfg);

/* Push all LauncherConfig settings into runtime environment variables. */
void launcher_config_apply_env(const LauncherConfig *cfg);

#endif /* SIMS_LAUNCHER_CONFIG_H */
