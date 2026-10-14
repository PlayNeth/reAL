/* comptest: the composer (IDirectMusicComposer8) on the compose case (tools/sty/cases.py compose: cmap1.cdm, cmap2.cdm,
 * compsty.sty, template.sgt in the current directory).  time() is fixed in dmcompos, dmstyle and dmime (their
 * srand(time(NULL)) seeds) by patching their imports; reDM takes REDM_TIME.  COMP_RANDLOG=<file> logs
 * dmcompos's rand() calls with their callers. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <dmusici.h>
#include <dmusicf.h>

DEFINE_GUID(GUID_IDirectMusicChordMap_, 0xd2ac28ad, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
static const GUID GUID_NULL_ = { 0 };
#define BAR 3072
#define FIXTIME 1234567

static long __cdecl fixed_time(long *t) { if (t) *t = FIXTIME; return FIXTIME; }
static FILE *g_randlog;
static int (__cdecl *g_rand)(void);
static int __cdecl rand_log(void)
{
    int v = g_rand();
    if (g_randlog) {
        fprintf(g_randlog, "R %p %d %lu", __builtin_return_address(0), v, GetCurrentThreadId());
        if (getenv("COMP_STACK")) {
            void *fr[16];
            USHORT n = RtlCaptureStackBackTrace(1, 16, fr, NULL);
            for (USHORT k = 0; k < n; k++) fprintf(g_randlog, " %p", fr[k]);
        }
        fprintf(g_randlog, "\n");
        fflush(g_randlog);
    }
    return v;
}
static void (__cdecl *g_srand)(unsigned);
static void __cdecl srand_log(unsigned seed)
{
    g_srand(seed);
    if (g_randlog) {
        fprintf(g_randlog, "S %p %u %lu", __builtin_return_address(0), seed, GetCurrentThreadId());
        if (getenv("COMP_STACK")) {
            void *fr[16];
            USHORT n = RtlCaptureStackBackTrace(1, 16, fr, NULL);
            for (USHORT k = 0; k < n; k++) fprintf(g_randlog, " %p", fr[k]);
        }
        fprintf(g_randlog, "\n");
        fflush(g_randlog);
    }
}
static void patch_import_name(HMODULE m, const char *dll, const char *name, void *fn)
{
    BYTE *base = (BYTE *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dd->VirtualAddress); dd->VirtualAddress && d->Name; d++) {
        if (lstrcmpiA((char *)base + d->Name, dll)) continue;
        IMAGE_THUNK_DATA *on = (void *)(base + d->OriginalFirstThunk);
        void **slot = (void **)(base + d->FirstThunk);
        for (; on->u1.AddressOfData; on++, slot++) {
            if (IMAGE_SNAP_BY_ORDINAL(on->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (void *)(base + on->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, name)) continue;
            DWORD old;
            VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old);
            *slot = fn;
            VirtualProtect(slot, sizeof(void *), old, &old);
        }
    }
}

static void wout(const WCHAR *w, int max)
{
    for (int i = 0; i < max && w[i]; i++) if (w[i] < 128 && w[i] >= 32) putchar((char)w[i]); else printf("<%04x>", w[i]);
}

/* the chords and commands a segment answers */
static void dump(IDirectMusicSegment8 *sg, const char *tag)
{
    MUSIC_TIME len = -1;
    IDirectMusicSegment8_GetLength(sg, &len);
    printf("%s: length %ld\n", tag, len);
    for (DWORD k = 0; k < 12; k++) {
        IDirectMusicTrack *tr = NULL;
        IDirectMusicSegment8_GetTrack(sg, &GUID_NULL_, 0xffffffff, k, &tr);
        if (!tr) break;
        IPersistStream *ps = NULL;
        CLSID c = { 0 };
        DWORD grp = 0;
        IDirectMusicSegment8_GetTrackGroup(sg, tr, &grp);
        if (SUCCEEDED(IDirectMusicTrack_QueryInterface(tr, &IID_IPersistStream, (void **)&ps))) { IPersistStream_GetClassID(ps, &c); IPersistStream_Release(ps); }
        printf("  track %lu class %08lx group %lx\n", k, c.Data1, grp);
        IDirectMusicTrack_Release(tr);
    }
    for (MUSIC_TIME t = 0; t < len && t < 64 * BAR; ) {
        DMUS_CHORD_KEY ch;
        memset(&ch, 0xcc, sizeof(ch));
        MUSIC_TIME nx = 0;
        HRESULT hr = IDirectMusicSegment8_GetParam(sg, &GUID_ChordParam, 0xffffffff, 0, t, &nx, &ch);
        printf("  chord %ld: %08lx next %ld", t, hr, nx);
        if (hr == S_OK) {
            printf(" '");
            wout(ch.wszName, 16);
            printf("' m %u b %u key %u scale %08lx subs %u", ch.wMeasure, ch.bBeat, ch.bKey, ch.dwScale, ch.bSubChordCount);
            for (int s = 0; s < ch.bSubChordCount && s < DMUS_MAXSUBCHORD; s++)
                printf(" [%lx %lx %lx %lx %u %u]", ch.SubChordList[s].dwChordPattern, ch.SubChordList[s].dwScalePattern,
                       ch.SubChordList[s].dwInversionPoints, ch.SubChordList[s].dwLevels, ch.SubChordList[s].bChordRoot,
                       ch.SubChordList[s].bScaleRoot);
        }
        printf("\n");
        if (hr != S_OK || nx <= 0) break;
        t += nx;
    }
    for (MUSIC_TIME t = 0; t < len && t < 64 * BAR; ) {
        DMUS_COMMAND_PARAM_2 cm;
        memset(&cm, 0xcc, sizeof(cm));
        MUSIC_TIME nx = 0;
        HRESULT hr = IDirectMusicSegment8_GetParam(sg, &GUID_CommandParam2, 0xffffffff, 0, t, &nx, &cm);
        printf("  command %ld: %08lx next %ld", t, hr, nx);
        if (hr == S_OK) printf(" t %ld cmd %u groove %u range %u repeat %u", cm.mtTime, cm.bCommand, cm.bGrooveLevel, cm.bGrooveRange, cm.bRepeatMode);
        printf("\n");
        if (hr != S_OK || nx <= 0) break;
        t += nx;
    }
}

/* a template's tracks, commands and saved signposts */
static void dump_template(IDirectMusicSegment *sg0)
{
    IDirectMusicSegment8 *sg = NULL;
    IDirectMusicSegment_QueryInterface(sg0, &IID_IDirectMusicSegment8, (void **)&sg);
    MUSIC_TIME len = -1;
    IDirectMusicSegment8_GetLength(sg, &len);
    printf("  length %ld\n", len);
    for (DWORD k = 0; k < 12; k++) {
        IDirectMusicTrack *tr = NULL;
        IDirectMusicSegment8_GetTrack(sg, &GUID_NULL_, 0xffffffff, k, &tr);
        if (!tr) break;
        IPersistStream *ps = NULL;
        CLSID c = { 0 };
        DWORD grp = 0;
        IDirectMusicSegment8_GetTrackGroup(sg, tr, &grp);
        if (SUCCEEDED(IDirectMusicTrack_QueryInterface(tr, &IID_IPersistStream, (void **)&ps))) IPersistStream_GetClassID(ps, &c);
        printf("  track %lu class %08lx group %lx\n", k, c.Data1, grp);
        if (ps && c.Data1 == 0xf17e8672) {
            IStream *st = NULL;
            CreateStreamOnHGlobal(NULL, TRUE, &st);
            HRESULT hr = IPersistStream_Save(ps, st, FALSE);
            HGLOBAL hg = NULL;
            GetHGlobalFromStream(st, &hg);
            ULARGE_INTEGER sz = { 0 };
            LARGE_INTEGER zero = { 0 };
            IStream_Seek(st, zero, STREAM_SEEK_END, &sz);
            BYTE *b = GlobalLock(hg);
            printf("  signposts %08lx:", hr);
            for (ULONG q = 12; q + 12 <= sz.LowPart; q += 12)
                printf(" %lu:%lx", (ULONG)*(WORD *)(b + q + 8), *(DWORD *)(b + q + 4));
            printf("\n");
            GlobalUnlock(hg);
            IStream_Release(st);
        }
        if (ps) IPersistStream_Release(ps);
        IDirectMusicTrack_Release(tr);
    }
    printf("  commands:");
    for (MUSIC_TIME t = 0; t < len && t < 400 * BAR; ) {
        DMUS_COMMAND_PARAM_2 cm;
        memset(&cm, 0xcc, sizeof(cm));
        MUSIC_TIME nx = 0;
        HRESULT hr = IDirectMusicSegment8_GetParam(sg, &GUID_CommandParam2, 0xffffffff, 0, t, &nx, &cm);
        if (hr != S_OK) { printf(" [%ld: %08lx]", t, hr); break; }
        printf(" %ld:%u/%u/%u/%u", t / BAR, cm.bCommand, cm.bGrooveLevel, cm.bGrooveRange, cm.bRepeatMode);
        if (nx <= 0) break;
        t += nx;
    }
    printf("\n");
    IDirectMusicSegment8_Release(sg);
}

/* ComposeTemplateFromShape and ComposeSegmentFromShape over shapes, lengths, intros and ends */
static int shapes(IDirectMusicLoader8 *ld)
{
    IDirectMusicComposer *cp = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_DirectMusicComposer, NULL, CLSCTX_INPROC, &IID_IDirectMusicComposer, (void **)&cp);
    printf("create composer %08lx\n", hr);
    if (!cp) return 1;
    static const WORD meas[] = { 1, 2, 3, 8, 17, 25, 40, 100, 0, 0x8005 };
    static const int ie[][3] = { { 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 2 }, { 1, 1, 1 }, { 0, 1, 0 }, { 1, 1, 3 } };
    for (WORD shape = 0; shape < 10; shape++)
        for (int m = 0; m < 10; m++)
            for (int k = 0; k < 6; k++) {
                IDirectMusicSegment *out = (IDirectMusicSegment *)0x1;
                hr = IDirectMusicComposer_ComposeTemplateFromShape(cp, meas[m], shape, ie[k][0], ie[k][1], (WORD)ie[k][2], &out);
                printf("template shape %u measures %u intro %d end %d/%d: %08lx\n", shape, meas[m], ie[k][0], ie[k][1], ie[k][2], hr);
                if (SUCCEEDED(hr) && out && out != (IDirectMusicSegment *)0x1) { dump_template(out); IDirectMusicSegment_Release(out); }
            }
    IDirectMusicChordMap *ca = NULL, *cb = NULL;
    IDirectMusicStyle8 *st = NULL, *sb = NULL, *s3 = NULL;
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicChordMap, &IID_IDirectMusicChordMap, L"cmapa.cdm", (void **)&ca);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicChordMap, &IID_IDirectMusicChordMap, L"cmapb.cdm", (void **)&cb);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"shapesty.sty", (void **)&st);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"compsty.sty", (void **)&sb);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"shapesty3.sty", (void **)&s3);
    printf("loaded %d %d %d %d %d\n", !!ca, !!cb, !!st, !!sb, !!s3);
    static const WORD smeas[] = { 1, 4, 9, 20, 26, 40, 70 };
    for (WORD shape = 0; shape < 10; shape++)
        for (int m = 0; m < 7; m++)
            for (int k = 0; k < 4; k++) {
                IDirectMusicSegment *out = NULL;
                IDirectMusicStyle8 *sty = (m + k) % 5 == 4 ? s3 : st;
                IDirectMusicChordMap *cm = (shape + k) % 2 ? cb : ca;
                WORD act = (WORD)((shape + m + k) % 4);
                hr = IDirectMusicComposer_ComposeSegmentFromShape(cp, (IDirectMusicStyle *)sty, smeas[m], shape, act, k & 1, k >> 1, cm, &out);
                printf("segment shape %u measures %u activity %u intro %d end %d style %s map %s: %08lx\n", shape, smeas[m], act,
                       k & 1, k >> 1, sty == s3 ? "3/4" : "4/4", cm == ca ? "a" : "b", hr);
                if (out) {
                    IDirectMusicSegment8 *o8 = NULL;
                    IDirectMusicSegment_QueryInterface(out, &IID_IDirectMusicSegment8, (void **)&o8);
                    dump(o8, "composed");
                    IDirectMusicSegment8_Release(o8);
                    IDirectMusicSegment_Release(out);
                }
            }
    {   /* no default band */
        IDirectMusicSegment *out = NULL;
        hr = IDirectMusicComposer_ComposeSegmentFromShape(cp, (IDirectMusicStyle *)sb, 8, 3, 1, TRUE, TRUE, ca, &out);
        printf("segment without a band: %08lx %d\n", hr, out != NULL);
        if (out) {
            IDirectMusicSegment8 *o8 = NULL;
            IDirectMusicSegment_QueryInterface(out, &IID_IDirectMusicSegment8, (void **)&o8);
            dump(o8, "composed");
            IDirectMusicSegment8_Release(o8);
            IDirectMusicSegment_Release(out);
        }
    }
    {   /* a template from a shape, composed */
        IDirectMusicSegment *tp = NULL, *out = NULL;
        hr = IDirectMusicComposer_ComposeTemplateFromShape(cp, 30, 8, TRUE, TRUE, 2, &tp);
        printf("template for composing: %08lx\n", hr);
        if (tp) {
            for (WORD act = 0; act < 4; act++) {
                hr = IDirectMusicComposer_ComposeSegmentFromTemplate(cp, (IDirectMusicStyle *)st, tp, act, cb, &out);
                printf("composed from it %u: %08lx\n", act, hr);
                if (out) {
                    IDirectMusicSegment8 *o8 = NULL;
                    IDirectMusicSegment_QueryInterface(out, &IID_IDirectMusicSegment8, (void **)&o8);
                    dump(o8, "composed");
                    IDirectMusicSegment8_Release(o8);
                    IDirectMusicSegment_Release(out);
                    out = NULL;
                }
            }
            IDirectMusicSegment_Release(tp);
        }
    }
    hr = IDirectMusicComposer_ComposeSegmentFromShape(cp, NULL, 8, 3, 1, TRUE, TRUE, ca, NULL);
    printf("null style: %08lx\n", hr);
    if (st) IDirectMusicStyle8_Release(st);
    if (sb) IDirectMusicStyle8_Release(sb);
    if (s3) IDirectMusicStyle8_Release(s3);
    if (ca) IDirectMusicChordMap_Release(ca);
    if (cb) IDirectMusicChordMap_Release(cb);
    IDirectMusicComposer_Release(cp);
    return 0;
}

/* ChangeChordMap on composed and loaded segments */
static int change(IDirectMusicLoader8 *ld)
{
    IDirectMusicComposer *cp = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_DirectMusicComposer, NULL, CLSCTX_INPROC, &IID_IDirectMusicComposer, (void **)&cp);
    printf("create composer %08lx\n", hr);
    if (!cp) return 1;
    static const WCHAR *const maps[] = { L"cmapa.cdm", L"cmapd.cdm", L"cmape.cdm", L"cmapf.cdm", L"cmapb.cdm" };
    IDirectMusicChordMap *cm[5] = { 0 };
    for (int k = 0; k < 5; k++)
        IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicChordMap, &IID_IDirectMusicChordMap, maps[k], (void **)&cm[k]);
    printf("maps %d %d %d %d %d\n", !!cm[0], !!cm[1], !!cm[2], !!cm[3], !!cm[4]);
    static const WCHAR *const segs[] = { L"t1.sgt", L"t2.sgt", L"t3.sgt", L"t4.sgt", L"basic.sgt", L"params.sgt" };
    for (int a = 0; a < 6; a++) {
        IDirectMusicSegment8 *tp = NULL;
        hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, segs[a], (void **)&tp);
        printf("load %d %08lx\n", a, hr);
        if (!tp) continue;
        hr = IDirectMusicComposer_ChangeChordMap(cp, (IDirectMusicSegment *)tp, TRUE, cm[1]);
        printf("change the loaded one: %08lx\n", hr);
        IDirectMusicSegment *sg = NULL;
        if (a < 4) {
            hr = IDirectMusicComposer_ComposeSegmentFromTemplate(cp, NULL, (IDirectMusicSegment *)tp, 1, NULL, &sg);
            printf("composed %08lx\n", hr);
        } else IDirectMusicSegment8_QueryInterface(tp, &IID_IDirectMusicSegment, (void **)&sg);
        if (sg) {
            IDirectMusicSegment8 *o8 = NULL;
            IDirectMusicSegment_QueryInterface(sg, &IID_IDirectMusicSegment8, (void **)&o8);
            dump(o8, "before");
            for (int k = 0; k < 10; k++) {
                hr = IDirectMusicComposer_ChangeChordMap(cp, sg, k & 1, cm[(k / 2 + a) % 5]);
                printf("change %d (map %d, track scale %d): %08lx\n", k, (k / 2 + a) % 5, k & 1, hr);
                dump(o8, "changed");
            }
            IDirectMusicSegment8_Release(o8);
            IDirectMusicSegment_Release(sg);
        }
        IDirectMusicSegment8_Release(tp);
    }
    for (int k = 0; k < 5; k++) if (cm[k]) IDirectMusicChordMap_Release(cm[k]);
    IDirectMusicComposer_Release(cp);
    return 0;
}

/* a transition: dump plus its valid starts, tempo, styles and next command */
static void dump_trans(IDirectMusicSegment *sg0, IDirectMusicStyle8 *const *styles, int nst)
{
    IDirectMusicSegment8 *sg = NULL;
    IDirectMusicSegment_QueryInterface(sg0, &IID_IDirectMusicSegment8, (void **)&sg);
    dump(sg, "transition");
    MUSIC_TIME len = 0;
    IDirectMusicSegment8_GetLength(sg, &len);
    for (MUSIC_TIME t = 0; t < len && t < 64 * BAR; ) {
        MUSIC_TIME v = -1, nx = 0;
        HRESULT hr = IDirectMusicSegment8_GetParam(sg, &GUID_Valid_Start_Time, 0xffffffff, 0, t, &nx, &v);
        printf("  valid start %ld: %08lx %ld next %ld\n", t, hr, v, nx);
        if (hr != S_OK || nx <= 0) break;
        t += v + 1;
    }
    for (MUSIC_TIME t = 0; t < len; t += BAR) {
        DMUS_TEMPO_PARAM tp = { 0 };
        MUSIC_TIME nx = 0;
        HRESULT hr = IDirectMusicSegment8_GetParam(sg, &GUID_TempoParam, 0xffffffff, 0, t, &nx, &tp);
        IDirectMusicStyle *st = NULL;
        HRESULT h2 = IDirectMusicSegment8_GetParam(sg, &GUID_IDirectMusicStyle, 0xffffffff, 0, t, NULL, &st);
        int which = -1;
        for (int k = 0; k < nst; k++) if (st && (void *)st == (void *)styles[k]) which = k;
        DMUS_BAND_PARAM bp = { 0 };
        MUSIC_TIME bn = 0;
        HRESULT h3 = IDirectMusicSegment8_GetParam(sg, &GUID_BandParam, 0xffffffff, 0, t, &bn, &bp);
        printf("  at %ld: tempo %08lx %.2f t %ld next %ld; style %08lx %d; band %08lx physical %ld next %ld\n", t, hr, tp.dblTempo,
               tp.mtTime, nx, h2, which, h3, bp.mtTimePhysical, h3 == S_OK ? bn : 0);
        if (st) IDirectMusicStyle_Release(st);
        if (bp.pBand) IDirectMusicBand_Release(bp.pBand);
    }
    DMUS_COMMAND_PARAM_2 cn;
    memset(&cn, 0xcc, sizeof(cn));
    HRESULT hr = IDirectMusicSegment8_GetParam(sg, &GUID_CommandParamNext, 0xffffffff, 0, 0, NULL, &cn);
    printf("  next command %08lx %u/%u\n", hr, cn.bCommand, cn.bGrooveLevel);
    IDirectMusicGraph *g = NULL;
    hr = IDirectMusicSegment8_GetGraph(sg, &g);
    printf("  graph %08lx\n", hr);
    if (g) IDirectMusicGraph_Release(g);
    IDirectMusicSegment8_Release(sg);
}

/* ComposeTransition between composed segments, and AutoTransition with nothing playing */
static int trans(IDirectMusicLoader8 *ld)
{
    IDirectMusicComposer *cp = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_DirectMusicComposer, NULL, CLSCTX_INPROC, &IID_IDirectMusicComposer, (void **)&cp);
    printf("create composer %08lx\n", hr);
    if (!cp) return 1;
    IDirectMusicChordMap *ca = NULL, *cb = NULL;
    IDirectMusicStyle8 *st[3] = { 0 };
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicChordMap, &IID_IDirectMusicChordMap, L"cmapa.cdm", (void **)&ca);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicChordMap, &IID_IDirectMusicChordMap, L"cmapb.cdm", (void **)&cb);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"shapesty.sty", (void **)&st[0]);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"shapesty3.sty", (void **)&st[1]);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"compsty.sty", (void **)&st[2]);
    IDirectMusicSegment *seg[4] = { 0 };
    hr = IDirectMusicComposer_ComposeSegmentFromShape(cp, (IDirectMusicStyle *)st[0], 12, 3, 1, TRUE, TRUE, ca, &seg[0]);
    printf("seg0 %08lx\n", hr);
    hr = IDirectMusicComposer_ComposeSegmentFromShape(cp, (IDirectMusicStyle *)st[1], 9, 7, 2, TRUE, FALSE, cb, &seg[1]);
    printf("seg1 %08lx\n", hr);
    hr = IDirectMusicComposer_ComposeSegmentFromShape(cp, (IDirectMusicStyle *)st[0], 30, 8, 0, FALSE, TRUE, cb, &seg[2]);
    printf("seg2 %08lx\n", hr);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment, L"basic.sgt", (void **)&seg[3]);
    static const DWORD fls[] = { 0, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000, 0x1, 0x3000, 0x7000, 0x16001, 0x1001, 0x6000, 0x2001, 0x1000 | 0x14000 };
    static const int pairs[][2] = { { 0, 1 }, { 1, 0 }, { 0, 2 }, { 2, 0 }, { 0, -1 }, { -1, 1 }, { 1, 1 }, { 3, 0 }, { 0, 3 } };
    int n = 0;
    for (int pi = 0; pi < 9; pi++)
        for (WORD cmd = 0; cmd < 6; cmd++)
            for (int f = 0; f < 14; f++) {
                n++;
                if ((n % 3) && pi > 1) continue;            /* (a sample of the combinations) */
                IDirectMusicSegment *from = pairs[pi][0] >= 0 ? seg[pairs[pi][0]] : NULL;
                IDirectMusicSegment *to = pairs[pi][1] >= 0 ? seg[pairs[pi][1]] : NULL;
                MUSIC_TIME t = (n % 4) * BAR + (n % 5 == 0 ? 700 : 0);
                IDirectMusicChordMap *cm = (n % 3 == 0) ? cb : NULL;
                IDirectMusicSegment *out = (IDirectMusicSegment *)0x1;
                hr = IDirectMusicComposer_ComposeTransition(cp, from, to, t, cmd, fls[f], cm, &out);
                printf("transition %d->%d at %ld command %u flags %lx map %d: %08lx %s\n", pairs[pi][0], pairs[pi][1], t, cmd, fls[f], cm != NULL,
                       hr, out == (IDirectMusicSegment *)0x1 ? "untouched" : out ? "segment" : "none");
                if (out && out != (IDirectMusicSegment *)0x1) { dump_trans(out, st, 3); IDirectMusicSegment_Release(out); }
            }
    /* AutoTransition with nothing playing */
    IDirectMusicPerformance8 *pf = NULL;
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&pf);
    hr = IDirectMusicPerformance8_InitAudio(pf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 16, DMUS_AUDIOF_ALL, NULL);
    printf("init %08lx\n", hr);
    Sleep(300);                         /* (the performance's threads have prepared ahead) */
    for (WORD cmd = 0; cmd < 6; cmd++) {
        IDirectMusicSegment *tr = NULL;
        IDirectMusicSegmentState *s1 = NULL, *s2 = NULL;
        hr = IDirectMusicComposer_AutoTransition(cp, (IDirectMusicPerformance *)pf, seg[cmd % 3], cmd, cmd == 4 ? 0x1000 : 0x2000, NULL, &tr, &s1, &s2);
        printf("auto %u: %08lx %d %d %d\n", cmd, hr, tr != NULL, s1 != NULL, s2 != NULL);
        if (tr) { dump_trans(tr, st, 3); IDirectMusicSegment_Release(tr); }
        if (s1) IDirectMusicSegmentState_Release(s1);
        if (s2) IDirectMusicSegmentState_Release(s2);
        Sleep(100);
        IDirectMusicPerformance8_Stop(pf, NULL, NULL, 0, 0);
        Sleep(100);
        REFERENCE_TIME rq = 0; MUSIC_TIME mq = 0;
        IDirectMusicPerformance8_GetQueueTime(pf, &rq);
        IDirectMusicPerformance8_ReferenceToMusicTime(pf, rq, &mq);
        IDirectMusicSegmentState *ss = NULL;
        HRESULT h2 = IDirectMusicPerformance8_GetSegmentState(pf, &ss, mq);
        printf("  after stop: state %08lx\n", h2);
        if (getenv("COMP_DEBUG")) {
            MUSIC_TIME st0 = 0; if (ss) IDirectMusicSegmentState_GetStartTime(ss, &st0);
            fprintf(stderr, "queue mt %ld state start %ld\n", mq, st0);
        }
        if (ss) IDirectMusicSegmentState_Release(ss);
    }
    /* from a playing segment's end (DMUS_COMPOSEF_SEGMENTEND: the time does not depend on the clock) */
    for (WORD cmd = 0; cmd < 6; cmd++) {
        IDirectMusicSegmentState *ps = NULL;
        hr = IDirectMusicPerformance8_PlaySegmentEx(pf, (IUnknown *)seg[cmd % 2 ? 2 : 0], NULL, NULL, 0, 0, &ps, NULL, NULL);
        Sleep(150);
        /* (how far PlaySegmentEx plays in this thread, and so its rand() calls, depends on the clock: a new composer
         * reseeds rand()) */
        IDirectMusicComposer *c2 = NULL;
        CoCreateInstance(&CLSID_DirectMusicComposer, NULL, CLSCTX_INPROC, &IID_IDirectMusicComposer, (void **)&c2);
        IDirectMusicSegment *tr = NULL;
        IDirectMusicSegmentState *s1 = NULL, *s2 = NULL;
        hr = IDirectMusicComposer_AutoTransition(c2, (IDirectMusicPerformance *)pf, cmd == 3 ? NULL : seg[1], cmd, 0x400 | (cmd & 1 ? 0x2000 : 0x1000),
                                                 cmd == 2 ? cb : NULL, &tr, &s1, &s2);
        printf("auto from the end %u: %08lx %d %d %d\n", cmd, hr, tr != NULL, s1 != NULL, s2 != NULL);
        if (tr) { dump_trans(tr, st, 3); IDirectMusicSegment_Release(tr); }
        if (s1) IDirectMusicSegmentState_Release(s1);
        if (s2) IDirectMusicSegmentState_Release(s2);
        if (ps) IDirectMusicSegmentState_Release(ps);
        IDirectMusicComposer_Release(c2);
        IDirectMusicPerformance8_Stop(pf, NULL, NULL, 0, 0);
        Sleep(100);
    }
    IDirectMusicPerformance8_CloseDown(pf);
    IDirectMusicPerformance8_Release(pf);
    for (int k = 0; k < 4; k++) if (seg[k]) IDirectMusicSegment_Release(seg[k]);
    for (int k = 0; k < 3; k++) if (st[k]) IDirectMusicStyle8_Release(st[k]);
    if (ca) IDirectMusicChordMap_Release(ca);
    if (cb) IDirectMusicChordMap_Release(cb);
    IDirectMusicComposer_Release(cp);
    return 0;
}

/* a track's parameters, read through the track itself */
static void dump_track(IDirectMusicTrack *tr, IDirectMusicStyle8 *const *styles, IDirectMusicChordMap *const *maps)
{
    for (MUSIC_TIME t = 0; t < 40 * BAR; ) {
        DMUS_CHORD_KEY ch; MUSIC_TIME nx = 0;
        memset(&ch, 0, sizeof(ch));
        HRESULT hr = IDirectMusicTrack_GetParam(tr, &GUID_ChordParam, t, &nx, &ch);
        if (hr != S_OK) { if (t == 0) printf("    chords %08lx\n", hr); break; }
        printf("    chord %ld: '", t); wout(ch.wszName, 16);
        printf("' m %u b %u key %u scale %lx subs %u root %u next %ld\n", ch.wMeasure, ch.bBeat, ch.bKey, ch.dwScale, ch.bSubChordCount,
               ch.SubChordList[0].bChordRoot, nx);
        if (nx <= 0) break;
        t += nx;
    }
    for (MUSIC_TIME t = 0; t < 40 * BAR; ) {
        DMUS_COMMAND_PARAM_2 c; MUSIC_TIME nx = 0;
        HRESULT hr = IDirectMusicTrack_GetParam(tr, &GUID_CommandParam2, t, &nx, &c);
        if (hr != S_OK) { if (t == 0) printf("    commands %08lx\n", hr); break; }
        printf("    command %ld: t %ld %u/%u next %ld\n", t, c.mtTime, c.bCommand, c.bGrooveLevel, nx);
        if (nx <= 0) break;
        t += nx;
    }
    for (MUSIC_TIME t = 0; t < 40 * BAR; ) {
        DMUS_TEMPO_PARAM tp; MUSIC_TIME nx = 0;
        HRESULT hr = IDirectMusicTrack_GetParam(tr, &GUID_TempoParam, t, &nx, &tp);
        if (hr != S_OK) { if (t == 0) printf("    tempo %08lx\n", hr); break; }
        printf("    tempo %ld: t %ld %.2f next %ld\n", t, tp.mtTime, tp.dblTempo, nx);
        if (nx <= 0) break;
        t += nx;
    }
    for (MUSIC_TIME t = 0; t < 40 * BAR; ) {
        IDirectMusicStyle *st = NULL; MUSIC_TIME nx = 0;
        HRESULT hr = IDirectMusicTrack_GetParam(tr, &GUID_IDirectMusicStyle, t, &nx, &st);
        if (hr != S_OK) { if (t == 0) printf("    style %08lx\n", hr); break; }
        int w = -1;
        for (int k = 0; k < 3; k++) if ((void *)st == (void *)styles[k]) w = k;
        printf("    style %ld: %d next %ld\n", t, w, nx);
        if (st) IDirectMusicStyle_Release(st);
        if (nx <= 0) break;
        t += nx;
    }
    for (MUSIC_TIME t = 0; t < 40 * BAR; ) {
        IDirectMusicChordMap *m = NULL; MUSIC_TIME nx = 0;
        HRESULT hr = IDirectMusicTrack_GetParam(tr, &GUID_IDirectMusicChordMap_, t, &nx, &m);
        if (hr != S_OK) { if (t == 0) printf("    chordmap %08lx\n", hr); break; }
        int w = -1;
        for (int k = 0; k < 3; k++) if ((void *)m == (void *)maps[k]) w = k;
        printf("    chordmap %ld: %d next %ld\n", t, w, nx);
        if (m) IDirectMusicChordMap_Release(m);
        if (nx <= 0) break;
        t += nx;
    }
    for (MUSIC_TIME t = 0; t < 40 * BAR; ) {
        DMUS_BAND_PARAM bp = { 0 }; MUSIC_TIME nx = 0;
        HRESULT hr = IDirectMusicTrack_GetParam(tr, &GUID_BandParam, t, &nx, &bp);
        if (hr != S_OK) { if (t == 0) printf("    band %08lx\n", hr); break; }
        printf("    band %ld: physical %ld next %ld\n", t, bp.mtTimePhysical, nx);
        if (bp.pBand) IDirectMusicBand_Release(bp.pBand);
        if (nx <= t) break;
        t = nx;
    }
    IPersistStream *ps = NULL;
    CLSID c = { 0 };
    if (SUCCEEDED(IDirectMusicTrack_QueryInterface(tr, &IID_IPersistStream, (void **)&ps))) {
        IPersistStream_GetClassID(ps, &c);
        if (c.Data1 == 0xf17e8672) {
            IStream *st = NULL;
            CreateStreamOnHGlobal(NULL, TRUE, &st);
            HRESULT hr = IPersistStream_Save(ps, st, FALSE);
            HGLOBAL hg = NULL; GetHGlobalFromStream(st, &hg);
            ULARGE_INTEGER sz = { 0 }; LARGE_INTEGER zero = { 0 };
            IStream_Seek(st, zero, STREAM_SEEK_END, &sz);
            BYTE *b = GlobalLock(hg);
            printf("    signposts %08lx:", hr);
            for (ULONG q = 12; q + 12 <= sz.LowPart; q += 12) printf(" %ld/%u:%lx", *(LONG *)(b + q), (unsigned)*(WORD *)(b + q + 8), *(DWORD *)(b + q + 4));
            printf("\n");
            GlobalUnlock(hg);
            IStream_Release(st);
        }
        IPersistStream_Release(ps);
    }
}

/* Track8::Join on each track class, and Segment::Compose with from and to segments */
static int join(IDirectMusicLoader8 *ld)
{
    IDirectMusicComposer *cp = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_DirectMusicComposer, NULL, CLSCTX_INPROC, &IID_IDirectMusicComposer, (void **)&cp);
    printf("create composer %08lx\n", hr);
    IDirectMusicChordMap *maps[3] = { 0 };
    IDirectMusicStyle8 *st[3] = { 0 };
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicChordMap, &IID_IDirectMusicChordMap, L"cmapa.cdm", (void **)&maps[0]);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicChordMap, &IID_IDirectMusicChordMap, L"cmapb.cdm", (void **)&maps[1]);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicChordMap, &IID_IDirectMusicChordMap, L"cmapc.cdm", (void **)&maps[2]);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"compsty.sty", (void **)&st[0]);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"compsty3.sty", (void **)&st[1]);
    IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"shapesty.sty", (void **)&st[2]);
    static const WCHAR *const tn[] = { L"t1.sgt", L"t2.sgt", L"t3.sgt", L"t4.sgt", L"t6.sgt" };
    IDirectMusicSegment *t6c = NULL;
    IDirectMusicSegment *cs[6] = { 0 };
    for (int k = 0; k < 4; k++) {
        IDirectMusicSegment *tp = NULL;
        IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment, tn[k], (void **)&tp);
        hr = IDirectMusicComposer_ComposeSegmentFromTemplate(cp, NULL, tp, 1, NULL, &cs[k]);
        printf("composed %d %08lx\n", k, hr);
        if (tp) IDirectMusicSegment_Release(tp);
    }
    {
        IDirectMusicSegment *tp = NULL;
        IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment, tn[4], (void **)&tp);
        hr = IDirectMusicComposer_ComposeSegmentFromTemplate(cp, NULL, tp, 2, NULL, &t6c);
        printf("composed t6 %08lx\n", hr);
        if (tp) IDirectMusicSegment_Release(tp);
    }
    hr = IDirectMusicComposer_ComposeSegmentFromShape(cp, (IDirectMusicStyle *)st[2], 6, 3, 1, FALSE, FALSE, maps[1], &cs[4]);
    printf("shape %08lx\n", hr);
    /* Join: each track of A with B's of its class, into a result and in place, at two times */
    static const int pairs[][2] = { { 0, 1 }, { 1, 3 }, { 2, 0 }, { 4, 0 }, { 0, 4 } };
    for (int pi = 0; pi < 5; pi++) {
        IDirectMusicSegment *A = cs[pairs[pi][0]], *B = cs[pairs[pi][1]];
        if (!A || !B) continue;
        for (DWORD k = 0; k < 12; k++) {
            IDirectMusicTrack *ta = NULL, *tb = NULL;
            IDirectMusicSegment_GetTrack(A, &GUID_NULL_, 0xffffffff, k, &ta);
            if (!ta) break;
            IPersistStream *ps = NULL; CLSID c = { 0 };
            if (SUCCEEDED(IDirectMusicTrack_QueryInterface(ta, &IID_IPersistStream, (void **)&ps))) { IPersistStream_GetClassID(ps, &c); IPersistStream_Release(ps); }
            IDirectMusicSegment_GetTrack(B, &c, 0xffffffff, 0, &tb);
            printf("pair %d track %lu class %08lx: other %d\n", pi, k, c.Data1, tb != NULL);
            IDirectMusicTrack8 *t8 = NULL;
            if (tb && SUCCEEDED(IDirectMusicTrack_QueryInterface(ta, &IID_IDirectMusicTrack8, (void **)&t8))) {
                for (int w = 0; w < 2; w++) {
                    MUSIC_TIME at = w ? 3 * BAR + 512 : 2 * BAR;
                    IDirectMusicTrack *res = NULL;
                    hr = IDirectMusicTrack8_Join(t8, tb, at, (IUnknown *)A, 0xffffffff, &res);
                    printf("  join at %ld into a new track: %08lx %d\n", at, hr, res != NULL);
                    if (res) { dump_track(res, st, maps); IDirectMusicTrack_Release(res); }
                }
                IDirectMusicTrack *cl = NULL;
                IDirectMusicTrack_Clone(ta, 0, 8 * BAR, &cl);
                IDirectMusicTrack8 *c8 = NULL;
                if (cl && SUCCEEDED(IDirectMusicTrack_QueryInterface(cl, &IID_IDirectMusicTrack8, (void **)&c8))) {
                    hr = IDirectMusicTrack8_Join(c8, tb, 2 * BAR + 768, (IUnknown *)A, 1, NULL);
                    printf("  join in place: %08lx\n", hr);
                    dump_track(cl, st, maps);
                    hr = IDirectMusicTrack8_Join(c8, tb, BAR, NULL, 1, NULL);
                    printf("  join without a segment: %08lx\n", hr);
                    IDirectMusicTrack8_Release(c8);
                }
                if (cl) IDirectMusicTrack_Release(cl);
                IDirectMusicTrack8_Release(t8);
            }
            if (tb) IDirectMusicTrack_Release(tb);
            IDirectMusicTrack_Release(ta);
        }
    }
    /* Segment::Compose from and to segments */
    static const WCHAR *const cn[] = { L"c1.sgt", L"c2.sgt", L"c3.sgt", L"c4.sgt", L"c5.sgt", L"c6.sgt" };
    static const int ft[][3] = { { 0, 1, 2 * BAR + 300 }, { 0, -1, 5 * BAR }, { -1, 3, 0 }, { 1, 0, 500 }, { 2, 2, 3 * BAR }, { 0, 4, BAR }, { 4, 0, BAR },
                                 { 5, 5, 2 * BAR + 100 }, { 5, -1, 3 * BAR }, { -1, 5, 0 }, { 5, 5, 300 }, { 5, 5, 0 } };
    for (int a = 0; a < 6; a++)
        for (int f = 0; f < 12; f++) {
            if ((a < 3) != (f < 7)) continue;
            IDirectMusicSegment8 *tp = NULL;
            IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, cn[a], (void **)&tp);
            if (!tp) continue;
            IDirectMusicSegment *from = ft[f][0] == 5 ? t6c : ft[f][0] >= 0 ? cs[ft[f][0]] : NULL;
            IDirectMusicSegment *to = ft[f][1] == 5 ? t6c : ft[f][1] >= 0 ? cs[ft[f][1]] : NULL;
            IDirectMusicSegment *out = NULL;
            hr = IDirectMusicSegment8_Compose(tp, ft[f][2], from, to, &out);
            printf("compose %d from %d to %d at %d: %08lx\n", a, ft[f][0], ft[f][1], ft[f][2], hr);
            if (out) {
                IDirectMusicSegment8 *o8 = NULL;
                IDirectMusicSegment_QueryInterface(out, &IID_IDirectMusicSegment8, (void **)&o8);
                dump(o8, "composed");
                for (DWORD k = 0; k < 12; k++) {
                    IDirectMusicTrack *tr = NULL;
                    IDirectMusicSegment8_GetTrack(o8, &GUID_NULL_, 0xffffffff, k, &tr);
                    if (!tr) break;
                    printf("  track %lu\n", k);
                    dump_track(tr, st, maps);
                    IDirectMusicTrack_Release(tr);
                }
                IDirectMusicSegment8_Release(o8);
                IDirectMusicSegment_Release(out);
            }
            hr = IDirectMusicSegment8_Compose(tp, ft[f][2], from, to, NULL);
            printf("in place: %08lx\n", hr);
            dump(tp, "in place");
            IDirectMusicSegment8_Release(tp);
        }
    for (int k = 0; k < 5; k++) if (cs[k]) IDirectMusicSegment_Release(cs[k]);
    if (t6c) IDirectMusicSegment_Release(t6c);
    for (int k = 0; k < 3; k++) { if (st[k]) IDirectMusicStyle8_Release(st[k]); if (maps[k]) IDirectMusicChordMap_Release(maps[k]); }
    IDirectMusicComposer_Release(cp);
    return 0;
}

/* recomposing while playing: PLAY_COMPOSE (c7) and LOOP_COMPOSE (c8, looped) signpost tracks */
static int recomp(IDirectMusicLoader8 *ld)
{
    if (g_randlog) fprintf(g_randlog, "app thread %lu\n", GetCurrentThreadId());
    IDirectMusicPerformance8 *pf = NULL;
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&pf);
    HRESULT hr = IDirectMusicPerformance8_InitAudio(pf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 16, DMUS_AUDIOF_ALL, NULL);
    printf("init %08lx\n", hr);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    IDirectMusicPerformance8_SetNotificationHandle(pf, ev, 0);
    hr = IDirectMusicPerformance8_AddNotificationType(pf, &GUID_NOTIFICATION_RECOMPOSE);
    printf("add recompose notification %08lx\n", hr);
    static const WCHAR *const names[] = { L"c7.sgt", L"c8.sgt", L"c1.sgt" };
    for (int a = 0; a < 3; a++) {
        IDirectMusicSegment8 *sg = NULL;
        hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, names[a], (void **)&sg);
        printf("load %d %08lx\n", a, hr);
        if (!sg) continue;
        if (a == 1) IDirectMusicSegment8_SetRepeats(sg, 2);
        dump(sg, "before");
        IDirectMusicSegmentState *ss = NULL;
        /* started a second ahead so the start is always played on the transport thread (its rand) */
        REFERENCE_TIME now = 0;
        IDirectMusicPerformance8_GetTime(pf, &now, NULL);
        hr = IDirectMusicPerformance8_PlaySegmentEx(pf, (IUnknown *)sg, NULL, NULL, DMUS_SEGF_REFTIME, now + 10000000, &ss, NULL, NULL);
        printf("play %08lx\n", hr);
        /* notifications are collected as they arrive: the performance drops delivered ones after two seconds */
        /* played up to a music time (not a wall time: the clocks may run at the audio device's pace), clear of the
         * measures where patterns are chosen even with the second played ahead; notifications are collected as
         * they arrive (the performance drops delivered ones after two seconds) */
        MUSIC_TIME st = 0, mt = 0, until = a == 1 ? 13000 : a == 0 ? 3800 : 1500;
        if (ss) IDirectMusicSegmentState_GetStartTime(ss, &st);
        DMUS_NOTIFICATION_PMSG *n;
        int count = 0;
        DWORD deadline = GetTickCount() + 30000;
        for (;;) {
            while (IDirectMusicPerformance8_GetNotificationPMsg(pf, &n) == S_OK) {
                if (getenv("COMP_DEBUG")) fprintf(stderr, "notification %08lx %lu at %ld\n", n->guidNotificationType.Data1, n->dwNotificationOption, n->mtTime);
                if (IsEqualGUID(&n->guidNotificationType, &GUID_NOTIFICATION_RECOMPOSE)) {
                    printf("  recompose notification: option %lu group %lx user %d\n", n->dwNotificationOption, n->dwGroupID, n->punkUser != NULL);
                    count++;
                }
                IDirectMusicPerformance8_FreePMsg(pf, (DMUS_PMSG *)n);
            }
            IDirectMusicPerformance8_GetTime(pf, NULL, &mt);
            if (mt - st >= until || (LONG)(deadline - GetTickCount()) <= 0) break;
            WaitForSingleObject(ev, 20);
        }
        IDirectMusicPerformance8_Stop(pf, NULL, NULL, 0, 0);
        Sleep(200);
        printf("  %d recompose notifications\n", count);
        dump(sg, "after");
        if (ss) IDirectMusicSegmentState_Release(ss);
        IDirectMusicSegment8_Release(sg);
    }
    IDirectMusicPerformance8_CloseDown(pf);
    IDirectMusicPerformance8_Release(pf);
    return 0;
}

/* align: the performance's start times for DMUS_SEGF_ALIGN, the VALID_START flags, MARKER and SEGMENTEND (each played
 * as a secondary segment against a primary started at a known time), and GetResolvedTime */
static IDirectMusicSegment8 *ld_seg(IDirectMusicLoader8 *ld, const WCHAR *name)
{
    IDirectMusicSegment8 *sg = NULL;
    HRESULT hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, name, (void **)&sg);
    if (FAILED(hr)) printf("load %ls %08lx\n", name, hr);
    return sg;
}
static int align(IDirectMusicLoader8 *ld)
{
    IDirectMusicPerformance8 *pf = NULL;
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&pf);
    HRESULT hr = IDirectMusicPerformance8_InitAudio(pf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 16, DMUS_AUDIOF_ALL, NULL);
    printf("init %08lx\n", hr);
    static const WCHAR *const prims[] = { L"aprim.sgt", L"aprim3.sgt" };
    static const WCHAR *const segs[] = { L"avs.sgt", L"anovs.sgt", L"aloop.sgt" };
    static const DWORD flags[] = {
        DMUS_SEGF_MEASURE, DMUS_SEGF_BEAT, DMUS_SEGF_GRID, DMUS_SEGF_MARKER, DMUS_SEGF_SEGMENTEND,
        DMUS_SEGF_ALIGN | DMUS_SEGF_MEASURE, DMUS_SEGF_ALIGN | DMUS_SEGF_BEAT, DMUS_SEGF_ALIGN | DMUS_SEGF_GRID,
        DMUS_SEGF_ALIGN | DMUS_SEGF_MEASURE | DMUS_SEGF_VALID_START_BEAT,
        DMUS_SEGF_ALIGN | DMUS_SEGF_MEASURE | DMUS_SEGF_VALID_START_GRID,
        DMUS_SEGF_ALIGN | DMUS_SEGF_MEASURE | DMUS_SEGF_VALID_START_TICK,
        DMUS_SEGF_ALIGN | DMUS_SEGF_MEASURE | DMUS_SEGF_VALID_START_MEASURE,
        DMUS_SEGF_ALIGN | DMUS_SEGF_BEAT | DMUS_SEGF_VALID_START_TICK,
        DMUS_SEGF_ALIGN | DMUS_SEGF_MARKER, DMUS_SEGF_ALIGN | DMUS_SEGF_MARKER | DMUS_SEGF_VALID_START_BEAT,
        DMUS_SEGF_ALIGN | DMUS_SEGF_SEGMENTEND, DMUS_SEGF_ALIGN | DMUS_SEGF_SEGMENTEND | DMUS_SEGF_VALID_START_MEASURE,
        DMUS_SEGF_ALIGN, DMUS_SEGF_ALIGN | DMUS_SEGF_VALID_START_GRID,
    };
    static const MUSIC_TIME offs[] = { 0, 500, 1000, 3100, 4000, 5000, 7000, 9050, 13000 };
    for (int p = 0; p < 2; p++) {
        IDirectMusicSegment8 *pr = ld_seg(ld, prims[p]);
        IDirectMusicSegment8 *sg[3];
        for (int k = 0; k < 3; k++) sg[k] = ld_seg(ld, segs[k]);
        if (!pr || !sg[0] || !sg[1] || !sg[2]) return 1;
        IDirectMusicSegment8_SetLoopPoints(sg[2], 3072, 9216);
        MUSIC_TIME now = 0, pst = 0;
        IDirectMusicPerformance8_GetTime(pf, NULL, &now);
        IDirectMusicSegmentState *ps = NULL;
        hr = IDirectMusicPerformance8_PlaySegmentEx(pf, (IUnknown *)pr, NULL, NULL, 0, now + 3000, &ps, NULL, NULL);
        IDirectMusicSegmentState_GetStartTime(ps, &pst);
        printf("primary %ls %08lx\n", prims[p], hr);
        for (int k = 0; k < 3; k++)
            for (int sp = 0; sp < 2; sp++) {
                IDirectMusicSegment8_SetStartPoint(sg[k], sp ? 1300 : 0);
                for (unsigned f = 0; f < sizeof(flags) / sizeof(*flags); f++)
                    for (unsigned o = 0; o < sizeof(offs) / sizeof(*offs); o++) {
                        IDirectMusicSegmentState *ss = NULL;
                        hr = IDirectMusicPerformance8_PlaySegmentEx(pf, (IUnknown *)sg[k], NULL, NULL, flags[f] | DMUS_SEGF_SECONDARY,
                                                                   pst + offs[o], &ss, NULL, NULL);
                        MUSIC_TIME st = 0, spt = 0;
                        if (ss) { IDirectMusicSegmentState_GetStartTime(ss, &st); IDirectMusicSegmentState_GetStartPoint(ss, &spt); }
                        printf("  %ls sp %d flags %08lx at %d: %08lx start %d point %d\n", segs[k], sp ? 1300 : 0, flags[f], offs[o], hr,
                               ss ? (int)(st - pst) : -1, (int)spt);
                        if (ss) {
                            IDirectMusicPerformance8_StopEx(pf, (IUnknown *)ss, 0, 0);
                            IDirectMusicSegmentState_Release(ss);
                        }
                    }
            }
        for (unsigned f = 0; f < 5; f++)
            for (unsigned o = 0; o < sizeof(offs) / sizeof(*offs); o++) {
                REFERENCE_TIME rt = 0, out = 0;
                MUSIC_TIME mt = 0;
                IDirectMusicPerformance8_MusicToReferenceTime(pf, pst + offs[o], &rt);
                hr = IDirectMusicPerformance8_GetResolvedTime(pf, rt, &out, flags[f]);
                IDirectMusicPerformance8_ReferenceToMusicTime(pf, out, &mt);
                printf("  resolved flags %08lx at %d: %08lx %d\n", flags[f], offs[o], hr, (int)(mt - pst));
            }
        IDirectMusicPerformance8_Stop(pf, NULL, NULL, 0, 0);
        Sleep(100);
        IDirectMusicSegmentState_Release(ps);
        IDirectMusicSegment8_Release(pr);
        for (int k = 0; k < 3; k++) IDirectMusicSegment8_Release(sg[k]);
    }
    IDirectMusicPerformance8_CloseDown(pf);
    IDirectMusicPerformance8_Release(pf);
    return 0;
}

/* pattrack: the pattern track's parameters and IDirectMusicPatternTrack (CreateSegment, SetVariation,
 * SetPatternByName), the variations seen through the performance's GUID_Variations */
DEFINE_GUID(G_PatStyle, 0x689821f4, 0xb3bc, 0x44dd, 0x80, 0xd4, 0x0c, 0xf3, 0x2f, 0xe4, 0xd2, 0x1b);
DEFINE_GUID(G_PatPart, 0x09f25101, 0x505d, 0x4c7c, 0xbe, 0x15, 0x66, 0x2b, 0x33, 0xe7, 0x8a, 0x7c);
static void pt_vars(IDirectMusicPerformance8 *pf, MUSIC_TIME t, const char *what)
{
    DMUS_VARIATIONS_PARAM v;
    memset(&v, 0, sizeof(v));
    MUSIC_TIME nx = 0;
    HRESULT hr = IDirectMusicPerformance8_GetParam(pf, &GUID_Variations, 0xffffffff, 0, t, &nx, &v);
    printf("  %s: variations %08lx next %ld:", what, hr, nx);
    if (SUCCEEDED(hr))
        for (DWORD k = 0; k < v.dwPChannelsUsed; k++) printf(" [%lu %08lx]", v.padwPChannels[k], v.padwVariations[k]);
    printf("\n");
}
static int pattrack(IDirectMusicLoader8 *ld)
{
    IDirectMusicSegment8 *sg = ld_seg(ld, L"pattrk.sgt");
    IDirectMusicStyle *st = NULL;
    HRESULT hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle, L"pat.sty", (void **)&st);
    printf("style %08lx\n", hr);
    if (!sg || !st) return 1;
    IDirectMusicTrack *tr = NULL;
    hr = IDirectMusicSegment8_GetTrack(sg, &CLSID_DirectMusicPatternTrack, 0xffffffff, 0, &tr);
    printf("track %08lx\n", hr);
    if (!tr) return 1;
    IUnknown *u = NULL;
    IDirectMusicPatternTrack *pt = NULL;
    hr = IDirectMusicTrack_QueryInterface(tr, &IID_IDirectMusicPatternTrack, (void **)&pt);
    printf("qi pattern track %08lx\n", hr);
    hr = IDirectMusicTrack_QueryInterface(tr, &IID_IPersist, (void **)&u);
    printf("qi persist %08lx\n", hr);
    if (u) IUnknown_Release(u);
    if (!pt) return 1;
    static const GUID *const gs[] = { &GUID_Variations, &GUID_SeedVariations, &GUID_Valid_Start_Time, &G_PatStyle, &G_PatPart,
                                      &GUID_TimeSignature, &GUID_EnableTimeSig, &GUID_DisableTimeSig, &GUID_IDirectMusicStyle };
    for (int k = 0; k < 9; k++) printf("  isparam %d %08lx\n", k, IDirectMusicTrack_IsParamSupported(tr, gs[k]));
    DMUS_VARIATIONS_PARAM v;
    MUSIC_TIME nx = 0, vs = 0;
    printf("  getparam variations %08lx\n", IDirectMusicTrack_GetParam(tr, &GUID_Variations, 0, &nx, &v));
    hr = IDirectMusicTrack_GetParam(tr, &GUID_Valid_Start_Time, 0, &nx, &vs);
    printf("  getparam valid start %08lx %ld %ld\n", hr, vs, nx);
    IDirectMusicStyle *ps = NULL;
    nx = 5;
    hr = IDirectMusicTrack_GetParam(tr, &G_PatStyle, 100, &nx, &ps);
    printf("  getparam style %08lx %d next %ld\n", hr, ps != NULL, nx);
    if (ps) {
        DMUS_TIMESIGNATURE ts;
        memset(&ts, 0, sizeof(ts));
        IDirectMusicStyle_GetTimeSignature(ps, &ts);
        printf("    its signature %d/%d %d\n", ts.bBeatsPerMeasure, ts.bBeat, ts.wGridsPerBeat);
        IDirectMusicStyle_Release(ps);
    }
    DWORD pp[3] = { 1, 0, 7 };
    printf("  getparam part %08lx\n", IDirectMusicTrack_GetParam(tr, &G_PatPart, 0, NULL, pp));
    printf("  getparam timesig %08lx\n", IDirectMusicTrack_GetParam(tr, &GUID_TimeSignature, 0, NULL, &v));

    IDirectMusicPerformance8 *pf = NULL;
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&pf);
    hr = IDirectMusicPerformance8_InitAudio(pf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 16, DMUS_AUDIOF_ALL, NULL);
    printf("init %08lx\n", hr);
    /* the variations read once the state has started, well before the second chord (GUID_Variations' next is the
     * state's next chord) */
    REFERENCE_TIME now = 0;
    IDirectMusicPerformance8_GetTime(pf, &now, NULL);
    IDirectMusicSegmentState *ss = NULL;
    hr = IDirectMusicPerformance8_PlaySegmentEx(pf, (IUnknown *)sg, NULL, NULL, DMUS_SEGF_REFTIME, now + 10000000, &ss, NULL, NULL);
    MUSIC_TIME s0 = 0, mt = 0;
    IDirectMusicSegmentState_GetStartTime(ss, &s0);
    printf("play %08lx\n", hr);
    do { Sleep(20); IDirectMusicPerformance8_GetTime(pf, NULL, &mt); } while (mt < s0 + 500);
    pt_vars(pf, s0 + 100, "playing");
    hr = IDirectMusicPatternTrack_SetVariation(pt, ss, 0x5, 2);
    printf("set variation %08lx\n", hr);
    pt_vars(pf, s0 + 100, "part 2 auditioned");
    hr = IDirectMusicPatternTrack_SetVariation(pt, NULL, 0x30, 1);
    printf("set variation (no state) %08lx\n", hr);
    pt_vars(pf, s0 + 100, "unchanged");
    DWORD len = 0;
    static const struct { const WCHAR *n; DWORD type; } byname[] = { { L"MID", 0 }, { L"nope", 0 }, { L"mid", 1 }, { L"fast", 2 }, { L"fast", 0 } };
    for (int k = 0; k < 5; k++) {
        len = 12345;
        hr = IDirectMusicPatternTrack_SetPatternByName(pt, ss, (WCHAR *)byname[k].n, st, byname[k].type, &len);
        printf("set pattern %ls %lu: %08lx length %lu\n", byname[k].n, byname[k].type, hr, len);
        pt_vars(pf, s0 + 100, "after");
    }
    IDirectMusicSegment *ns = NULL;
    hr = IDirectMusicPatternTrack_CreateSegment(pt, st, &ns);
    printf("create segment %08lx\n", hr);
    if (ns) {
        MUSIC_TIME l = 0;
        DWORD res = 7;
        IDirectMusicSegment_GetLength(ns, &l);
        IDirectMusicSegment_GetDefaultResolution(ns, &res);
        printf("  length %ld resolution %lu\n", l, res);
        for (int k = 0; k < 4; k++) {
            IDirectMusicTrack *x = NULL;
            hr = IDirectMusicSegment_GetTrack(ns, &GUID_All_Objects, 0xffffffff, k, &x);
            if (!x) break;
            DWORD grp = 0;
            IDirectMusicSegment_GetTrackGroup(ns, x, &grp);
            printf("  track %d same %d group %lx\n", k, x == tr, grp);
            IDirectMusicTrack_Release(x);
        }
        IDirectMusicSegment_Release(ns);
    }
    ps = NULL;
    hr = IDirectMusicTrack_GetParam(tr, &G_PatStyle, 0, NULL, &ps);
    printf("  getparam style after %08lx\n", hr);
    if (ps) IDirectMusicStyle_Release(ps);
    IDirectMusicPerformance8_Stop(pf, NULL, NULL, 0, 0);
    Sleep(100);
    IDirectMusicSegmentState_Release(ss);
    IDirectMusicPerformance8_CloseDown(pf);
    IDirectMusicPerformance8_Release(pf);
    IDirectMusicPatternTrack_Release(pt);
    IDirectMusicTrack_Release(tr);
    IDirectMusicStyle_Release(st);
    IDirectMusicSegment8_Release(sg);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    LoadLibraryA("dsound.dll");
    static const char *const dlls[] = { "dmcompos.dll", "dmstyle.dll", "dmime.dll" };
    if (getenv("COMP_RANDLOG")) g_randlog = fopen(getenv("COMP_RANDLOG"), "w");
    g_rand = (void *)GetProcAddress(LoadLibraryA("msvcrt.dll"), "rand");
    g_srand = (void *)GetProcAddress(LoadLibraryA("msvcrt.dll"), "srand");
    for (int k = 0; k < 3; k++) {
        HMODULE m = LoadLibraryA(dlls[k]);
        if (!m) continue;
        patch_import_name(m, "msvcrt.dll", "time", (void *)fixed_time);
        if (g_randlog) patch_import_name(m, "msvcrt.dll", "rand", (void *)rand_log);
        if (g_randlog) patch_import_name(m, "msvcrt.dll", "srand", (void *)srand_log);
    }
    CoInitialize(NULL);
    IDirectMusicLoader8 *ld;
    CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&ld);
    WCHAR dir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, dir);
    IDirectMusicLoader8_SetSearchDirectory(ld, &GUID_DirectMusicAllTypes, dir, FALSE);
    if (argc > 1 && !strcmp(argv[1], "shape")) return shapes(ld);
    if (argc > 1 && !strcmp(argv[1], "change")) return change(ld);
    if (argc > 1 && !strcmp(argv[1], "trans")) return trans(ld);
    if (argc > 1 && !strcmp(argv[1], "join")) return join(ld);
    if (argc > 1 && !strcmp(argv[1], "recomp")) return recomp(ld);
    if (argc > 1 && !strcmp(argv[1], "align")) return align(ld);
    if (argc > 1 && !strcmp(argv[1], "pattrack")) return pattrack(ld);
    if (argc > 1) {         /* templates (compose2): each composed with its own style and chord maps, then given ones */
        IDirectMusicComposer *cp = NULL;
        HRESULT hr = CoCreateInstance(&CLSID_DirectMusicComposer, NULL, CLSCTX_INPROC, &IID_IDirectMusicComposer, (void **)&cp);
        printf("create composer %08lx\n", hr);
        IDirectMusicChordMap *ca = NULL;
        IDirectMusicStyle8 *st = NULL;
        IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicChordMap, &IID_IDirectMusicChordMap, L"cmapa.cdm", (void **)&ca);
        IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"compsty.sty", (void **)&st);
        for (int a = 1; a < argc; a++) {
            WCHAR w[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, argv[a], -1, w, MAX_PATH);
            IDirectMusicSegment8 *tp = NULL;
            hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, w, (void **)&tp);
            printf("load %s %08lx\n", argv[a], hr);
            if (!tp) continue;
            if (argv[a][0] == 'c') {        /* Segment::Compose: into a new segment, then in place */
                IDirectMusicSegment *out = NULL;
                hr = IDirectMusicSegment8_Compose(tp, 0, NULL, NULL, &out);
                printf("%s Compose (new): %08lx\n", argv[a], hr);
                if (out) {
                    IDirectMusicSegment8 *o8 = NULL;
                    IDirectMusicSegment_QueryInterface(out, &IID_IDirectMusicSegment8, (void **)&o8);
                    dump(o8, "composed");
                    IDirectMusicSegment8_Release(o8);
                    IDirectMusicSegment_Release(out);
                }
                hr = IDirectMusicSegment8_Compose(tp, 0, NULL, NULL, NULL);
                printf("%s Compose (in place): %08lx\n", argv[a], hr);
                dump(tp, "in place");
                IDirectMusicSegment8_Release(tp);
                continue;
            }
            for (int k = 0; k < 6; k++) {
                IDirectMusicSegment *out = NULL;
                WORD act = (WORD)(k % 4);
                int given = k >= 4;
                hr = IDirectMusicComposer_ComposeSegmentFromTemplate(cp, given ? (IDirectMusicStyle *)st : NULL, (IDirectMusicSegment *)tp,
                                                                     given ? (WORD)(k - 3) : act, given ? ca : NULL, &out);
                printf("%s compose %d: %08lx\n", argv[a], k, hr);
                if (out) {
                    IDirectMusicSegment8 *o8 = NULL;
                    IDirectMusicSegment_QueryInterface(out, &IID_IDirectMusicSegment8, (void **)&o8);
                    dump(o8, "composed");
                    IDirectMusicSegment8_Release(o8);
                    IDirectMusicSegment_Release(out);
                }
            }
            IDirectMusicSegment8_Release(tp);
        }
        if (st) IDirectMusicStyle8_Release(st);
        if (ca) IDirectMusicChordMap_Release(ca);
        if (cp) IDirectMusicComposer_Release(cp);
        IDirectMusicLoader8_Release(ld);
        return 0;
    }
    IDirectMusicChordMap *c1 = NULL;
    HRESULT hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicChordMap, &IID_IDirectMusicChordMap, L"cmap1.cdm", (void **)&c1);
    printf("load cmap1 %08lx\n", hr);
    if (getenv("COMP_DEBUG") && c1) {
        static const GUID priv = { 0x93be9414, 0x5c4e, 0x11d1, { 0xa7, 0xce, 0x00, 0xa0, 0xc9, 0x13, 0xf7, 0x3c } };
        void *x = NULL;
        HRESULT h2 = IDirectMusicChordMap_QueryInterface(c1, &priv, &x);
        char mod[MAX_PATH] = "";
        HMODULE hm = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)*(void **)*(void **)c1, &hm);
        GetModuleFileNameA(hm, mod, MAX_PATH);
        fprintf(stderr, "private QI %08lx %p module %s\n", h2, x, mod);
    }
    IDirectMusicStyle8 *st = NULL;
    hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"compsty.sty", (void **)&st);
    printf("load style %08lx\n", hr);
    IDirectMusicSegment8 *tp = NULL;
    hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, L"template.sgt", (void **)&tp);
    printf("load template %08lx\n", hr);
    IDirectMusicComposer *cp = NULL;
    hr = CoCreateInstance(&CLSID_DirectMusicComposer, NULL, CLSCTX_INPROC, &IID_IDirectMusicComposer, (void **)&cp);
    printf("create composer %08lx\n", hr);
    if (!cp || !tp) return 1;
    for (WORD act = 0; act < 4; act++) {
        IDirectMusicSegment *out = NULL;
        hr = IDirectMusicComposer_ComposeSegmentFromTemplate(cp, (IDirectMusicStyle *)st, (IDirectMusicSegment *)tp, act, c1, &out);
        printf("ComposeSegmentFromTemplate activity %u: %08lx\n", act, hr);
        if (out) {
            IDirectMusicSegment8 *o8 = NULL;
            IDirectMusicSegment_QueryInterface(out, &IID_IDirectMusicSegment8, (void **)&o8);
            dump(o8, "composed");
            IDirectMusicSegment8_Release(o8);
            IDirectMusicSegment_Release(out);
        }
    }
    {
        IDirectMusicSegment *out = NULL;
        hr = IDirectMusicComposer_ComposeSegmentFromTemplate(cp, NULL, (IDirectMusicSegment *)tp, 1, NULL, &out);
        printf("ComposeSegmentFromTemplate from the template's own: %08lx\n", hr);
        if (out) {
            IDirectMusicSegment8 *o8 = NULL;
            IDirectMusicSegment_QueryInterface(out, &IID_IDirectMusicSegment8, (void **)&o8);
            dump(o8, "composed");
            IDirectMusicSegment8_Release(o8);
            IDirectMusicSegment_Release(out);
        }
    }
    IDirectMusicComposer_Release(cp);
    if (st) IDirectMusicStyle8_Release(st);
    if (c1) IDirectMusicChordMap_Release(c1);
    IDirectMusicSegment8_Release(tp);
    IDirectMusicLoader8_Release(ld);
    return 0;
}
