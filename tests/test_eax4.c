/* Compare reAL's EAX 4 port with ALchemy (records from re/gen_eax4_gt.py, see there for the layout).
 * usage: test_eax4 eax4.bin [-v] */
#include "../src/eax.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

static int verbose;
static int show(const char *what, int rec, const uint8_t *got, const uint8_t *want, size_t n)
{
    int bad = 0;
    for (size_t i = 0; i + 4 <= n; i += 4)
        if (memcmp(got + i, want + i, 4)) {
            if (verbose && bad < 6) {
                uint32_t a, b; float fa, fb;
                memcpy(&a, got + i, 4); memcpy(&b, want + i, 4); memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
                printf("  rec %d %s +0x%02zx: got %08x (%g) want %08x (%g)\n", rec, what, i, a, fa, b, fb);
            }
            bad++;
        }
    return bad;
}


/* effect properties other than reverb (records from re/gen_eax4fx_gt.py) */
static int test_fx(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    uint32_t fx[16] = { 0 }; int n = 0, bad = 0, type = 0;
    struct { uint32_t type, flags, id, len; uint8_t payload[64]; uint32_t ok; uint8_t all[64]; } r;
    while (rd(f, &r, sizeof(r))) {
        if (r.type == 0xff) { memset(fx, 0, sizeof(fx)); if (r.ok) memcpy(fx, r.payload, 64); continue; }
        type = (int)r.type;
        uint8_t buf[64]; memcpy(buf, r.payload, 64);
        uint32_t ret = r.len;
        int res = (r.flags & 1) ? eax4fx_effect_get(type, fx, r.id, buf, r.len, &ret) : eax4fx_effect_set(type, fx, r.id, buf, r.len);
        int b = (res == EAX_OK) != (r.ok != 0);
        b |= memcmp(fx, r.all, 4u * eax4fx_effect_props(type)) != 0;
        n++;
        if (b && verbose) for (int i = 0; i < eax4fx_effect_props(type); i++) if (fx[i] != ((uint32_t *)r.all)[i]) {
            float a, c; memcpy(&a, &fx[i], 4); memcpy(&c, &((uint32_t *)r.all)[i], 4);
            printf("    field %d: got %08x (%g) want %08x (%g)\n", i, fx[i], a, ((uint32_t *)r.all)[i], c); }
        if (b) { bad++; if (verbose) printf("  fx type %d %s id %#x len %u: result %d ok %u\n", type, (r.flags & 1) ? "get" : "set", r.id, r.len, res, r.ok);
                 memcpy(fx, r.all, 64); }
    }
    fclose(f);
    printf("EAX4 effect property ops %d/%d identical\n", n - bad, n);
    return bad;
}

/* FX slot rules (ported from the code, ALchemy's slot objects are not emulated) */
static int test_slots(void)
{
    eax4_context c; eax4_slot s[4]; int bad = 0;
    memset(s, 0, sizeof(s));
    eax4_context_reset(&c); eax4_slots_init(s);
#define T(cond) do { if (!(cond)) { printf("  slot check failed: %s (line %d)\n", #cond, __LINE__); bad++; } } while (0)
    T(s[0].type == EAX4FX_REVERB && s[1].type == EAX4FX_CHORUS && s[2].type == EAX4FX_NONE && s[0].lock == 1);
    /* slots 0 and 1 are locked by the system */
    T(eax4fx_set(&s[0], &c, EAX4FXS_LOADEFFECT, &EAX4_EFFECT_GUID[EAX4FX_ECHO], 16) != EAX_OK && c.last_error == EAX4ERR_UNKNOWN_EFFECT);
    int32_t v = 0;
    T(eax4fx_set(&s[1], &c, EAX4FXS_LOCK, &v, 4) != EAX_OK && c.last_error == EAX4ERR_INVALID_OPERATION);
    struct { eax_guid g; int32_t volume, lock; uint32_t flags; } all = { EAX4_EFFECT_GUID[EAX4FX_REVERB], -500, 1, 1 };
    T(eax4fx_set(&s[0], &c, EAX4FXS_ALL, &all, sizeof(all)) != EAX_OK && c.last_error == EAX4ERR_INVALID_VALUE);
    /* but their volume, flags and effect properties can change */
    v = -1200; T(eax4fx_set(&s[0], &c, EAX4FXS_VOLUME, &v, 4) == EAX_OK && s[0].volume == -1200);
    v = 1; T(eax4fx_set(&s[0], &c, EAX4FXS_VOLUME, &v, 4) != EAX_OK);
    uint32_t fl = 2; T(eax4fx_set(&s[0], &c, EAX4FXS_FLAGS, &fl, 4) != EAX_OK);
    int32_t room = -2000; T(eax4fx_set(&s[0], &c, 5, &room, 4) == EAX_OK && s[0].reverb.p.room == -2000);
    /* an empty slot has no effect properties */
    T(eax4fx_set(&s[3], &c, 5, &room, 4) != EAX_OK && c.last_error == EAX4ERR_NO_EFFECT);
    /* loading through ALLPARAMETERS starts the reverb from its defaults (ALchemy: zeros) */
    T(eax4fx_set(&s[2], &c, EAX4FXS_ALL, &all, sizeof(all)) == EAX_OK && s[2].type == EAX4FX_REVERB);
    T(s[2].reverb.p.hf_reference == 5000.0f && s[2].reverb.p.room == -1000 && s[2].volume == -500 && s[2].lock == 1);
    uint8_t rv[112]; uint32_t got = 0;
    T(eax4fx_get(&s[2], &c, 1, rv, sizeof(rv), &got) == EAX_OK && got == 112);
    T(eax4fx_set(&s[2], &c, 1, rv, sizeof(rv)) == EAX_OK);          /* F.E.A.R.'s read-modify-write */
    /* the same effect again keeps its properties; LOADEFFECT always resets them */
    room = -3000; eax4fx_set(&s[2], &c, 5, &room, 4);
    T(eax4fx_set(&s[2], &c, EAX4FXS_ALL, &all, sizeof(all)) == EAX_OK && s[2].reverb.p.room == -3000);
    T(eax4fx_set(&s[2], &c, EAX4FXS_LOADEFFECT, &EAX4_EFFECT_GUID[EAX4FX_REVERB], 16) == EAX_OK && s[2].reverb.p.room == -1000);
    /* another effect type: defaults and validation */
    T(eax4fx_set(&s[3], &c, EAX4FXS_LOADEFFECT, &EAX4_EFFECT_GUID[EAX4FX_CHORUS], 16) == EAX_OK && s[3].type == EAX4FX_CHORUS);
    T(eax4fx_value(EAX4FX_CHORUS, s[3].fx, 1) == 90.0f);
    float rate = 11.0f; T(eax4fx_set(&s[3], &c, 4, &rate, 4) != EAX_OK);
    eax_guid junk = { { 1, 2, 3, 4 } };
    T(eax4fx_set(&s[3], &c, EAX4FXS_LOADEFFECT, &junk, 16) != EAX_OK && c.last_error == EAX4ERR_UNKNOWN_EFFECT);
    int32_t err = 0; got = 0;
    T(eax4c_get(&c, EAX4C_LASTERROR, &err, 4, &got) == EAX_OK && err == EAX4ERR_UNKNOWN_EFFECT && c.last_error == 0);
#undef T
    printf("EAX4 slot rules: %s\n", bad ? "FAILED" : "ok");
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_eax4 eax4.bin [eax4fx.bin] [-v]\n"); return 2; }
    verbose = !strcmp(argv[argc - 1], "-v");
    int fxbad = test_slots();
    if (argc > 2 && strcmp(argv[2], "-v")) fxbad += test_fx(argv[2]);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    eax4_context ctx; eax4_source src;
    eax4_context_reset(&ctx); eax4_source_defaults(&src);
    int n[4] = { 0 }, bad[4] = { 0 }, rec = 0;
    uint32_t kind;
    while (rd(f, &kind, 4)) {
        rec++;
        if (kind == 0) { eax4_context_reset(&ctx); eax4_source_defaults(&src); continue; }
        if (kind == 1 || kind == 2) {
            uint32_t hdr[3], ok, retlen; uint8_t payload[256], o[256], wctx[0x24], wsrc[0xfc];
            if (!rd(f, hdr, 12) || !rd(f, payload, 256) || !rd(f, &ok, 4) || !rd(f, &retlen, 4) || !rd(f, o, 256) ||
                !rd(f, wctx, 0x24) || !rd(f, wsrc, 0xfc)) return 3;
            uint32_t flags = hdr[0], id = hdr[1], len = hdr[2];
            uint8_t buf[256]; memcpy(buf, payload, 256);
            uint32_t ret = len; int r;
            if (kind == 1) r = (flags & 1) ? eax4c_get(&ctx, id, buf, len, &ret) : eax4c_set(&ctx, id, buf, len);
            else r = (flags & 1) ? eax4s_get(&src, &ctx, id, buf, len, &ret) : eax4s_set(&src, &ctx, id, buf, len);
            int b = 0;
            if ((r == EAX_OK) != (ok != 0)) {
                if (verbose) printf("  rec %d kind %u %s id %#x len %u: result %d, ALchemy %s\n", rec, kind,
                                    (flags & 1) ? "get" : "set", id, len, r, ok ? "ok" : "fail");
                b++;
            }
            if (ok && (flags & 1)) {
                if (ret != retlen) { if (verbose) printf("  rec %d: ret len %u want %u\n", rec, ret, retlen); b++; }
                b += show("out", rec, buf, o, retlen < 256 ? retlen : 256);
            }
            b += show("ctx", rec, (const uint8_t *)&ctx, wctx, 0x24);
            b += show("src", rec, (const uint8_t *)&src, wsrc, 0xfc);
            n[kind]++; if (b) { bad[kind]++; if (verbose) printf("  ^ rec %d kind %u flags %u id %#x len %u\n", rec, kind, flags, id, len); }
            if (b) {   /* resync to ALchemy's state so one difference does not cascade */
                memcpy(&ctx, wctx, 0x24); memcpy(&src, wsrc, 0xfc);
            }
            continue;
        }
        if (kind == 3) {
            uint8_t cb[0x24], sb[0xfc];
            struct { uint32_t flags, type; float decay, hfr, air, hfref, rro; uint32_t rvf; } sl[4];
            struct { float x, cf; uint32_t inner, outer; int32_t ov; float df, ro, dop; } gg;
            float want[10];
            if (!rd(f, cb, 0x24) || !rd(f, sb, 0xfc) || !rd(f, sl, sizeof(sl)) || !rd(f, &gg, sizeof(gg)) || !rd(f, want, 40)) return 3;
            eax4_context c; eax4_source s; eax4_slot slots[4];
            memcpy(&c, cb, 0x24); memcpy(&s, sb, 0xfc);
            memset(slots, 0, sizeof(slots));
            for (int i = 0; i < 4; i++) {
                slots[i].flags = sl[i].flags; slots[i].type = (int)sl[i].type;
                eax3l_reset(&slots[i].reverb);
                slots[i].reverb.p.decay_time = sl[i].decay; slots[i].reverb.p.decay_hf_ratio = sl[i].hfr;
                slots[i].reverb.p.air_absorption_hf = sl[i].air; slots[i].reverb.p.hf_reference = sl[i].hfref;
                slots[i].reverb.p.room_rolloff = sl[i].rro; slots[i].reverb.p.flags = sl[i].rvf;
            }
            eax_geom g = { gg.x, gg.cf, gg.inner, gg.outer, gg.ov, gg.df, gg.ro };
            eax4_gains o;
            eax4_source_gains(&s, &c, slots, &g, gg.dop, &o);
            float got[9] = { o.direct, o.direct_hf, o.send[0], o.send[1], o.send[2], o.send[3],
                             o.send_hf[0], o.send_hf[1], o.send_hf[2] };
            int b = show("gains", rec, (const uint8_t *)got, (const uint8_t *)want, 36);
            if (memcmp(&o.send_hf[3], &want[9], 4)) { b++; if (verbose) printf("  rec %d send_hf[3] %g want %g\n", rec, o.send_hf[3], want[9]); }
            n[3]++; if (b) bad[3]++;
            continue;
        }
        fprintf(stderr, "bad record kind %u\n", kind);
        return 3;
    }
    fclose(f);
    printf("EAX4 context ops %d/%d, source ops %d/%d, source conversions %d/%d identical\n",
           n[1] - bad[1], n[1], n[2] - bad[2], n[2], n[3] - bad[3], n[3]);
    return (bad[1] || bad[2] || bad[3] || fxbad) ? 1 : 0;
}
