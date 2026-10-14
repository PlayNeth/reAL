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

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    LoadLibraryA("dsound.dll");
    static const char *const dlls[] = { "dmcompos.dll", "dmstyle.dll", "dmime.dll" };
    if (getenv("COMP_RANDLOG")) g_randlog = fopen(getenv("COMP_RANDLOG"), "w");
    g_rand = (void *)GetProcAddress(LoadLibraryA("msvcrt.dll"), "rand");
    for (int k = 0; k < 3; k++) {
        HMODULE m = LoadLibraryA(dlls[k]);
        if (!m) continue;
        patch_import_name(m, "msvcrt.dll", "time", (void *)fixed_time);
        if (g_randlog) patch_import_name(m, "msvcrt.dll", "rand", (void *)rand_log);
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
