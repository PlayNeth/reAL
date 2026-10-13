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
    if (s) { iids("sink interfaces", s); IUnknown_Release(s); }
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
