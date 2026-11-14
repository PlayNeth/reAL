/* The chunk reader over a RIFF file in memory that reDM's parsers share (segments, styles, chord maps and templates,
 * DLS collections, SoundFonts, audiopath configurations).  Each parser keeps a small wrapper of its own with its exact
 * reading on top: whether it wants a list's type, and what it does with a chunk that runs past its parent.
 *
 * The readers over a stream (the loader's, the tool graph's, the tracks', the script's) are not this one: each copies
 * a particular Microsoft DLL's RIFF parser, its quirks included. */
#ifndef REDM_RIFF_H
#define REDM_RIFF_H

#include <stdint.h>
#include <stddef.h>

#define RIFF_FCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

typedef struct { const uint8_t *p, *e; } riff_rd;      /* bytes still to read: [p, e) */
typedef struct {
    uint32_t id;        /* the chunk's id: 'RIFF', 'LIST' or another */
    uint32_t size;      /* its size as written */
    riff_rd data;       /* its data, cut at the parent's end (a list's type is the first 4 bytes of it) */
} riff_chunk;

enum {
    RIFF_CLAMP = 0,     /* a chunk that runs past its parent is cut at the parent's end, and is the last one */
    RIFF_STRICT = 1,    /* such a chunk ends the list: none is returned, and r stays where it was */
};

static inline uint32_t riff_u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* the chunk at r->p, and r moved past it (and its pad byte); 0 at the end, or for a chunk too short to have a header.
 * The sums are made on sizes, not pointers: a pointer plus a size from the file wraps in a 32-bit process. */
static inline int riff_next(riff_rd *r, riff_chunk *c, int mode)
{
    if (r->e - r->p < 8) return 0;
    const uint8_t *d = r->p + 8;
    const size_t left = (size_t)(r->e - d);
    uint32_t sz = riff_u32(r->p + 4);
    c->size = sz;
    if (left < sz) {
        if (mode == RIFF_STRICT) return 0;
        sz = (uint32_t)left;
    }
    c->id = riff_u32(r->p);
    c->data.p = d;
    c->data.e = d + sz;
    r->p = left - sz > (sz & 1) ? d + sz + (sz & 1) : r->e;
    return 1;
}

/* a RIFF form or a LIST, with room for its type */
static inline int riff_is_list(const riff_chunk *c)
{
    return (c->id == RIFF_FCC('R','I','F','F') || c->id == RIFF_FCC('L','I','S','T')) && c->data.e - c->data.p >= 4;
}
/* a form's or list's type (0 for any other chunk), and the chunks after it */
static inline uint32_t riff_list_type(const riff_chunk *c) { return riff_is_list(c) ? riff_u32(c->data.p) : 0; }
static inline riff_rd riff_list_body(const riff_chunk *c)
{
    riff_rd b = { riff_is_list(c) ? c->data.p + 4 : c->data.p, c->data.e };
    return b;
}

#endif
