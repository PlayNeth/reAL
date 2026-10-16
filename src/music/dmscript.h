/* reDM: DirectMusic's scripts (dmscript.dll's CDirectMusicScript) and the automation the scripts reach the other
 * objects through (dmscript's CAutDirectMusic* classes, aggregated by the objects that offer IDispatch). */
#ifndef REAL_DMSCRIPT_H
#define REAL_DMSCRIPT_H
#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>

HRESULT rm_script_create(REFIID riid, void **out);

/* an object's IDispatch, delegating its IUnknown to the object (`outer`, not referenced) */
enum { RM_AUTO_PERFORMANCE, RM_AUTO_SEGMENT };
IDispatch *rm_auto_new(IUnknown *outer, int kind);
void rm_auto_free(IDispatch *d);

#endif
