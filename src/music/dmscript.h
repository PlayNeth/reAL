/* reDM: DirectMusic's scripts (dmscript.dll's CDirectMusicScript) and the automation the scripts reach the other
 * objects through (dmscript's CAutDirectMusic* classes, aggregated by the objects that offer IDispatch). */
#ifndef REAL_DMSCRIPT_H
#define REAL_DMSCRIPT_H
#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <dmusici.h>

HRESULT rm_script_create(REFIID riid, void **out);

/* an object's IDispatch, delegating its IUnknown to the object (`outer`, not referenced) */
enum { RM_AUTO_PERFORMANCE, RM_AUTO_SEGMENT, RM_AUTO_SEGMENTSTATE, RM_AUTO_AUDIOPATH, RM_AUTO_CONFIG };
IDispatch *rm_auto_new(IUnknown *outer, int kind);
void rm_auto_free(IDispatch *d);
/* Segment.Recompose (dmcompos 77287974 through its private interface): the segment's chords composed again */
HRESULT rm_segment_recompose(IUnknown *seg);

/* the script track's COM side (dmscript.c): see there */
BOOL rm_script_init_blocks(IUnknown *script);
void rm_script_track_init(IUnknown *script, IDirectMusicPerformance *perf, IUnknown *state, DWORD vtrack, BOOL errnotify);
HRESULT rm_script_track_event(IDirectMusicPerformance *perf, IUnknown *state, IDirectMusicTool *tool, IUnknown *script,
                              const WCHAR *routine, DWORD evflags, MUSIC_TIME mt, DWORD vtrack);
HRESULT rm_script_track_process(IUnknown *user, DWORD vtrack, BOOL errnotify);

#endif
