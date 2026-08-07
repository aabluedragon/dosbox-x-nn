/*
 *  WebAssembly backend for the DOSBox-X recompiling core.
 *
 *  Native backends emit host machine code into the cache arena. WebAssembly
 *  cannot execute bytes written to linear memory, so this backend works in
 *  two stages:
 *
 *    1. The gen_* emitter interface writes fixed-size IR records into the
 *       cache arena. The decoder's patching idioms (branch fills, call
 *       rewriting via gen_fill_function_ptr) mutate those records in place.
 *    2. When the block is closed, the IR is translated into a small
 *       WebAssembly module (one exported function) which imports the main
 *       module's linear memory and indirect function table. It is compiled
 *       and instantiated synchronously and registered in the shared table;
 *       the table index (tagged with bit 31) is stored in cache.xstart as
 *       the block's "code address".
 *
 *  Because C function pointers under Emscripten *are* table indices, helper
 *  calls become call_indirect with the pointer value as the target, and
 *  block chaining (gen_jmp_ptr reads link[i].to->cache.xstart at runtime)
 *  becomes a return_call_indirect tail call - linking, unlinking and
 *  self-modifying-code invalidation work unchanged.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include <emscripten.h>
#include <type_traits>
#include <vector>
#include <cstdio>

// configuring defines
#define DRC_FLAGS_INVALIDATION
#define DRC_FLAGS_INVALIDATION_DCODE

#define DRC_PTR_SIZE_IM uint32_t
#define DRC_CALL_CONV
#define DRC_FC

// register mapping: HostReg values are wasm local indices of the block function
typedef uint8_t HostReg;

#define FC_RETOP      0
#define FC_ADDR       1
#define FC_OP1        2
#define FC_OP2        3
#define FC_OP3        4
#define FC_TMP_BA1    5
#define FC_TMP_BA2    6
#define TEMP_REG_DRC  7
#define DRC_NUM_LOCALS 13  /* 0..7 are the decoder's registers, 8..12 are ours */
#define DRC_LOCAL_SCRATCH 8
#define DRC_LOCAL_V1      9
#define DRC_LOCAL_V2      10
#define DRC_LOCAL_RES     11
#define DRC_LOCAL_SIGN    12
/* wasm zero-initialises every local on entry, so a block that never uses the
 * extra four should not declare them */
static bool drc_used_extra_locals = false;

/* The value lflags.type will hold at the record being translated, or -1 when
 * it cannot be known. The flag producer is nearly always in the same block as
 * its consumer - blocks end at branches, so a cmp travels with its jcc and an
 * add with its adc - which lets the consumers emit the one formula flags.cpp
 * would have picked instead of dispatching on the type at runtime. Set by the
 * inlined flag writers, forgotten at branch targets and at any call that may
 * clobber the lazy state. */
static int drc_flag_type = -1;

#define DRC_XSTART_TAG 0x80000000u

/* ------------------------------------------------------------------ */
/* IR records                                                          */
/* ------------------------------------------------------------------ */

enum {
    DRCIR_MOVRR = 1,   // a=dst b=src
    DRCIR_MOVI,        // a=dst x=imm
    DRCIR_LOADW,       // a=dst x=addr b=dword
    DRCIR_LOADB,       // a=dst x=addr
    DRCIR_STOREW,      // a=src x=addr b=dword
    DRCIR_STOREB,      // a=src x=addr
    DRCIR_EXT,         // a=reg b=sign c=width(8/16)
    DRCIR_ADDMEM,      // a=reg x=addr
    DRCIR_ADDI,        // a=reg x=imm
    DRCIR_ANDI,        // a=reg x=imm
    DRCIR_BOOL,        // a=reg: reg = (reg != 0) ? 1 : 0
    DRCIR_RETDISP,     // dispatch-cache lookup of cs:eip; tail-call on hit
    DRCIR_SHRI,        // a=reg b=imm
    DRCIR_LEA2,        // a=dst b=scale_reg c=scale x=imm
    DRCIR_LEA1,        // a=dst c=scale x=imm
    DRCIR_STOREI,      // x=addr y=imm (32bit)
    DRCIR_ADDDIR,      // x=addr y=imm b=dword
    DRCIR_CALL,        // x=funcidx b=nargs c=hasret
    DRCIR_SIMPLE,      // x=flags_type (RETOP = OP1 <op> OP2)
    DRCIR_JMPPTR,      // x=ptr y=offset  (tail call *( *(u32*)ptr + offset ))
    DRCIR_BR,          // a=cond(0=z,1=nz,2=leqz) b=reg c=width(8/16/32) x=target(abs IR ptr)
    DRCIR_RET,         // return FC_RETOP
};

struct DRCIRRec {
    uint8_t op, a, b, c;
    uint32_t x, y, z;
};

static INLINE DRCIRRec* drcir_emit(uint8_t op) {
    DRCIRRec* r = (DRCIRRec*)cache.pos;
    r->op = op; r->a = 0; r->b = 0; r->c = 0;
    r->x = 0; r->y = 0; r->z = 0;
    cache.pos += sizeof(DRCIRRec);
    return r;
}

/* ------------------------------------------------------------------ */
/* JS glue: compile bytes -> table index, and slot recycling           */
/* ------------------------------------------------------------------ */

EM_JS(int, drc_wasm_compile, (const uint8_t* bytes, int len), {
    const t0 = performance.now();
    try {
        const buf = HEAPU8.subarray(bytes, bytes + len);
        const mod = new WebAssembly.Module(buf);
        const imports = { e: { t: wasmTable, m: wasmMemory }, f: {} };
        for (const im of WebAssembly.Module.imports(mod)) {
            if (im.module === 'f') imports.f[im.name] = wasmTable.get(parseInt(im.name, 10));
        }
        const inst = new WebAssembly.Instance(mod, imports);
        let idx;
        if (Module.__drcFree && Module.__drcFree.length) idx = Module.__drcFree.pop();
        else {
            /* grow the table in chunks: growing one slot at a time is O(n) per call */
            if (!Module.__drcSpare || Module.__drcSpare.length === 0) {
                const base = wasmTable.grow(256);
                Module.__drcSpare = [];
                for (let i = 255; i >= 0; i--) Module.__drcSpare.push(base + i);
            }
            idx = Module.__drcSpare.pop();
        }
        wasmTable.set(idx, inst.exports.f);
        Module.__drcCount = (Module.__drcCount || 0) + 1;
        const dt = performance.now() - t0;
        Module.__drcTime = (Module.__drcTime || 0) + dt;
        if (!Module.__drcEvents) Module.__drcEvents = [];
        Module.__drcEvents.push(t0, dt);
        if (Module.__drcEvents.length > 4000) Module.__drcEvents.splice(0, 2000);
        return idx;
    } catch (e) {
        Module.__drcFail = (Module.__drcFail || 0) + 1;
        if (Module.__drcFail <= 3) console.warn('dynrec: block compile failed:', e.message || e);
        return 0;
    }
});

/* how many blocks were rejected before we even tried to compile them */
EM_JS(void, drc_note_bail, (int reason), {
    if (!Module.__drcBail) Module.__drcBail = {};
    Module.__drcBail[reason] = (Module.__drcBail[reason] || 0) + 1;
});

/* diagnostic: which opcodes the translator rejected (BR_Opcode) - each one
 * costs a full core exit and an interpreter round trip per execution */
EM_JS(void, drc_note_op, (int b0, int b1, int b2), {
    if (!Module.__drcOps) Module.__drcOps = {};
    const k = b0.toString(16) + ':' + b1.toString(16) + ':' + b2.toString(16);
    Module.__drcOps[k] = (Module.__drcOps[k] || 0) + 1;
});

/* diagnostic: histogram of self-modifying-code invalidations, keyed by
 * physical page and 16-byte line of the written address, so a capture can
 * show whether SMC churn concentrates on a few patch points */
EM_JS(void, drc_note_smc, (int page, int addr, int bstart, int hostptr), {
    if (!Module.__smc) Module.__smc = {};
    const k = page.toString(16) + ':' + (addr >> 4).toString(16) + ':' + (bstart >> 4).toString(16);
    Module.__smc[k] = (Module.__smc[k] || 0) + 1;
    /* snapshot the code bytes around the patch point a few times so the
     * patched instruction can be disassembled (and diffed across writes) */
    if (!Module.__smcBytes) Module.__smcBytes = {};
    const seen = Module.__smcBytes[k];
    if (!seen || (seen.length < 4 && Module.__smc[k] % 50 === 0)) {
        const base = hostptr - (addr & 15) - 16;
        const hex = Array.from(HEAPU8.subarray(base, base + 48))
            .map(b => b.toString(16).padStart(2, '0')).join('');
        (Module.__smcBytes[k] = seen || []).push('off' + (addr & 15).toString(16) + ':' + hex);
    }
});

/* debug aid: Module.__drcLimit caps how many blocks may be JIT compiled, so a
 * misbehaving block can be bisected without rebuilding */
EM_JS(int, drc_get_limit, (), {
    return (typeof Module.__drcLimit === 'number') ? Module.__drcLimit : 2000000000;
});

EM_JS(void, drc_wasm_release, (int idx), {
    if (!Module.__drcFree) Module.__drcFree = [];
    try { wasmTable.set(idx, null); } catch (e) {}
    Module.__drcFree.push(idx);
});

/* the checked memory accessors, declared here so generated code can inline
 * their fast path; they are defined later in decoder_basic.h */
bool DRC_CALL_CONV mem_readb_checked_drc(PhysPt address) DRC_FC;
bool DRC_CALL_CONV mem_readw_checked_drc(PhysPt address) DRC_FC;
bool DRC_CALL_CONV mem_readd_checked_drc(PhysPt address) DRC_FC;
bool DRC_CALL_CONV mem_writeb_checked_drc(PhysPt address,uint8_t val) DRC_FC;
bool DRC_CALL_CONV mem_writew_checked_drc(PhysPt address,uint16_t val) DRC_FC;
bool DRC_CALL_CONV mem_writed_checked_drc(PhysPt address,uint32_t val) DRC_FC;
static uint8_t  DRC_CALL_CONV dynrec_adc_byte_simple(uint8_t op1,uint8_t op2) DRC_FC;
static uint16_t DRC_CALL_CONV dynrec_adc_word_simple(uint16_t op1,uint16_t op2) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_adc_dword_simple(uint32_t op1,uint32_t op2) DRC_FC;
static uint8_t  DRC_CALL_CONV dynrec_sbb_byte_simple(uint8_t op1,uint8_t op2) DRC_FC;
static uint16_t DRC_CALL_CONV dynrec_sbb_word_simple(uint16_t op1,uint16_t op2) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_sbb_dword_simple(uint32_t op1,uint32_t op2) DRC_FC;
static void DRC_CALL_CONV dynrec_push_word(uint16_t value) DRC_FC;
static void DRC_CALL_CONV dynrec_push_dword(uint32_t value) DRC_FC;
static uint16_t DRC_CALL_CONV dynrec_pop_word(void) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_pop_dword(void) DRC_FC;
static uint8_t  DRC_CALL_CONV dynrec_adc_byte(uint8_t op1,uint8_t op2) DRC_FC;
static uint16_t DRC_CALL_CONV dynrec_adc_word(uint16_t op1,uint16_t op2) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_adc_dword(uint32_t op1,uint32_t op2) DRC_FC;
static uint8_t  DRC_CALL_CONV dynrec_sbb_byte(uint8_t op1,uint8_t op2) DRC_FC;
static uint16_t DRC_CALL_CONV dynrec_sbb_word(uint16_t op1,uint16_t op2) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_sbb_dword(uint32_t op1,uint32_t op2) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_get_sf(void) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_get_nsf(void) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_get_cf_or_zf(void) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_get_ncf_and_nzf(void) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_get_sf_eq_of(void) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_get_sf_neq_of(void) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_get_zf_or_sf_neq_of(void) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_get_nzf_and_sf_eq_of(void) DRC_FC;

/* Width mask for the types where a branch condition is computable from the
 * compare/subtract operands alone; zero means "call the helper". */
static const uint32_t drc_subcmp_mask[t_LASTFLAG] = {
    [t_SUBb]=0xFFu,[t_CMPb]=0xFFu,
    [t_SUBw]=0xFFFFu,[t_CMPw]=0xFFFFu,
    [t_SUBd]=0xFFFFFFFFu,[t_CMPd]=0xFFFFFFFFu,
};

static uint32_t DRC_CALL_CONV dynrec_get_cf(void) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_get_ncf(void) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_get_zf(void) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_get_nzf(void) DRC_FC;

/* Width mask per lazy flag type for the zero flag: get_ZF() returns
 * lf_res<width>==0 for exactly these types, so a zero entry means "not ours,
 * call the helper". Transcribed from the case lists in flags.cpp - the word
 * and dword groups include the double shifts, the byte group does not. */
static const uint32_t drc_zf_mask[t_LASTFLAG] = {
    [t_ADDb]=0xFFu,[t_ORb]=0xFFu,[t_ADCb]=0xFFu,[t_SBBb]=0xFFu,[t_ANDb]=0xFFu,
    [t_XORb]=0xFFu,[t_SUBb]=0xFFu,[t_CMPb]=0xFFu,[t_INCb]=0xFFu,[t_DECb]=0xFFu,
    [t_TESTb]=0xFFu,[t_SHLb]=0xFFu,[t_SHRb]=0xFFu,[t_SARb]=0xFFu,[t_NEGb]=0xFFu,
    [t_ADDw]=0xFFFFu,[t_ORw]=0xFFFFu,[t_ADCw]=0xFFFFu,[t_SBBw]=0xFFFFu,[t_ANDw]=0xFFFFu,
    [t_XORw]=0xFFFFu,[t_SUBw]=0xFFFFu,[t_CMPw]=0xFFFFu,[t_INCw]=0xFFFFu,[t_DECw]=0xFFFFu,
    [t_TESTw]=0xFFFFu,[t_SHLw]=0xFFFFu,[t_SHRw]=0xFFFFu,[t_SARw]=0xFFFFu,
    [t_DSHLw]=0xFFFFu,[t_DSHRw]=0xFFFFu,[t_NEGw]=0xFFFFu,
    [t_ADDd]=0xFFFFFFFFu,[t_ORd]=0xFFFFFFFFu,[t_ADCd]=0xFFFFFFFFu,[t_SBBd]=0xFFFFFFFFu,
    [t_ANDd]=0xFFFFFFFFu,[t_XORd]=0xFFFFFFFFu,[t_SUBd]=0xFFFFFFFFu,[t_CMPd]=0xFFFFFFFFu,
    [t_INCd]=0xFFFFFFFFu,[t_DECd]=0xFFFFFFFFu,[t_TESTd]=0xFFFFFFFFu,[t_SHLd]=0xFFFFFFFFu,
    [t_SHRd]=0xFFFFFFFFu,[t_SARd]=0xFFFFFFFFu,[t_DSHLd]=0xFFFFFFFFu,[t_DSHRd]=0xFFFFFFFFu,
    [t_NEGd]=0xFFFFFFFFu,
};

/* ------------------------------------------------------------------ */
/* gen_* emitter interface                                             */
/* ------------------------------------------------------------------ */

static void gen_mov_regs(HostReg reg_dst,HostReg reg_src) {
    if (reg_dst == reg_src) return;
    DRCIRRec* r = drcir_emit(DRCIR_MOVRR); r->a = reg_dst; r->b = reg_src;
}

static void gen_mov_dword_to_reg_imm(HostReg dest_reg,uint32_t imm) {
    DRCIRRec* r = drcir_emit(DRCIR_MOVI); r->a = dest_reg; r->x = imm;
}

static void gen_mov_word_to_reg(HostReg dest_reg,void* data,bool dword) {
    DRCIRRec* r = drcir_emit(DRCIR_LOADW); r->a = dest_reg; r->b = dword ? 1 : 0; r->x = (uint32_t)(uintptr_t)data;
}

static void gen_mov_word_to_reg_imm(HostReg dest_reg,uint16_t imm) {
    gen_mov_dword_to_reg_imm(dest_reg, imm);
}

static void gen_mov_word_from_reg(HostReg src_reg,void* dest,bool dword) {
    DRCIRRec* r = drcir_emit(DRCIR_STOREW); r->a = src_reg; r->b = dword ? 1 : 0; r->x = (uint32_t)(uintptr_t)dest;
}

static void gen_mov_byte_to_reg_low(HostReg dest_reg,void* data) {
    DRCIRRec* r = drcir_emit(DRCIR_LOADB); r->a = dest_reg; r->x = (uint32_t)(uintptr_t)data;
}

static void INLINE gen_mov_byte_to_reg_low_canuseword(HostReg dest_reg,void* data) {
    gen_mov_byte_to_reg_low(dest_reg, data);
}

static void INLINE gen_mov_byte_to_reg_low_imm(HostReg dest_reg,uint8_t imm) {
    gen_mov_dword_to_reg_imm(dest_reg, imm);
}

static void INLINE gen_mov_byte_to_reg_low_imm_canuseword(HostReg dest_reg,uint8_t imm) {
    gen_mov_dword_to_reg_imm(dest_reg, imm);
}

static void gen_mov_byte_from_reg_low(HostReg src_reg,void* dest) {
    DRCIRRec* r = drcir_emit(DRCIR_STOREB); r->a = src_reg; r->x = (uint32_t)(uintptr_t)dest;
}

static void gen_extend_byte(bool sign,HostReg reg) {
    DRCIRRec* r = drcir_emit(DRCIR_EXT); r->a = reg; r->b = sign ? 1 : 0; r->c = 8;
}

static void gen_extend_word(bool sign,HostReg reg) {
    DRCIRRec* r = drcir_emit(DRCIR_EXT); r->a = reg; r->b = sign ? 1 : 0; r->c = 16;
}

static void gen_add(HostReg reg,void* op) {
    DRCIRRec* r = drcir_emit(DRCIR_ADDMEM); r->a = reg; r->x = (uint32_t)(uintptr_t)op;
}

static void gen_add_imm(HostReg reg,uint32_t imm) {
    if (!imm) return;
    DRCIRRec* r = drcir_emit(DRCIR_ADDI); r->a = reg; r->x = imm;
}

static void gen_and_imm(HostReg reg,uint32_t imm) {
    DRCIRRec* r = drcir_emit(DRCIR_ANDI); r->a = reg; r->x = imm;
}

/* reg = reg != 0 - the branch-condition helpers return raw flag masks, not
 * booleans, so consumers that need a strict 0/1 (SETcc) normalize this way */
static void gen_bool_reg(HostReg reg) {
    DRCIRRec* r = drcir_emit(DRCIR_BOOL); r->a = reg;
}

/* End the block with a dispatch-cache lookup of the new cs:eip and a tail
 * call straight into the target block on a hit; a miss returns BR_Normal to
 * the C dispatcher, which performs the full lookup and refills the cache.
 * This keeps RET and computed-jump chains inside generated code - the C
 * dispatcher ran 13 million times a second in Duke3D without it. */
static void INLINE gen_ret_dispatch(void) {
    drcir_emit(DRCIR_RETDISP);
}

static void gen_shr_imm(HostReg reg,uint8_t imm) {
    DRCIRRec* r = drcir_emit(DRCIR_SHRI); r->a = reg; r->b = imm;
}

static void INLINE gen_mov_direct_dword(void* dest,uint32_t imm) {
    DRCIRRec* r = drcir_emit(DRCIR_STOREI); r->x = (uint32_t)(uintptr_t)dest; r->y = imm;
}

static void INLINE gen_mov_direct_ptr(void* dest,DRC_PTR_SIZE_IM imm) {
    gen_mov_direct_dword(dest, (uint32_t)imm);
}

static void INLINE gen_add_direct_word(void* dest,uint32_t imm,bool dword) {
    if (!imm) return;
    DRCIRRec* r = drcir_emit(DRCIR_ADDDIR); r->x = (uint32_t)(uintptr_t)dest; r->y = imm; r->b = dword ? 1 : 0;
}

static void INLINE gen_add_direct_byte(void* dest,int8_t imm) {
    gen_add_direct_word(dest, (uint32_t)(int32_t)imm, true);
}

static void INLINE gen_sub_direct_byte(void* dest,int8_t imm) {
    gen_add_direct_word(dest, (uint32_t)(-(int32_t)imm), true);
}

static void INLINE gen_sub_direct_word(void* dest,uint32_t imm,bool dword) {
    gen_add_direct_word(dest, (uint32_t)(-(int32_t)imm), dword);
}

static INLINE void gen_lea(HostReg dest_reg,HostReg scale_reg,Bitu scale,Bits imm) {
    DRCIRRec* r = drcir_emit(DRCIR_LEA2); r->a = dest_reg; r->b = scale_reg; r->c = (uint8_t)scale; r->x = (uint32_t)imm;
}

static INLINE void gen_lea(HostReg dest_reg,Bitu scale,Bits imm) {
    DRCIRRec* r = drcir_emit(DRCIR_LEA1); r->a = dest_reg; r->c = (uint8_t)scale; r->x = (uint32_t)imm;
}

/* signature extraction so call_indirect always uses the callee's exact type */
template <typename T> struct drc_sig;
template <typename R, typename... A> struct drc_sig<R(*)(A...)> {
    static const unsigned nargs = (unsigned)sizeof...(A);
    static const unsigned hasret = std::is_void<R>::value ? 0u : 1u;
};
template <typename R, typename... A> struct drc_sig<R(A...)> : drc_sig<R(*)(A...)> {};

template <typename T> static void INLINE gen_call_function_raw(const T func) {
    DRCIRRec* r = drcir_emit(DRCIR_CALL);
    r->x = (uint32_t)(uintptr_t)func;   /* emscripten fn ptr == table index */
    r->b = (uint8_t)drc_sig<T>::nargs;
    r->c = (uint8_t)drc_sig<T>::hasret;
}

template <typename T> static DRC_PTR_SIZE_IM INLINE gen_call_function_setup(const T func,Bitu paramcount,bool fastcall=false) {
    (void)paramcount; (void)fastcall;
    DRC_PTR_SIZE_IM proc_addr = (DRC_PTR_SIZE_IM)(uintptr_t)cache.pos;
    gen_call_function_raw(func);
    return proc_addr;
}

// parameters are passed in the FC_OP1..FC_OP3 locals
static void INLINE gen_load_param_imm(Bitu imm,Bitu param) {
    gen_mov_dword_to_reg_imm((HostReg)(FC_OP1 + param), (uint32_t)imm);
}

static void INLINE gen_load_param_addr(DRC_PTR_SIZE_IM addr,Bitu param) {
    gen_mov_dword_to_reg_imm((HostReg)(FC_OP1 + param), (uint32_t)addr);
}

static void INLINE gen_load_param_reg(Bitu reg,Bitu param) {
    gen_mov_regs((HostReg)(FC_OP1 + param), (HostReg)reg);
}

static void INLINE gen_load_param_mem(Bitu mem,Bitu param) {
    gen_mov_word_to_reg((HostReg)(FC_OP1 + param), (void*)mem, true);
}

static void INLINE gen_jmp_ptr(void * ptr,Bits imm=0) {
    DRCIRRec* r = drcir_emit(DRCIR_JMPPTR); r->x = (uint32_t)(uintptr_t)ptr; r->y = (uint32_t)imm;
}

static DRC_PTR_SIZE_IM INLINE gen_create_branch_on_zero(HostReg reg,bool dword) {
    DRCIRRec* r = drcir_emit(DRCIR_BR); r->a = 0; r->b = reg; r->c = dword ? 32 : 16;
    return (DRC_PTR_SIZE_IM)(uintptr_t)&r->x;
}

static DRC_PTR_SIZE_IM INLINE gen_create_branch_on_nonzero(HostReg reg,bool dword) {
    DRCIRRec* r = drcir_emit(DRCIR_BR); r->a = 1; r->b = reg; r->c = dword ? 32 : 16;
    return (DRC_PTR_SIZE_IM)(uintptr_t)&r->x;
}

static void INLINE gen_fill_branch(DRC_PTR_SIZE_IM data) {
    *(uint32_t*)(uintptr_t)data = (uint32_t)(uintptr_t)cache.pos;
}

static DRC_PTR_SIZE_IM gen_create_branch_long_nonzero(HostReg reg,bool isdword) {
    DRCIRRec* r = drcir_emit(DRCIR_BR); r->a = 1; r->b = reg; r->c = isdword ? 32 : 8;
    return (DRC_PTR_SIZE_IM)(uintptr_t)&r->x;
}

static DRC_PTR_SIZE_IM INLINE gen_create_branch_long_leqzero(HostReg reg) {
    DRCIRRec* r = drcir_emit(DRCIR_BR); r->a = 2; r->b = reg; r->c = 32;
    return (DRC_PTR_SIZE_IM)(uintptr_t)&r->x;
}

static void INLINE gen_fill_branch_long(DRC_PTR_SIZE_IM data) {
    gen_fill_branch(data);
}

static void gen_return_function(void) {
    drcir_emit(DRCIR_RET);
}

// the trampoline is a plain C function for this backend (see WASM_RunCode)
static void gen_run_code(void) {}

#ifdef DRC_FLAGS_INVALIDATION
// rewrite a call record into inline code (or retarget it) once the decoder
// knows the lazy flags are not needed
static void gen_fill_function_ptr(uint8_t * pos,void* fct_ptr,Bitu flags_type) {
    DRCIRRec* r = (DRCIRRec*)pos;
    switch (flags_type) {
        case t_ADDb: case t_ADDw: case t_ADDd:
        case t_ORb:  case t_ORw:  case t_ORd:
        case t_ANDb: case t_ANDw: case t_ANDd:
        case t_SUBb: case t_SUBw: case t_SUBd:
        case t_XORb: case t_XORw: case t_XORd:
        case t_CMPb: case t_CMPw: case t_CMPd:
        case t_TESTb:case t_TESTw:case t_TESTd:
        case t_INCb: case t_INCw: case t_INCd:
        case t_DECb: case t_DECw: case t_DECd:
        case t_SHLb: case t_SHLw: case t_SHLd:
        case t_SHRb: case t_SHRw: case t_SHRd:
        case t_SARd:
        case t_RORd:
        case t_NEGb: case t_NEGw: case t_NEGd:
            r->op = DRCIR_SIMPLE;
            r->x = (uint32_t)flags_type;
            break;
        default:
            /* keep it a call, but to the simpler function (same signature) */
            r->op = DRCIR_CALL;
            r->x = (uint32_t)(uintptr_t)fct_ptr;
            break;
    }
}
#endif

/* ------------------------------------------------------------------ */
/* IR -> wasm translation                                              */
/* ------------------------------------------------------------------ */

static std::vector<uint8_t> drc_buf;

static INLINE void drc_b(uint8_t v) { drc_buf.push_back(v); }
static void drc_uleb(uint32_t v) {
    do { uint8_t b = v & 0x7f; v >>= 7; if (v) b |= 0x80; drc_b(b); } while (v);
}
static void drc_sleb(int32_t v) {
    for (;;) {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) { drc_b(b); return; }
        drc_b(b | 0x80);
    }
}
static void drc_iconst(uint32_t v) { drc_b(0x41); drc_sleb((int32_t)v); }
static void drc_getl(uint32_t l) { drc_b(0x20); drc_uleb(l); }
static void drc_setl(uint32_t l) { drc_b(0x21); drc_uleb(l); }
static void drc_teel(uint32_t l) { drc_b(0x22); drc_uleb(l); }
static void drc_load32(void)  { drc_b(0x28); drc_uleb(0); drc_uleb(0); }
static void drc_load16u(void) { drc_b(0x2F); drc_uleb(0); drc_uleb(0); }
static void drc_load8u(void)  { drc_b(0x2D); drc_uleb(0); drc_uleb(0); }
static void drc_store32(void)  { drc_b(0x36); drc_uleb(0); drc_uleb(0); }
static void drc_store16(void)  { drc_b(0x3B); drc_uleb(0); drc_uleb(0); }
static void drc_store8(void)   { drc_b(0x3A); drc_uleb(0); drc_uleb(0); }

/* type indices in the emitted module: 0: ()->i32 (block sig),
 * 1..5: (i32 x 0..4)->i32,  6..10: (i32 x 0..4)->void */
#define DRC_TYPE_BLOCK 0
static INLINE uint32_t drc_calltype(unsigned nargs, unsigned hasret) {
    return hasret ? (1 + nargs) : (6 + nargs);
}

/* store-into-byte-of-value: RETOP = OP1 op OP2 without flags */
static void drc_emit_simple(uint32_t t) {
    switch (t) {
        case t_CMPb: case t_CMPw: case t_CMPd:
        case t_TESTb:case t_TESTw:case t_TESTd:
            return; /* result unused */
        case t_INCb: case t_INCw: case t_INCd:
            drc_getl(FC_OP1); drc_iconst(1); drc_b(0x6A); drc_setl(FC_RETOP); return;
        case t_DECb: case t_DECw: case t_DECd:
            drc_getl(FC_OP1); drc_iconst(1); drc_b(0x6B); drc_setl(FC_RETOP); return;
        case t_NEGb: case t_NEGw: case t_NEGd:
            drc_iconst(0); drc_getl(FC_OP1); drc_b(0x6B); drc_setl(FC_RETOP); return;
        default: break;
    }
    uint8_t op;
    switch (t) {
        case t_ADDb: case t_ADDw: case t_ADDd: op = 0x6A; break; /* add */
        case t_SUBb: case t_SUBw: case t_SUBd: op = 0x6B; break; /* sub */
        case t_ANDb: case t_ANDw: case t_ANDd: op = 0x71; break; /* and */
        case t_ORb:  case t_ORw:  case t_ORd:  op = 0x72; break; /* or  */
        case t_XORb: case t_XORw: case t_XORd: op = 0x73; break; /* xor */
        case t_SHLb: case t_SHLw: case t_SHLd: op = 0x74; break; /* shl */
        case t_SHRb: case t_SHRw: case t_SHRd: op = 0x76; break; /* shr_u */
        case t_SARd:                           op = 0x75; break; /* shr_s */
        case t_RORd:                           op = 0x78; break; /* rotr */
        default: return;
    }
    drc_getl(FC_OP1); drc_getl(FC_OP2); drc_b(op); drc_setl(FC_RETOP);
}

/* Inline the fast path of a checked memory accessor instead of calling it.
 * Each accessor looks up paging.tlb.read/write[address>>12]; a non-null entry
 * means the access is plain memory and becomes a single load or store, with
 * the helper called only on a miss. The wider accesses keep their guard
 * against straddling a page boundary, where the helper does more work.
 * Returns false if this call is not one of them. */
static bool drc_emit_mem_inline(uint32_t target, unsigned import_slot) {
    const uint32_t tlb_read  = (uint32_t)(uintptr_t)&paging.tlb.read[0];
    const uint32_t tlb_write = (uint32_t)(uintptr_t)&paging.tlb.write[0];
    const uint32_t readdata  = (uint32_t)(uintptr_t)&core_dynrec.readdata;

    unsigned width;             /* 1, 2 or 4 bytes */
    bool is_write;
    if      (target == (uint32_t)(uintptr_t)mem_readb_checked_drc)  { width = 1; is_write = false; }
    else if (target == (uint32_t)(uintptr_t)mem_readw_checked_drc)  { width = 2; is_write = false; }
    else if (target == (uint32_t)(uintptr_t)mem_readd_checked_drc)  { width = 4; is_write = false; }
    else if (target == (uint32_t)(uintptr_t)mem_writeb_checked_drc) { width = 1; is_write = true; }
    else if (target == (uint32_t)(uintptr_t)mem_writew_checked_drc) { width = 2; is_write = true; }
    else if (target == (uint32_t)(uintptr_t)mem_writed_checked_drc) { width = 4; is_write = true; }
    else return false;

    /* tlb entry, or 0 when the access straddles a page and the helper must
     * handle it: (addr & 0xfff) < limit ? tlb[addr>>12] : 0 */
    const uint32_t limit = (width == 1) ? 0 : (width == 2 ? 0xfffu : 0xffdu);
    if (limit) {
        drc_getl(FC_OP1); drc_iconst(0xfff); drc_b(0x71);        /* and  */
        drc_iconst(limit); drc_b(0x49);                          /* lt_u */
        drc_b(0x04); drc_b(0x7F);                                /* if (result i32) */
    }
    drc_getl(FC_OP1); drc_iconst(12); drc_b(0x76);               /* shr_u */
    drc_iconst(2); drc_b(0x74);                                  /* shl   */
    drc_iconst(is_write ? tlb_write : tlb_read); drc_b(0x6A);    /* add   */
    drc_load32();
    if (limit) {
        drc_b(0x05); drc_iconst(0); drc_b(0x0B);                 /* else 0; end */
    }
    drc_teel(DRC_LOCAL_SCRATCH);
    drc_b(0x04); drc_b(0x40);                                    /* if (non-null) */

    if (is_write) {
        drc_getl(DRC_LOCAL_SCRATCH); drc_getl(FC_OP1); drc_b(0x6A);
        drc_getl(FC_OP2);
        if (width == 1) drc_store8(); else if (width == 2) drc_store16(); else drc_store32();
    } else {
        drc_iconst(readdata);
        drc_getl(DRC_LOCAL_SCRATCH); drc_getl(FC_OP1); drc_b(0x6A);
        if (width == 1) drc_load8u(); else if (width == 2) drc_load16u(); else drc_load32();
        if (width == 1) drc_store8(); else if (width == 2) drc_store16(); else drc_store32();
    }
    drc_iconst(0); drc_setl(FC_RETOP);                           /* no fault */

    drc_b(0x05);                                                 /* else: call it */
    drc_getl(FC_OP1);
    if (is_write) drc_getl(FC_OP2);
    drc_b(0x10); drc_uleb(import_slot);
    drc_setl(FC_RETOP);
    drc_b(0x0B);                                                 /* end */
    return true;
}

/* Inline the near stack helpers. CALL/RET-heavy protected mode code pushes
 * and pops at the dispatcher's rate - the stack helpers' self time was ~10%
 * of a Duke3D profile. The fast path is the same TLB probe the memory inline
 * uses, with the ESP update fused; a page-straddling or unmapped access
 * falls back to the original helper, which also preserves fault ordering
 * (memory access first, ESP update after). */
static bool drc_emit_pushpop_inline(uint32_t target, unsigned import_slot) {
    unsigned width; bool is_push;
    if      (target == (uint32_t)(uintptr_t)dynrec_push_dword) { width = 4; is_push = true;  }
    else if (target == (uint32_t)(uintptr_t)dynrec_push_word)  { width = 2; is_push = true;  }
    else if (target == (uint32_t)(uintptr_t)dynrec_pop_dword)  { width = 4; is_push = false; }
    else if (target == (uint32_t)(uintptr_t)dynrec_pop_word)   { width = 2; is_push = false; }
    else return false;

    const uint32_t a_esp     = (uint32_t)(uintptr_t)&reg_esp;
    const uint32_t a_mask    = (uint32_t)(uintptr_t)&cpu.stack.mask;
    const uint32_t a_notmask = (uint32_t)(uintptr_t)&cpu.stack.notmask;
    const uint32_t a_ssphys  = (uint32_t)(uintptr_t)&Segs.phys[ss];
    const uint32_t tlb_read  = (uint32_t)(uintptr_t)&paging.tlb.read[0];
    const uint32_t tlb_write = (uint32_t)(uintptr_t)&paging.tlb.write[0];
    const uint32_t limit = (width == 2) ? 0xfffu : 0xffdu;

    drc_used_extra_locals = true;

    if (is_push) {
        /* V1 = new_esp = (esp & notmask) | ((esp - width) & mask) */
        drc_iconst(a_esp); drc_load32(); drc_iconst(a_notmask); drc_load32(); drc_b(0x71);
        drc_iconst(a_esp); drc_load32(); drc_iconst(width); drc_b(0x6B);
        drc_iconst(a_mask); drc_load32(); drc_b(0x71);
        drc_b(0x72); drc_setl(DRC_LOCAL_V1);
        /* V2 = linear = ssphys + (new_esp & mask) */
        drc_getl(DRC_LOCAL_V1); drc_iconst(a_mask); drc_load32(); drc_b(0x71);
        drc_iconst(a_ssphys); drc_load32(); drc_b(0x6A);
        drc_setl(DRC_LOCAL_V2);
    } else {
        /* V2 = linear = ssphys + (esp & mask) */
        drc_iconst(a_esp); drc_load32(); drc_iconst(a_mask); drc_load32(); drc_b(0x71);
        drc_iconst(a_ssphys); drc_load32(); drc_b(0x6A);
        drc_setl(DRC_LOCAL_V2);
    }

    /* tlb entry, or 0 when the access straddles a page */
    drc_getl(DRC_LOCAL_V2); drc_iconst(0xfff); drc_b(0x71);
    drc_iconst(limit); drc_b(0x49);
    drc_b(0x04); drc_b(0x7F);                                    /* if (result i32) */
    drc_getl(DRC_LOCAL_V2); drc_iconst(12); drc_b(0x76);
    drc_iconst(2); drc_b(0x74);
    drc_iconst(is_push ? tlb_write : tlb_read); drc_b(0x6A);
    drc_load32();
    drc_b(0x05); drc_iconst(0); drc_b(0x0B);
    drc_teel(DRC_LOCAL_SCRATCH);
    drc_b(0x04); drc_b(0x40);                                    /* if (mapped) */

    if (is_push) {
        drc_getl(DRC_LOCAL_SCRATCH); drc_getl(DRC_LOCAL_V2); drc_b(0x6A);
        drc_getl(FC_OP1);
        if (width == 2) drc_store16(); else drc_store32();
        drc_iconst(a_esp); drc_getl(DRC_LOCAL_V1); drc_store32();
    } else {
        drc_getl(DRC_LOCAL_SCRATCH); drc_getl(DRC_LOCAL_V2); drc_b(0x6A);
        if (width == 2) drc_load16u(); else drc_load32();
        drc_setl(FC_RETOP);
        /* esp = (esp & notmask) | ((esp + width) & mask) */
        drc_iconst(a_esp);
        drc_iconst(a_esp); drc_load32(); drc_iconst(a_notmask); drc_load32(); drc_b(0x71);
        drc_iconst(a_esp); drc_load32(); drc_iconst(width); drc_b(0x6A);
        drc_iconst(a_mask); drc_load32(); drc_b(0x71);
        drc_b(0x72);
        drc_store32();
    }

    drc_b(0x05);                                                 /* else: helper */
    if (is_push) { drc_getl(FC_OP1); drc_b(0x10); drc_uleb(import_slot); }
    else { drc_b(0x10); drc_uleb(import_slot); drc_setl(FC_RETOP); }
    drc_b(0x0B);
    return true;
}

/* the memory accessors never touch the lazy flag state, so a call to one
 * does not invalidate the tracked flag type */
static INLINE bool drc_call_is_mem(uint32_t target) {
    return target == (uint32_t)(uintptr_t)mem_readb_checked_drc
        || target == (uint32_t)(uintptr_t)mem_readw_checked_drc
        || target == (uint32_t)(uintptr_t)mem_readd_checked_drc
        || target == (uint32_t)(uintptr_t)mem_writeb_checked_drc
        || target == (uint32_t)(uintptr_t)mem_writew_checked_drc
        || target == (uint32_t)(uintptr_t)mem_writed_checked_drc;
}

static INLINE bool drc_touches_lflags(uint32_t addr, uint32_t width) {
    const uint32_t lo = (uint32_t)(uintptr_t)&lflags;
    return addr + width > lo && addr < lo + (uint32_t)sizeof(lflags);
}

/* operand width of a lazy flag type */
static uint32_t drc_type_mask(int t) {
    switch (t) {
    case t_ADDb: case t_ORb: case t_ADCb: case t_SBBb: case t_ANDb:
    case t_XORb: case t_SUBb: case t_CMPb: case t_INCb: case t_DECb:
    case t_TESTb: case t_SHLb: case t_SHRb: case t_SARb: case t_NEGb:
        return 0xFFu;
    case t_ADDw: case t_ORw: case t_ADCw: case t_SBBw: case t_ANDw:
    case t_XORw: case t_SUBw: case t_CMPw: case t_INCw: case t_DECw:
    case t_TESTw: case t_SHLw: case t_SHRw: case t_SARw: case t_NEGw:
    case t_DSHLw: case t_DSHRw:
        return 0xFFFFu;
    default:
        return 0xFFFFFFFFu;
    }
}

enum { DRC_F_CF, DRC_F_ZF, DRC_F_SF, DRC_F_OF };

/* With the flag type known, push the given flag as an i32 0/1 - the exact
 * formula the matching case in flags.cpp computes. With emit=false, only
 * answer whether the type/flag pair is statically computable; a false return
 * means the caller must call the helper. The shift-count-dependent carries
 * are left out. */
static bool drc_flag_emit(int t, int which, bool emit) {
    if (t < 0 || t >= t_LASTFLAG) return false;
    const uint32_t a_var1 = (uint32_t)(uintptr_t)&lf_var1d;
    const uint32_t a_var2 = (uint32_t)(uintptr_t)&lf_var2d;
    const uint32_t a_res  = (uint32_t)(uintptr_t)&lf_resd;
    const uint32_t a_flag = (uint32_t)(uintptr_t)DRCD_REG_FLAGS;
    const uint32_t a_oldcf = (uint32_t)(uintptr_t)&lflags.oldcf;
    const uint32_t m = drc_type_mask(t);
    const uint32_t sign = (m >> 1) + 1;

    /* a lazy operand, masked to the operand width: narrow helpers store into
     * the low bytes of the union, the rest is leftover from wider ops */
    #define DRC_LOADM(a) do { drc_iconst(a); drc_load32(); \
        if (m != 0xFFFFFFFFu) { drc_iconst(m); drc_b(0x71); } } while (0)
    /* one bit of the flags word as 0/1 */
    #define DRC_FLAGBIT(bit) do { drc_iconst(a_flag); drc_load32(); \
        drc_iconst(bit); drc_b(0x71); drc_iconst(0); drc_b(0x47); } while (0)

    switch (which) {
    case DRC_F_ZF:
        if (t == t_UNKNOWN) { if (emit) DRC_FLAGBIT(0x40u); return true; }
        if (t == t_MUL || t == t_DIV) { if (emit) drc_iconst(0); return true; }
        if (!drc_zf_mask[t]) return false;
        if (emit) { DRC_LOADM(a_res); drc_b(0x45); }
        return true;
    case DRC_F_SF:
        /* same case list as the zero flag; the sign bit is always within the
         * valid low bytes, so no width mask is needed */
        if (t == t_UNKNOWN) { if (emit) DRC_FLAGBIT(0x80u); return true; }
        if (t == t_MUL || t == t_DIV) { if (emit) drc_iconst(0); return true; }
        if (!drc_zf_mask[t]) return false;
        if (emit) { drc_iconst(a_res); drc_load32(); drc_iconst(sign); drc_b(0x71);
                    drc_iconst(0); drc_b(0x47); }
        return true;
    case DRC_F_CF:
        switch (t) {
        case t_ADDb: case t_ADDw: case t_ADDd:
            if (emit) { DRC_LOADM(a_res); DRC_LOADM(a_var1); drc_b(0x49); }
            return true;
        case t_SUBb: case t_SUBw: case t_SUBd:
        case t_CMPb: case t_CMPw: case t_CMPd:
            if (emit) { DRC_LOADM(a_var1); DRC_LOADM(a_var2); drc_b(0x49); }
            return true;
        case t_ADCb: case t_ADCw: case t_ADCd:
            /* (res < var1) || (oldcf && res == var1) */
            if (emit) {
                DRC_LOADM(a_res); DRC_LOADM(a_var1); drc_b(0x49);
                drc_iconst(a_oldcf); drc_load8u();
                DRC_LOADM(a_res); DRC_LOADM(a_var1); drc_b(0x46);
                drc_b(0x71); drc_b(0x72);
            }
            return true;
        case t_SBBb: case t_SBBw: case t_SBBd:
            /* (var1 < res) || (oldcf && var2 == mask) */
            if (emit) {
                DRC_LOADM(a_var1); DRC_LOADM(a_res); drc_b(0x49);
                drc_iconst(a_oldcf); drc_load8u();
                DRC_LOADM(a_var2); drc_iconst(m); drc_b(0x46);
                drc_b(0x71); drc_b(0x72);
            }
            return true;
        case t_ANDb: case t_ANDw: case t_ANDd:
        case t_ORb:  case t_ORw:  case t_ORd:
        case t_XORb: case t_XORw: case t_XORd:
        case t_TESTb: case t_TESTw: case t_TESTd:
        case t_DIV:
            if (emit) drc_iconst(0);
            return true;
        case t_NEGb: case t_NEGw: case t_NEGd:
            if (emit) { DRC_LOADM(a_var1); drc_iconst(0); drc_b(0x47); }
            return true;
        case t_INCb: case t_INCw: case t_INCd:
        case t_DECb: case t_DECw: case t_DECd:
        case t_UNKNOWN: case t_MUL:
            if (emit) DRC_FLAGBIT(1u);
            return true;
        default:
            return false;
        }
    case DRC_F_OF:
        switch (t) {
        case t_ADDb: case t_ADDw: case t_ADDd:
        case t_ADCb: case t_ADCw: case t_ADCd:
            /* ((v1 ^ v2 ^ sign) & (res ^ v2)) & sign */
            if (emit) {
                drc_iconst(a_var1); drc_load32();
                drc_iconst(a_var2); drc_load32(); drc_b(0x73);
                drc_iconst(sign); drc_b(0x73);
                drc_iconst(a_res); drc_load32();
                drc_iconst(a_var2); drc_load32(); drc_b(0x73);
                drc_b(0x71); drc_iconst(sign); drc_b(0x71);
                drc_iconst(0); drc_b(0x47);
            }
            return true;
        case t_SUBb: case t_SUBw: case t_SUBd:
        case t_CMPb: case t_CMPw: case t_CMPd:
        case t_SBBb: case t_SBBw: case t_SBBd:
            /* ((v1 ^ v2) & (v1 ^ res)) & sign */
            if (emit) {
                drc_iconst(a_var1); drc_load32();
                drc_iconst(a_var2); drc_load32(); drc_b(0x73);
                drc_iconst(a_var1); drc_load32();
                drc_iconst(a_res); drc_load32(); drc_b(0x73);
                drc_b(0x71); drc_iconst(sign); drc_b(0x71);
                drc_iconst(0); drc_b(0x47);
            }
            return true;
        case t_ANDb: case t_ANDw: case t_ANDd:
        case t_ORb:  case t_ORw:  case t_ORd:
        case t_XORb: case t_XORw: case t_XORd:
        case t_TESTb: case t_TESTw: case t_TESTd:
        case t_SARb: case t_SARw: case t_SARd:
            if (emit) drc_iconst(0);
            return true;
        case t_INCb: case t_INCw: case t_INCd:
            if (emit) { DRC_LOADM(a_res); drc_iconst(sign); drc_b(0x46); }
            return true;
        case t_DECb: case t_DECw: case t_DECd:
            if (emit) { DRC_LOADM(a_res); drc_iconst(sign - 1); drc_b(0x46); }
            return true;
        case t_NEGb: case t_NEGw: case t_NEGd:
            if (emit) { DRC_LOADM(a_var1); drc_iconst(sign); drc_b(0x46); }
            return true;
        case t_SHLb: case t_SHLw: case t_SHLd:
            /* (res ^ v1) & sign */
            if (emit) {
                drc_iconst(a_res); drc_load32();
                drc_iconst(a_var1); drc_load32(); drc_b(0x73);
                drc_iconst(sign); drc_b(0x71);
                drc_iconst(0); drc_b(0x47);
            }
            return true;
        case t_UNKNOWN: case t_MUL:
            if (emit) DRC_FLAGBIT(0x800u);
            return true;
        default:
            return false;
        }
    }
    #undef DRC_LOADM
    #undef DRC_FLAGBIT
    return false;
}

static INLINE bool drc_flag_supported(int t, int which) { return drc_flag_emit(t, which, false); }
static INLINE void drc_push_flag(int t, int which) { (void)drc_flag_emit(t, which, true); }

/* diagnostic twin of drc_note_bail: which path each flag consumer took at
 * translate time (1/2/3 adc-sbb static/helper/dynamic, 4/5/6 inc-dec helper/
 * static/dynamic, 7/8/9 carry read, 10/11/12 zero read, 13/14/15 branch
 * condition, 16/17 flag-setting adc-sbb inline/helper). Read via
 * Module.__drcPath / the worker's stats message. */
EM_JS(void, drc_note_path, (int reason), {
    if (!Module.__drcPath) Module.__drcPath = {};
    Module.__drcPath[reason] = (Module.__drcPath[reason] || 0) + 1;
});

static uint8_t DRC_CALL_CONV dynrec_add_byte(uint8_t op1,uint8_t op2) DRC_FC;
static uint16_t DRC_CALL_CONV dynrec_add_word(uint16_t op1,uint16_t op2) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_add_dword(uint32_t op1,uint32_t op2) DRC_FC;
static uint8_t DRC_CALL_CONV dynrec_sub_byte(uint8_t op1,uint8_t op2) DRC_FC;
static uint16_t DRC_CALL_CONV dynrec_sub_word(uint16_t op1,uint16_t op2) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_sub_dword(uint32_t op1,uint32_t op2) DRC_FC;
static uint8_t DRC_CALL_CONV dynrec_and_byte(uint8_t op1,uint8_t op2) DRC_FC;
static uint16_t DRC_CALL_CONV dynrec_and_word(uint16_t op1,uint16_t op2) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_and_dword(uint32_t op1,uint32_t op2) DRC_FC;
static uint8_t DRC_CALL_CONV dynrec_or_byte(uint8_t op1,uint8_t op2) DRC_FC;
static uint16_t DRC_CALL_CONV dynrec_or_word(uint16_t op1,uint16_t op2) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_or_dword(uint32_t op1,uint32_t op2) DRC_FC;
static uint8_t DRC_CALL_CONV dynrec_xor_byte(uint8_t op1,uint8_t op2) DRC_FC;
static uint16_t DRC_CALL_CONV dynrec_xor_word(uint16_t op1,uint16_t op2) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_xor_dword(uint32_t op1,uint32_t op2) DRC_FC;
static void DRC_CALL_CONV dynrec_cmp_byte(uint8_t op1,uint8_t op2) DRC_FC;
static void DRC_CALL_CONV dynrec_cmp_word(uint16_t op1,uint16_t op2) DRC_FC;
static void DRC_CALL_CONV dynrec_cmp_dword(uint32_t op1,uint32_t op2) DRC_FC;
static void DRC_CALL_CONV dynrec_test_byte(uint8_t op1,uint8_t op2) DRC_FC;
static void DRC_CALL_CONV dynrec_test_word(uint16_t op1,uint16_t op2) DRC_FC;
static void DRC_CALL_CONV dynrec_test_dword(uint32_t op1,uint32_t op2) DRC_FC;

static uint8_t DRC_CALL_CONV dynrec_inc_byte(uint8_t op) DRC_FC;
static uint16_t DRC_CALL_CONV dynrec_inc_word(uint16_t op) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_inc_dword(uint32_t op) DRC_FC;
static uint8_t DRC_CALL_CONV dynrec_dec_byte(uint8_t op) DRC_FC;
static uint16_t DRC_CALL_CONV dynrec_dec_word(uint16_t op) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_dec_dword(uint32_t op) DRC_FC;
static uint8_t DRC_CALL_CONV dynrec_neg_byte(uint8_t op) DRC_FC;
static uint16_t DRC_CALL_CONV dynrec_neg_word(uint16_t op) DRC_FC;
static uint32_t DRC_CALL_CONV dynrec_neg_dword(uint32_t op) DRC_FC;

/* increment, decrement and negate. neg is three stores; inc and dec are the
 * same but must first materialise the carry flag into the flags word, because
 * they leave carry alone while replacing the lazy state that would have
 * produced it. The carry comes from the compare/subtract fast path, and if
 * the current flag type is not one of those the helper is called instead. */
/* Types where get_CF() just returns the flags word's carry bit, so the
 * LoadCF that inc and dec begin with writes back what is already there and
 * can be skipped. Everything else needs carry computed before the lazy state
 * is replaced. */
static const uint8_t drc_cf_is_noop[t_LASTFLAG] = {
    [t_UNKNOWN]=1, [t_INCb]=1, [t_INCw]=1, [t_INCd]=1,
    [t_DECb]=1, [t_DECw]=1, [t_DECd]=1, [t_MUL]=1,
};

struct DrcUnOp { uint32_t fn; uint32_t mask; int kind; int type; };  /* 0 inc, 1 dec, 2 neg */
static const DrcUnOp drc_un_ops[] = {
  { (uint32_t)(uintptr_t)dynrec_inc_byte, 0xFFu, 0, t_INCb },
  { (uint32_t)(uintptr_t)dynrec_inc_word, 0xFFFFu, 0, t_INCw },
  { (uint32_t)(uintptr_t)dynrec_inc_dword, 0xFFFFFFFFu, 0, t_INCd },
  { (uint32_t)(uintptr_t)dynrec_dec_byte, 0xFFu, 1, t_DECb },
  { (uint32_t)(uintptr_t)dynrec_dec_word, 0xFFFFu, 1, t_DECw },
  { (uint32_t)(uintptr_t)dynrec_dec_dword, 0xFFFFFFFFu, 1, t_DECd },
  { (uint32_t)(uintptr_t)dynrec_neg_byte, 0xFFu, 2, t_NEGb },
  { (uint32_t)(uintptr_t)dynrec_neg_word, 0xFFFFu, 2, t_NEGw },
  { (uint32_t)(uintptr_t)dynrec_neg_dword, 0xFFFFFFFFu, 2, t_NEGd },
};

static bool drc_emit_unop_inline(uint32_t target, unsigned import_slot) {
    const DrcUnOp* f = NULL;
    for (unsigned i = 0; i < sizeof(drc_un_ops)/sizeof(drc_un_ops[0]); i++)
        if (drc_un_ops[i].fn == target) { f = &drc_un_ops[i]; break; }
    if (f == NULL) return false;

    const bool dynamic = (drc_flag_type < 0);
    if (!dynamic && f->kind != 2 && !drc_cf_is_noop[drc_flag_type]
        && !drc_flag_supported(drc_flag_type, DRC_F_CF)) {
        /* carry not statically computable: the helper does the whole thing
         * (and sets the type itself, so tracking survives) */
        drc_note_path(4);
        drc_getl(FC_OP1);
        drc_b(0x10); drc_uleb(import_slot);
        drc_setl(FC_RETOP);
        drc_flag_type = f->type;
        return true;
    }

    drc_used_extra_locals = true;
    const uint32_t a_var1 = (uint32_t)(uintptr_t)&lf_var1d;
    const uint32_t a_res  = (uint32_t)(uintptr_t)&lf_resd;
    const uint32_t a_type = (uint32_t)(uintptr_t)&lflags.type;
    const uint32_t a_flag = (uint32_t)(uintptr_t)DRCD_REG_FLAGS;
    const uint32_t a_var2 = (uint32_t)(uintptr_t)&lf_var2d;
    const uint32_t a_mask = (uint32_t)(uintptr_t)&drc_subcmp_mask[0];
    const bool narrow = (f->mask != 0xFFFFFFFFu);

    if (!dynamic) drc_note_path(5); else drc_note_path(6);
    if (!dynamic && f->kind != 2 && !drc_cf_is_noop[drc_flag_type]) {
        /* reg_flags = (reg_flags & ~1) | CF, with CF known statically */
        drc_iconst(a_flag);
        drc_iconst(a_flag); drc_load32(); drc_iconst(0xFFFFFFFEu); drc_b(0x71);
        drc_push_flag(drc_flag_type, DRC_F_CF);
        drc_b(0x72);
        drc_store32();
    }

    if (dynamic && f->kind != 2) {
        const uint32_t a_noop = (uint32_t)(uintptr_t)&drc_cf_is_noop[0];
        /* mask != 0 -> carry is computable here; noop -> carry needs no work */
        drc_iconst(a_type); drc_load32(); drc_setl(DRC_LOCAL_V2);
        drc_getl(DRC_LOCAL_V2); drc_iconst(2); drc_b(0x74);
        drc_iconst(a_mask); drc_b(0x6A); drc_load32();
        drc_teel(DRC_LOCAL_SCRATCH);
        drc_b(0x04); drc_b(0x40);                  /* if (compare/subtract) */
        /* reg_flags = (reg_flags & ~1) | (var1 < var2) */
        drc_iconst(a_flag);
        drc_iconst(a_flag); drc_load32(); drc_iconst(0xFFFFFFFEu); drc_b(0x71);
        drc_iconst(a_var1); drc_load32(); drc_getl(DRC_LOCAL_SCRATCH); drc_b(0x71);
        drc_iconst(a_var2); drc_load32(); drc_getl(DRC_LOCAL_SCRATCH); drc_b(0x71);
        drc_b(0x49);
        drc_b(0x72);
        drc_store32();
        drc_b(0x0B);                               /* end if */
        /* inline when carry was computed, or when it needed nothing */
        drc_getl(DRC_LOCAL_SCRATCH); drc_iconst(0); drc_b(0x47);   /* mask != 0 */
        drc_getl(DRC_LOCAL_V2); drc_iconst(a_noop); drc_b(0x6A); drc_load8u();
        drc_b(0x72);                                               /* or */
        drc_b(0x04); drc_b(0x40);
    }

    /* v1 = op & mask; res = (v1 +/- 1 or 0 - v1) & mask */
    drc_getl(FC_OP1); if (narrow) { drc_iconst(f->mask); drc_b(0x71); } drc_setl(DRC_LOCAL_V1);
    if (f->kind == 2) { drc_iconst(0); drc_getl(DRC_LOCAL_V1); drc_b(0x6B); }
    else { drc_getl(DRC_LOCAL_V1); drc_iconst(1); drc_b(f->kind == 0 ? 0x6A : 0x6B); }
    if (narrow) { drc_iconst(f->mask); drc_b(0x71); }
    drc_setl(DRC_LOCAL_RES);

    drc_iconst(a_var1); drc_getl(DRC_LOCAL_V1);
    if (f->mask == 0xFFu) drc_store8(); else if (f->mask == 0xFFFFu) drc_store16(); else drc_store32();
    drc_iconst(a_res); drc_getl(DRC_LOCAL_RES);
    if (f->mask == 0xFFu) drc_store8(); else if (f->mask == 0xFFFFu) drc_store16(); else drc_store32();
    drc_iconst(a_type); drc_iconst((uint32_t)f->type); drc_store32();
    drc_getl(DRC_LOCAL_RES); drc_setl(FC_RETOP);

    if (dynamic && f->kind != 2) {
        drc_b(0x05);                               /* else: let the helper do it */
        drc_getl(FC_OP1);
        drc_b(0x10); drc_uleb(import_slot);
        drc_setl(FC_RETOP);
        drc_b(0x0B);
    }
    drc_flag_type = f->type;
    return true;
}

/* The flag-setting arithmetic helpers all have one shape:
 *     lf_var1 = op1;  lf_var2 = op2;
 *     lf_res  = op1 <operator> op2;  lflags.type = <type>;
 * (returning the result, except compare and test). That is four stores, so
 * calling out for it costs more than doing it. Operands are masked to the
 * operand width first because the C reads the truncated values back out of
 * the union before computing. */
struct DrcFlagOp { uint32_t fn; uint32_t mask; uint8_t wasmop; int type; bool hasret; };
static const DrcFlagOp drc_flag_ops[] = {
  { (uint32_t)(uintptr_t)dynrec_add_byte, 0xFFu, 0x6A, t_ADDb, true },
  { (uint32_t)(uintptr_t)dynrec_add_word, 0xFFFFu, 0x6A, t_ADDw, true },
  { (uint32_t)(uintptr_t)dynrec_add_dword, 0xFFFFFFFFu, 0x6A, t_ADDd, true },
  { (uint32_t)(uintptr_t)dynrec_sub_byte, 0xFFu, 0x6B, t_SUBb, true },
  { (uint32_t)(uintptr_t)dynrec_sub_word, 0xFFFFu, 0x6B, t_SUBw, true },
  { (uint32_t)(uintptr_t)dynrec_sub_dword, 0xFFFFFFFFu, 0x6B, t_SUBd, true },
  { (uint32_t)(uintptr_t)dynrec_and_byte, 0xFFu, 0x71, t_ANDb, true },
  { (uint32_t)(uintptr_t)dynrec_and_word, 0xFFFFu, 0x71, t_ANDw, true },
  { (uint32_t)(uintptr_t)dynrec_and_dword, 0xFFFFFFFFu, 0x71, t_ANDd, true },
  { (uint32_t)(uintptr_t)dynrec_or_byte, 0xFFu, 0x72, t_ORb, true },
  { (uint32_t)(uintptr_t)dynrec_or_word, 0xFFFFu, 0x72, t_ORw, true },
  { (uint32_t)(uintptr_t)dynrec_or_dword, 0xFFFFFFFFu, 0x72, t_ORd, true },
  { (uint32_t)(uintptr_t)dynrec_xor_byte, 0xFFu, 0x73, t_XORb, true },
  { (uint32_t)(uintptr_t)dynrec_xor_word, 0xFFFFu, 0x73, t_XORw, true },
  { (uint32_t)(uintptr_t)dynrec_xor_dword, 0xFFFFFFFFu, 0x73, t_XORd, true },
  { (uint32_t)(uintptr_t)dynrec_cmp_byte, 0xFFu, 0x6B, t_CMPb, false },
  { (uint32_t)(uintptr_t)dynrec_cmp_word, 0xFFFFu, 0x6B, t_CMPw, false },
  { (uint32_t)(uintptr_t)dynrec_cmp_dword, 0xFFFFFFFFu, 0x6B, t_CMPd, false },
  { (uint32_t)(uintptr_t)dynrec_test_byte, 0xFFu, 0x71, t_TESTb, false },
  { (uint32_t)(uintptr_t)dynrec_test_word, 0xFFFFu, 0x71, t_TESTw, false },
  { (uint32_t)(uintptr_t)dynrec_test_dword, 0xFFFFFFFFu, 0x71, t_TESTd, false },
};

static bool drc_emit_flagop_inline(uint32_t target, unsigned import_slot) {
    const DrcFlagOp* f = NULL;
    for (unsigned i = 0; i < sizeof(drc_flag_ops)/sizeof(drc_flag_ops[0]); i++)
        if (drc_flag_ops[i].fn == target) { f = &drc_flag_ops[i]; break; }
    if (f == NULL) return false;
    (void)import_slot;   /* fully inlined, the helper is never needed */

    drc_used_extra_locals = true;
    const uint32_t a_var1 = (uint32_t)(uintptr_t)&lf_var1d;
    const uint32_t a_var2 = (uint32_t)(uintptr_t)&lf_var2d;
    const uint32_t a_res  = (uint32_t)(uintptr_t)&lf_resd;
    const uint32_t a_type = (uint32_t)(uintptr_t)&lflags.type;
    const bool narrow = (f->mask != 0xFFFFFFFFu);

    /* v1 = op1 & mask, v2 = op2 & mask */
    drc_getl(FC_OP1); if (narrow) { drc_iconst(f->mask); drc_b(0x71); } drc_setl(DRC_LOCAL_V1);
    drc_getl(FC_OP2); if (narrow) { drc_iconst(f->mask); drc_b(0x71); } drc_setl(DRC_LOCAL_V2);
    /* res = (v1 op v2) & mask */
    drc_getl(DRC_LOCAL_V1); drc_getl(DRC_LOCAL_V2); drc_b(f->wasmop);
    if (narrow) { drc_iconst(f->mask); drc_b(0x71); }
    drc_setl(DRC_LOCAL_RES);

    drc_iconst(a_var1); drc_getl(DRC_LOCAL_V1);
    if (f->mask == 0xFFu) drc_store8(); else if (f->mask == 0xFFFFu) drc_store16(); else drc_store32();
    drc_iconst(a_var2); drc_getl(DRC_LOCAL_V2);
    if (f->mask == 0xFFu) drc_store8(); else if (f->mask == 0xFFFFu) drc_store16(); else drc_store32();
    drc_iconst(a_res);  drc_getl(DRC_LOCAL_RES);
    if (f->mask == 0xFFu) drc_store8(); else if (f->mask == 0xFFFFu) drc_store16(); else drc_store32();
    drc_iconst(a_type); drc_iconst((uint32_t)f->type); drc_store32();

    if (f->hasret) { drc_getl(DRC_LOCAL_RES); drc_setl(FC_RETOP); }
    drc_flag_type = f->type;
    return true;
}

/* Inline add-with-carry and subtract-with-borrow. These are only a sum plus
 * the carry flag - "op1 + op2 + CF" and "op1 - (op2 + CF)" - and they were the
 * single largest consumer of get_CF in a game profile. The carry comes from
 * the same compare/subtract fast path used by the branches; any other flag
 * type calls the helper, which computes it the long way. */
static bool drc_emit_adcsbb_inline(uint32_t target, unsigned import_slot) {
    uint32_t opmask; bool is_sbb; int full_type = -1;
    if      (target == (uint32_t)(uintptr_t)dynrec_adc_byte_simple)  { opmask = 0xFFu;       is_sbb = false; }
    else if (target == (uint32_t)(uintptr_t)dynrec_adc_word_simple)  { opmask = 0xFFFFu;     is_sbb = false; }
    else if (target == (uint32_t)(uintptr_t)dynrec_adc_dword_simple) { opmask = 0xFFFFFFFFu; is_sbb = false; }
    else if (target == (uint32_t)(uintptr_t)dynrec_sbb_byte_simple)  { opmask = 0xFFu;       is_sbb = true;  }
    else if (target == (uint32_t)(uintptr_t)dynrec_sbb_word_simple)  { opmask = 0xFFFFu;     is_sbb = true;  }
    else if (target == (uint32_t)(uintptr_t)dynrec_sbb_dword_simple) { opmask = 0xFFFFFFFFu; is_sbb = true;  }
    else if (target == (uint32_t)(uintptr_t)dynrec_adc_byte)  { opmask = 0xFFu;       is_sbb = false; full_type = t_ADCb; }
    else if (target == (uint32_t)(uintptr_t)dynrec_adc_word)  { opmask = 0xFFFFu;     is_sbb = false; full_type = t_ADCw; }
    else if (target == (uint32_t)(uintptr_t)dynrec_adc_dword) { opmask = 0xFFFFFFFFu; is_sbb = false; full_type = t_ADCd; }
    else if (target == (uint32_t)(uintptr_t)dynrec_sbb_byte)  { opmask = 0xFFu;       is_sbb = true;  full_type = t_SBBb; }
    else if (target == (uint32_t)(uintptr_t)dynrec_sbb_word)  { opmask = 0xFFFFu;     is_sbb = true;  full_type = t_SBBw; }
    else if (target == (uint32_t)(uintptr_t)dynrec_sbb_dword) { opmask = 0xFFFFFFFFu; is_sbb = true;  full_type = t_SBBd; }
    else return false;

    /* full variants: the adc/sbb's own flags are live afterwards, so the
     * helper stores the whole lazy state. With the incoming flag type known
     * that is only a handful of stores: carry (computed statically before
     * the state it reads is overwritten), both operands, the sum, the type.
     * Either way the resulting flag type is known - the helper sets it too -
     * so tracking continues through adc chains. */
    if (full_type >= 0) {
        drc_used_extra_locals = true;
        const uint32_t a_oldcf = (uint32_t)(uintptr_t)&lflags.oldcf;
        const uint32_t a_type  = (uint32_t)(uintptr_t)&lflags.type;
        const uint32_t a_var1  = (uint32_t)(uintptr_t)&lf_var1d;
        const uint32_t a_var2  = (uint32_t)(uintptr_t)&lf_var2d;
        const uint32_t a_res   = (uint32_t)(uintptr_t)&lf_resd;
        if (drc_flag_type >= 0 && drc_flag_supported(drc_flag_type, DRC_F_CF)) {
            drc_note_path(16);
            drc_push_flag(drc_flag_type, DRC_F_CF); drc_setl(DRC_LOCAL_SCRATCH);
            drc_iconst(a_oldcf); drc_getl(DRC_LOCAL_SCRATCH); drc_store8();
            drc_iconst(a_var1); drc_getl(FC_OP1);
            if (opmask == 0xFFu) drc_store8(); else if (opmask == 0xFFFFu) drc_store16(); else drc_store32();
            drc_iconst(a_var2); drc_getl(FC_OP2);
            if (opmask == 0xFFu) drc_store8(); else if (opmask == 0xFFFFu) drc_store16(); else drc_store32();
            if (is_sbb) {   /* op1 - (op2 + carry) */
                drc_getl(FC_OP1);
                drc_getl(FC_OP2); drc_getl(DRC_LOCAL_SCRATCH); drc_b(0x6A);
                drc_b(0x6B);
            } else {        /* op1 + op2 + carry */
                drc_getl(FC_OP1); drc_getl(FC_OP2); drc_b(0x6A);
                drc_getl(DRC_LOCAL_SCRATCH); drc_b(0x6A);
            }
            if (opmask != 0xFFFFFFFFu) { drc_iconst(opmask); drc_b(0x71); }
            drc_setl(FC_RETOP);
            drc_iconst(a_res); drc_getl(FC_RETOP);
            if (opmask == 0xFFu) drc_store8(); else if (opmask == 0xFFFFu) drc_store16(); else drc_store32();
            drc_iconst(a_type); drc_iconst((uint32_t)full_type); drc_store32();
        } else {
            drc_note_path(17);
            drc_getl(FC_OP1); drc_getl(FC_OP2);
            drc_b(0x10); drc_uleb(import_slot);
            drc_setl(FC_RETOP);
        }
        drc_flag_type = full_type;
        return true;
    }

    /* the _simple helpers only read the lazy state, so the tracked type
     * survives either path */
    if (drc_flag_type >= 0) {
        if (drc_flag_supported(drc_flag_type, DRC_F_CF)) {
            drc_note_path(1);
            if (is_sbb) {   /* op1 - (op2 + carry) */
                drc_getl(FC_OP1);
                drc_getl(FC_OP2); drc_push_flag(drc_flag_type, DRC_F_CF); drc_b(0x6A);
                drc_b(0x6B);
            } else {        /* op1 + op2 + carry */
                drc_getl(FC_OP1); drc_getl(FC_OP2); drc_b(0x6A);
                drc_push_flag(drc_flag_type, DRC_F_CF); drc_b(0x6A);
            }
            if (opmask != 0xFFFFFFFFu) { drc_iconst(opmask); drc_b(0x71); }
        } else {
            drc_note_path(2);
            drc_getl(FC_OP1); drc_getl(FC_OP2);
            drc_b(0x10); drc_uleb(import_slot);
        }
        drc_setl(FC_RETOP);
        return true;
    }
    drc_note_path(3);

    drc_used_extra_locals = true;
    const uint32_t a_type = (uint32_t)(uintptr_t)&lflags.type;
    const uint32_t a_var1 = (uint32_t)(uintptr_t)&lf_var1d;
    const uint32_t a_var2 = (uint32_t)(uintptr_t)&lf_var2d;
    const uint32_t a_mask = (uint32_t)(uintptr_t)&drc_subcmp_mask[0];

    drc_iconst(a_type); drc_load32();
    drc_iconst(2); drc_b(0x74);
    drc_iconst(a_mask); drc_b(0x6A);
    drc_load32();
    drc_teel(DRC_LOCAL_SCRATCH);
    drc_b(0x04); drc_b(0x40);                       /* if (flag type is a compare/subtract) */

    /* carry = (var1 & m) < (var2 & m) */
    drc_iconst(a_var1); drc_load32(); drc_getl(DRC_LOCAL_SCRATCH); drc_b(0x71);
    drc_iconst(a_var2); drc_load32(); drc_getl(DRC_LOCAL_SCRATCH); drc_b(0x71);
    drc_b(0x49);
    drc_setl(DRC_LOCAL_V1);                          /* V1 holds the carry */

    if (is_sbb) {                                    /* op1 - (op2 + carry) */
        drc_getl(FC_OP1);
        drc_getl(FC_OP2); drc_getl(DRC_LOCAL_V1); drc_b(0x6A);
        drc_b(0x6B);
    } else {                                         /* op1 + op2 + carry */
        drc_getl(FC_OP1); drc_getl(FC_OP2); drc_b(0x6A);
        drc_getl(DRC_LOCAL_V1); drc_b(0x6A);
    }
    if (opmask != 0xFFFFFFFFu) { drc_iconst(opmask); drc_b(0x71); }
    drc_setl(FC_RETOP);

    drc_b(0x05);                                     /* else: call the helper */
    drc_getl(FC_OP1); drc_getl(FC_OP2);
    drc_b(0x10); drc_uleb(import_slot);
    drc_setl(FC_RETOP);
    drc_b(0x0B);
    return true;
}

/* Inline the branch conditions that need the sign and overflow flags - the
 * signed compares (jl/jge/jle/jg) and the unsigned pair (jbe/ja). After a
 * compare or subtract, which is what a branch is testing, flags.cpp computes
 * all four flags from the two operands and the result:
 *     ZF = res == 0                SF = res & sign
 *     CF = var1 < var2 (unsigned)  OF = ((v1^v2) & (v1^res)) & sign
 * so the whole condition folds into a few instructions. Any other flag type
 * falls back to the helper. */
enum DrcCond { DRC_C_SF, DRC_C_NSF, DRC_C_CF_OR_ZF, DRC_C_NCF_AND_NZF,
               DRC_C_SF_EQ_OF, DRC_C_SF_NEQ_OF, DRC_C_ZF_OR_SF_NEQ_OF,
               DRC_C_NZF_AND_SF_EQ_OF, DRC_C_NONE };

static bool drc_emit_cond_inline(uint32_t target, unsigned import_slot) {
    DrcCond cond;
    if      (target == (uint32_t)(uintptr_t)dynrec_get_sf)                cond = DRC_C_SF;
    else if (target == (uint32_t)(uintptr_t)dynrec_get_nsf)               cond = DRC_C_NSF;
    else if (target == (uint32_t)(uintptr_t)dynrec_get_cf_or_zf)          cond = DRC_C_CF_OR_ZF;
    else if (target == (uint32_t)(uintptr_t)dynrec_get_ncf_and_nzf)       cond = DRC_C_NCF_AND_NZF;
    else if (target == (uint32_t)(uintptr_t)dynrec_get_sf_eq_of)          cond = DRC_C_SF_EQ_OF;
    else if (target == (uint32_t)(uintptr_t)dynrec_get_sf_neq_of)         cond = DRC_C_SF_NEQ_OF;
    else if (target == (uint32_t)(uintptr_t)dynrec_get_zf_or_sf_neq_of)   cond = DRC_C_ZF_OR_SF_NEQ_OF;
    else if (target == (uint32_t)(uintptr_t)dynrec_get_nzf_and_sf_eq_of)  cond = DRC_C_NZF_AND_SF_EQ_OF;
    else return false;

    if (drc_flag_type >= 0) {
        const int K = drc_flag_type;
        bool ok;
        switch (cond) {
        case DRC_C_SF: case DRC_C_NSF:
            ok = drc_flag_supported(K, DRC_F_SF); break;
        case DRC_C_CF_OR_ZF: case DRC_C_NCF_AND_NZF:
            ok = drc_flag_supported(K, DRC_F_CF) && drc_flag_supported(K, DRC_F_ZF); break;
        case DRC_C_SF_EQ_OF: case DRC_C_SF_NEQ_OF:
            ok = drc_flag_supported(K, DRC_F_SF) && drc_flag_supported(K, DRC_F_OF); break;
        default:
            ok = drc_flag_supported(K, DRC_F_ZF) && drc_flag_supported(K, DRC_F_SF)
              && drc_flag_supported(K, DRC_F_OF); break;
        }
        if (ok) {
            drc_note_path(13);
            switch (cond) {
            case DRC_C_SF:  drc_push_flag(K, DRC_F_SF); break;
            case DRC_C_NSF: drc_push_flag(K, DRC_F_SF); drc_b(0x45); break;
            case DRC_C_CF_OR_ZF:
                drc_push_flag(K, DRC_F_CF); drc_push_flag(K, DRC_F_ZF); drc_b(0x72); break;
            case DRC_C_NCF_AND_NZF:
                drc_push_flag(K, DRC_F_CF); drc_push_flag(K, DRC_F_ZF); drc_b(0x72); drc_b(0x45); break;
            case DRC_C_SF_EQ_OF:
                drc_push_flag(K, DRC_F_SF); drc_push_flag(K, DRC_F_OF); drc_b(0x46); break;
            case DRC_C_SF_NEQ_OF:
                drc_push_flag(K, DRC_F_SF); drc_push_flag(K, DRC_F_OF); drc_b(0x47); break;
            case DRC_C_ZF_OR_SF_NEQ_OF:
                drc_push_flag(K, DRC_F_ZF);
                drc_push_flag(K, DRC_F_SF); drc_push_flag(K, DRC_F_OF); drc_b(0x47);
                drc_b(0x72); break;
            case DRC_C_NZF_AND_SF_EQ_OF:
                drc_push_flag(K, DRC_F_ZF);
                drc_push_flag(K, DRC_F_SF); drc_push_flag(K, DRC_F_OF); drc_b(0x47);
                drc_b(0x72); drc_b(0x45); break;
            default: break;
            }
        } else {
            drc_note_path(14);
            drc_b(0x10); drc_uleb(import_slot);
        }
        drc_setl(FC_RETOP);
        return true;
    }
    drc_note_path(15);

    drc_used_extra_locals = true;
    const uint32_t a_type = (uint32_t)(uintptr_t)&lflags.type;
    const uint32_t a_var1 = (uint32_t)(uintptr_t)&lf_var1d;
    const uint32_t a_var2 = (uint32_t)(uintptr_t)&lf_var2d;
    const uint32_t a_res  = (uint32_t)(uintptr_t)&lf_resd;
    const uint32_t a_mask = (uint32_t)(uintptr_t)&drc_subcmp_mask[0];

    /* mask = drc_subcmp_mask[lflags.type] */
    drc_iconst(a_type); drc_load32();
    drc_iconst(2); drc_b(0x74);
    drc_iconst(a_mask); drc_b(0x6A);
    drc_load32();
    drc_teel(DRC_LOCAL_SCRATCH);
    drc_b(0x04); drc_b(0x40);                       /* if (mask != 0) */

    /* sign = (mask >>> 1) + 1 */
    drc_getl(DRC_LOCAL_SCRATCH); drc_iconst(1); drc_b(0x76);
    drc_iconst(1); drc_b(0x6A); drc_setl(DRC_LOCAL_SIGN);
    /* operands, masked to width */
    drc_iconst(a_var1); drc_load32(); drc_getl(DRC_LOCAL_SCRATCH); drc_b(0x71); drc_setl(DRC_LOCAL_V1);
    drc_iconst(a_var2); drc_load32(); drc_getl(DRC_LOCAL_SCRATCH); drc_b(0x71); drc_setl(DRC_LOCAL_V2);
    drc_iconst(a_res);  drc_load32(); drc_getl(DRC_LOCAL_SCRATCH); drc_b(0x71); drc_setl(DRC_LOCAL_RES);

    /* the pieces each condition may need */
    #define DRC_PUSH_ZF() do { drc_getl(DRC_LOCAL_RES); drc_b(0x45); } while (0)          /* res == 0 */
    #define DRC_PUSH_SF() do { drc_getl(DRC_LOCAL_RES); drc_getl(DRC_LOCAL_SIGN); \
                               drc_b(0x71); drc_iconst(0); drc_b(0x47); } while (0)       /* (res & sign) != 0 */
    #define DRC_PUSH_CF() do { drc_getl(DRC_LOCAL_V1); drc_getl(DRC_LOCAL_V2); drc_b(0x49); } while (0)
    #define DRC_PUSH_OF() do { drc_getl(DRC_LOCAL_V1); drc_getl(DRC_LOCAL_V2); drc_b(0x73); \
                               drc_getl(DRC_LOCAL_V1); drc_getl(DRC_LOCAL_RES); drc_b(0x73); \
                               drc_b(0x71); drc_getl(DRC_LOCAL_SIGN); drc_b(0x71); \
                               drc_iconst(0); drc_b(0x47); } while (0)

    switch (cond) {
        case DRC_C_SF:  DRC_PUSH_SF(); break;
        case DRC_C_NSF: DRC_PUSH_SF(); drc_b(0x45); break;
        case DRC_C_CF_OR_ZF:      DRC_PUSH_CF(); DRC_PUSH_ZF(); drc_b(0x72); break;
        case DRC_C_NCF_AND_NZF:   DRC_PUSH_CF(); DRC_PUSH_ZF(); drc_b(0x72); drc_b(0x45); break;
        case DRC_C_SF_EQ_OF:      DRC_PUSH_SF(); DRC_PUSH_OF(); drc_b(0x46); break;
        case DRC_C_SF_NEQ_OF:     DRC_PUSH_SF(); DRC_PUSH_OF(); drc_b(0x47); break;
        case DRC_C_ZF_OR_SF_NEQ_OF:
            DRC_PUSH_ZF(); DRC_PUSH_SF(); DRC_PUSH_OF(); drc_b(0x47); drc_b(0x72); break;
        case DRC_C_NZF_AND_SF_EQ_OF:
            DRC_PUSH_ZF(); DRC_PUSH_SF(); DRC_PUSH_OF(); drc_b(0x47); drc_b(0x72); drc_b(0x45); break;
        default: break;
    }
    #undef DRC_PUSH_ZF
    #undef DRC_PUSH_SF
    #undef DRC_PUSH_CF
    #undef DRC_PUSH_OF
    drc_setl(FC_RETOP);
    drc_b(0x05);                                    /* else: call the helper */
    drc_b(0x10); drc_uleb(import_slot);
    drc_setl(FC_RETOP);
    drc_b(0x0B);
    return true;
}

/* Inline the carry-flag read. For a compare or subtract - what a branch is
 * almost always testing - flags.cpp computes carry as an unsigned comparison
 * of the two operands, so that is what we emit; every other flag type still
 * calls the helper. */
static bool drc_emit_cf_inline(uint32_t target, unsigned import_slot) {
    bool negate;
    if      (target == (uint32_t)(uintptr_t)dynrec_get_cf)  negate = false;
    else if (target == (uint32_t)(uintptr_t)dynrec_get_ncf) negate = true;
    else return false;

    if (drc_flag_type >= 0) {
        if (drc_flag_supported(drc_flag_type, DRC_F_CF)) {
            drc_note_path(7);
            drc_push_flag(drc_flag_type, DRC_F_CF);
            if (negate) drc_b(0x45);
        } else {
            drc_note_path(8);
            drc_b(0x10); drc_uleb(import_slot);
        }
        drc_setl(FC_RETOP);
        return true;
    }
    drc_note_path(9);

    const uint32_t a_type = (uint32_t)(uintptr_t)&lflags.type;
    const uint32_t a_var1 = (uint32_t)(uintptr_t)&lf_var1d;
    const uint32_t a_var2 = (uint32_t)(uintptr_t)&lf_var2d;
    struct { int type; uint32_t mask; } const cases[] = {
        { t_CMPd, 0xFFFFFFFFu }, { t_SUBd, 0xFFFFFFFFu },
        { t_CMPw, 0x0000FFFFu }, { t_SUBw, 0x0000FFFFu },
        { t_CMPb, 0x000000FFu }, { t_SUBb, 0x000000FFu },
    };
    drc_iconst(a_type); drc_load32(); drc_setl(DRC_LOCAL_SCRATCH);
    for (unsigned i = 0; i < 6; i++) {
        drc_getl(DRC_LOCAL_SCRATCH);
        drc_iconst((uint32_t)cases[i].type); drc_b(0x46);
        drc_b(0x04); drc_b(0x7F);
        drc_iconst(a_var1); drc_load32();
        if (cases[i].mask != 0xFFFFFFFFu) { drc_iconst(cases[i].mask); drc_b(0x71); }
        drc_iconst(a_var2); drc_load32();
        if (cases[i].mask != 0xFFFFFFFFu) { drc_iconst(cases[i].mask); drc_b(0x71); }
        drc_b(0x49);
        if (negate) drc_b(0x45);
        drc_b(0x05);
    }
    drc_b(0x10); drc_uleb(import_slot);
    for (unsigned i = 0; i < 6; i++) drc_b(0x0B);
    drc_setl(FC_RETOP);
    return true;
}

/* Inline the zero-flag read the conditional branches use: get_ZF() is just
 * "the result was zero" at the operand width for every type in drc_zf_mask.
 * A zero entry (t_UNKNOWN, rotates, mul/div) falls back to the helper. */
static bool drc_emit_zf_inline(uint32_t target, unsigned import_slot) {
    bool negate;
    if      (target == (uint32_t)(uintptr_t)dynrec_get_zf)  negate = false;
    else if (target == (uint32_t)(uintptr_t)dynrec_get_nzf) negate = true;
    else return false;

    if (drc_flag_type >= 0) {
        if (drc_flag_supported(drc_flag_type, DRC_F_ZF)) {
            drc_note_path(10);
            drc_push_flag(drc_flag_type, DRC_F_ZF);
            if (negate) drc_b(0x45);
        } else {
            drc_note_path(11);
            drc_b(0x10); drc_uleb(import_slot);
        }
        drc_setl(FC_RETOP);
        return true;
    }
    drc_note_path(12);

    const uint32_t a_type = (uint32_t)(uintptr_t)&lflags.type;
    const uint32_t a_res  = (uint32_t)(uintptr_t)&lf_resd;
    const uint32_t a_mask = (uint32_t)(uintptr_t)&drc_zf_mask[0];

    drc_iconst(a_type); drc_load32();
    drc_iconst(2); drc_b(0x74);
    drc_iconst(a_mask); drc_b(0x6A);
    drc_load32();
    drc_teel(DRC_LOCAL_SCRATCH);
    drc_b(0x04); drc_b(0x40);
    drc_iconst(a_res); drc_load32();
    drc_getl(DRC_LOCAL_SCRATCH); drc_b(0x71);
    drc_b(0x45);
    if (negate) drc_b(0x45);
    drc_setl(FC_RETOP);
    drc_b(0x05);
    drc_b(0x10); drc_uleb(import_slot);
    drc_setl(FC_RETOP);
    drc_b(0x0B);
    return true;
}

/* translate the IR in [start,end) and hand it to the JS compiler.
 * returns the tagged xstart value (DRC_XSTART_TAG on failure keeps idx 0). */
static uint32_t drc_translate_and_compile(uint8_t* start, uint8_t* end) {
    const DRCIRRec* first = (const DRCIRRec*)start;
    const DRCIRRec* last  = (const DRCIRRec*)end;

    static int drc_blocks_done = 0;
    static int drc_limit = -1;
    if (drc_limit < 0) drc_limit = drc_get_limit();
    if (drc_blocks_done++ >= drc_limit) return DRC_XSTART_TAG; /* stay interpreted */

    /* collect forward branch targets */
    uint32_t targets[512]; unsigned ntargets = 0;
    for (const DRCIRRec* r = first; r < last; r++) {
        if (r->op != DRCIR_BR) continue;
        if (r->x == 0 || r->x <= (uint32_t)(uintptr_t)r || r->x > (uint32_t)(uintptr_t)end)
            { drc_note_bail(1); return DRC_XSTART_TAG; }   /* unfilled or non-forward */
        bool seen = false;
        for (unsigned i = 0; i < ntargets; i++) if (targets[i] == r->x) { seen = true; break; }
        if (!seen) {
            if (ntargets >= 512) { drc_note_bail(2); return DRC_XSTART_TAG; }
            targets[ntargets++] = r->x;
        }
    }
    /* sort ascending */
    for (unsigned i = 1; i < ntargets; i++)
        for (unsigned j = i; j && targets[j-1] > targets[j]; j--) {
            uint32_t t = targets[j]; targets[j] = targets[j-1]; targets[j-1] = t;
        }

    /* Helpers are imported so the generated code can use a direct call
     * instead of call_indirect: no runtime type check, no table bounds check,
     * and the callee is a known function to the engine. */
    uint32_t calls[128]; uint8_t call_nargs[128], call_hasret[128];
    unsigned ncalls = 0;
    for (const DRCIRRec* r = first; r < last; r++) {
        if (r->op != DRCIR_CALL) continue;
        if (r->b > 4 || r->x == 0) { drc_note_bail(5); return DRC_XSTART_TAG; }
        bool seen = false;
        for (unsigned i = 0; i < ncalls; i++) if (calls[i] == r->x) { seen = true; break; }
        if (seen) continue;
        if (ncalls >= 128) { drc_note_bail(6); return DRC_XSTART_TAG; }
        calls[ncalls] = r->x; call_nargs[ncalls] = r->b; call_hasret[ncalls] = r->c;
        ncalls++;
    }

    drc_buf.clear();

    /* ---- module header ---- */
    static const uint8_t hdr[8] = {0x00,0x61,0x73,0x6D,0x01,0x00,0x00,0x00};
    drc_buf.insert(drc_buf.end(), hdr, hdr + 8);

    /* type section: type 0 = ()->i32 (block signature); 1..5 = (i32 x 0..4)->i32;
     * 6..10 = (i32 x 0..4)->void. Type 1 duplicates type 0, which is legal and
     * keeps the index arithmetic in drc_calltype() trivial. */
    {
        std::vector<uint8_t> s;
        s.push_back(11);                        /* count */
        for (int variant = 0; variant < 11; variant++) {
            unsigned nargs, hasret;
            if (variant == 0) { nargs = 0; hasret = 1; }
            else if (variant <= 5) { nargs = (unsigned)variant - 1; hasret = 1; }
            else { nargs = (unsigned)variant - 6; hasret = 0; }
            s.push_back(0x60);
            s.push_back((uint8_t)nargs);
            for (unsigned i = 0; i < nargs; i++) s.push_back(0x7F);
            s.push_back((uint8_t)hasret);
            if (hasret) s.push_back(0x7F);
        }
        drc_b(0x01); drc_uleb((uint32_t)s.size());
        drc_buf.insert(drc_buf.end(), s.begin(), s.end());
    }

    /* import section: table + memory */
    {
        std::vector<uint8_t> s;
        s.push_back(2);     /* patched below once helper imports are counted */
        /* "e" "t" table funcref min 0 */
        s.push_back(1); s.push_back('e'); s.push_back(1); s.push_back('t');
        s.push_back(0x01); s.push_back(0x70); s.push_back(0x00); s.push_back(0x00);
        /* "e" "m" memory min 0 */
        s.push_back(1); s.push_back('e'); s.push_back(1); s.push_back('m');
        s.push_back(0x02); s.push_back(0x00); s.push_back(0x00);
        /* one function import per distinct helper, named by its table index */
        for (unsigned i = 0; i < ncalls; i++) {
            char name[16];
            const int n = snprintf(name, sizeof(name), "%u", (unsigned)calls[i]);
            s.push_back(1); s.push_back('f');
            s.push_back((uint8_t)n);
            for (int j = 0; j < n; j++) s.push_back((uint8_t)name[j]);
            s.push_back(0x00);  /* func */
            s.push_back((uint8_t)drc_calltype(call_nargs[i], call_hasret[i]));
        }
        s[0] = (uint8_t)(2 + ncalls);
        drc_b(0x02); drc_uleb((uint32_t)s.size());
        drc_buf.insert(drc_buf.end(), s.begin(), s.end());
    }

    /* function section: one function of type 0 */
    drc_b(0x03); drc_uleb(2); drc_b(1); drc_b(DRC_TYPE_BLOCK);

    /* export section: "f" -> our function, which follows the imports */
    {
        std::vector<uint8_t> s;
        s.push_back(1); s.push_back(1); s.push_back('f'); s.push_back(0x00);
        uint32_t fi = ncalls; do { uint8_t b7 = fi & 0x7f; fi >>= 7; if (fi) b7 |= 0x80; s.push_back(b7); } while (fi);
        drc_b(0x07); drc_uleb((uint32_t)s.size());
        drc_buf.insert(drc_buf.end(), s.begin(), s.end());
    }

    /* code section */
    std::vector<uint8_t> body;
    {
        std::vector<uint8_t> saveMain; saveMain.swap(drc_buf);
        /* emit body into drc_buf then move to body */
        drc_buf.clear();
        drc_used_extra_locals = false;
        drc_flag_type = -1;

        /* Interrupt responsiveness: linked blocks chain without returning to
         * the dispatcher, so a pending IRQ otherwise waits for the cycle
         * budget to run out - up to a full emulated millisecond, which is
         * enough to unsettle timer-paced real-mode games. The interpreter
         * checks between instructions; checking at block entry gets within
         * one block of that for two loads per block. */
        {
            const uint32_t a_flags = (uint32_t)(uintptr_t)DRCD_REG_FLAGS;
            const uint32_t a_pic   = (uint32_t)(uintptr_t)&PIC_IRQCheck;
            drc_iconst(a_flags); drc_load32();
            drc_iconst(0x200u); drc_b(0x71);
            drc_b(0x04); drc_b(0x40);            /* if (IF set) */
            drc_iconst(a_pic); drc_load32();
            drc_b(0x04); drc_b(0x40);            /* if (an IRQ is pending) */
            drc_iconst(1);                        /* BR_Cycles */
            drc_b(0x0F);                          /* return: let it deliver */
            drc_b(0x0B); drc_b(0x0B);
        }

        /* open one wasm block per branch target, outermost = latest target */
        for (unsigned i = ntargets; i-- > 0;) { (void)i; drc_b(0x02); drc_b(0x40); }

        /* Redundant-load elimination: the decoder reloads the same emulator
         * state (a guest register, a segment base) into the same local over
         * and over inside one block. Remember what each local currently holds
         * and skip a load that would reproduce it. Anything that could change
         * emulator state - a call, a store, a branch target - clears the
         * record, so this only ever removes a provably repeated load. */
        uint32_t held_addr[DRC_NUM_LOCALS];
        uint8_t  held_kind[DRC_NUM_LOCALS];   /* 0 none, 1 byte, 2 word, 4 dword */
        for (unsigned i = 0; i < DRC_NUM_LOCALS; i++) { held_addr[i] = 0; held_kind[i] = 0; }
        #define DRC_FORGET_ALL() do { for (unsigned i_ = 0; i_ < DRC_NUM_LOCALS; i_++) held_kind[i_] = 0; } while (0)
        #define DRC_FORGET(l)    do { if ((l) < DRC_NUM_LOCALS) held_kind[l] = 0; } while (0)
        /* forget only what a store to [ad, ad+w) can alias - store addresses
         * in the IR are compile-time constants, so this is exact */
        #define DRC_FORGET_RANGE(ad, w) do { \
            for (unsigned i_ = 0; i_ < DRC_NUM_LOCALS; i_++) \
                if (held_kind[i_] && held_addr[i_] < (ad) + (w) && (ad) < held_addr[i_] + held_kind[i_]) \
                    held_kind[i_] = 0; \
        } while (0)
        const uint32_t a_lflags = (uint32_t)(uintptr_t)&lflags;
        const uint32_t a_rflags = (uint32_t)(uintptr_t)DRCD_REG_FLAGS;

        unsigned closed = 0;    /* targets already closed (label emitted) */
        for (const DRCIRRec* r = first; r < last; r++) {
            /* close blocks whose target is this record */
            while (closed < ntargets && targets[closed] == (uint32_t)(uintptr_t)r)
                { drc_b(0x0B); closed++; DRC_FORGET_ALL(); drc_flag_type = -1; }

            switch (r->op) {
                case DRCIR_MOVRR: drc_getl(r->b); drc_setl(r->a); DRC_FORGET(r->a); break;
                case DRCIR_MOVI:  drc_iconst(r->x); drc_setl(r->a); DRC_FORGET(r->a); break;
                case DRCIR_LOADW: {
                    const uint8_t kind = r->b ? 4 : 2;
                    if (held_kind[r->a] == kind && held_addr[r->a] == r->x) break;  /* already there */
                    int fwd = -1;   /* another local already holds this value */
                    for (unsigned i = 0; i < DRC_NUM_LOCALS; i++)
                        if (held_kind[i] == kind && held_addr[i] == r->x) { fwd = (int)i; break; }
                    if (fwd >= 0) { drc_getl((uint32_t)fwd); drc_setl(r->a); }
                    else {
                        drc_iconst(r->x);
                        if (r->b) drc_load32(); else drc_load16u();
                        drc_setl(r->a);
                    }
                    held_addr[r->a] = r->x; held_kind[r->a] = kind;
                    break;
                }
                case DRCIR_LOADB: {
                    if (held_kind[r->a] == 1 && held_addr[r->a] == r->x) break;
                    int fwd = -1;
                    for (unsigned i = 0; i < DRC_NUM_LOCALS; i++)
                        if (held_kind[i] == 1 && held_addr[i] == r->x) { fwd = (int)i; break; }
                    if (fwd >= 0) { drc_getl((uint32_t)fwd); drc_setl(r->a); }
                    else { drc_iconst(r->x); drc_load8u(); drc_setl(r->a); }
                    held_addr[r->a] = r->x; held_kind[r->a] = 1;
                    break;
                }
                case DRCIR_STOREW:
                    drc_iconst(r->x); drc_getl(r->a);
                    if (r->b) drc_store32(); else drc_store16();
                    if (drc_touches_lflags(r->x, r->b ? 4 : 2)) drc_flag_type = -1;
                    DRC_FORGET_RANGE(r->x, r->b ? 4u : 2u);
                    /* the source local now mirrors the stored dword (narrow
                     * stores may truncate bits the local still carries) */
                    if (r->b && r->a < DRC_NUM_LOCALS) { held_addr[r->a] = r->x; held_kind[r->a] = 4; }
                    break;
                case DRCIR_STOREB:
                    drc_iconst(r->x); drc_getl(r->a); drc_store8();
                    if (drc_touches_lflags(r->x, 1)) drc_flag_type = -1;
                    DRC_FORGET_RANGE(r->x, 1u);
                    break;
                case DRCIR_EXT:
                    drc_getl(r->a);
                    if (r->b) { drc_b(r->c == 8 ? 0xC0 : 0xC1); }             /* extend8_s/16_s */
                    else { drc_iconst(r->c == 8 ? 0xFF : 0xFFFF); drc_b(0x71); }
                    drc_setl(r->a); DRC_FORGET(r->a); break;
                case DRCIR_ADDMEM:
                    drc_getl(r->a); drc_iconst(r->x); drc_load32(); drc_b(0x6A); drc_setl(r->a);
                    DRC_FORGET(r->a); break;
                case DRCIR_ADDI:
                    drc_getl(r->a); drc_iconst(r->x); drc_b(0x6A); drc_setl(r->a);
                    DRC_FORGET(r->a); break;
                case DRCIR_ANDI:
                    drc_getl(r->a); drc_iconst(r->x); drc_b(0x71); drc_setl(r->a);
                    DRC_FORGET(r->a); break;
                case DRCIR_BOOL:
                    drc_getl(r->a); drc_b(0x45); drc_b(0x45); drc_setl(r->a);
                    DRC_FORGET(r->a); break;
                case DRCIR_RETDISP: {
                    const uint32_t a_csphys = (uint32_t)(uintptr_t)&Segs.phys[cs];
                    const uint32_t a_eip    = (uint32_t)(uintptr_t)&reg_eip;
                    const uint32_t a_cache  = (uint32_t)(uintptr_t)&drc_dispatch_cache[0];
                    const uint32_t a_gen    = (uint32_t)(uintptr_t)&drc_dispatch_gen;
                    drc_used_extra_locals = true;
                    /* ip = Segs.phys[cs] + reg_eip */
                    drc_iconst(a_csphys); drc_load32();
                    drc_iconst(a_eip); drc_load32();
                    drc_b(0x6A); drc_teel(DRC_LOCAL_SCRATCH);
                    /* entry = &cache[(ip ^ ip>>8) & (N-1)]; 16-byte entries */
                    drc_iconst(8); drc_b(0x76);
                    drc_getl(DRC_LOCAL_SCRATCH); drc_b(0x73);
                    drc_iconst(DRC_DISPATCH_CACHE_SIZE - 1); drc_b(0x71);
                    drc_iconst(4); drc_b(0x74);
                    drc_iconst(a_cache); drc_b(0x6A);
                    drc_setl(DRC_LOCAL_V1);
                    /* miss if entry.ip != ip or entry.gen != current gen */
                    drc_getl(DRC_LOCAL_V1); drc_b(0x28); drc_uleb(0); drc_uleb(0);
                    drc_getl(DRC_LOCAL_SCRATCH); drc_b(0x47);
                    drc_getl(DRC_LOCAL_V1); drc_b(0x28); drc_uleb(0); drc_uleb(4);
                    drc_iconst(a_gen); drc_load32(); drc_b(0x47);
                    drc_b(0x72);
                    drc_b(0x04); drc_b(0x40);
                    drc_iconst(0); drc_b(0x0F);         /* BR_Normal */
                    drc_b(0x0B);
                    /* tagged table index; 0 means the code was recycled */
                    drc_getl(DRC_LOCAL_V1); drc_b(0x28); drc_uleb(0); drc_uleb(8);
                    drc_iconst(0x7FFFFFFF); drc_b(0x71);
                    drc_teel(DRC_LOCAL_SCRATCH);
                    drc_b(0x45);
                    drc_b(0x04); drc_b(0x40);
                    drc_iconst(0); drc_b(0x0F);
                    drc_b(0x0B);
                    drc_getl(DRC_LOCAL_SCRATCH);
                    drc_b(0x13); drc_uleb(DRC_TYPE_BLOCK); drc_b(0x00);
                    break; }
                case DRCIR_SHRI:
                    drc_getl(r->a); drc_iconst(r->b); drc_b(0x76); drc_setl(r->a);
                    DRC_FORGET(r->a); break;
                case DRCIR_LEA2:
                    drc_getl(r->a); drc_getl(r->b);
                    if (r->c) { drc_iconst(r->c); drc_b(0x74); }
                    drc_b(0x6A);
                    if (r->x) { drc_iconst(r->x); drc_b(0x6A); }
                    drc_setl(r->a); DRC_FORGET(r->a); break;
                case DRCIR_LEA1:
                    drc_getl(r->a);
                    if (r->c) { drc_iconst(r->c); drc_b(0x74); }
                    if (r->x) { drc_iconst(r->x); drc_b(0x6A); }
                    drc_setl(r->a); DRC_FORGET(r->a); break;
                case DRCIR_STOREI:
                    drc_iconst(r->x); drc_iconst(r->y); drc_store32();
                    if (r->x == (uint32_t)(uintptr_t)&lflags.type)
                        drc_flag_type = (r->y < (uint32_t)t_LASTFLAG) ? (int)r->y : -1;
                    else if (drc_touches_lflags(r->x, 4)) drc_flag_type = -1;
                    DRC_FORGET_RANGE(r->x, 4u); break;
                case DRCIR_ADDDIR:
                    drc_iconst(r->x);
                    drc_iconst(r->x);
                    if (r->b) drc_load32(); else drc_load16u();
                    drc_iconst(r->y); drc_b(0x6A);
                    if (r->b) drc_store32(); else drc_store16();
                    if (drc_touches_lflags(r->x, r->b ? 4 : 2)) drc_flag_type = -1;
                    DRC_FORGET_RANGE(r->x, r->b ? 4u : 2u);
                    break;
                case DRCIR_CALL: {
                    unsigned slot = 0; bool found = false;
                    for (unsigned i = 0; i < ncalls; i++) if (calls[i] == r->x) { slot = i; found = true; break; }
                    if (!found) { drc_note_bail(5); return DRC_XSTART_TAG; }
                    if (drc_emit_mem_inline(r->x, slot)) { DRC_FORGET_ALL(); break; }
                    if (drc_emit_pushpop_inline(r->x, slot)) { DRC_FORGET_ALL(); break; }
                    if (drc_emit_zf_inline(r->x, slot)) { DRC_FORGET(FC_RETOP); break; }
                    if (drc_emit_cf_inline(r->x, slot)) { DRC_FORGET(FC_RETOP); break; }
                    if (drc_emit_cond_inline(r->x, slot)) { DRC_FORGET(FC_RETOP); break; }
                    if (drc_emit_adcsbb_inline(r->x, slot)) {
                        DRC_FORGET(FC_RETOP);
                        DRC_FORGET_RANGE(a_lflags, (uint32_t)sizeof(lflags));
                        break;
                    }
                    if (drc_emit_flagop_inline(r->x, slot)) {
                        DRC_FORGET(FC_RETOP);
                        DRC_FORGET_RANGE(a_lflags, (uint32_t)sizeof(lflags));
                        break;
                    }
                    if (drc_emit_unop_inline(r->x, slot)) {
                        DRC_FORGET(FC_RETOP);
                        DRC_FORGET_RANGE(a_lflags, (uint32_t)sizeof(lflags));
                        DRC_FORGET_RANGE(a_rflags, 4u);
                        break;
                    }
                    for (unsigned i = 0; i < r->b; i++) drc_getl(FC_OP1 + i);
                    drc_b(0x10); drc_uleb(slot);        /* call <imported helper> */
                    if (r->c) drc_setl(FC_RETOP);
                    if (!drc_call_is_mem(r->x)) drc_flag_type = -1;
                    DRC_FORGET_ALL();
                    break;
                }
                case DRCIR_SIMPLE: drc_emit_simple(r->x); DRC_FORGET(FC_RETOP); break;
                case DRCIR_JMPPTR:
                    /* idx = *( *(u32*)ptr + offset ); tail-call idx */
                    drc_iconst(r->x); drc_load32();
                    drc_iconst(r->y); drc_b(0x6A); drc_load32();
                    drc_iconst(0x7FFFFFFF); drc_b(0x71);    /* strip tag */
                    drc_teel(DRC_LOCAL_SCRATCH);
                    drc_b(0x45);                            /* eqz: uncompiled target guard */
                    drc_b(0x04); drc_b(0x40);               /* if */
                    drc_iconst(0); drc_b(0x0F);             /* return BR_Normal */
                    drc_b(0x0B);                            /* end if */
                    drc_getl(DRC_LOCAL_SCRATCH);
                    /* 0x13 = return_call_indirect (typeidx, tableidx); 0x12 is
                     * return_call, which would tail-call this same function */
                    drc_b(0x13); drc_uleb(DRC_TYPE_BLOCK); drc_b(0x00);
                    break;
                case DRCIR_BR: {
                    /* find depth: index of target among still-open blocks */
                    unsigned depth = 0; bool found = false;
                    for (unsigned i = closed; i < ntargets; i++) {
                        if (targets[i] == r->x) { found = true; break; }
                        depth++;
                    }
                    if (!found) { drc_note_bail(3); return DRC_XSTART_TAG; }
                    drc_getl(r->b);
                    if (r->c == 8)  { drc_iconst(0xFF); drc_b(0x71); }
                    else if (r->c == 16) { drc_iconst(0xFFFF); drc_b(0x71); }
                    if (r->a == 0) { drc_b(0x45); }                     /* eqz */
                    else if (r->a == 2) { drc_iconst(0); drc_b(0x4C); } /* le_s */
                    drc_b(0x0D); drc_uleb(depth);                       /* br_if */
                    break;
                }
                case DRCIR_RET:
                    drc_getl(FC_RETOP); drc_b(0x0F);
                    break;
                default:
                    drc_note_bail(4); return DRC_XSTART_TAG;
            }
        }
        while (closed < ntargets) { drc_b(0x0B); closed++; }
        /* safety tail */
        drc_iconst(0); drc_b(0x0F);
        drc_b(0x0B);    /* end of function body */

        /* now that the body is known, declare only the locals it used */
        {
            std::vector<uint8_t> withLocals;
            withLocals.push_back(1);
            const uint32_t n = drc_used_extra_locals ? DRC_NUM_LOCALS : (DRC_LOCAL_SCRATCH + 1);
            uint32_t v = n; do { uint8_t b7 = v & 0x7f; v >>= 7; if (v) b7 |= 0x80; withLocals.push_back(b7); } while (v);
            withLocals.push_back(0x7F);
            withLocals.insert(withLocals.end(), drc_buf.begin(), drc_buf.end());
            drc_buf.swap(withLocals);
        }
        body.swap(drc_buf);
        drc_buf.swap(saveMain);
    }
    drc_b(0x0A);
    {
        /* code section payload: count=1, then body size, then body */
        std::vector<uint8_t> s;
        s.push_back(1);
        uint32_t bs = (uint32_t)body.size();
        do { uint8_t b7 = bs & 0x7f; bs >>= 7; if (bs) b7 |= 0x80; s.push_back(b7); } while (bs);
        s.insert(s.end(), body.begin(), body.end());
        drc_uleb((uint32_t)s.size());
        drc_buf.insert(drc_buf.end(), s.begin(), s.end());
    }

    int idx = drc_wasm_compile(drc_buf.data(), (int)drc_buf.size());
    return DRC_XSTART_TAG | (uint32_t)idx;
}

/* ------------------------------------------------------------------ */
/* block lifecycle hooks                                               */
/* ------------------------------------------------------------------ */

static CacheBlockDynRec* drc_closing_block = NULL;
static uint8_t* drc_closing_start = NULL;

static void cache_block_before_close(void) {
    drc_closing_block = cache.block.active;
    drc_closing_start = drc_closing_block->cache.start;
}

static void cache_block_closing(uint8_t* block_start,Bitu block_size) {
    (void)block_size;
    if (!drc_closing_block || drc_closing_block->cache.start != block_start) {
        /* fall back: locate via the recorded start */
        if (!drc_closing_block) return;
    }
    uint8_t* end = cache.pos;
    /* cache_closeblock may have split the block; the IR ends at the split point */
    if (end > block_start + drc_closing_block->cache.size)
        end = block_start + drc_closing_block->cache.size;
    /* IR record stream must be 16-byte aligned in length */
    size_t len = (size_t)(end - block_start);
    len -= len % sizeof(DRCIRRec);
    drc_closing_block->cache.xstart = (uint8_t*)(uintptr_t)drc_translate_and_compile(block_start, block_start + len);
    drc_closing_block = NULL;
}

/* entry trampoline: xstart is a tagged table index */
static BlockReturnDynRec WASM_RunCode(uint8_t* xstart) {
    uint32_t v = (uint32_t)(uintptr_t)xstart;
    if (!(v & DRC_XSTART_TAG) || !(v & 0x7FFFFFFFu)) return BR_Opcode;
    typedef BlockReturnDynRec (*BlockFn)(void);
    return ((BlockFn)(uintptr_t)(v & 0x7FFFFFFFu))();
}

/* used by cache.h to compile the link stubs emitted with dyn_return() */
static uint8_t* drc_compile_range(uint8_t* start, uint8_t* end) {
    return (uint8_t*)(uintptr_t)drc_translate_and_compile(start, end);
}

static void drc_release_block_code(CacheBlockDynRec* block) {
    uint32_t v = (uint32_t)(uintptr_t)block->cache.xstart;
    if ((v & DRC_XSTART_TAG) && (v & 0x7FFFFFFFu)) {
        drc_wasm_release((int)(v & 0x7FFFFFFFu));
        block->cache.xstart = NULL;
    }
}
