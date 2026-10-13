/* reAL: the I3DL2 reverb of Windows XP SP3's dsdmo.dll, run as its translation (src/i3dl2.inc) on an emulated
 * 32-bit machine (src/i3dl2_machine.h).  What is written by hand here is what lies around the translated code:
 * the object as its constructor (6ca5b840) leaves it, the C runtime's functions the code calls (_ftol, _CIpow,
 * operator new / delete), CParams' parameter store (IMediaParams' values; no curves), and the base class's part:
 * the format fields, Init (6ca5a0b0: the parameters again, then Discontinuity) and the process call (FBRProcess
 * 6ca5a140: the kernel 66680 chose, in place). */
#include "i3dl2.h"
#include "i3dl2_machine.h"
#include <stdlib.h>
#include <string.h>

#define NPARAMS 13
/* the arena (32-bit addresses) */
#define A_SCRATCH  0x6ca43400u      /* a DSFXI3DL2Reverb for Init and the API */
#define A_OBJ      0x6ca44000u      /* the object, 0x4c8 bytes */
#define A_STACK    0x6ca4c000u      /* the stack's top (it grows down towards the object) */
#define A_HEAP     0x6ca4c000u      /* operator new from here */
#define OBJ_SIZE   0x4c8u

/* the object's fields the hand-written part uses */
#define O_RATE     0x130            /* sample rate */
#define O_CHANNELS 0x134
#define O_IS8      0x138            /* 8-bit PCM */
#define O_WFX      0x154            /* the format (WAVEFORMATEX) */
#define O_PARAMS   0x180            /* CParams (IMediaParams) */
#define O_REVERB   0x1d4            /* IDirectSoundFXI3DL2Reverb */
#define O_DIRTY    0x1e8
#define O_INIT     0x1e9            /* Init has run: the decay coefficients are computed from then on */
#define O_KERNEL   0x1f0            /* the process kernel and its this adjustment */
#define O_KTHIS    0x1f4
#define O_QUALITY  0x228
#define O_PRESET   0x3d4

typedef struct { uint32_t va, size; } blk;

struct i3dl2 {
    i3m m;
    uint32_t fixes;
    uint32_t pv[NPARAMS];           /* CParams' values (float bits), once set */
    uint8_t pset[NPARAMS];
    uint32_t size;                  /* arena bytes from I3_BASE */
    uint32_t top;                   /* the heap's end */
    blk *freel; uint32_t nfree, capfree;
    uint32_t scratch, scratch_size; /* process data */
    int hasfmt;
    unsigned block;                 /* bytes per frame */
};

#include "i3dl2.inc"

/* ------------------------------------------------------------------ the arena's heap */
static int arena_grow(i3dl2 *r, uint32_t need)
{
    if (need <= r->size) return 1;
    uint32_t n = r->size;
    while (n < need) n *= 2;
    uint8_t *p = realloc(r->m.mem, n);
    if (!p) return 0;
    memset(p + r->size, 0, n - r->size);
    r->m.mem = p;
    r->size = n;
    return 1;
}
/* block sizes are kept in a header word before the block; freed blocks are reused first fit */
static uint32_t heap_new(i3dl2 *r, uint32_t n)
{
    n = (n + 15) & ~15u;
    for (uint32_t i = 0; i < r->nfree; i++) {
        if (r->freel[i].size >= n) {
            uint32_t va = r->freel[i].va;
            r->freel[i] = r->freel[--r->nfree];
            return va;
        }
    }
    uint32_t va = r->top + 16;
    if (!arena_grow(r, va + n - I3_BASE)) return 0;
    memcpy(r->m.mem + (va - 4 - I3_BASE), &n, 4);
    r->top = va + n;
    return va;
}
static void heap_delete(i3dl2 *r, uint32_t va)
{
    if (!va) return;
    uint32_t n;
    memcpy(&n, r->m.mem + (va - 4 - I3_BASE), 4);
    if (va + n == r->top) { r->top = va - 16; return; }
    if (r->nfree == r->capfree) {
        uint32_t cap = r->capfree ? r->capfree * 2 : 32;
        blk *p = realloc(r->freel, cap * sizeof(blk));
        if (!p) return;                 /* lost: the arena only grows */
        r->freel = p; r->capfree = cap;
    }
    r->freel[r->nfree].va = va; r->freel[r->nfree].size = n; r->nfree++;
}

/* ------------------------------------------------------------------ what the translated code calls */
static i3dl2 *of(i3m *c) { return (i3dl2 *)c; }

/* msvcrt _ftol: ST0 truncated to 64 bits in edx:eax, popped */
static void f_6ca66c7e(i3m *c)
{
    long double v = ST(0);
    FPOP();
    int64_t r = INT64_MIN;              /* the x87's integer indefinite */
    if (v > -9223372036854775808.0L && v < 9223372036854775808.0L) r = (int64_t)v;
    c->eax = (uint32_t)r; c->edx = (uint32_t)((uint64_t)r >> 32);
    c->esp += 4;
}
/* msvcrt _CIpow: pow(ST1, ST0), one popped */
static void f_6ca59bd6(i3m *c)
{
    long double y = ST(0);
    FPOP();
    ST(0) = pow((double)ST(0), (double)y);
    c->esp += 4;
}
/* operator new (cdecl: the caller pops the size) */
static void f_6ca59dd8(i3m *c)
{
    c->eax = heap_new(of(c), M32(c->esp + 4));
    c->esp += 4;
}
static void f_6ca59be2(i3m *c)
{
    heap_delete(of(c), M32(c->esp + 4));
    c->esp += 4;
}
/* CParams' store (this, index, value), stdcall: SetParam's value kept as the current one */
static void f_6ca58e70(i3m *c)
{
    i3dl2 *r = of(c);
    uint32_t i = M32(c->esp + 8), v = M32(c->esp + 12);
    if (i >= NPARAMS) c->eax = 0x80070057u;
    else { r->pv[i] = v; r->pset[i] = 1; c->eax = 0; }
    c->esp += 16;
}
/* the parameter table (6ca6a1e0): 0x9c bytes an entry, the neutral (default) value at +0x14 */
static uint32_t param_default(uint32_t i)
{
    uint32_t v;
    memcpy(&v, i3_data[3].d + i * 0x9c + 0x14, 4);
    return v;
}
/* IMediaParams::GetParam (this, index, float *) */
static void f_6ca58de0(i3m *c)
{
    i3dl2 *r = of(c);
    uint32_t i = M32(c->esp + 8), p = M32(c->esp + 12);
    if (i >= NPARAMS) c->eax = 0x80070057u;
    else { M32(p) = r->pset[i] ? r->pv[i] : param_default(i); c->eax = 0; }
    c->esp += 16;
}

/* a call into the translated code: ecx (thiscall's this, or anything), the arguments, eax back */
static uint32_t call(i3dl2 *r, uint32_t fn, uint32_t ecx, int n, const uint32_t *args)
{
    i3m *c = &r->m;
    c->esp = A_STACK;
    for (int i = n - 1; i >= 0; i--) PUSH(args[i]);
    PUSH(0);                            /* the return address */
    c->ecx = ecx;
    i3_call(c, fn);
    return c->eax;
}
#define CALL(r, fn, ecx, ...) call(r, fn, ecx, sizeof((uint32_t[]){ __VA_ARGS__ }) / 4, (uint32_t[]){ __VA_ARGS__ })
#define CALL0(r, fn, ecx) call(r, fn, ecx, 0, NULL)

/* FBRProcess 6ca5a140: the kernel 66680 chose, in place (no parameter curves to update) */
static void kernel(i3dl2 *r, uint32_t frames, uint32_t at)
{
    i3m *c = &r->m;
    CALL(r, M32(A_OBJ + O_KERNEL), A_OBJ + M32(A_OBJ + O_KTHIS), frames, at, at);
}

/* ------------------------------------------------------------------ the API */
i3dl2 *i3dl2_new(uint32_t fixes)
{
    i3dl2 *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->size = 0x10000;
    r->m.mem = calloc(1, r->size);
    if (!r->m.mem) { free(r); return NULL; }
    r->top = A_HEAP;
    r->fixes = fixes;
    i3m *c = &r->m;
    for (unsigned k = 0; k < sizeof(i3_data) / sizeof(i3_data[0]); k++)
        if (i3_data[k].va + i3_data[k].len <= A_SCRATCH) memcpy(I3P(i3_data[k].va), i3_data[k].d, i3_data[k].len);
    /* 6ca5b840 (operator new's memory is not cleared: on a fresh heap it is zero, as here) */
    uint32_t o = A_OBJ;
    M32(o + O_PARAMS) = 0x6ca43204u;
    M32(o + O_REVERB) = 0x6ca431acu;
    M32(o + O_PRESET) = 0xffffffffu;
    M32(o + O_RATE) = 22050;
    M8(o + O_INIT) = 0;
    M8(o + O_DIRTY) = 1;
    for (uint32_t k = 0; k < 12; k++) M32(o + 0x3e0 + 12 * k) = 5;
    M32(o + 0x488) = 1; M32(o + 0x494) = 1;
    M32(o + 0x4a0) = 0xa3; M32(o + 0x4ac) = 0xa3;
    if (fixes & I3DL2_FIX_QUALITY) M32(o + O_QUALITY) = (uint32_t)(int32_t)i3dl2_get_param(r, 12);   /* the default, 2 */
    return r;
}

void i3dl2_free(i3dl2 *r)
{
    if (!r) return;
    free(r->m.mem);
    free(r->freel);
    free(r);
}

int i3dl2_set_format(i3dl2 *r, unsigned tag, unsigned ch, unsigned rate, unsigned bits)
{
    i3m *c = &r->m;
    uint32_t o = A_OBJ;
    unsigned short cw = x87_enter();
    M16(o + O_WFX) = (uint16_t)tag;
    M16(o + O_WFX + 2) = (uint16_t)ch;
    M32(o + O_WFX + 4) = rate;
    M32(o + O_WFX + 8) = rate * ch * bits / 8;
    M16(o + O_WFX + 12) = (uint16_t)(ch * bits / 8);
    M16(o + O_WFX + 14) = (uint16_t)bits;
    M32(o + O_RATE) = rate;
    M32(o + O_CHANNELS) = ch;
    M32(o + O_IS8) = bits == 8;
    r->block = ch * bits / 8;
    /* Init 6ca5a0b0: GetAllParameters, SetAllParameters, Discontinuity */
    M8(o + O_INIT) = 1;
    int hr = (int)CALL(r, 0x6ca5a890u, 0, o + O_REVERB, A_SCRATCH);
    if (hr >= 0) hr = (int)CALL(r, 0x6ca5a780u, 0, o + O_REVERB, A_SCRATCH);
    if (hr >= 0) hr = (int)CALL0(r, 0x6ca66860u, o);
    x87_leave(cw);
    r->hasfmt = hr >= 0 && M32(o + O_KERNEL);
    return hr < 0 ? hr : 0;
}

int i3dl2_set_param(i3dl2 *r, int i, float v)
{
    uint32_t b;
    memcpy(&b, &v, 4);
    unsigned short cw = x87_enter();
    int hr = (int)CALL(r, 0x6ca66630u, 0, A_OBJ + O_PARAMS, (uint32_t)i, b);
    x87_leave(cw);
    return hr;
}
float i3dl2_get_param(const i3dl2 *r, int i)
{
    if (i < 0 || i >= NPARAMS) return 0.0f;
    uint32_t b = r->pset[i] ? r->pv[i] : param_default((uint32_t)i);
    float v;
    memcpy(&v, &b, 4);
    return v;
}

int i3dl2_set_all(i3dl2 *r, const void *p)
{
    i3m *c = &r->m;
    memcpy(I3P(A_SCRATCH), p, 48);
    unsigned short cw = x87_enter();
    int hr = (int)CALL(r, 0x6ca5a780u, 0, A_OBJ + O_REVERB, A_SCRATCH);
    x87_leave(cw);
    return hr;
}
int i3dl2_get_all(i3dl2 *r, void *p)
{
    i3m *c = &r->m;
    unsigned short cw = x87_enter();
    memset(I3P(A_SCRATCH), 0, 48);
    int hr = (int)CALL(r, 0x6ca5a890u, 0, A_OBJ + O_REVERB, A_SCRATCH);
    x87_leave(cw);
    memcpy(p, I3P(A_SCRATCH), 48);
    return hr;
}
int i3dl2_set_preset(i3dl2 *r, uint32_t preset)
{
    unsigned short cw = x87_enter();
    int hr = (int)CALL(r, 0x6ca5aa10u, 0, A_OBJ + O_REVERB, preset);
    x87_leave(cw);
    return hr;
}
int i3dl2_get_preset(i3dl2 *r, uint32_t *preset)
{
    i3m *c = &r->m;
    int hr = (int)CALL(r, 0x6ca5aa50u, 0, A_OBJ + O_REVERB, A_SCRATCH);
    if (hr >= 0) *preset = M32(A_SCRATCH);
    return hr;
}
int i3dl2_set_quality(i3dl2 *r, int32_t q)
{
    unsigned short cw = x87_enter();
    int hr = (int)CALL(r, 0x6ca5aa90u, 0, A_OBJ + O_REVERB, (uint32_t)q);
    x87_leave(cw);
    return hr;
}
int i3dl2_get_quality(i3dl2 *r, int32_t *q)
{
    i3m *c = &r->m;
    unsigned short cw = x87_enter();
    int hr = (int)CALL(r, 0x6ca5aae0u, 0, A_OBJ + O_REVERB, A_SCRATCH);
    x87_leave(cw);
    *q = (int32_t)M32(A_SCRATCH);
    return hr;
}

void i3dl2_discontinuity(i3dl2 *r)
{
    i3m *c = &r->m;
    unsigned short cw = x87_enter();
    CALL0(r, 0x6ca66860u, A_OBJ);
    x87_leave(cw);
    r->hasfmt = M32(A_OBJ + O_KERNEL) != 0;
}

/* IMediaObjectInPlace::Process (6ca44e70) runs FBRProcess on blocks of 3 ms (at least 16 frames, 6ca45120's
 * +0x174) while more than a block is left, then on the rest: the kernels see those blocks (which shows in 8-bit
 * at half rate, where the sample a block owes from the last is written from the low byte) */
void i3dl2_process(i3dl2 *r, void *data, uint32_t frames)
{
    i3m *c = &r->m;
    if (!r->hasfmt || !frames) return;
    uint32_t n = frames * r->block;
    if (n > r->scratch_size) {
        heap_delete(r, r->scratch);
        r->scratch = heap_new(r, n);
        r->scratch_size = r->scratch ? n : 0;
        if (!r->scratch) return;
    }
    memcpy(I3P(r->scratch), data, n);
    uint32_t blk = M32(A_OBJ + O_WFX + 4) * 3 / 1000;
    if (blk < 16) blk = 16;
    unsigned short cw = x87_enter();
    uint32_t done = 0, at = r->scratch;
    if (frames > blk)
        for (; done < frames - blk; done += blk, at += blk * r->block) kernel(r, blk, at);
    kernel(r, frames - done, at);
    x87_leave(cw);
    memcpy(data, I3P(r->scratch), n);
}

void i3dl2_object(const i3dl2 *r, void *out)
{
    memcpy(out, r->m.mem + (A_OBJ - I3_BASE), OBJ_SIZE);
}
/* debugging: call one of the original's functions (thiscall on the object) and give the 0x200 bytes of stack
 * below the call, its frame as it left it */
void i3dl2_frame(i3dl2 *r, uint32_t fn, void *out)
{
    i3m *c = &r->m;
    unsigned short cw = x87_enter();
    memset(I3P(A_STACK - 0x200), 0, 0x200);
    c->esp = A_STACK;
    c->ecx = A_OBJ;
    i3_call(c, fn);
    x87_leave(cw);
    memcpy(out, I3P(A_STACK - 0x200), 0x200);
}
