/* dmtrace - DirectMusic API tracer.
 *
 * DirectMusic is reached through COM (CoCreateInstance), so instead of standing in for a DLL by name, dmtrace
 * registers itself as the in-process server of every DirectMusic class for the current user:
 *     regsvr32 dmtrace.dll        install  (writes HKCU\Software\Classes\CLSID\{...}\InprocServer32; no admin,
 *                                           Windows' own registration is not touched)
 *     regsvr32 /u dmtrace.dll     uninstall
 * (on 64-bit Windows use the 32-bit regsvr32 in SysWOW64; under Wine: wine regsvr32 dmtrace.dll).
 * DllGetClassObject then loads the real server (the path registered in HKLM, or target_dir) and logs every call
 * the game makes on the objects it gets.  Objects are traced by swapping their vtable pointer to a logging copy,
 * so the implementation keeps seeing its own objects.  Calls the DirectMusic DLLs make among themselves are not
 * logged unless level=3.
 *
 * dmtrace.ini next to the dll (all optional):
 *   level=1           1: everything but per-frame calls; 2: also AddRef/Release/GetTime/GetParam/...;
 *                     3: also the DirectMusic DLLs' (and other Windows DLLs') internal calls
 *   target_dir=C:\dm  load dmime.dll, dmusic.dll ... from this directory instead of the registered ones
 *   logfile=...       default: dmtrace.log next to the game's exe
 * Environment: DMTRACE_LEVEL, DMTRACE_TARGET_DIR.
 *
 * Note: Wine's builtin DirectMusic checks object vtables, so trace Microsoft's DLLs (target_dir) under Wine. */
#include "dmtrace.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

static HMODULE g_self;
static int g_level = 1;
static FILE *g_log;
static CRITICAL_SECTION g_cs;
static char g_target_dir[MAX_PATH];
static char g_self_path[MAX_PATH];

/* ======================================================================== setup / logging */

static void init_once(void)
{
    static LONG done;
    if (InterlockedExchange(&done, 1)) return;
    char dir[MAX_PATH], ini[MAX_PATH], buf[MAX_PATH], logpath[MAX_PATH];
    GetModuleFileNameA(g_self, g_self_path, MAX_PATH);
    strcpy(dir, g_self_path);
    char *s = strrchr(dir, '\\'); if (s) s[1] = 0; else dir[0] = 0;
    snprintf(ini, sizeof(ini), "%sdmtrace.ini", dir);
    g_level = GetPrivateProfileIntA("dmtrace", "level", 1, ini);
    GetPrivateProfileStringA("dmtrace", "target_dir", "", g_target_dir, MAX_PATH, ini);
    if (GetEnvironmentVariableA("DMTRACE_LEVEL", buf, sizeof(buf))) g_level = atoi(buf);
    if (GetEnvironmentVariableA("DMTRACE_TARGET_DIR", buf, sizeof(buf))) strcpy(g_target_dir, buf);
    GetPrivateProfileStringA("dmtrace", "logfile", "", logpath, MAX_PATH, ini);
    if (!logpath[0]) {
        GetModuleFileNameA(NULL, logpath, MAX_PATH);
        s = strrchr(logpath, '\\'); if (s) s[1] = 0; else logpath[0] = 0;
        strcat(logpath, "dmtrace.log");
    }
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    const char *eb = strrchr(exe, '\\'); eb = eb ? eb + 1 : exe;
    if (!lstrcmpiA(eb, "regsvr32.exe")) snprintf(logpath, sizeof(logpath), "%sdmtrace.log", dir);
    g_log = fopen(logpath, "a");
    if (!g_log) { snprintf(logpath, sizeof(logpath), "%sdmtrace.log", dir); g_log = fopen(logpath, "a"); }
    if (!g_log && GetTempPathA(MAX_PATH, buf)) { snprintf(logpath, sizeof(logpath), "%sdmtrace.log", buf); g_log = fopen(logpath, "a"); }
    SYSTEMTIME t; GetLocalTime(&t);
    if (g_log)
        fprintf(g_log, "==== dmtrace %04d-%02d-%02d %02d:%02d:%02d  exe=%s  level=%d  target_dir=%s\n", t.wYear,
                t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, exe, g_level, g_target_dir[0] ? g_target_dir : "(registry)");
}

static void logline(const char *s)
{
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    fprintf(g_log, "%8lu %04lx %s\n", (unsigned long)GetTickCount(), (unsigned long)GetCurrentThreadId(), s);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

static void logf(const char *fmt, ...)
{
    char b[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    logline(b);
}

/* ======================================================================== real modules (caller filter) */

static struct { uintptr_t lo, hi; } g_mods[16];
static int g_nmods;

static void add_module(HMODULE h)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)h;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)h + dos->e_lfanew);
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nmods; i++) if (g_mods[i].lo == (uintptr_t)h) { LeaveCriticalSection(&g_cs); return; }
    if (g_nmods < 16) { if (g_level >= 2) logf("module %p..%p", (void *)h, (void *)((uintptr_t)h + nt->OptionalHeader.SizeOfImage)); g_mods[g_nmods].lo = (uintptr_t)h; g_mods[g_nmods].hi = (uintptr_t)h + nt->OptionalHeader.SizeOfImage; g_nmods++; }
    LeaveCriticalSection(&g_cs);
}

/* callers inside Windows' own DLLs (dsound's mixer threads polling the clock, ...) are not the game */
static int in_windows_dir(void *ra)
{
    static struct { HMODULE m; int sys; } cache[64];
    static int ncache;
    static char windir[MAX_PATH];
    static size_t wl;
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ra, &m))
        return 0;
    for (int i = 0; i < ncache; i++) if (cache[i].m == m) return cache[i].sys;
    if (!wl) { GetSystemWindowsDirectoryA(windir, MAX_PATH); strcat(windir, "\\"); wl = strlen(windir); }
    char path[MAX_PATH];
    int sys = m != GetModuleHandleA(NULL) && GetModuleFileNameA(m, path, MAX_PATH) && !_strnicmp(path, windir, wl);
    if (ncache < 64) { cache[ncache].m = m; cache[ncache].sys = sys; ncache++; }
    return sys;
}

int dt_want(void *ra, int level)
{
    if (!g_log || g_level < level) return 0;
    if (g_level >= 3) return 1;
    uintptr_t a = (uintptr_t)ra;
    for (int i = 0; i < g_nmods; i++) if (a >= g_mods[i].lo && a < g_mods[i].hi) return 0;
    return !in_windows_dir(ra);
}

/* ======================================================================== vtable copies */

#define DT_MAGIC 0x444d5452u   /* 'DMTR' */
#define DT_PAD 32               /* raw slots copied past the known methods */
typedef struct dt_vt {
    uint32_t magic;
    struct dt_vt *self;
    const dt_desc *desc;
    void **orig;
    void *rtti;
    void *fns[];
} dt_vt;

static struct { void **orig; dt_vt *copy; } g_vts[512];
static int g_nvts;

static dt_vt *vt_of(void *obj)
{
    void **v = *(void ***)obj;
    if (!v) return NULL;
    dt_vt *b = (dt_vt *)((BYTE *)v - offsetof(dt_vt, fns));
    if (b->magic == DT_MAGIC && b->self == b) return b;
    return NULL;
}

void *dt_orig(void *obj, int idx) { return vt_of(obj)->orig[idx]; }

void dt_hook(void *obj, const dt_desc *d)
{
    if (!obj || !d) return;
    EnterCriticalSection(&g_cs);
    if (vt_of(obj)) { LeaveCriticalSection(&g_cs); return; }
    void **orig = *(void ***)obj;
    dt_vt *c = NULL;
    for (int i = 0; i < g_nvts; i++) if (g_vts[i].orig == orig) { c = g_vts[i].copy; break; }
    if (!c && g_nvts < 512) {
        c = VirtualAlloc(NULL, sizeof(dt_vt) + (d->n + DT_PAD) * sizeof(void *), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (c) {
            c->magic = DT_MAGIC; c->self = c; c->desc = d; c->orig = orig; c->rtti = orig[-1];
            for (int i = 0; i < d->n; i++) c->fns[i] = d->wrap[i];
            MEMORY_BASIC_INFORMATION mi;
            for (int i = d->n; i < d->n + DT_PAD; i++) {      /* methods we do not know: call through */
                void **p = &orig[i];
                if (!VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) break;
                c->fns[i] = *p;
            }
            g_vts[g_nvts].orig = orig; g_vts[g_nvts].copy = c; g_nvts++;
        }
    }
    if (c) InterlockedExchangePointer((void **)obj, c->fns);
    LeaveCriticalSection(&g_cs);
}

void dt_hook_iid(void *obj, const GUID *iid)
{
    if (!obj || !iid) return;
    for (int i = 0; dt_iids[i].iid; i++)
        if (IsEqualGUID(dt_iids[i].iid, iid)) { dt_hook(obj, dt_iids[i].desc); return; }
}

/* ======================================================================== object ids */

static struct { void *p; int id; } g_ids[4096];
static int g_nids, g_next_id = 1;

static int obj_id(void *p)
{
    EnterCriticalSection(&g_cs);
    int id = 0;
    for (int i = 0; i < g_nids; i++) if (g_ids[i].p == p) { id = g_ids[i].id; break; }
    if (!id) {
        id = g_next_id++;
        if (g_nids < 4096) { g_ids[g_nids].p = p; g_ids[g_nids].id = id; g_nids++; }
        else { memmove(g_ids, g_ids + 1, sizeof(g_ids[0]) * 4095); g_ids[4095].p = p; g_ids[4095].id = id; }
    }
    LeaveCriticalSection(&g_cs);
    return id;
}

void dt_forget(void *p)
{
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nids; i++) if (g_ids[i].p == p) { g_ids[i] = g_ids[--g_nids]; break; }
    LeaveCriticalSection(&g_cs);
}

/* ======================================================================== formatting */

static void app(dt_line *L, const char *fmt, ...)
{
    if (L->n >= (int)sizeof(L->b) - 1) return;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(L->b + L->n, sizeof(L->b) - L->n, fmt, ap);
    va_end(ap);
    if (k > 0) L->n += k;
    if (L->n > (int)sizeof(L->b) - 1) L->n = sizeof(L->b) - 1;
}

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return 0;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (const BYTE *)p + n <= (const BYTE *)mi.BaseAddress + mi.RegionSize;
}

static const char *objname(void *obj, char *b, size_t bs)
{
    if (!obj) return "NULL";
    dt_vt *v = readable(obj, 4) ? vt_of(obj) : NULL;
    if (v) snprintf(b, bs, "%s#%d", v->desc->shortname, obj_id(obj));
    else snprintf(b, bs, "%p", obj);
    return b;
}

static void appo(dt_line *L, const char *fmt, ...)
{
    if (L->on >= (int)sizeof(L->o) - 1) return;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(L->o + L->on, sizeof(L->o) - L->on, fmt, ap);
    va_end(ap);
    if (k > 0) L->on += k;
    if (L->on > (int)sizeof(L->o) - 1) L->on = sizeof(L->o) - 1;
}

void dt_begin(dt_line *L, void *obj, const char *method)
{
    char b[64];
    L->n = 0; L->on = 0; L->o[0] = 0;
    app(L, "%s.%s(", objname(obj, b, sizeof(b)), method);
}

void dt_str(dt_line *L, const char *s) { app(L, "%s", s); }
void dt_fnaddr(dt_line *L, void *f) { if (g_level >= 4) app(L, "<%p> ", f); }
void dt_int(dt_line *L, const char *sep, long v)
{
    if (v >= -9999 && v <= 99999) app(L, "%s%ld", sep, v); else app(L, "%s0x%lx", sep, (unsigned long)v);
}
void dt_i64(dt_line *L, const char *sep, long long v) { app(L, "%s%lld", sep, v); }
void dt_flt(dt_line *L, const char *sep, double v) { app(L, "%s%g", sep, v); }
void dt_ptr(dt_line *L, const char *sep, const void *p) { if (p) app(L, "%s%p", sep, p); else app(L, "%sNULL", sep); }

static const char *guidname(const GUID *g, char *b, size_t bs)
{
    if (!g) return "NULL";
    if (!readable(g, sizeof(GUID))) { snprintf(b, bs, "%p?", (const void *)g); return b; }
    for (int i = 0; dt_guids[i].name; i++) if (IsEqualGUID(&dt_guids[i].g, g)) return dt_guids[i].name;
    if (IsEqualGUID(g, &IID_IUnknown)) return "IID_IUnknown";
    if (IsEqualGUID(g, &IID_IClassFactory)) return "IID_IClassFactory";
    if (IsEqualGUID(g, &GUID_NULL)) return "GUID_NULL";
    snprintf(b, bs, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", (unsigned long)g->Data1, g->Data2, g->Data3,
             g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return b;
}

void dt_guid(dt_line *L, const char *sep, const GUID *g) { char b[64]; app(L, "%s%s", sep, guidname(g, b, sizeof(b))); }

void dt_obj(dt_line *L, const char *sep, void *obj) { char b[64]; app(L, "%s%s", sep, objname(obj, b, sizeof(b))); }

static void wstr(dt_line *L, const WCHAR *s, int max)
{
    if (!s) { app(L, "NULL"); return; }
    char b[300]; int n = 0;
    for (; n < max && n < 290 && readable(s + n, 2) && s[n]; n++) b[n] = s[n] < 128 && s[n] >= 32 ? (char)s[n] : '?';
    b[n] = 0;
    app(L, "\"%s\"", b);
}

void dt_wstr(dt_line *L, const char *sep, const WCHAR *s) { app(L, "%s", sep); wstr(L, s, 260); }

void dt_bytes(dt_line *L, const char *sep, const void *p, int n)
{
    app(L, "%s", sep);
    if (!p) { app(L, "NULL"); return; }
    if (!readable(p, n)) { app(L, "%p", p); return; }
    app(L, "[");
    for (int i = 0; i < n; i++) app(L, "%02x", ((const BYTE *)p)[i]);
    app(L, "]");
}

void dt_objdesc(dt_line *L, const char *sep, const DMUS_OBJECTDESC *d)
{
    char b[64];
    app(L, "%s", sep);
    if (!d || !readable(d, sizeof(*d))) { dt_ptr(L, "", d); return; }
    DWORD v = d->dwValidData;
    app(L, "{valid 0x%lx", (unsigned long)v);
    if (v & DMUS_OBJ_CLASS) app(L, " class %s", guidname(&d->guidClass, b, sizeof(b)));
    if (v & DMUS_OBJ_OBJECT) app(L, " object %s", guidname(&d->guidObject, b, sizeof(b)));
    if (v & DMUS_OBJ_NAME) { app(L, " name "); wstr(L, d->wszName, DMUS_MAX_NAME); }
    if (v & DMUS_OBJ_CATEGORY) { app(L, " category "); wstr(L, d->wszCategory, DMUS_MAX_CATEGORY); }
    if (v & DMUS_OBJ_FILENAME) { app(L, " file "); wstr(L, d->wszFileName, DMUS_MAX_FILENAME); }
    if (v & DMUS_OBJ_FULLPATH) app(L, " (full path)");
    if (v & DMUS_OBJ_MEMORY) app(L, " memory %p+%llu", (void *)d->pbMemData, (unsigned long long)d->llMemLength);
    if (v & DMUS_OBJ_STREAM) app(L, " stream %p", (void *)d->pStream);
    if (v & DMUS_OBJ_VERSION) app(L, " version %lu.%lu", (unsigned long)d->vVersion.dwVersionMS, (unsigned long)d->vVersion.dwVersionLS);
    app(L, "}");
}

void dt_audioparams(dt_line *L, const char *sep, const DMUS_AUDIOPARAMS *p)
{
    char b[64];
    app(L, "%s", sep);
    if (!p || !readable(p, sizeof(*p))) { dt_ptr(L, "", p); return; }
    app(L, "{size %lu valid 0x%lx initnow %d features 0x%lx voices %lu rate %lu synth %s}", (unsigned long)p->dwSize,
        (unsigned long)p->dwValidData, p->fInitNow, (unsigned long)p->dwFeatures, (unsigned long)p->dwVoices,
        (unsigned long)p->dwSampleRate, guidname(&p->clsidDefaultSynth, b, sizeof(b)));
}

void dt_portparams(dt_line *L, const char *sep, const DMUS_PORTPARAMS8 *p)
{
    app(L, "%s", sep);
    if (!p || !readable(p, 8)) { dt_ptr(L, "", p); return; }
    app(L, "{size %lu valid 0x%lx voices %lu groups %lu audioch %lu rate %lu effects 0x%lx share %d", (unsigned long)p->dwSize,
        (unsigned long)p->dwValidParams, (unsigned long)p->dwVoices, (unsigned long)p->dwChannelGroups,
        (unsigned long)p->dwAudioChannels, (unsigned long)p->dwSampleRate, (unsigned long)p->dwEffectFlags, p->fShare);
    if (p->dwSize >= sizeof(DMUS_PORTPARAMS8)) app(L, " features 0x%lx", (unsigned long)p->dwFeatures);
    app(L, "}");
}

void dt_pmsg(dt_line *L, const char *sep, const DMUS_PMSG *m)
{
    app(L, "%s", sep);
    if (!m || !readable(m, sizeof(*m))) { dt_ptr(L, "", m); return; }
    app(L, "{type %lu size %lu mt %ld rt %lld pch %lu flags 0x%lx}", (unsigned long)m->dwType, (unsigned long)m->dwSize,
        (long)m->mtTime, (long long)m->rtTime, (unsigned long)m->dwPChannel, (unsigned long)m->dwFlags);
}

void dt_variant(dt_line *L, const char *sep, const VARIANT *v)
{
    app(L, "%s{vt %u", sep, v->vt);
    if (v->vt == VT_I4 || v->vt == VT_INT) app(L, " %ld", (long)v->lVal);
    else if (v->vt == VT_BSTR) { app(L, " "); wstr(L, v->bstrVal, 128); }
    else if (v->vt == VT_UNKNOWN || v->vt == VT_DISPATCH) dt_obj(L, " ", (void *)v->punkVal);
    app(L, "}");
}

void dt_out_obj(dt_line *L, const char *name, void **pp)
{
    char b[64];
    if (pp && readable(pp, 4)) appo(L, " %s=%s", name, objname(*pp, b, sizeof(b)));
}

void dt_out_int(dt_line *L, const char *name, const void *p, int size)
{
    if (!p || !readable(p, size)) return;
    long long v = size == 1 ? *(const BYTE *)p : size == 2 ? *(const SHORT *)p : size == 8 ? *(const long long *)p : *(const LONG *)p;
    appo(L, " %s=%lld", name, v);
}

void dt_out_i64(dt_line *L, const char *name, const void *p)
{
    if (p && readable(p, 8)) appo(L, " %s=%lld", name, *(const long long *)p);
}

void dt_out_objdesc(dt_line *L, const DMUS_OBJECTDESC *d)
{
    dt_line t; t.n = 0;
    dt_objdesc(&t, "", d);
    appo(L, " desc=%s", t.b);
}

void dt_end(dt_line *L, const char *tail) { app(L, ")%s%s", tail, L->o); logline(L->b); }

static const char *hrname(HRESULT hr, char *b)
{
    static const dt_hrname t[] = {
        { S_OK, "S_OK" }, { S_FALSE, "S_FALSE" }, { E_FAIL, "E_FAIL" }, { E_NOINTERFACE, "E_NOINTERFACE" },
        { E_POINTER, "E_POINTER" }, { E_INVALIDARG, "E_INVALIDARG" }, { E_OUTOFMEMORY, "E_OUTOFMEMORY" },
        { E_NOTIMPL, "E_NOTIMPL" }, { CLASS_E_CLASSNOTAVAILABLE, "CLASS_E_CLASSNOTAVAILABLE" },
        { CLASS_E_NOAGGREGATION, "CLASS_E_NOAGGREGATION" }, { REGDB_E_CLASSNOTREG, "REGDB_E_CLASSNOTREG" },
        { (HRESULT)0x88780032, "DSERR_INVALIDPARAM" }, { (HRESULT)0x88780078, "DSERR_NODRIVER" } };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (t[i].hr == hr) return t[i].name;
    for (int i = 0; dt_hrs[i].name; i++) if (dt_hrs[i].hr == hr) return dt_hrs[i].name;
    sprintf(b, "%08lx", (unsigned long)hr);
    return b;
}

void dt_end_hr(dt_line *L, HRESULT hr) { char b[16]; app(L, ") -> %s%s", hrname(hr, b), L->o); logline(L->b); }
void dt_end_ul(dt_line *L, unsigned long v) { app(L, ") -> %lu%s", v, L->o); logline(L->b); }

/* ======================================================================== COM server */

static BOOL clsid_key(REFCLSID clsid, char *out)
{
    WCHAR w[64];
    if (!StringFromGUID2(clsid, w, 64)) return FALSE;
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, 64, NULL, NULL);
    return TRUE;
}

/* the real server of a class: HKLM registration (or target_dir + its file name) */
static BOOL real_server(REFCLSID clsid, char *path)
{
    char key[160], k[64], val[MAX_PATH] = "";
    if (!clsid_key(clsid, k)) return FALSE;
    snprintf(key, sizeof(key), "Software\\Classes\\CLSID\\%s\\InprocServer32", k);
    HKEY h;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return FALSE;
    DWORD sz = sizeof(val), type = 0;
    LONG r = RegQueryValueExA(h, NULL, NULL, &type, (BYTE *)val, &sz);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS || !val[0]) return FALSE;
    if (!lstrcmpiA(val, g_self_path)) {         /* HKLM redirected too (Wine): the original is in the backup */
        snprintf(key, sizeof(key), "Software\\dmtrace\\Originals");
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return FALSE;
        sz = sizeof(val);
        r = RegQueryValueExA(h, k, NULL, &type, (BYTE *)val, &sz);
        RegCloseKey(h);
        if (r != ERROR_SUCCESS) return FALSE;
    }
    char exp[MAX_PATH];
    if (ExpandEnvironmentStringsA(val, exp, MAX_PATH)) strcpy(val, exp);
    if (g_target_dir[0]) {
        const char *base = strrchr(val, '\\'); base = base ? base + 1 : val;
        snprintf(path, MAX_PATH, "%s\\%s", g_target_dir, base);
    } else strcpy(path, val);
    return TRUE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **ppv)
{
    init_once();
    char path[MAX_PATH], b[64], b2[64], b3[64];
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!real_server(clsid, path)) {
        logf("DllGetClassObject(%s) -> no registered server", guidname(clsid, b, sizeof(b)));
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    HMODULE h = LoadLibraryA(path);
    if (!h) { logf("DllGetClassObject(%s): cannot load %s (%lu)", guidname(clsid, b, sizeof(b)), path, GetLastError()); return CLASS_E_CLASSNOTAVAILABLE; }
    add_module(h);
    HRESULT (WINAPI *get)(REFCLSID, REFIID, void **) = (void *)GetProcAddress(h, "DllGetClassObject");
    if (!get) return CLASS_E_CLASSNOTAVAILABLE;
    HRESULT hr = get(clsid, iid, ppv);
    if (SUCCEEDED(hr) && *ppv && !getenv("DMTRACE_PASSTHRU")) dt_hook_iid(*ppv, iid);
    char hb[16];
    if (dt_want(__builtin_return_address(0), 1) || 1)
        logf("DllGetClassObject(%s, %s) from %s -> %s factory=%s", guidname(clsid, b, sizeof(b)), guidname(iid, b2, sizeof(b2)),
             path, hrname(hr, hb), SUCCEEDED(hr) ? objname(*ppv, b3, sizeof(b3)) : "-");
    return hr;
}

HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

static const char *const g_servers[] = { "dmusic.dll", "dmime.dll", "dmloader.dll", "dmsynth.dll", "dmstyle.dll",
                                         "dmcompos.dll", "dmband.dll", "dmscript.dll", "dswave.dll", "dsdmo.dll", NULL };

static BOOL is_dm_server(const char *path)
{
    const char *base = strrchr(path, '\\'); base = base ? base + 1 : path;
    for (int i = 0; g_servers[i]; i++) if (!lstrcmpiA(base, g_servers[i])) return TRUE;
    return FALSE;
}

/* COM on Windows merges HKCU\\Software\\Classes into HKCR; Wine does not, so there the machine-wide
 * registration is redirected as well and the original paths are kept in HKLM\\Software\\dmtrace\\Originals */
static BOOL hkcr_sees_us(void)
{
    char val[MAX_PATH] = "";
    LONG sz = sizeof(val);
    if (RegQueryValueA(HKEY_CLASSES_ROOT, "CLSID\\{D2AC2881-B39B-11D1-8704-00600893B1BD}\\InprocServer32", val, &sz) != ERROR_SUCCESS)
        return FALSE;
    return !lstrcmpiA(val, g_self_path);
}

static void redirect_hklm(void)
{
    HKEY clsids, bak;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Classes\\CLSID", 0, KEY_READ, &clsids) != ERROR_SUCCESS) return;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\dmtrace\\Originals", 0, NULL, 0, KEY_WRITE, NULL, &bak, NULL) != ERROR_SUCCESS) {
        RegCloseKey(clsids);
        logf("DllRegisterServer: HKCU classes are not used here and HKLM is not writable");
        return;
    }
    int n = 0;
    char name[64];
    for (DWORD i = 0;; i++) {
        DWORD nl = sizeof(name);
        LONG e = RegEnumKeyExA(clsids, i, name, &nl, NULL, NULL, NULL, NULL);
        if (e == ERROR_MORE_DATA || (e == ERROR_SUCCESS && name[0] != '{')) continue;
        if (e != ERROR_SUCCESS) break;
        char sub[128], val[MAX_PATH] = "";
        snprintf(sub, sizeof(sub), "%s\\InprocServer32", name);
        HKEY h;
        if (RegOpenKeyExA(clsids, sub, 0, KEY_READ | KEY_WRITE, &h) != ERROR_SUCCESS) continue;
        DWORD sz = sizeof(val);
        RegQueryValueExA(h, NULL, NULL, NULL, (BYTE *)val, &sz);
        if (is_dm_server(val)) {
            RegSetValueExA(bak, name, 0, REG_SZ, (const BYTE *)val, (DWORD)strlen(val) + 1);
            RegSetValueExA(h, NULL, 0, REG_SZ, (const BYTE *)g_self_path, (DWORD)strlen(g_self_path) + 1);
            n++;
        }
        RegCloseKey(h);
    }
    RegCloseKey(bak);
    RegCloseKey(clsids);
    logf("DllRegisterServer: HKCU classes are not used here (Wine): redirected %d machine-wide registrations too", n);
}

static void restore_hklm(void)
{
    HKEY bak;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\dmtrace\\Originals", 0, KEY_READ, &bak) != ERROR_SUCCESS) return;
    int n = 0;
    for (DWORD i = 0;; i++) {
        char name[64], val[MAX_PATH];
        DWORD nl = sizeof(name), vl = sizeof(val);
        if (RegEnumValueA(bak, i, name, &nl, NULL, NULL, (BYTE *)val, &vl) != ERROR_SUCCESS) break;
        char key[200];
        snprintf(key, sizeof(key), "Software\\Classes\\CLSID\\%s\\InprocServer32", name);
        HKEY h;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_WRITE, &h) == ERROR_SUCCESS) {
            RegSetValueExA(h, NULL, 0, REG_SZ, (const BYTE *)val, (DWORD)strlen(val) + 1);
            RegCloseKey(h);
            n++;
        }
    }
    RegCloseKey(bak);
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, "Software\\dmtrace\\Originals");
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, "Software\\dmtrace");
    logf("DllUnregisterServer: restored %d machine-wide registrations", n);
}

/* redirect one class to us in HKCU if its machine-wide server is a DirectMusic dll; 1 if it was */
static int register_one(HKEY clsids, const char *name, int n)
{
    char sub[128], val[MAX_PATH] = "", tm[64] = "Both", cur[MAX_PATH] = "";
    snprintf(sub, sizeof(sub), "%s\\InprocServer32", name);
    HKEY h;
    if (RegOpenKeyExA(clsids, sub, 0, KEY_READ, &h) != ERROR_SUCCESS) return 0;
    DWORD sz = sizeof(val);
    RegQueryValueExA(h, NULL, NULL, NULL, (BYTE *)val, &sz);
    sz = sizeof(tm);
    RegQueryValueExA(h, "ThreadingModel", NULL, NULL, (BYTE *)tm, &sz);
    RegCloseKey(h);
    if (!is_dm_server(val)) return 0;
    char key[200];
    snprintf(key, sizeof(key), "Software\\Classes\\CLSID\\%s\\InprocServer32", name);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, key, 0, KEY_READ, &h) == ERROR_SUCCESS) {
        sz = sizeof(cur);
        RegQueryValueExA(h, NULL, NULL, NULL, (BYTE *)cur, &sz);
        RegCloseKey(h);
        if (!lstrcmpiA(cur, g_self_path)) return 0;         /* already done */
    }
    if (n < 3) logf("  %s: %s", name, val);
    HKEY w;
    LONG ce = RegCreateKeyExA(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &w, NULL);
    if (ce != ERROR_SUCCESS) { logf("  cannot write HKCU\\%s (error %ld)", key, (long)ce); return 0; }
    RegSetValueExA(w, NULL, 0, REG_SZ, (const BYTE *)g_self_path, (DWORD)strlen(g_self_path) + 1);
    RegSetValueExA(w, "ThreadingModel", 0, REG_SZ, (const BYTE *)tm, (DWORD)strlen(tm) + 1);
    RegCloseKey(w);
    return 1;
}

HRESULT WINAPI DllRegisterServer(void)
{
    init_once();
    HKEY clsids;
    LONG e = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Classes\\CLSID", 0, KEY_READ, &clsids);
    if (e != ERROR_SUCCESS) { logf("DllRegisterServer: cannot open HKLM\\Software\\Classes\\CLSID (error %ld)", (long)e); return E_FAIL; }
    int n = 0, seen = 0;
    char name[64];
    /* the DirectMusic classes known from the headers first, then everything else registered to a DM dll */
    for (int i = 0; dt_guids[i].name; i++) {
        if (strncmp(dt_guids[i].name, "CLSID_", 6)) continue;
        if (clsid_key(&dt_guids[i].g, name)) n += register_one(clsids, name, n);
    }
    int known = n;
    for (DWORD i = 0;; i++) {
        DWORD nl = sizeof(name);
        e = RegEnumKeyExA(clsids, i, name, &nl, NULL, NULL, NULL, NULL);
        if (e == ERROR_MORE_DATA) continue;             /* odd long key names: not a CLSID */
        if (e != ERROR_SUCCESS) break;
        seen++;
        if (name[0] == '{') n += register_one(clsids, name, n);
    }
    RegCloseKey(clsids);
    logf("DllRegisterServer: %d known classes, %d more found among %d registered (scan ended with %ld)", known, n - known, seen, (long)e);
    logf("DllRegisterServer: %d DirectMusic classes now go through %s%s", n, g_self_path,
         n ? "" : " (no DirectMusic registration found under HKLM\\Software\\Classes\\CLSID)");
    if (n && !hkcr_sees_us()) redirect_hklm();
    else if (n) logf("DllRegisterServer: OK, DirectMusic now resolves to dmtrace for this user");
    return n ? S_OK : E_FAIL;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    init_once();
    restore_hklm();
    HKEY clsids;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Classes\\CLSID", 0, KEY_READ | KEY_WRITE, &clsids) != ERROR_SUCCESS) return S_OK;
    char name[64], del[512][64];
    int nd = 0;
    for (DWORD i = 0;; i++) {
        DWORD nl = sizeof(name);
        LONG e = RegEnumKeyExA(clsids, i, name, &nl, NULL, NULL, NULL, NULL);
        if (e == ERROR_MORE_DATA) continue;
        if (e != ERROR_SUCCESS) break;
        char sub[128], val[MAX_PATH] = "";
        snprintf(sub, sizeof(sub), "%s\\InprocServer32", name);
        HKEY h;
        if (RegOpenKeyExA(clsids, sub, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        DWORD sz = sizeof(val);
        RegQueryValueExA(h, NULL, NULL, NULL, (BYTE *)val, &sz);
        RegCloseKey(h);
        if (!lstrcmpiA(val, g_self_path) && nd < 512) strcpy(del[nd++], name);
    }
    for (int i = 0; i < nd; i++) {
        char sub[128];
        snprintf(sub, sizeof(sub), "%s\\InprocServer32", del[i]);
        RegDeleteKeyA(clsids, sub);
        RegDeleteKeyA(clsids, del[i]);
    }
    RegCloseKey(clsids);
    logf("DllUnregisterServer: removed %d redirections", nd);
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        InitializeCriticalSection(&g_cs);
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
