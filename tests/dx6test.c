/* dx6test - the DirectX 6/7 way of driving DirectMusic: Performance::Init with IDirectMusic::CreatePort and AddPort,
 * IDirectMusic::CreateMusicBuffer with Port::PlayBuffer, and the clocks' AdviseTime / AdvisePeriodic.  Prints every
 * HRESULT and out-parameter.  Run with tests/comcmp.sh dx6test.exe [port|buffer|play|clock]. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dmusici.h>
#include <dmusicc.h>
#include <dsound.h>
#include <stdio.h>
#include <string.h>

DEFINE_GUID(GUID_NULL_, 0,0,0,0,0,0,0,0,0,0,0);
DEFINE_GUID(GUID_random_, 0x12345678, 0x1234, 0x1234, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_DIRECTMUSIC_, 0x1a82f8bc, 0x3f8b, 0x11d2, 0xb7, 0x74, 0x00, 0x60, 0x08, 0x33, 0x16, 0xc1);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_MIDI_, 0x1d262760, 0xe957, 0x11cf, 0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00);

static IDirectMusicPerformance8 *perf;
static IDirectMusic8 *dm;

#define P(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)

static void params(const char *what, const DMUS_PORTPARAMS8 *p)
{
    P("    %s: size %lu valid %08lx voices %lu groups %lu channels %lu rate %lu effects %lx share %d features %lx\n", what,
      p->dwSize, p->dwValidParams, p->dwVoices, p->dwChannelGroups, p->dwAudioChannels, p->dwSampleRate, p->dwEffectFlags,
      p->fShare, p->dwSize >= sizeof(DMUS_PORTPARAMS8) ? p->dwFeatures : 0xdead);
}
static void describe(const char *what, IDirectMusicPort *p)
{
    DMUS_PORTCAPS c;
    memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
    HRESULT hr = IDirectMusicPort_GetCaps(p, &c);
    char d[128]; WideCharToMultiByte(CP_ACP, 0, c.wszDescription, -1, d, sizeof(d), NULL, NULL);
    P("    %s caps %08lx flags %lx synth %s class %lu type %lu groups %lu voices %lu channels %lu effects %lx \"%s\"\n", what,
      hr, c.dwFlags, IsEqualGUID(&c.guidPort, &CLSID_DirectMusicSynth) ? "yes" : "no", c.dwClass, c.dwType,
      c.dwMaxChannelGroups, c.dwMaxVoices, c.dwMaxAudioChannels, c.dwEffectFlags, d);
    DWORD n = 0xdead;
    hr = IDirectMusicPort_GetNumChannelGroups(p, &n);
    WAVEFORMATEX wf; DWORD sz = sizeof(wf), bs = 0;
    memset(&wf, 0, sizeof(wf));
    HRESULT h2 = IDirectMusicPort_GetFormat(p, &wf, &sz, &bs);
    P("    %s GetNumChannelGroups %08lx %lu GetFormat %08lx rate %lu channels %u\n", what, hr, n, h2, wf.nSamplesPerSec, wf.nChannels);
}
static void init_perf(void)
{
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf);
    IDirectMusic *d = NULL;
    HRESULT hr = IDirectMusicPerformance8_Init(perf, &d, NULL, NULL);
    P("Init %08lx dm %s\n", hr, d ? "yes" : "none");
    if (d) { IDirectMusic_QueryInterface(d, &IID_IDirectMusic8, (void **)&dm); IDirectMusic_Release(d); }
}
static void close_perf(void)
{
    if (dm) IDirectMusic8_Release(dm), dm = NULL;
    IDirectMusicPerformance8_CloseDown(perf);
    IDirectMusicPerformance8_Release(perf);
    perf = NULL;
}

/* ------------------------------------------------------------------ CreatePort, AddPort, AssignPChannelBlock */
/* each parameter alone, at and beyond its limits */
static void each_param(void)
{
    static const struct { DWORD valid; DWORD v, g, c, r, e; BOOL sh; DWORD f; const char *n; } t[] = {
        { DMUS_PORTPARAMS_VOICES, 0, 0, 0, 0, 0, 0, 0, "voices 0" },
        { DMUS_PORTPARAMS_VOICES, 5000, 0, 0, 0, 0, 0, 0, "voices 5000" },
        { DMUS_PORTPARAMS_VOICES, 1000, 0, 0, 0, 0, 0, 0, "voices 1000" },
        { DMUS_PORTPARAMS_CHANNELGROUPS, 0, 0, 0, 0, 0, 0, 0, "groups 0" },
        { DMUS_PORTPARAMS_CHANNELGROUPS, 0, 1000, 0, 0, 0, 0, 0, "groups 1000" },
        { DMUS_PORTPARAMS_CHANNELGROUPS, 0, 1001, 0, 0, 0, 0, 0, "groups 1001" },
        { DMUS_PORTPARAMS_AUDIOCHANNELS, 0, 0, 0, 0, 0, 0, 0, "channels 0" },
        { DMUS_PORTPARAMS_AUDIOCHANNELS, 0, 0, 1, 0, 0, 0, 0, "channels 1" },
        { DMUS_PORTPARAMS_AUDIOCHANNELS, 0, 0, 3, 0, 0, 0, 0, "channels 3" },
        { DMUS_PORTPARAMS_SAMPLERATE, 0, 0, 0, 0, 0, 0, 0, "rate 0" },
        { DMUS_PORTPARAMS_SAMPLERATE, 0, 0, 0, 11024, 0, 0, 0, "rate 11024" },
        { DMUS_PORTPARAMS_SAMPLERATE, 0, 0, 0, 32000, 0, 0, 0, "rate 32000" },
        { DMUS_PORTPARAMS_SAMPLERATE, 0, 0, 0, 48000, 0, 0, 0, "rate 48000" },
        { DMUS_PORTPARAMS_SAMPLERATE, 0, 0, 0, 96001, 0, 0, 0, "rate 96001" },
        { DMUS_PORTPARAMS_EFFECTS, 0, 0, 0, 0, 0, 0, 0, "effects 0" },
        { DMUS_PORTPARAMS_EFFECTS, 0, 0, 0, 0, 2, 0, 0, "effects 2" },
        { DMUS_PORTPARAMS_SHARE, 0, 0, 0, 0, 0, 1, 0, "share" },
        { DMUS_PORTPARAMS_FEATURES, 0, 0, 0, 0, 0, 0, 1, "features 1" },
        { 0x10, 0, 0, 0, 0, 0, 0, 0, "valid 0x10" },
        { 0xffffff00, 0, 0, 0, 0, 0, 0, 0, "valid 0xffffff00" },
    };
    DMUS_PORTPARAMS8 pp;
    IDirectMusicPort *x = NULL;
    HRESULT hr;
    for (unsigned k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
        memset(&pp, 0, sizeof(pp)); pp.dwSize = sizeof(pp);
        pp.dwValidParams = t[k].valid; pp.dwVoices = t[k].v; pp.dwChannelGroups = t[k].g; pp.dwAudioChannels = t[k].c;
        pp.dwSampleRate = t[k].r; pp.dwEffectFlags = t[k].e; pp.fShare = t[k].sh; pp.dwFeatures = t[k].f;
        hr = IDirectMusic8_CreatePort(dm, &CLSID_DirectMusicSynth, (DMUS_PORTPARAMS *)&pp, &x, NULL);
        P("  CreatePort(%s) %08lx %s\n", t[k].n, hr, x ? "port" : "none");
        params("back", &pp);
        if (x) {
            DWORD n = 0xdead; WAVEFORMATEX wf; DWORD sz = sizeof(wf), bs = 0;
            memset(&wf, 0, sizeof(wf));
            IDirectMusicPort_GetNumChannelGroups(x, &n);
            IDirectMusicPort_GetFormat(x, &wf, &sz, &bs);
            P("    groups %lu rate %lu channels %u\n", n, wf.nSamplesPerSec, wf.nChannels);
            IDirectMusicPort_Release(x), x = NULL;
        }
    }
    memset(&pp, 0, sizeof(pp)); pp.dwSize = sizeof(DMUS_PORTPARAMS7) - 4;
    hr = IDirectMusic8_CreatePort(dm, &CLSID_DirectMusicSynth, (DMUS_PORTPARAMS *)&pp, &x, NULL);
    P("  CreatePort(size 28) %08lx\n", hr);
    if (x) IDirectMusicPort_Release(x), x = NULL;
    memset(&pp, 0, sizeof(pp)); pp.dwSize = sizeof(pp) + 4;
    hr = IDirectMusic8_CreatePort(dm, &CLSID_DirectMusicSynth, (DMUS_PORTPARAMS *)&pp, &x, NULL);
    P("  CreatePort(size 40) %08lx\n", hr);
    if (x) IDirectMusicPort_Release(x), x = NULL;
    memset(&pp, 0, sizeof(pp)); pp.dwSize = sizeof(pp);
    x = (IDirectMusicPort *)0x1234;
    hr = IDirectMusic8_CreatePort(dm, &GUID_random_, (DMUS_PORTPARAMS *)&pp, &x, NULL);
    P("  CreatePort(another GUID) leaves the pointer %s\n", x == (IDirectMusicPort *)0x1234 ? "alone" : x ? "set" : "NULL");
}
/* which answer wins when one parameter is changed and another is not: each a parameter out of range with each in
 * range */
static void param_pairs(void)
{
    static const struct { DWORD valid; DWORD v, g, c, r, e; BOOL sh; DWORD f; } bad[] = {
        { DMUS_PORTPARAMS_VOICES, 0 }, { DMUS_PORTPARAMS_CHANNELGROUPS, 0, 0 }, { DMUS_PORTPARAMS_AUDIOCHANNELS, 0, 0, 0 },
        { DMUS_PORTPARAMS_SAMPLERATE, 0, 0, 0, 0 }, { DMUS_PORTPARAMS_EFFECTS, 0, 0, 0, 0, 2 }, { DMUS_PORTPARAMS_SHARE, 0, 0, 0, 0, 0, 1 },
    }, good[] = {
        { DMUS_PORTPARAMS_VOICES, 32 }, { DMUS_PORTPARAMS_CHANNELGROUPS, 0, 1 }, { DMUS_PORTPARAMS_AUDIOCHANNELS, 0, 0, 2 },
        { DMUS_PORTPARAMS_SAMPLERATE, 0, 0, 0, 22050 }, { DMUS_PORTPARAMS_EFFECTS, 0, 0, 0, 0, 1 }, { DMUS_PORTPARAMS_SHARE, 0, 0, 0, 0, 0, 0 },
        { DMUS_PORTPARAMS_FEATURES, 0, 0, 0, 0, 0, 0, 1 },
    };
    static const char *n[] = { "voices", "groups", "channels", "rate", "effects", "share", "features" };
    DMUS_PORTPARAMS8 pp;
    for (int i = 0; i < 6; i++) {
        P("  %-8s out of range, with each in range:", n[i]);
        for (int j = 0; j < 7; j++) {
            if (j == i) { P(" -"); continue; }
            memset(&pp, 0, sizeof(pp)); pp.dwSize = sizeof(pp);
            pp.dwValidParams = bad[i].valid | good[j].valid;
            pp.dwVoices = bad[i].v | good[j].v; pp.dwChannelGroups = bad[i].g | good[j].g;
            pp.dwAudioChannels = bad[i].c | good[j].c; pp.dwSampleRate = bad[i].r | good[j].r;
            pp.dwEffectFlags = bad[i].e | good[j].e; pp.fShare = bad[i].sh | good[j].sh; pp.dwFeatures = bad[i].f | good[j].f;
            IDirectMusicPort *x = NULL;
            HRESULT hr = IDirectMusic8_CreatePort(dm, &CLSID_DirectMusicSynth, (DMUS_PORTPARAMS *)&pp, &x, NULL);
            P(" %s=%lx", n[j], hr);
            if (x) IDirectMusicPort_Release(x);
        }
        P("\n");
    }
}
static void ports(void)
{
    init_perf();
    if (!dm) return;
    IDirectMusicPort *pi = NULL;
    DWORD g = 0xdead, c = 0xdead;
    HRESULT hr = IDirectMusicPerformance8_PChannelInfo(perf, 0, &pi, &g, &c);
    P("  PChannelInfo(0) before a port %08lx port %s group %lu channel %lu\n", hr, pi ? "yes" : "none", g, c);
    if (pi) IDirectMusicPort_Release(pi), pi = NULL;
    GUID def;
    memset(&def, 0, sizeof(def));
    hr = IDirectMusic8_GetDefaultPort(dm, &def);
    P("  GetDefaultPort %08lx synth %s\n", hr, IsEqualGUID(&def, &CLSID_DirectMusicSynth) ? "yes" : "no");
    {   /* the synth among the ports EnumPort lists (the others are the machine's MIDI devices) */
        DMUS_PORTCAPS pc;
        for (DWORD k = 0; k < 32; k++) {
            memset(&pc, 0, sizeof(pc)); pc.dwSize = sizeof(pc);
            if (IDirectMusic8_EnumPort(dm, k, &pc) != S_OK) break;
            if (!IsEqualGUID(&pc.guidPort, &CLSID_DirectMusicSynth)) continue;
            char d[128]; WideCharToMultiByte(CP_ACP, 0, pc.wszDescription, -1, d, sizeof(d), NULL, NULL);
            P("  EnumPort: the synth: flags %lx class %lu type %lu memory %lu groups %lu voices %lu channels %lu effects %lx \"%s\"\n",
              pc.dwFlags, pc.dwClass, pc.dwType, pc.dwMemorySize, pc.dwMaxChannelGroups, pc.dwMaxVoices, pc.dwMaxAudioChannels,
              pc.dwEffectFlags, d);
        }
    }

    DMUS_PORTPARAMS8 pp;
    IDirectMusicPort *a = NULL, *b = NULL, *x = NULL;
    memset(&pp, 0, sizeof(pp)); pp.dwSize = sizeof(pp);
    P("  CreatePort(synth, NULL params) %08lx\n", IDirectMusic8_CreatePort(dm, &CLSID_DirectMusicSynth, NULL, &x, NULL));
    if (x) IDirectMusicPort_Release(x), x = NULL;
    P("  CreatePort(synth, params, NULL) %08lx\n", IDirectMusic8_CreatePort(dm, &CLSID_DirectMusicSynth, (DMUS_PORTPARAMS *)&pp, NULL, NULL));
    pp.dwSize = 0;
    hr = IDirectMusic8_CreatePort(dm, &CLSID_DirectMusicSynth, (DMUS_PORTPARAMS *)&pp, &x, NULL);
    P("  CreatePort(size 0) %08lx %s\n", hr, x ? "port" : "none");
    if (x) IDirectMusicPort_Release(x), x = NULL;
    memset(&pp, 0, sizeof(pp)); pp.dwSize = sizeof(pp);
    hr = IDirectMusic8_CreatePort(dm, &GUID_random_, (DMUS_PORTPARAMS *)&pp, &x, NULL);
    P("  CreatePort(another GUID) %08lx %s\n", hr, x ? "port" : "none");
    if (x) IDirectMusicPort_Release(x), x = NULL;
    hr = IDirectMusic8_CreatePort(dm, &CLSID_DirectMusicSynth, (DMUS_PORTPARAMS *)&pp, &x, (IUnknown *)dm);
    P("  CreatePort(outer) %08lx %s\n", hr, x ? "port" : "none");
    if (x) IDirectMusicPort_Release(x), x = NULL;

    memset(&pp, 0, sizeof(pp)); pp.dwSize = sizeof(pp);
    hr = IDirectMusic8_CreatePort(dm, &GUID_NULL_, (DMUS_PORTPARAMS *)&pp, &a, NULL);
    P("  CreatePort(GUID_NULL, nothing valid) %08lx %s\n", hr, a ? "port" : "none");
    params("back", &pp);
    if (a) describe("first", a);

    memset(&pp, 0, sizeof(pp)); pp.dwSize = sizeof(DMUS_PORTPARAMS7);
    pp.dwValidParams = DMUS_PORTPARAMS_VOICES | DMUS_PORTPARAMS_CHANNELGROUPS | DMUS_PORTPARAMS_AUDIOCHANNELS |
                       DMUS_PORTPARAMS_SAMPLERATE | DMUS_PORTPARAMS_EFFECTS | DMUS_PORTPARAMS_SHARE;
    pp.dwVoices = 32; pp.dwChannelGroups = 2; pp.dwAudioChannels = 2; pp.dwSampleRate = 44100;
    pp.dwEffectFlags = DMUS_EFFECT_NONE; pp.fShare = FALSE;
    hr = IDirectMusic8_CreatePort(dm, &CLSID_DirectMusicSynth, (DMUS_PORTPARAMS *)&pp, &b, NULL);
    P("  CreatePort(synth, DX7 params) %08lx %s %s\n", hr, b ? "port" : "none", b ? (b == a ? "the same" : "another") : "");
    params("back", &pp);
    if (b) describe("second", b);

    memset(&pp, 0, sizeof(pp)); pp.dwSize = sizeof(pp);
    pp.dwValidParams = DMUS_PORTPARAMS_VOICES | DMUS_PORTPARAMS_CHANNELGROUPS | DMUS_PORTPARAMS_AUDIOCHANNELS |
                       DMUS_PORTPARAMS_SAMPLERATE | DMUS_PORTPARAMS_EFFECTS | DMUS_PORTPARAMS_SHARE | DMUS_PORTPARAMS_FEATURES;
    pp.dwVoices = 5000; pp.dwChannelGroups = 5000; pp.dwAudioChannels = 6; pp.dwSampleRate = 100000;
    pp.dwEffectFlags = 0xff; pp.fShare = TRUE; pp.dwFeatures = 0xff;
    hr = IDirectMusic8_CreatePort(dm, &CLSID_DirectMusicSynth, (DMUS_PORTPARAMS *)&pp, &x, NULL);
    P("  CreatePort(out of range) %08lx %s\n", hr, x ? "port" : "none");
    params("back", &pp);
    if (x) { describe("third", x); IDirectMusicPort_Release(x); x = NULL; }

    memset(&pp, 0, sizeof(pp)); pp.dwSize = sizeof(pp);
    pp.dwValidParams = DMUS_PORTPARAMS_SAMPLERATE | DMUS_PORTPARAMS_VOICES;
    pp.dwSampleRate = 8000; pp.dwVoices = 1;
    hr = IDirectMusic8_CreatePort(dm, &CLSID_DirectMusicSynth, (DMUS_PORTPARAMS *)&pp, &x, NULL);
    P("  CreatePort(8000 Hz, 1 voice) %08lx %s\n", hr, x ? "port" : "none");
    params("back", &pp);
    if (x) { describe("fourth", x); IDirectMusicPort_Release(x); x = NULL; }
    each_param();
    param_pairs();

    if (!a) { close_perf(); return; }
    P("  Activate %08lx\n", IDirectMusicPort_Activate(a, TRUE));
    P("  AssignPChannelBlock(0, not added) %08lx\n", IDirectMusicPerformance8_AssignPChannelBlock(perf, 0, a, 1));
    P("  RemovePort(not added) %08lx\n", IDirectMusicPerformance8_RemovePort(perf, a));
    P("  AddPort %08lx\n", IDirectMusicPerformance8_AddPort(perf, a));
    g = c = 0xdead;
    hr = IDirectMusicPerformance8_PChannelInfo(perf, 0, &pi, &g, &c);
    P("  PChannelInfo(0) after AddPort %08lx port %s group %lu channel %lu\n", hr, pi ? (pi == a ? "the same" : "another") : "none", g, c);
    if (pi) IDirectMusicPort_Release(pi), pi = NULL;
    P("  AddPort again %08lx\n", IDirectMusicPerformance8_AddPort(perf, a));
    P("  AssignPChannelBlock(0, port, 1) %08lx\n", IDirectMusicPerformance8_AssignPChannelBlock(perf, 0, a, 1));
    g = c = 0xdead;
    hr = IDirectMusicPerformance8_PChannelInfo(perf, 5, &pi, &g, &c);
    P("  PChannelInfo(5) %08lx port %s group %lu channel %lu\n", hr, pi ? (pi == a ? "the same" : "another") : "none", g, c);
    if (pi) IDirectMusicPort_Release(pi), pi = NULL;
    P("  AssignPChannelBlock(1, port, 2) %08lx\n", IDirectMusicPerformance8_AssignPChannelBlock(perf, 1, a, 2));
    P("  AssignPChannelBlock(2, port, 0) %08lx\n", IDirectMusicPerformance8_AssignPChannelBlock(perf, 2, a, 0));
    P("  AssignPChannelBlock(3, port, 1001) %08lx\n", IDirectMusicPerformance8_AssignPChannelBlock(perf, 3, a, 1001));
    g = c = 0xdead;
    hr = IDirectMusicPerformance8_PChannelInfo(perf, 20, &pi, &g, &c);
    P("  PChannelInfo(20) %08lx port %s group %lu channel %lu\n", hr, pi ? (pi == a ? "the same" : "another") : "none", g, c);
    if (pi) IDirectMusicPort_Release(pi), pi = NULL;
    DWORD n = 0xdead;
    hr = IDirectMusicPort_GetNumChannelGroups(a, &n);
    P("  GetNumChannelGroups %08lx %lu\n", hr, n);
    P("  AssignPChannel(100, port, 1, 3) %08lx\n", IDirectMusicPerformance8_AssignPChannel(perf, 100, a, 1, 3));
    if (b) {
        P("  AddPort(second) %08lx\n", IDirectMusicPerformance8_AddPort(perf, b));
        P("  AssignPChannelBlock(4, second, 1) %08lx\n", IDirectMusicPerformance8_AssignPChannelBlock(perf, 4, b, 1));
        g = c = 0xdead;
        hr = IDirectMusicPerformance8_PChannelInfo(perf, 64, &pi, &g, &c);
        P("  PChannelInfo(64) %08lx port %s group %lu channel %lu\n", hr, pi ? (pi == b ? "the second" : pi == a ? "the first" : "another") : "none", g, c);
        if (pi) IDirectMusicPort_Release(pi), pi = NULL;
        P("  RemovePort(second) %08lx\n", IDirectMusicPerformance8_RemovePort(perf, b));
        g = c = 0xdead;
        hr = IDirectMusicPerformance8_PChannelInfo(perf, 64, &pi, &g, &c);
        P("  PChannelInfo(64) after RemovePort %08lx port %s group %lu channel %lu\n", hr, pi ? (pi == b ? "the second" : pi == a ? "the first" : "another") : "none", g, c);
        if (pi) IDirectMusicPort_Release(pi), pi = NULL;
        P("  RemovePort(second) again %08lx\n", IDirectMusicPerformance8_RemovePort(perf, b));
    }
    P("  AddPort(NULL) %08lx\n", IDirectMusicPerformance8_AddPort(perf, NULL));
    close_perf();
    n = 0xdead;
    hr = IDirectMusicPort_GetNumChannelGroups(a, &n);
    P("  after CloseDown: GetNumChannelGroups %08lx %lu\n", hr, n);
    P("  after CloseDown: Activate(FALSE) %08lx\n", IDirectMusicPort_Activate(a, FALSE));
    IDirectMusicPort_Release(a);
    if (b) IDirectMusicPort_Release(b);
}

/* ------------------------------------------------------------------ CreateMusicBuffer */
static void dump(IDirectMusicBuffer *b)
{
    BYTE *raw = NULL; DWORD used = 0;
    IDirectMusicBuffer_GetRawBufferPtr(b, &raw);
    IDirectMusicBuffer_GetUsedBytes(b, &used);
    P("    raw:");
    for (DWORD k = 0; raw && k < used; k++) P("%s%02x", k % 8 ? "" : " ", raw[k]);
    P("\n");
}
static void events(IDirectMusicBuffer *b)
{
    P("    ResetReadPtr %08lx\n", IDirectMusicBuffer_ResetReadPtr(b));
    for (int k = 0; k < 20; k++) {
        REFERENCE_TIME t = 0; DWORD g = 0xdead, len = 0xdead; BYTE *d = NULL;
        HRESULT hr = IDirectMusicBuffer_GetNextEvent(b, &t, &g, &len, &d);
        if (hr != S_OK) { P("    GetNextEvent %08lx\n", hr); break; }
        P("    GetNextEvent %08lx time %ld group %lu len %lu data", hr, (long)t, g, len);
        for (DWORD j = 0; j < len && j < 8; j++) P(" %02x", d[j]);
        P("\n");
    }
}
static void buffers(void)
{
    init_perf();
    if (!dm) return;
    DMUS_BUFFERDESC d;
    IDirectMusicBuffer *b = NULL;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.cbBuffer = 32;
    P("  CreateMusicBuffer(NULL desc) %08lx\n", IDirectMusic8_CreateMusicBuffer(dm, NULL, &b, NULL));
    if (b) IDirectMusicBuffer_Release(b), b = NULL;
    P("  CreateMusicBuffer(desc, NULL) %08lx\n", IDirectMusic8_CreateMusicBuffer(dm, &d, NULL, NULL));
    d.dwSize = 0;
    P("  CreateMusicBuffer(size 0) %08lx %s\n", IDirectMusic8_CreateMusicBuffer(dm, &d, &b, NULL), b ? "buffer" : "none");
    if (b) IDirectMusicBuffer_Release(b), b = NULL;
    d.dwSize = sizeof(d) + 4;
    P("  CreateMusicBuffer(size +4) %08lx %s\n", IDirectMusic8_CreateMusicBuffer(dm, &d, &b, NULL), b ? "buffer" : "none");
    if (b) IDirectMusicBuffer_Release(b), b = NULL;
    d.dwSize = sizeof(d); d.cbBuffer = 0;
    P("  CreateMusicBuffer(0 bytes) %08lx %s\n", IDirectMusic8_CreateMusicBuffer(dm, &d, &b, NULL), b ? "buffer" : "none");
    if (b) IDirectMusicBuffer_Release(b), b = NULL;
    d.cbBuffer = 32; d.guidBufferFormat = GUID_random_;
    P("  CreateMusicBuffer(another format) %08lx %s\n", IDirectMusic8_CreateMusicBuffer(dm, &d, &b, NULL), b ? "buffer" : "none");
    if (b) {
        GUID f; memset(&f, 0, sizeof(f));
        IDirectMusicBuffer_GetBufferFormat(b, &f);
        P("    GetBufferFormat %s\n", IsEqualGUID(&f, &GUID_random_) ? "the one asked for" : "another");
        P("    PackStructured %08lx\n", IDirectMusicBuffer_PackStructured(b, 0, 1, 0x00643c90));
        IDirectMusicBuffer_Release(b), b = NULL;
    }
    d.guidBufferFormat = KSDATAFORMAT_SUBTYPE_MIDI_;
    P("  CreateMusicBuffer(MIDI format) %08lx %s\n", IDirectMusic8_CreateMusicBuffer(dm, &d, &b, NULL), b ? "buffer" : "none");
    if (b) {
        GUID f; memset(&f, 0, sizeof(f));
        HRESULT hr = IDirectMusicBuffer_GetBufferFormat(b, &f);
        P("    GetBufferFormat %08lx %s\n", hr, IsEqualGUID(&f, &KSDATAFORMAT_SUBTYPE_MIDI_) ? "MIDI" :
          IsEqualGUID(&f, &KSDATAFORMAT_SUBTYPE_DIRECTMUSIC_) ? "DirectMusic" : IsEqualGUID(&f, &GUID_NULL_) ? "GUID_NULL" : "other");
        IDirectMusicBuffer_Release(b), b = NULL;
    }
    d.guidBufferFormat = GUID_NULL_;
    P("  CreateMusicBuffer(outer) %08lx %s\n", IDirectMusic8_CreateMusicBuffer(dm, &d, &b, (IUnknown *)dm), b ? "buffer" : "none");
    if (b) IDirectMusicBuffer_Release(b), b = NULL;
    d.cbBuffer = 61;
    HRESULT hr = IDirectMusic8_CreateMusicBuffer(dm, &d, &b, NULL);
    P("  CreateMusicBuffer(61 bytes) %08lx %s\n", hr, b ? "buffer" : "none");
    if (!b) { close_perf(); return; }
    {
        static const struct { const GUID *g; const char *n; } t[] = {
            { &IID_IUnknown, "IUnknown" }, { &IID_IDirectMusicBuffer, "IDirectMusicBuffer" },
            { &IID_IDirectMusicPort, "IDirectMusicPort" },
        };
        P("    interfaces:");
        for (unsigned k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
            IUnknown *o = NULL;
            if (SUCCEEDED(IDirectMusicBuffer_QueryInterface(b, t[k].g, (void **)&o))) { P(" %s", t[k].n); IUnknown_Release(o); }
        }
        P("\n");
    }
    GUID f; memset(&f, 0, sizeof(f));
    hr = IDirectMusicBuffer_GetBufferFormat(b, &f);
    P("    GetBufferFormat %08lx %s\n", hr, IsEqualGUID(&f, &KSDATAFORMAT_SUBTYPE_MIDI_) ? "MIDI" :
      IsEqualGUID(&f, &KSDATAFORMAT_SUBTYPE_DIRECTMUSIC_) ? "DirectMusic" : IsEqualGUID(&f, &GUID_NULL_) ? "GUID_NULL" : "other");
    P("    GetBufferFormat(NULL) %08lx\n", IDirectMusicBuffer_GetBufferFormat(b, NULL));
    DWORD v = 0xdead;
    hr = IDirectMusicBuffer_GetMaxBytes(b, &v); P("    GetMaxBytes %08lx %lu\n", hr, v);
    P("    GetMaxBytes(NULL) %08lx\n", IDirectMusicBuffer_GetMaxBytes(b, NULL));
    v = 0xdead; hr = IDirectMusicBuffer_GetUsedBytes(b, &v); P("    GetUsedBytes %08lx %lu\n", hr, v);
    P("    GetUsedBytes(NULL) %08lx\n", IDirectMusicBuffer_GetUsedBytes(b, NULL));
    REFERENCE_TIME t = 0x1234;
    hr = IDirectMusicBuffer_GetStartTime(b, &t); P("    GetStartTime(empty) %08lx %ld\n", hr, (long)t);
    t = 0x1234; hr = IDirectMusicBuffer_TotalTime(b, &t); P("    TotalTime(empty) %08lx %ld\n", hr, (long)t);
    P("    GetStartTime(NULL) %08lx TotalTime(NULL) %08lx\n", IDirectMusicBuffer_GetStartTime(b, NULL), IDirectMusicBuffer_TotalTime(b, NULL));
    BYTE *raw = (BYTE *)1;
    P("    GetRawBufferPtr %08lx %s\n", IDirectMusicBuffer_GetRawBufferPtr(b, &raw), raw ? "pointer" : "NULL");
    P("    GetRawBufferPtr(NULL) %08lx\n", IDirectMusicBuffer_GetRawBufferPtr(b, NULL));
    events(b);
    P("    PackStructured(1000, 1, note on) %08lx\n", IDirectMusicBuffer_PackStructured(b, 1000, 1, 0x00643c90));
    P("    PackStructured(500, 2, note off) %08lx\n", IDirectMusicBuffer_PackStructured(b, 500, 2, 0x00003c80));
    t = 0; hr = IDirectMusicBuffer_GetStartTime(b, &t); P("    GetStartTime %08lx %ld\n", hr, (long)t);
    t = 0; hr = IDirectMusicBuffer_TotalTime(b, &t); P("    TotalTime %08lx %ld\n", hr, (long)t);
    BYTE sx[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
    P("    PackUnstructured(3000, 0, sysex 6) %08lx\n", IDirectMusicBuffer_PackUnstructured(b, 3000, 0, sizeof(sx), sx));
    P("    PackUnstructured(NULL data) %08lx\n", IDirectMusicBuffer_PackUnstructured(b, 3000, 0, 2, NULL));
    P("    PackUnstructured(0 bytes) %08lx\n", IDirectMusicBuffer_PackUnstructured(b, 3000, 0, 0, sx));
    v = 0; IDirectMusicBuffer_GetUsedBytes(b, &v); P("    GetUsedBytes %lu\n", v);
    t = 0; hr = IDirectMusicBuffer_TotalTime(b, &t); P("    TotalTime %08lx %ld\n", hr, (long)t);
    dump(b);
    P("    PackStructured(full) %08lx\n", IDirectMusicBuffer_PackStructured(b, 4000, 1, 0x00643c90));
    v = 0; IDirectMusicBuffer_GetUsedBytes(b, &v); P("    GetUsedBytes %lu\n", v);
    events(b);
    REFERENCE_TIME tt = 0; DWORD g = 0, len = 0; BYTE *dd = NULL;
    IDirectMusicBuffer_ResetReadPtr(b);
    P("    GetNextEvent(NULL time) %08lx\n", IDirectMusicBuffer_GetNextEvent(b, NULL, &g, &len, &dd));
    P("    GetNextEvent(NULL all) %08lx\n", IDirectMusicBuffer_GetNextEvent(b, NULL, NULL, NULL, NULL));
    P("    GetNextEvent %08lx\n", IDirectMusicBuffer_GetNextEvent(b, &tt, &g, &len, &dd));
    P("    SetStartTime(10000) %08lx\n", IDirectMusicBuffer_SetStartTime(b, 10000));
    t = 0; IDirectMusicBuffer_GetStartTime(b, &t); P("    GetStartTime %ld\n", (long)t);
    t = 0; IDirectMusicBuffer_TotalTime(b, &t); P("    TotalTime %ld\n", (long)t);
    events(b);
    P("    SetUsedBytes(65) %08lx\n", IDirectMusicBuffer_SetUsedBytes(b, 65));
    P("    SetUsedBytes(64) %08lx\n", IDirectMusicBuffer_SetUsedBytes(b, 64));
    P("    SetUsedBytes(24) %08lx\n", IDirectMusicBuffer_SetUsedBytes(b, 24));
    v = 0; IDirectMusicBuffer_GetUsedBytes(b, &v); P("    GetUsedBytes %lu\n", v);
    events(b);
    P("    Flush %08lx\n", IDirectMusicBuffer_Flush(b));
    v = 0xdead; IDirectMusicBuffer_GetUsedBytes(b, &v); P("    GetUsedBytes %lu\n", v);
    t = 0x1234; hr = IDirectMusicBuffer_GetStartTime(b, &t); P("    GetStartTime %08lx %ld\n", hr, (long)t);
    t = 0x1234; hr = IDirectMusicBuffer_TotalTime(b, &t); P("    TotalTime %08lx %ld\n", hr, (long)t);
    P("    PackStructured(-5, 1, 3-byte) %08lx\n", IDirectMusicBuffer_PackStructured(b, -5, 1, 0x00403cb0));
    P("    PackStructured(7, 1, 2-byte) %08lx\n", IDirectMusicBuffer_PackStructured(b, 7, 1, 0x000005c0));
    P("    PackStructured(9, 1, running status) %08lx\n", IDirectMusicBuffer_PackStructured(b, 9, 1, 0x00003c3c));
    P("    PackStructured(9, 1, 0xf8) %08lx\n", IDirectMusicBuffer_PackStructured(b, 9, 1, 0x000000f8));
    t = 0; IDirectMusicBuffer_GetStartTime(b, &t); P("    GetStartTime %ld\n", (long)t);
    t = 0; IDirectMusicBuffer_TotalTime(b, &t); P("    TotalTime %ld\n", (long)t);
    dump(b);
    events(b);
    /* unstructured events, and events at the same time */
    P("    Flush %08lx\n", IDirectMusicBuffer_Flush(b));
    P("    PackUnstructured(100, 3, sysex 6) %08lx\n", IDirectMusicBuffer_PackUnstructured(b, 100, 3, sizeof(sx), sx));
    P("    PackStructured(100, 1, a) %08lx\n", IDirectMusicBuffer_PackStructured(b, 100, 1, 0x00113c90));
    P("    PackUnstructured(50, 2, 9 bytes) %08lx\n", IDirectMusicBuffer_PackUnstructured(b, 50, 2, 9, (BYTE *)"\x90\x3c\x22\x90\x3e\x22\x90\x40\x22"));
    v = 0; IDirectMusicBuffer_GetUsedBytes(b, &v); P("    GetUsedBytes %lu\n", v);
    dump(b);
    events(b);
    P("    Flush %08lx\n", IDirectMusicBuffer_Flush(b));
    P("    PackStructured(100, 1, a) %08lx\n", IDirectMusicBuffer_PackStructured(b, 100, 1, 0x00113c90));
    P("    PackStructured(100, 2, b) %08lx\n", IDirectMusicBuffer_PackStructured(b, 100, 2, 0x00223c90));
    events(b);
    {
        static const DWORD m[] = { 0xf0, 0xf1, 0x7f7ff2, 0x7ff3, 0xf4, 0xf5, 0xf6, 0xf7, 0xff, 0x7f7fe0, 0x7fd0, 0x7f7fa0 };
        P("    PackStructured status:");
        for (unsigned k = 0; k < sizeof(m) / sizeof(m[0]); k++) {
            IDirectMusicBuffer_Flush(b);
            HRESULT h = IDirectMusicBuffer_PackStructured(b, 0, 1, m[k]);
            DWORD len = 0; REFERENCE_TIME tt; DWORD gg; BYTE *dd;
            IDirectMusicBuffer_ResetReadPtr(b);
            if (h == S_OK) IDirectMusicBuffer_GetNextEvent(b, &tt, &gg, &len, &dd);
            P(" %02lx:%08lx/%lu", m[k] & 0xff, h, len);
        }
        P("\n");
    }
    ULONG r = IDirectMusicBuffer_AddRef(b);
    P("    AddRef %lu Release %lu\n", r, IDirectMusicBuffer_Release(b));
    IDirectMusicBuffer_Release(b);
    /* a buffer is the DirectMusic object's, not the performance's */
    d.cbBuffer = 32;
    hr = IDirectMusic8_CreateMusicBuffer(dm, &d, &b, NULL);
    close_perf();
    if (b) {
        P("  after CloseDown: PackStructured %08lx\n", IDirectMusicBuffer_PackStructured(b, 0, 1, 0x00643c90));
        IDirectMusicBuffer_Release(b);
    }
}

/* ------------------------------------------------------------------ PlayBuffer */
static DWORD voices(IDirectMusicPort *p)
{
    DMUS_SYNTHSTATS st;
    memset(&st, 0, sizeof(st));
    st.dwSize = sizeof(st);
    IDirectMusicPort_GetRunningStats(p, &st);
    return st.dwVoices;
}
/* when the voices start and stop, in 100 ms steps from now */
static void watch(IDirectMusicPort *p, const char *what, int ms)
{
    DWORD t0 = GetTickCount();
    int on = -1, off = -1;
    while ((int)(GetTickCount() - t0) < ms) {
        DWORD v = voices(p);
        int at = (int)(GetTickCount() - t0);
        if (v && on < 0) on = at;
        if (!v && on >= 0 && off < 0) off = at;
        Sleep(10);
    }
    if (on < 0) P("  %s: silent\n", what);
    else if (off < 0) P("  %s: starts at ~%d00 ms, still sounding\n", what, (on + 50) / 100);
    else P("  %s: starts at ~%d00 ms, stops at ~%d00 ms\n", what, (on + 50) / 100, (off + 50) / 100);
}
static void play(void)
{
    init_perf();
    if (!dm) return;
    DMUS_PORTPARAMS8 pp;
    memset(&pp, 0, sizeof(pp)); pp.dwSize = sizeof(pp);
    if (GetEnvironmentVariableA("DX6TEST_MONO", NULL, 0)) {     /* (one audio channel, for checking reAL's mono port) */
        pp.dwValidParams = DMUS_PORTPARAMS_AUDIOCHANNELS; pp.dwAudioChannels = 1;
    }
    IDirectMusicPort *p = NULL;
    HRESULT hr = IDirectMusic8_CreatePort(dm, &GUID_NULL_, (DMUS_PORTPARAMS *)&pp, &p, NULL);
    P("  CreatePort %08lx\n", hr);
    if (!p) { close_perf(); return; }
    P("  Activate %08lx\n", IDirectMusicPort_Activate(p, TRUE));
    /* an instrument as program 0, bank 0 */
    IDirectMusicLoader8 *ld = NULL;
    CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&ld);
    IDirectMusicCollection *c = NULL;
    IDirectMusicInstrument *in = NULL;
    IDirectMusicDownloadedInstrument *di = NULL;
    if (ld) {
        WCHAR dir[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, dir);
        IDirectMusicLoader8_SetSearchDirectory(ld, &GUID_DirectMusicAllTypes, dir, FALSE);
        IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicCollection, &IID_IDirectMusicCollection, L"fe_hard.dls", (void **)&c);
    }
    DWORD patch = 0;
    if (c) {
        WCHAR nm[64];
        IDirectMusicCollection_EnumInstrument(c, 0, &patch, nm, 64);
        IDirectMusicCollection_GetInstrument(c, patch, &in);
    }
    if (in) {
        IDirectMusicInstrument_SetPatch(in, 0);
        P("  DownloadInstrument %08lx\n", IDirectMusicPort_DownloadInstrument(p, in, &di, NULL, 0));
    }
    DMUS_BUFFERDESC d;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.cbBuffer = 256;
    IDirectMusicBuffer *b = NULL;
    hr = IDirectMusic8_CreateMusicBuffer(dm, &d, &b, NULL);
    P("  CreateMusicBuffer %08lx\n", hr);
    P("  PlayBuffer(NULL) %08lx\n", IDirectMusicPort_PlayBuffer(p, NULL));
    IReferenceClock *ck = NULL;
    hr = IDirectMusicPort_GetLatencyClock(p, &ck);
    IReferenceClock *mc = NULL;
    IDirectMusic8_GetMasterClock(dm, NULL, &mc);
    if (b && ck && mc) {
        REFERENCE_TIME now = 0, lat = 0;
        IReferenceClock_GetTime(mc, &now);
        IReferenceClock_GetTime(ck, &lat);
        P("  latency clock ahead of the master clock: %s\n", lat >= now && lat - now < 2000000 ? "yes, < 200 ms" : "no");
        /* the notes: whether they sound (Microsoft's sink under Wine never lets its voices go, so their length is not
         * compared); with DX6TEST_TIMES set, when each starts, for checking reAL's timing */
        int times = GetEnvironmentVariableA("DX6TEST_TIMES", NULL, 0) != 0;
        for (int k = 0; k < 6; k++) {
            static const struct { int on, on2, group, setstart; const char *n; } t[] = {
                { 0, -1, 1, 0, "at time 0" },
                { 500, 1500, 1, 0, "500 ms on, then 1500 ms on" },
                { 300, -1, 1, 1, "start time 300 ms on" },
                { 0, -1, 2, 0, "group 2" },
                { 0, -1, 0, 0, "group 0" },
                { 0, -1, 3, 0, "group 3" },
            };
            IDirectMusicBuffer_Flush(b);
            IReferenceClock_GetTime(mc, &now);
            REFERENCE_TIME off = t[k].on && !t[k].setstart ? now : 0;
            IDirectMusicBuffer_PackStructured(b, off + (REFERENCE_TIME)(t[k].setstart ? 0 : t[k].on) * 10000, t[k].group, 0x00643c90);
            if (t[k].on2 >= 0) IDirectMusicBuffer_PackStructured(b, off + (REFERENCE_TIME)t[k].on2 * 10000, t[k].group, 0x00643e90);
            if (t[k].setstart) IDirectMusicBuffer_SetStartTime(b, now + (REFERENCE_TIME)t[k].on * 10000);
            hr = IDirectMusicPort_PlayBuffer(p, b);
            DWORD t0 = GetTickCount(), last = 0;
            int on[2] = { -1, -1 }, n = 0;
            while (GetTickCount() - t0 < 2500) {
                DWORD v = voices(p);
                int at = (int)(GetTickCount() - t0);
                if (n < 2 && v > last) on[n++] = at;
                last = v;
                Sleep(5);
            }
            P("  PlayBuffer(%s) %08lx: %s", t[k].n, hr, on[0] >= 0 ? "sounds" : "silent");
            if (times) for (int j = 0; j < n; j++) P(" [start %d ms]", on[j]);
            P("\n");
            IDirectMusicBuffer_Flush(b);
            IDirectMusicBuffer_PackStructured(b, 0, 0, 0x000078b0);
            IDirectMusicPort_PlayBuffer(p, b);
            Sleep(1500);
        }
        P("  Activate(FALSE) %08lx\n", IDirectMusicPort_Activate(p, FALSE));
        IDirectMusicBuffer_Flush(b);
        IDirectMusicBuffer_PackStructured(b, 0, 1, 0x00643c90);
        P("  PlayBuffer(inactive) %08lx\n", IDirectMusicPort_PlayBuffer(p, b));
        watch(p, "inactive", 2500);
        P("  Activate(TRUE) %08lx\n", IDirectMusicPort_Activate(p, TRUE));
        watch(p, "active again", 2500);
    }
    if (ck) IReferenceClock_Release(ck);
    if (mc) IReferenceClock_Release(mc);
    if (b) IDirectMusicBuffer_Release(b);
    if (di) { IDirectMusicPort_UnloadInstrument(p, di); IDirectMusicDownloadedInstrument_Release(di); }
    if (in) IDirectMusicInstrument_Release(in);
    if (c) IDirectMusicCollection_Release(c);
    if (ld) IDirectMusicLoader8_Release(ld);
    IDirectMusicPort_Release(p);
    close_perf();
}

/* ------------------------------------------------------------------ AdviseTime / AdvisePeriodic */
static void advise(IReferenceClock *ck, const char *what)
{
    REFERENCE_TIME now = 0;
    HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE s = CreateSemaphoreA(NULL, 0, 100, NULL);
    DWORD_PTR k = 0, k2 = 0;
    IReferenceClock_GetTime(ck, &now);
    HRESULT hr = IReferenceClock_AdviseTime(ck, now, 3000000, (HEVENT)e, &k);
    DWORD t0 = GetTickCount();
    DWORD w = WaitForSingleObject(e, 2000);
    P("  %s AdviseTime(300 ms on) %08lx cookie %s: %s at ~%lu00 ms\n", what, hr, k ? "yes" : "none",
      w == WAIT_OBJECT_0 ? "set" : "not set", (GetTickCount() - t0 + 50) / 100);
    P("    Unadvise(fired) %08lx\n", IReferenceClock_Unadvise(ck, k));
    IReferenceClock_GetTime(ck, &now);
    hr = IReferenceClock_AdviseTime(ck, now - 10000000, 0, (HEVENT)e, &k);
    w = WaitForSingleObject(e, 500);
    P("    AdviseTime(in the past) %08lx: %s\n", hr, w == WAIT_OBJECT_0 ? "set" : "not set");
    IReferenceClock_GetTime(ck, &now);
    hr = IReferenceClock_AdviseTime(ck, now + 5000000, 0, (HEVENT)e, &k);
    P("    AdviseTime(500 ms on) %08lx Unadvise %08lx", hr, IReferenceClock_Unadvise(ck, k));
    w = WaitForSingleObject(e, 800);
    P(": %s\n", w == WAIT_OBJECT_0 ? "set" : "not set");
    P("    Unadvise(again) %08lx\n", IReferenceClock_Unadvise(ck, k));
    P("    Unadvise(0) %08lx\n", IReferenceClock_Unadvise(ck, 0));
    P("    AdviseTime(NULL event) %08lx\n", IReferenceClock_AdviseTime(ck, now, 0, 0, &k2));
    P("    AdviseTime(NULL cookie) %08lx\n", IReferenceClock_AdviseTime(ck, now + 100000000, 0, (HEVENT)e, NULL));
    IReferenceClock_GetTime(ck, &now);
    hr = IReferenceClock_AdvisePeriodic(ck, now + 1000000, 1000000, (HSEMAPHORE)s, &k);
    Sleep(1050);
    LONG cnt = 0;
    while (WaitForSingleObject(s, 0) == WAIT_OBJECT_0) cnt++;
    P("    AdvisePeriodic(every 100 ms) %08lx: %s in 1 s\n", hr, cnt >= 8 && cnt <= 11 ? "8 to 11 times" : cnt ? "some times" : "never");
    P("    Unadvise %08lx\n", IReferenceClock_Unadvise(ck, k));
    Sleep(300);
    cnt = 0;
    while (WaitForSingleObject(s, 0) == WAIT_OBJECT_0) cnt++;
    P("    after Unadvise: %s\n", cnt > 1 ? "still released" : "stopped");
    P("    AdvisePeriodic(period 0) %08lx\n", IReferenceClock_AdvisePeriodic(ck, now, 0, (HSEMAPHORE)s, &k2));
    if (k2) IReferenceClock_Unadvise(ck, k2), k2 = 0;
    P("    AdvisePeriodic(NULL semaphore) %08lx\n", IReferenceClock_AdvisePeriodic(ck, now, 1000000, 0, &k2));
    if (k2) IReferenceClock_Unadvise(ck, k2), k2 = 0;
    P("    AdvisePeriodic(NULL cookie) %08lx\n", IReferenceClock_AdvisePeriodic(ck, now, 1000000, (HSEMAPHORE)s, NULL));
    /* one left set up when the clock is released */
    IReferenceClock_GetTime(ck, &now);
    hr = IReferenceClock_AdviseTime(ck, now + 2000000, 0, (HEVENT)e, &k);
    P("    AdviseTime(200 ms on, left) %08lx\n", hr);
    CloseHandle(s);
    Sleep(400);
    CloseHandle(e);
}
static void clocks(void)
{
    init_perf();
    if (!dm) return;
    IReferenceClock *mc = NULL;
    GUID g;
    HRESULT hr = IDirectMusic8_GetMasterClock(dm, &g, &mc);
    P("  GetMasterClock %08lx\n", hr);
    if (mc) { advise(mc, "master clock"); IReferenceClock_Release(mc); }
    DMUS_PORTPARAMS8 pp;
    memset(&pp, 0, sizeof(pp)); pp.dwSize = sizeof(pp);
    IDirectMusicPort *p = NULL;
    IDirectMusic8_CreatePort(dm, &GUID_NULL_, (DMUS_PORTPARAMS *)&pp, &p, NULL);
    if (p) {
        IDirectMusicPort_Activate(p, TRUE);
        IReferenceClock *ck = NULL;
        hr = IDirectMusicPort_GetLatencyClock(p, &ck);
        P("  GetLatencyClock %08lx\n", hr);
        if (ck) { advise(ck, "latency clock"); IReferenceClock_Release(ck); }
        IDirectMusicPort_Release(p);
    }
    close_perf();
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;
    LoadLibraryA("dsound.dll");
    CoInitialize(NULL);
    if (!only || !strcmp(only, "port")) { P("port:\n"); ports(); }
    if (!only || !strcmp(only, "buffer")) { P("buffer:\n"); buffers(); }
    if (!only || !strcmp(only, "play")) { P("play:\n"); play(); }
    if (!only || !strcmp(only, "clock")) { P("clock:\n"); clocks(); }
    CoUninitialize();
    return 0;
}
