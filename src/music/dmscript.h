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
enum { RM_AUTO_PERFORMANCE, RM_AUTO_SEGMENT, RM_AUTO_SEGMENTSTATE, RM_AUTO_AUDIOPATH, RM_AUTO_CONFIG };
IDispatch *rm_auto_new(IUnknown *outer, int kind);
void rm_auto_free(IDispatch *d);
/* Segment.Recompose (dmcompos 77287974 through its private interface): the segment's chords composed again */
HRESULT rm_segment_recompose(IUnknown *seg);

#endif
