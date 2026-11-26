/*
 * NV2A 128-bit Vertex Shader Microcode -> HLSL (vs_4_0) Compiler
 *
 * Uses the exact same bit-field layout and MAC/ILU execution semantics as
 * xboxrecomp's nv2a_vsh_interp.c, emitting straight-line HLSL that plugs
 * directly into the D3D11 register-combiner pixel shader (PSIn).
 */
#ifndef SIMS_NV2A_VSH_HLSL_H
#define SIMS_NV2A_VSH_HLSL_H

#include <stdint.h>
#include <stddef.h>

/* Translate `insn_count` NV2A 128-bit microcode instructions into HLSL `vs_4_0`
 * source text. `in_mask` is the bitmask of v0..v15 inputs read by the program.
 * Returns 1 on success (writing NUL-terminated HLSL into out_buf), 0 on error. */
int nv2a_vsh_emit_hlsl(const uint32_t (*program)[4], uint32_t insn_count,
                       uint32_t in_mask, char *out_buf, size_t out_cap);

#endif /* SIMS_NV2A_VSH_HLSL_H */
