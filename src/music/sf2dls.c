/* reAL-Music: SoundFont 2 -> DLS Level 2.
 *
 * A soundfont is rewritten as a DLS collection in memory and then loaded like any DLS file, so the synth and the
 * download path stay the ones verified against Microsoft's.  Each preset becomes an instrument (bank 128 = the
 * drum flag, other banks = bank select MSB); each (preset zone x instrument zone) pair whose key and velocity ranges
 * overlap becomes a region with its own DLS2 articulation:
 *   - generators: instrument zone over instrument global zone (absolute), plus preset zone over preset global zone
 *     (relative), SF2 defaults otherwise;
 *   - volume / modulation envelopes -> EG1 / EG2 (delay, attack, hold, decay, sustain, release; key -> decay),
 *     modulation LFO -> LFO (pitch, volume, filter), vibrato LFO -> vibrato, initial filter -> the DLS2 filter
 *     (with SF2's default velocity -> cutoff modulator when the filter is in use), pan, exclusive class -> key group;
 *   - tuning: root key (overridingRootKey or the sample's), coarse tune folded into the unity note, fine tune and
 *     the sample's pitch correction into the fine tune; initial attenuation with the EMU 0.4 factor most soundfonts
 *     are voiced for;
 *   - sample offsets and loops: each distinct (sample, start, end) becomes its own DLS wave; sample modes 1 / 3 ->
 *     forward / loop-and-release loops.
 * Not representable in DLS / this synth: custom modulators, scale tuning other than 100, key -> hold, chorus /
 * reverb send amounts (the synth uses CC91 / CC93 as usual), 24-bit sample extensions (the 16-bit part is used). */
#include "sf2dls.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define FCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ------------------------------------------------------------------ SF2 parsing */
typedef struct { char name[21]; uint16_t preset, bank, bag; } sf_phdr;
typedef struct { uint16_t gen, mod; } sf_bag;
typedef struct { uint16_t oper; int16_t amount; } sf_gen;
typedef struct { char name[21]; uint16_t bag; } sf_inst;
typedef struct { char name[21]; uint32_t start, end, sloop, eloop, rate; uint8_t origpitch; int8_t corr; uint16_t link, type; } sf_shdr;

typedef struct {
    const int16_t *smpl; uint32_t nsmpl;
    sf_phdr *ph; int nph;
    sf_bag *pb; int npb;
    sf_gen *pg; int npg;
    sf_inst *ih; int nih;
    sf_bag *ib; int nib;
    sf_gen *ig; int nig;
    sf_shdr *sh; int nsh;
} sf2;

typedef struct { const uint8_t *p, *e; } rd;
static int next_chunk(rd *r, uint32_t *id, rd *data, uint32_t *type)
{
    if (r->e - r->p < 8) return 0;
    uint32_t sz = u32(r->p + 4);
    *id = u32(r->p);
    const uint8_t *d = r->p + 8;
    if ((size_t)(r->e - d) < sz) sz = (uint32_t)(r->e - d);
    data->p = d; data->e = d + sz;
    r->p = d + sz + (sz & 1);
    if (r->p > r->e) r->p = r->e;
    *type = 0;
    if ((*id == FCC('R','I','F','F') || *id == FCC('L','I','S','T')) && sz >= 4) { *type = u32(d); data->p += 4; }
    return 1;
}

static void name20(char *d, const uint8_t *s) { memcpy(d, s, 20); d[20] = 0; }

static int parse_sf2(sf2 *f, const uint8_t *data, size_t size)
{
    memset(f, 0, sizeof(*f));
    rd file = { data, data + size }, r;
    uint32_t id, t;
    if (!next_chunk(&file, &id, &r, &t) || id != FCC('R','I','F','F') || t != FCC('s','f','b','k')) return 0;
    rd c;
    while (next_chunk(&r, &id, &c, &t)) {
        if (id != FCC('L','I','S','T')) continue;
        rd x; uint32_t xid, xt;
        while (next_chunk(&c, &xid, &x, &xt)) {
            size_t n = (size_t)(x.e - x.p);
            const uint8_t *p = x.p;
            if (t == FCC('s','d','t','a') && xid == FCC('s','m','p','l')) { f->smpl = (const int16_t *)p; f->nsmpl = (uint32_t)(n / 2); }
            else if (t != FCC('p','d','t','a')) continue;
            else if (xid == FCC('p','h','d','r')) {
                f->nph = (int)(n / 38); f->ph = calloc(f->nph + 1, sizeof(*f->ph));
                for (int i = 0; i < f->nph; i++) { name20(f->ph[i].name, p + 38 * i); f->ph[i].preset = u16(p + 38 * i + 20); f->ph[i].bank = u16(p + 38 * i + 22); f->ph[i].bag = u16(p + 38 * i + 24); }
            } else if (xid == FCC('p','b','a','g') || xid == FCC('i','b','a','g')) {
                int m = (int)(n / 4);
                sf_bag *b = calloc(m + 1, sizeof(*b));
                for (int i = 0; i < m; i++) { b[i].gen = u16(p + 4 * i); b[i].mod = u16(p + 4 * i + 2); }
                if (xid == FCC('p','b','a','g')) { f->pb = b; f->npb = m; } else { f->ib = b; f->nib = m; }
            } else if (xid == FCC('p','g','e','n') || xid == FCC('i','g','e','n')) {
                int m = (int)(n / 4);
                sf_gen *g = calloc(m + 1, sizeof(*g));
                for (int i = 0; i < m; i++) { g[i].oper = u16(p + 4 * i); g[i].amount = (int16_t)u16(p + 4 * i + 2); }
                if (xid == FCC('p','g','e','n')) { f->pg = g; f->npg = m; } else { f->ig = g; f->nig = m; }
            } else if (xid == FCC('i','n','s','t')) {
                f->nih = (int)(n / 22); f->ih = calloc(f->nih + 1, sizeof(*f->ih));
                for (int i = 0; i < f->nih; i++) { name20(f->ih[i].name, p + 22 * i); f->ih[i].bag = u16(p + 22 * i + 20); }
            } else if (xid == FCC('s','h','d','r')) {
                f->nsh = (int)(n / 46); f->sh = calloc(f->nsh + 1, sizeof(*f->sh));
                for (int i = 0; i < f->nsh; i++) {
                    const uint8_t *s = p + 46 * i;
                    sf_shdr *h = &f->sh[i];
                    name20(h->name, s);
                    h->start = u32(s + 20); h->end = u32(s + 24); h->sloop = u32(s + 28); h->eloop = u32(s + 32);
                    h->rate = u32(s + 36); h->origpitch = s[40]; h->corr = (int8_t)s[41]; h->link = u16(s + 42); h->type = u16(s + 44);
                }
            }
        }
    }
    return f->smpl && f->nph > 1 && f->npb && f->npg && f->nih > 1 && f->nib && f->nig && f->nsh > 1;
}

static void free_sf2(sf2 *f) { free(f->ph); free(f->pb); free(f->pg); free(f->ih); free(f->ib); free(f->ig); free(f->sh); }

/* ------------------------------------------------------------------ generators */
enum {
    G_START = 0, G_END = 1, G_SLOOP = 2, G_ELOOP = 3, G_STARTC = 4, G_MODLFO_PITCH = 5, G_VIBLFO_PITCH = 6,
    G_MODENV_PITCH = 7, G_FC = 8, G_Q = 9, G_MODLFO_FC = 10, G_MODENV_FC = 11, G_ENDC = 12, G_MODLFO_VOL = 13,
    G_PAN = 17, G_MODLFO_DELAY = 21, G_MODLFO_FREQ = 22, G_VIBLFO_DELAY = 23, G_VIBLFO_FREQ = 24,
    G_MODENV_DELAY = 25, G_MODENV_ATTACK = 26, G_MODENV_HOLD = 27, G_MODENV_DECAY = 28, G_MODENV_SUSTAIN = 29,
    G_MODENV_RELEASE = 30, G_KEY_MODENV_HOLD = 31, G_KEY_MODENV_DECAY = 32, G_VOLENV_DELAY = 33,
    G_VOLENV_ATTACK = 34, G_VOLENV_HOLD = 35, G_VOLENV_DECAY = 36, G_VOLENV_SUSTAIN = 37, G_VOLENV_RELEASE = 38,
    G_KEY_VOLENV_HOLD = 39, G_KEY_VOLENV_DECAY = 40, G_INSTRUMENT = 41, G_KEYRANGE = 43, G_VELRANGE = 44,
    G_SLOOPC = 45, G_ATTEN = 48, G_ELOOPC = 50, G_COARSE = 51, G_FINE = 52, G_SAMPLE = 53, G_MODES = 54,
    G_SCALE = 56, G_EXCL = 57, G_ROOT = 58, G_N = 61
};

typedef struct { int32_t v[G_N]; uint8_t set[G_N]; } gens;

static void gen_defaults(gens *g)
{
    memset(g, 0, sizeof(*g));
    for (int i = G_MODLFO_DELAY; i <= G_VOLENV_RELEASE; i++) g->v[i] = -12000;
    g->v[G_MODLFO_FREQ] = 0; g->v[G_VIBLFO_FREQ] = 0;
    g->v[G_MODENV_SUSTAIN] = 0; g->v[G_VOLENV_SUSTAIN] = 0;
    g->v[G_KEY_MODENV_HOLD] = g->v[G_KEY_MODENV_DECAY] = g->v[G_KEY_VOLENV_HOLD] = g->v[G_KEY_VOLENV_DECAY] = 0;
    g->v[G_FC] = 13500;
    g->v[G_KEYRANGE] = 0x7f00; g->v[G_VELRANGE] = 0x7f00;        /* lo in the low byte, hi in the high byte */
    g->v[G_SCALE] = 100; g->v[G_ROOT] = -1; g->v[G_SAMPLE] = -1; g->v[G_INSTRUMENT] = -1;
}

static void apply_zone(gens *g, const sf_gen *list, int from, int to, int relative)
{
    for (int i = from; i < to; i++) {
        uint16_t o = list[i].oper;
        if (o >= G_N) continue;
        int32_t a = list[i].amount;
        if (o == G_KEYRANGE || o == G_VELRANGE) { g->v[o] = (uint16_t)a; g->set[o] = 1; continue; }
        if (relative) {
            /* preset level: offsets, sample modes, exclusive class, root key and sample are instrument-only */
            if (o <= G_ENDC && (o == G_START || o == G_END || o == G_SLOOP || o == G_ELOOP || o == G_STARTC || o == G_ENDC)) continue;
            if (o == G_SLOOPC || o == G_ELOOPC || o == G_MODES || o == G_EXCL || o == G_ROOT || o == G_SAMPLE) continue;
            if (o == G_INSTRUMENT) { g->v[o] = (uint16_t)a; g->set[o] = 1; continue; }
            g->v[o] += a;
        } else {
            g->v[o] = (o == G_SAMPLE || o == G_INSTRUMENT) ? (uint16_t)a : a;
        }
        g->set[o] = 1;
    }
}

/* ------------------------------------------------------------------ output buffer */
/* written in two passes: the first only measures (b NULL, measure set), the second fills a buffer of the exact size
 * (a 32-bit game has no room for growing a several-hundred-MB buffer by doubling) */
typedef struct { uint8_t *b; size_t n, cap; int measure, failed; } out;

static void put(out *o, const void *p, size_t n)
{
    if (o->measure) { o->n += n; return; }
    if (o->failed) return;
    if (o->n + n > o->cap) {
        size_t c = o->cap ? o->cap : 65536;
        while (c < o->n + n) c *= 2;
        uint8_t *nb = realloc(o->b, c);
        if (!nb) { o->failed = 1; return; }
        o->b = nb; o->cap = c;
    }
    if (p) memcpy(o->b + o->n, p, n); else memset(o->b + o->n, 0, n);
    o->n += n;
}
static void p32(out *o, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; put(o, b, 4); }
static void p16(out *o, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put(o, b, 2); }
static size_t open_list(out *o, uint32_t id, uint32_t type) { p32(o, id); size_t at = o->n; p32(o, 0); p32(o, type); return at; }
static void close_chunk(out *o, size_t at)
{
    uint32_t sz = (uint32_t)(o->n - at - 4);
    if (o->b) { o->b[at] = (uint8_t)sz; o->b[at + 1] = (uint8_t)(sz >> 8); o->b[at + 2] = (uint8_t)(sz >> 16); o->b[at + 3] = (uint8_t)(sz >> 24); }
    if (sz & 1) put(o, NULL, 1);
}
static size_t open_chunk(out *o, uint32_t id) { p32(o, id); size_t at = o->n; p32(o, 0); return at; }

/* ------------------------------------------------------------------ waves */
typedef struct { int sample; uint32_t s, e; } wavekey;
typedef struct { wavekey *k; int n, cap; } wavelist;

static int wave_for(wavelist *w, int sample, uint32_t s, uint32_t e)
{
    for (int i = 0; i < w->n; i++) if (w->k[i].sample == sample && w->k[i].s == s && w->k[i].e == e) return i;
    if (w->n == w->cap) { w->cap = w->cap ? w->cap * 2 : 256; w->k = realloc(w->k, w->cap * sizeof(*w->k)); }
    w->k[w->n].sample = sample; w->k[w->n].s = s; w->k[w->n].e = e;
    return w->n++;
}

/* ------------------------------------------------------------------ articulation */
typedef struct { uint16_t src, ctl, dst, tr; int32_t scale; } conn;
typedef struct { conn c[48]; int n; } connlist;
static void add(connlist *l, uint16_t src, uint16_t ctl, uint16_t dst, int32_t scale)
{
    if (l->n < 48) { conn *c = &l->c[l->n++]; c->src = src; c->ctl = ctl; c->dst = dst; c->tr = 0; c->scale = scale; }
}
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static int32_t tc(int32_t t) { return t <= -12000 ? (int32_t)0x80000000 : clampi(t, -12000, 12000) * 65536; }  /* timecents; -12000 = none */
static int32_t hi16(int32_t v) { return clampi(v, -32768, 32767) * 65536; }

enum { SRC_NONE = 0, SRC_LFO = 1, SRC_VEL = 2, SRC_KEY = 3, SRC_EG2 = 5, SRC_VIB = 9 };
enum { DST_GAIN = 1, DST_PITCH = 3, DST_PAN = 4, DST_LFO_FREQ = 0x104, DST_LFO_DELAY = 0x105, DST_VIB_FREQ = 0x114,
       DST_VIB_DELAY = 0x115, DST_EG1_ATTACK = 0x206, DST_EG1_DECAY = 0x207, DST_EG1_RELEASE = 0x209,
       DST_EG1_SUSTAIN = 0x20a, DST_EG1_DELAY = 0x20b, DST_EG1_HOLD = 0x20c, DST_EG2_ATTACK = 0x30a,
       DST_EG2_DECAY = 0x30b, DST_EG2_RELEASE = 0x30d, DST_EG2_SUSTAIN = 0x30e, DST_EG2_DELAY = 0x30f,
       DST_EG2_HOLD = 0x310, DST_CUTOFF = 0x500, DST_Q = 0x501 };

static void articulation(const gens *g, connlist *l)
{
    const int32_t *v = g->v;
    /* volume envelope: SF2 key -> decay is relative to key 60 and per key; DLS scales by key / 128 */
    add(l, SRC_NONE, 0, DST_EG1_DELAY, tc(v[G_VOLENV_DELAY]));
    add(l, SRC_NONE, 0, DST_EG1_ATTACK, tc(v[G_VOLENV_ATTACK]));
    add(l, SRC_NONE, 0, DST_EG1_HOLD, tc(v[G_VOLENV_HOLD]));
    add(l, SRC_NONE, 0, DST_EG1_DECAY, tc(v[G_VOLENV_DECAY] + 60 * v[G_KEY_VOLENV_DECAY]));
    if (v[G_KEY_VOLENV_DECAY]) add(l, SRC_KEY, 0, DST_EG1_DECAY, hi16(-v[G_KEY_VOLENV_DECAY] * 128));
    /* sustain: SF2 attenuation in cB below peak; DLS 0.1 % of the 96 dB range */
    add(l, SRC_NONE, 0, DST_EG1_SUSTAIN, clampi(1000 - clampi(v[G_VOLENV_SUSTAIN], 0, 1440) * 1000 / 960, 0, 1000) * 65536);
    add(l, SRC_NONE, 0, DST_EG1_RELEASE, tc(v[G_VOLENV_RELEASE]));
    /* modulation envelope */
    if (v[G_MODENV_PITCH] || v[G_MODENV_FC]) {
        add(l, SRC_NONE, 0, DST_EG2_DELAY, tc(v[G_MODENV_DELAY]));
        add(l, SRC_NONE, 0, DST_EG2_ATTACK, tc(v[G_MODENV_ATTACK]));
        add(l, SRC_NONE, 0, DST_EG2_HOLD, tc(v[G_MODENV_HOLD]));
        add(l, SRC_NONE, 0, DST_EG2_DECAY, tc(v[G_MODENV_DECAY] + 60 * v[G_KEY_MODENV_DECAY]));
        if (v[G_KEY_MODENV_DECAY]) add(l, SRC_KEY, 0, DST_EG2_DECAY, hi16(-v[G_KEY_MODENV_DECAY] * 128));
        add(l, SRC_NONE, 0, DST_EG2_SUSTAIN, clampi(1000 - v[G_MODENV_SUSTAIN], 0, 1000) * 65536);
        add(l, SRC_NONE, 0, DST_EG2_RELEASE, tc(v[G_MODENV_RELEASE]));
        if (v[G_MODENV_PITCH]) add(l, SRC_EG2, 0, DST_PITCH, clampi(v[G_MODENV_PITCH], -12000, 12000) * 65536);
    }
    /* filter */
    int filt = v[G_FC] < 13500 || v[G_MODLFO_FC] || v[G_MODENV_FC];
    if (filt) {
        /* SF2's default velocity -> cutoff modulator: -2400 cents at velocity 0, none at 127 */
        add(l, SRC_NONE, 0, DST_CUTOFF, clampi(v[G_FC] - 2400, 1500, 13500) * 65536);
        add(l, SRC_NONE, 0, DST_Q, clampi(v[G_Q], 0, 960) * 65536);
        if (v[G_MODENV_FC]) add(l, SRC_EG2, 0, DST_CUTOFF, clampi(v[G_MODENV_FC], -12000, 12000) * 65536);
        add(l, SRC_VEL, 0, DST_CUTOFF, 2400 * 65536);
    }
    /* modulation LFO */
    if (v[G_MODLFO_PITCH] || v[G_MODLFO_VOL] || v[G_MODLFO_FC]) {
        add(l, SRC_NONE, 0, DST_LFO_FREQ, v[G_MODLFO_FREQ] * 65536);
        add(l, SRC_NONE, 0, DST_LFO_DELAY, tc(v[G_MODLFO_DELAY]));
        if (v[G_MODLFO_PITCH]) add(l, SRC_LFO, 0, DST_PITCH, clampi(v[G_MODLFO_PITCH], -1200, 1200) * 65536);
        if (v[G_MODLFO_VOL]) add(l, SRC_LFO, 0, DST_GAIN, clampi(v[G_MODLFO_VOL], -120, 120) * 65536);
        if (v[G_MODLFO_FC]) add(l, SRC_LFO, 0, DST_CUTOFF, clampi(v[G_MODLFO_FC], -12800, 12800) * 65536);
    }
    /* vibrato LFO */
    if (v[G_VIBLFO_PITCH]) {
        add(l, SRC_NONE, 0, DST_VIB_FREQ, v[G_VIBLFO_FREQ] * 65536);
        add(l, SRC_NONE, 0, DST_VIB_DELAY, tc(v[G_VIBLFO_DELAY]));
        add(l, SRC_VIB, 0, DST_PITCH, clampi(v[G_VIBLFO_PITCH], -1200, 1200) * 65536);
    }
    if (v[G_PAN]) add(l, SRC_NONE, 0, DST_PAN, clampi(v[G_PAN], -500, 500) * 65536);
}

/* ------------------------------------------------------------------ conversion */
typedef struct {
    gens g;
    int wave;
    uint32_t sloop, eloop;      /* relative to the wave */
} region;

static void write_region(out *o, const sf2 *f, const region *r)
{
    const int32_t *v = r->g.v;
    const sf_shdr *s = &f->sh[v[G_SAMPLE]];
    size_t rg = open_list(o, FCC('L','I','S','T'), FCC('r','g','n','2'));
    size_t c = open_chunk(o, FCC('r','g','n','h'));
    p16(o, (uint16_t)(v[G_KEYRANGE] & 0x7f)); p16(o, (uint16_t)((v[G_KEYRANGE] >> 8) & 0x7f));
    p16(o, (uint16_t)(v[G_VELRANGE] & 0x7f)); p16(o, (uint16_t)((v[G_VELRANGE] >> 8) & 0x7f));
    p16(o, 0); p16(o, (uint16_t)clampi(v[G_EXCL], 0, 15));
    close_chunk(o, c);
    /* tuning: coarse tune moves the unity note, fine tune + pitch correction the cents */
    int root = v[G_ROOT] >= 0 && v[G_ROOT] <= 127 ? v[G_ROOT] : s->origpitch;
    if (root > 127) root = 60;
    int unity = clampi(root - v[G_COARSE], 0, 127);
    int fine = clampi(v[G_FINE] + s->corr, -1200, 1200);
    double att_db = clampi(v[G_ATTEN], 0, 1440) * 0.4 / 10.0;     /* EMU's 0.4 factor */
    int mode = v[G_MODES] & 3;
    int looped = (mode == 1 || mode == 3) && r->eloop > r->sloop + 1;
    c = open_chunk(o, FCC('w','s','m','p'));
    p32(o, 20); p16(o, (uint16_t)unity); p16(o, (uint16_t)(int16_t)fine);
    p32(o, (uint32_t)(int32_t)(-att_db * 655360.0)); p32(o, 0); p32(o, looped ? 1 : 0);
    if (looped) { p32(o, 16); p32(o, mode == 3 ? 1 : 0); p32(o, r->sloop); p32(o, r->eloop - r->sloop); }
    close_chunk(o, c);
    c = open_chunk(o, FCC('w','l','n','k'));
    p16(o, 0); p16(o, 0); p32(o, 1); p32(o, (uint32_t)r->wave);
    close_chunk(o, c);
    connlist l;
    l.n = 0;
    articulation(&r->g, &l);
    size_t la = open_list(o, FCC('L','I','S','T'), FCC('l','a','r','2'));
    c = open_chunk(o, FCC('a','r','t','2'));
    p32(o, 8); p32(o, (uint32_t)l.n);
    for (int i = 0; i < l.n; i++) { p16(o, l.c[i].src); p16(o, l.c[i].ctl); p16(o, l.c[i].dst); p16(o, l.c[i].tr); p32(o, (uint32_t)l.c[i].scale); }
    close_chunk(o, c);
    close_chunk(o, la);
    close_chunk(o, rg);
}

static int emit(out *op, sf2 *fp);

void *sf2_to_dls(const void *data, size_t size, size_t *outsize, int *ninstr)
{
    sf2 f;
    if (!parse_sf2(&f, data, size)) { free_sf2(&f); return NULL; }
    out m = { 0 };
    m.measure = 1;
    emit(&m, &f);
    out o = { 0 };
    o.b = malloc(m.n ? m.n : 1);
    o.cap = m.n;
    int ninst = o.b ? emit(&o, &f) : 0;
    free_sf2(&f);
    if (!o.b || o.failed || o.n != m.n) { free(o.b); if (outsize) *outsize = m.n; return NULL; }
    *outsize = o.n;
    if (ninstr) *ninstr = ninst;
    return o.b;
}

static int emit(out *op, sf2 *fp)
{
    out o = *op;
    sf2 f = *fp;
    wavelist wl = { 0 };
    size_t riff = open_list(&o, FCC('R','I','F','F'), FCC('D','L','S',' '));
    size_t colh = o.n;
    size_t c = open_chunk(&o, FCC('c','o','l','h')); p32(&o, 0); close_chunk(&o, c);
    size_t lins = open_list(&o, FCC('L','I','S','T'), FCC('l','i','n','s'));
    int ninst = 0;
    region *rs = NULL;
    int rcap = 0;
    for (int p = 0; p + 1 < f.nph; p++) {
        const sf_phdr *ph = &f.ph[p];
        int pb0 = ph->bag, pb1 = f.ph[p + 1].bag;
        if (pb0 >= pb1 || pb1 > f.npb) continue;
        gens pglob;                     /* relative values: start at zero, full ranges */
        memset(&pglob, 0, sizeof(pglob));
        pglob.v[G_KEYRANGE] = pglob.v[G_VELRANGE] = 0x7f00;
        int nr = 0;
        for (int b = pb0; b < pb1; b++) {
            int g0 = f.pb[b].gen, g1 = f.pb[b + 1].gen;
            if (g1 > f.npg) g1 = f.npg;
            gens pz = pglob;
            pz.v[G_INSTRUMENT] = -1;
            apply_zone(&pz, f.pg, g0, g1, 1);
            if (pz.v[G_INSTRUMENT] < 0) {
                if (b == pb0) { pglob = pz; pglob.v[G_INSTRUMENT] = 0; }    /* the preset's global zone */
                continue;
            }
            int ii = pz.v[G_INSTRUMENT];
            if (ii + 1 >= f.nih) continue;
            int ib0 = f.ih[ii].bag, ib1 = f.ih[ii + 1].bag;
            if (ib1 > f.nib) ib1 = f.nib;
            gens iglob;
            gen_defaults(&iglob);
            for (int z = ib0; z < ib1; z++) {
                int h0 = f.ib[z].gen, h1 = f.ib[z + 1].gen;
                if (h1 > f.nig) h1 = f.nig;
                gens iz = iglob;
                iz.v[G_SAMPLE] = -1;
                apply_zone(&iz, f.ig, h0, h1, 0);
                if (iz.v[G_SAMPLE] < 0) { if (z == ib0) iglob = iz; continue; }
                if (iz.v[G_SAMPLE] + 1 >= f.nsh) continue;
                const sf_shdr *s = &f.sh[iz.v[G_SAMPLE]];
                if (s->type & 0x8000) continue;           /* ROM sample */
                /* ranges: intersection of the preset and instrument zones */
                int klo = (iz.v[G_KEYRANGE] & 0xff), khi = (iz.v[G_KEYRANGE] >> 8) & 0xff;
                int vlo = (iz.v[G_VELRANGE] & 0xff), vhi = (iz.v[G_VELRANGE] >> 8) & 0xff;
                int pklo = pz.v[G_KEYRANGE] & 0xff, pkhi = (pz.v[G_KEYRANGE] >> 8) & 0xff;
                int pvlo = pz.v[G_VELRANGE] & 0xff, pvhi = (pz.v[G_VELRANGE] >> 8) & 0xff;
                if (pklo > klo) klo = pklo;
                if (pkhi < khi) khi = pkhi;
                if (pvlo > vlo) vlo = pvlo;
                if (pvhi < vhi) vhi = pvhi;
                if (klo > khi || vlo > vhi) continue;
                region r;
                r.g = iz;
                for (int k = 0; k < G_N; k++)
                    if (k != G_KEYRANGE && k != G_VELRANGE && k != G_INSTRUMENT && k != G_SAMPLE) r.g.v[k] += pz.v[k];
                r.g.v[G_KEYRANGE] = klo | (khi << 8);
                r.g.v[G_VELRANGE] = vlo | (vhi << 8);
                /* sample window */
                int64_t st = (int64_t)s->start + iz.v[G_START] + 32768LL * iz.v[G_STARTC];
                int64_t en = (int64_t)s->end + iz.v[G_END] + 32768LL * iz.v[G_ENDC];
                int64_t ls = (int64_t)s->sloop + iz.v[G_SLOOP] + 32768LL * iz.v[G_SLOOPC];
                int64_t le = (int64_t)s->eloop + iz.v[G_ELOOP] + 32768LL * iz.v[G_ELOOPC];
                if (st < 0) st = 0;
                if (en > f.nsmpl) en = f.nsmpl;
                if (en <= st + 1) continue;
                if (ls < st) ls = st;
                if (le > en) le = en;
                r.wave = wave_for(&wl, iz.v[G_SAMPLE], (uint32_t)st, (uint32_t)en);
                r.sloop = (uint32_t)(ls - st); r.eloop = (uint32_t)(le - st);
                if (nr == rcap) { rcap = rcap ? rcap * 2 : 64; rs = realloc(rs, rcap * sizeof(*rs)); }
                rs[nr++] = r;
            }
        }
        if (!nr) continue;
        size_t ins = open_list(&o, FCC('L','I','S','T'), FCC('i','n','s',' '));
        c = open_chunk(&o, FCC('i','n','s','h'));
        uint32_t bank = ph->bank >= 128 ? 0x80000000u : ((uint32_t)(ph->bank & 0x7f) << 8);
        p32(&o, (uint32_t)nr); p32(&o, bank); p32(&o, ph->preset & 0x7f);
        close_chunk(&o, c);
        size_t lrgn = open_list(&o, FCC('L','I','S','T'), FCC('l','r','g','n'));
        for (int i = 0; i < nr; i++) write_region(&o, &f, &rs[i]);
        close_chunk(&o, lrgn);
        size_t info = open_list(&o, FCC('L','I','S','T'), FCC('I','N','F','O'));
        c = open_chunk(&o, FCC('I','N','A','M'));
        put(&o, ph->name, strlen(ph->name) + 1);
        close_chunk(&o, c);
        close_chunk(&o, info);
        close_chunk(&o, ins);
        ninst++;
    }
    close_chunk(&o, lins);
    /* pool table and wave pool */
    c = open_chunk(&o, FCC('p','t','b','l'));
    size_t ptbl = o.n + 8;
    p32(&o, 8); p32(&o, (uint32_t)wl.n);
    put(&o, NULL, 4 * (size_t)wl.n);
    close_chunk(&o, c);
    size_t wvpl = open_list(&o, FCC('L','I','S','T'), FCC('w','v','p','l'));
    size_t base = o.n;
    for (int i = 0; i < wl.n; i++) {
        uint32_t off = (uint32_t)(o.n - base);
        if (o.b) memcpy(o.b + ptbl + 4 * i, (uint8_t[4]){ (uint8_t)off, (uint8_t)(off >> 8), (uint8_t)(off >> 16), (uint8_t)(off >> 24) }, 4);
        const sf_shdr *s = &f.sh[wl.k[i].sample];
        size_t wv = open_list(&o, FCC('L','I','S','T'), FCC('w','a','v','e'));
        c = open_chunk(&o, FCC('f','m','t',' '));
        p16(&o, 1); p16(&o, 1); p32(&o, s->rate); p32(&o, s->rate * 2); p16(&o, 2); p16(&o, 16);
        close_chunk(&o, c);
        c = open_chunk(&o, FCC('d','a','t','a'));
        put(&o, f.smpl + wl.k[i].s, 2 * (size_t)(wl.k[i].e - wl.k[i].s));
        close_chunk(&o, c);
        close_chunk(&o, wv);
    }
    close_chunk(&o, wvpl);
    close_chunk(&o, riff);
    if (o.b) { uint32_t n = (uint32_t)ninst; memcpy(o.b + colh + 8, &n, 4); }
    free(rs); free(wl.k);
    *op = o;
    return ninst;
}
