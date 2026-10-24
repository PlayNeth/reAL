/* porttest - the synth port DirectMusic hands out: through an audiopath's port stage (DMUS_PATH_PORT), through
 * Performance::PChannelInfo, and the sink stage (0x5000).  Prints what each method of the port gives.
 * Run with tests/comcmp.sh porttest.exe [section]. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dmusici.h>
#include <dmusicc.h>
#include <dmksctrl.h>
#include <dsound.h>
#include <stdio.h>
#include <string.h>

DEFINE_GUID(IID_IKsControl_, 0x28f54685, 0x06fd, 0x11d2, 0xb2, 0x7a, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96);
DEFINE_GUID(IID_IDirectMusicThru_, 0xced153e7, 0x3606, 0x11d2, 0xb9, 0xf9, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12);
DEFINE_GUID(IID_IDirectMusicSynthSink_, 0x09823663, 0x5c85, 0x11d2, 0xaf, 0xa6, 0x00, 0xaa, 0x00, 0x24, 0xd8, 0xb6);
DEFINE_GUID(IID_IDirectSoundConnect_, 0x5d4ab9ee, 0xc3e9, 0x4e95, 0x9d, 0xee, 0x7b, 0x2c, 0x2c, 0xd1, 0x1d, 0x53);
DEFINE_GUID(IID_IDirectSoundSynthSink_, 0x73a6a85a, 0x493e, 0x4c87, 0xb4, 0xa5, 0xbe, 0x53, 0xeb, 0x92, 0x74, 0x4b);

/* IDirectSoundSynthSink (dsound's sink: 6efc7dcc) */
typedef struct dssink dssink;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(dssink *, REFIID, void **);
    ULONG (WINAPI *AddRef)(dssink *);
    ULONG (WINAPI *Release)(dssink *);
    HRESULT (WINAPI *GetLatencyClock)(dssink *, IReferenceClock **);
    HRESULT (WINAPI *Activate)(dssink *, BOOL);
    HRESULT (WINAPI *SampleToRefTime)(dssink *, LONGLONG, REFERENCE_TIME *);
    HRESULT (WINAPI *RefToSampleTime)(dssink *, REFERENCE_TIME, LONGLONG *);
    HRESULT (WINAPI *GetFormat)(dssink *, WAVEFORMATEX *, DWORD, DWORD *);
} dssinkvtbl;
struct dssink { const dssinkvtbl *lpVtbl; };

static IDirectMusicPerformance8 *perf;

static void iids(const char *what, IUnknown *u)
{
    static const struct { const GUID *g; const char *n; } t[] = {
        { &IID_IUnknown, "IUnknown" }, { &IID_IDirectMusicPort, "IDirectMusicPort" },
        { &IID_IDirectMusicPortDownload, "IDirectMusicPortDownload" }, { &IID_IKsControl_, "IKsControl" },
        { &IID_IDirectMusicThru_, "IDirectMusicThru" }, { &IID_IReferenceClock, "IReferenceClock" },
        { &IID_IDirectMusicSynthSink_, "IDirectMusicSynthSink" }, { &IID_IDirectSoundConnect_, "IDirectSoundConnect" },
        { &IID_IDirectSoundSynthSink_, "IDirectSoundSynthSink" }, { &IID_IDirectSound8, "IDirectSound8" },
    };
    printf("  %s:", what);
    for (unsigned k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
        IUnknown *o = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface(u, t[k].g, (void **)&o))) { printf(" %s", t[k].n); IUnknown_Release(o); }
    }
    printf("\n");
}

static void port_methods(IDirectMusicPort *p)
{
    iids("port interfaces", (IUnknown *)p);
    DMUS_PORTCAPS c;
    memset(&c, 0, sizeof(c));
    c.dwSize = sizeof(c);
    HRESULT hr = IDirectMusicPort_GetCaps(p, &c);
    char name[128];
    WideCharToMultiByte(CP_ACP, 0, c.wszDescription, -1, name, sizeof(name), NULL, NULL);
    printf("  GetCaps %08lx: flags %lx class %lu type %lu memsize %lu maxgroups %lu maxvoices %lu maxaudio %lu effects %lx \"%s\"\n",
           hr, c.dwFlags, c.dwClass, c.dwType, c.dwMemorySize, c.dwMaxChannelGroups, c.dwMaxVoices, c.dwMaxAudioChannels,
           c.dwEffectFlags, name);
    c.dwSize = 4;
    printf("  GetCaps small: %08lx\n", IDirectMusicPort_GetCaps(p, &c));
    printf("  GetCaps NULL: %08lx\n", IDirectMusicPort_GetCaps(p, NULL));
    DWORD g = 0xdead;
    printf("  GetNumChannelGroups %08lx %lu\n", IDirectMusicPort_GetNumChannelGroups(p, &g), g);
    DWORD pr = 0xdead;
    for (DWORD ch = 0; ch < 17; ch++) {
        pr = 0xdead;
        HRESULT hr = IDirectMusicPort_GetChannelPriority(p, 1, ch, &pr);
        printf("  GetChannelPriority(1, %lu) %08lx %lx\n", ch, hr, pr);
    }
    printf("  GetChannelPriority(2, 0) %08lx %lx\n", IDirectMusicPort_GetChannelPriority(p, 2, 0, &pr), pr);
    pr = 0xdead;
    printf("  GetChannelPriority(99, 0) %08lx %lx\n", IDirectMusicPort_GetChannelPriority(p, 99, 0, &pr), pr);
    printf("  SetChannelPriority(1, 3, 0x1234) %08lx\n", IDirectMusicPort_SetChannelPriority(p, 1, 3, 0x1234));
    printf("  GetChannelPriority(1, 3) %08lx %lx\n", IDirectMusicPort_GetChannelPriority(p, 1, 3, &pr), pr);
    DMUS_SYNTHSTATS st;
    memset(&st, 0, sizeof(st));
    st.dwSize = sizeof(st);
    hr = IDirectMusicPort_GetRunningStats(p, &st);
    printf("  GetRunningStats %08lx valid %lx voices %lu cpu %lu/%lu lost %lu free %lu peak %ld\n", hr, st.dwValidStats, st.dwVoices, st.dwTotalCPU, st.dwCPUPerVoice, st.dwLostNotes, st.dwFreeMemory, st.lPeakVolume);
    WAVEFORMATEX wf;
    DWORD wsz = sizeof(wf), bsz = 0;
    memset(&wf, 0, sizeof(wf));
    hr = IDirectMusicPort_GetFormat(p, &wf, &wsz, &bsz);
    printf("  GetFormat %08lx: tag %u ch %u rate %lu bits %u, size %lu, buffer %lu\n", hr, wf.wFormatTag, wf.nChannels,
           wf.nSamplesPerSec, wf.wBitsPerSample, wsz, bsz);
    wsz = 0;
    printf("  GetFormat NULL: %08lx size %lu\n", IDirectMusicPort_GetFormat(p, NULL, &wsz, NULL), wsz);
    IReferenceClock *ck = NULL;
    printf("  GetLatencyClock %08lx %s\n", IDirectMusicPort_GetLatencyClock(p, &ck), ck ? "clock" : "none");
    if (ck) IReferenceClock_Release(ck);
    printf("  Compact %08lx\n", IDirectMusicPort_Compact(p));
    printf("  PlayBuffer(NULL) %08lx\n", IDirectMusicPort_PlayBuffer(p, NULL));
    printf("  Read(NULL) %08lx\n", IDirectMusicPort_Read(p, NULL));
    printf("  SetReadNotificationHandle(NULL) %08lx\n", IDirectMusicPort_SetReadNotificationHandle(p, NULL));
    printf("  DownloadInstrument(NULL) %08lx\n", IDirectMusicPort_DownloadInstrument(p, NULL, NULL, NULL, 0));
    printf("  UnloadInstrument(NULL) %08lx\n", IDirectMusicPort_UnloadInstrument(p, NULL));
    DWORD got = 0;
    printf("  DeviceIoControl %08lx\n", IDirectMusicPort_DeviceIoControl(p, 0, NULL, 0, NULL, 0, &got, NULL));
    IKsControl *ks = NULL;
    if (SUCCEEDED(IDirectMusicPort_QueryInterface(p, &IID_IKsControl_, (void **)&ks))) {
        static const struct { const GUID *g; const char *n; } pp[] = {
            { &GUID_DMUS_PROP_GM_Hardware, "GM_Hardware" }, { &GUID_DMUS_PROP_GS_Hardware, "GS_Hardware" },
            { &GUID_DMUS_PROP_XG_Hardware, "XG_Hardware" }, { &GUID_DMUS_PROP_DLS1, "DLS1" },
            { &GUID_DMUS_PROP_DLS2, "DLS2" }, { &GUID_DMUS_PROP_Effects, "Effects" },
            { &GUID_DMUS_PROP_SampleMemorySize, "SampleMemorySize" }, { &GUID_DMUS_PROP_SamplePlaybackRate, "SamplePlaybackRate" },
            { &GUID_DMUS_PROP_Volume, "Volume" }, { &GUID_DMUS_PROP_SynthSink_DSOUND, "SynthSink_DSOUND" },
            { &GUID_DMUS_PROP_SynthSink_WAVE, "SynthSink_WAVE" }, { &GUID_DMUS_PROP_GM_Hardware, NULL },
        };
        for (unsigned k = 0; pp[k].n; k++) {
            KSPROPERTY q = { { { *pp[k].g, 0, KSPROPERTY_TYPE_GET } } };
            DWORD v = 0xdead; ULONG ret = 0;
            HRESULT h = ks->lpVtbl->KsProperty(ks, &q, sizeof(q), &v, sizeof(v), &ret);
            printf("  KsProperty %s: %08lx %lx (%lu bytes)\n", pp[k].n, h, v, ret);
        }
        ks->lpVtbl->Release(ks);
    }
    IDirectMusicPortDownload *dl = NULL;
    if (SUCCEEDED(IDirectMusicPort_QueryInterface(p, &IID_IDirectMusicPortDownload, (void **)&dl))) {
        DWORD a = 0xdead;
        printf("  GetAppend %08lx %lu\n", IDirectMusicPortDownload_GetAppend(dl, &a), a);
        IDirectMusicPortDownload_Release(dl);
    }
}

/* IDirectMusicPortDownload as an application uses it to download its own instruments (Pro Evolution Soccer 6) */
static IDirectMusicDownload *dlbuf(IDirectMusicPortDownload *dl, DWORD size, void **data, const char *what)
{
    IDirectMusicDownload *d = NULL;
    HRESULT hr = IDirectMusicPortDownload_AllocateBuffer(dl, size, &d);
    DWORD got = 0;
    *data = NULL;
    if (d) { HRESULT h2 = d->lpVtbl->GetBuffer(d, data, &got); printf("  AllocateBuffer(%lu) for %s %08lx, GetBuffer %08lx %lu bytes\n", size, what, hr, h2, got); }
    else printf("  AllocateBuffer(%lu) for %s %08lx\n", size, what, hr);
    return d;
}
static void download(IDirectMusicPort *p)
{
    IDirectMusicPortDownload *dl = NULL;
    if (FAILED(IDirectMusicPort_QueryInterface(p, &IID_IDirectMusicPortDownload, (void **)&dl))) { printf("  no PortDownload\n"); return; }
    IDirectMusicDownload *d = NULL;
    void *v;
    printf("  AllocateBuffer(0) %08lx\n", IDirectMusicPortDownload_AllocateBuffer(dl, 0, &d));
    if (d) d->lpVtbl->Release(d), d = NULL;
    printf("  AllocateBuffer(NULL) %08lx\n", IDirectMusicPortDownload_AllocateBuffer(dl, 16, NULL));
    DWORD id0 = 0xdead, id1 = 0xdead, id2 = 0xdead;
    printf("  GetDLId(0 ids) %08lx\n", IDirectMusicPortDownload_GetDLId(dl, &id0, 0));
    HRESULT hr = IDirectMusicPortDownload_GetDLId(dl, &id1, 2);
    HRESULT hr2 = IDirectMusicPortDownload_GetDLId(dl, &id2, 1);
    printf("  GetDLId(2) %08lx, then GetDLId(1) %08lx: %ld apart\n", hr, hr2, (long)(id2 - id1));
    printf("  GetBuffer(an id never used) %08lx\n", IDirectMusicPortDownload_GetBuffer(dl, id1, &d));
    if (d) d->lpVtbl->Release(d), d = NULL;
    printf("  Download(NULL) %08lx, Unload(NULL) %08lx\n", IDirectMusicPortDownload_Download(dl, NULL), IDirectMusicPortDownload_Unload(dl, NULL));
    /* a wave: DMUS_DOWNLOADINFO, 2 offsets, DMUS_WAVE, DMUS_WAVEDATA (a 100 Hz square, 0.5 s at 22050 Hz) */
    const DWORD frames = 11025, wsize = 16 + 8 + sizeof(DMUS_WAVE) + 4 + frames * 2;
    IDirectMusicDownload *wd = dlbuf(dl, wsize, &v, "a wave");
    if (!wd) { IDirectMusicPortDownload_Release(dl); return; }
    BYTE *b = v;
    DMUS_DOWNLOADINFO *info = (DMUS_DOWNLOADINFO *)b;
    info->dwDLType = DMUS_DOWNLOADINFO_WAVE; info->dwDLId = id1; info->dwNumOffsetTableEntries = 2; info->cbSize = wsize;
    ULONG *ofs = (ULONG *)(b + 16);
    ofs[0] = 24; ofs[1] = 24 + sizeof(DMUS_WAVE);
    DMUS_WAVE *w = (DMUS_WAVE *)(b + ofs[0]);
    w->ulWaveDataIdx = 1;
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0 };
    w->WaveformatEx = f;
    DMUS_WAVEDATA *wdat = (DMUS_WAVEDATA *)(b + ofs[1]);
    wdat->cbSize = frames * 2;
    short *smp = (short *)wdat->byData;
    for (DWORD k = 0; k < frames; k++) smp[k] = (k / 110) & 1 ? 12000 : -12000;
    printf("  Download(wave) %08lx\n", IDirectMusicPortDownload_Download(dl, wd));
    printf("  Download(wave) again %08lx\n", IDirectMusicPortDownload_Download(dl, wd));
    IDirectMusicDownload *same = NULL;
    hr = IDirectMusicPortDownload_GetBuffer(dl, id1, &same);
    printf("  GetBuffer(the wave's id) %08lx: %s\n", hr, same ? (same == wd ? "the same buffer" : "another") : "none");
    if (same) same->lpVtbl->Release(same);
    /* an instrument on that wave: DMUS_INSTRUMENT, one DMUS_REGION, then (in the second) DMUS_ARTICULATION + DMUS_ARTICPARAMS */
    for (int art = 0; art < 2; art++) {
        DWORD isize = 16 + 16 + sizeof(DMUS_INSTRUMENT) + sizeof(DMUS_REGION) + sizeof(DMUS_ARTICULATION) + sizeof(DMUS_ARTICPARAMS);
        IDirectMusicDownload *id = dlbuf(dl, isize, &v, art ? "an instrument with an articulation" : "an instrument without");
        if (!id) continue;
        b = v;
        info = (DMUS_DOWNLOADINFO *)b;
        info->dwDLType = DMUS_DOWNLOADINFO_INSTRUMENT; info->dwDLId = id1 + 1 + art * 0; info->dwNumOffsetTableEntries = art ? 4 : 2; info->cbSize = isize;
        if (art) info->dwDLId = id2;
        ofs = (ULONG *)(b + 16);
        ofs[0] = 32; ofs[1] = ofs[0] + sizeof(DMUS_INSTRUMENT); ofs[2] = ofs[1] + sizeof(DMUS_REGION); ofs[3] = ofs[2] + sizeof(DMUS_ARTICULATION);
        DMUS_INSTRUMENT *in = (DMUS_INSTRUMENT *)(b + ofs[0]);
        in->ulPatch = art ? 5 : 4; in->ulFirstRegionIdx = 1; in->ulGlobalArtIdx = art ? 2 : 0;
        DMUS_REGION *r = (DMUS_REGION *)(b + ofs[1]);
        r->RangeKey.usLow = 0; r->RangeKey.usHigh = 127; r->RangeVelocity.usLow = 0; r->RangeVelocity.usHigh = 127;
        r->WaveLink.ulChannel = 1; r->WaveLink.ulTableIndex = id1;
        r->WSMP.cbSize = sizeof(WSMPL); r->WSMP.usUnityNote = 60; r->WSMP.cSampleLoops = 1;
        r->WLOOP[0].cbSize = sizeof(WLOOP); r->WLOOP[0].ulStart = 0; r->WLOOP[0].ulLength = frames;
        if (art) {
            DMUS_ARTICULATION *a = (DMUS_ARTICULATION *)(b + ofs[2]);
            a->ulArt1Idx = 3;
            DMUS_ARTICPARAMS *ap = (DMUS_ARTICPARAMS *)(b + ofs[3]);
            ap->VolEG.tcAttack = 0x80000000; ap->VolEG.tcDecay = 0x80000000; ap->VolEG.ptSustain = 1000 << 16; ap->VolEG.tcRelease = 0x80000000;
            ap->PitchEG.tcAttack = 0x80000000; ap->PitchEG.tcDecay = 0x80000000; ap->PitchEG.tcRelease = 0x80000000;
        }
        HRESULT h = IDirectMusicPortDownload_Download(dl, id);
        printf("  Download(instrument%s) %08lx\n", art ? " with articulation" : "", h);
        if (SUCCEEDED(h) && art) {
            printf("  Unload(instrument) %08lx\n", IDirectMusicPortDownload_Unload(dl, id));
        }
        printf("  Unload(instrument) again %08lx\n", IDirectMusicPortDownload_Unload(dl, id));
        id->lpVtbl->Release(id);
    }
    printf("  Unload(wave) %08lx, again %08lx\n", IDirectMusicPortDownload_Unload(dl, wd), IDirectMusicPortDownload_Unload(dl, wd));
    printf("  GetBuffer(the wave's id) after %08lx\n", IDirectMusicPortDownload_GetBuffer(dl, id1, &d));
    if (d) d->lpVtbl->Release(d), d = NULL;
    wd->lpVtbl->Release(wd);
    /* a buffer too small to be a download, and one of a type there is none of */
    IDirectMusicDownload *bad = dlbuf(dl, 8, &v, "8 bytes");
    if (bad) { printf("  Download(8 bytes) %08lx\n", IDirectMusicPortDownload_Download(dl, bad)); bad->lpVtbl->Release(bad); }
    bad = dlbuf(dl, 64, &v, "type 99");
    if (bad) {
        DWORD *q = v; q[0] = 99; q[1] = id2 + 10; q[2] = 0; q[3] = 64;
        printf("  Download(type 99) %08lx\n", IDirectMusicPortDownload_Download(dl, bad));
        bad->lpVtbl->Release(bad);
    }
    IDirectMusicPortDownload_Release(dl);
}

/* DirectX 6/7 instrument downloads: Collection::GetInstrument, the instrument's patch, Port::DownloadInstrument /
 * UnloadInstrument and Performance::DownloadInstrument (the voices a note gets show whether it went in) */
static IDirectMusicCollection *load_coll(const char *file)
{
    IDirectMusicLoader8 *ld = NULL;
    CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&ld);
    if (!ld) return NULL;
    WCHAR dir[MAX_PATH], w[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, dir);
    IDirectMusicLoader8_SetSearchDirectory(ld, &GUID_DirectMusicAllTypes, dir, FALSE);
    MultiByteToWideChar(CP_ACP, 0, file, -1, w, MAX_PATH);
    IDirectMusicCollection *c = NULL;
    HRESULT hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicCollection, &IID_IDirectMusicCollection, w, (void **)&c);
    printf("  load %s %08lx\n", file, hr);
    IDirectMusicLoader8_Release(ld);
    return c;
}
static DWORD voices(IDirectMusicPort *p)
{
    DMUS_SYNTHSTATS st;
    memset(&st, 0, sizeof(st));
    st.dwSize = sizeof(st);
    IDirectMusicPort_GetRunningStats(p, &st);
    return st.dwVoices;
}
/* a note on the PChannel through the performance, and the voices it gets 200 ms later */
static void note(DWORD pch, BYTE prog, IDirectMusicPort *p, const char *what)
{
    DMUS_PATCH_PMSG *pm = NULL;
    if (SUCCEEDED(IDirectMusicPerformance8_AllocPMsg(perf, sizeof(*pm), (DMUS_PMSG **)&pm))) {
        pm->dwType = DMUS_PMSGT_PATCH; pm->dwFlags = DMUS_PMSGF_REFTIME; pm->dwPChannel = pch;
        pm->byInstrument = prog; pm->byMSB = 0; pm->byLSB = 0;
        IDirectMusicPerformance8_GetTime(perf, &pm->rtTime, NULL);
        if (FAILED(IDirectMusicPerformance8_SendPMsg(perf, (DMUS_PMSG *)pm))) IDirectMusicPerformance8_FreePMsg(perf, (DMUS_PMSG *)pm);
    }
    DMUS_MIDI_PMSG *m = NULL;
    if (SUCCEEDED(IDirectMusicPerformance8_AllocPMsg(perf, sizeof(*m), (DMUS_PMSG **)&m))) {
        m->dwType = DMUS_PMSGT_MIDI; m->dwFlags = DMUS_PMSGF_REFTIME; m->dwPChannel = pch;
        m->bStatus = 0x90; m->bByte1 = 60; m->bByte2 = 100;
        IDirectMusicPerformance8_GetTime(perf, &m->rtTime, NULL);
        m->rtTime += 300000;
        if (FAILED(IDirectMusicPerformance8_SendPMsg(perf, (DMUS_PMSG *)m))) IDirectMusicPerformance8_FreePMsg(perf, (DMUS_PMSG *)m);
    }
    DWORD most = 0;
    for (int k = 0; k < 15; k++) { Sleep(100); DWORD v = voices(p); if (v > most) most = v; }
    printf("  %s: %s\n", what, most ? "sounds" : "silent");
    if (SUCCEEDED(IDirectMusicPerformance8_AllocPMsg(perf, sizeof(*m), (DMUS_PMSG **)&m))) {
        m->dwType = DMUS_PMSGT_MIDI; m->dwFlags = DMUS_PMSGF_REFTIME; m->dwPChannel = pch;
        m->bStatus = 0xb0; m->bByte1 = 120; m->bByte2 = 0;         /* all sound off */
        IDirectMusicPerformance8_GetTime(perf, &m->rtTime, NULL);
        if (FAILED(IDirectMusicPerformance8_SendPMsg(perf, (DMUS_PMSG *)m))) IDirectMusicPerformance8_FreePMsg(perf, (DMUS_PMSG *)m);
    }
    Sleep(300);
}
static void instruments(void)
{
    IDirectMusicCollection *c = load_coll("fe_hard.dls");
    if (!c) return;
    DWORD patch[4] = { 0 }, n = 0;
    WCHAR wn[64];
    for (DWORD k = 0; n < 4 && IDirectMusicCollection_EnumInstrument(c, k, &patch[n], wn, 64) == S_OK; k++) n++;
    printf("  %lu patches, first %06lx\n", n, patch[0]);
    IDirectMusicInstrument *ins = NULL, *x = NULL;
    printf("  GetInstrument(0x7f7f7f) %08lx %s\n", IDirectMusicCollection_GetInstrument(c, 0x7f7f7f, &x), x ? "object" : "none");
    if (x) IDirectMusicInstrument_Release(x), x = NULL;
    HRESULT hr = IDirectMusicCollection_GetInstrument(c, patch[0], &ins);
    printf("  GetInstrument(%06lx) %08lx %s\n", patch[0], hr, ins ? "object" : "none");
    if (!ins) { IDirectMusicCollection_Release(c); return; }
    hr = IDirectMusicCollection_GetInstrument(c, patch[0], &x);
    printf("  GetInstrument again %08lx: %s\n", hr, x ? (x == ins ? "the same object" : "another object") : "none");
    if (x) IDirectMusicInstrument_Release(x), x = NULL;
    {
        static const struct { const GUID *g; const char *n; } t[] = {
            { &IID_IUnknown, "IUnknown" }, { &IID_IDirectMusicInstrument, "IDirectMusicInstrument" },
            { &IID_IDirectMusicObject, "IDirectMusicObject" }, { &IID_IPersistStream, "IPersistStream" },
            { &IID_IDirectMusicDownloadedInstrument, "IDirectMusicDownloadedInstrument" },
        };
        printf("  instrument interfaces:");
        for (unsigned k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
            IUnknown *o = NULL;
            if (SUCCEEDED(IDirectMusicInstrument_QueryInterface(ins, t[k].g, (void **)&o))) { printf(" %s", t[k].n); IUnknown_Release(o); }
        }
        printf("\n");
    }
    DWORD pt = 0xdead;
    printf("  GetPatch %08lx %06lx\n", IDirectMusicInstrument_GetPatch(ins, &pt), pt);
    printf("  SetPatch(0x123456) %08lx\n", IDirectMusicInstrument_SetPatch(ins, 0x123456));
    pt = 0xdead;
    printf("  GetPatch %08lx %06lx\n", IDirectMusicInstrument_GetPatch(ins, &pt), pt);
    printf("  SetPatch(0x80000005) %08lx\n", IDirectMusicInstrument_SetPatch(ins, 0x80000005));
    printf("  SetPatch(0xff000000) %08lx\n", IDirectMusicInstrument_SetPatch(ins, 0xff000000));
    pt = 0xdead;
    printf("  GetPatch %08lx %06lx\n", IDirectMusicInstrument_GetPatch(ins, &pt), pt);
    printf("  SetPatch(5) %08lx\n", IDirectMusicInstrument_SetPatch(ins, 5));      /* program 5, bank 0 */
    /* the port */
    IDirectMusicPort *p = NULL;
    DWORD g = 0, ch = 0;
    IDirectMusicPerformance8_PChannelInfo(perf, 0, &p, &g, &ch);
    if (!p) { IDirectMusicInstrument_Release(ins); IDirectMusicCollection_Release(c); return; }
    note(0, 5, p, "before any download");
    IDirectMusicDownloadedInstrument *d = NULL, *d2 = NULL;
    printf("  Port::DownloadInstrument(NULL, &d) %08lx\n", IDirectMusicPort_DownloadInstrument(p, NULL, &d, NULL, 0));
    printf("  Port::DownloadInstrument(ins, NULL) %08lx\n", IDirectMusicPort_DownloadInstrument(p, ins, NULL, NULL, 0));
    hr = IDirectMusicPort_DownloadInstrument(p, ins, &d, NULL, 0);
    printf("  Port::DownloadInstrument %08lx %s\n", hr, d ? "object" : "none");
    if (d) {
        static const struct { const GUID *g; const char *n; } t[] = {
            { &IID_IUnknown, "IUnknown" }, { &IID_IDirectMusicDownloadedInstrument, "IDirectMusicDownloadedInstrument" },
            { &IID_IDirectMusicInstrument, "IDirectMusicInstrument" },
        };
        printf("  downloaded interfaces:");
        for (unsigned k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
            IUnknown *o = NULL;
            if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)d, t[k].g, (void **)&o))) { printf(" %s", t[k].n); IUnknown_Release(o); }
        }
        printf("\n");
    }
    note(0, 5, p, "after the download");
    hr = IDirectMusicPort_DownloadInstrument(p, ins, &d2, NULL, 0);
    printf("  Port::DownloadInstrument again %08lx %s\n", hr, d2 ? (d2 == d ? "the same object" : "another object") : "none");
    printf("  Port::UnloadInstrument(NULL) %08lx\n", IDirectMusicPort_UnloadInstrument(p, NULL));
    if (d2) {
        printf("  Port::UnloadInstrument(second) %08lx\n", IDirectMusicPort_UnloadInstrument(p, d2));
        note(0, 5, p, "one of two unloaded");
        IDirectMusicDownloadedInstrument_Release(d2), d2 = NULL;
    }
    if (d) {
        printf("  Port::UnloadInstrument %08lx\n", IDirectMusicPort_UnloadInstrument(p, d));
        printf("  Port::UnloadInstrument again %08lx\n", IDirectMusicPort_UnloadInstrument(p, d));
        note(0, 5, p, "unloaded");
        IDirectMusicDownloadedInstrument_Release(d), d = NULL;
        Sleep(2000);
        note(0, 5, p, "unloaded, 2 s later");
        note(0, 6, p, "program 6, never downloaded");
    }
    /* what makes a download the same one: the instrument object, its patch, the ranges */
    {
        IDirectMusicDownloadedInstrument *a = NULL, *b = NULL, *e = NULL;
        IDirectMusicInstrument *other = NULL;
        IDirectMusicPort_DownloadInstrument(p, ins, &a, NULL, 0);
        IDirectMusicInstrument_SetPatch(ins, 6);
        hr = IDirectMusicPort_DownloadInstrument(p, ins, &b, NULL, 0);
        printf("  after SetPatch(6): DownloadInstrument %08lx %s\n", hr, b ? (b == a ? "the same object" : "another object") : "none");
        note(0, 6, p, "program 6");
        IDirectMusicInstrument_SetPatch(ins, 5);
        DMUS_NOTERANGE r2 = { 0, 127 };
        hr = IDirectMusicPort_DownloadInstrument(p, ins, &e, &r2, 1);
        printf("  with a range: DownloadInstrument %08lx %s\n", hr, e ? (e == a ? "the same object" : "another object") : "none");
        if (n > 1 && SUCCEEDED(IDirectMusicCollection_GetInstrument(c, patch[1], &other)) && other) {
            IDirectMusicDownloadedInstrument *o2 = NULL;
            hr = IDirectMusicPort_DownloadInstrument(p, other, &o2, NULL, 0);
            printf("  another instrument: DownloadInstrument %08lx %s\n", hr, o2 ? (o2 == a ? "the same object" : "another object") : "none");
            if (o2) { printf("    UnloadInstrument %08lx\n", IDirectMusicPort_UnloadInstrument(p, o2)); IDirectMusicDownloadedInstrument_Release(o2); }
            IDirectMusicInstrument_Release(other);
        }
        if (e) { printf("  UnloadInstrument(with a range) %08lx\n", IDirectMusicPort_UnloadInstrument(p, e)); IDirectMusicDownloadedInstrument_Release(e); }
        if (b) { printf("  UnloadInstrument(after SetPatch) %08lx\n", IDirectMusicPort_UnloadInstrument(p, b)); IDirectMusicDownloadedInstrument_Release(b); }
        if (a) { printf("  UnloadInstrument(first) %08lx\n", IDirectMusicPort_UnloadInstrument(p, a)); IDirectMusicDownloadedInstrument_Release(a); }
        Sleep(2000);
    }
    /* note ranges: only keys 0-59 */
    DMUS_NOTERANGE r = { 0, 59 };
    hr = IDirectMusicPort_DownloadInstrument(p, ins, &d, &r, 1);
    printf("  Port::DownloadInstrument(keys 0-59) %08lx\n", hr);
    note(0, 5, p, "key 60 with keys 0-59 downloaded");
    if (d) { IDirectMusicPort_UnloadInstrument(p, d); IDirectMusicDownloadedInstrument_Release(d); d = NULL; }
    printf("  Port::DownloadInstrument(ranges NULL, 1) %08lx\n", IDirectMusicPort_DownloadInstrument(p, ins, &d, NULL, 1));
    if (d) { IDirectMusicPort_UnloadInstrument(p, d); IDirectMusicDownloadedInstrument_Release(d); d = NULL; }
    /* the performance's */
    IDirectMusicPort *pp = NULL;
    DWORD pg = 0xdead, pc = 0xdead;
    hr = IDirectMusicPerformance8_DownloadInstrument(perf, ins, 2, &d, NULL, 0, &pp, &pg, &pc);
    printf("  Performance::DownloadInstrument(PChannel 2) %08lx port %s group %lu channel %lu\n", hr,
           pp ? (pp == p ? "the same" : "another") : "none", pg, pc);
    if (pp) IDirectMusicPort_Release(pp), pp = NULL;
    note(2, 5, p, "on PChannel 2");
    if (d) { printf("  Port::UnloadInstrument %08lx\n", IDirectMusicPort_UnloadInstrument(p, d)); IDirectMusicDownloadedInstrument_Release(d); d = NULL; }
    pg = pc = 0xdead;
    hr = IDirectMusicPerformance8_DownloadInstrument(perf, ins, 5000, &d, NULL, 0, &pp, &pg, &pc);
    printf("  Performance::DownloadInstrument(PChannel 5000) %08lx port %s group %lu channel %lu\n", hr, pp ? "yes" : "none", pg, pc);
    if (pp) IDirectMusicPort_Release(pp), pp = NULL;
    if (d) { IDirectMusicPort_UnloadInstrument(p, d); IDirectMusicDownloadedInstrument_Release(d); d = NULL; }
    hr = IDirectMusicPerformance8_DownloadInstrument(perf, ins, 2, &d, NULL, 0, NULL, &pg, &pc);
    printf("  Performance::DownloadInstrument(NULL port) %08lx\n", hr);
    if (d) { IDirectMusicPort_UnloadInstrument(p, d); IDirectMusicDownloadedInstrument_Release(d); d = NULL; }
    /* the instrument and collection let go of before the download is unloaded */
    hr = IDirectMusicPort_DownloadInstrument(p, ins, &d, NULL, 0);
    printf("  Port::DownloadInstrument %08lx\n", hr);
    IDirectMusicInstrument_Release(ins); ins = NULL;
    IDirectMusicCollection_Release(c); c = NULL;
    note(0, 5, p, "instrument and collection released");
    if (d) {
        printf("  Port::UnloadInstrument %08lx\n", IDirectMusicPort_UnloadInstrument(p, d));
        IDirectMusicDownloadedInstrument_Release(d), d = NULL;
    }
    Sleep(3000);        /* (the last note's release over, and a second of statistics without it, for the sections after) */
    IDirectMusicPort_Release(p);
}

static void get_port(IDirectMusicAudioPath *ap, const char *what)
{
    IDirectMusicPort *p = NULL, *q = NULL;
    HRESULT hr = IDirectMusicAudioPath_GetObjectInPath(ap, DMUS_PCHANNEL_ALL, DMUS_PATH_PORT, 0, &GUID_All_Objects, 0,
                                                       &IID_IDirectMusicPort, (void **)&p);
    printf("%s: port %08lx\n", what, hr);
    printf("  index 1: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, DMUS_PCHANNEL_ALL, DMUS_PATH_PORT, 0, &GUID_All_Objects, 1,
           &IID_IDirectMusicPort, (void **)&q));
    if (q) IDirectMusicPort_Release(q), q = NULL;
    printf("  PChannel 3: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, 3, DMUS_PATH_PORT, 0, &GUID_All_Objects, 0,
           &IID_IDirectMusicPort, (void **)&q));
    if (q) { printf("  same object: %s\n", q == p ? "yes" : "no"); IDirectMusicPort_Release(q); q = NULL; }
    printf("  PChannel 500: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, 500, DMUS_PATH_PORT, 0, &GUID_All_Objects, 0,
           &IID_IDirectMusicPort, (void **)&q));
    if (q) IDirectMusicPort_Release(q), q = NULL;
    printf("  by GUID_Synth_Default: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, DMUS_PCHANNEL_ALL, DMUS_PATH_PORT, 0,
           &GUID_Synth_Default, 0, &IID_IDirectMusicPort, (void **)&q));
    if (q) IDirectMusicPort_Release(q), q = NULL;
    IUnknown *s = NULL;
    hr = IDirectMusicAudioPath_GetObjectInPath(ap, 0, 0x5000, 0, &GUID_All_Objects, 0, &IID_IUnknown, (void **)&s);
    printf("  sink: %08lx\n", hr);
    if (s) {
        iids("sink interfaces", s);
        dssink *k = NULL;
        IUnknown_QueryInterface(s, &IID_IDirectSoundSynthSink_, (void **)&k);
        if (k) {
            IReferenceClock *c = NULL, *c2 = NULL;
            printf("  GetLatencyClock(NULL) %08lx\n", k->lpVtbl->GetLatencyClock(k, NULL));
            hr = k->lpVtbl->GetLatencyClock(k, &c);
            IUnknown_QueryInterface(s, &IID_IReferenceClock, (void **)&c2);
            printf("  GetLatencyClock %08lx %s\n", hr, !c ? "NULL" : c == c2 ? "the sink's clock" : "another");
            if (c) {
                REFERENCE_TIME a = 0, b = 0, l = 0;
                IReferenceClock_GetTime(c, &a);
                Sleep(20);
                IReferenceClock_GetTime(c, &b);
                printf("  its time runs: %s\n", b >= a ? "yes" : "no");
                (void)l;
                IReferenceClock_Release(c);
            }
            if (c2) IReferenceClock_Release(c2);
            REFERENCE_TIME rt = 0; LONGLONG sm = 0;
            printf("  SampleToRefTime(NULL) %08lx\n", k->lpVtbl->SampleToRefTime(k, 0, NULL));
            printf("  RefToSampleTime(NULL) %08lx\n", k->lpVtbl->RefToSampleTime(k, 0, NULL));
            REFERENCE_TIME r0 = 0, r1 = 0;
            k->lpVtbl->SampleToRefTime(k, 0, &r0);
            hr = k->lpVtbl->SampleToRefTime(k, 22050, &r1);
            /* (dsound's sample clock starts where the sink last synced, reDM's at its start: compared to a few
             * samples, after the steps' truncation) */
            printf("  SampleToRefTime %08lx: 22050 samples on is %s\n", hr, r1 - r0 > 9990000 && r1 - r0 <= 10000000 ? "a second" : "not a second");
            hr = k->lpVtbl->RefToSampleTime(k, r1, &sm);
            printf("  RefToSampleTime %08lx: back to %s\n", hr, sm >= 22045 && sm <= 22050 ? "22050" : "elsewhere");
            LONGLONG s0 = 0;
            k->lpVtbl->RefToSampleTime(k, r0, &s0);
            k->lpVtbl->RefToSampleTime(k, r0 + 10000000, &sm);
            printf("  a second on is %s\n", sm - s0 >= 22045 && sm - s0 <= 22051 ? "22050 samples" : "not 22050 samples");
            REFERENCE_TIME now = 0;
            IReferenceClock *mc = NULL;
            IDirectMusicPerformance8_GetTime(perf, &now, NULL);
            k->lpVtbl->RefToSampleTime(k, now, &sm);
            k->lpVtbl->SampleToRefTime(k, sm, &rt);
            printf("  now there and back: within %s\n", now - rt >= 0 && now - rt < 10000 ? "a ms" : "no");
            (void)mc;
            DWORD n = 0xdead;
            BYTE w[64]; memset(w, 0xcc, sizeof(w));
            printf("  GetFormat(NULL, NULL) %08lx\n", k->lpVtbl->GetFormat(k, NULL, 0, NULL));
            hr = k->lpVtbl->GetFormat(k, NULL, 0, &n);
            printf("  GetFormat(NULL) %08lx size %lu\n", hr, n);
            n = 0xdead;
            hr = k->lpVtbl->GetFormat(k, (WAVEFORMATEX *)w, 4, &n);
            printf("  GetFormat(4 bytes) %08lx size %lx first %02x\n", hr, n, w[0]);
            n = 0xdead;
            hr = k->lpVtbl->GetFormat(k, (WAVEFORMATEX *)w, sizeof(w), &n);
            WAVEFORMATEX *f = (WAVEFORMATEX *)w;
            printf("  GetFormat %08lx size %lu: tag %u ch %u rate %lu avg %lu align %u bits %u cbSize %u byte18 %02x\n", hr, n,
                   f->wFormatTag, f->nChannels, f->nSamplesPerSec, f->nAvgBytesPerSec, f->nBlockAlign, f->wBitsPerSample,
                   f->cbSize, w[18]);
            hr = k->lpVtbl->GetFormat(k, (WAVEFORMATEX *)w, sizeof(w), NULL);
            printf("  GetFormat(no size out) %08lx\n", hr);
            printf("  Activate(TRUE) %08lx\n", k->lpVtbl->Activate(k, TRUE));
            k->lpVtbl->Release(k);
        }
        IUnknown_Release(s);
    }
    if (!p) return;
    /* PChannelInfo's port */
    IDirectMusicPort *pi = NULL;
    DWORD g = 0, c = 0, real = 0;
    IDirectMusicAudioPath_ConvertPChannel(ap, 3, &real);
    hr = IDirectMusicPerformance8_PChannelInfo(perf, real, &pi, &g, &c);
    printf("  PChannelInfo(%lu) %08lx group %lu channel %lu, port %s\n", real, hr, g, c, pi ? (pi == p ? "the same" : "another") : "none");
    if (pi) IDirectMusicPort_Release(pi);
    port_methods(p);
    IDirectMusicPort_Release(p);
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;
    LoadLibraryA("dsound.dll");
    CoInitialize(NULL);
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf);
    HRESULT hr = IDirectMusicPerformance8_InitAudio(perf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, DMUS_AUDIOF_ALL, NULL);
    printf("InitAudio %08lx\n", hr);
    IDirectMusicAudioPath *ap = NULL;
    if (!only || !strcmp(only, "default")) {
        IDirectMusicPerformance8_GetDefaultAudioPath(perf, &ap);
        if (ap) { get_port(ap, "default path"); IDirectMusicAudioPath_Release(ap); ap = NULL; }
    }
    if (!only || !strcmp(only, "download")) {
        IDirectMusicPort *pi = NULL;
        DWORD g = 0, c = 0;
        hr = IDirectMusicPerformance8_PChannelInfo(perf, 0, &pi, &g, &c);
        printf("download: PChannelInfo(0) %08lx\n", hr);
        if (pi) { download(pi); IDirectMusicPort_Release(pi); }
    }
    if (!only || !strcmp(only, "inst")) {
        printf("inst:\n");
        instruments();
    }
    if (!only || !strcmp(only, "std")) {
        static const DWORD types[] = { DMUS_APATH_DYNAMIC_3D, DMUS_APATH_DYNAMIC_MONO, DMUS_APATH_DYNAMIC_STEREO };
        for (int k = 0; k < 3; k++) {
            if (FAILED(IDirectMusicPerformance8_CreateStandardAudioPath(perf, types[k], 16, TRUE, &ap))) continue;
            char n[32]; sprintf(n, "standard path %lu", types[k]);
            get_port(ap, n);
            IDirectMusicAudioPath_Release(ap); ap = NULL;
        }
    }
    if (!only || !strcmp(only, "pchinfo")) {   /* PChannelInfo on PChannels the application assigned */
        IDirectMusicPort *pi = NULL;
        DWORD g = 0, c = 0;
        printf("PChannelInfo(1000) %08lx\n", IDirectMusicPerformance8_PChannelInfo(perf, 1000, &pi, &g, &c));
        if (pi) IDirectMusicPort_Release(pi), pi = NULL;
        printf("AssignPChannelBlock(10, NULL, 1) %08lx\n", IDirectMusicPerformance8_AssignPChannelBlock(perf, 10, NULL, 1));
        hr = IDirectMusicPerformance8_PChannelInfo(perf, 165, &pi, &g, &c);
        printf("PChannelInfo(165) %08lx group %lu channel %lu port %s\n", hr, g, c, pi ? "yes" : "none");
        if (pi) IDirectMusicPort_Release(pi), pi = NULL;
        hr = IDirectMusicPerformance8_PChannelInfo(perf, 165, NULL, &g, &c);
        printf("PChannelInfo(165, NULL port) %08lx\n", hr);
    }
    IDirectMusicPerformance8_CloseDown(perf);
    IDirectMusicPerformance8_Release(perf);
    CoUninitialize();
    return 0;
}
