/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <windows.h>
#include <xbox/xboxrecomp.h>

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern ptrdiff_t g_xbox_mem_offset;

extern recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
extern void sub_002224B5(void);
extern void sub_0024D240(void);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);

/* sub_0024D300: D3D_BlockOnTime(uint32_t fence, uint32_t flags) (stdcall, ret 8).
 * Replaces lifted spin loop (where non-volatile MEM32 is hoisted by /O2).
 * Hands the real GPU semaphore VA to nv2a_pb_exec and waits up to 250ms. */
void sub_0024D300(void)
{
    uint8_t *base = (uint8_t *)g_xbox_mem_offset;
    uint32_t fence = *(uint32_t *)(base + g_esp + 4);
    uint32_t dev   = *(uint32_t *)(base + 0x00264948u);
    if (dev) {
        uint32_t put    = *(volatile uint32_t *)(base + dev + 0x2Cu);
        uint32_t sem_va = *(volatile uint32_t *)(base + dev + 0x30u);
        if (sem_va) {
            static uint32_t s_last_sem_va = 0;
            if (sem_va != s_last_sem_va) {
                s_last_sem_va = sem_va;
                nv2a_pb_set_semaphore_target(sem_va);
                fprintf(stderr, "[D3D] GPU semaphore at 0x%08X (dev=0x%08X)\n", sem_va, dev);
                fflush(stderr);
            }
            if (fence == put) {
                uint32_t saved_esi = g_esi, saved_edi = g_edi, saved_ebx = g_ebx;
                g_esp -= 4; *(uint32_t *)(base + g_esp) = 0;
                g_esp -= 4; *(uint32_t *)(base + g_esp) = 0x0024D32Fu;
                sub_0024D240();
                g_esi = saved_esi; g_edi = saved_edi; g_ebx = saved_ebx;
            }
            {
                DWORD t0 = GetTickCount();
                while ((int32_t)(*(volatile uint32_t *)(base + sem_va) - fence) < 0) {
                    if (GetTickCount() - t0 >= 250u)
                        break;
                    Sleep(1);
                }
            }
        }
    }
    g_esp += 12; /* ret 8 */
}

/* 0x00223498: CRT x87 math helper (fstp st(0); fstp st(0); fldz; ret) */
static void manual_00223498(void)
{
    g_fp_top = (g_fp_top + 2) & 7;
    g_fp_top = (g_fp_top - 1) & 7;
    g_fp_stack[g_fp_top] = 0.0;
    g_esp += 4;
}

/* 0x0016E9B0: D3DDevice_InsertCallback frame-complete callback.
 * cdecl(void *ctx): mov ecx, [esp+4]; mov eax, [ecx]; jmp [eax+0xC] (Semaphore::Release) */
static void manual_0016E9B0(void)
{
    uint8_t *base = (uint8_t *)g_xbox_mem_offset;
    uint32_t ctx = *(uint32_t *)(base + g_esp + 4);
    uint32_t vtbl = *(uint32_t *)(base + ctx);
    uint32_t target = *(uint32_t *)(base + vtbl + 0x0Cu);
    recomp_func_t fn = recomp_lookup_manual(target);
    if (!fn) fn = recomp_lookup(target);
    g_ecx = ctx;
    g_eax = vtbl;
    if (fn) {
        fn();
    } else {
        g_esp += 4;
    }
}

/* 0x002283CD: XapiThreadNotify callback (swallowed inside sub_002283A3 / _endthreadex).
 * stdcall(BOOL fCreate): dispatches [0x30D5C4] on create, or [0x30D5C8] + sub_002224B5(0) on exit. */
static void manual_002283CD(void)
{
    uint8_t *base = (uint8_t *)g_xbox_mem_offset;
    uint32_t f_create = *(uint32_t *)(base + g_esp + 4);
    uint32_t cb = *(uint32_t *)(base + (f_create ? 0x0030D5C4u : 0x0030D5C8u));
    g_eax = cb;
    if (cb) {
        recomp_func_t fn = recomp_lookup(cb);
        if (fn) {
            g_esp -= 4;
            *(uint32_t *)(base + g_esp) = 0x002283DFu;
            fn();
        }
    }
    if (!f_create) {
        g_esp -= 4;
        *(uint32_t *)(base + g_esp) = 0;
        g_esp -= 4;
        *(uint32_t *)(base + g_esp) = 0x002283F3u;
        sub_002224B5();
        g_esp += 4; /* pop ecx */
    }
    g_esp += 8; /* ret 4 (pop return address + 1 arg) */
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
/* 0x002D9E9C: XPP MU_Init (Memory Unit USB class driver init, stdcall ret 4).
 * Linear sweep desynced over preceding data table. Zeroes MU globals and
 * initializes the empty list head at 0x3E0894; no Memory Units are attached
 * (saves go to HDD U:\). */
static void manual_002D9E9C(void)
{
    uint8_t *base = (uint8_t *)g_xbox_mem_offset;
    memset(base + 0x003E0890u, 0, 0xC04u * 4u);
    *(uint32_t *)(base + 0x003E0894u) = 0x003E0894u;
    *(uint32_t *)(base + 0x003E0898u) = 0x003E0894u;
    g_eax = 0;
    g_esp += 8; /* ret 4 */
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va == 0x002283CDu) return manual_002283CD;
    if (xbox_va == 0x00223498u) return manual_00223498;
    if (xbox_va == 0x0016E9B0u) return manual_0016E9B0;
    if (xbox_va == 0x002D9E9Cu) return manual_002D9E9C;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu, esp=0x%08X, ecx=0x%08X)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count, g_esp, g_ecx);
    if (hits[i] == 1 && g_xbox_mem_offset && g_esp) {
        const uint8_t *base = (const uint8_t *)g_xbox_mem_offset;
        const uint32_t *sp = (const uint32_t *)(base + g_esp);
        fprintf(stderr, "  guest stack:");
        for (int k = 0, shown = 0; k < 64 && shown < 10; k++) {
            uint32_t v = sp[k];
            if (v >= 0x00010000u && v < 0x002E1C00u) {
                fprintf(stderr, " [+%d]=0x%08X", k * 4, v);
                shown++;
            }
        }
        fprintf(stderr, "\n  regs: eax=0x%08X ebx=0x%08X ecx=0x%08X edx=0x%08X esi=0x%08X edi=0x%08X\n",
                g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi);
        if (g_ecx >= 0x00010000u && g_ecx < 0x84000000u) {
            fprintf(stderr, "  [ecx]:");
            for (int k = 0; k < 12; k++)
                fprintf(stderr, " %08X", *(const uint32_t *)(base + g_ecx + k * 4));
            fprintf(stderr, "\n");
        }
        if (g_ebx >= 0x00010000u && g_ebx < 0x84000000u) {
            fprintf(stderr, "  [ebx]:");
            for (int k = 0; k < 12; k++)
                fprintf(stderr, " %08X", *(const uint32_t *)(base + g_ebx + k * 4));
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "  recent ICALLs:");
        for (int k = 0; k < 16; k++)
            fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}
