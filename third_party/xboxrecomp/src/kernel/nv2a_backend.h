/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState.
 *
 * ponytail: register-combiner state and texture stages 1-3 are not passed on:
 * a back end sees stage 0 and the diffuse colour. Extend Nv2aBatch when that
 * matters rather than adding callbacks.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t clip_x, clip_y;    /* clip rectangle origin, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
    uint32_t surface_format;    /* raw NV097_SET_SURFACE_FORMAT (0x0208) */
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
    uint32_t palette;           /* guest address of CLUT for P8 (0x0B) */
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* stage-0 texels */
    uint32_t specular;          /* 0xAARRGGBB (oD1 / secondary colour) */
    float    u1, v1;            /* stage-1 texels */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
} Nv2aRenderState;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* stage 0; NULL: untextured */
    const Nv2aTexture *texture1;    /* stage 1; NULL: unused */
    const Nv2aRenderState *state;
    const void        *combiner;    /* const Nv2aCombiner *, or NULL */
    uint32_t           fog_color;   /* SET_FOG_COLOR (0x02A8), 0xAABBGGRR/ARGB */
} Nv2aBatch;

typedef struct {
    const uint8_t *base;        /* host pointer to vertex 0, or NULL if immediate */
    uint32_t       stride;      /* bytes per vertex */
    uint32_t       type;        /* NV2A attr type: 0=D3DCOLOR, 1=SNORM16, 2=FLOAT, 4=UNORM8, 5=SINT16, 6=CMP11:11:10 */
    uint32_t       size;        /* 0..4 components (0 = immediate) */
    uint32_t       max_bytes;   /* upper bound of readable bytes from base (0 = guest RAM unchecked) */
    float          imm[4];      /* immediate attribute value when size == 0 */
} Nv2aVshAttr;

typedef struct {
    const uint32_t (*program)[4];  /* active NV2A microcode words [0..insn_count-1] */
    uint32_t         insn_count;
    uint32_t         in_mask;      /* bitmask of v0..v15 read by program */
    uint64_t         prog_hash;    /* FNV-1a hash of active microcode */
    const float    (*constants)[4];/* 192 float4 constants c[0..191] */
    Nv2aVshAttr      attrs[16];
    const uint32_t  *indices;
    uint32_t         index_count;
    uint32_t         prim_type;    /* 5=TRIANGLES, 6=TRISTRIP, 7=TRIFAN, 8=QUADS, 9=QUADSTRIP, 10=POLYGON */
    int              strip_alt_winding;
    const Nv2aTexture *texture;    /* stage 0; NULL: untextured */
    const Nv2aTexture *texture1;   /* stage 1; NULL: unused */
    int              tex0_normalized;
    int              tex1_normalized;
    const Nv2aRenderState *state;
    const void      *combiner;     /* const Nv2aCombiner *, or NULL */
    uint32_t         fog_color;
} Nv2aVshBatch;

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
    /* Optional GPU-side vertex shader execution. Returns 1 if handled, 0 to fall back to CPU VSH. */
    int  (*draw_vsh)(const Nv2aSurface *s, const Nv2aVshBatch *b);
    /* Optional: called at the end of a pushbuffer walk so a GPU backend can
     * sync small off-screen render targets (e.g. 64x64 thumbnails) back to
     * guest memory before a waiting CPU thread reads them. */
    void (*sync_rt)(void);
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Register a back end that must synchronize a surface into guest memory
 * before the flip's window snapshot or framebuffer dump. present_sync runs
 * synchronously on the executor thread and must complete pending GPU work and
 * readback before returning; it must not present a window. flip still runs
 * after the snapshot and dump. A NULL present_sync disables synchronization;
 * a NULL backend removes both registrations. A later nv2a_backend_register
 * call replaces both registrations. */
void nv2a_backend_register_with_present_sync(const Nv2aBackend *backend,
                                             void (*present_sync)(void));

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#endif /* NV2A_BACKEND_H */
