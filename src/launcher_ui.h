/*
 * Native Win32 Launcher UI (ISO Picker, XISO Extractor, Settings)
 * for The Sims: Bustin' Out
 */
#ifndef SIMS_LAUNCHER_UI_H
#define SIMS_LAUNCHER_UI_H

#include "launcher_config.h"

/* Show the modal launcher window.
 *  - Loads/edits settings in *cfg and persists them when clicking Save or Play.
 *  - If capture_bmp_path is non-NULL, renders the launcher, saves a screenshot
 *    BMP to capture_bmp_path, and returns 0 (used for automated UI verification).
 * Returns 1 if the user clicked "Play Game", 0 if closed/cancelled. */
int launcher_ui_show(HINSTANCE hInst, LauncherConfig *cfg,
                     const char *capture_bmp_path, int initial_tab);

#endif /* SIMS_LAUNCHER_UI_H */
