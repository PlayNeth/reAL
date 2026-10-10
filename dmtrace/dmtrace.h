/* dmtrace - DirectMusic API tracer (see dmtrace.c) */
#ifndef DMTRACE_H
#define DMTRACE_H
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdint.h>
#include <dmusici.h>
#include <dmusicc.h>

typedef struct dt_desc { const char *name, *shortname; int n; void *const *wrap; } dt_desc;
typedef struct dt_guidname { GUID g; const char *name; } dt_guidname;
typedef struct dt_iidmap { const GUID *iid; const dt_desc *desc; } dt_iidmap;
typedef struct dt_hrname { HRESULT hr; const char *name; } dt_hrname;
typedef struct dt_line { char b[3072]; int n; char o[1024]; int on; } dt_line;

extern const dt_guidname dt_guids[];
extern const dt_iidmap dt_iids[];
extern const dt_hrname dt_hrs[];
#include "dmtrace_descs.h"

void *dt_orig(void *obj, int idx);
int dt_want(void *ret_addr, int level);
void dt_hook(void *obj, const dt_desc *d);
void dt_hook_iid(void *obj, const GUID *iid);
void dt_forget(void *obj);

void dt_begin(dt_line *L, void *obj, const char *method);
void dt_fnaddr(dt_line *L, void *f);
void dt_str(dt_line *L, const char *s);
void dt_int(dt_line *L, const char *sep, long v);
void dt_i64(dt_line *L, const char *sep, long long v);
void dt_flt(dt_line *L, const char *sep, double v);
void dt_ptr(dt_line *L, const char *sep, const void *p);
void dt_guid(dt_line *L, const char *sep, const GUID *g);
void dt_obj(dt_line *L, const char *sep, void *obj);
void dt_wstr(dt_line *L, const char *sep, const WCHAR *s);
void dt_bytes(dt_line *L, const char *sep, const void *p, int n);
void dt_objdesc(dt_line *L, const char *sep, const DMUS_OBJECTDESC *d);
void dt_audioparams(dt_line *L, const char *sep, const DMUS_AUDIOPARAMS *p);
void dt_portparams(dt_line *L, const char *sep, const DMUS_PORTPARAMS8 *p);
void dt_variant(dt_line *L, const char *sep, const VARIANT *v);
void dt_pmsg(dt_line *L, const char *sep, const DMUS_PMSG *m);
void dt_out_obj(dt_line *L, const char *name, void **pp);
void dt_out_int(dt_line *L, const char *name, const void *p, int size);
void dt_out_i64(dt_line *L, const char *name, const void *p);
void dt_out_objdesc(dt_line *L, const DMUS_OBJECTDESC *d);
void dt_end(dt_line *L, const char *tail);
void dt_end_hr(dt_line *L, HRESULT hr);
void dt_end_ul(dt_line *L, unsigned long v);
#endif
