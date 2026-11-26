/*
 * NV2A 128-bit Vertex Shader Microcode -> HLSL (vs_4_0) Compiler
 *
 * Matches xboxrecomp's nv2a_vsh_interp.c instruction decoding and MAC/ILU
 * semantics identically, while running all vertex transformation, skinning,
 * lighting, and near-plane clipping on the D3D11 GPU.
 */
#include "nv2a_vsh_hlsl.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef struct {
    char  *buf;
    size_t pos;
    size_t cap;
    int    overflow;
} StrBuf;

static void sb_append(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (sb->overflow) return;
    va_start(ap, fmt);
    n = vsnprintf(sb->buf + sb->pos, sb->cap - sb->pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sb->cap - sb->pos) {
        sb->overflow = 1;
        return;
    }
    sb->pos += (size_t)n;
}

static uint32_t vsh_field(const uint32_t *ins, int word, int shift, int bits)
{
    return (ins[word] >> shift) & ((1u << bits) - 1u);
}

static uint8_t vsh_reads(uint32_t mac, uint32_t ilu)
{
    uint8_t r = 0;
    if (mac) r |= 1u;
    if (mac == 2u || (mac >= 4u && mac <= 12u)) r |= 2u;
    if (mac == 3u || mac == 4u || ilu) r |= 4u;
    return r;
}

static void fmt_wmask(uint32_t mask, char out[8])
{
    int p = 0;
    if (mask & 8u) out[p++] = 'x';
    if (mask & 4u) out[p++] = 'y';
    if (mask & 2u) out[p++] = 'z';
    if (mask & 1u) out[p++] = 'w';
    out[p] = '\0';
}

static void fmt_src(const uint32_t *ins, int which, uint32_t in_mask, char out[64])
{
    static const char sw[4] = { 'x', 'y', 'z', 'w' };
    uint32_t neg, swz, mux, reg;
    char base[48];

    if (which == 0) {
        neg = vsh_field(ins, 1, 8, 1);  swz = vsh_field(ins, 1, 0, 8);
        mux = vsh_field(ins, 2, 26, 2); reg = vsh_field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = vsh_field(ins, 2, 25, 1); swz = vsh_field(ins, 2, 17, 8);
        mux = vsh_field(ins, 2, 11, 2); reg = vsh_field(ins, 2, 13, 4);
    } else {
        neg = vsh_field(ins, 2, 10, 1); swz = vsh_field(ins, 2, 2, 8);
        mux = vsh_field(ins, 3, 28, 2);
        reg = (vsh_field(ins, 2, 0, 2) << 2) | vsh_field(ins, 3, 30, 2);
    }

    if (mux == 1u) {
        uint32_t ridx = (reg <= 12u) ? reg : 0u;
        snprintf(base, sizeof(base), "R%u", ridx);
    } else if (mux == 2u) {
        uint32_t vidx = vsh_field(ins, 1, 9, 4);
        if (in_mask & (1u << vidx))
            snprintf(base, sizeof(base), "i.v%u", vidx);
        else
            snprintf(base, sizeof(base), "float4(0,0,0,1)");
    } else {
        uint32_t ci = vsh_field(ins, 1, 13, 8);
        uint32_t rel = vsh_field(ins, 3, 1, 1);
        if (rel) {
            snprintf(base, sizeof(base), "c[clamp(%d + a0, 0, 191)]", (int)ci);
        } else {
            if (ci >= 192u) ci = 0u;
            snprintf(base, sizeof(base), "c[%u]", ci);
        }
    }

    snprintf(out, 64, "%s%s.%c%c%c%c",
             neg ? "-" : "",
             base,
             sw[(swz >> 6) & 3u],
             sw[(swz >> 4) & 3u],
             sw[(swz >> 2) & 3u],
             sw[swz & 3u]);
}

static void emit_write(StrBuf *sb, const char *dst, uint32_t mask, const char *val)
{
    char m[8];
    if (!mask) return;
    if (mask == 15u) {
        sb_append(sb, "    %s = %s;\n", dst, val);
    } else {
        fmt_wmask(mask, m);
        sb_append(sb, "    %s.%s = (%s).%s;\n", dst, m, val, m);
    }
}

int nv2a_vsh_emit_hlsl(const uint32_t (*program)[4], uint32_t insn_count,
                       uint32_t in_mask, char *out_buf, size_t out_cap)
{
    StrBuf sb;
    uint32_t i;

    if (!program || insn_count == 0 || !out_buf || out_cap < 1024)
        return 0;

    sb.buf = out_buf;
    sb.pos = 0;
    sb.cap = out_cap;
    sb.overflow = 0;

    sb_append(&sb,
        "cbuffer VshFrameCB : register(b0) {\n"
        "    float2 inv_rt_size;\n"
        "    float2 uv0_scale;\n"
        "    float2 uv1_scale;\n"
        "    float  z_scale;\n"
        "    float  aa_sx;\n"
        "    float  aa_sy;\n"
        "    float3 _pad0;\n"
        "};\n"
        "cbuffer VshConsts : register(b1) {\n"
        "    float4 c[192];\n"
        "};\n"
        "struct VSIn {\n");

    if (in_mask == 0u) {
        sb_append(&sb, "    float4 v0 : TEXCOORD0;\n");
    } else {
        for (i = 0; i < 16u; i++) {
            if (in_mask & (1u << i))
                sb_append(&sb, "    float4 v%u : TEXCOORD%u;\n", i, i);
        }
    }

    sb_append(&sb,
        "};\n"
        "struct VSOut {\n"
        "    float4 pos  : SV_Position;\n"
        "    float4 col  : COLOR0;\n"
        "    float2 uv   : TEXCOORD0;\n"
        "    float4 spec : COLOR1;\n"
        "    float2 uv1  : TEXCOORD1;\n"
        "};\n"
        "float4 vsh_rcc(float x) {\n"
        "    float f = (x == 0.0f) ? 1.884467e19f : (1.0f / x);\n"
        "    float af = abs(f);\n"
        "    float s = (f < 0.0f) ? -1.0f : 1.0f;\n"
        "    af = clamp(af, 5.42101e-20f, 1.884467e19f);\n"
        "    return (s * af).xxxx;\n"
        "}\n"
        "float4 vsh_exp(float x) {\n"
        "    float fl = floor(x);\n"
        "    return float4(exp2(fl), x - fl, exp2(x), 1.0f);\n"
        "}\n"
        "float4 vsh_log(float x) {\n"
        "    float ax = max(abs(x), 1e-20f);\n"
        "    float e = floor(log2(ax));\n"
        "    return float4(e, ax / exp2(e), log2(ax), 1.0f);\n"
        "}\n"
        "float4 vsh_lit(float4 v) {\n"
        "    float nl = max(v.x, 0.0f);\n"
        "    float nh = max(v.y, 0.0f);\n"
        "    float p  = clamp(v.w, -127.9961f, 127.9961f);\n"
        "    float sp = (v.x > 0.0f) ? ((nh > 0.0f) ? pow(nh, p) : ((p == 0.0f) ? 1.0f : 0.0f)) : 0.0f;\n"
        "    return float4(1.0f, nl, sp, 1.0f);\n"
        "}\n"
        "VSOut main(VSIn i) {\n"
        "    float4 R0=0, R1=0, R2=0, R3=0, R4=0, R5=0, R6=0, R7=0, R8=0, R9=0, R10=0, R11=0, R12=0;\n"
        "    float4 oD0 = float4(0, 0, 0, 1);\n"
        "    float4 oD1 = float4(0, 0, 0, 1);\n"
        "    float4 oT0 = float4(0, 0, 0, 1);\n"
        "    float4 oT1 = float4(0, 0, 0, 1);\n"
        "    int a0 = 0;\n"
        "    float4 mres = 0, ires = 0;\n");

    for (i = 0; i < insn_count; i++) {
        const uint32_t *ins = program[i];
        uint32_t mac      = vsh_field(ins, 1, 21, 4);
        uint32_t ilu      = vsh_field(ins, 1, 25, 3);
        uint32_t mac_mask = vsh_field(ins, 3, 24, 4);
        uint32_t ilu_mask = vsh_field(ins, 3, 16, 4);
        uint32_t tdst     = vsh_field(ins, 3, 20, 4);
        uint32_t it       = mac ? 1u : tdst;
        uint32_t omask    = vsh_field(ins, 3, 12, 4);
        uint32_t oaddr    = vsh_field(ins, 3, 3, 8);
        uint32_t to_o     = vsh_field(ins, 3, 11, 1);
        uint32_t from_ilu = vsh_field(ins, 3, 2, 1);
        uint32_t is_final = vsh_field(ins, 3, 0, 1);
        uint8_t  reads    = vsh_reads(mac, ilu);
        char sa[64] = "0", sb_s[64] = "0", sc[64] = "0";
        char dst_name[16];

        if (!mac || mac == 13u) mac_mask = 0;
        if (!ilu)               ilu_mask = 0;

        if (reads & 1u) fmt_src(ins, 0, in_mask, sa);
        if (reads & 2u) fmt_src(ins, 1, in_mask, sb_s);
        if (reads & 4u) fmt_src(ins, 2, in_mask, sc);

        /* Evaluate MAC & ILU from pre-instruction registers before any writes */
        if (mac) {
            switch (mac) {
            case 1u:  sb_append(&sb, "    mres = %s;\n", sa); break;
            case 2u:  sb_append(&sb, "    mres = (%s) * (%s);\n", sa, sb_s); break;
            case 3u:  sb_append(&sb, "    mres = (%s) + (%s);\n", sa, sc); break;
            case 4u:  sb_append(&sb, "    mres = (%s) * (%s) + (%s);\n", sa, sb_s, sc); break;
            case 5u:  sb_append(&sb, "    mres = dot((%s).xyz, (%s).xyz).xxxx;\n", sa, sb_s); break;
            case 6u:  sb_append(&sb, "    mres = (dot((%s).xyz, (%s).xyz) + (%s).w).xxxx;\n", sa, sb_s, sb_s); break;
            case 7u:  sb_append(&sb, "    mres = dot((%s), (%s)).xxxx;\n", sa, sb_s); break;
            case 8u:  sb_append(&sb, "    mres = float4(1.0f, (%s).y * (%s).y, (%s).z, (%s).w);\n", sa, sb_s, sa, sb_s); break;
            case 9u:  sb_append(&sb, "    mres = min((%s), (%s));\n", sa, sb_s); break;
            case 10u: sb_append(&sb, "    mres = max((%s), (%s));\n", sa, sb_s); break;
            case 11u: sb_append(&sb, "    mres = float4((%s) < (%s));\n", sa, sb_s); break;
            case 12u: sb_append(&sb, "    mres = float4((%s) >= (%s));\n", sa, sb_s); break;
            case 13u: sb_append(&sb, "    mres = %s;\n", sa); break;
            default:  break;
            }
        }
        if (ilu) {
            switch (ilu) {
            case 1u: sb_append(&sb, "    ires = %s;\n", sc); break;
            case 2u: sb_append(&sb, "    { float _x = (%s).x; ires = (_x == 0.0f ? 1e20f : (1.0f / _x)).xxxx; }\n", sc); break;
            case 3u: sb_append(&sb, "    ires = vsh_rcc((%s).x);\n", sc); break;
            case 4u: sb_append(&sb, "    ires = rsqrt(max(abs((%s).x), 1e-20f)).xxxx;\n", sc); break;
            case 5u: sb_append(&sb, "    ires = vsh_exp((%s).x);\n", sc); break;
            case 6u: sb_append(&sb, "    ires = vsh_log((%s).x);\n", sc); break;
            case 7u: sb_append(&sb, "    ires = vsh_lit(%s);\n", sc); break;
            default: break;
            }
        }

        /* Apply register writes */
        if (mac == 13u)
            sb_append(&sb, "    a0 = (int)floor(mres.x + 0.001f);\n");
        if (mac_mask && tdst <= 12u) {
            snprintf(dst_name, sizeof(dst_name), "R%u", tdst);
            emit_write(&sb, dst_name, mac_mask, "mres");
        }
        if (ilu_mask && it <= 12u) {
            snprintf(dst_name, sizeof(dst_name), "R%u", it);
            emit_write(&sb, dst_name, ilu_mask, "ires");
        }
        if (omask && to_o) {
            const char *val = from_ilu ? "ires" : "mres";
            if      (oaddr == 0u)  emit_write(&sb, "R12", omask, val);
            else if (oaddr == 3u)  emit_write(&sb, "oD0", omask, val);
            else if (oaddr == 4u)  emit_write(&sb, "oD1", omask, val);
            else if (oaddr == 9u)  emit_write(&sb, "oT0", omask, val);
            else if (oaddr == 10u) emit_write(&sb, "oT1", omask, val);
        }

        if (is_final)
            break;
    }

    /* Convert NV2A post-viewport oPos (R12) back to D3D11 clip space for
     * exact hardware clipping & perspective-correct interpolation. */
    sb_append(&sb,
        "    VSOut o;\n"
        "    float w = R12.w;\n"
        "    if (w == 0.0f || isinf(w) || isnan(w)) w = 1.0f;\n"
        "    float sx = R12.x * aa_sx;\n"
        "    float sy = R12.y * aa_sy;\n"
        "    float ndc_x = sx * inv_rt_size.x * 2.0f - 1.0f;\n"
        "    float ndc_y = 1.0f - sy * inv_rt_size.y * 2.0f;\n"
        "    float ndc_z = R12.z * z_scale;\n"
        "    float cz = (w > 0.0f) ? min(ndc_z * w, w) : (ndc_z * w);\n"
        "    o.pos  = float4(ndc_x * w, ndc_y * w, cz, w);\n"
        "    o.col  = saturate(oD0);\n"
        "    o.spec = saturate(oD1);\n"
        "    o.uv   = oT0.xy * uv0_scale;\n"
        "    o.uv1  = oT1.xy * uv1_scale;\n"
        "    return o;\n"
        "}\n");

    return sb.overflow ? 0 : 1;
}
