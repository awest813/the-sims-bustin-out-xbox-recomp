/*
 * Native Xbox XISO (XDVDFS) Inspector & Extractor for Windows
 *
 * Supports both raw XISO images (partition at 0x0) and Redump/retail dual-layer
 * DVD images (game partition at 0x18300000, 0x0FD90000, or 0x02080000).
 * Uses 64-bit Win32 file I/O so >4 GB ISOs work cleanly.
 */
#ifndef SIMS_XISO_EXTRACT_H
#define SIMS_XISO_EXTRACT_H

#include <stdint.h>

#define SIMS_EXPECTED_TITLE_ID 0x45410043u /* EA - The Sims: Bustin' Out */

typedef struct {
    int      valid_xiso;
    int      has_default_xbe;
    int      title_id_matches;
    uint64_t partition_offset;
    uint32_t title_id;
    char     title_name[128];
    uint32_t file_count;
    uint32_t dir_count;
    uint64_t total_bytes;
    char     error[256];
} XisoInfo;

typedef struct {
    int      valid;
    int      title_id_matches;
    int      has_data_dir;
    uint32_t title_id;
    char     title_name[128];
    char     status[256];
} GameDirInfo;

typedef void (*XisoProgressFn)(uint32_t files_done, uint32_t files_total,
                               uint64_t bytes_done, uint64_t bytes_total,
                               const char *current_file, void *user_data);

/* Inspect an Xbox ISO without modifying it. Returns 1 if a valid XISO partition was found. */
int xiso_inspect(const char *iso_path, XisoInfo *out_info);

/* Extract all files from an Xbox ISO into out_dir (creating subdirectories as needed).
 * If cancel_flag is non-NULL and becomes non-zero, extraction stops cleanly and returns -1.
 * Returns 1 on success, 0 on error, -1 on cancel. */
int xiso_extract_all(const char *iso_path, const char *out_dir,
                     XisoProgressFn progress_cb, void *user_data,
                     volatile long *cancel_flag,
                     char *err_buf, size_t err_buf_sz);

/* Validate an extracted game directory (checks default.xbe, Title ID, and data/). */
int xiso_validate_game_dir(const char *game_dir, GameDirInfo *out_info);

#endif /* SIMS_XISO_EXTRACT_H */
