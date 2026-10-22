/* multiperf - reDM with several performances at once, and objects that outlive their performance:
 *   churn:   performances made (InitAudio), played and released again and again; each one's synth port and its
 *            latency clock are handed to the user thread before the performance goes;
 *   user:    the port and clock of whichever performance churn made last, used all the time, also after that
 *            performance is released (GetRunningStats, channel groups and priorities, the synth's properties,
 *            GetFormat, the latency clock), and the master clock (never earlier than a time it gave before);
 *   steady:  a second performance that lives throughout, playing the same segments churn plays;
 *   editor:  those shared segments edited while both performances play them (repeats, a track inserted and taken
 *            out again, a track's parameters, notification types, clones);
 *   reloader / objuser: styles, a chord map, a collection and a wave loaded again and again while another thread
 *            uses them (their bands downloaded, motifs, names, the composer, SetParam of each on the shared segments);
 *   (churn also plays a track itself (Track::InitPlay) on each of its performances, and ends it (EndPlay) after that
 *            performance is gone, its segment loaded again meanwhile (IPersistStream::Load from a
 *            stream that knows the loader, so the styles and scripts it references load too) while the other
 *            threads use it.
 * A watchdog reports a deadlock (no thread moving for 10 s) and ends the process with exit code 3; a crash ends it
 * otherwise.  multiperf.exe <seconds> <segment>...  (in a folder with the segments and compsty.sty, mix1.sty,
 * cmap1.cdm, bells.dls, tone.wav; MP_THREADS=<mask> runs only some threads: churn 1, user 2, steady 4, editor 8,
 * reloader 0x10, objuser 0x20) */
#define COBJMACROS
#include <windows.h>
#include <dmusici.h>
#include <dmusicc.h>
#include <dmksctrl.h>
#include <dmplugin.h>
#include <stdio.h>
#include <stdlib.h>
#include <dsound.h>
#include <shlwapi.h>

#define IDirectMusicTrack8_InitPlay(p,a,b,c,d,e) (p)->lpVtbl->InitPlay(p,a,b,c,d,e)
#define IDirectMusicTrack8_EndPlay(p,a) (p)->lpVtbl->EndPlay(p,a)

static const GUID s_IID_IKsControl = { 0x28f54685, 0x06fd, 0x11d2, { 0xb2, 0x7a, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };

static IDirectMusicLoader8 *g_loader;
static IDirectMusicSegment8 *g_segs[16];
static WCHAR g_files[16][MAX_PATH];
static int g_nsegs;
static volatile LONG g_stop, g_moves[6], g_bad;
/* the objects loaded again (reloader) while used (objuser): styles, a chord map, a collection, a wave */
static const WCHAR *g_objfiles[5] = { L"compsty.sty", L"mix1.sty", L"cmap1.cdm", L"bells.dls", L"tone.wav" };
static IUnknown *g_objs[5];
static CRITICAL_SECTION g_slot;
static IDirectMusicPort *g_port;            /* the last churn performance's port, and its latency clock */
static IReferenceClock *g_lat;

/* ------------------------------------------------------------------ a file stream that knows the loader
 * (IDirectMusicGetLoader, as the loader's own streams do): a segment loaded again through it finds its styles,
 * chord maps and scripts */
typedef struct { IStream IStream_iface; IDirectMusicGetLoader IDirectMusicGetLoader_iface; LONG ref; IStream *in; } lstream;
static lstream *ls_of(IStream *i) { return CONTAINING_RECORD(i, lstream, IStream_iface); }
static lstream *lg_of(IDirectMusicGetLoader *i) { return CONTAINING_RECORD(i, lstream, IDirectMusicGetLoader_iface); }
static HRESULT WINAPI ls_qi(IStream *i, REFIID r, void **o)
{
    lstream *s = ls_of(i);
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IStream) || IsEqualIID(r, &IID_ISequentialStream)) *o = &s->IStream_iface;
    else if (IsEqualIID(r, &IID_IDirectMusicGetLoader)) *o = &s->IDirectMusicGetLoader_iface;
    else { *o = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&s->ref);
    return S_OK;
}
static ULONG WINAPI ls_addref(IStream *i) { return InterlockedIncrement(&ls_of(i)->ref); }
static ULONG WINAPI ls_release(IStream *i)
{
    lstream *s = ls_of(i);
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) { IStream_Release(s->in); free(s); }
    return r;
}
static HRESULT WINAPI ls_read(IStream *i, void *p, ULONG n, ULONG *got) { return IStream_Read(ls_of(i)->in, p, n, got); }
static HRESULT WINAPI ls_write(IStream *i, const void *p, ULONG n, ULONG *got) { return STG_E_ACCESSDENIED; }
static HRESULT WINAPI ls_seek(IStream *i, LARGE_INTEGER d, DWORD o, ULARGE_INTEGER *np) { return IStream_Seek(ls_of(i)->in, d, o, np); }
static HRESULT WINAPI ls_setsize(IStream *i, ULARGE_INTEGER n) { return E_NOTIMPL; }
static HRESULT WINAPI ls_copyto(IStream *i, IStream *t, ULARGE_INTEGER n, ULARGE_INTEGER *r, ULARGE_INTEGER *w) { return E_NOTIMPL; }
static HRESULT WINAPI ls_commit(IStream *i, DWORD f) { return S_OK; }
static HRESULT WINAPI ls_revert(IStream *i) { return S_OK; }
static HRESULT WINAPI ls_lock(IStream *i, ULARGE_INTEGER o, ULARGE_INTEGER n, DWORD t) { return E_NOTIMPL; }
static HRESULT WINAPI ls_stat(IStream *i, STATSTG *s, DWORD f) { return IStream_Stat(ls_of(i)->in, s, f); }
static HRESULT WINAPI ls_clone(IStream *i, IStream **o) { *o = NULL; return E_NOTIMPL; }
static const IStreamVtbl ls_vtbl = { ls_qi, ls_addref, ls_release, ls_read, ls_write, ls_seek, ls_setsize, ls_copyto, ls_commit,
                                     ls_revert, ls_lock, ls_lock, ls_stat, ls_clone };
static HRESULT WINAPI lg_qi(IDirectMusicGetLoader *i, REFIID r, void **o) { return ls_qi(&lg_of(i)->IStream_iface, r, o); }
static ULONG WINAPI lg_addref(IDirectMusicGetLoader *i) { return ls_addref(&lg_of(i)->IStream_iface); }
static ULONG WINAPI lg_release(IDirectMusicGetLoader *i) { return ls_release(&lg_of(i)->IStream_iface); }
static HRESULT WINAPI lg_getloader(IDirectMusicGetLoader *i, IDirectMusicLoader **l)
{
    *l = (IDirectMusicLoader *)g_loader;
    IDirectMusicLoader8_AddRef(g_loader);
    return S_OK;
}
static const IDirectMusicGetLoaderVtbl lg_vtbl = { lg_qi, lg_addref, lg_release, lg_getloader };
static IStream *loader_stream(const WCHAR *file)
{
    IStream *in = NULL;
    if (FAILED(SHCreateStreamOnFileW(file, STGM_READ, &in))) return NULL;
    lstream *s = calloc(1, sizeof(*s));
    if (!s) { IStream_Release(in); return NULL; }
    s->IStream_iface.lpVtbl = (IStreamVtbl *)&ls_vtbl;
    s->IDirectMusicGetLoader_iface.lpVtbl = (IDirectMusicGetLoaderVtbl *)&lg_vtbl;
    s->ref = 1; s->in = in;
    return &s->IStream_iface;
}

static IDirectMusicPerformance8 *new_perf(void)
{
    IDirectMusicPerformance8 *p = NULL;
    if (FAILED(CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&p)))
        return NULL;
    if (FAILED(IDirectMusicPerformance8_InitAudio(p, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, DMUS_AUDIOF_ALL, NULL))) {
        IDirectMusicPerformance8_Release(p);
        return NULL;
    }
    return p;
}

static void play_some(IDirectMusicPerformance8 *p, int n)
{
    IDirectMusicSegment8 *s = g_segs[n % g_nsegs];
    IDirectMusicSegment8_Download(s, (IUnknown *)p);
    IDirectMusicSegmentState *st = NULL;
    IDirectMusicPerformance8_PlaySegmentEx(p, (IUnknown *)s, NULL, NULL, (n & 1) ? DMUS_SEGF_SECONDARY : 0, 0, &st, NULL, NULL);
    if (st) IDirectMusicSegmentState_Release(st);
}

static DWORD WINAPI th_churn(void *x)
{
    CoInitialize(NULL);
    for (int n = 0; !g_stop; n++) {
        IDirectMusicPerformance8 *p = new_perf();
        if (!p) { InterlockedIncrement(&g_bad); Sleep(10); continue; }
        IDirectMusicPort *port = NULL;
        DWORD g, c;
        IDirectMusicPerformance8_PChannelInfo(p, 0, &port, &g, &c);
        IReferenceClock *lat = NULL;
        if (port) IDirectMusicPort_GetLatencyClock(port, &lat);
        EnterCriticalSection(&g_slot);
        IDirectMusicPort *op = g_port; IReferenceClock *ol = g_lat;
        g_port = port; g_lat = lat;
        LeaveCriticalSection(&g_slot);
        if (op) IDirectMusicPort_Release(op);
        if (ol) IReferenceClock_Release(ol);
        play_some(p, n);
        play_some(p, n + 1);
        /* a track played by the application itself, ended after the performance is gone */
        IDirectMusicTrack8 *t = NULL;
        void *data = NULL;
        IDirectMusicSegmentState *st = NULL;
        IDirectMusicSegment8 *s = g_segs[n % g_nsegs];
        IDirectMusicPerformance8_PlaySegmentEx(p, (IUnknown *)s, NULL, NULL, DMUS_SEGF_SECONDARY, 0, &st, NULL, NULL);
        if (st && SUCCEEDED(IDirectMusicSegment8_GetTrack(s, &GUID_NULL, 0xffffffff, 0, (IDirectMusicTrack **)&t)) && t)
            if (FAILED(IDirectMusicTrack8_InitPlay(t, st, (IDirectMusicPerformance *)p, &data, 0, 0))) data = NULL;
        Sleep(n % 7);
        if (n % 3 == 0) IDirectMusicPerformance8_StopEx(p, NULL, 0, 0);
        /* (CloseDown before the last Release, as DirectMusic asks: the default audiopath holds the performance) */
        if (n % 2) IDirectMusicPerformance8_CloseDown(p);
        if (st) IDirectMusicSegmentState_Release(st);
        if (!(n % 2)) IDirectMusicPerformance8_CloseDown(p);
        IDirectMusicPerformance8_Release(p);
        if (t) {
            /* the segment loaded again from its file while the state is still out: the track moves to the new data */
            IPersistStream *ps = NULL;
            IStream *f = NULL;
            if (SUCCEEDED(IDirectMusicSegment8_QueryInterface(s, &IID_IPersistStream, (void **)&ps)) &&
                (f = loader_stream(g_files[n % g_nsegs])))
                IPersistStream_Load(ps, f);
            if (f) IStream_Release(f);
            if (ps) IPersistStream_Release(ps);
            if (data) IDirectMusicTrack8_EndPlay(t, data);
            IDirectMusicTrack8_Release(t);
        }
        InterlockedIncrement(&g_moves[0]);
    }
    CoUninitialize();
    return 0;
}

static DWORD WINAPI th_user(void *x)
{
    CoInitialize(NULL);
    IDirectMusic8 *dm = NULL;
    IReferenceClock *master = NULL;
    REFERENCE_TIME last = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_DirectMusic, NULL, CLSCTX_INPROC, &IID_IDirectMusic8, (void **)&dm)) && dm)
        IDirectMusic8_GetMasterClock(dm, NULL, &master);
    for (int n = 0; !g_stop; n++) {
        EnterCriticalSection(&g_slot);
        IDirectMusicPort *port = g_port; IReferenceClock *lat = g_lat;
        if (port) IDirectMusicPort_AddRef(port);
        if (lat) IReferenceClock_AddRef(lat);
        LeaveCriticalSection(&g_slot);
        if (port) {
            DMUS_SYNTHSTATS ss = { sizeof(ss) };
            IDirectMusicPort_GetRunningStats(port, &ss);
            DWORD groups = 0, prio = 0, size = sizeof(WAVEFORMATEX), buf = 0;
            IDirectMusicPort_GetNumChannelGroups(port, &groups);
            IDirectMusicPort_SetChannelPriority(port, 1, n & 15, DAUD_STANDARD_VOICE_PRIORITY + (n & 7));
            IDirectMusicPort_GetChannelPriority(port, 1, n & 15, &prio);
            WAVEFORMATEX wf;
            IDirectMusicPort_GetFormat(port, &wf, &size, &buf);
            IKsControl *ks = NULL;
            if (SUCCEEDED(IDirectMusicPort_QueryInterface(port, &s_IID_IKsControl, (void **)&ks)) && ks) {
                KSPROPERTY kp = { { { GUID_DMUS_PROP_SamplePlaybackRate, 0, KSPROPERTY_TYPE_GET } } };
                DWORD v = 0; ULONG got = 0;
                ks->lpVtbl->KsProperty(ks, &kp, sizeof(kp), &v, sizeof(v), &got);
                KSPROPERTY kv = { { { GUID_DMUS_PROP_Volume, 0, KSPROPERTY_TYPE_SET } } };
                LONG vol = 0;
                ks->lpVtbl->KsProperty(ks, &kv, sizeof(kv), &vol, sizeof(vol), &got);
                ks->lpVtbl->Release(ks);
            }
            IDirectMusicPort_Release(port);
        }
        if (lat) {
            REFERENCE_TIME t;
            IReferenceClock_GetTime(lat, &t);
            IReferenceClock_Release(lat);
        }
        if (master) {
            REFERENCE_TIME t = 0;
            IReferenceClock_GetTime(master, &t);
            if (t < last) { printf("master clock went back: %lld after %lld\n", (long long)t, (long long)last); InterlockedIncrement(&g_bad); }
            last = t;
        }
        InterlockedIncrement(&g_moves[1]);
    }
    if (master) IReferenceClock_Release(master);
    if (dm) IDirectMusic8_Release(dm);
    CoUninitialize();
    return 0;
}

static DWORD WINAPI th_steady(void *x)
{
    CoInitialize(NULL);
    IDirectMusicPerformance8 *p = new_perf();
    if (!p) { InterlockedIncrement(&g_bad); CoUninitialize(); return 0; }
    for (int n = 0; !g_stop; n++) {
        play_some(p, n * 3 + 1);
        Sleep(n % 11);
        if (n % 5 == 0) IDirectMusicPerformance8_StopEx(p, NULL, 0, 0);
        InterlockedIncrement(&g_moves[2]);
    }
    IDirectMusicPerformance8_CloseDown(p);
    IDirectMusicPerformance8_Release(p);
    CoUninitialize();
    return 0;
}

static DWORD WINAPI th_editor(void *x)
{
    CoInitialize(NULL);
    for (int n = 0; !g_stop; n++) {
        IDirectMusicSegment8 *s = g_segs[n % g_nsegs];
        IDirectMusicSegment8_SetRepeats(s, n % 3);
        IDirectMusicTrack *t = NULL;
        if (SUCCEEDED(IDirectMusicSegment8_GetTrack(s, &GUID_NULL, 0xffffffff, (n / g_nsegs) % 4, &t)) && t) {
            IDirectMusicTrack *c = NULL;
            if (SUCCEEDED(IDirectMusicTrack_Clone(t, 0, 0, &c)) && c) {
                if (SUCCEEDED(IDirectMusicSegment8_InsertTrack(s, c, 0x80000000))) {
                    Sleep(n % 3);
                    IDirectMusicSegment8_RemoveTrack(s, c);
                }
                IDirectMusicTrack_Release(c);
            }
            struct { MUSIC_TIME mtTime; double dblTempo; } tp = { 0, 100.0 + n % 40 };     /* DMUS_TEMPO_PARAM */
            IDirectMusicTrack_SetParam(t, &GUID_TempoParam, 0, &tp);
            IDirectMusicTrack_Release(t);
        }
        IDirectMusicSegment8_AddNotificationType(s, &GUID_NOTIFICATION_MEASUREANDBEAT);
        IDirectMusicSegment8_RemoveNotificationType(s, &GUID_NOTIFICATION_MEASUREANDBEAT);
        IDirectMusicSegment *k = NULL;
        if (SUCCEEDED(IDirectMusicSegment8_Clone(s, 0, 0, &k)) && k) IDirectMusicSegment_Release(k);
        Sleep(1);
        InterlockedIncrement(&g_moves[3]);
    }
    CoUninitialize();
    return 0;
}

static DWORD WINAPI th_reloader(void *x)
{
    CoInitialize(NULL);
    for (int n = 0; !g_stop; n++) {
        int k = n % 5;
        IPersistStream *ps = NULL;
        IStream *f = NULL;
        if (g_objs[k] && SUCCEEDED(IUnknown_QueryInterface(g_objs[k], &IID_IPersistStream, (void **)&ps)) &&
            (f = loader_stream(g_objfiles[k])))
            IPersistStream_Load(ps, f);
        if (f) IStream_Release(f);
        if (ps) IPersistStream_Release(ps);
        Sleep(n % 2);
        InterlockedIncrement(&g_moves[4]);
    }
    CoUninitialize();
    return 0;
}

static DWORD WINAPI th_objuser(void *x)
{
    CoInitialize(NULL);
    IDirectMusicPerformance8 *p = new_perf();
    IDirectMusicComposer *comp = NULL;
    CoCreateInstance(&CLSID_DirectMusicComposer, NULL, CLSCTX_INPROC, &IID_IDirectMusicComposer, (void **)&comp);
    IDirectMusicChordMap *cm = (IDirectMusicChordMap *)g_objs[2];
    for (int n = 0; !g_stop; n++) {
        IDirectMusicStyle8 *st = (IDirectMusicStyle8 *)g_objs[n & 1];
        IDirectMusicSegment8 *s = g_segs[n % g_nsegs];
        if (st) {
            IDirectMusicBand *b = NULL;
            if (IDirectMusicStyle8_GetDefaultBand(st, &b) == S_OK && b) {
                if (p) { IDirectMusicBand_Download(b, (IDirectMusicPerformance *)p); IDirectMusicBand_Unload(b, (IDirectMusicPerformance *)p); }
                IDirectMusicSegment *bs = NULL;
                if (SUCCEEDED(IDirectMusicBand_CreateSegment(b, &bs)) && bs) IDirectMusicSegment_Release(bs);
                IDirectMusicBand_Release(b);
            }
            WCHAR name[0x104];
            for (DWORD k = 0; k < 4 && IDirectMusicStyle8_EnumBand(st, k, name) == S_OK; k++) {
                IDirectMusicBand *nb = NULL;
                if (IDirectMusicStyle8_GetBand(st, name, &nb) == S_OK && nb) IDirectMusicBand_Release(nb);
            }
            for (DWORD k = 0; k < 2 && IDirectMusicStyle8_EnumMotif(st, k, name) == S_OK; k++) {
                IDirectMusicSegment *ms = NULL;
                if (IDirectMusicStyle8_GetMotif(st, name, &ms) == S_OK && ms) IDirectMusicSegment_Release(ms);
            }
            double tempo; DMUS_TIMESIGNATURE ts; DWORD mn, mx;
            IDirectMusicStyle8_GetTempo(st, &tempo);
            IDirectMusicStyle8_GetTimeSignature(st, &ts);
            IDirectMusicStyle8_GetEmbellishmentLength(st, 0, 50, &mn, &mx);
            IDirectMusicChordMap *dc = NULL;
            if (IDirectMusicStyle8_GetDefaultChordMap(st, &dc) == S_OK && dc) IDirectMusicChordMap_Release(dc);
            IDirectMusicSegment8_SetParam(s, &GUID_IDirectMusicStyle, 0xffffffff, 0, 0, st);
            if (comp && cm) {
                IDirectMusicSegment *cs = NULL;
                IDirectMusicComposer_ComposeSegmentFromShape(comp, (IDirectMusicStyle *)st, 2, n % 8, 1, n & 1, n & 2, cm, &cs);
                if (cs) IDirectMusicSegment_Release(cs);    /* (a style without a band: an error, with the segment) */
            }
        }
        if (cm) {
            DWORD scale;
            IDirectMusicChordMap_GetScale(cm, &scale);
            IDirectMusicSegment8_SetParam(s, &GUID_IDirectMusicChordMap, 0xffffffff, 0, 0, cm);
        }
        IDirectMusicCollection *col = (IDirectMusicCollection *)g_objs[3];
        if (col) {
            DWORD patch; WCHAR nm[64];
            for (DWORD k = 0; k < 3 && IDirectMusicCollection_EnumInstrument(col, k, &patch, nm, 64) == S_OK; k++) ;
            IDirectMusicSegment8_SetParam(s, &GUID_ConnectToDLSCollection, 0xffffffff, 0, 0, col);
        }
        InterlockedIncrement(&g_moves[5]);
    }
    if (comp) IDirectMusicComposer_Release(comp);
    if (p) { IDirectMusicPerformance8_CloseDown(p); IDirectMusicPerformance8_Release(p); }
    CoUninitialize();
    return 0;
}

int main(int argc, char **argv)
{
    void *volatile keep = (void *)DirectSoundCreate; (void)keep;     /* reAL loaded as a DirectSound game loads it */
    int secs = argc > 1 ? atoi(argv[1]) : 20;
    CoInitialize(NULL);
    InitializeCriticalSection(&g_slot);
    if (FAILED(CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&g_loader))) return 1;
    WCHAR dir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, dir);
    IDirectMusicLoader8_SetSearchDirectory(g_loader, &GUID_DirectMusicAllTypes, dir, FALSE);
    for (int i = 2; i < argc && g_nsegs < 16; i++) {
        MultiByteToWideChar(CP_ACP, 0, argv[i], -1, g_files[g_nsegs], MAX_PATH);
        IDirectMusicSegment8 *s = NULL;
        IDirectMusicLoader8_LoadObjectFromFile(g_loader, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, g_files[g_nsegs], (void **)&s);
        if (s) g_segs[g_nsegs++] = s;
        else printf("%s not loaded\n", argv[i]);
    }
    if (!g_nsegs) { fprintf(stderr, "usage: multiperf seconds segment...\n"); return 2; }
    static const GUID *cls[5] = { &CLSID_DirectMusicStyle, &CLSID_DirectMusicStyle, &CLSID_DirectMusicChordMap,
                                  &CLSID_DirectMusicCollection, &CLSID_DirectSoundWave };
    for (int k = 0; k < 5; k++)
        if (FAILED(IDirectMusicLoader8_LoadObjectFromFile(g_loader, cls[k], &IID_IUnknown, (WCHAR *)g_objfiles[k], (void **)&g_objs[k])))
            g_objs[k] = NULL;
    LPTHREAD_START_ROUTINE fn[6] = { th_churn, th_user, th_steady, th_editor, th_reloader, th_objuser };
    HANDLE th[6];
    int nth = 0;
    const char *mask = getenv("MP_THREADS");          /* (which threads, a bit each in the order above: all by default) */
    unsigned m = mask ? (unsigned)strtoul(mask, NULL, 0) : 0x3f;
    for (int k = 0; k < 6; k++) if (m & (1u << k)) th[nth++] = CreateThread(NULL, 0, fn[k], NULL, 0, NULL);
    LONG last[6] = { 0 };
    DWORD still = GetTickCount(), end = GetTickCount() + secs * 1000;
    while ((LONG)(GetTickCount() - end) < 0) {
        Sleep(250);
        int moved = 0;
        for (int k = 0; k < 6; k++) if (g_moves[k] != last[k]) { moved = 1; last[k] = g_moves[k]; }
        if (moved) still = GetTickCount();
        else if (GetTickCount() - still > 10000) {
            printf("DEADLOCK: no thread moved for 10 s (performances %ld, port calls %ld, plays %ld, edits %ld, reloads %ld, "
                   "object calls %ld)\n", g_moves[0], g_moves[1], g_moves[2], g_moves[3], g_moves[4], g_moves[5]);
            fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 3);
        }
    }
    g_stop = 1;
    if (WaitForMultipleObjects(nth, th, TRUE, 20000) != WAIT_OBJECT_0) {
        printf("DEADLOCK at the end (performances %ld, port calls %ld, plays %ld, edits %ld)\n",
               g_moves[0], g_moves[1], g_moves[2], g_moves[3]);
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    if (g_port) IDirectMusicPort_Release(g_port);
    if (g_lat) IReferenceClock_Release(g_lat);
    printf("%s: performances %ld, port calls %ld, plays %ld, edits %ld, reloads %ld, object calls %ld, objects %d, failures %ld\n",
           g_bad ? "FAILED" : "ok", g_moves[0], g_moves[1], g_moves[2], g_moves[3], g_moves[4], g_moves[5],
           !!g_objs[0] + !!g_objs[1] + !!g_objs[2] + !!g_objs[3] + !!g_objs[4], g_bad);
    for (int i = 0; i < g_nsegs; i++) IDirectMusicSegment8_Release(g_segs[i]);
    for (int k = 0; k < 5; k++) if (g_objs[k]) IUnknown_Release(g_objs[k]);
    IDirectMusicLoader8_Release(g_loader);
    CoUninitialize();
    return g_bad != 0;
}
