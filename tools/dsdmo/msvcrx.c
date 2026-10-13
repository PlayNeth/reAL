/* msvcrx.dll: msvcrt.dll for Microsoft's dsdmo.dll under Wine.  dsdmo imports msvcrt's SSE2 libm entry points
 * (_libm_sse2_*_precise: double arguments in xmm0/xmm1, the result in xmm0), which Wine's msvcrt lacks.  This DLL
 * has them, as thunks to the C runtime, and forwards everything else dsdmo takes from msvcrt (msvcrx.def).
 * tools/dsdmo/patch_dsdmo.py makes a copy of dsdmo.dll that imports msvcrx.dll instead of msvcrt.dll. */
#include <math.h>

__attribute__((used)) double __cdecl rx_cos(double a) { return cos(a); }
__attribute__((used)) double __cdecl rx_sin(double a) { return sin(a); }
__attribute__((used)) double __cdecl rx_tan(double a) { return tan(a); }
__attribute__((used)) double __cdecl rx_log(double a) { return log(a); }
__attribute__((used)) double __cdecl rx_pow(double a, double b) { return pow(a, b); }

#define THUNK1(name, fn) __attribute__((naked)) void name(void) { \
    __asm__("subl $12, %esp\n movsd %xmm0, (%esp)\n call _" #fn "\n fstpl (%esp)\n movsd (%esp), %xmm0\n addl $12, %esp\n ret"); }
THUNK1(libm_cos, rx_cos)
THUNK1(libm_sin, rx_sin)
THUNK1(libm_tan, rx_tan)
THUNK1(libm_log, rx_log)
__attribute__((naked)) void libm_pow(void)
{
    __asm__("subl $20, %esp\n movsd %xmm0, (%esp)\n movsd %xmm1, 8(%esp)\n call _rx_pow\n fstpl (%esp)\n"
            " movsd (%esp), %xmm0\n addl $20, %esp\n ret");
}
__attribute__((naked)) void libm_sqrt(void) { __asm__("sqrtsd %xmm0, %xmm0\n ret"); }
