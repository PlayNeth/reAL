/* msynth: port of Microsoft's software synthesizer (dmsynth.dll 5.3.x, DirectX 9).  See msynth.h.
 *
 * Structure follows the original: CSynth (synth), CControlLogic (one per channel group: MIDI state, controller
 * curves, note queue), CVoice (a playing region: two LFOs, two envelopes, a DLS2 low-pass, CDigitalAudio sample
 * playback), CInstManager (downloaded waves/instruments).  All timing is in sample times; controllers are kept as
 * time-stamped value lists that the voices sample at the start of each update span.  Comments give the original
 * function addresses (image base 0x77480000). */
#include "msynth.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "msynth_tables.h"

#define I64MAX 0x7fffffffffffffffLL

/* ------------------------------------------------------------------ helpers */
static int32_t muldiv(int32_t a, int32_t b, int32_t c)  /* kernel32 MulDiv (rounds half away from zero) */
{
    int64_t r;
    if (!c) return -1;
    if (c < 0) { a = -a; c = -c; }
    if ((a < 0 && b < 0) || (a >= 0 && b >= 0)) r = ((int64_t)a * b + c / 2) / c;
    else r = ((int64_t)a * b - c / 2) / c;
    if (r > 2147483647 || r < -2147483647) return -1;
    return (int32_t)r;
}

#define DB2LIN(i) ms_db2lin[(i) + 1000]
#define FINE(i) ms_cent_fine[(i) + 100]
#define OCT(i) ms_cent_oct[(i) + 48]

/* 7748a150: cents -> 4096 * 2^(c/1200) */
static int32_t cents2ratio(int32_t c)
{
    if (c > 100) {
        if (c > 4800) c = 4800;
        return (int32_t)((OCT((c / 100) % 12) * (FINE(c % 100) << ((c / 100) / 12)))) >> 12;
    }
    if (c < -100) {
        if (c < -4800) c = -4800;
        int s = c / 100;
        return (OCT(-((-s) % 12)) * (FINE(-((-c) % 100)) >> ((-s) / 12))) >> 12;
    }
    return FINE(c);
}

/* 7748a100: millibels -> 12 bit gain */
static int32_t mb2gain(int32_t mb)
{
    int i = mb / 10;
    if (i < -1000) return DB2LIN(-1000);
    if (i >= 0) return DB2LIN(0);
    return DB2LIN(i);
}

static int16_t sat16add(int16_t a, int16_t b)
{
    int r = a + b;
    return r > 32767 ? 32767 : r < -32768 ? -32768 : (int16_t)r;
}

/* ------------------------------------------------------------------ controller curves and the note queue */
typedef struct ms_node { struct ms_node *next; int64_t t; int32_t v; } ms_node;
typedef struct { ms_node *head; int64_t last; int32_t cur; int32_t range; } ms_curve;   /* range: pitch bend */

static ms_node *node_new(int64_t t, int32_t v)
{
    ms_node *n = calloc(1, sizeof(*n));
    n->t = t; n->v = v;
    return n;
}

/* 77488650: insert keeping time order (after equal times), dropping an exact duplicate */
static void curve_add(ms_curve *c, int64_t t, int32_t v)
{
    ms_node *n = node_new(t, v), *p = c->head;
    if (!p || t < p->t) { n->next = c->head; c->head = n; return; }
    for (;;) {
        if (p->t == t && p->v == v) { free(n); return; }
        ms_node *nx = p->next;
        if (!nx) { p->next = n; return; }
        if (t < nx->t) { n->next = nx; p->next = n; return; }
        p = nx;
    }
}

/* 77488930: value at time t */
static int32_t curve_at(const ms_curve *c, int64_t t)
{
    int32_t v = c->cur;
    for (ms_node *p = c->head; p && p->t <= t; p = p->next) v = p->v;
    return v;
}

/* 77488500: retire the entries before t; returns whether the list is empty */
static int curve_retire(ms_curve *c, int64_t t)
{
    ms_node *p;
    while ((p = c->head) && p->t < t) {
        c->head = p->next;
        c->last = p->t; c->cur = p->v;
        free(p);
    }
    return c->head == NULL;
}

/* 77488490: drop entries at or after t */
static int curve_cut(ms_curve *c, int64_t t)
{
    ms_node *p = c->head, *prev = NULL;
    while (p && p->t < t) { prev = p; p = p->next; }
    if (prev) prev->next = NULL; else c->head = NULL;
    while (p) { ms_node *n = p->next; free(p); p = n; }
    return c->head == NULL;
}

static void curve_clear(ms_curve *c) { curve_cut(c, -I64MAX - 1); }

/* note queue: 77488580 insert after equal times, 774887c0 pop if due */
typedef struct { ms_node *head; } ms_queue;

static void queue_add(ms_queue *q, int64_t t, uint32_t packed)
{
    ms_node *n = node_new(t, (int32_t)packed), *p = q->head;
    if (!p || t < p->t) { n->next = q->head; q->head = n; return; }
    for (;;) {
        ms_node *nx = p->next;
        if (!nx) { p->next = n; return; }
        if (t < nx->t) { n->next = nx; p->next = n; return; }
        p = nx;
    }
}

static int queue_pop(ms_queue *q, int64_t end, int64_t *t, uint32_t *packed)
{
    ms_node *p = q->head;
    if (!p || p->t > end) return 0;
    *t = p->t; *packed = (uint32_t)p->v;
    q->head = p->next;
    free(p);
    return 1;
}

/* 77488830: pending events from t on happen at t with value 0 (note-ons become note-offs) */
static void queue_flatten(ms_queue *q, int64_t t)
{
    for (ms_node *p = q->head; p; p = p->next)
        if (p->t >= t) { p->t = t; p->v &= ~0xff; }
}

/* ------------------------------------------------------------------ downloaded data */
typedef struct ms_wave {
    struct ms_wave *next;
    uint32_t len;           /* samples, +1 guard sample */
    uint32_t rate;
    uint32_t id;
    void *data;             /* int16 or int8 (signed) */
    uint8_t fmt;            /* 1: 16 bit, 2: 8 bit */
    int refs;
    void *block;            /* download buffer copy owning data */
} ms_wave;

typedef struct ms_eg {      /* artic EG block (0x38 bytes in the original) */
    int64_t attack, decay, release;
    int16_t vel2attack, key2decay, sustain, range;
    int64_t delay, hold;
    int16_t x30, eg2cut;
} ms_eg;

typedef struct ms_lfo {     /* artic LFO block (0x28) */
    int32_t freq;
    int64_t delay;
    int16_t mw2vol, mw2pitch, lfo2vol, lfo2pitch, press2pitch, press2vol, lfo2cut, mw2cut, press2cut;
} ms_lfo;

typedef struct ms_fltp {    /* artic filter block (artic+0xc8) */
    int16_t nyq, cutoff, cutrel, q;
    int32_t qidx;
    int16_t vel2cut, key2cut;
} ms_fltp;

typedef struct ms_artic {
    ms_eg peg;              /* +0x00 */
    ms_eg veg;              /* +0x38 */
    ms_lfo lfo;             /* +0x70 */
    uint32_t rate;          /* +0x98 */
    int refs;               /* +0x9c */
    int16_t pan;            /* +0x9e */
    ms_lfo vib;             /* +0xa0 */
    ms_fltp flt;            /* +0xc8 */
} ms_artic;

typedef struct ms_region {
    struct ms_region *next;
    /* "source" block (region+4), copied into the voice's sample player */
    ms_wave *wave;          /* +04 */
    uint32_t loopstart;     /* +08 */
    uint32_t loopend;       /* +0c */
    uint32_t len;           /* +10 */
    uint32_t rate;          /* +14 */
    int16_t finetune;       /* +18 */
    uint32_t waveid;        /* +1c */
    uint8_t fmt;            /* +20 */
    uint8_t oneshot;        /* +21 */
    uint8_t unity;          /* +22 */
    uint32_t looptype;      /* +24 */
    ms_artic *artic;        /* +28 */
    int16_t atten;          /* +2c */
    int16_t tune;           /* +2e */
    uint8_t nonexcl;        /* +30 */
    uint8_t keyhi, keylo, group, velhi, vello;
    uint16_t wlopts;        /* +36 */
    uint32_t channel;       /* +38 */
} ms_region;

typedef struct ms_instr {
    struct ms_instr *next;
    ms_region *regions;
    uint32_t patch;
} ms_instr;

/* ------------------------------------------------------------------ voice parts */
typedef struct {            /* CVoiceLFO (voice+0x08 / +0x240) */
    ms_lfo p;
    int64_t start;
    ms_curve *mw;
    int64_t interval;
    ms_curve *press;
    int on;
} v_lfo;

typedef struct {            /* CVoiceEG (voice+0x50 / +0xa0) */
    ms_eg p;
    int64_t start, reltime;
    int on;
} v_eg;

typedef struct {            /* CVoiceFilter (voice+0x21c) */
    ms_fltp p;
    v_lfo *lfo;
    v_eg *peg;
    int32_t velcut, keycut;
} v_flt;

typedef struct {            /* CDigitalAudio (voice+0xf0) */
    ms_region src;          /* copy of the region's source block (wave .. looptype) */
    struct msynth *synth;   /* +24 */
    void *data;             /* +28 */
    int32_t base;           /* +2c pitch increment at no modulation (1/4096 samples) */
    int32_t inc;            /* +30 */
    int32_t lastpitch;      /* +34 */
    int32_t pos;            /* +38 position << 12 */
    int32_t lstart, lend, end;  /* +3c +40 +44 */
    int big;                /* +48 wave longer than 2^19 samples: positions rebased */
    int64_t pos64, lstart64, lend64, end64;  /* +50 .. +68 */
    int32_t rebase;         /* +70 */
    int oneshot;            /* +74 */
    int32_t b0, a1, a2;     /* +78 filter coefficients */
    int32_t y1, y2;         /* +84 filter state */
    int64_t played;         /* +98 */
} v_da;

typedef struct ms_voice {
    struct ms_voice *next;
    v_lfo lfo;              /* +008 */
    v_eg peg;               /* +050 */
    v_eg veg;               /* +0a0 */
    v_da da;                /* +0f0 */
    ms_curve *pb, *expr, *vol, *pan, *press, *rev, *cho, *cc74;  /* +190 .. */
    struct msynth *synth;   /* +1ac */
    int64_t interval;       /* +1b0 */
    int64_t lastmix;        /* +1b8 */
    int32_t panofs;         /* +1c0 */
    int32_t lastcut;        /* +1c4 */
    uint32_t seq;           /* +1c8 */
    int64_t start, stop;    /* +1d0 +1d8 */
    int64_t x1e0;
    int active;             /* +1e8 */
    int keyon;              /* +1ec */
    int stealing;           /* +1f0 */
    int32_t volmb;          /* +1f4 */
    int sustained;          /* +1f8 */
    uint16_t slot, note;    /* +1fc +1fe */
    uint32_t nonexcl, group;/* +200 +204 */
    uint32_t patch, prio;   /* +208 +20c */
    struct ms_cl *cl;       /* +210 */
    int32_t voiceid;        /* +214 */
    ms_region *region;      /* +218 */
    v_flt flt;              /* +21c */
    v_lfo vib;              /* +240 */
    uint32_t buses[33];     /* +288 count + ids */
    uint32_t looptype;      /* +30c */
    int multichan;          /* +310 */
    int32_t lastvol[32];    /* +314 */
    int32_t busofs[32];     /* +394 */
    int32_t lastgain[32];   /* +414 */
} ms_voice;

/* ------------------------------------------------------------------ control logic (one per channel group) */
typedef struct ms_cl {
    struct msynth *synth;
    ms_queue q;             /* +008 */
    ms_curve mw[16];        /* +020 */
    ms_curve pb[16];        /* +1a0 */
    ms_curve vol[16];       /* +3a0 */
    ms_curve expr[16];      /* +520 */
    ms_curve pan[16];       /* +6a0 */
    ms_curve rev[16];       /* +820 */
    ms_curve cho[16];       /* +9a0 */
    ms_curve cc74[16];      /* +b20 */
    ms_curve press[16];     /* +1200 */
    uint32_t sustain[16];   /* +ca0 */
    uint16_t rpn[16];       /* +ce0 */
    uint8_t bankmsb[16], banklsb[16];  /* +d00 +d10 */
    uint32_t patch[16];     /* +d20 */
    int clean;              /* +d60 */
    int32_t gain;           /* +d64 */
    uint32_t prio[16];      /* +d68 */
    int xgmode, gsmode;     /* +da8 +dac */
    uint16_t rpndata[16];   /* +db0 */
    int32_t master;         /* +dd0 master volume (millibels) */
    int32_t finetune[16];   /* +dd4 */
    int32_t scale[16][12];  /* +e14 */
    int32_t coarse[16];     /* +1114 */
    uint8_t chan[16];       /* +1154 */
    uint8_t drum[16];       /* +1164 */
    uint32_t mono[16];      /* +1174 */
    uint32_t buses[16][33]; /* +1380 */
} ms_cl;

/* ------------------------------------------------------------------ synth */
#define NBUCKET 31
struct msynth {
    ms_voice *freelist;     /* +20 */
    ms_voice *extra;        /* +24 */
    ms_voice *active;       /* +28 */
    int16_t maxvoices, extravoices;  /* +2c +2e */
    int64_t lastrender;     /* +18 */
    int32_t gain;           /* +84 */
    int dynpan, dynvol;     /* +88 +8c */
    int64_t interval;       /* +98 */
    uint32_t rate;          /* +a0 */
    int stereo;             /* +a4 */
    int mmx;                /* +268 */
    ms_cl **cl;             /* +244 */
    uint32_t ncl;           /* +248 */
    uint32_t seq;           /* DAT_77498b64 */
    /* CInstManager (+a8) */
    ms_instr *instr[NBUCKET];
    ms_wave *waves[NBUCKET];
    uint32_t imrate;        /* +a8+fc */
};

/* ================================================================== downloads (CInstManager) */

/* 77486a60: time cents -> samples, capped at rate*mult */
static int64_t tc2samples(int32_t tc, uint32_t rate, uint32_t mult)
{
    if (tc == (int32_t)0x80000000) return 0;
    int64_t v = (int64_t)(pow(2.0, tc * 1.2715657552083334e-08) * (double)rate);
    if (v >= 0 && (uint64_t)v > (uint32_t)(rate * mult)) v = (uint32_t)(rate * mult);
    return v;
}

/* 77486ac0: absolute pitch cents (16.16) -> LFO phase increment (2^24 per cycle) */
static int32_t pc2lfo(int32_t pc, uint32_t rate)
{
    return (int32_t)(int64_t)(pow(2.0, ((double)pc * 1.52587890625e-05 - 6900.0) * 0.0008333333333333334) * 7381975040.0 /
                              (double)rate);
}

/* 77486470 */
static void fltp_init(ms_fltp *f, uint32_t rate)
{
    f->nyq = (int16_t)(int32_t)(log((double)rate * 0.0022727272727272726) / log(2.0) * 1200.0 + 6900.0);
    f->cutoff = 0x7fff;
    f->q = 0; f->cutrel = 0; f->qidx = 0;
    f->key2cut = 0; f->vel2cut = 0;
}

/* 77486540 */
static void artic_init(ms_artic *a, uint32_t rate)
{
    int32_t f = (int32_t)(0x5000000ULL / rate);
    memset(a, 0, sizeof(*a));
    a->rate = rate;
    a->lfo.freq = f;
    a->vib.freq = f;
    a->peg.sustain = 1000;
    a->veg.sustain = 1000;
    fltp_init(&a->flt, rate);
}

static int16_t clamps(int v, int lo, int hi) { return (int16_t)(v < lo ? lo : v > hi ? hi : v); }

static void lfo_clamp(ms_lfo *l)      /* 77486120 */
{
    if (l->freq < 0x40) l->freq = 0x40; else if (l->freq > 0x1db0) l->freq = 0x1db0;
    if (l->delay < 0) l->delay = 0; else if (l->delay > 0x6baa8) l->delay = 0x6baa8;
    l->lfo2vol = clamps(l->lfo2vol, -1200, 1200);
    l->mw2vol = clamps(l->mw2vol, -1200, 1200);
    l->lfo2pitch = clamps(l->lfo2pitch, -1200, 1200);
    l->mw2pitch = clamps(l->mw2pitch, -1200, 1200);
    l->press2pitch = clamps(l->press2pitch, -1200, 1200);
    l->press2vol = clamps(l->press2vol, -1200, 1200);
    l->lfo2cut = clamps(l->lfo2cut, -12800, 12800);
    l->mw2cut = clamps(l->mw2cut, -12800, 12800);
    l->press2cut = clamps(l->press2cut, -12800, 12800);
}

static void eg_clamp(ms_eg *e)        /* 77486330 */
{
    if (e->attack < 0) e->attack = 0; else if (e->attack > 0x1aeaa0) e->attack = 0x1aeaa0;
    if (e->decay < 0) e->decay = 0; else if (e->decay > 0x1aeaa0) e->decay = 0x1aeaa0;
    e->sustain = clamps(e->sustain, 0, 1000);
    if (e->release < 0) e->release = 0; else if (e->release > 0x1aeaa0) e->release = 0x1aeaa0;
    e->range = clamps(e->range, -1200, 1200);
    e->key2decay = clamps(e->key2decay, -12000, 12000);
    e->vel2attack = clamps(e->vel2attack, -12000, 12000);
    e->x30 = clamps(e->x30, -12000, 12000);
    e->eg2cut = clamps(e->eg2cut, -12800, 12800);
}

static void flt_clamp(ms_fltp *f)     /* 774864d0 */
{
    if (f->cutoff == 0x7fff) { f->q = 0; f->vel2cut = 0; return; }
    f->cutoff = clamps(f->cutoff, 0x159f, 0x2e91);
    f->q = clamps(f->q, 0, 0xe1);
    f->vel2cut = clamps(f->vel2cut, -12800, 12800);
}

#define OFS(i) ((const uint8_t *)buf + ofs[i])
#define S16(p, o) (*(const int16_t *)((const uint8_t *)(p) + (o)))
#define U16(p, o) (*(const uint16_t *)((const uint8_t *)(p) + (o)))
#define I32(p, o) (*(const int32_t *)((const uint8_t *)(p) + (o)))
#define U32(p, o) (*(const uint32_t *)((const uint8_t *)(p) + (o)))

/* 77486b10: articulation from DMUS_ARTICPARAMS (DLS1) or a DMUS_ARTICULATION2 chain of connection lists */
static int artic_parse(ms_artic *a, const void *buf, const uint32_t *ofs, uint32_t nofs, uint32_t idx, uint32_t rate,
                       int dls2)
{
    if (!dls2) {
        uint32_t i = U32(OFS(idx), 0);
        if (i) {
            if (i >= nofs) return -1;
            const uint8_t *p = OFS(i);
            a->lfo.freq = pc2lfo(I32(p, 0), rate);
            a->lfo.delay = tc2samples(I32(p, 4), rate, 10);
            a->lfo.lfo2vol = (int16_t)((uint32_t)(I32(p, 8) * 10) >> 16);
            a->lfo.lfo2pitch = S16(p, 0xe);
            a->lfo.mw2vol = (int16_t)((uint32_t)(I32(p, 0x10) * 10) >> 16);
            a->lfo.mw2pitch = S16(p, 0x16);
            a->veg.attack = tc2samples(I32(p, 0x18), rate, 40);
            a->veg.decay = tc2samples(I32(p, 0x1c), rate, 40);
            a->veg.sustain = S16(p, 0x22);
            a->veg.release = tc2samples(I32(p, 0x24), rate, 40);
            a->veg.vel2attack = S16(p, 0x2a);
            a->veg.key2decay = S16(p, 0x2e);
            a->peg.key2decay = S16(p, 0x46);
            a->peg.range = S16(p, 0x4a);
            a->peg.vel2attack = S16(p, 0x42);
            a->peg.attack = tc2samples(I32(p, 0x30), rate, 40);
            a->peg.decay = tc2samples(I32(p, 0x34), rate, 40);
            a->peg.sustain = S16(p, 0x3a);
            a->peg.release = tc2samples(I32(p, 0x3c), rate, 40);
            a->pan = (int16_t)((I32(p, 0x4c) >> 12) / 125);
        }
    } else {
        const uint8_t *art = OFS(idx);
        while (art) {
            uint32_t li = U32(art, 0);
            if (li) {
                if (li >= nofs) return -1;
                const uint8_t *cl = OFS(li);
                uint32_t n = U32(cl, 4);
                const uint8_t *c = cl + 8;
                for (uint32_t k = 0; k < n; k++, c += 12) {
                    uint16_t src = U16(c, 0), ctl = U16(c, 2), dst = U16(c, 4);
                    int32_t sc = I32(c, 8);
                    int16_t hi = S16(c, 10);
                    switch (src) {
                    case 0:
                        switch (dst) {
                        case 0x20b: a->veg.delay = tc2samples(sc, rate, 40); break;
                        case 0x115: a->vib.delay = tc2samples(sc, rate, 10); break;
                        case 0x105: a->lfo.delay = tc2samples(sc, rate, 10); break;
                        case 4: a->pan = (int16_t)((sc >> 12) / 125); break;
                        case 0x104: a->lfo.freq = pc2lfo(sc, rate); break;
                        case 0x114: a->vib.freq = pc2lfo(sc, rate); break;
                        case 0x206: a->veg.attack = tc2samples(sc, rate, 40); break;
                        case 0x207: a->veg.decay = tc2samples(sc, rate, 40); break;
                        case 0x209: a->veg.release = tc2samples(sc, rate, 40); break;
                        case 0x20a: a->veg.sustain = hi; break;
                        case 0x30e: a->peg.sustain = hi; break;
                        case 0x30b: a->peg.decay = tc2samples(sc, rate, 40); break;
                        case 0x20c: a->veg.hold = tc2samples(sc, rate, 40); break;
                        case 0x30a: a->peg.attack = tc2samples(sc, rate, 40); break;
                        case 0x30d: a->peg.release = tc2samples(sc, rate, 40); break;
                        case 0x500: a->flt.cutoff = hi; a->flt.cutrel = (int16_t)(hi - a->flt.nyq + 0x29c0); break;
                        case 0x30f: a->peg.delay = tc2samples(sc, rate, 40); break;
                        case 0x310: a->peg.hold = tc2samples(sc, rate, 40); break;
                        case 0x501: a->flt.q = hi; a->flt.qidx = (int32_t)(int64_t)((float)hi * 0.06666667f + 0.5f); break;
                        }
                        break;
                    case 1:
                        if (ctl == 0) {
                            if (dst == 1) a->lfo.lfo2vol = (int16_t)((uint32_t)(sc * 10) >> 16);
                            else if (dst == 3) a->lfo.lfo2pitch = hi;
                            else if (dst == 0x500) a->lfo.lfo2cut = hi;
                        } else if (ctl == 8) {
                            if (dst == 1) a->lfo.press2vol = hi;
                            else if (dst == 3) a->lfo.press2pitch = hi;
                            else if (dst == 0x500) a->lfo.press2cut = hi;
                        } else if (ctl == 0x81) {
                            if (dst == 1) a->lfo.mw2vol = (int16_t)((uint32_t)(sc * 10) >> 16);
                            else if (dst == 3) a->lfo.mw2pitch = hi;
                            else if (dst == 0x500) a->lfo.mw2cut = hi;
                        }
                        break;
                    case 2:
                        if (dst == 0x206) a->veg.vel2attack = hi;
                        else if (dst == 0x30a) a->peg.vel2attack = hi;
                        else if (dst == 0x500) a->flt.vel2cut = hi;
                        break;
                    case 3:
                        if (dst == 0x30b) a->peg.key2decay = hi;
                        else if (dst == 0x207) a->veg.key2decay = hi;
                        else if (dst == 0x20c) a->peg.key2decay = hi;     /* sic */
                        else if (dst == 0x310) { a->peg.key2decay = hi; a->flt.key2cut = hi; }  /* sic: falls through */
                        else if (dst == 0x500) a->flt.key2cut = hi;
                        break;
                    case 5:
                        if (dst == 3) a->peg.range = hi;
                        else if (dst == 0x500) a->peg.eg2cut = hi;
                        break;
                    case 9:
                        if (ctl == 0) { if (dst == 3) a->vib.lfo2pitch = hi; }
                        else if (ctl == 8) { if (dst == 3) a->vib.press2pitch = hi; }
                        else if (ctl == 0x81) { if (dst == 3) a->vib.mw2pitch = hi; }
                        break;
                    }
                }
            }
            uint32_t nx = U32(art, 8);
            if (!nx) break;
            if (nx >= nofs) return -1;
            art = OFS(nx);
        }
    }
    lfo_clamp(&a->lfo);
    eg_clamp(&a->peg);
    eg_clamp(&a->veg);
    lfo_clamp(&a->vib);
    flt_clamp(&a->flt);
    return 0;
}

static ms_wave *find_wave(msynth *s, uint32_t id)
{
    for (ms_wave *w = s->waves[id % NBUCKET]; w; w = w->next)
        if (w->id == id) return w;
    return NULL;
}

/* 77486760 */
static void region_fix(ms_region *r)
{
    if (r->wave) {
        if (r->wave->len < r->len) r->len = r->wave->len;
        if (r->loopend == 0) r->loopend = 1;
        else if (r->len < r->loopend) r->loopend = r->len;
        if (r->loopend < r->loopstart) r->loopstart = r->loopend;
        if (r->loopend - r->loopstart < 6) r->oneshot = 1;
    }
    if (r->rate < 3000) r->rate = 3000; else if (r->rate > 200000) r->rate = 200000;
    if (r->unity > 0x7f) r->unity = 0x7f;
    r->finetune = clamps(r->finetune, -1200, 1200);
}

/* 774867f0: take the wave's length/rate/format; write the guard sample after the data */
static void region_link(ms_region *r)
{
    ms_wave *w = r->wave;
    if (!w) return;
    r->len = w->len;
    r->rate = w->rate;
    r->fmt = w->fmt;
    if (r->oneshot) {
        r->len--;
        if (w->fmt & 1) ((int16_t *)w->data)[r->len] = 0;
        else ((int8_t *)w->data)[r->len] = 0;
    } else {
        if (r->len <= r->loopstart) r->loopstart = 0;
        if (w->fmt & 1) ((int16_t *)w->data)[r->len - 1] = ((int16_t *)w->data)[r->loopstart];
        else ((int8_t *)w->data)[r->len - 1] = ((int8_t *)w->data)[r->loopstart];
    }
    region_fix(r);
}

static void artic_unref(ms_artic *a) { if (a && --a->refs == 0) free(a); }

static void instr_free(ms_instr *in)
{
    while (in->regions) {
        ms_region *r = in->regions;
        in->regions = r->next;
        artic_unref(r->artic);
        if (r->wave) r->wave->refs--;
        free(r);
    }
    free(in);
}

/* 77487d40 */
static int dl_instrument(msynth *s, const void *buf, const uint32_t *ofs, uint32_t nofs, int dls2, void **handle)
{
    const uint8_t *di = OFS(0);
    ms_instr *in = calloc(1, sizeof(*in));
    in->patch = U32(di, 0);
    uint32_t ri = U32(di, 4);
    while (ri) {
        if (ri >= nofs) { instr_free(in); return -1; }
        const uint8_t *p = OFS(ri);
        ms_region *r = calloc(1, sizeof(*r));
        /* 77487c50 defaults */
        r->rate = 0x5622; r->unity = 0x3c; r->loopend = 1; r->oneshot = 1; r->keyhi = 0x7f; r->velhi = 0x7f;
        r->next = in->regions; in->regions = r;
        /* 774877c0 */
        ri = U32(p, 0x10);
        r->keyhi = p[2]; r->keylo = p[0];
        r->velhi = p[6]; r->vello = p[4];
        if (!r->velhi && !r->vello) r->velhi = 0x7f;
        r->nonexcl = p[8] & 1;
        r->group = p[10];
        r->atten = (int16_t)((uint32_t)(I32(p, 0x2c) * 10) >> 16);
        r->finetune = S16(p, 0x2a);
        r->unity = p[0x28];
        if (U32(p, 0x34) == 0) r->oneshot = 1;
        else {
            r->loopstart = U32(p, 0x40);
            r->loopend = U32(p, 0x44) + r->loopstart;
            r->oneshot = 0;
            r->looptype = U32(p, 0x3c);
        }
        r->rate = s->imrate;
        r->wlopts = U16(p, 0x18);
        r->channel = U32(p, 0x1c);
        if (r->channel != 1 && !(r->wlopts & 2)) { instr_free(in); return -2; }
        r->waveid = U32(p, 0x20);
        uint32_t ai = U32(p, 0xc);
        if (ai) {
            if (ai >= nofs) { instr_free(in); return -1; }
            ms_artic *a = malloc(sizeof(*a));
            artic_init(a, s->imrate);
            if (artic_parse(a, buf, ofs, nofs, ai, s->imrate, dls2) < 0) { free(a); instr_free(in); return -1; }
            a->refs = 1;
            r->artic = a;
        }
        ms_wave *w = find_wave(s, r->waveid);
        if (w) { r->wave = w; w->refs++; region_link(r); }
    }
    uint32_t gi = U32(di, 8);
    if (gi == 0) {
        for (ms_region *r = in->regions; r; r = r->next)
            if (!r->artic) { instr_free(in); return -3; }
    } else {
        if (gi >= nofs) { instr_free(in); return -1; }
        ms_artic *a = malloc(sizeof(*a));
        artic_init(a, s->imrate);
        if (artic_parse(a, buf, ofs, nofs, gi, s->imrate, dls2) < 0) { free(a); instr_free(in); return -1; }
        a->refs = 0;
        for (ms_region *r = in->regions; r; r = r->next)
            if (!r->artic) { r->artic = a; a->refs++; }
        if (!a->refs) free(a);
    }
    ms_instr **b = &s->instr[in->patch % NBUCKET];
    if (U32(di, 0x14) & 1) {        /* GM instrument: behind the others */
        while (*b) b = &(*b)->next;
        *b = in;
    } else {
        in->next = *b;
        *b = in;
    }
    *handle = in;
    return 0;
}

/* 77487250 */
static int dl_wave(msynth *s, const void *buf, const uint32_t *ofs, uint32_t nofs, uint32_t id, void **handle)
{
    const uint8_t *w = OFS(0);
    if (U16(w, 0xc) != 1) return -4;
    if (U16(w, 0xe) != 1) return -5;
    if (U32(w, 8) >= nofs) return -6;
    const uint8_t *wd = OFS(U32(w, 8));
    ms_wave *wv = calloc(1, sizeof(*wv));
    uint32_t size = U32(wd, 0);
    wv->id = id;
    wv->rate = U32(w, 0x10);
    /* the original keeps pointing into the download buffer and uses one sample past the data as guard */
    wv->block = calloc(1, size + 8);
    memcpy(wv->block, wd + 4, size);
    wv->data = wv->block;
    if (U16(w, 0x1a) == 8) {
        wv->fmt = 2;
        for (uint32_t i = 0; i < size; i++) ((uint8_t *)wv->data)[i] -= 0x80;
        wv->len = size;
    } else if (U16(w, 0x1a) == 16) {
        wv->fmt = 1;
        wv->len = size >> 1;
    } else { free(wv->block); free(wv); return -7; }
    wv->len++;
    ms_wave **b = &s->waves[id % NBUCKET];
    wv->next = *b; *b = wv;
    wv->refs++;
    *handle = wv;
    return 0;
}

int msynth_download(msynth *s, const void *buf, uint32_t size, void **handle)
{
    const uint32_t *h = buf;
    if (size < 16 || h[3] > size) return -1;
    uint32_t nofs = h[2];
    uint32_t *ofs = malloc(nofs * sizeof(uint32_t) + 4);
    for (uint32_t i = 0; i < nofs; i++) {
        if (h[4 + i] >= h[3]) { free(ofs); return -1; }
        ofs[i] = h[4 + i];
    }
    int r = -1;
    switch (h[0]) {
    case 1: r = dl_instrument(s, buf, ofs, nofs, 0, handle); break;
    case 3: r = dl_instrument(s, buf, ofs, nofs, 1, handle); break;
    case 2: r = dl_wave(s, buf, ofs, nofs, h[1], handle); break;
    default: break;   /* wave articulation / streaming downloads: not yet */
    }
    free(ofs);
    return r;
}

int msynth_unload(msynth *s, void *handle)
{
    for (int b = 0; b < NBUCKET; b++) {
        for (ms_instr **p = &s->instr[b]; *p; p = &(*p)->next)
            if (*p == handle) { ms_instr *in = *p; *p = in->next; instr_free(in); return 0; }
        for (ms_wave **p = &s->waves[b]; *p; p = &(*p)->next)
            if (*p == handle) { ms_wave *w = *p; *p = w->next; free(w->block); free(w); return 0; }
    }
    return -1;
}

/* 774869e0: an instrument with this patch that has a region for (note, vel) */
static ms_instr *find_instr(msynth *s, uint32_t patch, uint32_t note, uint32_t vel)
{
    for (ms_instr *in = s->instr[patch % NBUCKET]; in; in = in->next) {
        if (in->patch != patch) continue;
        for (ms_region *r = in->regions; r; r = r->next)
            if (r->keylo <= note && note <= r->keyhi && r->vello <= vel && vel <= r->velhi) return in;
    }
    return NULL;
}

/* 77486920 */
static ms_region *next_region(ms_instr *in, ms_region *r, uint32_t note, uint32_t vel)
{
    for (r = r ? r->next : in->regions; r; r = r->next)
        if (!(note < r->keylo || r->keyhi < note || vel < r->vello || r->velhi < vel)) return r;
    return NULL;
}

/* ================================================================== voice parts */

/* 774895d0 */
static int64_t lfo_start(v_lfo *l, const ms_lfo *p, int64_t t, ms_curve *mw, ms_curve *press)
{
    l->press = press;
    l->mw = mw;
    l->on = 1;
    l->p = *p;
    l->start = t;
    if (!p->mw2pitch && !p->mw2vol && !p->lfo2pitch && !p->lfo2vol) { l->interval = 44100; return 44100; }
    l->interval = (int32_t)(0x200000 / (int64_t)p->freq);
    return l->interval;
}

/* 77489650 */
static int32_t lfo_wave(v_lfo *l, int64_t t, int64_t *next)
{
    if (!l->on) return 0;
    int64_t dt = t - (l->start + l->p.delay);
    if (dt < 0) { *next = -dt; return 0; }
    *next = l->interval;
    int64_t ph = (int64_t)l->p.freq * dt;
    return ms_sine[(ph >> 16) & 0xff];
}

static int32_t lfo_pitch(v_lfo *l, int64_t t, int64_t *next)     /* 77489770 */
{
    if (!l->on) return 0;
    int32_t press = curve_at(l->press, t), mw = curve_at(l->mw, t);
    int32_t w = lfo_wave(l, t, next);
    return (w * (l->p.lfo2pitch + (l->p.press2pitch * press) / 127 + (l->p.mw2pitch * mw) / 127)) / 100;
}

static int32_t lfo_vol(v_lfo *l, int64_t t, int64_t *next)       /* 774896d0 */
{
    if (!l->on) return 0;
    int32_t press = curve_at(l->press, t), mw = curve_at(l->mw, t);
    int32_t w = lfo_wave(l, t, next);
    return (w * (l->p.lfo2vol + (l->p.press2vol * press) / 127 + (l->p.mw2vol * mw) / 127)) / 100;
}

static int32_t lfo_cut(v_lfo *l, int64_t t)                      /* 77489810 */
{
    int64_t next;
    if (!l->on) return 0;
    int32_t press = curve_at(l->press, t), mw = curve_at(l->mw, t);
    int32_t w = lfo_wave(l, t, &next);
    return (w * (l->p.lfo2cut + (l->p.press2cut * press) / 127 + (l->p.mw2cut * mw) / 127)) / 100;
}

/* 77489920: envelope level 0..1000 (mapped: attack through the convex table) and the time to the next change */
static int32_t eg_level(v_eg *e, int64_t t, int64_t *next, int mapped)
{
    if (t > e->reltime) {
        int64_t dt = t - e->reltime, R = e->p.release;
        if (dt >= R) { *next = I64MAX; return 0; }
        int64_t rem = R - dt, tmp;
        int32_t lvl = eg_level(e, e->reltime, &tmp, mapped);
        int32_t v = (int32_t)((int64_t)lvl * rem / R);
        int64_t q = R >> 2, h = R >> 1;
        if (dt < q - 100) *next = q - dt;
        else if (dt < h - 100) *next = h - dt;
        else *next = rem;
        return v;
    }
    int64_t dt = t - e->start;
    if (dt < e->p.delay) { *next = e->p.delay - dt; return 0; }
    dt -= e->p.delay;
    if (dt < e->p.attack) {
        int64_t lvl = e->p.attack ? dt * 1000 / e->p.attack : 0;
        *next = e->p.attack - dt;
        if (lvl < 0) lvl = 0; else if (lvl > 1000) lvl = 1000;
        return mapped ? ms_egatk[lvl / 5] : (int32_t)lvl;
    }
    dt -= e->p.attack;
    if (dt < e->p.hold) { *next = e->p.hold - dt; return mapped ? ms_hold_level : 1000; }
    dt -= e->p.hold;
    int64_t D = e->p.decay;
    if (dt >= D) { *next = 44100; return e->p.sustain; }
    int32_t lvl = (int32_t)(1000 - (int64_t)(1000 - e->p.sustain) * dt / D);
    int64_t q = D >> 2, h = D >> 1;
    if (dt < q - 100) *next = q - dt;
    else if (dt < h - 100) *next = h - dt;
    else *next = D - dt;
    return lvl;
}

/* 7748bb90 */
static int64_t eg_start(v_eg *e, const ms_eg *p, int64_t t, uint16_t key, uint16_t vel, int64_t minattack)
{
    e->start = t;
    e->on = 1;
    e->reltime = I64MAX;
    e->p = *p;
    if (e->p.attack < minattack) e->p.attack = minattack;
    if (e->p.release < minattack) e->p.release = minattack;
    e->p.attack = e->p.attack * cents2ratio((e->p.vel2attack * vel) / 0x7f) / 0x1000;
    int32_t k = (e->p.key2decay * key) / 0x7f;
    e->p.hold = e->p.hold * cents2ratio(k) / 0x1000;
    e->p.decay = e->p.decay * cents2ratio(k) / 0x1000;
    e->p.decay = e->p.decay * (1000 - e->p.sustain) / 1000;
    return e->p.delay ? e->p.delay : e->p.attack;
}

/* 7748baa0: release at t */
static void eg_release(v_eg *e, int64_t t)
{
    if (e->on) {
        int32_t lvl = eg_level(e, t, &e->reltime, 1);
        e->p.release = (int64_t)lvl * e->p.release / 1000;
    }
    e->reltime = t;
}

/* 7748bb00: release at t, quickly (at most rate/70 samples) */
static void eg_release_fast(v_eg *e, int64_t t, uint32_t rate)
{
    if (e->on) {
        int32_t lvl = eg_level(e, t, &e->reltime, 1);
        e->p.release = (int64_t)lvl * e->p.release / 1000;
        if (e->p.release >= 0 && (uint64_t)e->p.release > rate / 0x46) e->p.release = rate / 0x46;
    }
    e->reltime = t;
}

static int32_t peg_pitch(v_eg *e, int64_t t, int64_t *next)   /* 77489cd0 */
{
    if (e->on) {
        if (e->p.range) return (e->p.range * eg_level(e, t, next, 0)) / 1000;
        *next = 44100;
    }
    return 0;
}

static int32_t peg_cut(v_eg *e, int64_t t)                      /* 77489d30 */
{
    int64_t next;
    if (e->on && e->p.eg2cut) return (e->p.eg2cut * eg_level(e, t, &next, 0)) / 1000;
    return 0;
}

/* 77489d80: low-pass coefficients for the cutoff in effect */
static void flt_coefs(v_flt *f, int64_t t, int32_t cc74, int32_t *b0, int32_t *a1, int32_t *a2)
{
    if (f->p.cutoff == 0x7fff) { *b0 = 0x40000000; *a1 = 0; *a2 = 0; return; }
    int32_t lfo = lfo_cut(f->lfo, t), eg = peg_cut(f->peg, t);
    int32_t row = f->p.qidx * 89;
    int32_t c = f->keycut + f->velcut + f->p.cutrel + lfo + eg + cc74;
    if (c >= 0) {
        int32_t i = c / 100, r = c % 100;
        if (i >= 0) {
            if (i > 0x57) { *b0 = ms_flt_b0[row + 88]; *a1 = ms_flt_a1[row + 88]; *a2 = ms_flt_a2[row + 88]; return; }
            const int32_t *B = ms_flt_b0 + row + i, *A1 = ms_flt_a1 + row + i, *A2 = ms_flt_a2 + row + i;
            if (i > 0x53) {
                *b0 = (int32_t)((((uint32_t)(B[1] - B[0]) >> 1) * (uint32_t)r) / 0x32 + (uint32_t)B[0]);
                *a1 = A1[0] - (int32_t)((((uint32_t)(A1[0] - A1[1]) >> 1) * (uint32_t)r) / 0x32);
                *a2 = A2[0] - (int32_t)((((uint32_t)(A2[0] - A2[1]) >> 1) * (uint32_t)r) / 0x32);
                return;
            }
            *b0 = (int32_t)(((uint32_t)(B[1] - B[0]) * (uint32_t)r) / 100 + (uint32_t)B[0]);
            *a1 = A1[0] - (int32_t)(((uint32_t)(A1[0] - A1[1]) * (uint32_t)r) / 100);
            *a2 = A2[0] - (int32_t)(((uint32_t)(A2[0] - A2[1]) * (uint32_t)r) / 100);
            return;
        }
    }
    *b0 = ms_flt_b0[row]; *a1 = ms_flt_a1[row]; *a2 = ms_flt_a2[row];
}

/* ================================================================== sample playback (CDigitalAudio) */

/* 7748a220 */
static void da_start(v_da *d, msynth *s, const ms_region *r, int32_t tune, uint32_t note)
{
    d->lastpitch = 0;
    d->y1 = d->y2 = 0;
    d->b0 = d->a1 = d->a2 = 0;
    d->src = *r;
    d->data = r->wave->data;
    d->synth = s;
    d->oneshot = r->oneshot;
    r->wave->refs += 2;
    int32_t ratio = cents2ratio(r->finetune + tune + ((int32_t)note - r->unity) * 100);
    d->base = (int32_t)((uint32_t)(r->rate * (uint32_t)ratio) / s->rate);
    d->inc = d->base;
    d->big = r->len > 0x7ffff;
    if (r->loopend - r->loopstart > 0x7ffff) d->oneshot = 1;
    d->pos64 = 0;
    d->lstart64 = (int64_t)r->loopstart << 12;
    d->lend64 = (int64_t)r->loopend << 12;
    d->end64 = (int64_t)r->len << 12;
    d->lstart = (int32_t)d->lstart64;
    d->lend = (int32_t)d->lend64;
    d->pos = 0;
    if ((uint64_t)d->lend64 <= (uint64_t)d->lstart64) d->oneshot = 1;
    d->end = d->big ? 0x7fffffff : (int32_t)d->end64;
    d->played = 0;
}

/* 7748a650: keep positions of long waves within 32 bits by moving the data pointer */
static void da_rebase(v_da *d)
{
    if (!d->big) return;
    uint64_t p = (uint64_t)d->pos64;
    if (!d->oneshot && (uint64_t)d->lend64 <= p) p = (uint64_t)d->lend64;
    uint64_t smp = (p >> 12) & ~(uint64_t)1;
    d->rebase = (int32_t)smp;
    d->pos = (int32_t)(d->pos64 - (int64_t)(smp << 12));
    if ((uint64_t)d->lend64 >= (smp << 12) && (uint64_t)d->lend64 - (smp << 12) < 0x7fffffff) {
        d->lstart = (int32_t)(d->lstart64 - (int64_t)(smp << 12));
        d->lend = (int32_t)(d->lend64 - (int64_t)(smp << 12));
    } else { d->lstart = 0; d->lend = 0x7fffffff; }
    uint64_t e = (uint64_t)d->end64 - (smp << 12);
    d->end = ((uint64_t)d->end64 >= (smp << 12) && e < 0x80000000u) ? (int32_t)e : 0x7fffffff;
    uint64_t off = (uint64_t)(uint32_t)smp;
    if (d->src.fmt & 2) off >>= 1;
    d->data = (uint8_t *)d->src.wave->data + off * 2;
}

typedef struct {
    int32_t nsamples, step, dpitch, end, looplen;
    int32_t dgain[2];
    int32_t *old;
    int32_t db0, da1, da2;
} mixargs;

/* 774892c0 / 77488a60: stereo, linear interpolation, 16/8 bit samples */
static uint32_t mix_stereo(v_da *d, int16_t *out, const mixargs *m, int bits8)
{
    int32_t pos = d->pos, inc = d->inc, gL = m->old[0], gR = m->old[1];
    int32_t accI = inc << 8, accL = gL << 8, accR = gR << 8, cnt = m->step;
    int32_t k;
    for (k = 0; k < m->nsamples; k++) {
        if (m->end <= pos) {
            if (!m->looplen) break;
            pos -= m->looplen;
        }
        if (--cnt == 0) {
            accI += m->dpitch; accL += m->dgain[0]; inc = accI >> 8; gL = accL >> 8;
            accR += m->dgain[1]; gR = accR >> 8; cnt = m->step;
        }
        int32_t i = pos >> 12, f = pos & 0xfff, smp, a, b;
        pos += inc;
        if (bits8) { a = ((int8_t *)d->data)[i]; b = ((int8_t *)d->data)[i + 1]; }
        else { a = ((int16_t *)d->data)[i]; b = ((int16_t *)d->data)[i + 1]; }
        smp = (int32_t)((uint32_t)(b - a) * (uint32_t)f) >> 12;
        smp += a;
        int sh = bits8 ? 5 : 13;
        out[2 * k] = sat16add(out[2 * k], (int16_t)(gL * smp >> sh));
        out[2 * k + 1] = sat16add(out[2 * k + 1], (int16_t)(gR * smp >> sh));
    }
    m->old[0] = gL; m->old[1] = gR;
    d->inc = inc; d->pos = pos;
    return (uint32_t)k;
}

static inline int16_t pmulhw(int16_t a, int16_t b) { return (int16_t)(((int32_t)a * b) >> 16); }
static inline int16_t packss(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

static inline int16_t mmx_interp(const v_da *d, uint32_t pos, int bits8)
{
    int32_t a, b, f = pos & 0xfff;
    uint32_t i = pos >> 12;
    if (bits8) { a = (int16_t)(((uint8_t *)d->data)[i] << 8); b = (int16_t)(((uint8_t *)d->data)[i + 1] << 8); }
    else { a = ((int16_t *)d->data)[i]; b = ((int16_t *)d->data)[i + 1]; }
    return (int16_t)((a * (int16_t)(0x1000 - f) + b * (int16_t)f) >> 12);
}

/* 77492640 / 77492450: the MMX version, two frames at a time */
static uint32_t mix_stereo_mmx(v_da *d, int16_t *out, const mixargs *m, int bits8)
{
    uint32_t pos = (uint32_t)d->pos, inc = (uint32_t)d->inc, accI = inc << 8, i = 0, n2 = (uint32_t)m->nsamples * 2;
    uint32_t end = (uint32_t)m->end, ll = (uint32_t)m->looplen;
    int32_t accL = m->old[0] << 8, accR = m->old[1] << 8;
    uint32_t mL = (uint32_t)accL >> 5, mR = (uint32_t)accR >> 5;
    int32_t cnt = m->step;
    for (;;) {
        uint32_t next = pos;
        if (n2 <= i) { d->inc = (int32_t)inc; d->pos = (int32_t)next; return i >> 1; }
        if (end <= pos) {
            if (!ll) { d->inc = (int32_t)inc; d->pos = (int32_t)next; return i >> 1; }
            pos -= ll;
        }
        uint32_t p1 = pos + inc;
        cnt -= 2;
        if (cnt == 0) {
            accL += m->dgain[0]; accR += m->dgain[1];
            accI += (uint32_t)m->dpitch;
            mL = (uint32_t)(accL < 0 ? 0 : accL) >> 5;
            mR = (uint32_t)(accR < 0 ? 0 : accR) >> 5;
            inc = accI >> 8;
            cnt = m->step;
        }
        int single = 0;
        if (n2 <= i + 2) single = 1;
        else if (end <= p1) {
            if (!ll) single = 1;
            else p1 -= ll;
        }
        int16_t gl = packss((int32_t)mL), gr = packss((int32_t)mR);
        if (single) {
            int16_t s0 = mmx_interp(d, pos, bits8);
            out[i] = sat16add(out[i], pmulhw(gl, s0));
            out[i + 1] = sat16add(out[i + 1], pmulhw(gr, s0));
            d->inc = (int32_t)inc; d->pos = (int32_t)p1;
            return (i + 2) >> 1;
        }
        int16_t s0 = mmx_interp(d, pos, bits8), s1 = mmx_interp(d, p1, bits8);
        out[i] = sat16add(out[i], pmulhw(gl, s0));
        out[i + 1] = sat16add(out[i + 1], pmulhw(gr, s0));
        out[i + 2] = sat16add(out[i + 2], pmulhw(gl, s1));
        out[i + 3] = sat16add(out[i + 3], pmulhw(gr, s1));
        pos = p1 + inc;
        i += 4;
    }
}

/* 77488ff0 / 77488d20: stereo with the DLS2 low-pass.  The original adds into the output with a 32 bit add and
 * tests the overflow flag of that, so it never saturates: results wrap at 16 bits. */
static uint32_t mix_stereo_flt(v_da *d, int16_t *out, const mixargs *m, int bits8)
{
    int32_t pos = d->pos, inc = d->inc, gL = m->old[0], gR = m->old[1];
    int32_t accI = inc << 8, accL = gL << 8, accR = gR << 8, cnt = m->step;
    int32_t b0 = d->b0, a1 = d->a1, a2 = d->a2;
    uint32_t k;
    for (k = 0; k < (uint32_t)m->nsamples; k++) {
        if (m->end <= pos) {
            if (!m->looplen) break;
            pos -= m->looplen;
        }
        if (--cnt == 0) {
            cnt = m->step;
            accI += m->dpitch; inc = accI >> 8;
            accL += m->dgain[0]; gL = accL >> 8;
            accR += m->dgain[1]; gR = accR >> 8;
            b0 += m->db0; a1 += m->da1; a2 += m->da2;
        }
        int32_t i = pos >> 12, f = pos & 0xfff, a, b, x;
        pos += inc;
        if (bits8) { a = ((int8_t *)d->data)[i]; b = ((int8_t *)d->data)[i + 1]; }
        else { a = ((int16_t *)d->data)[i]; b = ((int16_t *)d->data)[i + 1]; }
        x = ((int32_t)((uint32_t)(b - a) * (uint32_t)f) >> 12) + a;
        if (bits8) x *= 0x100;
        int32_t y = muldiv(x, b0, 0x40000000) + muldiv(d->y1, a1, 0x40000000) - muldiv(d->y2, a2, 0x40000000);
        d->y2 = d->y1; d->y1 = y;
        out[2 * k] = (int16_t)(out[2 * k] + (int16_t)(y * gL >> 15));
        out[2 * k + 1] = (int16_t)(out[2 * k + 1] + (int16_t)(y * gR >> 15));
    }
    m->old[0] = gL; m->old[1] = gR;
    d->inc = inc; d->pos = pos;
    d->b0 = b0; d->a1 = a1; d->a2 = a2;
    return k;
}

/* 7748a720: mix nsamples of this voice; returns 0 when the voice has ended */
static int da_mix(v_da *d, int16_t **bufs, uint32_t nbuf, int stereo, uint32_t n, uint32_t maxdelta,
                  const int32_t *newg, int32_t *oldg, int32_t pitch, int filter, int32_t b0, int32_t a1, int32_t a2)
{
    msynth *s = d->synth;
    if (n == 0) {
        d->inc = cents2ratio(pitch) * d->base >> 12;
        d->lastpitch = pitch;
        d->b0 = b0; d->a1 = a1; d->a2 = a2;
        return 1;
    }
    if (!d->src.wave || !d->src.wave->data) return 0;
    uint32_t dp = (uint32_t)abs(pitch - d->lastpitch) * 2;
    if ((int32_t)maxdelta <= (int32_t)dp) maxdelta = dp;
    d->lastpitch = pitch;
    uint32_t step;
    if ((maxdelta >> 1) == 0 || (step = (n * 8) / (maxdelta >> 1)) > 0x200) step = 0x200;
    else if (step == 0) step = 1;
    step = (step + 3) & ~3u;
    int32_t target = cents2ratio(pitch) * d->base >> 12;
    mixargs m;
    memset(&m, 0, sizeof(m));
    m.step = (int32_t)step;
    m.dpitch = muldiv(target - d->inc, (int32_t)(step << 8), (int32_t)n);
    int flags = 0;
    if (stereo) {
        m.dgain[0] = muldiv(newg[0] - oldg[0], (int32_t)(step << 8), (int32_t)n);
        m.dgain[1] = muldiv(newg[1] - oldg[1], (int32_t)(step << 8), (int32_t)n);
        flags = 0x40;
    } else {
        for (uint32_t i = 0; i < nbuf && i < 2; i++) m.dgain[i] = muldiv(newg[i] - oldg[i], (int32_t)(step << 8), (int32_t)n);
    }
    if (s->mmx && n > 8) flags |= 0x10;
    flags |= d->src.fmt;
    if (filter) {
        flags |= 0x80;
        m.db0 = muldiv(b0 - d->b0, (int32_t)step, (int32_t)n);
        m.da1 = muldiv(a1 - d->a1, (int32_t)step, (int32_t)n);
        m.da2 = muldiv(a2 - d->a2, (int32_t)step, (int32_t)n);
    }
    m.old = oldg;
    uint32_t done = 0;
    for (;;) {
        if (n < 9) flags &= ~0x10;
        if (d->big) da_rebase(d);
        int32_t startpos = 0;
        if (!d->oneshot) {
            m.end = d->lend;
            m.looplen = d->lend - d->lstart;
            if (m.looplen <= target) return 0;
            if (d->end < m.looplen) return 0;
        } else {
            m.end = d->end;
            m.looplen = 0;
            startpos = d->pos;
        }
        m.nsamples = (int32_t)n;
        uint32_t mixed;
        int16_t *out = bufs[0];
        switch (flags) {
        case 0x41: case 0x42: mixed = mix_stereo(d, out, &m, flags & 2); break;
        case 0x51: case 0x52: mixed = mix_stereo_mmx(d, out, &m, flags & 2); break;
        case 0xc1: case 0xc2: case 0xd1: case 0xd2: mixed = mix_stereo_flt(d, out, &m, flags & 2); break;
        default: return 0;  /* multi-buffer paths: not yet */
        }
        if (d->big) {
            d->pos64 = d->pos;
            d->pos64 += (int64_t)(uint32_t)d->rebase << 12;
            d->rebase = 0;
        }
        if (!d->oneshot) {
            if (n <= mixed) break;
            done += mixed << (stereo & 31);
            d->pos += d->lstart - d->lend;
            for (uint32_t i = 0; i < nbuf; i++) bufs[i] += done;
        } else {
            if (n <= mixed) { d->played += (d->pos - startpos) >> 12; break; }
            d->played += ((d->pos - startpos) >> 12) - 1;
            return 0;   /* streamed waves would continue with the next buffer */
        }
        n -= mixed;
    }
    d->inc = target;
    d->b0 = b0; d->a1 = a1; d->a2 = a2;
    return 1;
}

/* ================================================================== voices (CVoice) */

static int32_t curve_mb(const ms_curve *c, int64_t t) { return ms_cc2mb[curve_at(c, t) & 0x7f]; }

/* the bus description of the Mix call in progress (77485a00 stores it in the synth for the voices) */
static int find_bus(msynth *s, uint32_t id, uint32_t *func, int32_t *pitch, uint32_t *index);

static void voice_free_wave(ms_voice *v)    /* 7748b160 */
{
    v->active = 0;
    if (v->da.src.wave) { v->da.src.wave->refs -= 2; v->da.src.wave = NULL; }
}

/* 7748b380: pitch modulation (cents) at t */
static int32_t voice_pitch(ms_voice *v, int64_t t)
{
    int64_t next = v->interval;
    int32_t p = lfo_pitch(&v->lfo, t, &next);
    if (next < v->interval) v->interval = next;
    p += lfo_pitch(&v->vib, t, &next);
    if (next < v->interval) v->interval = next;
    p += peg_pitch(&v->peg, t, &next);
    if (next < v->interval) v->interval = next;
    p += (curve_at(v->pb, t) - 0x2000) * v->pb->range >> 13;
    return p;
}

/* 7748b1b0: volume (millibels) at t: overall, left, right, reverb and chorus sends */
static void voice_vol(ms_voice *v, int64_t t, int32_t *base, int32_t *L, int32_t *R, int32_t *rev, int32_t *cho)
{
    msynth *s = v->synth;
    int64_t next = v->interval;
    int32_t vm = 0;
    if (v->veg.on) vm = (int32_t)((uint32_t)eg_level(&v->veg, t, &next, 1) * 0x60) / 10 - 0x2580;
    v->volmb = vm;
    if (next < v->interval) v->interval = next;
    *base = vm;
    *base += lfo_vol(&v->lfo, t, &next);
    if (next < v->interval) v->interval = next;
    *base += curve_mb(v->expr, t);
    if (s->dynvol) *base += curve_mb(v->vol, t);
    *base += s->gain;
    *L = *base;
    *R = *base;
    if (s->stereo && s->dynpan && !v->multichan) {
        int p;
        if (!v->pan) p = 0x3f;
        else {
            p = curve_at(v->pan, t) + v->panofs;
            if (p < 0) p = 0; else if (p > 0x7f) p = 0x7f;
        }
        *L += ms_pan[127 - p];
        *R += ms_pan[p];
    }
    *rev = ms_send2mb[curve_at(v->rev, t) & 0x7f] + *base;
    *cho = ms_send2mb[curve_at(v->cho, t) & 0x7f] + *base;
}

/* 7748bd10: note off at t */
static void voice_noteoff(ms_voice *v, int64_t t)
{
    if (!v->keyon) return;
    if (t <= v->start) t = v->start + 1;
    eg_release(&v->peg, t);
    eg_release(&v->veg, t);
    v->stop = t;
    v->keyon = 0;
    v->sustained = 0;
    v->x1e0 = 0;
    if (v->looptype == 1) v->da.oneshot = 1;
}

/* 7748bdb0: fast release (stealing, exclusive groups, all sound off) */
static void voice_stop(ms_voice *v, int64_t t)
{
    msynth *s = v->synth;
    v->stealing = 1;
    if (!v->keyon && !v->sustained) { eg_release_fast(&v->veg, v->stop, s->rate); return; }
    if (t <= v->start) t = v->start + 1;
    eg_release(&v->peg, t);
    eg_release_fast(&v->veg, t, s->rate);
    v->stop = t;
    v->keyon = 0;
    v->sustained = 0;
}

typedef struct {
    ms_curve *mw, *pb, *expr, *vol, *pan, *press, *rev, *cho, *cc74;
    const uint32_t *buses;
} ms_chan;

/* 7748be80: start a voice for a region */
static int voice_noteon(ms_voice *v, msynth *s, ms_region *r, int64_t t, const ms_chan *c, uint16_t note, uint16_t vel,
                        int32_t master, int32_t tune)
{
    v->synth = s;
    ms_artic *a = r->artic;
    if (!a) return 0;
    v->looptype = r->looptype;
    int32_t vb = master;
    if (!s->dynvol) vb += curve_mb(c->vol, t);
    tune += r->tune;
    v->group = r->group;
    v->nonexcl = r->nonexcl;
    int32_t iv = vb + r->atten + ms_cc2mb[vel & 0x7f];
    v->panofs = a->pan;
    v->multichan = r->wlopts & 2;
    uint32_t func;
    int32_t bp;
    if (c->buses[0] == 1 && find_bus(s, c->buses[1], &func, NULL, NULL) && func == 0) v->multichan = 1;
    int32_t volR = iv, volL = iv;
    if (s->stereo && !s->dynpan && !v->multichan) {
        int p = curve_at(c->pan, t) + v->panofs;
        if (p < 0) p = 0; else if (p > 0x7f) p = 0x7f;
        volR = ms_pan[p] + iv;
        volL = ms_pan[127 - p] + iv;
    }
    int32_t pofs = 0;
    int dyn = 0;
    for (uint32_t i = 0; i < c->buses[0] && i < 32; i++) {
        bp = 0;
        if (!find_bus(s, c->buses[1 + i], &func, &bp, NULL)) continue;
        if (!dyn) pofs = bp;
        v->busofs[i] = -0x2580;
        if (func < 0x12) {
            if (!(r->wlopts & 2)) {
                if (func == 0) v->busofs[i] = volL;
                else if (func == 1) v->busofs[i] = volR;
            } else if (r->channel & (1u << func)) v->busofs[i] = iv;
        } else if (func <= 0x200) {
            if (func == 0x200) dyn = 1;
            v->busofs[i] = iv;
        } else if (func == 0xffffffff) v->busofs[i] = -0x2580;
        else v->busofs[i] = iv;
        v->lastvol[i] = -0x2580;
        v->lastgain[i] = DB2LIN(-1000);
    }
    int64_t iv64;
    v->interval = lfo_start(&v->lfo, &a->lfo, t, c->mw, c->press);
    iv64 = lfo_start(&v->vib, &a->vib, t, c->mw, c->press);
    if (iv64 < v->interval) v->interval = iv64;
    iv64 = eg_start(&v->peg, &a->peg, t, note, vel, 0);
    if (iv64 < v->interval) v->interval = iv64;
    iv64 = eg_start(&v->veg, &a->veg, t, note, vel, s->rate / 1000);
    if (iv64 < v->interval) v->interval = iv64;
    if (s->interval < v->interval) v->interval = s->interval;
    v->flt.p = a->flt;
    v->flt.lfo = &v->lfo;
    v->flt.peg = &v->peg;
    v->flt.velcut = (a->flt.vel2cut * vel) / 0x7f;
    v->flt.keycut = (a->flt.key2cut * note) / 0x7f;
    if (!r->wave || !r->wave->data) return 0;
    da_start(&v->da, s, r, tune, note);
    v->pb = c->pb; v->press = c->press; v->expr = c->expr; v->vol = c->vol; v->pan = c->pan;
    v->rev = c->rev; v->cho = c->cho; v->cc74 = c->cc74;
    memcpy(v->buses, c->buses, sizeof(v->buses));
    v->lastmix = t - 1;
    v->keyon = 1;
    v->stealing = 0;
    v->sustained = 0;
    v->start = t;
    v->stop = I64MAX;
    v->x1e0 = 0;
    if (v->interval == 0) {
        int32_t pitch = voice_pitch(v, t);
        int32_t cut = (curve_at(v->cc74, t) - 0x40) * 100, b0, a1, a2;
        v->lastcut = cut;
        flt_coefs(&v->flt, t, cut, &b0, &a1, &a2);
        da_mix(&v->da, NULL, 0, 0, 0, 0, NULL, NULL, pitch + pofs, v->flt.p.cutoff != 0x7fff, b0, a1, a2);
    }
    v->volmb = 0;
    return 1;
}

/* 7748b480: mix the voice over [start, end) */
static void voice_mix(ms_voice *v, int16_t **bufs, uint32_t flags, int64_t start, int64_t end)
{
    msynth *s = v->synth;
    int64_t cur = v->start, chunk = start;
    int ok = 1, ret = 1, alive;
    if (cur < start) cur = start;
    if (end <= v->lastmix) return;
    if (cur <= v->lastmix) cur = v->lastmix;
    if (cur < end) {
        do {
            if (!ok) { alive = 0; goto out; }
            chunk = v->interval + cur;
            if (end < chunk) chunk = end;
            v->interval = s->interval;
            if (v->lastmix < v->stop && v->stop < end) {
                int64_t d = v->stop - v->lastmix;
                if (d < v->interval) v->interval = d;
            }
            int32_t pitch = voice_pitch(v, chunk), base, L, R, rev, cho, b0, a1, a2;
            voice_vol(v, chunk, &base, &L, &R, &rev, &cho);
            int32_t cut = (curve_at(v->cc74, chunk) - 0x40) * 100;
            flt_coefs(&v->flt, chunk, cut, &b0, &a1, &a2);
            if (v->veg.on && v->veg.reltime < chunk && v->volmb < -8000) { v->da.oneshot = 1; ok = 0; }
            uint32_t maxd = 0, n = 0;
            int32_t newg[32] = { 0 }, oldg[32];
            int16_t *bp[32] = { 0 };
            int32_t pofs = 0;
            if (!(flags & 2)) {
                uint32_t d = (uint32_t)abs(L - v->lastvol[0]);
                if ((int32_t)d >= 0) maxd = d;
                int32_t gi = (v->busofs[0] + L) / 10;
                v->lastvol[0] = L;
                if (gi < -1000) gi = -1000; else if (gi >= 0) gi = 0;
                oldg[0] = v->lastgain[0];
                newg[0] = DB2LIN(gi);
                n = 1;
                v->lastgain[0] = newg[0];
                if (!(flags & 1)) bp[0] = bufs[0] + (cur - start);
                else {
                    d = (uint32_t)abs(R - v->lastvol[1]);
                    if ((int32_t)maxd <= (int32_t)d) maxd = d;
                    v->lastvol[1] = R;
                    newg[1] = mb2gain(v->busofs[1] + R);
                    oldg[1] = v->lastgain[1];
                    v->lastgain[1] = newg[1];
                    bp[0] = bufs[0] + (cur - start) * 2;
                }
            } else {
                ok = 0;     /* audiopath (multi-buffer) mixing: not yet */
                continue;
            }
            int fon = v->flt.p.cutoff != 0x7fff;
            if (fon) {
                uint32_t d = (uint32_t)abs(cut - v->lastcut);
                if ((int32_t)maxd <= (int32_t)d) maxd = d;
                v->lastcut = cut;
            }
            ret = da_mix(&v->da, bp, n, flags & 1, (uint32_t)(chunk - cur), maxd, newg, oldg, pofs + pitch, fon, b0, a1, a2);
            cur = chunk;
        } while (cur < end);
        alive = ok && ret;
    } else alive = 1;
out:
    v->active = alive;
    if (!alive) {
        v->active = 0;
        if (v->da.src.wave) { v->da.src.wave->refs -= 2; v->da.src.wave = NULL; }
        v->stop = chunk;
    }
    v->lastmix = chunk;
}

/* ================================================================== channel groups (CControlLogic) */

/* 77482590: GM-style reset of the per channel state */
static void cl_reset(ms_cl *c)
{
    for (int i = 0; i < 16; i++) {
        c->rpndata[i] = 0;
        c->finetune[i] = 0;
        c->drum[i] = 0;
        memset(c->scale[i], 0, sizeof(c->scale[i]));
        c->rpn[i] = 0x3fff;
        c->coarse[i] = 0;
        c->chan[i] = ms_chanmap[i];
        c->prio[i] = 0x80000010u - (uint32_t)i;
        c->mono[i] = 0;
        c->bankmsb[i] = 0;
        c->banklsb[i] = 0;
    }
    c->master = 0;
    c->gsmode = 0;
    c->xgmode = 0;
    c->drum[0] = 1;
}

static void cl_init(ms_cl *c, msynth *s)    /* 77484400 */
{
    memset(c, 0, sizeof(*c));
    c->synth = s;
    for (int i = 0; i < 16; i++) {
        c->pb[i].cur = 0x2000; c->pb[i].range = 200;
        c->vol[i].cur = 100;
        c->expr[i].cur = 0x7f;
        c->rev[i].cur = 0x28;
        c->pan[i].cur = 0x40;
        c->cc74[i].cur = 0x40;
        c->buses[i][0] = 4; c->buses[i][1] = 0; c->buses[i][2] = 1; c->buses[i][3] = 0x40; c->buses[i][4] = 0x41;
    }
    c->clean = 1;
    cl_reset(c);
}

static void cl_free(ms_cl *c)
{
    ms_node *p = c->q.head;
    while (p) { ms_node *n = p->next; free(p); p = n; }
    for (int i = 0; i < 16; i++) {
        curve_clear(&c->mw[i]); curve_clear(&c->pb[i]); curve_clear(&c->vol[i]); curve_clear(&c->expr[i]);
        curve_clear(&c->pan[i]); curve_clear(&c->rev[i]); curve_clear(&c->cho[i]); curve_clear(&c->cc74[i]);
        curve_clear(&c->press[i]);
    }
}

#define QEV(slot, code, val) (((uint32_t)(slot) << 16) | ((uint32_t)(code) << 8) | (uint32_t)(val))

/* 77483310: a short MIDI message at t */
static void cl_midi(ms_cl *c, int64_t t, uint8_t st, uint8_t d1, uint8_t d2)
{
    for (int slot = 0; slot < 16; slot++) {
        if ((st & 0xf) != c->chan[slot]) continue;
        switch (st & 0xf0) {
        case 0x80: d2 = 0; /* fall through */
        case 0x90: queue_add(&c->q, t, QEV(slot, d1, d2)); break;
        case 0xb0:
            switch (d1) {
            case 0: queue_add(&c->q, t, QEV(slot, 0xf2, d2)); break;
            case 1: c->clean = 0; curve_add(&c->mw[slot], t, d2); break;
            case 7: c->clean = 0; curve_add(&c->vol[slot], t, d2); break;
            case 10: c->clean = 0; curve_add(&c->pan[slot], t, d2); break;
            case 11: c->clean = 0; curve_add(&c->expr[slot], t, d2); break;
            case 32: queue_add(&c->q, t, QEV(slot, 0xf3, d2)); break;
            case 121:
                c->clean = 0;
                if (d2) { curve_add(&c->vol[slot], t, 100); curve_add(&c->pan[slot], t, 0x40); }
                curve_add(&c->expr[slot], t, 0x7f);
                curve_add(&c->pb[slot], t, 0x2000);
                curve_add(&c->mw[slot], t, 0);
                curve_add(&c->press[slot], t, 0);
                curve_add(&c->rev[slot], t, 0x28);
                curve_add(&c->cho[slot], t, 0);
                curve_add(&c->cc74[slot], t, 0x40);
                queue_add(&c->q, t, QEV(slot, 0xfe, 0));
                break;
            case 64: queue_add(&c->q, t, QEV(slot, 0xfe, d2)); break;
            case 120: queue_add(&c->q, t, QEV(slot, 0xfd, 0)); break;
            case 123: queue_add(&c->q, t, QEV(slot, 0xff, 0)); break;
            case 6: queue_add(&c->q, t, QEV(slot, 0xfa, d2)); break;
            case 38: queue_add(&c->q, t, QEV(slot, 0xf9, d2)); break;
            case 98: case 99: queue_add(&c->q, t, QEV(slot, 0xf8, d2)); break;
            case 100: queue_add(&c->q, t, QEV(slot, 0xf7, d2)); break;
            case 101: queue_add(&c->q, t, QEV(slot, 0xf6, d2)); break;
            case 126: queue_add(&c->q, t, QEV(slot, 0xf5, d2)); queue_add(&c->q, t, QEV(slot, 0xfd, 0)); break;
            case 127: queue_add(&c->q, t, QEV(slot, 0xf4, d2)); queue_add(&c->q, t, QEV(slot, 0xfd, 0)); break;
            case 91: c->clean = 0; curve_add(&c->rev[slot], t, d2); break;
            case 93: c->clean = 0; curve_add(&c->cho[slot], t, d2); break;
            case 74: c->clean = 0; curve_add(&c->cc74[slot], t, d2); break;
            }
            break;
        case 0xc0: queue_add(&c->q, t, QEV(slot, 0xf1, d1)); break;
        case 0xd0: c->clean = 0; curve_add(&c->press[slot], t, d1); break;
        case 0xe0: c->clean = 0; curve_add(&c->pb[slot], t, (d2 << 7) | d1); break;
        }
    }
}

/* 77483110: drop controller changes and pending notes from t on */
static void cl_flush(ms_cl *c, int64_t t)
{
    if (c->clean) return;
    c->clean = 1;
    for (int i = 0; i < 16; i++) {
        ms_curve *cv[9] = { &c->mw[i], &c->pb[i], &c->vol[i], &c->expr[i], &c->pan[i], &c->press[i], &c->rev[i],
                            &c->cho[i], &c->cc74[i] };
        for (int k = 0; k < 9; k++) if (!curve_cut(cv[k], t)) c->clean = 0;
    }
    queue_flatten(&c->q, t);
}

/* 77483880: system exclusive messages (GM/GS/XG resets, GS scale tuning/drum parts, master volume) */
static void cl_sysex(ms_cl *c, int64_t t, const uint8_t *m, uint32_t len)
{
    int reset = 0;
    if (len < 6) return;
    switch (m[1]) {
    case 0x41:      /* Roland */
        if (len < 0xb || m[3] != 0x42 || m[4] != 0x12) return;
        {
            uint32_t part = m[6] & 0xf;
            uint32_t addr = ((uint32_t)m[5] << 16) | ((uint32_t)(m[6] & 0xf0) << 8) | m[7];
            if (addr == 0x401040) {     /* scale tuning */
                if (c->gsmode) {
                    for (int k = 0; k < 12 && !(m[8 + k + 1] & 0x80); k++) {
                        c->scale[part][k] = m[8 + k] - 0x40;
                        if (k == 11) break;
                    }
                }
                return;
            }
            if (addr == 0x401015) {     /* use for rhythm part */
                if (c->gsmode) c->drum[part] = m[8];
                return;
            }
            if (addr == 0x40007f) {     /* GS reset */
                cl_reset(c);
                c->xgmode = 0;
                c->gsmode = 1;
                reset = 1;
                break;
            }
            if (addr == 0x401002 && c->gsmode && m[8]) queue_add(&c->q, t, QEV(part, 0xfb, m[8] - 1));
        }
        return;
    case 0x43:      /* Yamaha XG system on */
        if (m[3] != 'L' || m[4] || m[5] || m[6] != 0x7e || m[7]) return;
        c->xgmode = 1;
        c->gsmode = 0;
        cl_reset(c);
        c->xgmode = 1;
        c->drum[0] = 0;
        c->bankmsb[0] = 0x7f;
        reset = 1;
        break;
    case 0x7e:      /* GM system on */
        if (m[3] != 9) return;
        cl_reset(c);
        reset = 1;
        break;
    case 0x7f:      /* master volume */
        if (m[3] == 4 && m[4] == 1) queue_add(&c->q, t, QEV(0, 0xfc, m[6]));
        return;
    default:
        return;
    }
    cl_flush(c, 0);
    for (int i = 0; i < 16; i++) {
        queue_add(&c->q, t, QEV(i, 0xfd, 0));
        queue_add(&c->q, t, QEV(i, 0xfe, 0));
        curve_add(&c->vol[i], t, 100);
        curve_add(&c->pan[i], t, 0x40);
        curve_add(&c->expr[i], t, 0x7f);
        curve_add(&c->pb[i], t, 0x2000);
        curve_add(&c->mw[i], t, 0);
        curve_add(&c->press[i], t, 0);
        curve_add(&c->rev[i], t, 0x28);
        curve_add(&c->cho[i], t, 0);
        curve_add(&c->cc74[i], t, 0x40);
    }
    if (reset) {
        for (int i = 0; i < 16; i++) {
            queue_add(&c->q, t - 1, QEV(i, 0xf2, (i == 0 && c->xgmode) ? 0x7f : 0));
            queue_add(&c->q, t - 1, QEV(i, 0xf3, 0));
            queue_add(&c->q, t, QEV(i, 0xf1, 0));
        }
    }
}

static void synth_free_voice(msynth *s, ms_voice *v);
static ms_voice *synth_steal(msynth *s, uint32_t prio);
static void synth_insert(msynth *s, ms_voice *v);

/* 774828b0: process the queued events due by 'end' */
static void cl_process(ms_cl *c, int64_t start, int64_t end)
{
    msynth *s = c->synth;
    int64_t t;
    uint32_t ev;
    while (queue_pop(&c->q, end, &t, &ev)) {
        if (t < start) t = start;
        uint32_t slot = (ev >> 16) & 0xff, code = (ev >> 8) & 0xff, val = ev & 0xff;
        if (code < 0x80) {
            uint32_t note = code, vel = val;
            if (vel == 0) {
                uint32_t seq = 0;
                for (ms_voice *v = s->active; v; v = v->next) {
                    if (v->keyon && !v->sustained && v->note == note && v->slot == slot && v->cl == c &&
                        (seq == 0 || seq == v->seq)) {
                        seq = v->seq;
                        if (!c->sustain[slot]) voice_noteoff(v, t);
                        else v->sustained = 1;
                    }
                }
                continue;
            }
            uint32_t patch = c->patch[slot];
            if (c->drum[slot]) patch |= 0x80000000u;
            if (c->mono[slot])
                for (ms_voice *v = s->active; v; v = v->next)
                    if (v->keyon && v->slot == slot && v->cl == c) voice_noteoff(v, t);
            ms_instr *in = find_instr(s, patch, note, vel);
            if (!in) {
                if ((int32_t)patch < 0) { patch = 0x80000000u; in = find_instr(s, patch, note, vel); }
                else if (c->xgmode) {
                    if ((patch & 0x7f0000) == 0x7f0000) {
                        patch &= 0x7f007f;
                        in = find_instr(s, patch, note, vel);
                        if (!in) { patch = 0x7f0000; in = find_instr(s, patch, note, vel); }
                    } else {
                        patch &= 0x7f;
                        in = find_instr(s, patch, note, vel);
                    }
                }
            }
            if (!in) continue;
            s->seq++;
            for (ms_region *r = next_region(in, NULL, note, vel); r; r = next_region(in, r, note, vel)) {
                if (!r->nonexcl)
                    for (ms_voice *v = s->active; v; v = v->next)
                        if (v->slot == slot && v->note == note && v->cl == c && v->region == r) voice_stop(v, t);
                if (r->group)
                    for (ms_voice *v = s->active; v; v = v->next)
                        if (v->group == r->group && v->slot == slot && v->patch == patch && v->cl == c) voice_stop(v, t);
                ms_voice *v = s->freelist;
                if (v) { s->freelist = v->next; v->next = NULL; }
                else if ((v = s->extra)) { s->extra = v->next; v->next = NULL; }
                else if ((v = synth_steal(s, c->prio[slot]))) v->voiceid = -1;
                if (!v) continue;
                int32_t tune = c->scale[slot][note % 12] + c->finetune[slot];
                if (!c->drum[slot] && (!c->xgmode || (patch & 0x7f0000) != 0x7f0000)) tune += c->coarse[slot];
                v->note = (uint16_t)note;
                v->slot = (uint16_t)slot;
                v->patch = patch;
                v->prio = c->prio[slot];
                v->seq = s->seq;
                v->cl = c;
                v->region = r;
                ms_chan ch = { &c->mw[slot], &c->pb[slot], &c->expr[slot], &c->vol[slot], &c->pan[slot],
                               &c->press[slot], &c->rev[slot], &c->cho[slot], &c->cc74[slot], c->buses[slot] };
                if (!voice_noteon(v, s, r, t, &ch, (uint16_t)note, (uint16_t)vel, c->master, tune)) {
                    v->next = s->freelist; s->freelist = v;
                } else {
                    v->active = 1;
                    synth_insert(s, v);
                }
            }
            continue;
        }
        switch (code) {
        case 0xf1: c->patch[slot] = ((uint32_t)c->bankmsb[slot] << 16) | ((uint32_t)c->banklsb[slot] << 8) | val; break;
        case 0xf2: c->bankmsb[slot] = (uint8_t)val; break;
        case 0xf3: c->banklsb[slot] = (uint8_t)val; break;
        case 0xf4: c->mono[slot] = 0; break;
        case 0xf5: c->mono[slot] = 1; break;
        case 0xf6: c->rpn[slot] = (uint16_t)(val * 0x80 + (c->rpn[slot] & 0x7f)); break;
        case 0xf7: c->rpn[slot] = (uint16_t)((c->rpn[slot] & 0x3f80) + val); break;
        case 0xf8: c->rpn[slot] = 0x3fff; break;
        case 0xf9:
            c->rpndata[slot] = (uint16_t)((c->rpndata[slot] & 0xff80) | val);
            if (c->rpn[slot] == 1) c->finetune[slot] = (((int32_t)c->rpndata[slot] - 0x2000) * 100) / 8192;
            break;
        case 0xfa:
            c->rpndata[slot] = (uint16_t)((val << 7) | (c->rpndata[slot] & 0xc07f));
            if (c->rpn[slot] == 0) c->pb[slot].range = (int32_t)val * 100;
            else if (c->rpn[slot] == 1) c->finetune[slot] = (((int32_t)c->rpndata[slot] - 0x2000) * 100) / 8192;
            else if (c->rpn[slot] == 2) c->coarse[slot] = ((int32_t)val - 0x40) * 100;
            break;
        case 0xfb: c->chan[slot] = (uint8_t)val; break;
        case 0xfc: c->master = ms_cc2mb[val & 0x7f]; break;
        case 0xfd:
            for (ms_voice *v = s->active; v; v = v->next)
                if (v->slot == slot && v->cl == c) voice_stop(v, t);
            break;
        case 0xfe:
            c->sustain[slot] = val;
            if (!val)
                for (ms_voice *v = s->active; v; v = v->next)
                    if (v->sustained && v->slot == slot && v->cl == c) voice_noteoff(v, t);
            break;
        case 0xff:
            for (ms_voice *v = s->active; v; v = v->next)
                if (v->keyon && !v->sustained && v->slot == slot && v->cl == c) {
                    if (!c->sustain[slot]) voice_noteoff(v, t);
                    else v->sustained = 1;
                }
            break;
        }
    }
}

/* 774826a0: retire controller history before 'end' */
static void cl_retire(ms_cl *c, int64_t end)
{
    if (c->clean) return;
    c->clean = 1;
    for (int i = 0; i < 16; i++) {
        ms_curve *cv[9] = { &c->mw[i], &c->pb[i], &c->vol[i], &c->expr[i], &c->pan[i], &c->press[i], &c->rev[i],
                            &c->cho[i], &c->cc74[i] };
        for (int k = 0; k < 9; k++) if (!curve_retire(cv[k], end)) c->clean = 0;
    }
}

/* ================================================================== synth (CSynth) */

static struct { const uint32_t *ids, *funcs; const int32_t *pitch; uint32_t n; } g_bus;   /* set during a Mix */

static int find_bus(msynth *s, uint32_t id, uint32_t *func, int32_t *pitch, uint32_t *index)   /* 774848c0 */
{
    (void)s;
    for (uint32_t i = 0; i < g_bus.n; i++)
        if (g_bus.ids[i] == id) {
            *func = g_bus.funcs[i];
            if (pitch) *pitch = g_bus.pitch[i];
            if (index) *index = i;
            return 1;
        }
    return 0;
}

static ms_voice *voice_new(void)    /* 7748afa0 */
{
    ms_voice *v = calloc(1, sizeof(*v));
    v->lfo.p.freq = 0xedc;
    v->vib.p.freq = 0xedc;
    v->peg.on = 0; v->veg.on = 0;
    v->peg.p.sustain = 1000; v->veg.p.sustain = 1000;
    fltp_init(&v->flt.p, 0x5622);
    v->stop = I64MAX;
    v->buses[0] = 4; v->buses[1] = 0; v->buses[2] = 1; v->buses[3] = 0x40; v->buses[4] = 0x41;
    return v;
}

static void voice_push(ms_voice **list, ms_voice *v) { v->next = *list; *list = v; }

static void synth_remove(msynth *s, ms_voice *v)     /* 77482490 on the active list */
{
    for (ms_voice **p = &s->active; *p; p = &(*p)->next)
        if (*p == v) { *p = v->next; v->next = NULL; return; }
}

static void synth_insert(msynth *s, ms_voice *v)     /* 77484a60: by priority */
{
    ms_voice **p = &s->active;
    if (!*p) { v->next = NULL; *p = v; return; }
    if (v->prio < (*p)->prio) { v->next = *p; *p = v; return; }
    ms_voice *a = *p;
    while (a->next && !(v->prio < a->next->prio)) a = a->next;
    v->next = a->next;
    a->next = v;
}

static void synth_free_voice(msynth *s, ms_voice *v) { (void)s; voice_free_wave(v); }

static ms_voice *synth_steal(msynth *s, uint32_t prio)  /* 774849b0 */
{
    ms_voice *best = NULL;
    for (ms_voice *v = s->active; v; v = v->next) {
        if (prio < v->prio) continue;
        if (!best) best = v;
        else if (!v->keyon) { if (best->keyon == 1 || v->volmb < best->volmb) best = v; }
        else if (v->start < best->start) best = v;
    }
    if (best) {
        synth_free_voice(s, best);
        best->active = 0;
        synth_remove(s, best);
    }
    return best;
}

static ms_voice *synth_candidate(msynth *s)          /* 77484920 */
{
    ms_voice *best = s->active;
    if (!best) return NULL;
    for (ms_voice *v = best->next; v; v = v->next) {
        if (v->stealing) continue;
        if (!best->stealing) {
            if (v->prio <= best->prio) {
                if (!v->keyon) { if (best->keyon || v->volmb < best->volmb) best = v; }
                else if (best->keyon && v->start < best->start) best = v;
            }
        } else best = v;
    }
    return best->stealing ? NULL : best;
}

static void synth_prealloc(msynth *s, int64_t end)   /* 77484ad0 */
{
    int n = 0;
    for (ms_voice *v = s->extra; v; v = v->next) n++;
    int k = s->extravoices - n;
    if (k <= 0) return;
    while (s->freelist) {
        ms_voice *v = s->freelist;
        s->freelist = v->next;
        voice_push(&s->extra, v);
        if (--k < 1) return;
    }
    for (ms_voice *v = s->active; v; v = v->next) if (v->stealing) k--;
    ms_voice *v;
    while (k > 0 && (v = synth_candidate(s))) { voice_stop(v, end); k--; }
}

msynth *msynth_new(uint32_t rate, int voices, int groups, int stereo)
{
    msynth *s = calloc(1, sizeof(*s));
    s->mmx = 1;
    s->dynpan = 1;
    s->dynvol = 1;
    s->gain = 600;
    s->imrate = rate;
    s->rate = rate;
    s->interval = (rate + 0x13) / 0x14;
    s->stereo = stereo ? 1 : 0;
    if (voices < 1) voices = 1;
    if (voices > 1000) voices = 1000;
    int extra = voices / 4;
    if (extra < 4) extra = 4;
    /* the constructor makes 32 + 8 voices, Open grows the lists to the requested size (pushing at the head) */
    for (int i = 0; i < 32; i++) voice_push(&s->freelist, voice_new());
    for (int i = 0; i < 8; i++) voice_push(&s->extra, voice_new());
    for (int i = 32; i < voices; i++) voice_push(&s->freelist, voice_new());
    for (int i = 8; i < extra; i++) voice_push(&s->extra, voice_new());
    for (int i = voices; i < 32; i++) { ms_voice *v = s->freelist; s->freelist = v->next; free(v); }
    for (int i = extra; i < 8; i++) { ms_voice *v = s->extra; s->extra = v->next; free(v); }
    s->maxvoices = (int16_t)voices;
    s->extravoices = (int16_t)extra;
    if (groups < 1) groups = 1;
    s->ncl = (uint32_t)groups;
    s->cl = calloc((size_t)groups, sizeof(ms_cl *));
    for (int i = 0; i < groups; i++) {
        s->cl[i] = malloc(sizeof(ms_cl));
        cl_init(s->cl[i], s);
        s->cl[i]->gain = s->gain;
    }
    return s;
}

void msynth_set_mmx(msynth *s, int on) { s->mmx = on; }

void msynth_set_gain(msynth *s, int32_t mb)    /* GUID_DMUS_PROP_Volume: 774846d0 */
{
    s->gain = mb;
    for (uint32_t i = 0; i < s->ncl; i++) s->cl[i]->gain = mb;
}

static void free_list(ms_voice *v) { while (v) { ms_voice *n = v->next; free(v); v = n; } }

void msynth_free(msynth *s)
{
    if (!s) return;
    free_list(s->freelist); free_list(s->extra); free_list(s->active);
    for (uint32_t i = 0; i < s->ncl; i++) { cl_free(s->cl[i]); free(s->cl[i]); }
    free(s->cl);
    for (int b = 0; b < NBUCKET; b++) {
        while (s->instr[b]) { ms_instr *in = s->instr[b]; s->instr[b] = in->next; instr_free(in); }
        while (s->waves[b]) { ms_wave *w = s->waves[b]; s->waves[b] = w->next; free(w->block); free(w); }
    }
    free(s);
}

void msynth_midi(msynth *s, int64_t t, const uint8_t *msg, uint32_t len, uint32_t group)
{
    if (group > s->ncl) return;
    for (uint32_t i = group ? group - 1 : 0; i < (group ? group : s->ncl); i++) {
        if (len < 5 && msg[0] != 0xf0) cl_midi(s->cl[i], t, msg[0], len > 1 ? msg[1] : 0, len > 2 ? msg[2] : 0);
        else cl_sysex(s->cl[i], t, msg, len);
    }
}

/* 77484b60 / 77484c00: the mix runs at half scale; the output is doubled with saturation afterwards */
static void halve(msynth *s, int16_t *b, uint32_t n)
{
    uint32_t i = 0;
    if (s->mmx && n > 15) for (; i < (n & ~15u); i++) b[i] >>= 1;
    for (; i < n; i++) b[i] = (int16_t)(b[i] << 1);     /* sic: the non-MMX tail shifts the other way */
}

static void twice(msynth *s, int16_t *b, uint32_t n)
{
    uint32_t i = 0;
    if (s->mmx && n > 3) for (; i < (n & ~3u); i++) b[i] = packss(b[i] * 2);
    for (; i < n; i++) {
        int v = b[i] * 2;
        b[i] = (int16_t)(v < -0x7fff ? -0x7fff : v > 0x7fff ? 0x7fff : v);
    }
}

void msynth_render(msynth *s, int16_t *buf, uint32_t frames, int64_t pos)
{
    static const uint32_t ids[2] = { 0, 1 }, funcs[2] = { 0, 1 };
    static const int32_t pitch[2] = { 0, 0 };
    uint32_t flags = (uint32_t)s->stereo;
    int64_t end = pos + frames;
    g_bus.ids = ids; g_bus.funcs = funcs; g_bus.pitch = pitch; g_bus.n = s->stereo ? 2 : 1;
    synth_prealloc(s, end);
    halve(s, buf, frames << s->stereo);
    for (uint32_t i = 0; i < s->ncl; i++) cl_process(s->cl[i], pos, end);
    int16_t *bufs[1] = { buf };
    for (ms_voice *v = s->active, *next; v; v = next) {
        next = v->next;
        if (v->x1e0 && v->x1e0 < end) voice_noteoff(v, v->x1e0);
        voice_mix(v, bufs, flags, pos, end);
        if (!v->active) {
            synth_remove(s, v);
            voice_push(&s->freelist, v);
        }
    }
    for (uint32_t i = 0; i < s->ncl; i++) cl_retire(s->cl[i], end);
    twice(s, buf, frames << s->stereo);
    if (s->lastrender < end) s->lastrender = end;
    g_bus.n = 0;
}
