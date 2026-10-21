/* reDM: music values (dmime's MusicToMIDI / MIDIToMusic), translated from XP dmime's code (77307a02..773086a0).
 * The helpers take some arguments in registers in the original; they keep its byte and word arithmetic, its signed
 * remainders and its shift counts (taken modulo 32, as x86 does). */
#include "dmtheory.h"
#include <string.h>

_Static_assert(sizeof(dm_subchord) == 0x14, "DMUS_SUBCHORD");
_Static_assert(sizeof(dm_chord_key) == 0xcc, "DMUS_CHORD_KEY");

static uint32_t shr(uint32_t x, uint32_t n) { return x >> (n & 31); }
static int32_t sar(int32_t x, uint32_t n) { return x >> (n & 31); }

/* 77307a02 */
static uint32_t popcount(uint32_t x) { uint32_t n = 0; for (; x; x &= x - 1) n++; return n; }

/* 77307a5d: the bits two 24-bit patterns agree on */
static uint32_t agree24(uint32_t a, uint32_t b)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < 24; i++) if ((a & (1u << i)) == (b & (1u << i))) n++;
    return n;
}

/* 77307a89: a scale of fewer than five notes is replaced by the closest of the twelve diatonic modes */
static const uint32_t g_modes[12] = { 0xab5ab5, 0x6ad6ad, 0x5ab5ab, 0xad5ad5, 0x6b56b5, 0x5ad5ad, 0x56b56b, 0xd5ad5a,
                                      0xb56b56, 0xd6ad6a, 0xb5ab5a, 0xad6ad6 };
static uint32_t fix_scale(uint32_t s)
{
    if (popcount(s & 0xfff) > 4) return s;
    uint32_t best = 0xab5ab5, n = 0;
    for (int i = 0; i < 12; i++) {
        uint32_t k = agree24(s, g_modes[i]);
        if (k > n) { best = g_modes[i]; n = k; }
    }
    return best;
}

/* 77307ae4: a 12-bit pattern doubled to 24 (one with bits above 24 kept) */
static uint32_t dbl(uint32_t x) { return (x & 0xff000000) ? x : ((x & 0xfffff000) << 12) | x; }

/* 77307b14: a scale moved up by root (C-based) */
static uint32_t scale_up(uint8_t root, uint32_t s)
{
    s &= 0xfff;
    uint32_t d = shr(s << 12 | s, 12 - root % 12) & 0xfff;
    return d << 12 | d;
}
/* 77307b52: a scale moved down by root */
static uint32_t scale_down(uint8_t root, uint32_t s)
{
    s &= 0xfff;
    uint32_t d = shr(s << 12 | s, root % 12) & 0xfff;
    return d << 12 | d;
}
/* 77307b8c (eax root, ecx scale): the chord the scale makes from root (every other scale note, four of them) */
static uint32_t scale_chord(uint8_t root, uint32_t s)
{
    uint32_t t = shr(s, root % 12) & 0xfff, n = 0;
    t = t << 12 | t;
    for (uint32_t i = 0; i < 24; i++) {
        uint32_t b = 1u << i;
        if (!(t & b)) continue;
        if ((n & 1) || n > 7) t &= ~b;
        n++;
    }
    return t;
}
/* 77307bd8 (eax chord): the chord pattern relative to root, its inversions folded the way dmime does; *flag when the
 * result reaches into the octave below */
static uint32_t key_chord(uint32_t pat, uint8_t root, uint8_t key, uint8_t *flag)
{
    *flag = 0;
    uint8_t cl = root % 12, dl = key % 12;
    if (cl < dl) cl += 12;
    uint8_t bl = (uint8_t)(cl - dl);
    uint32_t eax;
    if (!(pat & 0xfff000)) {
        pat &= 0xfff;
        eax = shr(dbl(pat << 12 | pat), bl) & 0xfff;
        if ((!(eax & 7) && (eax & 0xc00)) || (!(eax & 3) && (eax & 0x800))) {
            eax = ((eax << 12) | eax) & 0x3ffc00;
            *flag = 1;
        }
        return eax;
    }
    dl = (uint8_t)(dl - cl + 12);
    uint32_t esi = pat & 0xffffff;
    if (bl > dl) { eax = shr(esi, 24 - dl); esi = esi << (dl & 31); }
    else { eax = esi << ((24 - bl) & 31); esi = shr(esi, bl); }
    eax = (eax | esi) & 0xffffff;
    if (!(eax & 7)) {
        uint32_t edx = eax & 0x7000;
        if ((!edx && (eax & 0xc00000)) || (!(eax & 0x3000) && (eax & 0x800000)) || (edx && (eax & 0xc00000)))
            eax = ((eax >> 12) | (eax << 12)) & 0xffffff;
        if (!(eax & 7) && (eax & 0xc00)) goto fold;
    }
    if (!(eax & 3) && (eax & 0x800)) goto fold;
    if (eax & 1) return eax;
    if (!(eax & 0x1000)) return eax;
fold:
    {
        uint32_t ecx = ((eax << 24) | eax) & 0xfffffc00;
        eax = ((eax & 0x300) << 12) | ecx;
        if (!(eax & 0xfff)) return eax >> 12;
        *flag = 1;
        return eax;
    }
}

/* 77307fef: the accidental nibble, signed (9..15 are -7..-1) */
static int32_t accidental(uint32_t mv) { int32_t a = mv & 0xf; return a > 8 ? a - 16 : a; }

/* 77307d23 (eax scale, cx music value): the MIDI note a music value makes against a chord from base (byte) */
static uint8_t mv_note(uint32_t scale, uint16_t mv, uint8_t p1, int8_t key, uint32_t chord, uint8_t base)
{
    uint8_t acc = mv & 0xf;
    uint8_t oct = (uint8_t)(mv >> 12);
    uint8_t al = (uint8_t)(oct * 12 + base);
    uint8_t bl = (uint8_t)(mv >> 4) & 0xf;
    uint8_t dl = (uint8_t)(mv >> 8) & 0xf;
    uint8_t note = al;
    if ((int8_t)acc > 8) acc = (uint8_t)(acc + 0xf0);
    int32_t c = (int32_t)chord;
    if (c != 0) {
        for (;;) {
            if (c & 1) {
                if (dl == 0) goto scale;
                dl--;
            }
            c >>= 1;
            if (c == 0) break;
            note++;
        }
        if (p1 == 0) return 0;
        bl = (uint8_t)((int8_t)p1 >> 4);
        acc = p1 & 0xf;
        note = al;
    }
scale:
    if (bl != 0) {
        int32_t d = ((int32_t)note - (int32_t)key) % 12;
        uint32_t s = scale & 0xfff;
        int32_t e = sar((int32_t)(s << 12 | s), (uint32_t)d);
        while (e != 0) {
            if (e & 1) {
                if (bl == 0) break;
                bl--;
            }
            e >>= 1;
            note++;
        }
    }
    return (uint8_t)(note + acc);
}

/* 77307ddb (edi scale): the music value of a MIDI note against a chord pattern from root */
static uint32_t note_mv(uint32_t scale, uint8_t midi, int8_t key, uint32_t chord, uint8_t root)
{
    uint8_t bl = 0, oct = 0, pos = 0, sc = 0xff, last, cl, al = root;
    if (midi < root) return 0;
    int32_t lim = (int32_t)midi - 24;
    while ((int32_t)al < lim) { al += 12; oct++; }
    last = al;
    cl = al;
    if (al > midi) goto above;
    for (;;) {
        uint32_t c = chord;
        pos = 0xff;
        last = cl;
        al = cl;
        if (cl == midi) {
            al = 0;
            if (!(chord & 1))
                while (c != 0) {
                    c = (uint32_t)((int32_t)c >> 1);
                    al--;
                    if (c & 1) break;
                }
            return ((uint32_t)oct << 12) + (al & 0xf);
        }
        if (chord != 0) {
            for (;;) {
                if (c & 1) {
                    if (al == midi) return ((uint32_t)(uint8_t)(pos + 1) | (uint32_t)oct << 4) << 8;
                    if (al > midi) {
                        if (c != 0) goto next_octave;
                        break;
                    }
                    pos++;
                    last = al;
                }
                al++;
                c = (uint32_t)((int32_t)c >> 1);
                if (c == 0) break;
            }
        }
        al = (uint8_t)(cl + 12);
    next_octave:
        oct++;
        if (al > midi) break;
        cl = (uint8_t)(cl + 12);
        if (cl > midi) break;
    }
above:
    {
        int32_t d = ((int32_t)last - (int32_t)key) % 12;
        oct--;
        uint32_t s = scale & 0xfff;
        int32_t e = sar((int32_t)(s << 12 | s), (uint32_t)d);
        if (e != 0) {
            for (;;) {
                if (e & 1) { sc++; bl = 0; }
                else bl++;
                if (last == midi) break;
                last++;
                e >>= 1;
                if (e == 0) break;
            }
            if (bl != 0 && sc != 0) {
                uint32_t esi = ((uint32_t)oct << 4) + pos;
                uint32_t v1 = (((esi << 4) + sc) << 4) + 1 + bl;
                uint8_t r1 = mv_note(scale, (uint16_t)v1, 0, key, chord, root);
                uint8_t r2 = mv_note(scale, (uint16_t)((esi + 1) << 8), 0, key, chord, root);
                if (r1 == r2) { pos++; sc = 0; bl = 0xff; }
            }
        }
        uint32_t v = (((((uint32_t)oct << 4) + pos) << 4) + sc) << 4;
        v += bl & 0xf;
        uint8_t r = mv_note(scale, (uint16_t)v, 0, key, chord, root);
        if ((int32_t)(int16_t)(uint16_t)r != (int32_t)midi) bl = (uint8_t)(bl + (uint8_t)(midi - r));
        return (((((uint32_t)oct << 4) + pos) << 4) + sc) * 16 + (bl & 0xf);
    }
}

/* 77308011 */
static uint32_t bits(uint32_t x) { uint32_t n = 0; for (; x; x >>= 1) n += x & 1; return n; }

/* 77308032: a music value's note against a subchord, relative to base (the octave and the fixed modes outside) */
static uint32_t mv_midi(uint32_t mv, uint8_t pm, const dm_subchord *s, uint8_t base)
{
    uint8_t chordint = pm & 8;
    if (!chordint && !(pm & 4)) return (uint32_t)base + mv;
    uint32_t edi = dbl(fix_scale(scale_up(s->scaleroot, s->scale ? s->scale : 0xab5ab5)));
    uint32_t ebx = s->chord ? s->chord : 1;
    uint8_t flag = 0;
    if ((pm & 1) && pm != 5) ebx = key_chord(ebx, base, s->chordroot, &flag);
    uint32_t esi = (mv >> 8) & 0xf;         /* chord position */
    uint32_t pos = esi;
    uint32_t scoff = ((mv & 0xff) >> 4) & 7;
    uint32_t ntones = bits(ebx);
    uint32_t cnt = 0;                       /* -0x8 */
    uint8_t dl = base % 12;
    uint32_t eax;
    if (chordint) {
        if ((uint16_t)scoff == 0 && (int16_t)esi < (int16_t)ntones) {
            esi = (uint16_t)accidental(mv) + base;
            eax = pos;
            goto count;
        }
        if ((int16_t)esi < (int16_t)ntones) {
            if (ebx) while (!(ebx & 1)) { ebx >>= 1; cnt++; }
            while ((int16_t)esi > 0) {
                ebx >>= 1;
                cnt++;
                if (ebx & 1) esi--;
                if (ebx == 0) { cnt += esi; break; }
            }
            ebx = (ebx & 0xffff0000) | dl;
            cnt += ebx;
            esi = (uint16_t)accidental(mv) + (uint32_t)base - ebx;
            int32_t r = (int32_t)(int16_t)cnt % 12;
            eax = scoff;
            edi = shr(edi, (uint32_t)r);
            ebx = edi;
            goto count;
        }
    }
    if (!(pm & 4)) return 0x1000;
    ebx = (ebx & 0xffff0000) | dl;
    flag = 0;
    cnt = ebx;
    esi = (uint16_t)accidental(mv) + (uint32_t)base - ebx;
    eax = scoff + pos * 2;
    edi = shr(edi, dl);
    ebx = edi;
count:
    eax++;
    while ((int16_t)eax > 0) {
        cnt++;
        if (ebx & 1) eax--;
        if (ebx == 0) { cnt += eax; break; }
        ebx >>= 1;
    }
    eax = cnt - 1;
    if (flag) eax -= 12;
    return eax + esi;
}

/* 773081f2 */
static uint32_t mv_midi_root(uint32_t mv, uint8_t pm, const dm_subchord *s, uint8_t key)
{
    uint8_t base = 0;
    if (pm & 2) base = s->chordroot;
    else if (pm & 1) base = key;
    return mv_midi(mv, pm, s, base);
}

/* 7730822e */
int dmt_music_value(uint32_t mv, uint8_t pm, const dm_subchord *s, uint8_t key)
{
    uint32_t ebx = mv;
    int32_t edi = 0;
    while ((uint16_t)ebx >= 0xe000) { ebx += 0x1000; edi -= 12; }
    uint32_t eax = ((ebx & 0xff) & 0xf0) + 0x70;
    if (eax & 0xf00) {
        eax = ((eax ^ ebx) & 0xff) & 0xf0;
        edi = -12;
        ebx ^= eax;
    }
    uint32_t r = mv_midi_root(ebx, pm, s, key);
    if ((uint16_t)r == 0x1000) return 0x1000;
    ebx = (ebx & 0xffff0000) | (uint16_t)((uint16_t)ebx >> 12);
    eax = r + (uint32_t)edi;
    if (pm & 2) eax = eax - 12 + ebx * 12;
    else eax = eax + ebx * 12;
    return (int)(int16_t)(uint16_t)eax;
}

const dm_subchord *dmt_subchord(const dm_chord_key *c, uint8_t level)
{
    int n = c->nsub < 8 ? c->nsub : 8;     /* (the count may say more than the eight there are) */
    for (int i = 0; i < n; i++) if (c->sub[i].levels & (1u << (level & 31))) return &c->sub[i];
    return &c->sub[0];
}

static int32_t octave_fix(int32_t v, uint8_t *out, int32_t hr)
{
    if (v < 0) { hr = DMT_S_UP_OCTAVE; v = v + 12 + (int32_t)((uint32_t)(-v - 1) / 12) * 12; }
    if (v > 0x7f) { hr = DMT_S_DOWN_OCTAVE; v = v - (int32_t)((uint32_t)(v - 0x80) / 12) * 12 - 12; }
    *out = (uint8_t)v;
    return hr;
}

/* 77307a89(77307b52): the scale a note is tested against for DMUS_NOTEF_NOINVALIDATE_INSCALE */
uint32_t dmt_note_scale(uint8_t root, uint32_t scale) { return fix_scale(scale_down(root, scale)); }
/* 77307a21: whether a MIDI note is in a 12-note pattern from root */
int dmt_in_pattern(uint8_t midi, uint8_t root, uint32_t pattern)
{
    int32_t k = ((int32_t)midi - root % 12 + 12) % 12;
    return ((1u << k) & ((pattern >> 12) | pattern) & 0xfff) != 0;
}

/* 773085af */
int32_t dmt_music_to_midi(uint16_t mv, const dm_chord_key *c, uint8_t pm, uint8_t level, uint8_t *midi)
{
    int32_t v = mv;
    if (pm != 0) {
        const dm_subchord *s = dmt_subchord(c, level);
        if (pm & 0x10) { *midi = 0; return (int32_t)0x80070057; }
        if (pm == 2) v = v + s->chordroot % 24;
        else if (pm == 1) v = v + c->key;
        else v = (int16_t)dmt_music_value((uint32_t)v, pm, s, c->key);
    }
    if (v == 0x1000) return DMT_S_OVER_CHORD;
    return octave_fix(v, midi, 0);
}

/* 773082a6 */
int32_t dmt_midi_to_music(uint8_t midi, const dm_chord_key *c, uint8_t pm, uint8_t level, uint16_t *mv)
{
    int32_t hr = 0;
    if ((pm & 0x10) || (midi & 0x80)) return (int32_t)0x80070057;
    if (pm == 0) { *mv = midi & 0x7f; return 0; }
    if (pm == 1 || pm == 2) {
        int32_t v = midi;
        if (pm == 1) v -= c->key;
        else v -= dmt_subchord(c, level)->chordroot % 24;
        if (v < 0) { hr = DMT_S_UP_OCTAVE; v = v + 12 + (int32_t)((uint32_t)(-1 - v) / 12) * 12; }
        if (v > 0x7f) { hr = DMT_S_DOWN_OCTAVE; v = v - (int32_t)((uint32_t)(v - 0x80) / 12) * 12 - 12; }
        *mv = (uint16_t)v;
        return hr;
    }
    dm_subchord sub = *dmt_subchord(c, level);
    uint32_t chordpat = sub.chord, chord = sub.chord;
    uint8_t root = sub.chordroot, key = c->key, bl = sub.chordroot, flag = 0;
    uint32_t scale = fix_scale(scale_up(sub.scaleroot, sub.scale));
    pm &= 0xf;
    if (pm & 1) {
        bl = key;
        if (key > midi) {
            uint8_t q = (uint8_t)((uint8_t)(key - midi - 1) / 12);
            hr = DMT_S_UP_OCTAVE;
            midi = (uint8_t)(midi + (uint8_t)(q * 12) + 12);
        }
        scale = scale_down(key, scale);
        if (pm == 5 || chord == 0) {
            root = bl;
            chord = scale_chord(0, scale);
        } else {
            uint32_t r = key_chord(chord, key, root, &flag);
            uint8_t n = 0;
            if (r != 0 && !(r & 1)) do { n++; } while (!(r & (1u << n)));
            bl = (uint8_t)(n + key);
            r = shr(r, n);
            root = bl;
            chord = r;
        }
    } else {
        key = 0;
        if (pm == 6 || chord == 0) chord = scale_chord(bl, scale);
    }
    int low = 0;
    if (midi < 24) { midi += 24; low = 1; }
    uint32_t v = note_mv(scale, midi, (int8_t)key, chord, root);
    if (low) { v += 0xe000; midi -= 24; }
    if ((pm & 2) || flag) v += 0x1000;
    int r = dmt_music_value(v, pm, &sub, c->key);
    if ((int32_t)(int16_t)r == (int32_t)midi) { *mv = (uint16_t)v; return hr; }
    if ((uint16_t)r == 0x1000 && popcount(chordpat) < 4) {
        sub.chord = scale_chord(bl, scale);
        r = dmt_music_value(v, pm, &sub, c->key);
        sub.chord = chordpat;
        if ((int32_t)(int16_t)r == (int32_t)midi) { *mv = (uint16_t)v; return hr; }
    }
    *mv = (uint16_t)v;
    return DMT_E_CANNOT_CONVERT;
}
