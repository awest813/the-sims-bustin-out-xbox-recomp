/*
 * Native Xbox XISO (XDVDFS) Inspector & Extractor for Windows
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "xiso_extract.h"

#define XISO_SECTOR_SIZE 2048ULL
#define XISO_MAGIC       "MICROSOFT*XBOX*MEDIA"
#define XISO_MAGIC_LEN   20

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int file_read_at(HANDLE hf, uint64_t offset, void *buf, uint32_t size)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(hf, li, NULL, FILE_BEGIN))
        return 0;
    if (!ReadFile(hf, buf, size, &got, NULL) || got != size)
        return 0;
    return 1;
}

static uint64_t file_get_size(HANDLE hf)
{
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(hf, &sz) || sz.QuadPart < 0)
        return 0;
    return (uint64_t)sz.QuadPart;
}

static int parse_xbe_cert(const uint8_t *hdr, size_t hdr_len,
                          uint32_t *out_title_id, char *out_name, size_t out_name_sz)
{
    uint32_t base_addr, cert_addr, cert_off;
    size_t i, o = 0;

    if (hdr_len < 0x180 || memcmp(hdr, "XBEH", 4) != 0)
        return 0;
    base_addr = rd_u32(hdr + 0x104);
    cert_addr = rd_u32(hdr + 0x118);
    if (cert_addr < base_addr)
        return 0;
    cert_off = cert_addr - base_addr;
    if ((size_t)cert_off + 0x60 > hdr_len)
        return 0;

    *out_title_id = rd_u32(hdr + cert_off + 0x08);
    for (i = 0; i < 40 && o + 1 < out_name_sz; i++) {
        uint16_t ch = rd_u16(hdr + cert_off + 0x0C + i * 2);
        if (ch == 0) break;
        out_name[o++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
    }
    out_name[o] = '\0';
    if (!out_name[0] && *out_title_id == SIMS_EXPECTED_TITLE_ID)
        snprintf(out_name, out_name_sz, "The Sims: Bustin' Out");
    return 1;
}

static int find_xiso_partition(HANDLE hf, uint64_t file_size,
                               uint64_t *out_base,
                               uint32_t *out_root_sector,
                               uint32_t *out_root_size)
{
    static const uint64_t k_bases[] = {
        0x00000000ULL, /* Raw XISO */
        0x18300000ULL, /* Redump standard retail DVD9 (387 MB video partition) */
        0x0FD90000ULL, /* XGD2 / alternate offset */
        0x02080000ULL  /* XGD3 / alternate offset */
    };
    uint8_t sec[2048];
    int i;

    for (i = 0; i < (int)(sizeof(k_bases) / sizeof(k_bases[0])); i++) {
        uint64_t base = k_bases[i];
        if (base + 32ULL * XISO_SECTOR_SIZE + XISO_SECTOR_SIZE > file_size)
            continue;
        if (!file_read_at(hf, base + 32ULL * XISO_SECTOR_SIZE, sec, sizeof(sec)))
            continue;
        if (memcmp(sec, XISO_MAGIC, XISO_MAGIC_LEN) == 0
         && memcmp(sec + 0x7EC, XISO_MAGIC, XISO_MAGIC_LEN) == 0) {
            *out_base        = base;
            *out_root_sector = rd_u32(sec + 20);
            *out_root_size   = rd_u32(sec + 24);
            return 1;
        }
    }
    return 0;
}

static int sanitize_name(const char *name, size_t len)
{
    size_t i;
    if (len == 0 || len > 250) return 0;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
         || c == '"' || c == '<' || c == '>' || c == '|')
            return 0;
    }
    return 1;
}

static int ensure_dirs_recursive(const char *path)
{
    char tmp[MAX_PATH * 2];
    size_t len, i;

    if (!path || !*path) return 0;
    len = strlen(path);
    if (len >= sizeof(tmp)) return 0;
    memcpy(tmp, path, len + 1);

    for (i = 0; i < len; i++) {
        if (tmp[i] == '/') tmp[i] = '\\';
    }
    for (i = 1; i < len; i++) {
        if (tmp[i] == '\\') {
            if (i == 2 && tmp[1] == ':') continue; /* drive root like C:\ */
            tmp[i] = '\0';
            CreateDirectoryA(tmp, NULL);
            tmp[i] = '\\';
        }
    }
    if (!CreateDirectoryA(tmp, NULL)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS)
            return 0;
    }
    return 1;
}

typedef struct {
    HANDLE         hf;
    uint64_t       file_size;
    uint64_t       part_base;
    int            do_extract;
    const char    *out_root;
    XisoInfo      *info;
    uint32_t       files_done;
    uint32_t       files_total;
    uint64_t       bytes_done;
    uint64_t       bytes_total;
    XisoProgressFn progress_cb;
    void          *user_data;
    volatile long *cancel_flag;
    uint8_t       *copy_buf;
    uint32_t       copy_buf_sz;
    char           err[256];
    int            cancelled;
    int            failed;
} WalkCtx;

static void walk_dir(WalkCtx *ctx, uint32_t dir_sector, uint32_t dir_size,
                     const char *rel_path, int depth);

static void walk_node(WalkCtx *ctx, const uint8_t *table, uint32_t table_size,
                      uint32_t off, const char *rel_path, int depth, int node_depth)
{
    uint16_t left_dw, right_dw;
    uint32_t sector, size;
    uint8_t attr, nlen;
    char name[260];
    char child_rel[MAX_PATH * 2];

    if (ctx->failed || ctx->cancelled) return;
    if (ctx->cancel_flag && *ctx->cancel_flag) {
        ctx->cancelled = 1;
        return;
    }
    if (node_depth > 256) return;
    if (off + 14u > table_size) return;

    left_dw  = rd_u16(table + off);
    right_dw = rd_u16(table + off + 2);
    sector   = rd_u32(table + off + 4);
    size     = rd_u32(table + off + 8);
    attr     = table[off + 12];
    nlen     = table[off + 13];

    if (left_dw == 0xFFFFu && right_dw == 0xFFFFu)
        return;
    if (off + 14u + nlen > table_size)
        return;

    if (left_dw != 0 && left_dw != 0xFFFFu) {
        uint32_t loff = (uint32_t)left_dw * 4u;
        if (loff != off && loff < table_size)
            walk_node(ctx, table, table_size, loff, rel_path, depth, node_depth + 1);
    }

    if (!ctx->failed && !ctx->cancelled && sanitize_name((const char *)(table + off + 14), nlen)) {
        memcpy(name, table + off + 14, nlen);
        name[nlen] = '\0';
        if (rel_path && *rel_path)
            snprintf(child_rel, sizeof(child_rel), "%s\\%s", rel_path, name);
        else
            snprintf(child_rel, sizeof(child_rel), "%s", name);

        if (attr & 0x10u) {
            if (ctx->info) ctx->info->dir_count++;
            if (ctx->do_extract) {
                char full_dir[MAX_PATH * 2];
                snprintf(full_dir, sizeof(full_dir), "%s\\%s", ctx->out_root, child_rel);
                ensure_dirs_recursive(full_dir);
            }
            if (size > 0 && depth < 32)
                walk_dir(ctx, sector, size, child_rel, depth + 1);
        } else {
            uint64_t data_off = ctx->part_base + (uint64_t)sector * XISO_SECTOR_SIZE;
            if (data_off + size <= ctx->file_size) {
                if (ctx->info) {
                    ctx->info->file_count++;
                    ctx->info->total_bytes += size;
                    if ((!rel_path || !*rel_path) && _stricmp(name, "default.xbe") == 0) {
                        uint8_t xbe_hdr[4096];
                        uint32_t to_read = size < sizeof(xbe_hdr) ? size : (uint32_t)sizeof(xbe_hdr);
                        ctx->info->has_default_xbe = 1;
                        if (file_read_at(ctx->hf, data_off, xbe_hdr, to_read)) {
                            if (parse_xbe_cert(xbe_hdr, to_read,
                                               &ctx->info->title_id,
                                               ctx->info->title_name,
                                               sizeof(ctx->info->title_name))) {
                                ctx->info->title_id_matches =
                                    (ctx->info->title_id == SIMS_EXPECTED_TITLE_ID) ? 1 : 0;
                            }
                        }
                    }
                }
                if (ctx->do_extract) {
                    char full_path[MAX_PATH * 2];
                    HANDLE out_hf;
                    uint64_t rem = size;
                    uint64_t cur = data_off;

                    snprintf(full_path, sizeof(full_path), "%s\\%s", ctx->out_root, child_rel);
                    if (ctx->progress_cb) {
                        ctx->progress_cb(ctx->files_done, ctx->files_total,
                                         ctx->bytes_done, ctx->bytes_total,
                                         child_rel, ctx->user_data);
                    }
                    out_hf = CreateFileA(full_path, GENERIC_WRITE, 0, NULL,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (out_hf == INVALID_HANDLE_VALUE) {
                        snprintf(ctx->err, sizeof(ctx->err),
                                 "Failed to create output file: %s", child_rel);
                        ctx->failed = 1;
                    } else {
                        LARGE_INTEGER li;
                        li.QuadPart = (LONGLONG)cur;
                        if (!SetFilePointerEx(ctx->hf, li, NULL, FILE_BEGIN)) {
                            snprintf(ctx->err, sizeof(ctx->err),
                                     "Seek error extracting %s", child_rel);
                            ctx->failed = 1;
                        }
                        while (rem > 0 && !ctx->failed && !ctx->cancelled) {
                            DWORD chunk = (rem > ctx->copy_buf_sz) ? ctx->copy_buf_sz : (DWORD)rem;
                            DWORD got = 0, wrote = 0;
                            if (ctx->cancel_flag && *ctx->cancel_flag) {
                                ctx->cancelled = 1;
                                break;
                            }
                            if (!ReadFile(ctx->hf, ctx->copy_buf, chunk, &got, NULL) || got != chunk) {
                                snprintf(ctx->err, sizeof(ctx->err),
                                         "Read error extracting %s", child_rel);
                                ctx->failed = 1;
                                break;
                            }
                            if (!WriteFile(out_hf, ctx->copy_buf, chunk, &wrote, NULL) || wrote != chunk) {
                                snprintf(ctx->err, sizeof(ctx->err),
                                         "Write error extracting %s (disk full?)", child_rel);
                                ctx->failed = 1;
                                break;
                            }
                            rem -= chunk;
                            ctx->bytes_done += chunk;
                            if (ctx->progress_cb) {
                                ctx->progress_cb(ctx->files_done, ctx->files_total,
                                                 ctx->bytes_done, ctx->bytes_total,
                                                 child_rel, ctx->user_data);
                            }
                        }
                        CloseHandle(out_hf);
                        if (!ctx->failed && !ctx->cancelled) {
                            ctx->files_done++;
                        } else {
                            DeleteFileA(full_path);
                        }
                    }
                }
            }
        }
    }

    if (!ctx->failed && !ctx->cancelled && right_dw != 0 && right_dw != 0xFFFFu) {
        uint32_t roff = (uint32_t)right_dw * 4u;
        if (roff != off && roff < table_size)
            walk_node(ctx, table, table_size, roff, rel_path, depth, node_depth + 1);
    }
}

static void walk_dir(WalkCtx *ctx, uint32_t dir_sector, uint32_t dir_size,
                     const char *rel_path, int depth)
{
    uint64_t off;
    uint8_t *table;

    if (ctx->failed || ctx->cancelled) return;
    if (dir_size == 0 || dir_size > 16u * 1024u * 1024u) return;

    off = ctx->part_base + (uint64_t)dir_sector * XISO_SECTOR_SIZE;
    if (off + dir_size > ctx->file_size) return;

    table = (uint8_t *)malloc(dir_size);
    if (!table) {
        snprintf(ctx->err, sizeof(ctx->err), "Out of memory reading directory table");
        ctx->failed = 1;
        return;
    }
    if (!file_read_at(ctx->hf, off, table, dir_size)) {
        free(table);
        snprintf(ctx->err, sizeof(ctx->err), "Failed to read directory table at sector %u", dir_sector);
        ctx->failed = 1;
        return;
    }

    walk_node(ctx, table, dir_size, 0, rel_path, depth, 0);
    free(table);
}

int xiso_inspect(const char *iso_path, XisoInfo *out_info)
{
    HANDLE hf;
    uint64_t file_size, part_base = 0;
    uint32_t root_sector = 0, root_size = 0;
    WalkCtx ctx;

    memset(out_info, 0, sizeof(*out_info));
    if (!iso_path || !*iso_path) {
        snprintf(out_info->error, sizeof(out_info->error), "No ISO path specified.");
        return 0;
    }

    hf = CreateFileA(iso_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        snprintf(out_info->error, sizeof(out_info->error), "Cannot open ISO file.");
        return 0;
    }

    file_size = file_get_size(hf);
    if (!find_xiso_partition(hf, file_size, &part_base, &root_sector, &root_size)) {
        CloseHandle(hf);
        snprintf(out_info->error, sizeof(out_info->error),
                 "Not a valid Original Xbox ISO (MICROSOFT*XBOX*MEDIA header not found).");
        return 0;
    }

    out_info->valid_xiso       = 1;
    out_info->partition_offset = part_base;

    memset(&ctx, 0, sizeof(ctx));
    ctx.hf        = hf;
    ctx.file_size = file_size;
    ctx.part_base = part_base;
    ctx.info      = out_info;

    walk_dir(&ctx, root_sector, root_size, "", 0);
    CloseHandle(hf);

    if (ctx.failed) {
        snprintf(out_info->error, sizeof(out_info->error), "%s", ctx.err);
        return 0;
    }
    return 1;
}

int xiso_extract_all(const char *iso_path, const char *out_dir,
                     XisoProgressFn progress_cb, void *user_data,
                     volatile long *cancel_flag,
                     char *err_buf, size_t err_buf_sz)
{
    XisoInfo info;
    HANDLE hf;
    uint64_t file_size, part_base = 0;
    uint32_t root_sector = 0, root_size = 0;
    WalkCtx ctx;

    if (err_buf && err_buf_sz) err_buf[0] = '\0';
    if (!xiso_inspect(iso_path, &info)) {
        if (err_buf && err_buf_sz)
            snprintf(err_buf, err_buf_sz, "%s", info.error);
        return 0;
    }
    if (!ensure_dirs_recursive(out_dir)) {
        if (err_buf && err_buf_sz)
            snprintf(err_buf, err_buf_sz, "Failed to create destination folder: %s", out_dir);
        return 0;
    }

    hf = CreateFileA(iso_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        if (err_buf && err_buf_sz)
            snprintf(err_buf, err_buf_sz, "Cannot reopen ISO file for extraction.");
        return 0;
    }

    file_size = file_get_size(hf);
    if (!find_xiso_partition(hf, file_size, &part_base, &root_sector, &root_size)) {
        CloseHandle(hf);
        if (err_buf && err_buf_sz)
            snprintf(err_buf, err_buf_sz, "Failed to locate XISO partition.");
        return 0;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.hf          = hf;
    ctx.file_size   = file_size;
    ctx.part_base   = part_base;
    ctx.do_extract  = 1;
    ctx.out_root    = out_dir;
    ctx.files_total = info.file_count;
    ctx.bytes_total = info.total_bytes;
    ctx.progress_cb = progress_cb;
    ctx.user_data   = user_data;
    ctx.cancel_flag = cancel_flag;
    ctx.copy_buf_sz = 1024u * 1024u;
    ctx.copy_buf    = (uint8_t *)malloc(ctx.copy_buf_sz);
    if (!ctx.copy_buf) {
        CloseHandle(hf);
        if (err_buf && err_buf_sz)
            snprintf(err_buf, err_buf_sz, "Out of memory allocating extraction buffer.");
        return 0;
    }

    walk_dir(&ctx, root_sector, root_size, "", 0);

    free(ctx.copy_buf);
    CloseHandle(hf);

    if (ctx.cancelled) {
        if (err_buf && err_buf_sz)
            snprintf(err_buf, err_buf_sz, "Extraction cancelled by user.");
        return -1;
    }
    if (ctx.failed) {
        if (err_buf && err_buf_sz)
            snprintf(err_buf, err_buf_sz, "%s", ctx.err);
        return 0;
    }
    if (progress_cb) {
        progress_cb(ctx.files_done, ctx.files_total,
                    ctx.bytes_total, ctx.bytes_total,
                    "Extraction complete!", user_data);
    }
    return 1;
}

int xiso_validate_game_dir(const char *game_dir, GameDirInfo *out_info)
{
    char xbe_path[MAX_PATH * 2];
    HANDLE hf;
    uint8_t buf[4096];
    DWORD got = 0;

    memset(out_info, 0, sizeof(*out_info));
    if (!game_dir || !*game_dir) {
        snprintf(out_info->status, sizeof(out_info->status), "No game folder selected.");
        return 0;
    }

    snprintf(xbe_path, sizeof(xbe_path), "%s\\default.xbe", game_dir);
    hf = CreateFileA(xbe_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        snprintf(out_info->status, sizeof(out_info->status),
                 "Missing default.xbe in folder. Pick an ISO to extract or browse to extracted files.");
        return 0;
    }

    if (!ReadFile(hf, buf, sizeof(buf), &got, NULL) || got < 0x180) {
        CloseHandle(hf);
        snprintf(out_info->status, sizeof(out_info->status), "default.xbe is unreadable or truncated.");
        return 0;
    }
    CloseHandle(hf);

    if (!parse_xbe_cert(buf, got, &out_info->title_id,
                        out_info->title_name, sizeof(out_info->title_name))) {
        snprintf(out_info->status, sizeof(out_info->status), "default.xbe header is invalid.");
        return 0;
    }

    /* Count extracted files in game_dir (The Sims: Bustin' Out stores its 60 .arc/.xmv/.bnk files in root) */
    {
        char pat[MAX_PATH * 2];
        WIN32_FIND_DATAA fd;
        HANDLE hfind;
        uint32_t fcnt = 0;
        snprintf(pat, sizeof(pat), "%s\\*", game_dir);
        hfind = FindFirstFileA(pat, &fd);
        if (hfind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    fcnt++;
            } while (FindNextFileA(hfind, &fd));
            FindClose(hfind);
        }
        out_info->has_data_dir = (fcnt >= 5) ? 1 : 0;
    }

    out_info->valid = 1;
    out_info->title_id_matches = (out_info->title_id == SIMS_EXPECTED_TITLE_ID) ? 1 : 0;

    if (!out_info->title_id_matches) {
        snprintf(out_info->status, sizeof(out_info->status),
                 "Warning: Found '%s' (Title ID 0x%08X), expected 0x%08X (The Sims: Bustin' Out).",
                 out_info->title_name[0] ? out_info->title_name : "Unknown",
                 out_info->title_id, SIMS_EXPECTED_TITLE_ID);
    } else if (!out_info->has_data_dir) {
        snprintf(out_info->status, sizeof(out_info->status),
                 "Found '%s' (0x%08X), but game asset archives are missing. Extract the ISO first.",
                 out_info->title_name, out_info->title_id);
        out_info->valid = 0;
    } else {
        snprintf(out_info->status, sizeof(out_info->status),
                 "Ready: %s  |  Title ID: 0x%08X  |  Game assets verified",
                 out_info->title_name[0] ? out_info->title_name : "The Sims: Bustin' Out",
                 out_info->title_id);
    }
    return out_info->valid;
}
