/* reAL-Music: wave files as DirectMusic loads them (dswave.dll CDirectSoundWave, Windows XP 5.3.2600).
 *
 * Load 6c781e31 reads the RIFF WAVE chunks: 'fmt ', 'data', 'fact', 'wavh' (DMUS_IO_WAVE_HEADER: read-ahead and
 * the streaming / no-preroll flags), 'wavu' (DirectMusic Producer's compressed-wave header: the decompressed format
 * and a count of lead-in samples to skip), 'guid', 'vers' and LIST INFO/INAM.  A compressed file plays through ACM as
 * the PCM format acmFormatSuggest gives (or the one 'wavu' names); only 8 and 16-bit PCM is accepted.  dswave decodes
 * while streaming; here the whole file is decoded at load (through the host's decoder), which plays the same. */
#include "dmi.h"
#include <stdlib.h>
#include <string.h>

#define FCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static dm_wave_decoder g_decode;
void dm_wave_set_decoder(dm_wave_decoder fn) { g_decode = fn; }
static int g_fixlen;
void dm_wave_set_fix_length(int on) { g_fixlen = on; }

/* 6c782777: the length in milliseconds (IDirectSoundWave private GetLength): frames of the file's own format (the
 * 'fact' count for a compressed file; without one, the data size over the block size, which for a compressed file
 * counts blocks), less the lead-in, at the file's rate */
static uint32_t wave_ms(const dm_wave *w)
{
    uint64_t n = w->fileframes;
    if (!n || !w->filerate) return 0;
    uint32_t f = (uint32_t)n;
    if (w->skip) f -= w->skip;
    return (uint32_t)((uint64_t)f * 1000 / w->filerate);
}
uint32_t dm_wave_length_ms(const dm_wave *w) { return wave_ms(w); }

dm_wave *dm_wave_load(const void *data, size_t size)
{
    const uint8_t *p = data, *e = p + size;
    if (size < 12 || u32(p) != FCC('R','I','F','F') || u32(p + 8) != FCC('W','A','V','E')) return NULL;
    uint32_t rs = u32(p + 4);
    if (rs < (size_t)(e - p) - 8) e = p + 8 + rs;
    p += 12;
    const uint8_t *fmt = NULL, *dat = NULL, *ufmt = NULL;
    uint32_t fmtlen = 0, datlen = 0, fact = 0;
    dm_wave *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->refs = 1;
    int havewavh = 0;
    while (e - p >= 8) {
        uint32_t id = u32(p), len = u32(p + 4);
        const uint8_t *c = p + 8;
        if (len > (size_t)(e - c)) len = (uint32_t)(e - c);
        if (id == FCC('w','a','v','h')) {
            uint8_t h[16] = { 0 };
            memcpy(h, c, len < 16 ? len : 16);
            havewavh = 1;
            if (u32(h + 8) & 2) { w->streaming = 1; memcpy(&w->readahead, h, 8); }
            else { w->streaming = 0; w->readahead = 0; }
            w->nopreroll = (u32(h + 8) & 8) != 0;
        } else if (id == FCC('f','m','t',' ')) {
            fmt = c; fmtlen = len;
        } else if (id == FCC('d','a','t','a')) {
            dat = c; datlen = len;
        } else if (id == FCC('f','a','c','t') && len >= 4) {
            fact = u32(c);
        } else if (id == FCC('w','a','v','u') && len >= 2) {
            if (c[0] && c[1]) {
                if (len < 2 + 0x12) { free(w); return NULL; }
                ufmt = c + 2;
                if (len > 0x14 && len >= 0x18) w->skip = u32(c + 0x14);
            }
        } else if (id == FCC('g','u','i','d') && len >= 16) {
            w->guid.d1 = u32(c); w->guid.d2 = u16(c + 4); w->guid.d3 = u16(c + 6); memcpy(w->guid.d4, c + 8, 8);
            w->hasguid = 1;
        } else if (id == FCC('v','e','r','s') && len >= 8) {
            w->vers[0] = u32(c); w->vers[1] = u32(c + 4); w->hasvers = 1;
        } else if (id == FCC('L','I','S','T') && len >= 4 && u32(c) == FCC('I','N','F','O')) {
            const uint8_t *q = c + 4, *qe = c + len;
            while (qe - q >= 8) {
                uint32_t sid = u32(q), sl = u32(q + 4);
                if (sl > (size_t)(qe - q - 8)) sl = (uint32_t)(qe - q - 8);
                if (sid == FCC('I','N','A','M')) {
                    size_t k = sl < sizeof(w->name) - 1 ? sl : sizeof(w->name) - 1;
                    memcpy(w->name, q + 8, k);
                    w->name[k] = 0;
                }
                q += 8 + sl + (sl & 1);
            }
        }
        p = c + len + (len & 1);
    }
    if (!fmt || fmtlen < 14 || !dat) { free(w); return NULL; }
    uint16_t tag = u16(fmt);
    w->filerate = u32(fmt + 4);
    uint16_t falign = u16(fmt + 12);
    w->fileframes = tag != 1 && fact ? fact : (falign ? datlen / falign : 0);
    if (tag == 1) {
        if (fmtlen < 16) { free(w); return NULL; }
        w->channels = u16(fmt + 2); w->rate = w->filerate; w->bits = u16(fmt + 14);
        if ((w->bits != 8 && w->bits != 16) || !w->channels) { free(w); return NULL; }
        uint32_t fr = w->channels * (w->bits / 8);
        w->frames = datlen / fr;
        w->data = malloc(w->frames * fr + 1);
        if (!w->data) { free(w); return NULL; }
        memcpy(w->data, dat, w->frames * fr);
    } else {
        /* compressed: the decompressed format is 'wavu''s, else acmFormatSuggest's (the decoder picks it) */
        uint8_t *pcm = NULL;
        uint32_t pcmlen = 0;
        uint16_t ch = 0, bits = 0;
        uint32_t rate = 0;
        if (ufmt) { ch = u16(ufmt + 2); rate = u32(ufmt + 4); bits = u16(ufmt + 14); }
        if (!g_decode || !g_decode(fmt, fmtlen, dat, datlen, &pcm, &pcmlen, &ch, &rate, &bits)) { free(w); return NULL; }
        if ((bits != 8 && bits != 16) || !ch) { free(pcm); free(w); return NULL; }
        w->channels = ch; w->rate = rate; w->bits = bits;
        uint32_t fr = ch * (bits / 8);
        uint32_t frames = pcmlen / fr, skip = w->skip < frames ? w->skip : frames;
        w->frames = frames - skip;
        if (g_fixlen && !fact) { w->fileframes = frames; w->filerate = rate; }
        w->data = malloc(w->frames * fr + 1);
        if (!w->data) { free(pcm); free(w); return NULL; }
        memcpy(w->data, pcm + skip * fr, w->frames * fr);
        free(pcm);
    }
    /* 6c781a4a: without a 'wavh', waves up to 5 seconds load whole and longer ones stream (half a second ahead) */
    if (!havewavh) {
        if (wave_ms(w) < 5001) { w->streaming = 0; w->readahead = 0; }
        else { w->streaming = 1; w->readahead = 5000000; }
    }
    return w;
}

void dm_wave_addref(dm_wave *w) { if (w) __sync_add_and_fetch(&w->refs, 1); }
void dm_wave_release(dm_wave *w)
{
    if (w && __sync_sub_and_fetch(&w->refs, 1) <= 0) { free(w->data); free(w); }
}
