/*
 * Native Direct3D 11 hardware render backend for The Sims: Bustin' Out
 *
 * Implements xboxrecomp's Nv2aBackend interface (clear, draw, flip, plus
 * optional present_sync when RECOMP_FB_DUMP is active).
 *
 * Key features:
 *  - Lazy Win32 window + D3D11 device/swapchain on the NV2A poll thread
 *  - Full Win32 keyboard forwarding into xboxrecomp's keyboard pad layer
 *    (RECOMP_KEYBOARD=1), plus Alt+Enter / F11 fullscreen toggle
 *  - Render-target & Z24S8 depth-stencil cache keyed by guest VA, enabling
 *    zero-copy GPU-side 4x MSAA resolve & render-to-texture
 *  - 512-entry LRU texture cache with fast content + P8 CLUT hashing
 *    (handles DXT1/3/5, P8 palettized, swizzled A8B8G8R8, linear XMV frames)
 *  - Full 8-stage + final-stage NV2A register combiner evaluation in HLSL
 *  - Per-triangle projected-space culling (matching NV2A), full blend/depth/
 *    stencil/alpha-test state caching
 */

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "nv2a_d3d11_backend.h"
#include "nv2a_vsh_hlsl.h"
#include "nv2a_backend.h"
#include "nv2a_combiner.h"

/* Runtime hooks from xboxrecomp */
extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_InputKeySet(int vk, int down);
extern int  xbox_InputWin32SideVk(int vk, uint32_t lparam);

/* ── GL / NV2A constants ───────────────────────────────────── */

#define GL_NEVER              0x0200
#define GL_LESS               0x0201
#define GL_EQUAL              0x0202
#define GL_LEQUAL             0x0203
#define GL_GREATER            0x0204
#define GL_NOTEQUAL           0x0205
#define GL_GEQUAL             0x0206
#define GL_ALWAYS             0x0207

#define GL_ZERO               0x0000
#define GL_ONE                0x0001
#define GL_SRC_COLOR          0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA          0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA          0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR          0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_CONSTANT_COLOR     0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define GL_CONSTANT_ALPHA     0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004

#define GL_FUNC_ADD           0x8006
#define GL_MIN                0x8007
#define GL_MAX                0x8008
#define GL_FUNC_SUBTRACT      0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B

#define GL_KEEP               0x1E00
#define GL_REPLACE            0x1E01
#define GL_INCR               0x1E02
#define GL_DECR               0x1E03
#define GL_INVERT             0x150A
#define GL_INCR_WRAP          0x8507
#define GL_DECR_WRAP          0x8508

#define GL_FRONT              0x0404
#define GL_BACK               0x0405
#define GL_FRONT_AND_BACK     0x0408
#define GL_CW                 0x0900
#define GL_CCW                0x0901

/* ── HLSL Shaders ──────────────────────────────────────────── */

static const char k_main_vs_hlsl[] =
"cbuffer VsCB : register(b0) {\n"
"    float2 inv_rt_size;\n"
"    float2 inv_tex_size;\n"
"    float2 inv_tex1_size;\n"
"    float  z_scale;\n"
"    float  _pad;\n"
"};\n"
"struct VSIn {\n"
"    float4 pos  : POSITION;\n"
"    float4 col  : COLOR0;\n"
"    float2 uv   : TEXCOORD0;\n"
"    float4 spec : COLOR1;\n"
"    float2 uv1  : TEXCOORD1;\n"
"};\n"
"struct VSOut {\n"
"    float4 pos  : SV_Position;\n"
"    float4 col  : COLOR0;\n"
"    float2 uv   : TEXCOORD0;\n"
"    float4 spec : COLOR1;\n"
"    float2 uv1  : TEXCOORD1;\n"
"};\n"
"VSOut main(VSIn i) {\n"
"    VSOut o;\n"
"    float ndc_x = i.pos.x * inv_rt_size.x * 2.0f - 1.0f;\n"
"    float ndc_y = 1.0f - i.pos.y * inv_rt_size.y * 2.0f;\n"
"    float ndc_z = saturate(i.pos.z * z_scale);\n"
"    float w = (i.pos.w > 1e-6f && i.pos.w < 1e6f) ? (1.0f / i.pos.w) : 1.0f;\n"
"    o.pos  = float4(ndc_x * w, ndc_y * w, ndc_z * w, w);\n"
"    o.col  = i.col;\n"
"    o.uv   = i.uv * inv_tex_size;\n"
"    o.spec = i.spec;\n"
"    o.uv1  = i.uv1 * inv_tex1_size;\n"
"    return o;\n"
"}\n";

static const char k_main_ps_hlsl[] =
"Texture2D    tex0 : register(t0);\n"
"Texture2D    tex1 : register(t1);\n"
"SamplerState smp0 : register(s0);\n"
"SamplerState smp1 : register(s1);\n"
"\n"
"cbuffer PsCB : register(b0) {\n"
"    uint4  stages[8];      /* x=c_icw, y=c_ocw, z=a_icw, w=a_ocw */\n"
"    float4 factor0[8];\n"
"    float4 factor1[8];\n"
"    float4 final_c0;\n"
"    float4 final_c1;\n"
"    float4 fog_col;\n"
"    uint4  rc_meta;        /* x=control, y=final0, z=final1, w=stage_prog */\n"
"    uint4  flags;          /* x=has_tex0, y=use_rc, z=alpha_test, w=alpha_func */\n"
"    float4 alpha_misc;     /* x=alpha_ref, y=has_tex1 (0=none,1=rgba,2=opaque) */\n"
"};\n"
"\n"
"struct PSIn {\n"
"    float4 pos  : SV_Position;\n"
"    float4 col  : COLOR0;\n"
"    float2 uv   : TEXCOORD0;\n"
"    float4 spec : COLOR1;\n"
"    float2 uv1  : TEXCOORD1;\n"
"};\n"
"\n"
"float map_in1(float x, uint b) {\n"
"    float p = max(x, 0.0f);\n"
"    uint m = (b >> 5) & 7u;\n"
"    if (m == 0u) return p;\n"
"    if (m == 1u) return 1.0f - saturate(x);\n"
"    if (m == 2u) return 2.0f * p - 1.0f;\n"
"    if (m == 3u) return -2.0f * p + 1.0f;\n"
"    if (m == 4u) return p - 0.5f;\n"
"    if (m == 5u) return -p + 0.5f;\n"
"    if (m == 6u) return x;\n"
"    return -x;\n"
"}\n"
"\n"
"float3 map_in3(float3 x, uint b) {\n"
"    float3 p = max(x, 0.0f);\n"
"    uint m = (b >> 5) & 7u;\n"
"    if (m == 0u) return p;\n"
"    if (m == 1u) return 1.0f - saturate(x);\n"
"    if (m == 2u) return 2.0f * p - 1.0f;\n"
"    if (m == 3u) return -2.0f * p + 1.0f;\n"
"    if (m == 4u) return p - 0.5f;\n"
"    if (m == 5u) return -p + 0.5f;\n"
"    if (m == 6u) return x;\n"
"    return -x;\n"
"}\n"
"\n"
"float map_out1(float x, uint m) {\n"
"    if (m == 0x08u) return x - 0.5f;\n"
"    if (m == 0x10u) return x * 2.0f;\n"
"    if (m == 0x18u) return (x - 0.5f) * 2.0f;\n"
"    if (m == 0x20u) return x * 4.0f;\n"
"    if (m == 0x30u) return x * 0.5f;\n"
"    return x;\n"
"}\n"
"\n"
"float3 map_out3(float3 x, uint m) {\n"
"    if (m == 0x08u) return x - 0.5f;\n"
"    if (m == 0x10u) return x * 2.0f;\n"
"    if (m == 0x18u) return (x - 0.5f) * 2.0f;\n"
"    if (m == 0x20u) return x * 4.0f;\n"
"    if (m == 0x30u) return x * 0.5f;\n"
"    return x;\n"
"}\n"
"\n"
"float4 get_reg(uint idx, float4 c0, float4 c1, float4 fog, float4 v0, float4 v1,\n"
"               float4 t0, float4 t1, float4 r0, float4 r1, float4 sumv, float4 efv) {\n"
"    if (idx == 1u)  return c0;\n"
"    if (idx == 2u)  return c1;\n"
"    if (idx == 3u)  return fog;\n"
"    if (idx == 4u)  return v0;\n"
"    if (idx == 5u)  return v1;\n"
"    if (idx == 8u)  return t0;\n"
"    if (idx == 9u)  return t1;\n"
"    if (idx == 10u || idx == 11u) return float4(1, 1, 1, 1);\n"
"    if (idx == 12u) return r0;\n"
"    if (idx == 13u) return r1;\n"
"    if (idx == 14u) return sumv;\n"
"    if (idx == 15u) return efv;\n"
"    return float4(0, 0, 0, 0);\n"
"}\n"
"\n"
"float3 read_rgb(uint b, float4 c0, float4 c1, float4 fog, float4 v0, float4 v1,\n"
"                float4 t0, float4 t1, float4 r0, float4 r1, float4 sumv, float4 efv) {\n"
"    float4 reg = get_reg(b & 0xFu, c0, c1, fog, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"    float3 src = ((b & 0x10u) != 0u) ? reg.aaa : reg.rgb;\n"
"    return map_in3(src, b);\n"
"}\n"
"\n"
"float read_alpha(uint b, float4 c0, float4 c1, float4 fog, float4 v0, float4 v1,\n"
"                 float4 t0, float4 t1, float4 r0, float4 r1, float4 sumv, float4 efv) {\n"
"    float4 reg = get_reg(b & 0xFu, c0, c1, fog, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"    float src = ((b & 0x10u) != 0u) ? reg.a : reg.b;\n"
"    return map_in1(src, b);\n"
"}\n"
"\n"
"float4 eval_combiner(float4 v0, float4 v1, float4 t0, float4 t1) {\n"
"    float4 r0   = float4(0, 0, 0, ((rc_meta.w & 0x1Fu) != 0u) ? t0.a : 1.0f);\n"
"    float4 r1   = float4(0, 0, 0, 0);\n"
"    float4 sumv = float4(0, 0, 0, 0);\n"
"    float4 efv  = float4(0, 0, 0, 0);\n"
"    uint n = min(rc_meta.x & 0xFFu, 8u);\n"
"    uint cflags = rc_meta.x >> 8;\n"
"\n"
"    [loop] for (uint s = 0u; s < n; s++) {\n"
"        uint icw  = stages[s].x;\n"
"        uint ocw  = stages[s].y;\n"
"        uint aicw = stages[s].z;\n"
"        uint aocw = stages[s].w;\n"
"        uint fl   = ocw >> 12;\n"
"        uint afl  = aocw >> 12;\n"
"        float4 c0 = ((cflags & 0x010u) != 0u) ? factor0[s] : factor0[0];\n"
"        float4 c1 = ((cflags & 0x100u) != 0u) ? factor1[s] : factor1[0];\n"
"        bool mux_cd = ((cflags & 1u) != 0u) ? (r0.a >= 0.5f)\n"
"                                            : ((((int)(r0.a * 255.0f)) & 1) != 0);\n"
"\n"
"        float3 A = read_rgb(icw >> 24, c0, c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"        float3 B = read_rgb(icw >> 16, c0, c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"        float3 C = read_rgb(icw >>  8, c0, c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"        float3 D = read_rgb(icw,       c0, c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"        float aA = read_alpha(aicw >> 24, c0, c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"        float aB = read_alpha(aicw >> 16, c0, c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"        float aC = read_alpha(aicw >>  8, c0, c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"        float aD = read_alpha(aicw,       c0, c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"\n"
"        float3 pab = ((fl & 2u) != 0u) ? dot(A, B).xxx : (A * B);\n"
"        float3 pcd = ((fl & 1u) != 0u) ? dot(C, D).xxx : (C * D);\n"
"        float3 msv = ((fl & 4u) != 0u) ? (mux_cd ? pcd : pab) : (pab + pcd);\n"
"        float3 ab  = clamp(map_out3(pab, fl & 0x38u), -1.0f, 1.0f);\n"
"        float3 cd  = clamp(map_out3(pcd, fl & 0x38u), -1.0f, 1.0f);\n"
"        float3 ms  = clamp(map_out3(msv, fl & 0x38u), -1.0f, 1.0f);\n"
"\n"
"        float apab = aA * aB;\n"
"        float apcd = aC * aD;\n"
"        float amsv = ((afl & 4u) != 0u) ? (mux_cd ? apcd : apab) : (apab + apcd);\n"
"        float aab  = clamp(map_out1(apab, afl & 0x38u), -1.0f, 1.0f);\n"
"        float acd  = clamp(map_out1(apcd, afl & 0x38u), -1.0f, 1.0f);\n"
"        float ams  = clamp(map_out1(amsv, afl & 0x38u), -1.0f, 1.0f);\n"
"\n"
"        uint d_ab = (ocw >> 4) & 0xFu;\n"
"        uint d_cd =  ocw       & 0xFu;\n"
"        uint d_ms = (ocw >> 8) & 0xFu;\n"
"        if (d_ab == 12u) { r0.rgb = ab; if ((fl & 0x80u) != 0u) r0.a = ab.b; }\n"
"        else if (d_ab == 13u) { r1.rgb = ab; if ((fl & 0x80u) != 0u) r1.a = ab.b; }\n"
"        else if (d_ab ==  8u) { t0.rgb = ab; if ((fl & 0x80u) != 0u) t0.a = ab.b; }\n"
"        else if (d_ab ==  9u) { t1.rgb = ab; if ((fl & 0x80u) != 0u) t1.a = ab.b; }\n"
"        if (d_cd == 12u) { r0.rgb = cd; if ((fl & 0x40u) != 0u) r0.a = cd.b; }\n"
"        else if (d_cd == 13u) { r1.rgb = cd; if ((fl & 0x40u) != 0u) r1.a = cd.b; }\n"
"        else if (d_cd ==  8u) { t0.rgb = cd; if ((fl & 0x40u) != 0u) t0.a = cd.b; }\n"
"        else if (d_cd ==  9u) { t1.rgb = cd; if ((fl & 0x40u) != 0u) t1.a = cd.b; }\n"
"        if (d_ms == 12u) r0.rgb = ms;\n"
"        else if (d_ms == 13u) r1.rgb = ms;\n"
"        else if (d_ms ==  8u) t0.rgb = ms;\n"
"        else if (d_ms ==  9u) t1.rgb = ms;\n"
"\n"
"        uint ad_ab = (aocw >> 4) & 0xFu;\n"
"        uint ad_cd =  aocw       & 0xFu;\n"
"        uint ad_ms = (aocw >> 8) & 0xFu;\n"
"        if (ad_ab == 12u) r0.a = aab; else if (ad_ab == 13u) r1.a = aab; else if (ad_ab == 8u) t0.a = aab; else if (ad_ab == 9u) t1.a = aab;\n"
"        if (ad_cd == 12u) r0.a = acd; else if (ad_cd == 13u) r1.a = acd; else if (ad_cd == 8u) t0.a = acd; else if (ad_cd == 9u) t1.a = acd;\n"
"        if (ad_ms == 12u) r0.a = ams; else if (ad_ms == 13u) r1.a = ams; else if (ad_ms == 8u) t0.a = ams; else if (ad_ms == 9u) t1.a = ams;\n"
"    }\n"
"\n"
"    float4 outc;\n"
"    if (rc_meta.y != 0u || rc_meta.z != 0u) {\n"
"        uint f0 = rc_meta.y;\n"
"        uint f1 = rc_meta.z;\n"
"        uint ffl = f1 & 0xFFu;\n"
"        float3 sa = ((ffl & 0x40u) != 0u) ? (1.0f - v1.rgb) : v1.rgb;\n"
"        float3 sb = ((ffl & 0x20u) != 0u) ? (1.0f - r0.rgb) : r0.rgb;\n"
"        sumv = float4(((ffl & 0x80u) != 0u) ? saturate(sa + sb) : (sa + sb), 0.0f);\n"
"        float3 E = read_rgb(f1 >> 24, final_c0, final_c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"        float3 F = read_rgb(f1 >> 16, final_c0, final_c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"        efv = float4(E * F, 0.0f);\n"
"        float3 A = read_rgb(f0 >> 24, final_c0, final_c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"        float3 B = read_rgb(f0 >> 16, final_c0, final_c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"        float3 C = read_rgb(f0 >>  8, final_c0, final_c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"        float3 D = read_rgb(f0,       final_c0, final_c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"        outc.rgb = D + C * (1.0f - A) + B * A;\n"
"        outc.a   = read_alpha(f1 >> 8, final_c0, final_c1, fog_col, v0, v1, t0, t1, r0, r1, sumv, efv);\n"
"    } else {\n"
"        outc = r0;\n"
"    }\n"
"    return saturate(outc);\n"
"}\n"
"\n"
"float4 main(PSIn i) : SV_Target {\n"
"    float4 t0 = (flags.x != 0u) ? tex0.Sample(smp0, i.uv) : float4(1, 1, 1, 1);\n"
"    if (flags.x == 2u) t0.a = 1.0f;\n"
"    float4 t1 = (alpha_misc.y > 0.5f) ? tex1.Sample(smp1, i.uv1) : float4(1, 1, 1, 1);\n"
"    if (alpha_misc.y > 1.5f) t1.a = 1.0f;\n"
"    float4 outc = (flags.y != 0u) ? eval_combiner(i.col, i.spec, t0, t1) : (i.col * t0);\n"
"    if (flags.z != 0u) {\n"
"        float a = outc.a;\n"
"        float r = alpha_misc.x;\n"
"        uint fn = flags.w;\n"
"        bool pass_a = true;\n"
"        if      (fn == 0x200u) pass_a = false;\n"
"        else if (fn == 0x201u) pass_a = (a <  r);\n"
"        else if (fn == 0x202u) pass_a = (abs(a - r) < (0.5f / 255.0f));\n"
"        else if (fn == 0x203u) pass_a = (a <= r + (0.5f / 255.0f));\n"
"        else if (fn == 0x204u) pass_a = (a >  r);\n"
"        else if (fn == 0x205u) pass_a = (abs(a - r) >= (0.5f / 255.0f));\n"
"        else if (fn == 0x206u) pass_a = (a >= r - (0.5f / 255.0f));\n"
"        if (!pass_a) discard;\n"
"    }\n"
"    return outc;\n"
"}\n";

static const char k_blit_vs_hlsl[] =
"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"VSOut main(uint id : SV_VertexID) {\n"
"    VSOut o;\n"
"    float2 uv = float2((id << 1) & 2u, id & 2u);\n"
"    o.uv  = uv;\n"
"    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);\n"
"    return o;\n"
"}\n";

static const char k_blit_ps_hlsl[] =
"Texture2D    src_tex : register(t0);\n"
"SamplerState src_smp : register(s0);\n"
"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"float4 main(VSOut i) : SV_Target {\n"
"    float4 c = src_tex.Sample(src_smp, i.uv);\n"
"    return float4(c.rgb, 1.0f);\n"
"}\n";

/* ── Structures & Caches ───────────────────────────────────── */

typedef struct {
    float    x, y, z, rhw;
    uint32_t diffuse;
    float    u, v;
    uint32_t specular;
    float    u1, v1;
} GpuVert;

typedef struct {
    float inv_rt_size[2];
    float inv_tex_size[2];
    float inv_tex1_size[2];
    float z_scale;
    float pad;
} VsCB;

typedef struct {
    uint32_t stages[8][4];
    float    factor0[8][4];
    float    factor1[8][4];
    float    final_c0[4];
    float    final_c1[4];
    float    fog_col[4];
    uint32_t rc_meta[4];
    uint32_t flags[4];
    float    alpha_misc[4];
} PsCB;

#define MAX_RTS       64
#define MAX_DSVS      32
#define MAX_TEX_CACHE 2048
#define MAX_TEX_FAST  4096
#define MAX_BS_CACHE  256
#define MAX_DSS_CACHE 256
#define MAX_SMP_CACHE 128

typedef struct {
    uint32_t color_va;
    uint32_t w, h;
    uint32_t pitch, bpp;
    uint32_t aa_sx, aa_sy;
    uint32_t surface_format;
    int      has_drawn;
    uint64_t last_used;
    ID3D11Texture2D          *tex;
    ID3D11RenderTargetView   *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11Texture2D          *staging;
    uint32_t staging_w, staging_h;
} RtEntry;

typedef struct {
    uint32_t zeta_va;
    uint32_t w, h;
    uint64_t last_used;
    ID3D11Texture2D        *tex;
    ID3D11DepthStencilView *dsv;
} DsvEntry;

static uint64_t s_rt_clock = 0;
static uint64_t s_dsv_clock = 0;

static void flush_draw_queue(void);
static void flush_vsh_queue(void);

typedef struct {
    uint32_t offset, w, h, color, palette, pitch;
    DXGI_FORMAT dxgi_fmt;
    uint64_t hash;
    uint64_t last_used;
    uint64_t validated_frame;
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
} TexEntry;

typedef struct {
    uint32_t enable, src, dst, eq, mask;
    ID3D11BlendState *state;
} BsEntry;

typedef struct {
    uint32_t depth_en, depth_fn, depth_wr;
    uint32_t st_en, st_fn, st_fmask, st_wmask, st_fail, st_zfail, st_zpass;
    ID3D11DepthStencilState *state;
} DssEntry;

typedef struct {
    uint32_t addr_u, addr_v, linear;
    ID3D11SamplerState *state;
} SmpEntry;

/* ── Global Backend State (accessed solely on NV2A poll thread) ── */

static int  s_init_tried;
static int  s_ready;
static int  s_headless;
static int  s_want_dump_sync;
static int  s_cull_mode;       /* 0 = off (matches CPU rasterizer), 1 = projected cull */
static int  s_aspect_stretch;  /* 0 = 4:3 pillarbox, 1 = stretch to fill */
static int  s_vsync = 1;       /* 1 = present sync interval 1, 0 = unlocked */
static int  s_fullscreen;
static RECT s_saved_rect;
static LONG s_saved_style;

static HWND                  s_hwnd;
static uint32_t              s_win_w = 1280, s_win_h = 960;
static ID3D11Device         *s_dev;
static ID3D11DeviceContext  *s_ctx;
static IDXGISwapChain       *s_swap;
static ID3D11RenderTargetView *s_back_rtv;

static ID3D11VertexShader   *s_main_vs;
static ID3D11PixelShader    *s_main_ps;
static ID3D11InputLayout    *s_main_il;
static ID3D11Buffer         *s_vs_cb;
static ID3D11Buffer         *s_ps_cb;
static ID3D11VertexShader   *s_blit_vs;
static ID3D11PixelShader    *s_blit_ps;
static ID3D11SamplerState   *s_blit_smp;
static ID3D11RasterizerState *s_rs_scissor;
static ID3D11RasterizerState *s_rs_noscissor;
static ID3D11RasterizerState *s_rs_vsh_none;
static ID3D11RasterizerState *s_rs_vsh_cw;
static ID3D11RasterizerState *s_rs_vsh_ccw;
static ID3D11BlendState     *s_bs_opaque;
static ID3D11DepthStencilState *s_dss_off;

static ID3D11Buffer         *s_vb;
static uint32_t              s_vb_cap;
static uint32_t              s_vb_head;
static GpuVert              *s_cpu_verts;
static uint32_t              s_cpu_verts_cap;

static RtEntry               s_rts[MAX_RTS];
static int                   s_rt_count;
static RtEntry              *s_last_rt;
static RtEntry              *s_present_rt;
static RtEntry              *s_resolve_src_rt;

static DsvEntry              s_dsvs[MAX_DSVS];
static int                   s_dsv_count;

static TexEntry              s_texs[MAX_TEX_CACHE];
static int16_t               s_tex_fast[MAX_TEX_FAST]; /* 1-based index into s_texs[] */
static uint64_t              s_tex_clock;
static uint64_t              s_frame_seq = 1;
static uint32_t             *s_decode_buf;
static size_t                s_decode_cap;

/* Pipeline & constant-buffer state cache (invalidated on clear quad / flip) */
static int                       s_pipe_valid;
static ID3D11RenderTargetView   *s_bound_rtv;
static ID3D11DepthStencilView   *s_bound_dsv;
static ID3D11BlendState         *s_bound_bs;
static uint32_t                  s_bound_bcol;
static ID3D11DepthStencilState  *s_bound_dss;
static uint32_t                  s_bound_stref;
static ID3D11ShaderResourceView *s_bound_srv;
static ID3D11SamplerState       *s_bound_smp;
static ID3D11ShaderResourceView *s_bound_srv1;
static ID3D11SamplerState       *s_bound_smp1;
static uint32_t                  s_bound_rt_w, s_bound_rt_h;
static LONG                      s_bound_sc[4];
static VsCB                      s_last_vs_cb;
static PsCB                      s_last_ps_cb;
static int                       s_vs_cb_valid;
static int                       s_ps_cb_valid;

/* Lightweight telemetry */
static LARGE_INTEGER             s_qpc_freq;
static uint64_t                  s_stat_draw_ticks;
static uint64_t                  s_stat_tex_ticks;
static uint64_t                  s_stat_flush_ticks;
static uint64_t                  s_stat_draws;
static uint64_t                  s_stat_gpu_draws;
static uint64_t                  s_stat_tex_uploads;
static uint64_t                  s_stat_multitex;
static uint64_t                  s_stat_flips;
static int                       s_inspect_mode = -1;
static int                       s_inspect_active;
static uint32_t                  s_frame_draws;

/* Batched single-Map draw queue */
#define MAX_Q_CMDS 2048
#define MAX_Q_VSCB 256
#define MAX_Q_PSCB 1024

typedef struct {
    RtEntry                  *rt;
    ID3D11DepthStencilView   *dsv;
    ID3D11BlendState         *bs;
    uint32_t                  bcol;
    ID3D11DepthStencilState  *dss;
    uint32_t                  stref;
    ID3D11ShaderResourceView *srv;
    ID3D11SamplerState       *smp;
    ID3D11ShaderResourceView *srv1;
    ID3D11SamplerState       *smp1;
    LONG                      sc[4];
    uint16_t                  vs_idx;
    uint16_t                  ps_idx;
    uint32_t                  start_vert;
    uint32_t                  count;
} QCmd;

static GpuVert  *s_q_verts;
static uint32_t  s_q_verts_cap;
static uint32_t  s_q_verts_n;
static QCmd      s_q_cmds[MAX_Q_CMDS];
static uint32_t  s_q_cmds_n;
static VsCB      s_q_vscbs[MAX_Q_VSCB];
static uint32_t  s_q_vscbs_n;
static PsCB      s_q_pscbs[MAX_Q_PSCB];
static uint32_t  s_q_pscbs_n;

/* GPU-side NV2A Vertex Shader cache & batched indexed draw queue */
#define MAX_VSH_CACHE 1024
#define MAX_VQ_CMDS   2048
#define MAX_VQ_CONSTS 1024

typedef struct {
    float inv_rt_size[2];
    float uv0_scale[2];
    float uv1_scale[2];
    float z_scale;
    float aa_sx;
    float aa_sy;
    float pad0[3];
} VshFrameCB;

typedef struct {
    uint64_t            key;
    uint32_t            in_mask;
    uint32_t            stride; /* bytes per packed vertex (multiple of 16) */
    ID3D11VertexShader *vs;
    ID3D11InputLayout  *il;
    int                 failed;
} VshEntry;

typedef struct {
    VshEntry                 *vsh;
    RtEntry                  *rt;
    ID3D11DepthStencilView   *dsv;
    ID3D11BlendState         *bs;
    uint32_t                  bcol;
    ID3D11DepthStencilState  *dss;
    uint32_t                  stref;
    ID3D11RasterizerState    *rs_state;
    ID3D11ShaderResourceView *srv;
    ID3D11SamplerState       *smp;
    ID3D11ShaderResourceView *srv1;
    ID3D11SamplerState       *smp1;
    LONG                      sc[4];
    uint16_t                  fcb_idx;
    uint16_t                  ps_idx;
    uint16_t                  const_idx;
    uint32_t                  vb_byte_off;
    uint32_t                  start_idx;
    uint32_t                  idx_count;
} VshCmd;

static VshEntry      s_vsh_cache[MAX_VSH_CACHE];
static int           s_vsh_count;
static ID3D11Buffer *s_vsh_frame_cb;
static ID3D11Buffer *s_vsh_const_cb;
static ID3D11Buffer *s_vsh_vb;
static uint32_t      s_vsh_vb_cap; /* bytes */
static ID3D11Buffer *s_vsh_ib;
static uint32_t      s_vsh_ib_cap; /* index count */

static uint8_t      *s_vq_vbytes;
static uint32_t      s_vq_vbytes_cap;
static uint32_t      s_vq_vbytes_n;
static uint32_t     *s_vq_idxs;
static uint32_t      s_vq_idxs_cap;
static uint32_t      s_vq_idxs_n;
static VshCmd        s_vq_cmds[MAX_VQ_CMDS];
static uint32_t      s_vq_cmds_n;
static VshFrameCB    s_vq_fcbs[MAX_Q_VSCB];
static uint32_t      s_vq_fcbs_n;
static PsCB          s_vq_pscbs[MAX_Q_PSCB];
static uint32_t      s_vq_pscbs_n;
static float        *s_vq_consts; /* MAX_VQ_CONSTS * 192 * 4 floats */
static uint32_t      s_vq_consts_n;
static uint64_t      s_stat_vsh_draws;
static uint64_t      s_stat_vsh_verts;

static void flush_vsh_queue(void);

static void invalidate_pipe_cache(void)
{
    s_pipe_valid  = 0;
    s_bound_rtv   = NULL;
    s_bound_dsv   = NULL;
    s_bound_bs    = NULL;
    s_bound_dss   = NULL;
    s_bound_srv   = NULL;
    s_bound_smp   = NULL;
    s_bound_srv1  = NULL;
    s_bound_smp1  = NULL;
    s_vs_cb_valid = 0;
    s_ps_cb_valid = 0;
}

static BsEntry               s_bs_cache[MAX_BS_CACHE];
static int                   s_bs_count;
static DssEntry              s_dss_cache[MAX_DSS_CACHE];
static int                   s_dss_count;
static SmpEntry              s_smp_cache[MAX_SMP_CACHE];
static int                   s_smp_count;

/* ── Helpers ───────────────────────────────────────────────── */

static void unpack_argb_f4(uint32_t c, float out[4])
{
    out[0] = (float)((c >> 16) & 0xFFu) / 255.0f;
    out[1] = (float)((c >>  8) & 0xFFu) / 255.0f;
    out[2] = (float)( c        & 0xFFu) / 255.0f;
    out[3] = (float)((c >> 24) & 0xFFu) / 255.0f;
}

static void toggle_fullscreen(HWND hwnd)
{
    if (!hwnd || s_headless) return;
    if (!s_fullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        s_saved_style = GetWindowLongA(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &s_saved_rect);
        if (GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongA(hwnd, GWL_STYLE, s_saved_style & ~(WS_CAPTION | WS_THICKFRAME));
            SetWindowPos(hwnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            s_fullscreen = 1;
        }
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, s_saved_style);
        SetWindowPos(hwnd, NULL,
                     s_saved_rect.left, s_saved_rect.top,
                     s_saved_rect.right - s_saved_rect.left,
                     s_saved_rect.bottom - s_saved_rect.top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        s_fullscreen = 0;
    }
}

static void resize_swapchain(uint32_t w, uint32_t h)
{
    ID3D11Texture2D *bb = NULL;
    if (!s_swap || !s_dev || !w || !h) return;
    if (w == s_win_w && h == s_win_h && s_back_rtv) return;
    if (s_ctx) ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    invalidate_pipe_cache();
    if (s_back_rtv) { ID3D11RenderTargetView_Release(s_back_rtv); s_back_rtv = NULL; }
    if (SUCCEEDED(IDXGISwapChain_ResizeBuffers(s_swap, 0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
        s_win_w = w;
        s_win_h = h;
    }
    if (SUCCEEDED(IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_back_rtv);
        ID3D11Texture2D_Release(bb);
    }
}

static LRESULT CALLBACK d3d11_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30)))
        || (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30)))) {
        toggle_fullscreen(h);
        return 0;
    }
    if (m == WM_SYSKEYDOWN && w == VK_RETURN)
        return 0;

    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        PostQuitMessage(0);
        ExitProcess(0);
        return 0;
    case WM_SIZE:
        if (w != SIZE_MINIMIZED && s_swap) {
            uint32_t nw = LOWORD(l), nh = HIWORD(l);
            if (nw > 0 && nh > 0) resize_swapchain(nw, nh);
        }
        return 0;
    case WM_KILLFOCUS:
        for (int k = 0; k < 256; k++)
            xbox_InputKeySet(k, 0);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        xbox_InputKeySet((int)w, 1);
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side) xbox_InputKeySet(side, 1);
        }
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0);
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side) xbox_InputKeySet(side, 0);
        }
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;
    default:
        return DefWindowProcA(h, m, w, l);
    }
}

static ID3DBlob *compile_hlsl(const char *src, const char *target, const char *label)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), label, NULL, NULL, "main",
                            target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] shader compile failed (%s): hr=0x%08X %s\n",
                label, (unsigned)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static int d3d11_ensure_init(void)
{
    WNDCLASSA wc;
    RECT rc;
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL fl;
    ID3D11Texture2D *bb = NULL;
    ID3DBlob *vsb = NULL, *psb = NULL;
    HRESULT hr;

    if (s_init_tried)
        return s_ready;
    s_init_tried = 1;

    {
        const char *e = getenv("RECOMP_HEADLESS");
        const char *d = getenv("RECOMP_FB_DUMP");
        const char *w = getenv("RECOMP_FB_WINDOW");
        const char *c = getenv("RECOMP_D3D11_CULL");
        const char *sc = getenv("RECOMP_WINDOW_SCALE");
        const char *ww = getenv("RECOMP_WINDOW_WIDTH");
        const char *wh = getenv("RECOMP_WINDOW_HEIGHT");
        const char *as = getenv("RECOMP_ASPECT_STRETCH");
        const char *vs = getenv("RECOMP_VSYNC");
        s_headless = (e && atoi(e) != 0) || (d && *d && !(w && atoi(w) != 0));
        s_cull_mode = (c && atoi(c) != 0) ? 1 : 0;
        s_aspect_stretch = (as && atoi(as) != 0) ? 1 : 0;
        s_vsync = (vs && *vs) ? (atoi(vs) != 0 ? 1 : 0) : 1;
        if (sc) {
            int k = atoi(sc);
            if (k >= 1 && k <= 6) { s_win_w = 640u * (uint32_t)k; s_win_h = 480u * (uint32_t)k; }
        }
        if (ww && wh) {
            int nw = atoi(ww), nh = atoi(wh);
            if (nw >= 320 && nw <= 7680 && nh >= 240 && nh <= 4320) {
                s_win_w = (uint32_t)nw;
                s_win_h = (uint32_t)nh;
            }
        }
    }

    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = d3d11_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "SimsBustinOutD3D11";
    RegisterClassA(&wc);

    rc.left = 0; rc.top = 0; rc.right = (LONG)s_win_w; rc.bottom = (LONG)s_win_h;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    {
        int ww_px = rc.right - rc.left;
        int wh_px = rc.bottom - rc.top;
        int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT;
        RECT work;
        if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
            int work_w = work.right - work.left;
            int work_h = work.bottom - work.top;
            wx = work.left + (work_w - ww_px) / 2;
            wy = work.top  + (work_h - wh_px) / 2;
            if (wx < work.left) wx = work.left;
            if (wy < work.top)  wy = work.top;
        }
        s_hwnd = CreateWindowA(wc.lpszClassName, "The Sims: Bustin' Out (D3D11)",
                               WS_OVERLAPPEDWINDOW,
                               wx, wy, ww_px, wh_px,
                               NULL, NULL, wc.hInstance, NULL);
    }
    if (!s_hwnd) {
        fprintf(stderr, "[D3D11] CreateWindowA failed\n");
        return 0;
    }
    if (!s_headless) {
        RECT cr;
        ShowWindow(s_hwnd, SW_SHOW);
        UpdateWindow(s_hwnd);
        if (getenv("RECOMP_FULLSCREEN") && atoi(getenv("RECOMP_FULLSCREEN")) != 0)
            toggle_fullscreen(s_hwnd);
        if (GetClientRect(s_hwnd, &cr) && cr.right > 0 && cr.bottom > 0) {
            s_win_w = (uint32_t)cr.right;
            s_win_h = (uint32_t)cr.bottom;
        }
    }

    memset(&scd, 0, sizeof(scd));
    scd.BufferCount       = 2;
    scd.BufferDesc.Width  = s_win_w;
    scd.BufferDesc.Height = s_win_h;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = s_hwnd;
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;
    scd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    {
        static const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
            levels, 3, D3D11_SDK_VERSION,
            &scd, &s_swap, &s_dev, &fl, &s_ctx);
        if (FAILED(hr)) {
            hr = D3D11CreateDeviceAndSwapChain(
                NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
                levels, 3, D3D11_SDK_VERSION,
                &scd, &s_swap, &s_dev, &fl, &s_ctx);
        }
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", (unsigned)hr);
        return 0;
    }

    if (SUCCEEDED(IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_back_rtv);
        ID3D11Texture2D_Release(bb);
    }
    /* Disable DXGI's default exclusive-fullscreen Alt+Enter hook so it never
     * conflicts with our borderless toggle_fullscreen() in d3d11_wndproc. */
    {
        IDXGIFactory *fac = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetParent(s_swap, &IID_IDXGIFactory, (void **)&fac)) && fac) {
            IDXGIFactory_MakeWindowAssociation(fac, s_hwnd, DXGI_MWA_NO_ALT_ENTER);
            IDXGIFactory_Release(fac);
        }
    }

    /* Main shaders */
    vsb = compile_hlsl(k_main_vs_hlsl, "vs_4_0", "main_vs");
    psb = compile_hlsl(k_main_ps_hlsl, "ps_4_0", "main_ps");
    if (!vsb || !psb) return 0;

    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_main_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &s_main_ps);
    {
        static const D3D11_INPUT_ELEMENT_DESC il[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    1, DXGI_FORMAT_B8G8R8A8_UNORM,     0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ID3D11Device_CreateInputLayout(s_dev, il, 5,
                                       ID3D10Blob_GetBufferPointer(vsb),
                                       ID3D10Blob_GetBufferSize(vsb), &s_main_il);
    }
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    /* Blit shaders */
    vsb = compile_hlsl(k_blit_vs_hlsl, "vs_4_0", "blit_vs");
    psb = compile_hlsl(k_blit_ps_hlsl, "ps_4_0", "blit_ps");
    if (!vsb || !psb) return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_blit_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &s_blit_ps);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    /* Constant buffers */
    {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof(bd));
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth      = (sizeof(VsCB) + 15u) & ~15u;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vs_cb);
        bd.ByteWidth      = (sizeof(PsCB) + 15u) & ~15u;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_ps_cb);
        bd.ByteWidth      = (sizeof(VshFrameCB) + 15u) & ~15u;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vsh_frame_cb);
        bd.ByteWidth      = 192u * 4u * sizeof(float);
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vsh_const_cb);
    }

    /* Fixed rasterizer, blit sampler, opaque blend, depth-off states */
    {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof(rd));
        rd.FillMode        = D3D11_FILL_SOLID;
        rd.CullMode        = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        rd.ScissorEnable   = TRUE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_scissor);
        rd.ScissorEnable   = FALSE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_noscissor);

        /* Hardware-clipped rasterizer states for GPU VSH path */
        rd.DepthClipEnable = TRUE;
        rd.ScissorEnable   = TRUE;
        rd.CullMode        = D3D11_CULL_NONE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_vsh_none);
        rd.CullMode              = D3D11_CULL_BACK;
        rd.FrontCounterClockwise = FALSE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_vsh_cw);
        rd.FrontCounterClockwise = TRUE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_vsh_ccw);
    }
    {
        D3D11_SAMPLER_DESC sd;
        memset(&sd, 0, sizeof(sd));
        sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD         = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(s_dev, &sd, &s_blit_smp);
    }
    {
        D3D11_BLEND_DESC bd;
        memset(&bd, 0, sizeof(bd));
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ID3D11Device_CreateBlendState(s_dev, &bd, &s_bs_opaque);
    }
    {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof(dd));
        dd.DepthEnable   = FALSE;
        dd.StencilEnable = FALSE;
        ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_dss_off);
    }

    s_ready = 1;
    fprintf(stderr, "[D3D11] Hardware GPU backend ready (%ux%u, %s)\n",
            s_win_w, s_win_h,
            s_headless ? "headless" : (s_fullscreen ? "fullscreen" : "windowed"));
    fflush(stderr);
    return 1;
}

/* ── NV2A -> D3D11 Enum Translation ────────────────────────── */

static D3D11_COMPARISON_FUNC nv_cmp(uint32_t fn)
{
    switch (fn) {
    case GL_NEVER:    return D3D11_COMPARISON_NEVER;
    case GL_LESS:     return D3D11_COMPARISON_LESS;
    case GL_EQUAL:    return D3D11_COMPARISON_EQUAL;
    case GL_LEQUAL:   return D3D11_COMPARISON_LESS_EQUAL;
    case GL_GREATER:  return D3D11_COMPARISON_GREATER;
    case GL_NOTEQUAL: return D3D11_COMPARISON_NOT_EQUAL;
    case GL_GEQUAL:   return D3D11_COMPARISON_GREATER_EQUAL;
    default:          return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP nv_stencil_op(uint32_t op)
{
    switch (op) {
    case GL_ZERO:      return D3D11_STENCIL_OP_ZERO;
    case GL_REPLACE:   return D3D11_STENCIL_OP_REPLACE;
    case GL_INCR:      return D3D11_STENCIL_OP_INCR_SAT;
    case GL_DECR:      return D3D11_STENCIL_OP_DECR_SAT;
    case GL_INVERT:    return D3D11_STENCIL_OP_INVERT;
    case GL_INCR_WRAP: return D3D11_STENCIL_OP_INCR;
    case GL_DECR_WRAP: return D3D11_STENCIL_OP_DECR;
    default:           return D3D11_STENCIL_OP_KEEP;
    }
}

static D3D11_BLEND nv_blend_factor(uint32_t f, int is_alpha)
{
    switch (f) {
    case GL_ZERO:                return D3D11_BLEND_ZERO;
    case GL_ONE:                 return D3D11_BLEND_ONE;
    case GL_SRC_COLOR:           return is_alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return is_alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case GL_SRC_ALPHA:           return D3D11_BLEND_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
    case GL_DST_ALPHA:           return D3D11_BLEND_DEST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
    case GL_DST_COLOR:           return is_alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return is_alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case GL_SRC_ALPHA_SATURATE:  return D3D11_BLEND_SRC_ALPHA_SAT;
    case GL_CONSTANT_COLOR:      return D3D11_BLEND_BLEND_FACTOR;
    case GL_ONE_MINUS_CONSTANT_COLOR: return D3D11_BLEND_INV_BLEND_FACTOR;
    case GL_CONSTANT_ALPHA:      return D3D11_BLEND_BLEND_FACTOR;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:                     return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP nv_blend_op(uint32_t eq)
{
    switch (eq) {
    case GL_MIN:                   return D3D11_BLEND_OP_MIN;
    case GL_MAX:                   return D3D11_BLEND_OP_MAX;
    case GL_FUNC_SUBTRACT:         return D3D11_BLEND_OP_SUBTRACT;
    case GL_FUNC_REVERSE_SUBTRACT: return D3D11_BLEND_OP_REV_SUBTRACT;
    default:                       return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_TEXTURE_ADDRESS_MODE nv_addr(uint32_t a)
{
    switch (a) {
    case 1: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

/* ── State Object Caches ───────────────────────────────────── */

static ID3D11BlendState *get_blend_state(const Nv2aRenderState *rs)
{
    uint32_t en   = rs->blend_enable ? 1u : 0u;
    uint32_t src  = en ? rs->blend_src : GL_ONE;
    uint32_t dst  = en ? rs->blend_dst : GL_ZERO;
    uint32_t eq   = en ? rs->blend_eq  : GL_FUNC_ADD;
    uint32_t mask = rs->color_mask;
    uint8_t  wmask = 0;
    D3D11_BLEND_DESC bd;
    int i;

    for (i = 0; i < s_bs_count; i++) {
        if (s_bs_cache[i].enable == en && s_bs_cache[i].src == src
         && s_bs_cache[i].dst == dst && s_bs_cache[i].eq == eq
         && s_bs_cache[i].mask == mask)
            return s_bs_cache[i].state;
    }

    if (mask & 0x00010000u) wmask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (mask & 0x00000100u) wmask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (mask & 0x00000001u) wmask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (mask & 0x01000000u) wmask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;

    memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].BlendEnable           = en ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend              = nv_blend_factor(src, 0);
    bd.RenderTarget[0].DestBlend             = nv_blend_factor(dst, 0);
    bd.RenderTarget[0].BlendOp               = nv_blend_op(eq);
    bd.RenderTarget[0].SrcBlendAlpha         = nv_blend_factor(src, 1);
    bd.RenderTarget[0].DestBlendAlpha        = nv_blend_factor(dst, 1);
    bd.RenderTarget[0].BlendOpAlpha          = nv_blend_op(eq);
    bd.RenderTarget[0].RenderTargetWriteMask = wmask;

    {
        ID3D11BlendState *st = NULL;
        if (s_bs_count >= MAX_BS_CACHE)
            return s_bs_cache[0].state;
        ID3D11Device_CreateBlendState(s_dev, &bd, &st);
        s_bs_cache[s_bs_count].enable = en;
        s_bs_cache[s_bs_count].src    = src;
        s_bs_cache[s_bs_count].dst    = dst;
        s_bs_cache[s_bs_count].eq     = eq;
        s_bs_cache[s_bs_count].mask   = mask;
        s_bs_cache[s_bs_count].state  = st;
        s_bs_count++;
        return st;
    }
}

static ID3D11DepthStencilState *get_dss_state(const Nv2aRenderState *rs, int has_dsv)
{
    uint32_t d_en = (has_dsv && rs->depth_test_enable) ? 1u : 0u;
    uint32_t d_fn = d_en ? (rs->depth_func ? rs->depth_func : GL_LESS) : GL_ALWAYS;
    /* On NV2A / D3D8 / OpenGL, disabling depth testing also disables depth writes
     * regardless of depth_write. Writing depth with depth_test_enable==0 corrupts
     * the Z-buffer during projected stencil shadow passes. */
    uint32_t d_wr = (d_en && rs->depth_write) ? 1u : 0u;
    uint32_t s_en = (has_dsv && rs->stencil_enable) ? 1u : 0u;
    uint32_t s_fn = s_en ? (rs->stencil_func ? rs->stencil_func : GL_ALWAYS) : GL_ALWAYS;
    uint32_t s_fm = s_en ? (rs->stencil_func_mask & 0xFFu) : 0xFFu;
    uint32_t s_wm = s_en ? (rs->stencil_write_mask & 0xFFu) : 0xFFu;
    uint32_t s_fl = s_en ? rs->stencil_fail  : GL_KEEP;
    uint32_t s_zf = s_en ? rs->stencil_zfail : GL_KEEP;
    uint32_t s_zp = s_en ? rs->stencil_zpass : GL_KEEP;
    D3D11_DEPTH_STENCIL_DESC dd;
    int i;

    for (i = 0; i < s_dss_count; i++) {
        DssEntry *e = &s_dss_cache[i];
        if (e->depth_en == d_en && e->depth_fn == d_fn && e->depth_wr == d_wr
         && e->st_en == s_en && e->st_fn == s_fn && e->st_fmask == s_fm
         && e->st_wmask == s_wm && e->st_fail == s_fl && e->st_zfail == s_zf
         && e->st_zpass == s_zp)
            return e->state;
    }

    memset(&dd, 0, sizeof(dd));
    dd.DepthEnable      = d_en ? TRUE : FALSE;
    dd.DepthWriteMask   = d_wr ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc        = d_en ? nv_cmp(d_fn) : D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable    = s_en ? TRUE : FALSE;
    dd.StencilReadMask  = (UINT8)s_fm;
    dd.StencilWriteMask = (UINT8)s_wm;
    dd.FrontFace.StencilFailOp      = nv_stencil_op(s_fl);
    dd.FrontFace.StencilDepthFailOp = nv_stencil_op(s_zf);
    dd.FrontFace.StencilPassOp      = nv_stencil_op(s_zp);
    dd.FrontFace.StencilFunc        = nv_cmp(s_fn);
    dd.BackFace                     = dd.FrontFace;

    {
        ID3D11DepthStencilState *st = NULL;
        DssEntry *e;
        if (s_dss_count >= MAX_DSS_CACHE)
            return s_dss_cache[0].state;
        ID3D11Device_CreateDepthStencilState(s_dev, &dd, &st);
        e = &s_dss_cache[s_dss_count++];
        e->depth_en = d_en; e->depth_fn = d_fn; e->depth_wr = d_wr;
        e->st_en = s_en; e->st_fn = s_fn; e->st_fmask = s_fm;
        e->st_wmask = s_wm; e->st_fail = s_fl; e->st_zfail = s_zf;
        e->st_zpass = s_zp; e->state = st;
        return st;
    }
}

static ID3D11SamplerState *get_sampler_state(const Nv2aTexture *tex)
{
    uint32_t au = tex->addr_u;
    uint32_t av = tex->addr_v;
    /* NV097_SET_TEXTURE_FILTER: MAG in bits 24-27 (2 = linear), MIN in bits 16-23 */
    uint32_t mag = (tex->filter >> 24) & 0xFu;
    uint32_t lin = (mag == 0 || mag == 2) ? 1u : 0u;
    D3D11_SAMPLER_DESC sd;
    int i;

    for (i = 0; i < s_smp_count; i++) {
        if (s_smp_cache[i].addr_u == au && s_smp_cache[i].addr_v == av
         && s_smp_cache[i].linear == lin)
            return s_smp_cache[i].state;
    }

    memset(&sd, 0, sizeof(sd));
    sd.Filter         = lin ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU       = nv_addr(au);
    sd.AddressV       = nv_addr(av);
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;

    {
        ID3D11SamplerState *st = NULL;
        if (s_smp_count >= MAX_SMP_CACHE)
            return s_smp_cache[0].state;
        ID3D11Device_CreateSamplerState(s_dev, &sd, &st);
        s_smp_cache[s_smp_count].addr_u = au;
        s_smp_cache[s_smp_count].addr_v = av;
        s_smp_cache[s_smp_count].linear = lin;
        s_smp_cache[s_smp_count].state  = st;
        s_smp_count++;
        return st;
    }
}

/* ── Render Target & Depth-Stencil Management ──────────────── */

static void compute_rt_dims(const Nv2aSurface *s, uint32_t *out_w, uint32_t *out_h)
{
    uint32_t bpp = s->bytes_per_pixel ? s->bytes_per_pixel : 4u;
    uint32_t w = s->pitch ? (s->pitch / bpp) : (s->clip_x + s->width);
    uint32_t h = s->clip_y + s->height;
    if (w < s->clip_x + s->width) w = s->clip_x + s->width;
    if (w == 0) w = 640;
    if (h == 0) h = 480;
    /* Keep full framebuffer height when letterboxed (e.g. 1280x896+0+32 -> 1280x960) */
    if (w == 1280 && h >= 896 && h < 960) h = 960;
    if (w == 640  && h >= 440 && h < 480) h = 480;
    *out_w = w;
    *out_h = h;
}

static RtEntry *get_rt(const Nv2aSurface *s)
{
    uint32_t w, h;
    int i;
    RtEntry *e = NULL;

    compute_rt_dims(s, &w, &h);

    for (i = 0; i < s_rt_count; i++) {
        if (s_rts[i].color_va == s->color_va) {
            e = &s_rts[i];
            e->last_used = ++s_rt_clock;
            if (e->w >= w && e->h >= h) {
                e->pitch          = s->pitch;
                e->bpp            = s->bytes_per_pixel;
                e->aa_sx          = s->aa_sx;
                e->aa_sy          = s->aa_sy;
                e->surface_format = s->surface_format;
                return e;
            }
            /* Grow if needed: flush any queued draws referencing this RT first */
            flush_vsh_queue();
            flush_draw_queue();
            invalidate_pipe_cache();
            if (e->w > w) w = e->w;
            if (e->h > h) h = e->h;
            if (e->srv)     { ID3D11ShaderResourceView_Release(e->srv); e->srv = NULL; }
            if (e->rtv)     { ID3D11RenderTargetView_Release(e->rtv);   e->rtv = NULL; }
            if (e->tex)     { ID3D11Texture2D_Release(e->tex);          e->tex = NULL; }
            if (e->staging) { ID3D11Texture2D_Release(e->staging);      e->staging = NULL; }
            e->staging_w = e->staging_h = 0;
            break;
        }
    }

    if (!e) {
        if (s_rt_count < MAX_RTS) {
            e = &s_rts[s_rt_count++];
            memset(e, 0, sizeof(*e));
        } else {
            /* Evict LRU off-screen RT, never evicting active display/present targets */
            int victim = -1;
            uint64_t oldest = UINT64_MAX;
            for (i = 0; i < s_rt_count; i++) {
                RtEntry *cand = &s_rts[i];
                if (cand == s_present_rt || cand == s_resolve_src_rt || cand == s_last_rt)
                    continue;
                if (cand->color_va < 0x81000000u)
                    continue;
                if (cand->last_used < oldest) {
                    oldest = cand->last_used;
                    victim = i;
                }
            }
            if (victim < 0) {
                for (i = 0; i < s_rt_count; i++) {
                    RtEntry *cand = &s_rts[i];
                    if (cand == s_present_rt || cand == s_resolve_src_rt) continue;
                    if (cand->last_used < oldest) { oldest = cand->last_used; victim = i; }
                }
            }
            if (victim < 0) victim = 0;
            flush_vsh_queue();
            flush_draw_queue();
            invalidate_pipe_cache();
            e = &s_rts[victim];
            if (e == s_last_rt) s_last_rt = NULL;
            if (e->srv)     { ID3D11ShaderResourceView_Release(e->srv); e->srv = NULL; }
            if (e->rtv)     { ID3D11RenderTargetView_Release(e->rtv);   e->rtv = NULL; }
            if (e->tex)     { ID3D11Texture2D_Release(e->tex);          e->tex = NULL; }
            if (e->staging) { ID3D11Texture2D_Release(e->staging);      e->staging = NULL; }
            e->staging_w = e->staging_h = 0;
        }
    }

    {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof(td));
        td.Width            = w;
        td.Height           = h;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &e->tex)))
            return NULL;
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)e->tex, NULL, &e->rtv);
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, NULL, &e->srv);
    }

    e->color_va       = s->color_va;
    e->w              = w;
    e->h              = h;
    e->pitch          = s->pitch;
    e->bpp            = s->bytes_per_pixel;
    e->aa_sx          = s->aa_sx;
    e->aa_sy          = s->aa_sy;
    e->surface_format = s->surface_format;
    e->has_drawn      = 0;
    e->last_used      = ++s_rt_clock;
    return e;
}

static int tex_is_rt_compatible(uint32_t c)
{
    switch (c) {
    case 0x0B: /* P8 palettized */
    case 0x0C: /* DXT1 */
    case 0x0E: /* DXT3 */
    case 0x0F: /* DXT5 */
        return 0;
    default:
        return 1;
    }
}

static RtEntry *find_rt_by_va(const Nv2aTexture *tex)
{
    int i;
    if (!tex || !tex_is_rt_compatible(tex->color))
        return NULL;
    for (i = 0; i < s_rt_count; i++) {
        if (s_rts[i].color_va == tex->offset && s_rts[i].has_drawn && s_rts[i].srv
         && s_rts[i].w == tex->width && s_rts[i].h == tex->height)
            return &s_rts[i];
    }
    return NULL;
}

static ID3D11DepthStencilView *get_dsv(uint32_t zeta_va, uint32_t w, uint32_t h)
{
    int i;
    DsvEntry *e = NULL;
    if (!zeta_va) return NULL;

    for (i = 0; i < s_dsv_count; i++) {
        if (s_dsvs[i].zeta_va == zeta_va && s_dsvs[i].w == w && s_dsvs[i].h == h) {
            s_dsvs[i].last_used = ++s_dsv_clock;
            return s_dsvs[i].dsv;
        }
    }
    for (i = 0; i < s_dsv_count; i++) {
        if (s_dsvs[i].zeta_va == zeta_va) {
            flush_vsh_queue();
            flush_draw_queue();
            invalidate_pipe_cache();
            e = &s_dsvs[i];
            if (e->dsv) { ID3D11DepthStencilView_Release(e->dsv); e->dsv = NULL; }
            if (e->tex) { ID3D11Texture2D_Release(e->tex);        e->tex = NULL; }
            break;
        }
    }
    if (!e) {
        if (s_dsv_count < MAX_DSVS) {
            e = &s_dsvs[s_dsv_count++];
            memset(e, 0, sizeof(*e));
        } else {
            int victim = 0;
            uint64_t oldest = UINT64_MAX;
            for (i = 0; i < s_dsv_count; i++) {
                if (s_dsvs[i].last_used < oldest) {
                    oldest = s_dsvs[i].last_used;
                    victim = i;
                }
            }
            flush_vsh_queue();
            flush_draw_queue();
            invalidate_pipe_cache();
            e = &s_dsvs[victim];
            if (e->dsv) { ID3D11DepthStencilView_Release(e->dsv); e->dsv = NULL; }
            if (e->tex) { ID3D11Texture2D_Release(e->tex);        e->tex = NULL; }
        }
    }

    {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof(td));
        td.Width            = w;
        td.Height           = h;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_D24_UNORM_S8_UINT;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &e->tex)))
            return NULL;
        ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)e->tex, NULL, &e->dsv);
    }
    e->zeta_va   = zeta_va;
    e->w         = w;
    e->h         = h;
    e->last_used = ++s_dsv_clock;
    return e->dsv;
}

/* ── Texture Cache ─────────────────────────────────────────── */

static uint64_t hash_guest_texture(const Nv2aTexture *tex)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p   = mem + tex->offset;
    uint64_t h = 1469598103934665603ULL;
    size_t approx_bytes;
    int i;

    if (tex->color == 0x0C)
        approx_bytes = ((size_t)(tex->width + 3) / 4) * ((size_t)(tex->height + 3) / 4) * 8u;
    else if (tex->color == 0x0E || tex->color == 0x0F)
        approx_bytes = ((size_t)(tex->width + 3) / 4) * ((size_t)(tex->height + 3) / 4) * 16u;
    else if (tex->pitch)
        approx_bytes = (size_t)tex->pitch * tex->height;
    else if (tex->color == 0x0B || tex->color == 0x00 || tex->color == 0x19)
        approx_bytes = (size_t)tex->width * tex->height;
    else
        approx_bytes = (size_t)tex->width * tex->height * 2u;

    if (approx_bytes < 16) approx_bytes = 16;

    /* Sample 16 dwords spread evenly across the image */
    for (i = 0; i < 16; i++) {
        size_t off = ((approx_bytes - 4u) * (size_t)i) / 15u;
        uint32_t v;
        memcpy(&v, p + (off & ~(size_t)3), 4);
        h ^= v;
        h *= 1099511628211ULL;
    }

    /* For P8 palettized textures, also sample the 256-entry CLUT */
    if (tex->color == 0x0B && tex->palette) {
        const uint32_t *clut = (const uint32_t *)(mem + tex->palette);
        for (i = 0; i < 256; i += 8) {
            h ^= clut[i];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

static int tex_is_opaque_fmt(uint32_t c)
{
    switch (c) {
    case 0x00: case 0x03: case 0x05: case 0x07:
    case 0x11: case 0x13: case 0x1C: case 0x1E:
    case 0x24: case 0x25:
        return 1;
    default:
        return 0;
    }
}

static void swizzle_masks_2d(uint32_t w, uint32_t h, uint32_t *mx, uint32_t *my)
{
    uint32_t x = 0, y = 0, bit = 1, mb = 1;
    while (bit < w || bit < h) {
        if (bit < w) { x |= mb; mb <<= 1; }
        if (bit < h) { y |= mb; mb <<= 1; }
        bit <<= 1;
    }
    *mx = x;
    *my = y;
}

/* Fast inline decoder for the hot Xbox texture formats (P8, swizzled 32-bit,
 * linear 32-bit). Returns 1 if decoded directly into argb_out, 0 to fall back. */
static int fast_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *src = mem + tex->offset;
    uint32_t w = tex->width, h = tex->height;
    uint32_t x, y, mx, my, oy, ox;

    if (tex->color == 0x0B) { /* SZ_I8_A8R8G8B8 (P8 palettized) */
        const uint32_t *clut;
        uint32_t pal_buf[256];
        if (!tex->palette || (w & (w - 1u)) || (h & (h - 1u))) return 0;
        clut = (const uint32_t *)(mem + tex->palette);
        if (w == 32u && h == 32u) {
            uint32_t ci;
            for (ci = 0; ci < 256u; ci++) {
                uint32_t c = clut[ci];
                /* Sims 32x32 P8 Sim portraits use A=0xFE magenta (0xFEFF00FF / 0xFEFD00FD)
                 * as their chroma-key background when quantized from the 64x64 portrait RT. */
                if ((c >> 24) == 0xFEu && ((c >> 16) & 0xFFu) >= 0xFCu
                 && ((c >> 8) & 0xFFu) <= 0x03u && (c & 0xFFu) >= 0xFCu)
                    c = 0x00000000u;
                pal_buf[ci] = c;
            }
            clut = pal_buf;
        }
        swizzle_masks_2d(w, h, &mx, &my);
        oy = 0;
        for (y = 0; y < h; y++) {
            uint32_t *dst = argb_out + (size_t)y * w;
            ox = 0;
            for (x = 0; x < w; x++) {
                dst[x] = clut[src[oy + ox]];
                ox = (ox - mx) & mx;
            }
            oy = (oy - my) & my;
        }
        return 1;
    }
    if (tex->color == 0x06 || tex->color == 0x07 || tex->color == 0x3A) {
        const uint32_t *s32 = (const uint32_t *)src;
        if ((w & (w - 1u)) || (h & (h - 1u))) return 0;
        swizzle_masks_2d(w, h, &mx, &my);
        oy = 0;
        if (tex->color == 0x3A) { /* SZ_A8B8G8R8 -> B8G8R8A8 */
            for (y = 0; y < h; y++) {
                uint32_t *dst = argb_out + (size_t)y * w;
                ox = 0;
                for (x = 0; x < w; x++) {
                    uint32_t p = s32[oy + ox];
                    dst[x] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
                    ox = (ox - mx) & mx;
                }
                oy = (oy - my) & my;
            }
        } else {
            uint32_t or_a = (tex->color == 0x07) ? 0xFF000000u : 0u;
            for (y = 0; y < h; y++) {
                uint32_t *dst = argb_out + (size_t)y * w;
                ox = 0;
                for (x = 0; x < w; x++) {
                    dst[x] = s32[oy + ox] | or_a;
                    ox = (ox - mx) & mx;
                }
                oy = (oy - my) & my;
            }
        }
        return 1;
    }
    if ((tex->color == 0x12 || tex->color == 0x1E) && tex->pitch >= w * 4u) {
        for (y = 0; y < h; y++) {
            uint32_t *dst = argb_out + (size_t)y * w;
            memcpy(dst, src + (size_t)y * tex->pitch, (size_t)w * 4u);
            if (tex->color == 0x1E) {
                for (x = 0; x < w; x++) dst[x] |= 0xFF000000u;
            }
        }
        return 1;
    }
    return 0;
}

static ID3D11ShaderResourceView *get_texture_srv(const Nv2aTexture *tex,
                                                 uint32_t *out_w, uint32_t *out_h,
                                                 RtEntry **out_src_rt)
{
    RtEntry *rt;
    uint64_t hash;
    int i, victim = 0;
    uint64_t oldest = UINT64_MAX;
    TexEntry *slot = NULL;
    size_t npix;

    if (out_src_rt) *out_src_rt = NULL;
    if (!tex || !tex->offset || !tex->width || !tex->height)
        return NULL;

    /* GPU-side render-to-texture / 4x MSAA resolve hit! */
    rt = find_rt_by_va(tex);
    if (rt) {
        if (out_src_rt) *out_src_rt = rt;
        *out_w = rt->w;
        *out_h = rt->h;
        return rt->srv;
    }

    /* Fast O(1) direct-mapped lookup before falling back to cache scan */
    {
        uint32_t bk = ((tex->offset >> 7) ^ (tex->palette >> 7)) & (MAX_TEX_FAST - 1u);
        int16_t fi = s_tex_fast[bk];
        if (fi > 0) {
            TexEntry *e = &s_texs[fi - 1];
            if (e->tex && e->offset == tex->offset && e->w == tex->width
             && e->h == tex->height && e->color == tex->color
             && e->palette == tex->palette && e->pitch == tex->pitch) {
                e->last_used = ++s_tex_clock;
                if (e->validated_frame == s_frame_seq) {
                    *out_w = e->w;
                    *out_h = e->h;
                    return e->srv;
                }
                hash = hash_guest_texture(tex);
                if (e->hash == hash) {
                    e->validated_frame = s_frame_seq;
                    *out_w = e->w;
                    *out_h = e->h;
                    return e->srv;
                }
                slot = e;
                goto do_upload;
            }
        }
    }

    hash = hash_guest_texture(tex);
    s_tex_clock++;

    for (i = 0; i < MAX_TEX_CACHE; i++) {
        TexEntry *e = &s_texs[i];
        if (e->tex && e->offset == tex->offset && e->w == tex->width
         && e->h == tex->height && e->color == tex->color
         && e->palette == tex->palette && e->pitch == tex->pitch) {
            uint32_t bk = ((tex->offset >> 7) ^ (tex->palette >> 7)) & (MAX_TEX_FAST - 1u);
            e->last_used = s_tex_clock;
            s_tex_fast[bk] = (int16_t)(i + 1);
            if (e->hash == hash) {
                e->validated_frame = s_frame_seq;
                *out_w = e->w;
                *out_h = e->h;
                return e->srv;
            }
            /* Content updated in place (e.g. XMV video frame): re-upload! */
            slot = e;
            break;
        }
        if (!e->tex) {
            if (!slot) slot = e;
        } else if (e->last_used < oldest) {
            oldest = e->last_used;
            victim = i;
        }
    }

do_upload:
    {
        LARGE_INTEGER tu0, tu1;
        DXGI_FORMAT upload_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        const void *upload_ptr = NULL;
        UINT upload_pitch = tex->width * 4u;

        QueryPerformanceCounter(&tu0);
        if (!slot) {
            /* Flush queued draws before evicting a cached SRV/texture */
            flush_vsh_queue();
            flush_draw_queue();
            invalidate_pipe_cache();
            slot = &s_texs[victim];
            if (slot->srv) { ID3D11ShaderResourceView_Release(slot->srv); slot->srv = NULL; }
            if (slot->tex) { ID3D11Texture2D_Release(slot->tex);          slot->tex = NULL; }
        } else if (slot->tex) {
            /* Existing texture modified in-place: flush earlier draws first */
            flush_vsh_queue();
            flush_draw_queue();
        }
        s_stat_tex_uploads++;

        /* Zero-copy hardware BC1/BC2/BC3 upload for DXT1/DXT3/DXT5 textures */
        if ((tex->color == 0x0C || tex->color == 0x0E || tex->color == 0x0F)
         && tex->width >= 4u && tex->height >= 4u
         && (tex->width & 3u) == 0u && (tex->height & 3u) == 0u) {
            const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
            uint32_t bb = (tex->color == 0x0C) ? 8u : 16u;
            upload_fmt   = (tex->color == 0x0C) ? DXGI_FORMAT_BC1_UNORM
                         : (tex->color == 0x0E) ? DXGI_FORMAT_BC2_UNORM
                                                : DXGI_FORMAT_BC3_UNORM;
            upload_ptr   = mem + tex->offset;
            upload_pitch = (tex->width >> 2) * bb;
        } else {
            npix = (size_t)tex->width * (size_t)tex->height;
            if (npix > s_decode_cap) {
                uint32_t *nb = (uint32_t *)realloc(s_decode_buf, npix * sizeof(uint32_t));
                if (!nb) return NULL;
                s_decode_buf = nb;
                s_decode_cap = npix;
            }
            if (!fast_decode_texture(tex, s_decode_buf)) {
                if (!nv2a_backend_decode_texture(tex, s_decode_buf))
                    return NULL;
            }
            upload_ptr   = s_decode_buf;
            upload_pitch = tex->width * 4u;
        }

        if (slot->tex && slot->w == tex->width && slot->h == tex->height && slot->dxgi_fmt == upload_fmt) {
            ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)slot->tex,
                                                  0, NULL, upload_ptr, upload_pitch, 0);
        } else {
            D3D11_TEXTURE2D_DESC td;
            D3D11_SUBRESOURCE_DATA sd;
            if (slot->srv) { ID3D11ShaderResourceView_Release(slot->srv); slot->srv = NULL; }
            if (slot->tex) { ID3D11Texture2D_Release(slot->tex);          slot->tex = NULL; }
            memset(&td, 0, sizeof(td));
            td.Width            = tex->width;
            td.Height           = tex->height;
            td.MipLevels        = 1;
            td.ArraySize        = 1;
            td.Format           = upload_fmt;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D11_USAGE_DEFAULT;
            td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
            sd.pSysMem          = upload_ptr;
            sd.SysMemPitch      = upload_pitch;
            sd.SysMemSlicePitch = 0;
            if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &slot->tex)))
                return NULL;
            ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)slot->tex,
                                                  NULL, &slot->srv);
        }
        slot->dxgi_fmt = upload_fmt;
        QueryPerformanceCounter(&tu1);
        s_stat_tex_ticks += (uint64_t)(tu1.QuadPart - tu0.QuadPart);
        if (s_inspect_active == 1) {
            fprintf(stderr, "[INSPECT] tex_up flip=%llu off=0x%08X %ux%u fmt=0x%02X pitch=%u pal=0x%08X\n",
                    (unsigned long long)s_stat_flips, tex->offset, tex->width, tex->height,
                    tex->color, tex->pitch, tex->palette);
        }
    }

    slot->offset          = tex->offset;
    slot->w               = tex->width;
    slot->h               = tex->height;
    slot->color           = tex->color;
    slot->palette         = tex->palette;
    slot->pitch           = tex->pitch;
    slot->hash            = hash;
    slot->last_used       = s_tex_clock;
    slot->validated_frame = s_frame_seq;
    {
        uint32_t bk = ((tex->offset >> 7) ^ (tex->palette >> 7)) & (MAX_TEX_FAST - 1u);
        s_tex_fast[bk] = (int16_t)((slot - s_texs) + 1);
    }

    *out_w = slot->w;
    *out_h = slot->h;
    return slot->srv;
}

/* ── Vertex Buffer Ring Upload ─────────────────────────────── */

static int ensure_vb(uint32_t count)
{
    if (count <= s_vb_cap && s_vb)
        return 1;
    if (s_vb) { ID3D11Buffer_Release(s_vb); s_vb = NULL; }
    {
        uint32_t cap = s_vb_cap ? s_vb_cap : 131072u; /* 128K verts (~3.6 MB) ring */
        D3D11_BUFFER_DESC bd;
        while (cap < count) cap *= 2u;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth      = cap * (uint32_t)sizeof(GpuVert);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb)))
            return 0;
        s_vb_cap  = cap;
        s_vb_head = 0;
        s_pipe_valid = 0;
    }
    return 1;
}

static void flush_draw_queue(void)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t k;
    LARGE_INTEGER t0, t1;

    if (!s_q_cmds_n || !s_q_verts_n) {
        s_q_cmds_n = s_q_verts_n = s_q_vscbs_n = s_q_pscbs_n = 0;
        return;
    }

    QueryPerformanceCounter(&t0);
    if (!ensure_vb(s_q_verts_n)) {
        s_q_cmds_n = s_q_verts_n = s_q_vscbs_n = s_q_pscbs_n = 0;
        return;
    }

    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, s_q_verts, (size_t)s_q_verts_n * sizeof(GpuVert));
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    }

    if (!s_pipe_valid) {
        UINT stride = sizeof(GpuVert), off = 0;
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs_scissor);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, s_main_il);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &off);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_main_vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_vs_cb);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_main_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_ps_cb);
        s_pipe_valid = 1;
    }

    for (k = 0; k < s_q_cmds_n; k++) {
        const QCmd *c = &s_q_cmds[k];
        const VsCB *vcb = &s_q_vscbs[c->vs_idx];
        const PsCB *pcb = &s_q_pscbs[c->ps_idx];

        if (!s_vs_cb_valid || memcmp(vcb, &s_last_vs_cb, sizeof(VsCB)) != 0) {
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vs_cb, 0,
                                                  D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, vcb, sizeof(VsCB));
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vs_cb, 0);
                s_last_vs_cb = *vcb;
                s_vs_cb_valid = 1;
            }
        }
        if (!s_ps_cb_valid || memcmp(pcb, &s_last_ps_cb, sizeof(PsCB)) != 0) {
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ps_cb, 0,
                                                  D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, pcb, sizeof(PsCB));
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ps_cb, 0);
                s_last_ps_cb = *pcb;
                s_ps_cb_valid = 1;
            }
        }

        if (c->rt->rtv != s_bound_rtv || c->dsv != s_bound_dsv) {
            if (s_bound_srv && s_bound_srv == c->rt->srv) {
                ID3D11ShaderResourceView *null_srv = NULL;
                ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &null_srv);
                s_bound_srv = NULL;
            }
            if (s_bound_srv1 && s_bound_srv1 == c->rt->srv) {
                ID3D11ShaderResourceView *null_srv = NULL;
                ID3D11DeviceContext_PSSetShaderResources(s_ctx, 1, 1, &null_srv);
                s_bound_srv1 = NULL;
            }
            ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &c->rt->rtv, c->dsv);
            s_bound_rtv = c->rt->rtv;
            s_bound_dsv = c->dsv;
        }
        if (c->rt->w != s_bound_rt_w || c->rt->h != s_bound_rt_h) {
            D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)c->rt->w, (float)c->rt->h, 0.0f, 1.0f };
            ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
            s_bound_rt_w = c->rt->w;
            s_bound_rt_h = c->rt->h;
        }
        if (c->sc[0] != s_bound_sc[0] || c->sc[1] != s_bound_sc[1]
         || c->sc[2] != s_bound_sc[2] || c->sc[3] != s_bound_sc[3]) {
            D3D11_RECT sc = { c->sc[0], c->sc[1], c->sc[2], c->sc[3] };
            ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
            s_bound_sc[0] = c->sc[0]; s_bound_sc[1] = c->sc[1];
            s_bound_sc[2] = c->sc[2]; s_bound_sc[3] = c->sc[3];
        }
        if (c->bs != s_bound_bs || c->bcol != s_bound_bcol) {
            float bfac[4];
            unpack_argb_f4(c->bcol, bfac);
            ID3D11DeviceContext_OMSetBlendState(s_ctx, c->bs, bfac, 0xFFFFFFFFu);
            s_bound_bs   = c->bs;
            s_bound_bcol = c->bcol;
        }
        if (c->dss != s_bound_dss || c->stref != s_bound_stref) {
            ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, c->dss, c->stref);
            s_bound_dss   = c->dss;
            s_bound_stref = c->stref;
        }
        if (c->srv != s_bound_srv) {
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, (ID3D11ShaderResourceView **)&c->srv);
            s_bound_srv = c->srv;
        }
        if (c->srv && c->smp != s_bound_smp) {
            ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, (ID3D11SamplerState **)&c->smp);
            s_bound_smp = c->smp;
        }
        if (c->srv1 != s_bound_srv1) {
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, 1, 1, (ID3D11ShaderResourceView **)&c->srv1);
            s_bound_srv1 = c->srv1;
        }
        if (c->srv1 && c->smp1 != s_bound_smp1) {
            ID3D11DeviceContext_PSSetSamplers(s_ctx, 1, 1, (ID3D11SamplerState **)&c->smp1);
            s_bound_smp1 = c->smp1;
        }
        ID3D11DeviceContext_Draw(s_ctx, c->count, c->start_vert);
        s_stat_gpu_draws++;
    }

    s_q_cmds_n = s_q_verts_n = s_q_vscbs_n = s_q_pscbs_n = 0;
    QueryPerformanceCounter(&t1);
    s_stat_draw_ticks  += (uint64_t)(t1.QuadPart - t0.QuadPart);
    s_stat_flush_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
}

/* ── Backend Callbacks ─────────────────────────────────────── */

static void d3d11_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                        uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    RtEntry *rt;
    ID3D11DepthStencilView *dsv;
    if (!d3d11_ensure_init()) return;

    flush_vsh_queue();
    flush_draw_queue();

    rt = get_rt(s);
    if (!rt || !rt->rtv) return;
    dsv = get_dsv(rs->zeta_va, rt->w, rt->h);

    /* Colour clear (NV097_CLEAR_SURFACE colour bits are 0xF0) */
    if (flags & 0xF0u) {
        float col[4];
        unpack_argb_f4(argb, col);
        if (s->clip_x == 0 && s->clip_y == 0 && s->width >= rt->w && s->height >= rt->h) {
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rt->rtv, col);
        } else {
            /* Sub-rect clear via 2-triangle solid quad */
            GpuVert q[6];
            float x0 = (float)s->clip_x, y0 = (float)s->clip_y;
            float x1 = (float)(s->clip_x + s->width), y1 = (float)(s->clip_y + s->height);
            D3D11_MAPPED_SUBRESOURCE m;
            D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)rt->w, (float)rt->h, 0.0f, 1.0f };
            D3D11_RECT sc = { (LONG)s->clip_x, (LONG)s->clip_y,
                              (LONG)(s->clip_x + s->width), (LONG)(s->clip_y + s->height) };
            UINT stride = sizeof(GpuVert), off = 0;

            memset(q, 0, sizeof(q));
            q[0].x = x0; q[0].y = y0; q[0].z = 0; q[0].rhw = 1; q[0].diffuse = argb;
            q[1].x = x1; q[1].y = y0; q[1].z = 0; q[1].rhw = 1; q[1].diffuse = argb;
            q[2].x = x1; q[2].y = y1; q[2].z = 0; q[2].rhw = 1; q[2].diffuse = argb;
            q[3] = q[0]; q[4] = q[2];
            q[5].x = x0; q[5].y = y1; q[5].z = 0; q[5].rhw = 1; q[5].diffuse = argb;

            if (ensure_vb(6)) {
                if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    memcpy(m.pData, q, sizeof(q));
                    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
                }
                if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vs_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    VsCB *cb = (VsCB *)m.pData;
                    memset(cb, 0, sizeof(*cb));
                    cb->inv_rt_size[0] = 1.0f / (float)rt->w;
                    cb->inv_rt_size[1] = 1.0f / (float)rt->h;
                    cb->inv_tex_size[0] = 1.0f; cb->inv_tex_size[1] = 1.0f;
                    cb->z_scale = 1.0f;
                    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vs_cb, 0);
                }
                if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ps_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    memset(m.pData, 0, sizeof(PsCB));
                    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ps_cb, 0);
                }
                ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rt->rtv, NULL);
                ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
                ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
                ID3D11DeviceContext_RSSetState(s_ctx, s_rs_scissor);
                ID3D11DeviceContext_OMSetBlendState(s_ctx, s_bs_opaque, NULL, 0xFFFFFFFFu);
                ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_dss_off, 0);
                ID3D11DeviceContext_IASetInputLayout(s_ctx, s_main_il);
                ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &off);
                ID3D11DeviceContext_VSSetShader(s_ctx, s_main_vs, NULL, 0);
                ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_vs_cb);
                ID3D11DeviceContext_PSSetShader(s_ctx, s_main_ps, NULL, 0);
                ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_ps_cb);
                ID3D11DeviceContext_Draw(s_ctx, 6, 0);
                s_vb_head = 6;
                invalidate_pipe_cache();
            }
        }
        rt->has_drawn = 1;
        s_last_rt = rt;
    }

    /* Depth / stencil clear (Z=0x1, Stencil=0x2) */
    if (dsv && (flags & 0x3u)) {
        UINT cflags = 0;
        float zval = (float)(zstencil >> 8) / 16777215.0f;
        UINT8 sval = (UINT8)(zstencil & 0xFFu);
        if (flags & 0x1u) cflags |= D3D11_CLEAR_DEPTH;
        if (flags & 0x2u) cflags |= D3D11_CLEAR_STENCIL;
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, dsv, cflags, zval, sval);
    }
}

static void d3d11_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    RtEntry *rt;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv = NULL, *srv1 = NULL;
    ID3D11SamplerState *smp = NULL, *smp1 = NULL;
    uint32_t tex_w = 1, tex_h = 1, tex1_w = 1, tex1_h = 1;
    uint32_t out_n = 0, i, start_vert = 0;
    const Nv2aRenderState *rs = b->state;
    const Nv2aCombiner *rc = (const Nv2aCombiner *)b->combiner;
    int tex_stage_on, tex1_stage_on;
    LARGE_INTEGER t0, t1;

    if (!b || !b->count || !b->vertices) return;
    if (!d3d11_ensure_init()) return;
    if (s_vq_cmds_n > 0) flush_vsh_queue();

    QueryPerformanceCounter(&t0);
    s_stat_draws++;
    s_frame_draws++;
    if (rc && (rc->stage_program & ~0x1Fu) != 0u)
        s_stat_multitex++;
    if (s_inspect_mode < 0) {
        const char *e = getenv("RECOMP_D3D11_INSPECT");
        s_inspect_mode = (e && atoi(e) != 0) ? atoi(e) : 0;
    }
    if (s_inspect_active == 1) {
        static uint64_t seen_k[64];
        static int seen_n;
        uint64_t key = ((uint64_t)rs->stencil_enable << 60)
                     ^ ((uint64_t)(rs->stencil_func & 0xFF) << 52)
                     ^ ((uint64_t)(rs->stencil_zpass & 0xFFFF) << 36)
                     ^ ((uint64_t)(rs->blend_enable ? rs->blend_src : 0) << 20)
                     ^ ((uint64_t)(rs->blend_enable ? rs->blend_dst : 0) << 4)
                     ^ ((uint64_t)(rc ? rc->stage_program : 0) << 12)
                     ^ (rs->color_mask ? 1u : 2u);
        int dup = 0, si;
        for (si = 0; si < seen_n; si++) if (seen_k[si] == key) { dup = 1; break; }
        if (!dup && seen_n < 64) {
            seen_k[seen_n++] = key;
            fprintf(stderr, "[INSPECT] pass#%d n=%u surf=0x%08X %ux%u | st=%u fn=%X ref=%X ops=%X/%X/%X cmask=%08X | "
                    "bl=%u %X/%X eq=%X bcol=%08X | cull=%u/%X/%X | z=%u/%X/w%u | tex0=%08X(%02X) | "
                    "rc:ctl=%08X prog=%05X f0=%08X f1=%08X s0=%08X/%08X/%08X/%08X c0=%08X\n",
                    seen_n, b->count, s->color_va, s->width, s->height,
                    rs->stencil_enable, rs->stencil_func, rs->stencil_ref,
                    rs->stencil_fail, rs->stencil_zfail, rs->stencil_zpass, rs->color_mask,
                    rs->blend_enable, rs->blend_src, rs->blend_dst, rs->blend_eq, rs->blend_color,
                    rs->cull_enable, rs->cull_face, rs->front_face,
                    rs->depth_test_enable, rs->depth_func, rs->depth_write,
                    b->texture ? b->texture->offset : 0u, b->texture ? b->texture->color : 0u,
                    rc ? rc->control : 0u, rc ? rc->stage_program : 0u,
                    rc ? rc->final0 : 0u, rc ? rc->final1 : 0u,
                    rc ? rc->color_icw[0] : 0u, rc ? rc->color_ocw[0] : 0u,
                    rc ? rc->alpha_icw[0] : 0u, rc ? rc->alpha_ocw[0] : 0u,
                    rc ? rc->factor0[0] : 0u);
        }
    }

    rt = get_rt(s);
    if (!rt || !rt->rtv) return;
    dsv = get_dsv(rs->zeta_va, rt->w, rt->h);

    /* Texture binding (check combiner stage_program if present) */
    RtEntry *src_rt = NULL, *src_rt1 = NULL;
    tex_stage_on  = (!rc || (rc->stage_program & 0x1Fu) != 0u);
    tex1_stage_on = (rc && ((rc->stage_program >> 5) & 0x1Fu) != 0u);
    if (b->texture && tex_stage_on) {
        srv = get_texture_srv(b->texture, &tex_w, &tex_h, &src_rt);
        if (srv)
            smp = get_sampler_state(b->texture);
    }
    if (b->texture1 && tex1_stage_on) {
        srv1 = get_texture_srv(b->texture1, &tex1_w, &tex1_h, &src_rt1);
        if (srv1)
            smp1 = get_sampler_state(b->texture1);
    }

    /* Prevent hazard: never bind the active RT as both RTV and SRV */
    if (srv && srv == rt->srv) {
        srv = NULL;
        src_rt = NULL;
    }
    if (srv1 && srv1 == rt->srv) {
        srv1 = NULL;
        src_rt1 = NULL;
    }

    /* Flush queue before sampling a recently rendered RT, or if queue is full */
    if (src_rt || src_rt1 || s_q_cmds_n >= MAX_Q_CMDS
     || s_q_vscbs_n >= MAX_Q_VSCB - 1u || s_q_pscbs_n >= MAX_Q_PSCB - 1u
     || s_q_verts_n + b->count > 262144u) {
        QueryPerformanceCounter(&t1);
        s_stat_draw_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
        flush_draw_queue();
        QueryPerformanceCounter(&t0);
    }

    if (s_q_verts_n + b->count > s_q_verts_cap) {
        uint32_t cap = s_q_verts_cap ? s_q_verts_cap : 65536u;
        GpuVert *nv;
        while (cap < s_q_verts_n + b->count) cap *= 2u;
        nv = (GpuVert *)realloc(s_q_verts, (size_t)cap * sizeof(GpuVert));
        if (!nv) return;
        s_q_verts = nv;
        s_q_verts_cap = cap;
    }

    start_vert = s_q_verts_n;
    {
        int do_cull = s_cull_mode && rs->cull_enable;
        if (!do_cull) {
            out_n = (b->count / 3u) * 3u;
            if (out_n)
                memcpy(s_q_verts + start_vert, b->vertices, (size_t)out_n * sizeof(GpuVert));
        } else {
            int front_cw = (rs->front_face == GL_CW);
            uint32_t cf = rs->cull_face ? rs->cull_face : GL_BACK;
            GpuVert *dst = s_q_verts + start_vert;
            for (i = 0; i + 2 < b->count; i += 3) {
                const Nv2aVertex *v0 = &b->vertices[i];
                const Nv2aVertex *v1 = &b->vertices[i + 1];
                const Nv2aVertex *v2 = &b->vertices[i + 2];
                float area = (v1->x - v0->x) * (v2->y - v0->y)
                           - (v2->x - v0->x) * (v1->y - v0->y);
                int is_cw = (area > 0.0f);
                int is_front = (is_cw == front_cw);
                if (cf == GL_FRONT_AND_BACK) continue;
                if (cf == GL_BACK  && !is_front) continue;
                if (cf == GL_FRONT &&  is_front) continue;
                memcpy(&dst[out_n], v0, 3 * sizeof(GpuVert));
                out_n += 3;
            }
        }
    }
    if (!out_n) {
        QueryPerformanceCounter(&t1);
        s_stat_draw_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
        return;
    }
    s_q_verts_n += out_n;

    {
        VsCB vs_cb;
        PsCB ps_cb;
        uint16_t vs_idx, ps_idx;
        ID3D11BlendState *bs = get_blend_state(rs);
        ID3D11DepthStencilState *dss = get_dss_state(rs, dsv != NULL);
        uint32_t stref = rs->stencil_ref & 0xFFu;
        LONG sc_l = (LONG)s->clip_x, sc_t = (LONG)s->clip_y;
        LONG sc_r = (LONG)(s->clip_x + s->width), sc_b = (LONG)(s->clip_y + s->height);

        memset(&vs_cb, 0, sizeof(vs_cb));
        vs_cb.inv_rt_size[0]   = 1.0f / (float)rt->w;
        vs_cb.inv_rt_size[1]   = 1.0f / (float)rt->h;
        vs_cb.inv_tex_size[0]  = (srv && tex_w) ? (1.0f / (float)tex_w) : 1.0f;
        vs_cb.inv_tex_size[1]  = (srv && tex_h) ? (1.0f / (float)tex_h) : 1.0f;
        vs_cb.inv_tex1_size[0] = (srv1 && tex1_w) ? (1.0f / (float)tex1_w) : 1.0f;
        vs_cb.inv_tex1_size[1] = (srv1 && tex1_h) ? (1.0f / (float)tex1_h) : 1.0f;
        if (rs->depth_max > 65536.0f || rs->depth_max <= 0.0f)
            vs_cb.z_scale = 1.0f / 16777215.0f;
        else if (rs->depth_max > 1.5f)
            vs_cb.z_scale = 1.0f / 65535.0f;
        else
            vs_cb.z_scale = 1.0f;

        if (s_q_vscbs_n > 0 && memcmp(&vs_cb, &s_q_vscbs[s_q_vscbs_n - 1], sizeof(vs_cb)) == 0) {
            vs_idx = (uint16_t)(s_q_vscbs_n - 1);
        } else {
            vs_idx = (uint16_t)s_q_vscbs_n;
            s_q_vscbs[s_q_vscbs_n++] = vs_cb;
        }

        memset(&ps_cb, 0, sizeof(ps_cb));
        ps_cb.flags[0] = srv ? (tex_is_opaque_fmt(b->texture->color) ? 2u : 1u) : 0u;
        ps_cb.flags[1] = rc ? 1u : 0u;
        ps_cb.flags[2] = rs->alpha_test_enable ? 1u : 0u;
        ps_cb.flags[3] = rs->alpha_func ? rs->alpha_func : GL_ALWAYS;
        ps_cb.alpha_misc[0] = (float)(rs->alpha_ref & 0xFFu) / 255.0f;
        ps_cb.alpha_misc[1] = srv1 ? (tex_is_opaque_fmt(b->texture1->color) ? 2.0f : 1.0f) : 0.0f;
        unpack_argb_f4(b->fog_color, ps_cb.fog_col);
        if (rc) {
            int st, nst = (int)(rc->control & 0xFFu);
            if (nst > 8) nst = 8;
            for (st = 0; st < nst; st++) {
                ps_cb.stages[st][0] = rc->color_icw[st];
                ps_cb.stages[st][1] = rc->color_ocw[st];
                ps_cb.stages[st][2] = rc->alpha_icw[st];
                ps_cb.stages[st][3] = rc->alpha_ocw[st];
                unpack_argb_f4(rc->factor0[st], ps_cb.factor0[st]);
                unpack_argb_f4(rc->factor1[st], ps_cb.factor1[st]);
            }
            unpack_argb_f4(rc->final_c0, ps_cb.final_c0);
            unpack_argb_f4(rc->final_c1, ps_cb.final_c1);
            ps_cb.rc_meta[0] = rc->control;
            ps_cb.rc_meta[1] = rc->final0;
            ps_cb.rc_meta[2] = rc->final1;
            ps_cb.rc_meta[3] = rc->stage_program;
        }

        if (s_q_pscbs_n > 0 && memcmp(&ps_cb, &s_q_pscbs[s_q_pscbs_n - 1], sizeof(ps_cb)) == 0) {
            ps_idx = (uint16_t)(s_q_pscbs_n - 1);
        } else {
            ps_idx = (uint16_t)s_q_pscbs_n;
            s_q_pscbs[s_q_pscbs_n++] = ps_cb;
        }

        if (s_q_cmds_n > 0) {
            QCmd *prev = &s_q_cmds[s_q_cmds_n - 1];
            if (prev->rt == rt && prev->dsv == dsv && prev->bs == bs && prev->bcol == rs->blend_color
             && prev->dss == dss && prev->stref == stref && prev->srv == srv && prev->smp == smp
             && prev->srv1 == srv1 && prev->smp1 == smp1
             && prev->sc[0] == sc_l && prev->sc[1] == sc_t && prev->sc[2] == sc_r && prev->sc[3] == sc_b
             && prev->vs_idx == vs_idx && prev->ps_idx == ps_idx
             && prev->start_vert + prev->count == start_vert) {
                prev->count += out_n;
                goto queued_done;
            }
        }
        {
            QCmd *c = &s_q_cmds[s_q_cmds_n++];
            c->rt = rt; c->dsv = dsv; c->bs = bs; c->bcol = rs->blend_color;
            c->dss = dss; c->stref = stref; c->srv = srv; c->smp = smp;
            c->srv1 = srv1; c->smp1 = smp1;
            c->sc[0] = sc_l; c->sc[1] = sc_t; c->sc[2] = sc_r; c->sc[3] = sc_b;
            c->vs_idx = vs_idx; c->ps_idx = ps_idx;
            c->start_vert = start_vert; c->count = out_n;
        }
    }
queued_done:
    rt->has_drawn    = 1;
    if (rt->w > 256u || rt->h > 256u) {
        s_present_rt     = rt;
        s_resolve_src_rt = (src_rt && src_rt->w > rt->w) ? src_rt : NULL;
    }
    s_last_rt        = rt;

    QueryPerformanceCounter(&t1);
    s_stat_draw_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
}

static uint16_t pack_bgra_to_16(uint32_t bgra, uint32_t color_fmt)
{
    uint32_t b =  bgra        & 0xFFu;
    uint32_t g = (bgra >>  8) & 0xFFu;
    uint32_t r = (bgra >> 16) & 0xFFu;
    uint32_t a = (bgra >> 24) & 0xFFu;
    if (color_fmt == 0x1u || color_fmt == 0x2u) {
        /* A1R5G5B5 / X1R5G5B5 */
        return (uint16_t)(((a >= 128u ? 1u : 0u) << 15)
                        | ((r >> 3) << 10)
                        | ((g >> 3) << 5)
                        |  (b >> 3));
    }
    if (color_fmt == 0xAu) {
        /* A4R4G4B4 */
        return (uint16_t)(((a >> 4) << 12) | ((r >> 4) << 8)
                        | ((g >> 4) << 4)  |  (b >> 4));
    }
    /* Default 0x3: R5G6B5 */
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint32_t swizzle_deposit_bits(uint32_t v, uint32_t mask)
{
    uint32_t res = 0, bit = 1;
    while (mask) {
        uint32_t lsb = mask & (0u - mask);
        if (v & bit) res |= lsb;
        mask ^= lsb;
        bit <<= 1;
    }
    return res;
}

static void sync_rt_to_guest(RtEntry *rt)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *dst_mem;
    uint32_t x, y;
    int is_swz;
    uint32_t color_fmt;

    if (!s_ready || !rt || !rt->tex || !rt->color_va)
        return;

    is_swz = (((rt->surface_format >> 8) & 0xFu) == 2u);
    color_fmt = rt->surface_format & 0xFu;
    if (!is_swz && !rt->pitch)
        return;

    if (!rt->staging || rt->staging_w != rt->w || rt->staging_h != rt->h) {
        D3D11_TEXTURE2D_DESC td;
        if (rt->staging) { ID3D11Texture2D_Release(rt->staging); rt->staging = NULL; }
        memset(&td, 0, sizeof(td));
        td.Width            = rt->w;
        td.Height           = rt->h;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_STAGING;
        td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &rt->staging)))
            return;
        rt->staging_w = rt->w;
        rt->staging_h = rt->h;
    }

    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)rt->staging,
                                     (ID3D11Resource *)rt->tex);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)rt->staging,
                                       0, D3D11_MAP_READ, 0, &m)))
        return;

    dst_mem = (uint8_t *)xbox_GetMemoryOffset() + rt->color_va;
    if (is_swz) {
        uint32_t swz_w = 1u << ((rt->surface_format >> 16) & 0xFu);
        uint32_t swz_h = 1u << ((rt->surface_format >> 24) & 0xFu);
        uint32_t mx = 0, my = 0, bit = 1, bw = swz_w, bh = swz_h;
        if (swz_w < rt->w) swz_w = rt->w;
        if (swz_h < rt->h) swz_h = rt->h;
        bw = swz_w; bh = swz_h;
        while (bw > 1u || bh > 1u) {
            if (bw > 1u) { mx |= bit; bit <<= 1; bw >>= 1; }
            if (bh > 1u) { my |= bit; bit <<= 1; bh >>= 1; }
        }
        for (y = 0; y < rt->h; y++) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            uint32_t oy = swizzle_deposit_bits(y, my);
            for (x = 0; x < rt->w; x++) {
                uint32_t off = oy | swizzle_deposit_bits(x, mx);
                if (rt->bpp == 2u)
                    ((uint16_t *)dst_mem)[off] = pack_bgra_to_16(src[x], color_fmt);
                else
                    ((uint32_t *)dst_mem)[off] = src[x];
            }
        }
    } else if (rt->bpp == 2u) {
        uint32_t max_w = rt->pitch / 2u;
        if (max_w > rt->w) max_w = rt->w;
        for (y = 0; y < rt->h; y++) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            uint16_t *dst = (uint16_t *)(dst_mem + (size_t)y * rt->pitch);
            for (x = 0; x < max_w; x++)
                dst[x] = pack_bgra_to_16(src[x], color_fmt);
        }
    } else if (rt->bpp == 4u || rt->bpp == 0u) {
        uint32_t copy_bytes = rt->w * 4u;
        if (copy_bytes > rt->pitch) copy_bytes = rt->pitch;
        for (y = 0; y < rt->h; y++) {
            memcpy(dst_mem + (size_t)y * rt->pitch,
                   (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
                   copy_bytes);
        }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)rt->staging, 0);
}

static void d3d11_present_sync(void)
{
    /* Only sync permanent display/AA surfaces below 0x81000000 (e.g. 0x804F4000
     * and 0x809A4000) plus the currently active s_present_rt. Never touch old
     * off-screen render targets in the EOR heap (0x81000000+), which get freed
     * and reused across scene transitions. */
    int i;
    flush_vsh_queue();
    flush_draw_queue();
    for (i = 0; i < s_rt_count; i++) {
        if (s_rts[i].has_drawn && (s_rts[i].color_va < 0x81000000u || &s_rts[i] == s_present_rt))
            sync_rt_to_guest(&s_rts[i]);
    }
}

static void d3d11_flip(void)
{
    MSG msg;
    RtEntry *rt;

    if (!d3d11_ensure_init()) return;
    flush_vsh_queue();
    flush_draw_queue();
    rt = s_resolve_src_rt ? s_resolve_src_rt
       : (s_present_rt ? s_present_rt : s_last_rt);

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            ExitProcess(0);
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (!s_headless && s_back_rtv && rt && rt->srv) {
        static const float k_black[4] = { 0, 0, 0, 1 };
        ID3D11ShaderResourceView *null_srv = NULL;
        float aspect = 4.0f / 3.0f;
        float win_aspect = (s_win_h > 0) ? ((float)s_win_w / (float)s_win_h) : aspect;
        D3D11_VIEWPORT vp;

        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        if (s_aspect_stretch) {
            vp.TopLeftX = 0.0f;
            vp.TopLeftY = 0.0f;
            vp.Width    = (float)s_win_w;
            vp.Height   = (float)s_win_h;
        } else if (win_aspect > aspect) {
            vp.Height   = (float)s_win_h;
            vp.Width    = vp.Height * aspect;
            vp.TopLeftX = ((float)s_win_w - vp.Width) * 0.5f;
            vp.TopLeftY = 0.0f;
        } else {
            vp.Width    = (float)s_win_w;
            vp.Height   = vp.Width / aspect;
            vp.TopLeftX = 0.0f;
            vp.TopLeftY = ((float)s_win_h - vp.Height) * 0.5f;
        }

        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_back_rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back_rtv, k_black);
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs_noscissor);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, s_bs_opaque, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_dss_off, 0);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &rt->srv);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_smp);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &null_srv);

        IDXGISwapChain_Present(s_swap, s_vsync ? 1u : 0u, 0);
        invalidate_pipe_cache();
    }

    s_frame_seq++;
    s_stat_flips++;
    if (s_inspect_mode > 0) {
        if (s_inspect_active == 1) s_inspect_active = 2; /* done after 1 full frame */
        else if (s_inspect_active == 0 && s_frame_draws >= (uint32_t)s_inspect_mode) {
            s_inspect_active = 1;
            fprintf(stderr, "[INSPECT] === capturing flip %llu (prev frame had %u draws) ===\n",
                    (unsigned long long)s_stat_flips, s_frame_draws);
        }
    }
    s_frame_draws = 0;
    if ((s_stat_flips % 150) == 0) {
        static int want_prof = -1;
        if (want_prof < 0) {
            const char *pe = getenv("RECOMP_GPU_PROFILE");
            want_prof = (pe && *pe && atoi(pe) != 0) ? 1 : 0;
        }
        if (want_prof) {
            if (!s_qpc_freq.QuadPart)
                QueryPerformanceFrequency(&s_qpc_freq);
            double draw_ms = s_qpc_freq.QuadPart
                ? ((double)s_stat_draw_ticks * 1000.0 / (double)s_qpc_freq.QuadPart)
                : 0.0;
            double tex_ms = s_qpc_freq.QuadPart
                ? ((double)s_stat_tex_ticks * 1000.0 / (double)s_qpc_freq.QuadPart)
                : 0.0;
            double flush_ms = s_qpc_freq.QuadPart
                ? ((double)s_stat_flush_ticks * 1000.0 / (double)s_qpc_freq.QuadPart)
                : 0.0;
            fprintf(stderr, "[D3D11] flip %llu: avg %.2f ms/f (tex %.2f, flush %.2f; %llu->%llu draws, %llu vsh_d/%lluK vsh_v, %llu tex_up, %llu multitex)\n",
                    (unsigned long long)s_stat_flips, draw_ms / 150.0,
                    tex_ms / 150.0, flush_ms / 150.0,
                    (unsigned long long)s_stat_draws,
                    (unsigned long long)s_stat_gpu_draws,
                    (unsigned long long)s_stat_vsh_draws,
                    (unsigned long long)(s_stat_vsh_verts / 1000u),
                    (unsigned long long)s_stat_tex_uploads,
                    (unsigned long long)s_stat_multitex);
        }
        s_stat_draw_ticks  = 0;
        s_stat_tex_ticks   = 0;
        s_stat_flush_ticks = 0;
        s_stat_draws       = 0;
        s_stat_gpu_draws   = 0;
        s_stat_vsh_draws   = 0;
        s_stat_vsh_verts   = 0;
        s_stat_tex_uploads = 0;
        s_stat_multitex    = 0;
    }
}

/* ── GPU-Side NV2A Vertex Shader Translation & Batched Execution ── */

static VshEntry *get_or_compile_vsh(const Nv2aVshBatch *b)
{
    uint64_t key = b->prog_hash ^ ((uint64_t)b->in_mask << 48);
    int i;
    VshEntry *e;
    static char hlsl_buf[131072];
    ID3DBlob *vsb;
    D3D11_INPUT_ELEMENT_DESC il[16];
    UINT il_n = 0;
    uint32_t m = b->in_mask;

    for (i = 0; i < s_vsh_count; i++) {
        if (s_vsh_cache[i].key == key && s_vsh_cache[i].in_mask == b->in_mask)
            return s_vsh_cache[i].failed ? NULL : &s_vsh_cache[i];
    }

    if (s_vsh_count >= MAX_VSH_CACHE)
        return NULL;

    e = &s_vsh_cache[s_vsh_count++];
    memset(e, 0, sizeof(*e));
    e->key     = key;
    e->in_mask = b->in_mask;

    if (!nv2a_vsh_emit_hlsl(b->program, b->insn_count, b->in_mask, hlsl_buf, sizeof(hlsl_buf))) {
        e->failed = 1;
        return NULL;
    }

    vsb = compile_hlsl(hlsl_buf, "vs_4_0", "nv2a_vsh_gpu");
    if (!vsb) {
        e->failed = 1;
        return NULL;
    }

    if (FAILED(ID3D11Device_CreateVertexShader(
            s_dev, ID3D10Blob_GetBufferPointer(vsb),
            ID3D10Blob_GetBufferSize(vsb), NULL, &e->vs))) {
        ID3D10Blob_Release(vsb);
        e->failed = 1;
        return NULL;
    }

    if (m == 0u) {
        il[0].SemanticName         = "TEXCOORD";
        il[0].SemanticIndex        = 0;
        il[0].Format               = DXGI_FORMAT_R32G32B32A32_FLOAT;
        il[0].InputSlot            = 0;
        il[0].AlignedByteOffset    = 0;
        il[0].InputSlotClass       = D3D11_INPUT_PER_VERTEX_DATA;
        il[0].InstanceDataStepRate = 0;
        il_n = 1;
    } else {
        uint32_t a;
        for (a = 0; a < 16u; a++) {
            if (!(m & (1u << a))) continue;
            il[il_n].SemanticName         = "TEXCOORD";
            il[il_n].SemanticIndex        = a;
            il[il_n].Format               = DXGI_FORMAT_R32G32B32A32_FLOAT;
            il[il_n].InputSlot            = 0;
            il[il_n].AlignedByteOffset    = il_n * 16u;
            il[il_n].InputSlotClass       = D3D11_INPUT_PER_VERTEX_DATA;
            il[il_n].InstanceDataStepRate = 0;
            il_n++;
        }
    }
    e->stride = il_n * 16u;

    if (FAILED(ID3D11Device_CreateInputLayout(
            s_dev, il, il_n,
            ID3D10Blob_GetBufferPointer(vsb),
            ID3D10Blob_GetBufferSize(vsb), &e->il))) {
        ID3D11VertexShader_Release(e->vs);
        e->vs = NULL;
        ID3D10Blob_Release(vsb);
        e->failed = 1;
        return NULL;
    }
    ID3D10Blob_Release(vsb);

    if (getenv("RECOMP_GPU_PROFILE")) {
        fprintf(stderr, "[D3D11-VSH] Compiled GPU shader #%d (insns=%u, in_mask=0x%04X, stride=%u)\n",
                s_vsh_count, b->insn_count, b->in_mask, e->stride);
    }
    return e;
}

static __forceinline void unpack_vsh_attr(const Nv2aVshAttr *a, uint32_t idx, float dst[4])
{
    const uint8_t *p;
    uint32_t i, elem_bytes;

    if (!a->size || !a->stride || !a->base) {
        dst[0] = a->imm[0]; dst[1] = a->imm[1];
        dst[2] = a->imm[2]; dst[3] = a->imm[3];
        return;
    }
    {
        uint32_t comps = (a->size < 4u) ? a->size : 4u;
        switch (a->type) {
        case 2: elem_bytes = comps * 4u; break; /* FLOAT */
        case 1:
        case 5: elem_bytes = comps * 2u; break; /* SNORM16 / SINT16 */
        case 4: elem_bytes = comps;      break; /* UNORM8 */
        default: elem_bytes = 4u;        break; /* D3DCOLOR / CMP */
        }
    }
    if (a->max_bytes && (uint64_t)idx * a->stride + elem_bytes > a->max_bytes) {
        dst[0] = 0.0f; dst[1] = 0.0f; dst[2] = 0.0f; dst[3] = 1.0f;
        return;
    }
    p = a->base + (size_t)idx * a->stride;
    switch (a->type) {
    case 2: /* FLOAT (most common) */
        if (a->size == 3) {
            const float *fp = (const float *)p;
            dst[0] = fp[0]; dst[1] = fp[1]; dst[2] = fp[2]; dst[3] = 1.0f;
        } else if (a->size == 2) {
            const float *fp = (const float *)p;
            dst[0] = fp[0]; dst[1] = fp[1]; dst[2] = 0.0f; dst[3] = 1.0f;
        } else if (a->size == 4) {
            memcpy(dst, p, 16);
        } else {
            dst[0] = ((const float *)p)[0]; dst[1] = 0.0f; dst[2] = 0.0f; dst[3] = 1.0f;
        }
        return;
    case 0: /* D3DCOLOR (BGRA byte order -> RGBA floats) */
        dst[0] = (float)p[2] * (1.0f / 255.0f);
        dst[1] = (float)p[1] * (1.0f / 255.0f);
        dst[2] = (float)p[0] * (1.0f / 255.0f);
        dst[3] = (float)p[3] * (1.0f / 255.0f);
        return;
    case 4: /* UNORM8 */
        dst[0] = dst[1] = dst[2] = 0.0f; dst[3] = 1.0f;
        for (i = 0; i < a->size && i < 4; i++)
            dst[i] = (float)p[i] * (1.0f / 255.0f);
        return;
    case 1: /* SNORM16 */
        dst[0] = dst[1] = dst[2] = 0.0f; dst[3] = 1.0f;
        for (i = 0; i < a->size && i < 4; i++)
            dst[i] = (float)((const int16_t *)p)[i] * (1.0f / 32767.0f);
        return;
    case 5: /* SINT16 */
        dst[0] = dst[1] = dst[2] = 0.0f; dst[3] = 1.0f;
        for (i = 0; i < a->size && i < 4; i++)
            dst[i] = (float)((const int16_t *)p)[i];
        return;
    case 6: { /* CMP 11:11:10 packed normal */
        uint32_t v = *(const uint32_t *)p;
        dst[0] = (float)((int32_t)(v << 21) >> 21) * (1.0f / 1023.0f);
        dst[1] = (float)((int32_t)(v << 10) >> 21) * (1.0f / 1023.0f);
        dst[2] = (float)((int32_t)v >> 22) * (1.0f / 511.0f);
        dst[3] = 1.0f;
        return;
    }
    default:
        dst[0] = dst[1] = dst[2] = 0.0f; dst[3] = 1.0f;
        return;
    }
}

static int ensure_vsh_vb_ib(uint32_t vbytes, uint32_t icount)
{
    if (vbytes > s_vsh_vb_cap || !s_vsh_vb) {
        uint32_t cap = s_vsh_vb_cap ? s_vsh_vb_cap : (8u * 1024u * 1024u);
        D3D11_BUFFER_DESC bd;
        if (s_vsh_vb) { ID3D11Buffer_Release(s_vsh_vb); s_vsh_vb = NULL; }
        while (cap < vbytes) cap *= 2u;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth      = cap;
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vsh_vb)))
            return 0;
        s_vsh_vb_cap = cap;
    }
    if (icount > s_vsh_ib_cap || !s_vsh_ib) {
        uint32_t cap = s_vsh_ib_cap ? s_vsh_ib_cap : 262144u;
        D3D11_BUFFER_DESC bd;
        if (s_vsh_ib) { ID3D11Buffer_Release(s_vsh_ib); s_vsh_ib = NULL; }
        while (cap < icount) cap *= 2u;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth      = cap * (uint32_t)sizeof(uint32_t);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_INDEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vsh_ib)))
            return 0;
        s_vsh_ib_cap = cap;
    }
    return 1;
}

static void flush_vsh_queue(void)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t k;
    LARGE_INTEGER t0, t1;
    VshEntry *cur_vsh = NULL;
    uint32_t cur_vb_off = 0xFFFFFFFFu;
    uint16_t cur_fcb = 0xFFFFu;
    uint16_t cur_ps  = 0xFFFFu;
    uint16_t cur_cst = 0xFFFFu;
    ID3D11RasterizerState *cur_rs = NULL;
    ID3D11Buffer *vs_cbs[2];

    if (!s_vq_cmds_n || !s_vq_idxs_n) {
        s_vq_cmds_n = s_vq_vbytes_n = s_vq_idxs_n = s_vq_fcbs_n = s_vq_pscbs_n = s_vq_consts_n = 0;
        return;
    }

    QueryPerformanceCounter(&t0);
    if (!ensure_vsh_vb_ib(s_vq_vbytes_n, s_vq_idxs_n)) {
        s_vq_cmds_n = s_vq_vbytes_n = s_vq_idxs_n = s_vq_fcbs_n = s_vq_pscbs_n = s_vq_consts_n = 0;
        return;
    }

    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vsh_vb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, s_vq_vbytes, s_vq_vbytes_n);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vsh_vb, 0);
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vsh_ib, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, s_vq_idxs, (size_t)s_vq_idxs_n * sizeof(uint32_t));
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vsh_ib, 0);
    }

    vs_cbs[0] = s_vsh_frame_cb;
    vs_cbs[1] = s_vsh_const_cb;
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_vsh_ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 2, vs_cbs);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_main_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_ps_cb);

    for (k = 0; k < s_vq_cmds_n; k++) {
        const VshCmd *c = &s_vq_cmds[k];

        if (c->vsh != cur_vsh) {
            ID3D11DeviceContext_IASetInputLayout(s_ctx, c->vsh->il);
            ID3D11DeviceContext_VSSetShader(s_ctx, c->vsh->vs, NULL, 0);
            cur_vsh = c->vsh;
            cur_vb_off = 0xFFFFFFFFu;
        }
        if (c->vb_byte_off != cur_vb_off) {
            UINT stride = c->vsh->stride;
            UINT off    = c->vb_byte_off;
            ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vsh_vb, &stride, &off);
            cur_vb_off = c->vb_byte_off;
        }
        if (c->fcb_idx != cur_fcb) {
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vsh_frame_cb, 0,
                                                  D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, &s_vq_fcbs[c->fcb_idx], sizeof(VshFrameCB));
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vsh_frame_cb, 0);
                cur_fcb = c->fcb_idx;
            }
        }
        if (c->const_idx != cur_cst) {
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vsh_const_cb, 0,
                                                  D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, s_vq_consts + (size_t)c->const_idx * (192u * 4u),
                       192u * 4u * sizeof(float));
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vsh_const_cb, 0);
                cur_cst = c->const_idx;
            }
        }
        if (c->ps_idx != cur_ps) {
            const PsCB *pcb = &s_vq_pscbs[c->ps_idx];
            if (!s_ps_cb_valid || memcmp(pcb, &s_last_ps_cb, sizeof(PsCB)) != 0) {
                if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ps_cb, 0,
                                                      D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    memcpy(m.pData, pcb, sizeof(PsCB));
                    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ps_cb, 0);
                    s_last_ps_cb = *pcb;
                    s_ps_cb_valid = 1;
                }
            }
            cur_ps = c->ps_idx;
        }
        if (c->rs_state != cur_rs) {
            ID3D11DeviceContext_RSSetState(s_ctx, c->rs_state);
            cur_rs = c->rs_state;
        }
        if (c->rt->rtv != s_bound_rtv || c->dsv != s_bound_dsv) {
            if (s_bound_srv && s_bound_srv == c->rt->srv) {
                ID3D11ShaderResourceView *null_srv = NULL;
                ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &null_srv);
                s_bound_srv = NULL;
            }
            if (s_bound_srv1 && s_bound_srv1 == c->rt->srv) {
                ID3D11ShaderResourceView *null_srv = NULL;
                ID3D11DeviceContext_PSSetShaderResources(s_ctx, 1, 1, &null_srv);
                s_bound_srv1 = NULL;
            }
            ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &c->rt->rtv, c->dsv);
            s_bound_rtv = c->rt->rtv;
            s_bound_dsv = c->dsv;
        }
        if (c->rt->w != s_bound_rt_w || c->rt->h != s_bound_rt_h) {
            D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)c->rt->w, (float)c->rt->h, 0.0f, 1.0f };
            ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
            s_bound_rt_w = c->rt->w;
            s_bound_rt_h = c->rt->h;
        }
        if (c->sc[0] != s_bound_sc[0] || c->sc[1] != s_bound_sc[1]
         || c->sc[2] != s_bound_sc[2] || c->sc[3] != s_bound_sc[3]) {
            D3D11_RECT sc = { c->sc[0], c->sc[1], c->sc[2], c->sc[3] };
            ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
            s_bound_sc[0] = c->sc[0]; s_bound_sc[1] = c->sc[1];
            s_bound_sc[2] = c->sc[2]; s_bound_sc[3] = c->sc[3];
        }
        if (c->bs != s_bound_bs || c->bcol != s_bound_bcol) {
            float bfac[4];
            unpack_argb_f4(c->bcol, bfac);
            ID3D11DeviceContext_OMSetBlendState(s_ctx, c->bs, bfac, 0xFFFFFFFFu);
            s_bound_bs   = c->bs;
            s_bound_bcol = c->bcol;
        }
        if (c->dss != s_bound_dss || c->stref != s_bound_stref) {
            ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, c->dss, c->stref);
            s_bound_dss   = c->dss;
            s_bound_stref = c->stref;
        }
        if (c->srv != s_bound_srv) {
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, (ID3D11ShaderResourceView **)&c->srv);
            s_bound_srv = c->srv;
        }
        if (c->srv && c->smp != s_bound_smp) {
            ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, (ID3D11SamplerState **)&c->smp);
            s_bound_smp = c->smp;
        }
        if (c->srv1 != s_bound_srv1) {
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, 1, 1, (ID3D11ShaderResourceView **)&c->srv1);
            s_bound_srv1 = c->srv1;
        }
        if (c->srv1 && c->smp1 != s_bound_smp1) {
            ID3D11DeviceContext_PSSetSamplers(s_ctx, 1, 1, (ID3D11SamplerState **)&c->smp1);
            s_bound_smp1 = c->smp1;
        }

        ID3D11DeviceContext_DrawIndexed(s_ctx, c->idx_count, c->start_idx, 0);
        s_stat_gpu_draws++;
    }

    s_vq_cmds_n = s_vq_vbytes_n = s_vq_idxs_n = s_vq_fcbs_n = s_vq_pscbs_n = s_vq_consts_n = 0;
    s_pipe_valid = 0; /* main 2D path must rebind its VS/IL/VB/VSCB */
    QueryPerformanceCounter(&t1);
    s_stat_draw_ticks  += (uint64_t)(t1.QuadPart - t0.QuadPart);
    s_stat_flush_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
}

static int d3d11_draw_vsh(const Nv2aSurface *s, const Nv2aVshBatch *b)
{
    static uint32_t vc_gen;
    static uint32_t vc_tag[1024];
    static uint32_t vc_idx[1024];
    static uint32_t vc_pos[1024];
    static uint32_t *s_map_buf;
    static uint32_t *s_uidx_buf;
    static uint32_t  s_map_cap;

    VshEntry *vsh;
    RtEntry *rt, *src_rt = NULL, *src_rt1 = NULL;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv = NULL, *srv1 = NULL;
    ID3D11SamplerState *smp = NULL, *smp1 = NULL;
    uint32_t tex_w = 1, tex_h = 1, tex1_w = 1, tex1_h = 1;
    const Nv2aRenderState *rs = b->state;
    const Nv2aCombiner *rc = (const Nv2aCombiner *)b->combiner;
    int tex_stage_on, tex1_stage_on;
    uint32_t n = b->index_count, u = 0, i, gen;
    uint32_t est_tris = 0, tri_idxs = 0;
    uint32_t attr_list[16], attr_n = 0;
    uint32_t vb_byte_off, start_idx;
    LARGE_INTEGER t0, t1;

    if (!b || !b->program || !b->insn_count || n < 3) return 0;
    if (b->prim_type < 5u || b->prim_type > 10u) return 0;
    if (!d3d11_ensure_init()) return 0;

    if (s_q_cmds_n > 0) flush_draw_queue();

    QueryPerformanceCounter(&t0);

    vsh = get_or_compile_vsh(b);
    if (!vsh) return 0;

    if (!s_vq_consts) {
        s_vq_consts = (float *)malloc((size_t)MAX_VQ_CONSTS * 192u * 4u * sizeof(float));
        if (!s_vq_consts) return 0;
    }

    s_stat_draws++;
    s_stat_vsh_draws++;
    s_frame_draws++;
    if (rc && (rc->stage_program & ~0x1Fu) != 0u)
        s_stat_multitex++;

    rt = get_rt(s);
    if (!rt || !rt->rtv) return 1;
    dsv = get_dsv(rs->zeta_va, rt->w, rt->h);

    tex_stage_on  = (!rc || (rc->stage_program & 0x1Fu) != 0u);
    tex1_stage_on = (rc && ((rc->stage_program >> 5) & 0x1Fu) != 0u);
    if (b->texture && tex_stage_on) {
        srv = get_texture_srv(b->texture, &tex_w, &tex_h, &src_rt);
        if (srv) smp = get_sampler_state(b->texture);
    }
    if (b->texture1 && tex1_stage_on) {
        srv1 = get_texture_srv(b->texture1, &tex1_w, &tex1_h, &src_rt1);
        if (srv1) smp1 = get_sampler_state(b->texture1);
    }
    if (srv && srv == rt->srv)   { srv = NULL;  src_rt = NULL;  }
    if (srv1 && srv1 == rt->srv) { srv1 = NULL; src_rt1 = NULL; }

    /* Estimate expanded triangle index count */
    switch (b->prim_type) {
    case 5:  est_tris = n / 3u; break;                 /* TRIANGLES */
    case 6:  est_tris = (n >= 2u) ? (n - 2u) : 0u; break; /* TRISTRIP */
    case 7:
    case 10: est_tris = (n >= 2u) ? (n - 2u) : 0u; break; /* TRIFAN / POLYGON */
    case 8:  est_tris = (n / 4u) * 2u; break;          /* QUADS */
    case 9:  est_tris = (n >= 2u) ? ((n - 2u) / 2u) * 2u : 0u; break; /* QUADSTRIP */
    default: return 0;
    }
    if (!est_tris) return 1;

    /* Flush if RT hazard or queue limits reached */
    if (src_rt || src_rt1 || s_vq_cmds_n >= MAX_VQ_CMDS
     || s_vq_fcbs_n >= MAX_Q_VSCB - 1u || s_vq_pscbs_n >= MAX_Q_PSCB - 1u
     || s_vq_consts_n >= MAX_VQ_CONSTS - 1u
     || s_vq_vbytes_n + n * vsh->stride > 16u * 1024u * 1024u
     || s_vq_idxs_n + est_tris * 3u > 524288u) {
        QueryPerformanceCounter(&t1);
        s_stat_draw_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
        flush_vsh_queue();
        QueryPerformanceCounter(&t0);
    }

    /* Deduplicate vertices */
    if (n > s_map_cap) {
        uint32_t cap = s_map_cap ? s_map_cap : 65536u;
        uint32_t *nm, *nu;
        while (cap < n) cap *= 2u;
        nm = (uint32_t *)realloc(s_map_buf, (size_t)cap * sizeof(uint32_t));
        if (!nm) return 0;
        s_map_buf = nm;
        nu = (uint32_t *)realloc(s_uidx_buf, (size_t)cap * sizeof(uint32_t));
        if (!nu) return 0;
        s_uidx_buf = nu;
        s_map_cap = cap;
    }

    gen = ++vc_gen;
    if (gen == 0) {
        memset(vc_tag, 0, sizeof(vc_tag));
        gen = ++vc_gen;
    }
    for (i = 0; i < n; i++) {
        uint32_t idx = b->indices[i];
        uint32_t slot = idx & 1023u;
        if (vc_tag[slot] == gen && vc_idx[slot] == idx) {
            s_map_buf[i] = vc_pos[slot];
        } else {
            vc_tag[slot] = gen;
            vc_idx[slot] = idx;
            vc_pos[slot] = u;
            s_uidx_buf[u] = idx;
            s_map_buf[i]  = u;
            u++;
        }
    }
    s_stat_vsh_verts += u;

    /* Ensure CPU staging buffers */
    {
        uint32_t need_vb = s_vq_vbytes_n + u * vsh->stride;
        if (need_vb > s_vq_vbytes_cap) {
            uint32_t cap = s_vq_vbytes_cap ? s_vq_vbytes_cap : (4u * 1024u * 1024u);
            uint8_t *nb;
            while (cap < need_vb) cap *= 2u;
            nb = (uint8_t *)realloc(s_vq_vbytes, cap);
            if (!nb) return 0;
            s_vq_vbytes = nb;
            s_vq_vbytes_cap = cap;
        }
    }
    {
        uint32_t need_ib = s_vq_idxs_n + est_tris * 3u;
        if (need_ib > s_vq_idxs_cap) {
            uint32_t cap = s_vq_idxs_cap ? s_vq_idxs_cap : 131072u;
            uint32_t *ni;
            while (cap < need_ib) cap *= 2u;
            ni = (uint32_t *)realloc(s_vq_idxs, (size_t)cap * sizeof(uint32_t));
            if (!ni) return 0;
            s_vq_idxs = ni;
            s_vq_idxs_cap = cap;
        }
    }

    /* Pack only active vertex attributes (from in_mask) */
    if (b->in_mask == 0u) {
        attr_list[0] = 0;
        attr_n = 1;
    } else {
        uint32_t a;
        for (a = 0; a < 16u; a++) {
            if (b->in_mask & (1u << a))
                attr_list[attr_n++] = a;
        }
    }

    vb_byte_off = s_vq_vbytes_n;
    {
        float *dst = (float *)(s_vq_vbytes + vb_byte_off);
        uint32_t a;
        for (i = 0; i < u; i++) {
            uint32_t vidx = s_uidx_buf[i];
            for (a = 0; a < attr_n; a++) {
                unpack_vsh_attr(&b->attrs[attr_list[a]], vidx, dst);
                dst += 4;
            }
        }
    }
    s_vq_vbytes_n += u * vsh->stride;

    /* Expand primitive indices into triangle list */
    start_idx = s_vq_idxs_n;
    {
        uint32_t *out = s_vq_idxs + start_idx;
        switch (b->prim_type) {
        case 5: /* TRIANGLES */
            for (i = 0; i + 2 < n; i += 3) {
                out[tri_idxs++] = s_map_buf[i];
                out[tri_idxs++] = s_map_buf[i + 1];
                out[tri_idxs++] = s_map_buf[i + 2];
            }
            break;
        case 6: /* TRISTRIP */
            for (i = 0; i + 2 < n; i++) {
                uint32_t i0 = s_map_buf[i], i1 = s_map_buf[i + 1], i2 = s_map_buf[i + 2];
                if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                if ((i & 1u) && b->strip_alt_winding) {
                    out[tri_idxs++] = i1; out[tri_idxs++] = i0; out[tri_idxs++] = i2;
                } else {
                    out[tri_idxs++] = i0; out[tri_idxs++] = i1; out[tri_idxs++] = i2;
                }
            }
            break;
        case 7:
        case 10: /* TRIFAN / POLYGON */
            for (i = 1; i + 1 < n; i++) {
                out[tri_idxs++] = s_map_buf[0];
                out[tri_idxs++] = s_map_buf[i];
                out[tri_idxs++] = s_map_buf[i + 1];
            }
            break;
        case 8: /* QUADS */
            for (i = 0; i + 3 < n; i += 4) {
                uint32_t i0 = s_map_buf[i], i1 = s_map_buf[i+1], i2 = s_map_buf[i+2], i3 = s_map_buf[i+3];
                out[tri_idxs++] = i0; out[tri_idxs++] = i1; out[tri_idxs++] = i2;
                out[tri_idxs++] = i0; out[tri_idxs++] = i2; out[tri_idxs++] = i3;
            }
            break;
        case 9: /* QUADSTRIP */
            for (i = 0; i + 3 < n; i += 2) {
                uint32_t i0 = s_map_buf[i], i1 = s_map_buf[i+1], i2 = s_map_buf[i+2], i3 = s_map_buf[i+3];
                out[tri_idxs++] = i0; out[tri_idxs++] = i1; out[tri_idxs++] = i3;
                out[tri_idxs++] = i0; out[tri_idxs++] = i3; out[tri_idxs++] = i2;
            }
            break;
        }
    }
    if (!tri_idxs) {
        s_vq_vbytes_n = vb_byte_off;
        return 1;
    }
    s_vq_idxs_n += tri_idxs;

    /* Build CBs & queue command */
    {
        VshFrameCB fcb;
        PsCB ps_cb;
        uint16_t fcb_idx, ps_idx, cst_idx;
        ID3D11BlendState *bs = get_blend_state(rs);
        ID3D11DepthStencilState *dss = get_dss_state(rs, dsv != NULL);
        ID3D11RasterizerState *rs_st = s_rs_vsh_none;
        uint32_t stref = rs->stencil_ref & 0xFFu;
        LONG sc_l = (LONG)s->clip_x, sc_t = (LONG)s->clip_y;
        LONG sc_r = (LONG)(s->clip_x + s->width), sc_b = (LONG)(s->clip_y + s->height);

        if (s_cull_mode && rs->cull_enable) {
            int front_cw = (rs->front_face == GL_CW);
            uint32_t cf = rs->cull_face ? rs->cull_face : GL_BACK;
            if (cf == GL_BACK)  rs_st = front_cw ? s_rs_vsh_cw : s_rs_vsh_ccw;
            if (cf == GL_FRONT) rs_st = front_cw ? s_rs_vsh_ccw : s_rs_vsh_cw;
        }

        memset(&fcb, 0, sizeof(fcb));
        fcb.inv_rt_size[0] = 1.0f / (float)rt->w;
        fcb.inv_rt_size[1] = 1.0f / (float)rt->h;
        if (srv && !b->tex0_normalized && tex_w && tex_h) {
            fcb.uv0_scale[0] = 1.0f / (float)tex_w;
            fcb.uv0_scale[1] = 1.0f / (float)tex_h;
        } else {
            fcb.uv0_scale[0] = 1.0f;
            fcb.uv0_scale[1] = 1.0f;
        }
        if (srv1 && !b->tex1_normalized && tex1_w && tex1_h) {
            fcb.uv1_scale[0] = 1.0f / (float)tex1_w;
            fcb.uv1_scale[1] = 1.0f / (float)tex1_h;
        } else {
            fcb.uv1_scale[0] = 1.0f;
            fcb.uv1_scale[1] = 1.0f;
        }
        if (rs->depth_max > 65536.0f || rs->depth_max <= 0.0f)
            fcb.z_scale = 1.0f / 16777215.0f;
        else if (rs->depth_max > 1.5f)
            fcb.z_scale = 1.0f / 65535.0f;
        else
            fcb.z_scale = 1.0f;
        fcb.aa_sx = (float)(s->aa_sx ? s->aa_sx : 1u);
        fcb.aa_sy = (float)(s->aa_sy ? s->aa_sy : 1u);

        if (s_vq_fcbs_n > 0 && memcmp(&fcb, &s_vq_fcbs[s_vq_fcbs_n - 1], sizeof(fcb)) == 0) {
            fcb_idx = (uint16_t)(s_vq_fcbs_n - 1);
        } else {
            fcb_idx = (uint16_t)s_vq_fcbs_n;
            s_vq_fcbs[s_vq_fcbs_n++] = fcb;
        }

        if (s_vq_consts_n > 0
         && memcmp(b->constants, s_vq_consts + (size_t)(s_vq_consts_n - 1) * (192u * 4u),
                   192u * 4u * sizeof(float)) == 0) {
            cst_idx = (uint16_t)(s_vq_consts_n - 1);
        } else {
            cst_idx = (uint16_t)s_vq_consts_n;
            memcpy(s_vq_consts + (size_t)cst_idx * (192u * 4u),
                   b->constants, 192u * 4u * sizeof(float));
            s_vq_consts_n++;
        }

        memset(&ps_cb, 0, sizeof(ps_cb));
        ps_cb.flags[0] = srv ? (tex_is_opaque_fmt(b->texture->color) ? 2u : 1u) : 0u;
        ps_cb.flags[1] = rc ? 1u : 0u;
        ps_cb.flags[2] = rs->alpha_test_enable ? 1u : 0u;
        ps_cb.flags[3] = rs->alpha_func ? rs->alpha_func : GL_ALWAYS;
        ps_cb.alpha_misc[0] = (float)(rs->alpha_ref & 0xFFu) / 255.0f;
        ps_cb.alpha_misc[1] = srv1 ? (tex_is_opaque_fmt(b->texture1->color) ? 2.0f : 1.0f) : 0.0f;
        unpack_argb_f4(b->fog_color, ps_cb.fog_col);
        if (rc) {
            int st, nst = (int)(rc->control & 0xFFu);
            if (nst > 8) nst = 8;
            for (st = 0; st < nst; st++) {
                ps_cb.stages[st][0] = rc->color_icw[st];
                ps_cb.stages[st][1] = rc->color_ocw[st];
                ps_cb.stages[st][2] = rc->alpha_icw[st];
                ps_cb.stages[st][3] = rc->alpha_ocw[st];
                unpack_argb_f4(rc->factor0[st], ps_cb.factor0[st]);
                unpack_argb_f4(rc->factor1[st], ps_cb.factor1[st]);
            }
            unpack_argb_f4(rc->final_c0, ps_cb.final_c0);
            unpack_argb_f4(rc->final_c1, ps_cb.final_c1);
            ps_cb.rc_meta[0] = rc->control;
            ps_cb.rc_meta[1] = rc->final0;
            ps_cb.rc_meta[2] = rc->final1;
            ps_cb.rc_meta[3] = rc->stage_program;
        }

        if (s_vq_pscbs_n > 0 && memcmp(&ps_cb, &s_vq_pscbs[s_vq_pscbs_n - 1], sizeof(ps_cb)) == 0) {
            ps_idx = (uint16_t)(s_vq_pscbs_n - 1);
        } else {
            ps_idx = (uint16_t)s_vq_pscbs_n;
            s_vq_pscbs[s_vq_pscbs_n++] = ps_cb;
        }

        /* Merge consecutive identical-state VSH draws into one DrawIndexed! */
        if (s_vq_cmds_n > 0) {
            VshCmd *prev = &s_vq_cmds[s_vq_cmds_n - 1];
            if (prev->vsh == vsh && prev->rt == rt && prev->dsv == dsv
             && prev->bs == bs && prev->bcol == rs->blend_color
             && prev->dss == dss && prev->stref == stref && prev->rs_state == rs_st
             && prev->srv == srv && prev->smp == smp
             && prev->srv1 == srv1 && prev->smp1 == smp1
             && prev->sc[0] == sc_l && prev->sc[1] == sc_t
             && prev->sc[2] == sc_r && prev->sc[3] == sc_b
             && prev->fcb_idx == fcb_idx && prev->ps_idx == ps_idx
             && prev->const_idx == cst_idx
             && prev->start_idx + prev->idx_count == start_idx) {
                uint32_t base_v = (vb_byte_off - prev->vb_byte_off) / vsh->stride;
                uint32_t *idxp = s_vq_idxs + start_idx;
                for (i = 0; i < tri_idxs; i++)
                    idxp[i] += base_v;
                prev->idx_count += tri_idxs;
                goto vsh_queued_done;
            }
        }
        {
            VshCmd *c = &s_vq_cmds[s_vq_cmds_n++];
            c->vsh = vsh; c->rt = rt; c->dsv = dsv;
            c->bs = bs; c->bcol = rs->blend_color;
            c->dss = dss; c->stref = stref; c->rs_state = rs_st;
            c->srv = srv; c->smp = smp; c->srv1 = srv1; c->smp1 = smp1;
            c->sc[0] = sc_l; c->sc[1] = sc_t; c->sc[2] = sc_r; c->sc[3] = sc_b;
            c->fcb_idx = fcb_idx; c->ps_idx = ps_idx; c->const_idx = cst_idx;
            c->vb_byte_off = vb_byte_off;
            c->start_idx = start_idx;
            c->idx_count = tri_idxs;
        }
    }
vsh_queued_done:
    rt->has_drawn    = 1;
    if (rt->w > 256u || rt->h > 256u) {
        s_present_rt     = rt;
        s_resolve_src_rt = (src_rt && src_rt->w > rt->w) ? src_rt : NULL;
    }
    s_last_rt        = rt;

    QueryPerformanceCounter(&t1);
    s_stat_draw_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
    return 1;
}

static const Nv2aBackend k_d3d11_backend = {
    d3d11_clear,
    d3d11_draw,
    d3d11_flip,
    d3d11_draw_vsh,
    NULL,
};

void sims_d3d11_backend_register(void)
{
    const char *sw = getenv("RECOMP_SOFTWARE_GPU");
    const char *dump = getenv("RECOMP_FB_DUMP");

    if (sw && atoi(sw) != 0) {
        fprintf(stderr, "[BOOT] Software CPU rasterizer selected (RECOMP_SOFTWARE_GPU=1)\n");
        return;
    }

    s_want_dump_sync = (dump && *dump) ? 1 : 0;
    if (s_want_dump_sync) {
        nv2a_backend_register_with_present_sync(&k_d3d11_backend, d3d11_present_sync);
    } else {
        nv2a_backend_register(&k_d3d11_backend);
    }
    fprintf(stderr, "[BOOT] Registered native D3D11 GPU backend%s\n",
            s_want_dump_sync ? " (with dump readback sync)" : "");
}
