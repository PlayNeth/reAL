/* reAL: the emulated 32-bit machine that runs the I3DL2 kernels translated from XP's dsdmo.dll
 * (tools/i3dl2/asm2c.py -> i3dl2_kernels.inc).  Memory is an arena addressed by 32-bit offsets; the integer
 * registers and flags are variables; the x87 stack holds long doubles (the x87's own format), so with the control
 * word set to 53-bit precision, round to nearest (x87_enter) the arithmetic is the original's, bit for bit. */
#ifndef REAL_I3DL2_MACHINE_H
#define REAL_I3DL2_MACHINE_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

typedef struct i3m {
    uint8_t *mem;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t res;
    int zf, sf, of, cf, pf;
    uint32_t fsw;          /* the x87 status word's condition codes (C0 C2 C3), for fnstsw */
    void *user;
    long double S[8];
    int T;
} i3m;

/* the arena holds the 32-bit addresses from I3_BASE (the DLL's image base) up: the DLL's data the code reads
 * (vtables, presets, the parameter table) at their own addresses, then the object, the stack and the heap */
#define I3_BASE 0x6ca40000u
#define I3P(a) (c->mem + ((uint32_t)(a) - I3_BASE))
#define M8(a)  (*I3P(a))
#define M16(a) (*(uint16_t *)I3P(a))
#define M32(a) (*(uint32_t *)I3P(a))
#define MF(a)  (*(float *)I3P(a))
#define MD(a)  (*(double *)I3P(a))
#define ST(i)  (c->S[(c->T + (i)) & 7])
#define FPUSH(v) do { long double fpush_v = (v); c->T = (c->T - 1) & 7; c->S[c->T] = fpush_v; } while (0)
#define FPOP()   (c->T = (c->T + 1) & 7)
#define PUSH(v)  do { uint32_t push_v = (v); c->esp -= 4; M32(c->esp) = push_v; } while (0)
#define POP()    (c->esp += 4, M32(c->esp - 4))

#define PARITY(r) (!(__builtin_popcount((r) & 0xffu) & 1))
#define SZF(r) (c->res = (r), c->zf = c->res == 0, c->sf = (int)(c->res >> 31), c->pf = PARITY(c->res))
#define FLAGS_ADD(a, b) do { uint32_t fa = (a), fb = (b), fr = fa + fb; SZF(fr); c->cf = fr < fa; \
    c->of = (int)((~(fa ^ fb) & (fa ^ fr)) >> 31); } while (0)
#define FLAGS_SUB(a, b) do { uint32_t fa = (a), fb = (b), fr = fa - fb; SZF(fr); c->cf = fa < fb; \
    c->of = (int)(((fa ^ fb) & (fa ^ fr)) >> 31); } while (0)
#define FLAGS_CMP FLAGS_SUB
#define FLAGS_AND(a, b) do { SZF((uint32_t)(a) & (uint32_t)(b)); c->cf = c->of = 0; } while (0)
#define FLAGS_TEST FLAGS_AND
#define FLAGS_OR(a, b)  do { SZF((uint32_t)(a) | (uint32_t)(b)); c->cf = c->of = 0; } while (0)
#define FLAGS_XOR(a, b) do { SZF((uint32_t)(a) ^ (uint32_t)(b)); c->cf = c->of = 0; } while (0)
#define FLAGS_INC(a) do { uint32_t fa = (a); SZF(fa + 1); c->of = fa == 0x7fffffffu; } while (0)
#define FLAGS_DEC(a) do { uint32_t fa = (a); SZF(fa - 1); c->of = fa == 0x80000000u; } while (0)
#define FLAGS_NEG(a) do { uint32_t fa = (a); SZF(0u - fa); c->cf = fa != 0; c->of = fa == 0x80000000u; } while (0)
#define FLAGS_NOT(a) (c->res = ~(uint32_t)(a))
#define FLAGS_SHL(a, n) do { uint32_t fa = (a); SZF(fa << (n)); c->cf = (int)((fa >> (32 - (n))) & 1); } while (0)
#define FLAGS_SHR(a, n) do { uint32_t fa = (a); SZF(fa >> (n)); c->cf = (int)((fa >> ((n) - 1)) & 1); } while (0)
#define FLAGS_SAR(a, n) do { uint32_t fa = (a); SZF((uint32_t)((int32_t)fa >> (n))); c->cf = (int)((fa >> ((n) - 1)) & 1); } while (0)
/* 8-bit forms: the flags of the low byte (the result is stored back by the caller) */
#define SZF8(r) (c->res = (r) & 0xffu, c->zf = c->res == 0, c->sf = (int)((c->res >> 7) & 1), c->pf = PARITY(c->res))
#define FLAGS8_TEST(a, b) do { SZF8((uint32_t)(a) & (uint32_t)(b)); c->cf = c->of = 0; } while (0)
#define FLAGS8_AND FLAGS8_TEST
#define FLAGS8_ADD(a, b) do { uint32_t fa = (a) & 0xffu, fb = (b) & 0xffu, fr = fa + fb; SZF8(fr); c->cf = fr > 0xffu; \
    c->of = (int)(((~(fa ^ fb) & (fa ^ fr)) >> 7) & 1); } while (0)
#define FLAGS8_SUB(a, b) do { uint32_t fa = (a) & 0xffu, fb = (b) & 0xffu, fr = fa - fb; SZF8(fr); c->cf = fa < fb; \
    c->of = (int)((((fa ^ fb) & (fa ^ fr)) >> 7) & 1); } while (0)
#define FLAGS8_CMP FLAGS8_SUB
#define FLAGS8_XOR(a, b) do { SZF8((uint32_t)(a) ^ (uint32_t)(b)); c->cf = c->of = 0; } while (0)
#define FLAGS8_OR(a, b)  do { SZF8((uint32_t)(a) | (uint32_t)(b)); c->cf = c->of = 0; } while (0)

/* fcom: C3 C2 C0 = 000 (a > b), 001 (a < b), 100 (equal), 111 (unordered) */
#define FCOM(a, b) do { long double fca = (a), fcb = (b); \
    c->fsw = fca > fcb ? 0 : fca < fcb ? 0x100u : fca == fcb ? 0x4000u : 0x4500u; } while (0)

/* fistp under the control word (round to nearest): the x87's own instruction */
static inline int32_t x87_fist(long double v)
{
    int32_t r;
    __asm__ __volatile__("fistpl %0" : "=m"(r) : "t"(v) : "st");
    return r;
}
/* the x87 at MSVC's settings (53-bit precision, round to nearest, exceptions masked); the old control word back */
static inline unsigned short x87_enter(void)
{
    unsigned short old, cw = 0x27f;
    __asm__ __volatile__("fnstcw %0" : "=m"(old));
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
    return old;
}
static inline void x87_leave(unsigned short old) { __asm__ __volatile__("fldcw %0" : : "m"(old)); }
static inline long double x87_cos(long double v) { long double r; __asm__("fcos" : "=t"(r) : "0"(v)); return r; }
static inline long double x87_sqrt(long double v) { long double r; __asm__("fsqrt" : "=t"(r) : "0"(v)); return r; }

#endif
