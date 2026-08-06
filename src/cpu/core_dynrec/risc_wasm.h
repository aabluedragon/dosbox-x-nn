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
#define DRC_NUM_LOCALS 9   /* one scratch local (8) used by the translator */
#define DRC_LOCAL_SCRATCH 8

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
        const inst = new WebAssembly.Instance(mod, { e: { t: wasmTable, m: wasmMemory } });
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
        s.push_back(2);
        /* "e" "t" table funcref min 0 */
        s.push_back(1); s.push_back('e'); s.push_back(1); s.push_back('t');
        s.push_back(0x01); s.push_back(0x70); s.push_back(0x00); s.push_back(0x00);
        /* "e" "m" memory min 0 */
        s.push_back(1); s.push_back('e'); s.push_back(1); s.push_back('m');
        s.push_back(0x02); s.push_back(0x00); s.push_back(0x00);
        drc_b(0x02); drc_uleb((uint32_t)s.size());
        drc_buf.insert(drc_buf.end(), s.begin(), s.end());
    }

    /* function section: one function of type 0 */
    drc_b(0x03); drc_uleb(2); drc_b(1); drc_b(DRC_TYPE_BLOCK);

    /* export section: "f" -> func 0 */
    drc_b(0x07); drc_uleb(5); drc_b(1); drc_b(1); drc_b('f'); drc_b(0x00); drc_b(0);

    /* code section */
    std::vector<uint8_t> body;
    {
        std::vector<uint8_t> saveMain; saveMain.swap(drc_buf);
        /* emit body into drc_buf then move to body */
        drc_buf.clear();

        /* locals: DRC_NUM_LOCALS i32 */
        drc_b(1); drc_uleb(DRC_NUM_LOCALS); drc_b(0x7F);

        /* open one wasm block per branch target, outermost = latest target */
        for (unsigned i = ntargets; i-- > 0;) { (void)i; drc_b(0x02); drc_b(0x40); }

        unsigned closed = 0;    /* targets already closed (label emitted) */
        for (const DRCIRRec* r = first; r < last; r++) {
            /* close blocks whose target is this record */
            while (closed < ntargets && targets[closed] == (uint32_t)(uintptr_t)r)
                { drc_b(0x0B); closed++; }

            switch (r->op) {
                case DRCIR_MOVRR: drc_getl(r->b); drc_setl(r->a); break;
                case DRCIR_MOVI:  drc_iconst(r->x); drc_setl(r->a); break;
                case DRCIR_LOADW:
                    drc_iconst(r->x);
                    if (r->b) drc_load32(); else drc_load16u();
                    drc_setl(r->a); break;
                case DRCIR_LOADB:
                    drc_iconst(r->x); drc_load8u(); drc_setl(r->a); break;
                case DRCIR_STOREW:
                    drc_iconst(r->x); drc_getl(r->a);
                    if (r->b) drc_store32(); else drc_store16(); break;
                case DRCIR_STOREB:
                    drc_iconst(r->x); drc_getl(r->a); drc_store8(); break;
                case DRCIR_EXT:
                    drc_getl(r->a);
                    if (r->b) { drc_b(r->c == 8 ? 0xC0 : 0xC1); }             /* extend8_s/16_s */
                    else { drc_iconst(r->c == 8 ? 0xFF : 0xFFFF); drc_b(0x71); }
                    drc_setl(r->a); break;
                case DRCIR_ADDMEM:
                    drc_getl(r->a); drc_iconst(r->x); drc_load32(); drc_b(0x6A); drc_setl(r->a); break;
                case DRCIR_ADDI:
                    drc_getl(r->a); drc_iconst(r->x); drc_b(0x6A); drc_setl(r->a); break;
                case DRCIR_ANDI:
                    drc_getl(r->a); drc_iconst(r->x); drc_b(0x71); drc_setl(r->a); break;
                case DRCIR_SHRI:
                    drc_getl(r->a); drc_iconst(r->b); drc_b(0x76); drc_setl(r->a); break;
                case DRCIR_LEA2:
                    drc_getl(r->a); drc_getl(r->b);
                    if (r->c) { drc_iconst(r->c); drc_b(0x74); }
                    drc_b(0x6A);
                    if (r->x) { drc_iconst(r->x); drc_b(0x6A); }
                    drc_setl(r->a); break;
                case DRCIR_LEA1:
                    drc_getl(r->a);
                    if (r->c) { drc_iconst(r->c); drc_b(0x74); }
                    if (r->x) { drc_iconst(r->x); drc_b(0x6A); }
                    drc_setl(r->a); break;
                case DRCIR_STOREI:
                    drc_iconst(r->x); drc_iconst(r->y); drc_store32(); break;
                case DRCIR_ADDDIR:
                    drc_iconst(r->x);
                    drc_iconst(r->x);
                    if (r->b) drc_load32(); else drc_load16u();
                    drc_iconst(r->y); drc_b(0x6A);
                    if (r->b) drc_store32(); else drc_store16();
                    break;
                case DRCIR_CALL: {
                    if (r->b > 4) { drc_note_bail(5); return DRC_XSTART_TAG; }
                    for (unsigned i = 0; i < r->b; i++) drc_getl(FC_OP1 + i);
                    drc_iconst(r->x);
                    drc_b(0x11); drc_uleb(drc_calltype(r->b, r->c)); drc_b(0x00);
                    if (r->c) drc_setl(FC_RETOP);
                    break;
                }
                case DRCIR_SIMPLE: drc_emit_simple(r->x); break;
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
