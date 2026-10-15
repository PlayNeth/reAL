/* test_dsfx <effect> <in.raw> <ref.raw> <rate> <channels> <bits> <chunk frames> [parameters...]: run reAL's port of a
 * dsdmo.dll effect on raw PCM as dmgt's "dmo" command runs Microsoft's (same arguments), print the defaults and
 * parameters the same way, and count the samples that differ from dsdmo's output */
#include "../src/dsfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *s_names[] = { "gargle", "chorus", "flanger", "echo", "distortion", "compressor", "parameq", "waves", "i3dl2" };
static const char *s_types[] = { "uu", "ffffifi", "ffffifi", "ffffi", "fffff", "ffffff", "fff", "ffff", "iifffififfff" };

static void print_params(const char *types, const uint32_t *v)
{
    for (int k = 0; types[k]; k++) {
        if (types[k] == 'f') { float f; memcpy(&f, &v[k], 4); fprintf(stderr, " %.9g", f); }
        else fprintf(stderr, " %ld", (long)(int32_t)v[k]);
    }
    fprintf(stderr, "\n");
}

int main(int argc, char **argv)
{
    if (argc < 8) { fprintf(stderr, "usage: test_dsfx effect in ref rate ch bits chunk [params]\n"); return 2; }
    int e;
    for (e = 0; e < 9; e++) if (!strcmp(argv[1], s_names[e])) break;
    if (e == 9) return 2;
    unsigned rate = atoi(argv[4]), ch = atoi(argv[5]), bits = atoi(argv[6]), chunk = atoi(argv[7]);
    dsfx *f = dsfx_new(e);
    if (!f) { fprintf(stderr, "not implemented\n"); return 2; }
    if (getenv("DSFX_FIXES")) dsfx_set_fixes(f, strtoul(getenv("DSFX_FIXES"), NULL, 0));
    int hr = dsfx_set_format(f, bits == 32 ? 3 : 1, ch, rate, bits);
    fprintf(stderr, "SetInputType %08x\n", hr);
    if (hr < 0) return 1;
    if (getenv("DMO_DISC_FIRST")) dsfx_discontinuity(f);
    uint32_t v[16] = { 0 };
    dsfx_get_all(f, v);
    fprintf(stderr, "defaults"); print_params(s_types[e], v);
    if (e == DSFX_I3DL2) {              /* as dmgt: DMO_I3DL2_QUALITY / DMO_I3DL2_PRESET first */
        int32_t q = 0; uint32_t pr = 0;
        if (getenv("DMO_I3DL2_QUALITY")) fprintf(stderr, "SetQuality %08x\n", dsfx_i3dl2_set_quality(f, atol(getenv("DMO_I3DL2_QUALITY"))));
        if (getenv("DMO_I3DL2_PRESET")) fprintf(stderr, "SetPreset %08x\n", dsfx_i3dl2_set_preset(f, strtoul(getenv("DMO_I3DL2_PRESET"), NULL, 0)));
        dsfx_i3dl2_get_quality(f, &q);
        dsfx_i3dl2_get_preset(f, &pr);
        memset(v, 0, sizeof(v));
        dsfx_get_all(f, v);
        fprintf(stderr, "quality %ld preset %lu params", (long)q, (unsigned long)pr); print_params(s_types[e], v);
    }
    if (argc > 8) {
        int np = (int)strlen(s_types[e]);
        for (int k = 0; k < np && 8 + k < argc; k++) {
            if (s_types[e][k] == 'f') { float x = (float)atof(argv[8 + k]); memcpy(&v[k], &x, 4); }
            else v[k] = (uint32_t)strtol(argv[8 + k], NULL, 0);
        }
        fprintf(stderr, "SetAllParameters %08x\n", dsfx_set_all(f, v));
        memset(v, 0, sizeof(v));
        dsfx_get_all(f, v);
        fprintf(stderr, "parameters"); print_params(s_types[e], v);
    }
    if (!getenv("DMO_DISC_FIRST")) dsfx_discontinuity(f);
    if (getenv("DMO_FRAME") && e == DSFX_I3DL2) {
        unsigned char o[0x200]; FILE *d = fopen(getenv("DMO_FRAME_OUT"), "wb");
        dsfx_i3dl2_frame(f, (uint32_t)strtoul(getenv("DMO_FRAME"), NULL, 16), o);
        if (d) { fwrite(o, 1, sizeof(o), d); fclose(d); }
    }
    if (getenv("DSFX_DUMP") && e == DSFX_I3DL2) {
        unsigned char o[0x4c8]; FILE *d = fopen(getenv("DSFX_DUMP"), "wb");
        dsfx_i3dl2_object(f, o);
        if (d) { fwrite(o, 1, sizeof(o), d); fclose(d); }
    }
    /* DMO_INFO: IMediaParamInfo; DMO_ENV / DMO_SETPARAM / DMO_FLUSH (as dmgt): envelopes, then GetParam */
    int np = dsfx_nparams(e);
    if (getenv("DMO_INFO") && e != DSFX_I3DL2) {
        fprintf(stderr, "count %d\n", np);
        for (int k = 0; k < np; k++) {
            dsfx_pinfo pi;
            dsfx_param_info(e, k, &pi);
            fprintf(stderr, "info %d: type %u caps %x %.9g %.9g %.9g text", k, pi.type, pi.caps, pi.min, pi.max, pi.neutral);
            const uint16_t *ss[3] = { pi.label, pi.unit, pi.enums };
            for (int j = 0; j < 3; j++) {
                fprintf(stderr, " ");
                for (const uint16_t *c = ss[j]; *c; c++) fputc(*c == ',' ? '|' : *c, stderr);
                if (j < 2 || *ss[j]) fprintf(stderr, "|");
            }
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "formats 1 93ad712b current 93ad712b 0\n");
        fprintf(stderr, "SetTimeFormat(music) 80070057 (reference) 00000000\n");
        fprintf(stderr, "bad index 80070057 80070057 80070057\n");
    }
    if (getenv("DMO_ENV")) {
        char *spec = strdup(getenv("DMO_ENV")), *sv = NULL;
        for (char *g = strtok_r(spec, ";", &sv); g; g = strtok_r(NULL, ";", &sv)) {
            dsfx_envseg sg[8]; int idx = 0, m = 0; char *sv2 = NULL;
            idx = atoi(g); g = strchr(g, ':') + 1;
            for (char *q = strtok_r(g, "/", &sv2); q && m < 8; q = strtok_r(NULL, "/", &sv2), m++) {
                long long a0, a1; float v0, v1; unsigned c, fl;
                sscanf(q, "%lld,%lld,%f,%f,%u,%u", &a0, &a1, &v0, &v1, &c, &fl);
                sg[m] = (dsfx_envseg){ a0, a1, v0, v1, c, fl };
            }
            fprintf(stderr, "AddEnvelope %d %d %08x\n", idx, m, dsfx_add_envelope(f, idx, m, sg));
        }
    }
    if (getenv("DMO_SETPARAM")) {
        char *spec = strdup(getenv("DMO_SETPARAM")), *sv = NULL;
        for (char *g = strtok_r(spec, ";", &sv); g; g = strtok_r(NULL, ";", &sv))
            fprintf(stderr, "SetParam %08x\n", dsfx_set_param(f, atoi(g), (float)atof(strchr(g, ',') + 1)));
    }
    if (getenv("DMO_FLUSH")) {
        char *spec = strdup(getenv("DMO_FLUSH")), *sv = NULL;
        for (char *g = strtok_r(spec, ";", &sv); g; g = strtok_r(NULL, ";", &sv)) {
            int idx; long long a0, a1;
            sscanf(g, "%d,%lld,%lld", &idx, &a0, &a1);
            fprintf(stderr, "FlushEnvelope %08x\n", dsfx_flush_envelope(f, idx, a0, a1));
        }
    }
    if (getenv("DMO_ENV") || getenv("DMO_SETPARAM") || getenv("DMO_FLUSH")) {
        fprintf(stderr, "getparam");
        for (int k = 0; k < np; k++) fprintf(stderr, " %.9g", dsfx_get_param(f, k));
        fprintf(stderr, "\n");
    }
    long long t0 = getenv("DMO_T0") ? atoll(getenv("DMO_T0")) : 0;
    FILE *fi = fopen(argv[2], "rb"), *fr = fopen(argv[3], "rb");
    if (!fi || !fr) return 1;
    fseek(fi, 0, SEEK_END); long n = ftell(fi); fseek(fi, 0, SEEK_SET);
    unsigned char *a = malloc(n), *r = malloc(n);
    if (fread(a, 1, n, fi) != (size_t)n || fread(r, 1, n, fr) != (size_t)n) return 1;
    unsigned fb = ch * bits / 8;
    for (long o = 0; o < n; o += (long)chunk * fb) {
        long m = n - o < (long)chunk * fb ? n - o : (long)chunk * fb;
        dsfx_process_at(f, a + o, (uint32_t)(m / fb), getenv("DMO_T0") ? t0 + (long long)(o / fb) * 10000000 / rate : 0);
    }
    if (getenv("DMO_ENV")) {
        memset(v, 0, sizeof(v));
        dsfx_get_all(f, v);
        fprintf(stderr, "after"); print_params(s_types[e], v);
    }
    if (getenv("DSFX_OUT")) { FILE *fo = fopen(getenv("DSFX_OUT"), "wb"); if (fo) { fwrite(a, 1, n, fo); fclose(fo); } }
    long bad = 0, first = -1, cnt = n / (bits / 8); double maxd = 0;
    for (long i = 0; i < cnt; i++) {
        double x, y;
        if (bits == 8) { x = a[i]; y = r[i]; }
        else if (bits == 16) { x = ((int16_t *)a)[i]; y = ((int16_t *)r)[i]; }
        else { float p, q; memcpy(&p, a + 4 * i, 4); memcpy(&q, r + 4 * i, 4); if (!memcmp(&p, &q, 4)) continue; x = p; y = q; }
        if (x != y) { bad++; if (first < 0) first = i; if (fabs(x - y) > maxd) maxd = fabs(x - y); }
    }
    printf("%ld samples, %ld differ (first %ld, max diff %g)\n", cnt, bad, first, maxd);
    dsfx_free(f);
    return bad != 0;
}
