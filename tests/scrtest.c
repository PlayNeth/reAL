/* scrtest - DirectMusic scripts (AudioVBScript), printed for comparison with Microsoft's dmscript:
 *   scrtest <dir> lang     scripts made here from source snippets: syntax errors, routines, variables, runtime
 *                          errors, the "DirectMusic" item's constants and performance methods (the messages they
 *                          send, seen by an application tool)
 *   scrtest <dir> files    the script files of tools/sty/cases.py's 'script' case: content, file names in errors
 *   scrtest <dir> auto     the 'scriptauto' case: scripts driving segments, states and audiopaths
 *   scrtest <dir> ole      VBScript and JScript
 *   scrtest <dir> track    the 'scripttrack' case: segments with a script track */
#define COBJMACROS
#include <windows.h>
#include <dmusici.h>
#include <dmplugin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IDirectMusicScript_Init
#define IDirectMusicScript_Init(p,a,b) (p)->lpVtbl->Init(p,a,b)
#define IDirectMusicScript_CallRoutine(p,a,b) (p)->lpVtbl->CallRoutine(p,a,b)
#define IDirectMusicScript_SetVariableVariant(p,a,b,c,d) (p)->lpVtbl->SetVariableVariant(p,a,b,c,d)
#define IDirectMusicScript_GetVariableVariant(p,a,b,c) (p)->lpVtbl->GetVariableVariant(p,a,b,c)
#define IDirectMusicScript_SetVariableNumber(p,a,b,c) (p)->lpVtbl->SetVariableNumber(p,a,b,c)
#define IDirectMusicScript_GetVariableNumber(p,a,b,c) (p)->lpVtbl->GetVariableNumber(p,a,b,c)
#define IDirectMusicScript_SetVariableObject(p,a,b,c) (p)->lpVtbl->SetVariableObject(p,a,b,c)
#define IDirectMusicScript_GetVariableObject(p,a,b,c,d) (p)->lpVtbl->GetVariableObject(p,a,b,c,d)
#define IDirectMusicScript_EnumRoutine(p,a,b) (p)->lpVtbl->EnumRoutine(p,a,b)
#define IDirectMusicScript_EnumVariable(p,a,b) (p)->lpVtbl->EnumVariable(p,a,b)
#endif

static IDirectMusicPerformance8 *perf;
static IDirectMusicLoader8 *loader;

/* ------------------------------------------------------------------ a tool printing the messages it gets */
static IDirectMusicTool toolobj;
static int g_quiet;                 /* (only the script methods' messages) */
static CRITICAL_SECTION logcs;
static char logbuf[16384];
static HRESULT WINAPI t_qi(IDirectMusicTool *i, REFIID r, void **o)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirectMusicTool)) { *o = i; return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI t_addref(IDirectMusicTool *i) { return 2; }
static ULONG WINAPI t_release(IDirectMusicTool *i) { return 1; }
static HRESULT WINAPI t_init(IDirectMusicTool *i, IDirectMusicGraph *g) { return S_OK; }
static HRESULT WINAPI t_deliv(IDirectMusicTool *i, DWORD *d) { *d = DMUS_PMSGF_TOOL_IMMEDIATE; return S_OK; }
static HRESULT WINAPI t_nt(IDirectMusicTool *i, DWORD *n) { return E_NOTIMPL; }
static HRESULT WINAPI t_types(IDirectMusicTool *i, DWORD **a, DWORD n) { return E_NOTIMPL; }
static HRESULT WINAPI t_process(IDirectMusicTool *i, IDirectMusicPerformance *p, DMUS_PMSG *m)
{
    char b[512];
    b[0] = 0;
    if (m->dwType == DMUS_PMSGT_SCRIPTLYRIC) {
        snprintf(b, sizeof(b), "  msg scriptlyric size %lu flags %lx pch %lx vt %lu grp %lx \"%ls\"\n", m->dwSize,
                 m->dwFlags & ~DMUS_PMSGF_MUSICTIME, m->dwPChannel, m->dwVirtualTrackID, m->dwGroupID,
                 (WCHAR *)((BYTE *)m + 0x38));
    } else if (m->dwType == DMUS_PMSGT_CURVE) {
        DMUS_CURVE_PMSG *c = (DMUS_CURVE_PMSG *)m;
        snprintf(b, sizeof(b), "  msg curve size %lu flags %lx pch %lx grp %lx dur %ld start %d end %d type %d shape %d cc %d fl %d merge %x\n",
                 m->dwSize, m->dwFlags & ~DMUS_PMSGF_MUSICTIME, m->dwPChannel, m->dwGroupID, c->mtDuration,
                 c->nStartValue, c->nEndValue, c->bType, c->bCurveShape, c->bCCData, c->bFlags, c->wMergeIndex);
    } else if (m->dwType == DMUS_PMSGT_TRANSPOSE) {
        DMUS_TRANSPOSE_PMSG *t = (DMUS_TRANSPOSE_PMSG *)m;
        snprintf(b, sizeof(b), "  msg transpose size %lu flags %lx pch %lx grp %lx by %d merge %x\n", m->dwSize,
                 m->dwFlags & ~DMUS_PMSGF_MUSICTIME, m->dwPChannel, m->dwGroupID, t->nTranspose, t->wMergeIndex);
    } else if (m->dwType == 15000) {   /* a script track's script error */
        const DMUS_SCRIPT_ERRORINFO *e = (const DMUS_SCRIPT_ERRORINFO *)((BYTE *)m + 0x38);
        snprintf(b, sizeof(b), "  msg scripterror size %lu flags %lx vt %lu grp %lx tool %d graph %d err %08lx line %lu col %ld file '%ls' comp '%ls' desc '%ls'\n",
                 m->dwSize, m->dwFlags, m->dwVirtualTrackID, m->dwGroupID, m->pTool != NULL, m->pGraph != NULL,
                 e->hr, e->ulLineNumber, e->ichCharPosition, e->wszSourceFile, e->wszSourceComponent, e->wszDescription);
    } else if (m->dwType != DMUS_PMSGT_DIRTY && m->dwType != DMUS_PMSGT_NOTIFICATION && !g_quiet) {
        snprintf(b, sizeof(b), "  msg type %lu pch %lx\n", m->dwType, m->dwPChannel);
    }
    EnterCriticalSection(&logcs);
    if (strlen(logbuf) + strlen(b) < sizeof(logbuf)) strcat(logbuf, b);
    LeaveCriticalSection(&logcs);
    return DMUS_S_FREE;
}
static HRESULT WINAPI t_flush(IDirectMusicTool *i, IDirectMusicPerformance *p, DMUS_PMSG *m, REFERENCE_TIME t) { return DMUS_S_FREE; }
static IDirectMusicToolVtbl tvtbl = { t_qi, t_addref, t_release, t_init, t_deliv, t_nt, t_types, t_process, t_flush };
static void flushlog(void)
{
    Sleep(150);
    EnterCriticalSection(&logcs);
    fputs(logbuf, stdout);
    logbuf[0] = 0;
    LeaveCriticalSection(&logcs);
}

/* ------------------------------------------------------------------ printing */
static void perr(const DMUS_SCRIPT_ERRORINFO *e)
{
    if (!e->hr && !e->ulLineNumber && !e->wszDescription[0]) return;
    printf("    err %08lx line %lu col %ld file '%ls' comp '%ls'\n      desc '%ls'\n      text '%ls'\n", e->hr,
           e->ulLineNumber, e->ichCharPosition, e->wszSourceFile, e->wszSourceComponent, e->wszDescription,
           e->wszSourceLineText);
}
static void einit(DMUS_SCRIPT_ERRORINFO *e) { memset(e, 0, sizeof(*e)); e->dwSize = sizeof(*e); }
static void pvar(const VARIANT *v)
{
    switch (V_VT(v)) {
    case VT_EMPTY: printf("empty"); break;
    case VT_I4: printf("i4 %ld", V_I4(v)); break;
    case VT_I2: printf("i2 %d", V_I2(v)); break;
    case VT_BSTR: printf("bstr '%ls'", V_BSTR(v) ? V_BSTR(v) : L"(null)"); break;
    case VT_UNKNOWN: case VT_DISPATCH: printf("%s %s", V_VT(v) == VT_UNKNOWN ? "unk" : "disp", V_UNKNOWN(v) ? "obj" : "null"); break;
    default: printf("vt %d", V_VT(v));
    }
}
static void enum_names(IDirectMusicScript *s)
{
    WCHAR n[300];
    printf("  routines:");
    for (DWORD k = 0; k < 40; k++) {
        HRESULT hr = IDirectMusicScript_EnumRoutine(s, k, n);
        if (hr != S_OK) { printf(" [%08lx]", hr); break; }
        printf(" %ls", n);
    }
    printf("\n  variables:");
    for (DWORD k = 0; k < 40; k++) {
        HRESULT hr = IDirectMusicScript_EnumVariable(s, k, n);
        if (hr != S_OK) { printf(" [%08lx]", hr); break; }
        printf(" %ls", n);
    }
    printf("\n");
}
static HRESULT call(IDirectMusicScript *s, const char *name)
{
    WCHAR w[128];
    DMUS_SCRIPT_ERRORINFO e;
    einit(&e);
    MultiByteToWideChar(CP_ACP, 0, name, -1, w, 128);
    HRESULT hr = IDirectMusicScript_CallRoutine(s, w, &e);
    printf("  call %s: %08lx\n", name, hr);
    perr(&e);
    flushlog();
    return hr;
}
static void getvar(IDirectMusicScript *s, const char *name)
{
    WCHAR w[128];
    VARIANT v;
    DMUS_SCRIPT_ERRORINFO e;
    einit(&e);
    MultiByteToWideChar(CP_ACP, 0, name, -1, w, 128);
    V_VT(&v) = 77;
    HRESULT hr = IDirectMusicScript_GetVariableVariant(s, w, &v, &e);
    printf("  %s = ", name);
    pvar(&v);
    printf(" (%08lx)", hr);
    LONG n = 12345;
    hr = IDirectMusicScript_GetVariableNumber(s, w, &n, NULL);
    printf(" number %ld (%08lx)\n", n, hr);
    perr(&e);
    VariantClear(&v);
}

/* ------------------------------------------------------------------ scripts made here */
static BYTE *put(BYTE *p, const void *d, DWORD n) { memcpy(p, d, n); return p + n; }
static BYTE *chunk(BYTE *p, const char *id, const void *d, DWORD n)
{
    p = put(p, id, 4);
    p = put(p, &n, 4);
    p = put(p, d, n);
    if (n & 1) *p++ = 0;
    return p;
}
/* a 'DMSC' form: flags, an empty container, the language and the source (\n becomes CRLF) */
static BYTE *make_script(const char *src, const WCHAR *lang, DWORD *size)
{
    size_t n = strlen(src);
    BYTE *b = calloc(1, 4096 + n * 6);
    WCHAR *w = calloc(n * 2 + 2, sizeof(WCHAR));
    size_t k = 0;
    for (const char *c = src; *c; c++) {
        if (*c == '\n') w[k++] = '\r';
        w[k++] = (BYTE)*c;
    }
    BYTE *p = b + 12;
    DWORD flags = 0, scve[2] = { 0x00080000, 0 };
    p = chunk(p, "schd", &flags, 4);
    p = chunk(p, "scve", scve, 8);
    BYTE cn[32], *q = cn;
    DWORD sz = 4 + 12 + 12;
    q = put(q, "RIFF", 4); q = put(q, &sz, 4); q = put(q, "DMCN", 4);
    q = chunk(q, "cnth", &flags, 4);
    sz = 4;
    q = put(q, "LIST", 4); q = put(q, &sz, 4); q = put(q, "cosl", 4);
    p = put(p, cn, (DWORD)(q - cn));
    p = chunk(p, "scla", lang, (lstrlenW(lang) + 1) * 2);
    p = chunk(p, "scsr", w, (DWORD)(k + 1) * 2);
    DWORD total = (DWORD)(p - b) - 8;
    memcpy(b, "RIFF", 4); memcpy(b + 4, &total, 4); memcpy(b + 8, "DMSC", 4);
    *size = (DWORD)(p - b);
    free(w);
    return b;
}
static IDirectMusicScript *load_mem(BYTE *b, DWORD n, HRESULT *hr)
{
    DMUS_OBJECTDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_MEMORY;
    d.guidClass = CLSID_DirectMusicScript;
    d.pbMemData = b;
    d.llMemLength = n;
    IDirectMusicScript *s = NULL;
    *hr = IDirectMusicLoader8_GetObject(loader, &d, &IID_IDirectMusicScript, (void **)&s);
    return s;
}
static IDirectMusicScript *script_init(const char *src, const WCHAR *lang)
{
    DWORD n;
    BYTE *b = make_script(src, lang, &n);
    HRESULT hr;
    IDirectMusicScript *s = load_mem(b, n, &hr);
    printf("  load %08lx\n", hr);
    if (!s) return NULL;
    DMUS_SCRIPT_ERRORINFO e;
    einit(&e);
    hr = IDirectMusicScript_Init(s, (IDirectMusicPerformance *)perf, &e);
    printf("  init %08lx\n", hr);
    perr(&e);
    IDirectMusicLoader8_ReleaseObjectByUnknown(loader, (IUnknown *)s);
    return s;          /* (b kept: the loader knows memory objects by their address) */
}

static const char *const s_syntax[] = {
    "",
    "\n\n\n",
    "Sub A\nEnd Sub\n",
    "Sub A\nEnd Sub",
    "  ' only a comment\n",
    "Dim x\nSub A\nx = 1\nEnd Sub\nDim y\n",
    "Sub\n",
    "Sub A B\nEnd Sub\n",
    "Sub A\nx = \nEnd Sub\n",
    "Sub A\nx = (1 + 2\nEnd Sub\n",
    "Sub A\nx = 1 + 2)\nEnd Sub\n",
    "Sub A\nx = \"a\" + 1\nEnd Sub\n",
    "Sub A\nx = 1 + \"a\"\nEnd Sub\n",
    "Dim x\nDim X\n",
    "Sub A\nEnd Sub\nSub a\nEnd Sub\n",
    "Dim A\nSub A\nEnd Sub\n",
    "Sub A\nIf 1 Then\nEnd Sub\n",
    "Sub A\nIf 1\nx=1\nEnd If\nEnd Sub\n",
    "Sub A\nIf 1 Then\nElse If 2 Then\nEnd If\nEnd Sub\n",
    "Sub A\nIf 1 Then x = 1\nEnd Sub\n",
    "Sub A\nTrue = 1\nEnd Sub\n",
    "Sub A\nset nothing = x\nEnd Sub\n",
    "Sub A\nTrace 1, 2)\nEnd Sub\n",
    "Sub A\nTrace (1, 2)\nEnd Sub\n",
    "Sub A\nx = A\nEnd Sub\n",
    "Sub A\nx = 99999999999\nEnd Sub\n",
    "Sub A\nx = 2147483648\nEnd Sub\n",
    "Sub A\nx = 2147483647\nEnd Sub\n",
    "Sub A\nx = \"abc\nEnd Sub\n",
    "Sub A\nx = 1 # 2\nEnd Sub\n",
    "Sub A\n  x = 1\nEnd Sub\nfoo\n",
    "Sub A\nSet x\nEnd Sub\n",
    "Sub A\nCall Trace\nEnd Sub\n",
    /* ("x = Rand(3": dmscript loses the error in a call's arguments and crashes running the routine) */
    "Sub A\n",
    "Sub A\nEnd\n",
    "Sub A\nEnd If\n",
    "Sub sub_x\nEnd Sub\n",
    "Sub A\nx = 1:y = 2\nEnd Sub\n",
    "Sub A\nx = \"\xe9\"\nEnd Sub\n",
    "Sub A\n\tx = 3 ' tab\n\nEnd Sub\n",
    "Sub A\nx = not\nEnd Sub\n",
    "Sub A\nx = 1 2\nEnd Sub\n",
    "Sub A\nx.y. = 1\nEnd Sub\n",
    "Sub A\nx = (\nEnd Sub\n",
    "Sub A\nEnd Sub\nDim z\n",
    "Dim x\n\nSub A\n\n  x = 1\n\nEnd Sub\n\n\n",
    "Sub A\nCall B(1)\nEnd Sub\nSub B\nEnd Sub\n",
    "Sub A\nCall B 1\nEnd Sub\n",
    "Sub A\nSetMasterVolume -100,\nEnd Sub\n",
    "Sub A\nx = Rand(A)\nEnd Sub\n",
    "Sub A\nx = Rand(A) + 1\nEnd Sub\n",
    "Sub A\nIf s = \"a\" Then\nEnd If\nEnd Sub\n",
    "Sub A\nelseif\nEnd Sub\n",
    "Sub A\nthen = 1\nEnd Sub\n",
    "Sub A\nx = 1 <> 2 <= 3 >= 4 < 5 > 6 = 7\nEnd Sub\n",
};
static char s_long[600];

static const char s_main[] =
    "' the main script\n"
    "Dim n\nDim s\nDim o\nDim r\nDim big\n"
    "Sub Arith\n"
    "  n = 7 \\ 2 + 3 * 4 - 2 ^ 3 Mod 5\n"
    "  r = -n\n"
    "  big = 2147483647 + 1\n"
    "End Sub\n"
    "Sub Arith2\n"
    "  n = -7 \\ 2\n  r = -7 Mod 2\n  big = 2 ^ 10 ^ 2\n"
    "End Sub\n"
    "Sub Arith3\n"
    "  n = (1 + 2) * (3 + 4)\n  r = Not 5 And 12 Or 1\n  big = - - 3\n"
    "End Sub\n"
    "Sub Arith4\n"
    "  n = 2 ^ -1\n  r = 3 ^ 0\n  big = (2 = 2) + (3 > 4)\n"
    "End Sub\n"
    "Sub Compare\n"
    "  If n > 5 And n < 100 Then\n    r = 1\n  ElseIf n = 5 Then\n    r = 2\n  Else\n    r = 3\n  End If\n"
    "End Sub\n"
    "Sub Compare2\n"
    "  n = 5\n  Compare\n  big = r\n  n = 0\n  Compare\n"
    "End Sub\n"
    "Sub Compare3\n"
    "  If 1 Then\n    r = 10\n  End If\n  If -1 Then\n    r = 11\n  End If\n"
    "End Sub\n"
    "Sub Strs\n  s = \"hello \"\"world\"\"\"\nEnd Sub\n"
    "Sub Locals\n  t = 5\n  n = t + q\nEnd Sub\n"
    "Sub Locals2\n  n = t\n  t = 9\nEnd Sub\n"
    "Sub DivZero\n  n = 1\n  n = 1 \\ 0\nEnd Sub\n"
    "Sub ModZero\n  n = 1 Mod 0\nEnd Sub\n"
    "Sub BadIf\n  s = \"x\"\n  If s Then\n  End If\nEnd Sub\n"
    "Sub BadIf2\n  Set o = Nothing\n  If o Then\n  End If\nEnd Sub\n"
    "Sub BadAdd\n  s = \"x\"\n  n = s + 1\nEnd Sub\n"
    "Sub BadNeg\n  s = \"x\"\n  n = -s\nEnd Sub\n"
    "Sub Unknown\n  NoSuchRoutine\nEnd Sub\n"
    "Sub NotObj\n  n = 5\n  n.Foo\nEnd Sub\n"
    "Sub NotObj2\n  n = 5\n  r = n.Foo\nEnd Sub\n"
    "Sub NoSet\n  o = Nothing\nEnd Sub\n"
    "Sub BadSet\n  Set o = 5\nEnd Sub\n"
    "Sub SetNothing\n  Set o = Nothing\n  If o Is Nothing Then\n    r = 77\n  End If\nEnd Sub\n"
    "Sub Nested\n  r = 0\n  Inner1\nEnd Sub\n"
    "Sub Inner1\n  r = r + 1\n  Inner2\nEnd Sub\n"
    "Sub Inner2\n  r = 1 \\ 0\nEnd Sub\n"
    "Sub WithArgs\n  Compare 1, 2\nEnd Sub\n"
    "Sub Perf\n"
    "  Trace \"hello\"\n  SetMasterVolume -500\n  SetMasterVolume -1234, 1000\n  SetMasterVolume 300\n"
    "  SetMasterVolume -20000\n  SetMasterTranspose 3\n  SetMasterTranspose -40000\n"
    "End Sub\n"
    "Sub PerfGet\n"
    "  n = GetMasterVolume\n  r = GetMasterTranspose\n  big = GetMasterTempo\n"
    "End Sub\n"
    "Sub Tempo\n"
    "  SetMasterTempo 150\n  n = GetMasterTempo\n  SetMasterTempo 0\n  r = GetMasterTempo\n"
    "  SetMasterTempo 100000\n  big = GetMasterTempo\n  SetMasterTempo 100\n"
    "End Sub\n"
    "Sub Groove\n"
    "  n = GetMasterGrooveLevel\n  SetMasterGrooveLevel 5\n  r = GetMasterGrooveLevel()\n"
    "  SetMasterGrooveLevel -300\n  big = GetMasterGrooveLevel\n  SetMasterGrooveLevel 0\n"
    "End Sub\n"
    "Sub Consts\n"
    "  n = AtMeasure + IsSecondary\n  r = NoCutoff\n  big = playendandintro\n"
    "End Sub\n"
    "Sub RandOk\n  t = Rand(10)\n  If t >= 1 And t <= 10 Then\n    r = 1\n  Else\n    r = 0\n  End If\n  big = Rand(1)\nEnd Sub\n"
    "Sub RandBad\n  n = Rand(0)\nEnd Sub\n"
    "Sub RandBad2\n  n = Rand(40000)\nEnd Sub\n"
    "Sub TraceNone\n  Trace\nEnd Sub\n"
    "Sub TraceTwo\n  Trace \"a\", \"b\"\nEnd Sub\n"
    "Sub TraceNum\n  Trace 5\nEnd Sub\n"
    "Sub VolNone\n  SetMasterVolume\nEnd Sub\n"
    "Sub VolOmit\n  SetMasterVolume , 5\nEnd Sub\n"
    "Sub TempoStr\n  SetMasterTempo \"x\"\nEnd Sub\n"
    "Sub SetConst\n  AtBeat = 5\nEnd Sub\n"
    "Sub ReadMethod\n  n = Trace\nEnd Sub\n"
    "Sub SetMethod\n  GetMasterTempo = 5\nEnd Sub\n"
    "Sub CallConst\n  AtBeat\nEnd Sub\n"
    "Sub CallParen\n  Call Trace(\"called\")\n  Call Arith()\nEnd Sub\n"
    "Sub Expr\n  n = Rand(1) + Rand(1) * 2\n  r = GetMasterVolume() - 1\nEnd Sub\n"
    "Sub StrCmp\n  s = \"a\"\n  t = s\n  If s = t Then\n    r = 1\n  End If\nEnd Sub\n"
    "Sub ObjCmp\n  Set o = Nothing\n  If o = 1 Then\n    r = 1\n  End If\nEnd Sub\n"
    "Sub IsNum\n  If 1 Is 1 Then\n    r = 1\n  End If\nEnd Sub\n"
    "Sub UseObj\n  o.Trace \"via object\"\nEnd Sub\n"
    "Sub UseObj2\n  n = o.GetMasterTempo\n  r = o.NoSuchThing\nEnd Sub\n"
    "Sub UseObj3\n  o.SetMasterTempo 120\n  o.Nope 1\nEnd Sub\n"
    "Sub ScriptCall\n  o.Arith\n  n = o.r\n  o.r = 99\nEnd Sub\n"
    "Sub ScriptCall2\n  o.DivZero\nEnd Sub\n"
    "Sub ScriptCall3\n  o.Load\n  o.n = 3\n  r = o.n\nEnd Sub\n"
    "Sub Empty\nEnd Sub\n";

static void globals(void)
{
    static const GUID other = { 0x12345678, 1, 2, { 3, 4, 5, 6, 7, 8, 9, 10 } };
    const GUID *gs[] = { &GUID_PerfMasterTempo, &GUID_PerfMasterVolume, &GUID_PerfMasterGrooveLevel, &GUID_PerfAutoDownload, &other };
    const char *nm[] = { "tempo", "volume", "groove", "autodl", "other" };
    printf("== global parameters\n");
    for (int k = 0; k < 5; k++) {
        BYTE b[8];
        memset(b, 0xcc, 8);
        HRESULT h4 = IDirectMusicPerformance8_GetGlobalParam(perf, gs[k], b, 4);
        HRESULT h1 = IDirectMusicPerformance8_GetGlobalParam(perf, gs[k], b + 4, 1);
        printf("  get %s: %08lx %02x%02x%02x%02x / %08lx %02x\n", nm[k], h4, b[0], b[1], b[2], b[3], h1, b[4]);
    }
    float t = 1.5f; long v = -300; char g = 3; DWORD d = 7, z = 0;
    printf("  set tempo 2: %08lx\n", IDirectMusicPerformance8_SetGlobalParam(perf, &GUID_PerfMasterTempo, &t, 2));
    printf("  set tempo: %08lx\n", IDirectMusicPerformance8_SetGlobalParam(perf, &GUID_PerfMasterTempo, &t, 4));
    printf("  set volume 8: %08lx\n", IDirectMusicPerformance8_SetGlobalParam(perf, &GUID_PerfMasterVolume, &v, 8));
    printf("  set volume: %08lx\n", IDirectMusicPerformance8_SetGlobalParam(perf, &GUID_PerfMasterVolume, &v, 4));
    printf("  set groove: %08lx\n", IDirectMusicPerformance8_SetGlobalParam(perf, &GUID_PerfMasterGrooveLevel, &g, 1));
    printf("  set groove 4: %08lx\n", IDirectMusicPerformance8_SetGlobalParam(perf, &GUID_PerfMasterGrooveLevel, &d, 4));
    printf("  set other 4: %08lx\n", IDirectMusicPerformance8_SetGlobalParam(perf, &other, &d, 4));
    printf("  set other 2: %08lx\n", IDirectMusicPerformance8_SetGlobalParam(perf, &other, &d, 2));
    printf("  set other 0: %08lx\n", IDirectMusicPerformance8_SetGlobalParam(perf, &other, &z, 0));
    t = 500.0f;
    printf("  set tempo 500: %08lx\n", IDirectMusicPerformance8_SetGlobalParam(perf, &GUID_PerfMasterTempo, &t, 4));
    for (int k = 0; k < 5; k++) {
        BYTE b[8];
        memset(b, 0xcc, 8);
        HRESULT h4 = IDirectMusicPerformance8_GetGlobalParam(perf, gs[k], b, 4);
        HRESULT h1 = IDirectMusicPerformance8_GetGlobalParam(perf, gs[k], b + 4, 1);
        printf("  get %s: %08lx %02x%02x%02x%02x / %08lx %02x\n", nm[k], h4, b[0], b[1], b[2], b[3], h1, b[4]);
    }
    t = 1.0f; v = 0; g = 0;
    IDirectMusicPerformance8_SetGlobalParam(perf, &GUID_PerfMasterTempo, &t, 4);
    IDirectMusicPerformance8_SetGlobalParam(perf, &GUID_PerfMasterVolume, &v, 4);
    IDirectMusicPerformance8_SetGlobalParam(perf, &GUID_PerfMasterGrooveLevel, &g, 1);
}

static void lang(void)
{
    globals();
    printf("== syntax\n");
    for (size_t k = 0; k < sizeof(s_syntax) / sizeof(*s_syntax); k++) {
        printf("case %u\n", (unsigned)k);
        IDirectMusicScript *s = script_init(s_syntax[k], L"AudioVBScript");
        if (!s) continue;
        enum_names(s);
        DMUS_SCRIPT_ERRORINFO e;
        einit(&e);
        HRESULT hr = IDirectMusicScript_CallRoutine(s, L"A", &e);
        printf("  call A %08lx\n", hr);
        perr(&e);
        IDirectMusicScript_Release(s);
    }
    printf("case long identifier\n");
    memset(s_long, 'a', sizeof(s_long));
    memcpy(s_long, "Sub ", 4);
    strcpy(s_long + 300, "\nEnd Sub\n");
    IDirectMusicScript *s = script_init(s_long, L"AudioVBScript");
    if (s) IDirectMusicScript_Release(s);
    printf("case long string\n");
    memset(s_long, 'b', sizeof(s_long));
    memcpy(s_long, "Sub A\nx = \"", 11);
    strcpy(s_long + 300, "\"\nEnd Sub\n");
    s = script_init(s_long, L"AudioVBScript");
    if (s) IDirectMusicScript_Release(s);
    printf("case 255 identifier\n");
    memset(s_long, 'c', sizeof(s_long));
    memcpy(s_long, "Sub ", 4);
    strcpy(s_long + 4 + 255, "\nEnd Sub\n");
    s = script_init(s_long, L"AudioVBScript");
    if (s) { enum_names(s); IDirectMusicScript_Release(s); }
    printf("case 256 identifier\n");
    strcpy(s_long + 4 + 256, "\nEnd Sub\n");
    s_long[4 + 255] = 'c';
    s = script_init(s_long, L"AudioVBScript");
    if (s) IDirectMusicScript_Release(s);
    printf("case language audiovbscript.1\n");
    s = script_init("Sub A\nEnd Sub\n", L"audiovbscript.1");
    if (s) IDirectMusicScript_Release(s);

    printf("== main\n");
    s = script_init(s_main, L"AudioVBScript");
    if (!s) return;
    enum_names(s);
    static const char *const routines[] = {
        "Arith", "Arith2", "Arith3", "Arith4", "Compare", "Compare2", "Compare3", "Strs", "Locals", "Locals2",
        "DivZero", "ModZero", "BadIf", "BadIf2", "BadAdd", "BadNeg", "Unknown", "NotObj", "NotObj2", "NoSet",
        "BadSet", "SetNothing", "Nested", "WithArgs", "Perf", "PerfGet", "Tempo", "Groove", "Consts", "RandOk",
        "RandBad", "RandBad2", "TraceNone", "TraceTwo", "TraceNum", "VolNone", "VolOmit", "TempoStr",
        "SetConst", "ReadMethod", "SetMethod", "CallConst", "CallParen", "Expr", "StrCmp", "ObjCmp", "IsNum",
        "Empty", "arith", "NoSuch", "n", "",
    };
    for (size_t k = 0; k < sizeof(routines) / sizeof(*routines); k++) {
        call(s, routines[k]);
        getvar(s, "n"); getvar(s, "r"); getvar(s, "big");
    }
    getvar(s, "s");
    getvar(s, "o");
    getvar(s, "t");
    getvar(s, "Arith");
    getvar(s, "true");
    getvar(s, "DirectMusic");
    getvar(s, "AtBeat");
    getvar(s, "nosuch");

    printf("== variables\n");
    DMUS_SCRIPT_ERRORINFO e;
    VARIANT v;
    HRESULT hr;
#define SHOW(what, x) do { einit(&e); hr = (x); printf("  %s: %08lx\n", what, hr); perr(&e); } while (0)
    SHOW("setnum n 42", IDirectMusicScript_SetVariableNumber(s, L"n", 42, &e));
    getvar(s, "n");
    SHOW("setnum N -7", IDirectMusicScript_SetVariableNumber(s, L"N", -7, &e));
    getvar(s, "n");
    SHOW("setnum nosuch", IDirectMusicScript_SetVariableNumber(s, L"nosuch", 1, &e));
    SHOW("setnum Arith", IDirectMusicScript_SetVariableNumber(s, L"Arith", 1, &e));
    SHOW("setnum true", IDirectMusicScript_SetVariableNumber(s, L"true", 1, &e));
    SHOW("setobj o perf", IDirectMusicScript_SetVariableObject(s, L"o", (IUnknown *)perf, &e));
    getvar(s, "o");
    IDirectMusicPerformance8 *p2 = NULL;
    SHOW("getobj o perf8", IDirectMusicScript_GetVariableObject(s, L"o", &IID_IDirectMusicPerformance8, (void **)&p2, &e));
    printf("    same %d\n", p2 == perf);
    if (p2) IDirectMusicPerformance8_Release(p2);
    IDirectMusicSegment8 *sg = NULL;
    SHOW("getobj o segment", IDirectMusicScript_GetVariableObject(s, L"o", &IID_IDirectMusicSegment8, (void **)&sg, &e));
    SHOW("getobj n", IDirectMusicScript_GetVariableObject(s, L"n", &IID_IUnknown, (void **)&sg, &e));
    call(s, "UseObj");
    call(s, "UseObj2");
    getvar(s, "n");
    call(s, "UseObj3");
    SHOW("setobj n null", IDirectMusicScript_SetVariableObject(s, L"n", NULL, &e));
    getvar(s, "n");
    /* (GetVariableObject of Nothing: dmscript queries a NULL object) */
    VariantInit(&v); V_VT(&v) = VT_UNKNOWN; V_UNKNOWN(&v) = (IUnknown *)perf;
    SHOW("setvariant n obj let", IDirectMusicScript_SetVariableVariant(s, L"n", v, FALSE, &e));
    VariantInit(&v); V_VT(&v) = VT_I4; V_I4(&v) = 3;
    SHOW("setvariant n 3 set", IDirectMusicScript_SetVariableVariant(s, L"n", v, TRUE, &e));
    VariantInit(&v); V_VT(&v) = VT_I2; V_I2(&v) = -5;
    SHOW("setvariant n i2", IDirectMusicScript_SetVariableVariant(s, L"n", v, FALSE, &e));
    getvar(s, "n");
    call(s, "Compare");
    VariantInit(&v); V_VT(&v) = VT_R8; V_R8(&v) = 2.5;
    SHOW("setvariant n r8", IDirectMusicScript_SetVariableVariant(s, L"n", v, FALSE, &e));
    getvar(s, "n");
    VariantInit(&v); V_VT(&v) = VT_BSTR; V_BSTR(&v) = SysAllocString(L"text");
    SHOW("setvariant s bstr", IDirectMusicScript_SetVariableVariant(s, L"s", v, FALSE, &e));
    VariantClear(&v);
    getvar(s, "s");
    VariantInit(&v); V_VT(&v) = VT_BOOL; V_BOOL(&v) = VARIANT_TRUE;
    SHOW("setvariant n bool", IDirectMusicScript_SetVariableVariant(s, L"n", v, FALSE, &e));
    getvar(s, "n");
    VariantInit(&v); V_VT(&v) = VT_EMPTY;
    SHOW("setvariant n empty", IDirectMusicScript_SetVariableVariant(s, L"n", v, FALSE, &e));
    getvar(s, "n");
    call(s, "Compare");

    printf("== script in a script\n");
    IDirectMusicScript *s2 = script_init(s_main, L"AudioVBScript");
    if (s2) {
        SHOW("setobj o script", IDirectMusicScript_SetVariableObject(s, L"o", (IUnknown *)s2, &e));
        call(s, "ScriptCall");
        getvar(s, "n");
        getvar(s2, "r");
        call(s, "ScriptCall2");
        call(s, "ScriptCall3");
        getvar(s2, "n");
        IDispatch *d = NULL;
        IDirectMusicScript_QueryInterface(s2, &IID_IDispatch, (void **)&d);
        if (d) {
            UINT tc = 99;
            hr = IDispatch_GetTypeInfoCount(d, &tc);
            printf("  dispatch typeinfo count %08lx %u\n", hr, tc);
            LPOLESTR names[] = { L"Arith", L"n", L"load", L"nosuch", L"true" };
            for (int k = 0; k < 5; k++) {
                DISPID id = 7;
                hr = IDispatch_GetIDsOfNames(d, &IID_NULL, &names[k], 1, 0x409, &id);
                printf("  dispatch id %ls %08lx %ld\n", names[k], hr, id);
            }
            IDispatch_Release(d);
        }
        IDirectMusicScript_Release(s2);
    }
    SHOW("setnum o 0", IDirectMusicScript_SetVariableNumber(s, L"o", 0, &e));

    printf("== uninitialized\n");
    DWORD n;
    BYTE *b = make_script(s_main, L"AudioVBScript", &n);
    IDirectMusicScript *s3 = load_mem(b, n, &hr);
    printf("  load %08lx\n", hr);
    if (s3) {
        WCHAR nm[300];
        SHOW("call", IDirectMusicScript_CallRoutine(s3, L"Arith", &e));
        SHOW("setnum", IDirectMusicScript_SetVariableNumber(s3, L"n", 1, &e));
        LONG x = 5;
        SHOW("getnum", IDirectMusicScript_GetVariableNumber(s3, L"n", &x, &e));
        SHOW("enumroutine", IDirectMusicScript_EnumRoutine(s3, 0, nm));
        SHOW("enumvariable", IDirectMusicScript_EnumVariable(s3, 0, nm));
        SHOW("init null", IDirectMusicScript_Init(s3, NULL, &e));
        SHOW("init", IDirectMusicScript_Init(s3, (IDirectMusicPerformance *)perf, &e));
        SHOW("init again", IDirectMusicScript_Init(s3, (IDirectMusicPerformance *)perf, &e));
        getvar(s3, "n");
        call(s3, "Arith");
        IDirectMusicObject *o = NULL;
        IDirectMusicScript_QueryInterface(s3, &IID_IDirectMusicObject, (void **)&o);
        if (o) {
            DMUS_OBJECTDESC d;
            memset(&d, 0, sizeof(d));
            hr = IDirectMusicObject_GetDescriptor(o, &d);
            printf("  desc %08lx valid %lx name '%ls' version %lx.%lx\n", hr, d.dwValidData, d.wszName,
                   d.vVersion.dwVersionMS, d.vVersion.dwVersionLS);
            IDirectMusicObject_Release(o);
        }
        IDirectMusicLoader8_ReleaseObjectByUnknown(loader, (IUnknown *)s3);
        IDirectMusicScript_Release(s3);
    }
    free(b);
    IDirectMusicScript_Release(s);

    printf("== errinfo sizes\n");
    s = script_init("Sub A\nx = 1 \\ 0\nEnd Sub\n", L"AudioVBScript");
    if (s) {
        DMUS_SCRIPT_ERRORINFO big;
        memset(&big, 0xcc, sizeof(big));
        big.dwSize = 16;
        hr = IDirectMusicScript_CallRoutine(s, L"A", &big);
        printf("  size 16: %08lx size %lu hr %08lx line %lu col %ld file[0] %04x\n", hr, big.dwSize, big.hr,
               big.ulLineNumber, big.ichCharPosition, big.wszSourceFile[0]);
        memset(&big, 0xcc, sizeof(big));
        big.dwSize = 0;
        hr = IDirectMusicScript_CallRoutine(s, L"A", &big);
        printf("  size 0: %08lx size %lu hr %08lx\n", hr, big.dwSize, big.hr);
        hr = IDirectMusicScript_CallRoutine(s, L"A", NULL);
        printf("  null: %08lx\n", hr);
        IDirectMusicScript_Release(s);
    }
}

/* ------------------------------------------------------------------ script files */
static IDirectMusicScript *load_file(const WCHAR *name, HRESULT *hr)
{
    IDirectMusicScript *s = NULL;
    *hr = IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicScript, &IID_IDirectMusicScript, (WCHAR *)name, (void **)&s);
    return s;
}
static void files(void)
{
    static const WCHAR *const names[] = { L"synerr.spt", L"rterr.spt", L"nosrc.spt", L"vbs.spt", L"nolang.spt",
                                          L"content1m.spt" };
    DMUS_SCRIPT_ERRORINFO e;
    HRESULT hr;
    for (size_t k = 0; k < sizeof(names) / sizeof(*names); k++) {
        printf("== %ls\n", names[k]);
        IDirectMusicScript *s = load_file(names[k], &hr);
        printf("  load %08lx\n", hr);
        if (!s) continue;
        einit(&e);
        hr = IDirectMusicScript_Init(s, (IDirectMusicPerformance *)perf, &e);
        printf("  init %08lx\n", hr);
        perr(&e);
        IDirectMusicObject *o = NULL;
        IDirectMusicScript_QueryInterface(s, &IID_IDirectMusicObject, (void **)&o);
        if (o) {
            DMUS_OBJECTDESC d;
            memset(&d, 0, sizeof(d));
            hr = IDirectMusicObject_GetDescriptor(o, &d);
            printf("  desc %08lx valid %lx name '%ls' file '%ls'\n", hr, d.dwValidData, d.wszName, d.wszFileName);
            IDirectMusicObject_Release(o);
        }
        call(s, "X");
        call(s, "Y");
        IDirectMusicScript_Release(s);
    }
    static const WCHAR *const content[] = { L"content0.spt", L"content1.spt", L"content3.spt" };
    for (size_t k = 0; k < sizeof(content) / sizeof(*content); k++) {
        printf("== %ls\n", content[k]);
        IDirectMusicScript *s = load_file(content[k], &hr);
        printf("  load %08lx\n", hr);
        if (!s) continue;
        einit(&e);
        hr = IDirectMusicScript_Init(s, (IDirectMusicPerformance *)perf, &e);
        printf("  init %08lx\n", hr);
        perr(&e);
        enum_names(s);
        static const WCHAR *const objs[] = { L"Tune", L"lazy", L"Inner", L"Missing" };
        for (int j = 0; j < 4; j++) {
            VARIANT v;
            einit(&e);
            hr = IDirectMusicScript_GetVariableVariant(s, (WCHAR *)objs[j], &v, &e);
            printf("  get %ls %08lx ", objs[j], hr);
            pvar(&v);
            printf("\n");
            VariantClear(&v);
            IUnknown *u = NULL;
            hr = IDirectMusicScript_GetVariableObject(s, (WCHAR *)objs[j], &IID_IDirectMusicSegment8, (void **)&u, NULL);
            printf("    as segment %08lx\n", hr);
            if (u) IUnknown_Release(u);
            hr = IDirectMusicScript_GetVariableObject(s, (WCHAR *)objs[j], &IID_IDispatch, (void **)&u, NULL);
            printf("    as dispatch %08lx\n", hr);
            if (u) IUnknown_Release(u);
            hr = IDirectMusicScript_GetVariableObject(s, (WCHAR *)objs[j], &IID_IDirectMusicScript, (void **)&u, NULL);
            printf("    as script %08lx\n", hr);
            if (u) IUnknown_Release(u);
            einit(&e);
            hr = IDirectMusicScript_SetVariableNumber(s, (WCHAR *)objs[j], 1, &e);
            printf("    set %08lx\n", hr);
        }
        call(s, "LoadOne");
        IUnknown *u = NULL;
        hr = IDirectMusicScript_GetVariableObject(s, L"Lazy", &IID_IDirectMusicSegment8, (void **)&u, NULL);
        printf("  lazy as segment now %08lx\n", hr);
        if (u) IUnknown_Release(u);
        call(s, "LoadTwice");
        call(s, "GetSeg");
        getvar(s, "Obj");
        call(s, "IsSame");
        getvar(s, "Result");
        call(s, "SetSeg");
        call(s, "CallInner");
        getvar(s, "Result");
        call(s, "LoadMissing");
        IDirectMusicScript_Release(s);
    }
}


/* ------------------------------------------------------------------ automation: segments, states, audiopaths */
static void playing(IDirectMusicScript *s)
{
    IDirectMusicSegment8 *t = NULL, *sh = NULL;
    IDirectMusicScript_GetVariableObject(s, L"Tune", &IID_IDirectMusicSegment8, (void **)&t, NULL);
    IDirectMusicScript_GetVariableObject(s, L"Short", &IID_IDirectMusicSegment8, (void **)&sh, NULL);
    printf("    playing: tune %08lx short %08lx\n",
           t ? IDirectMusicPerformance8_IsPlaying(perf, (IDirectMusicSegment *)t, NULL) : 0xdead,
           sh ? IDirectMusicPerformance8_IsPlaying(perf, (IDirectMusicSegment *)sh, NULL) : 0xdead);
    if (t) IDirectMusicSegment8_Release(t);
    if (sh) IDirectMusicSegment8_Release(sh);
}
static void autotest(void)
{
    HRESULT hr;
    g_quiet = 1;
    IDirectMusicScript *s = NULL;
    hr = IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicScript, &IID_IDirectMusicScript, L"auto.spt", (void **)&s);
    printf("load %08lx\n", hr);
    if (!s) return;
    DMUS_SCRIPT_ERRORINFO e;
    einit(&e);
    hr = IDirectMusicScript_Init(s, (IDirectMusicPerformance *)perf, &e);
    printf("init %08lx\n", hr);
    perr(&e);
    /* ("!": the segments' playing state, checked only after immediate plays and stops: reAL's clock runs fast
     * in the test setup, so anything waiting for a boundary would compare timing) */
    static const char *const steps[] = {
        "Loads", "PlayAgain", "-", "!", "IsIt", "PlaySecondary", "-", "IsIt", "StopStateNow", "-", "!", "IsIt",
        "PlayControl", "PlayAligned", "PlayQueued", "StopSegNow", "-", "!", "IsIt",
        "PlayAgain", "-", "DoFill", "-", "DoEnd", "-", "StopSegNow", "-", "!",
        "PlayIt", "PlayFrom", "-", "IsIt", "PlayNoCutoff", "-", "!", "PlayTransition", "-", "IsIt", "StopState",
        "StopSeg", "StopSegNow", "-", "!", "PlayBad", "PlayBadPath", "PlayBadFlags",
        "MakePath", "PathPlay", "-", "!", "IsIt", "StopSegNow", "-", "!", "Download", "Recomp", "NothingState",
        "BadMethod", "CfgVolume", NULL };
    for (int k = 0; steps[k]; k++) {
        if (!strcmp(steps[k], "-")) { Sleep(400); continue; }
        if (!strcmp(steps[k], "!")) { playing(s); continue; }
        call(s, steps[k]);
        getvar(s, "R");
        getvar(s, "V");
    }
    getvar(s, "SS");
    getvar(s, "P");
    IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
    Sleep(200);
    IDirectMusicScript_Release(s);
}

/* ------------------------------------------------------------------ VBScript and JScript (Active Scripting) */
static const char s_vbs[] =
    "Dim n, s, d, o, arr\n"
    "Sub Basics\n"
    "  n = 7 \\ 2 + 3 * 4\n  s = \"hello \" & n\n  d = 1.5 * 3\n  arr = Array(1, 2, 3)\n"
    "End Sub\n"
    "Sub Perf\n"
    "  Trace \"from vbscript \" & n\n  SetMasterVolume -500\n  SetMasterTranspose 2\n  n = GetMasterVolume\n"
    "  d = GetMasterTranspose()\n  SetMasterTranspose 0\n  SetMasterVolume 0\n"
    "End Sub\n"
    "Sub Consts\n  n = AtMeasure + IsSecondary\n  s = TypeName(AtBeat)\nEnd Sub\n"
    "Sub Strs\n  Trace \"x\" & 5\n  SetMasterTempo \"150\"\n  n = GetMasterTempo\n  SetMasterTempo 100\nEnd Sub\n"
    "Sub DivZero\n  n = 1 / 0\nEnd Sub\n"
    "Sub BadCall\n  NoSuchThing 1\nEnd Sub\n"
    "Sub BadArgs\n  Trace\nEnd Sub\n"
    "Sub RandBad\n  n = Rand(0)\nEnd Sub\n"
    "Sub WithArg(x)\n  n = x\nEnd Sub\n"
    "Function Twice(x)\n  Twice = x * 2\nEnd Function\n"
    "Sub UseObj\n  o.Trace \"via object\"\nEnd Sub\n"
    "Sub Nested\n  Basics\n  n = Twice(n)\nEnd Sub\n"
    "Sub ScriptCall\n  o.Basics\n  n = o.n\n  o.n = 99\nEnd Sub\n"
    "Sub ScriptErr\n  o.DivZero\nEnd Sub\n";
static const char s_js[] =
    "var n = 0, s = 'init';\n"
    "function Basics() { n = 6 * 7; s = 'js ' + n; }\n"
    "function Perf() { Trace('from jscript ' + n); SetMasterVolume(-300); n = GetMasterVolume(); SetMasterVolume(0); }\n"
    "function Consts() { n = AtMeasure | IsSecondary; }\n"
    "function Thrower() { throw new Error('boom'); }\n"
    "function BadCall() { NoSuchThing(1); }\n";
static void ole_one(const char *what, const char *src, const WCHAR *lang, const char *const *routines,
                    const char *const *vars, IDirectMusicScript **keep)
{
    printf("== %s\n", what);
    IDirectMusicScript *s = script_init(src, lang);
    if (!s) return;
    enum_names(s);
    for (int k = 0; routines[k]; k++) call(s, routines[k]);
    for (int k = 0; vars[k]; k++) getvar(s, vars[k]);
    if (keep) *keep = s; else IDirectMusicScript_Release(s);
}
static void oletest(void)
{
    static const char *const vbs_r[] = { "Basics", "Perf", "Consts", "Strs", "DivZero", "BadCall", "BadArgs", "RandBad",
                                         "WithArg", "Twice", "Nested", "NoSuch", NULL };
    static const char *const vbs_v[] = { "n", "s", "d", "arr", "o", "nosuch", "Basics", NULL };
    IDirectMusicScript *vb = NULL, *js = NULL;
    ole_one("vbscript", s_vbs, L"VBScript", vbs_r, vbs_v, &vb);
    static const char *const js_r[] = { "Basics", "Perf", "Consts", "Thrower", "BadCall", NULL };
    static const char *const js_v[] = { "n", "s", NULL };
    ole_one("jscript", s_js, L"JScript", js_r, js_v, &js);
    if (vb) {
        DMUS_SCRIPT_ERRORINFO e;
        HRESULT hr;
        printf("== variables\n");
        einit(&e); hr = IDirectMusicScript_SetVariableNumber(vb, L"n", 42, &e); printf("  setnum n: %08lx\n", hr); perr(&e);
        getvar(vb, "n");
        VARIANT v;
        VariantInit(&v); V_VT(&v) = VT_BSTR; V_BSTR(&v) = SysAllocString(L"12");
        einit(&e); hr = IDirectMusicScript_SetVariableVariant(vb, L"s", v, FALSE, &e); printf("  setvariant s '12': %08lx\n", hr); perr(&e);
        VariantClear(&v);
        getvar(vb, "s");
        VariantInit(&v); V_VT(&v) = VT_R8; V_R8(&v) = 2.75;
        einit(&e); hr = IDirectMusicScript_SetVariableVariant(vb, L"d", v, FALSE, &e); printf("  setvariant d r8: %08lx\n", hr); perr(&e);
        getvar(vb, "d");
        einit(&e); hr = IDirectMusicScript_SetVariableObject(vb, L"o", (IUnknown *)perf, &e); printf("  setobj o perf: %08lx\n", hr); perr(&e);
        call(vb, "UseObj");
        VariantInit(&v); V_VT(&v) = VT_UNKNOWN; V_UNKNOWN(&v) = (IUnknown *)perf;
        einit(&e); hr = IDirectMusicScript_SetVariableVariant(vb, L"n", v, FALSE, &e); printf("  setvariant n obj let: %08lx\n", hr); perr(&e);
        VariantInit(&v); V_VT(&v) = VT_I4; V_I4(&v) = 1;
        einit(&e); hr = IDirectMusicScript_SetVariableVariant(vb, L"n", v, TRUE, &e); printf("  setvariant n 1 set: %08lx\n", hr); perr(&e);
        einit(&e); hr = IDirectMusicScript_SetVariableNumber(vb, L"Basics", 1, &e); printf("  setnum Basics: %08lx\n", hr); perr(&e);
        einit(&e); hr = IDirectMusicScript_SetVariableNumber(vb, L"nosuch", 1, &e); printf("  setnum nosuch: %08lx\n", hr); perr(&e);
        printf("== script to script\n");
        IDirectMusicScript *vb2 = script_init(s_vbs, L"VBScript");
        if (vb2) {
            einit(&e); hr = IDirectMusicScript_SetVariableObject(vb, L"o", (IUnknown *)vb2, &e); printf("  setobj o script: %08lx\n", hr);
            call(vb, "ScriptCall");
            getvar(vb, "n");
            getvar(vb2, "n");
            call(vb, "ScriptErr");
            IDirectMusicScript_SetVariableNumber(vb, L"o", 0, NULL);
            IDirectMusicScript_Release(vb2);
        }
        if (js) {
            einit(&e); hr = IDirectMusicScript_SetVariableObject(vb, L"o", (IUnknown *)js, &e); printf("  setobj o jscript: %08lx\n", hr);
            call(vb, "ScriptCall");
            getvar(vb, "n");
            getvar(js, "n");
            IDirectMusicScript_SetVariableNumber(vb, L"o", 0, NULL);
        }
        IDirectMusicScript_Release(vb);
    }
    if (js) IDirectMusicScript_Release(js);
    static const char *const syn[] = {
        "Sub A\n  x = \nEnd Sub\n", "Sub A(\nEnd Sub\n", "Dim x\nx = 1 +\n", "Sub A\n  If 1 Then\nEnd Sub\n",
        "Sub A\nEnd Sub\nSub A\nEnd Sub\n", "Trace \"at load\"\nSub A\nEnd Sub\n", "Dim q\nq = 1 / 0\nSub A\nEnd Sub\n", NULL };
    for (int k = 0; syn[k]; k++) {
        char b[16];
        snprintf(b, sizeof(b), "vbs syntax %d", k);
        printf("== %s\n", b);
        IDirectMusicScript *x = script_init(syn[k], L"VBScript");
        if (x) { call(x, "A"); IDirectMusicScript_Release(x); }
        flushlog();
    }
    printf("== js syntax\n");
    IDirectMusicScript *x = script_init("function A( { }\n", L"JScript");
    if (x) IDirectMusicScript_Release(x);
}

/* ------------------------------------------------------------------ the script track */
static const GUID G_ScriptTrack = { 0x4108fa85, 0x3586, 0x11d3, { 0x8b, 0xd7, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xb6 } };
static const GUID G_ScriptNotify = { 0x1cc7e0bf, 0x981c, 0x4b9f, { 0xbe, 0x17, 0xd5, 0x72, 0xfc, 0x5f, 0xa9, 0x33 } };
static IDirectMusicSegment8 *load_seg(const WCHAR *name, HRESULT *hr)
{
    IDirectMusicSegment8 *g = NULL;
    *hr = IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, (WCHAR *)name, (void **)&g);
    return g;
}
static void play_through(IDirectMusicSegment8 *g)
{
    IDirectMusicSegmentState *ss = NULL;
    HRESULT hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)g, NULL, NULL, 0, 0, &ss, NULL, NULL);
    printf("  play %08lx\n", hr);
    /* (until it has played: IsPlaying may answer S_FALSE until it starts) */
    for (int k = 0, seen = 0; k < 300; k++) {
        HRESULT p = IDirectMusicPerformance8_IsPlaying(perf, (IDirectMusicSegment *)g, NULL);
        if (p == S_OK) seen = 1;
        else if (seen || k > 60) break;
        Sleep(50);
    }
    Sleep(300);
    flushlog();
    if (ss) IDirectMusicSegmentState_Release(ss);
}
static void tracktest(void)
{
    HRESULT hr;
    g_quiet = 1;
    IDirectMusicSegment8 *g = load_seg(L"trk.sgt", &hr);
    printf("== trk.sgt\n  load %08lx\n", hr);
    if (!g) return;
    IDirectMusicTrack *t = NULL;
    hr = IDirectMusicSegment8_GetTrack(g, &G_ScriptTrack, 0xffffffff, 0, &t);
    printf("  gettrack %08lx\n", hr);
    if (t) {
        printf("  isparam notify %08lx tempo %08lx\n", IDirectMusicTrack_IsParamSupported(t, &G_ScriptNotify),
               IDirectMusicTrack_IsParamSupported(t, &GUID_TempoParam));
        struct { MUSIC_TIME mt; double tempo; } tp = { 0, 100.0 };
        MUSIC_TIME nx = 0;
        printf("  setparam tempo %08lx getparam notify %08lx tempo %08lx\n", IDirectMusicTrack_SetParam(t, &GUID_TempoParam, 0, &tp),
               IDirectMusicTrack_GetParam(t, &G_ScriptNotify, 0, &nx, NULL), IDirectMusicTrack_GetParam(t, &GUID_TempoParam, 0, &nx, &tp));
        IDirectMusicTrack8 *t8 = NULL;
        hr = IDirectMusicTrack_QueryInterface(t, &IID_IDirectMusicTrack8, (void **)&t8);
        printf("  track8 %08lx\n", hr);
        if (t8) IDirectMusicTrack8_Release(t8);
    }
    printf(" without notification\n");
    play_through(g);
    /* (not played with the notification: dmscript crashes sending a routine's runtime error, 0040416a) */
    hr = IDirectMusicSegment8_SetParam(g, &G_ScriptNotify, 0xffffffff, 0, 0, NULL);
    printf(" notification: setparam %08lx\n", hr);
    IDirectMusicScript *s = load_file(L"trk.spt", &hr);
    printf("  script %08lx\n", hr);
    if (s) { getvar(s, "N"); IDirectMusicScript_Release(s); }
    /* a clone: the events in [768, 3500) */
    IDirectMusicSegment *c = NULL;
    hr = IDirectMusicSegment8_Clone(g, 768, 3500, &c);
    printf("== clone %08lx\n", hr);
    if (c) {
        IDirectMusicSegment8 *c8 = NULL;
        IDirectMusicSegment_QueryInterface(c, &IID_IDirectMusicSegment8, (void **)&c8);
        if (c8) { play_through(c8); IDirectMusicSegment8_Release(c8); }
        IDirectMusicSegment_Release(c);
    }
    if (t) {
        IDirectMusicTrack *tc = NULL;
        hr = IDirectMusicTrack_Clone(t, -5, -10, &tc);
        printf("  track clone (-5, -10) %08lx\n", hr);
        if (tc) IDirectMusicTrack_Release(tc);
        IDirectMusicTrack_Release(t);
    }
    IDirectMusicSegment8_Release(g);

    printf("== trkbad.sgt\n");
    g = load_seg(L"trkbad.sgt", &hr);
    printf("  load %08lx\n", hr);
    if (g) {
        play_through(g);
        hr = IDirectMusicSegment8_SetParam(g, &G_ScriptNotify, 0xffffffff, 0, 0, NULL);
        printf(" with notification (setparam %08lx)\n", hr);
        play_through(g);
        IDirectMusicSegment8_Release(g);
    }
    printf("== trkvbs.sgt\n");     /* (a VBScript script: started by the track, its code calling the performance) */
    g = load_seg(L"trkvbs.sgt", &hr);
    printf("  load %08lx\n", hr);
    if (g) { play_through(g); play_through(g); IDirectMusicSegment8_Release(g); }
    static const WCHAR *const bad[] = { L"noname.sgt", L"nohdr.sgt", L"noref.sgt", L"missing.sgt", L"empty.sgt" };
    for (size_t k = 0; k < sizeof(bad) / sizeof(*bad); k++) {
        g = load_seg(bad[k], &hr);
        printf("== %ls\n  load %08lx\n", bad[k], hr);
        if (!g) continue;
        t = NULL;
        hr = IDirectMusicSegment8_GetTrack(g, &G_ScriptTrack, 0xffffffff, 0, &t);
        printf("  gettrack %08lx\n", hr);
        if (t) IDirectMusicTrack_Release(t);
        IDirectMusicSegment8_Release(g);
    }
    IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
    Sleep(200);
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("scrtest <dir> lang|files\n"); return 1; }
    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&logcs);
    LoadLibraryA("dsound.dll");     /* (reAL's COM hooks) */
    CoInitialize(NULL);
    toolobj.lpVtbl = &tvtbl;
    HRESULT hr = CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf);
    if (FAILED(hr)) { printf("perf %08lx\n", hr); return 1; }
    hr = IDirectMusicPerformance8_InitAudio(perf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, DMUS_AUDIOF_ALL, NULL);
    printf("initaudio %08lx\n", hr);
    IDirectMusicGraph *g = NULL;
    hr = CoCreateInstance(&CLSID_DirectMusicGraph, NULL, CLSCTX_INPROC, &IID_IDirectMusicGraph, (void **)&g);
    if (SUCCEEDED(hr)) {
        IDirectMusicGraph_InsertTool(g, &toolobj, NULL, 0, 0);
        IDirectMusicPerformance8_SetGraph(perf, g);
        IDirectMusicGraph_Release(g);
    }
    hr = CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&loader);
    if (FAILED(hr)) { printf("loader %08lx\n", hr); return 1; }
    WCHAR dir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, dir, MAX_PATH);
    IDirectMusicLoader8_SetSearchDirectory(loader, &GUID_DirectMusicAllTypes, dir, FALSE);
    if (!strcmp(argv[2], "lang")) lang();
    else if (!strcmp(argv[2], "files")) files();
    else if (!strcmp(argv[2], "auto")) autotest();
    else if (!strcmp(argv[2], "ole")) oletest();
    else if (!strcmp(argv[2], "track")) tracktest();
    IDirectMusicLoader8_Release(loader);
    IDirectMusicPerformance8_CloseDown(perf);
    IDirectMusicPerformance8_Release(perf);
    CoUninitialize();
    printf("done\n");
    return 0;
}
