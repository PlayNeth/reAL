/* wocomp - compressed formats through waveOut (reAL's winmm.dll decodes them; Windows' mapper does otherwise).
 * A 441 Hz tone, rising, as mu-law and as IMA ADPCM, two seconds each in eight buffers; what the calls return, when the
 * buffers come back and where the position ends, in each of its units.
 *   wocomp.exe [split | adpcm | pcm]   split: the ADPCM buffers cut at odd places, not at the codec's blocks;
 *                              adpcm: the ADPCM alone;
 *                              pcm: the tone as it is, for comparison */
#include <windows.h>
#include <mmreg.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static unsigned char mulaw(short s)
{
    int sign = s < 0 ? 0x80 : 0, v = s < 0 ? -s : s, seg = 0;
    if (v > 32635) v = 32635;
    v += 0x84;
    for (int m = 0x4000; seg < 8 && !(v & m); m >>= 1) seg++;
    seg = 7 - seg;
    return (unsigned char)~(sign | (seg << 4) | ((v >> (seg + 3)) & 0x0f));
}

static const int ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int ima_step[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
    1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
/* one mono block: 4 bytes of header (the first sample, the step index), then two samples a byte */
static void ima_block(unsigned char *out, const short *in, int samples, int *index)
{
    int pred = in[0], idx = *index;
    out[0] = (unsigned char)pred; out[1] = (unsigned char)(pred >> 8); out[2] = (unsigned char)idx; out[3] = 0;
    for (int i = 1; i < samples; i++) {
        int step = ima_step[idx], diff = in[i] - pred, code = 0, d = step >> 3;
        if (diff < 0) { code = 8; diff = -diff; }
        if (diff >= step) { code |= 4; diff -= step; d += step; }
        if (diff >= step >> 1) { code |= 2; diff -= step >> 1; d += step >> 1; }
        if (diff >= step >> 2) { code |= 1; d += step >> 2; }
        pred += code & 8 ? -d : d;
        if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
        idx += ima_index[code];
        if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
        const int at = 4 + (i - 1) / 2;
        if ((i - 1) & 1) out[at] |= (unsigned char)(code << 4); else out[at] = (unsigned char)code;
    }
    *index = idx;
}

static void play(const char *name, WAVEFORMATEX *f, unsigned char *data, DWORD bytes, int split)
{
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    HWAVEOUT h = NULL;
    MMRESULT r = waveOutOpen(NULL, WAVE_MAPPER, f, 0, 0, WAVE_FORMAT_QUERY);
    printf("%s: query %u\n", name, r);
    r = waveOutOpen(&h, WAVE_MAPPER, f, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    CHECK(!r && h, "%s: waveOutOpen %u", name, r);
    if (r) return;
    enum { N = 8 };
    WAVEHDR hd[N];
    DWORD at = 0;
    memset(hd, 0, sizeof(hd));
    for (int i = 0; i < N; i++) {
        DWORD n = bytes / N / f->nBlockAlign * f->nBlockAlign;
        if (split) n += (DWORD)(i * 37 % 101) - 50;
        if (i == N - 1 || at + n > bytes) n = bytes - at;
        hd[i].lpData = (char *)data + at; hd[i].dwBufferLength = n; at += n;
        r = waveOutPrepareHeader(h, &hd[i], sizeof(WAVEHDR));
        MMRESULT r2 = waveOutWrite(h, &hd[i], sizeof(WAVEHDR));
        if (r || r2) { CHECK(0, "%s: buffer %d prepare %u write %u", name, i, r, r2); }
    }
    DWORD t0 = GetTickCount(), mid_bytes = 0;
    int back = 0, order = 1, monotonic = 1;
    DWORD last = 0;
    while (back < N && GetTickCount() - t0 < 6000) {
        WaitForSingleObject(ev, 50);
        MMTIME t; t.wType = TIME_BYTES;
        waveOutGetPosition(h, &t, sizeof(t));
        if (t.u.cb < last) monotonic = 0;
        last = t.u.cb;
        if (!mid_bytes && GetTickCount() - t0 >= 1000) mid_bytes = t.u.cb;
        while (back < N && (hd[back].dwFlags & WHDR_DONE)) back++;
        for (int i = back + 1; i < N; i++) if (hd[i].dwFlags & WHDR_DONE) order = 0;
    }
    const DWORD took = GetTickCount() - t0;
    CHECK(back == N && order, "%s: %d of %d buffers back, in order %d", name, back, N, order);
    /* (up to half a second more for the first stream of all, when the output device is slow to start) */
    CHECK(took > 1800 && took < 2700, "%s: they took %lu ms (2000 of sound)", name, (unsigned long)took);
    CHECK(monotonic, "%s: the position never went back", name);
    CHECK(mid_bytes > f->nAvgBytesPerSec * 6 / 10 && mid_bytes < f->nAvgBytesPerSec * 13 / 10, "%s: after a second the position is about a second's bytes", name);
    Sleep(300);
    MMTIME t;
    t.wType = TIME_BYTES; waveOutGetPosition(h, &t, sizeof(t)); printf("%s: at the end: type %u, %lu bytes of %lu\n", name, t.wType, (unsigned long)t.u.cb, (unsigned long)bytes);
    t.wType = TIME_SAMPLES; waveOutGetPosition(h, &t, sizeof(t)); printf("%s: at the end: type %u, %lu samples\n", name, t.wType, (unsigned long)t.u.sample);
    t.wType = TIME_MS; waveOutGetPosition(h, &t, sizeof(t)); printf("%s: at the end: type %u, %lu ms\n", name, t.wType, (unsigned long)t.u.ms);
    r = waveOutReset(h);
    for (int i = 0; i < N; i++) waveOutUnprepareHeader(h, &hd[i], sizeof(WAVEHDR));
    MMRESULT rc = waveOutClose(h);
    CHECK(!r && !rc, "%s: reset %u close %u", name, r, rc);
    CloseHandle(ev);
}

/* a reset half a second in: every buffer comes back, the position is 0 again, and what is written next plays
 * (from a block's start: the codec starts afresh too); then a loop of two buffers, three times */
static void reset_and_loop(WAVEFORMATEX *f, unsigned char *data, DWORD bytes)
{
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    HWAVEOUT h = NULL;
    if (waveOutOpen(&h, WAVE_MAPPER, f, (DWORD_PTR)ev, 0, CALLBACK_EVENT)) { CHECK(0, "reset: waveOutOpen"); return; }
    enum { N = 8 };
    WAVEHDR hd[N];
    memset(hd, 0, sizeof(hd));
    const DWORD n = bytes / N / f->nBlockAlign * f->nBlockAlign;
    for (int i = 0; i < N; i++) {
        hd[i].lpData = (char *)data + i * n; hd[i].dwBufferLength = n;
        waveOutPrepareHeader(h, &hd[i], sizeof(WAVEHDR));
        waveOutWrite(h, &hd[i], sizeof(WAVEHDR));
    }
    Sleep(500);
    MMRESULT r = waveOutReset(h);
    int back = 0;
    for (int i = 0; i < N; i++) back += (hd[i].dwFlags & (WHDR_DONE | WHDR_INQUEUE)) == WHDR_DONE;
    MMTIME t; t.wType = TIME_BYTES; waveOutGetPosition(h, &t, sizeof(t));
    CHECK(!r && back == N && t.u.cb == 0, "reset: %u, %d of %d buffers back, position %lu", r, back, N, (unsigned long)t.u.cb);
    hd[0].dwFlags &= ~WHDR_DONE; hd[1].dwFlags &= ~WHDR_DONE;
    hd[0].dwFlags |= WHDR_BEGINLOOP; hd[0].dwLoops = 3;
    hd[1].dwFlags |= WHDR_ENDLOOP;
    DWORD t0 = GetTickCount();
    r = waveOutWrite(h, &hd[0], sizeof(WAVEHDR));
    MMRESULT r2 = waveOutWrite(h, &hd[1], sizeof(WAVEHDR));
    while (!(hd[1].dwFlags & WHDR_DONE) && GetTickCount() - t0 < 4000) WaitForSingleObject(ev, 50);
    const DWORD took = GetTickCount() - t0, want = (DWORD)(2 * n * 3 * 1000.0 / f->nAvgBytesPerSec);
    t.wType = TIME_BYTES; waveOutGetPosition(h, &t, sizeof(t));
    CHECK(!r && !r2 && (hd[0].dwFlags & WHDR_DONE) && (hd[1].dwFlags & WHDR_DONE) && took + 150 > want && took < want + 400,
          "loop: two buffers three times took %lu ms (%lu of sound)", (unsigned long)took, (unsigned long)want);
    printf("loop: position %lu bytes (the two buffers are %lu)\n", (unsigned long)t.u.cb, (unsigned long)(2 * n));
    waveOutReset(h);
    for (int i = 0; i < N; i++) waveOutUnprepareHeader(h, &hd[i], sizeof(WAVEHDR));
    CHECK(!waveOutClose(h), "reset: close");
    CloseHandle(ev);
}

int main(int argc, char **argv)
{
    const int split = argc > 1 && !strcmp(argv[1], "split");
    enum { RATE = 22050, SECS = 2 };
    static short pcm[RATE * SECS + 2048];
    /* (rising from 4000 to 12000, so that a recording shows which part of it a stretch is) */
    for (int i = 0; i < RATE * SECS; i++) pcm[i] = (short)((4000 + 8000.0 * i / (RATE * SECS)) * sin(2 * M_PI * 441 * i / RATE));

    if (argc > 1 && !strcmp(argv[1], "pcm")) {
        WAVEFORMATEX fp = { WAVE_FORMAT_PCM, 1, RATE, RATE * 2, 2, 16, 0 };
        play("PCM", &fp, (unsigned char *)pcm, RATE * SECS * 2, 0);
        printf("== %d failure(s)\n", fails);
        return fails != 0;
    }
    static unsigned char mu[RATE * SECS];
    for (int i = 0; i < RATE * SECS; i++) mu[i] = mulaw(pcm[i]);
    WAVEFORMATEX fm = { WAVE_FORMAT_MULAW, 1, RATE, RATE, 1, 8, 0 };
    if (!split && !(argc > 1 && !strcmp(argv[1], "adpcm"))) play("mu-law", &fm, mu, sizeof(mu), 0);

    struct { WAVEFORMATEX f; WORD spb; } fa = { { WAVE_FORMAT_IMA_ADPCM, 1, RATE, 0, 512, 4, 2 }, 1017 };
    fa.f.nAvgBytesPerSec = RATE * 512 / 1017;
    const int blocks = RATE * SECS / 1017;
    static unsigned char ad[(RATE * SECS / 1017 + 1) * 512];
    int index = 0;
    for (int b = 0; b < blocks; b++) ima_block(ad + b * 512, pcm + b * 1017, 1017, &index);
    play(split ? "IMA ADPCM, cut anywhere" : "IMA ADPCM", &fa.f, ad, (DWORD)blocks * 512, split);
    if (argc < 2) reset_and_loop(&fa.f, ad, (DWORD)blocks * 512);

    printf("== %d failure(s)\n", fails);
    return fails != 0;
}
