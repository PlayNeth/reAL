/* reDM: the chord, command and mute tracks' parameters and Play (dmstyle), included by dmtrack.c.
 * Their data and loading are in dmstyload.c. */
#include "dmsty.h"
#include "dmcomp.h"

static int wt_rand(dm_perf *pf);
#include "dmstyplay.c"

/* IsParamSupported: chord 77401d9e, command 77403498, mute 774153d1 */
static int sty_isparam(const dm_track *t, int prm)
{
    switch (t->kind) {
    case TRK_CHORD: return prm == PRM_CHORD || prm == PRM_RHYTHM ? DM_OK : DMUS_E_TYPE_UNSUPPORTED;
    case TRK_COMMAND:
        return prm == PRM_COMMAND || prm == PRM_COMMAND2 || prm == PRM_COMMANDNEXT ? DM_OK : DMUS_E_TYPE_UNSUPPORTED;
    case TRK_MUTE: return prm == PRM_MUTE ? DM_OK : DMUS_E_TYPE_UNSUPPORTED;
    case TRK_STYLE: return stytrack_isparam(t, prm);
    case TRK_MOTIF: return motiftrack_isparam(t, prm);
    case TRK_PATTERN: return patterntrack_isparam(t, prm);
    case TRK_CHORDMAP: return prm == PRM_CHORDMAP ? DM_OK : DMUS_E_TYPE_UNSUPPORTED;     /* 7728bd75 */
    case TRK_SIGNPOST: return DM_E_NOTIMPL;                                             /* 7728acae */
    }
    return DMUS_E_TYPE_UNSUPPORTED;
}

/* GetParam: chord 77402387, command 774038da, mute 774155e0 (each with its own order of checks) */
static int sty_getparam(const dm_track *t, int prm, mtime_t mt, mtime_t *next, void *data)
{
    switch (t->kind) {
    case TRK_CHORD: {
        const dms_chordtrack *T = t->data;
        if (!data) return DM_E_POINTER;
        if (!T->chords) return DMUS_E_NOT_FOUND;
        if (prm == PRM_CHORD) return dms_chord_getparam(T, 0, mt, next, data);
        if (prm == PRM_RHYTHM) return dms_chord_getparam(T, 1, mt, next, data);
        return DMUS_E_GET_UNSUPPORTED;
    }
    case TRK_COMMAND: {
        dms_cmdtrack *T = t->data;
        if (prm == PRM_COMMAND2) return data ? dms_cmd_getparam(T, 2, mt, next, data) : DM_E_POINTER;
        if (prm == PRM_COMMANDNEXT) return data ? dms_cmd_getparam(T, 1, mt, next, data) : DM_E_POINTER;
        if (prm != PRM_COMMAND) return DMUS_E_GET_UNSUPPORTED;
        return data ? dms_cmd_getparam(T, 0, mt, next, data) : DM_E_POINTER;
    }
    case TRK_MUTE:
        if (!data) return DM_E_POINTER;
        if (prm != PRM_MUTE) return DMUS_E_TYPE_UNSUPPORTED;
        return dms_mute_getparam(t->data, mt, next, data);
    case TRK_STYLE: return stytrack_getparam(t, NULL, prm, mt, next, data);
    case TRK_MOTIF: return motiftrack_getparam(t, prm, mt, next, data);
    case TRK_PATTERN: return patterntrack_getparam(t, NULL, prm, mt, next, data);
    case TRK_CHORDMAP:                  /* 7728c1d1 */
        if (!data) return DM_E_POINTER;
        if (prm != PRM_CHORDMAP) return DMUS_E_GET_UNSUPPORTED;
        return dmc_cmtrack_getparam(t->data, mt, next, data);
    case TRK_SIGNPOST: return DM_E_NOTIMPL;   /* 7728ac6d */
    }
    return DMUS_E_GET_UNSUPPORTED;
}

/* SetParam: chord 774029a8, command 77403a32, mute 774158b7 */
static int sty_setparam(dm_track *t, int prm, mtime_t mt, void *data)
{
    switch (t->kind) {
    case TRK_CHORD:
        if (!data) return DM_E_POINTER;
        if (prm != PRM_CHORD) return DMUS_E_SET_UNSUPPORTED;
        return dms_chord_setparam(t->data, mt, data);
    case TRK_COMMAND:
        if (prm == PRM_COMMANDNEXT) return dms_cmd_setparam(t->data, 1, mt, data);
        if (prm != PRM_COMMAND) return DMUS_E_SET_UNSUPPORTED;
        if (!data) return DM_E_POINTER;
        return dms_cmd_setparam(t->data, 0, mt, data);
    case TRK_MUTE:
        if (!data) return DM_E_POINTER;
        if (prm != PRM_MUTE) return DMUS_E_TYPE_UNSUPPORTED;
        return dms_mute_setparam(t->data, mt, data);
    case TRK_STYLE: return stytrack_setparam(t, prm, mt, data);
    case TRK_MOTIF: case TRK_PATTERN: return motiftrack_setparam(t, prm, data);
    case TRK_CHORDMAP:                  /* 7728c327 */
        if (!data) return DM_E_POINTER;
        if (prm != PRM_CHORDMAP) return DMUS_E_SET_UNSUPPORTED;
        return dmc_cmtrack_setparam(t->data, mt, data);
    case TRK_SIGNPOST: return DM_E_NOTIMPL;   /* 7728c0fa */
    }
    return DMUS_E_SET_UNSUPPORTED;
}

/* 77401c59 / 774032a7: a notification from a dmstyle track (punkUser the state, the track's group).  The private chord
 * notification goes straight to the performance's tool (TOOL_IMMEDIATE); the others through the state's graph. */
static int sty_notify(dm_perf *pf, dm_segstate *ss, sstrack *e, const dm_guid *g, uint32_t option, mtime_t mt,
                      uint32_t flags)
{
    if (flags & TF_NOTIFY_OFF) return DM_OK;
    dm_pmsg *m = pm_alloc(pf, 0x58);
    if (!m) return DM_E_OUTOFMEMORY;
    m->type = PT_NOTIFICATION;
    m->mt = mt;
    m->flags = PF_MUSICTIME;
    m->user = ss; m->userref = 1; ss->refs++;
    m->nf_guid = *g;
    m->nf_option = option;
    m->group = e->group;
    if (guid_same(g, &GUID_NOTIFICATION_PRIVATE_CHORD_)) {
        m->flags |= PF_TOOL_IMMEDIATE;
        m->tool = 1;
        if (send_pmsg(pf, m) < 0) { pm_free(pf, m); return DM_E_FAIL; }
        return DM_OK;
    }
    m->flags |= PF_TOOL_ATTIME;
    stamp_send(pf, ss, m);
    return DM_OK;
}

/* 774020ba: the chord track tells the performance of every chord in [start, end) (and, seeking, of the one in effect
 * at start, unless one falls in the range: then not before it), with GUID_NOTIFICATION_CHORD when asked for */
static int chord_play(dm_perf *pf, dm_segstate *ss, sstrack *e, mtime_t start, mtime_t end, mtime_t off, uint32_t flags)
{
    const dms_chordtrack *T = e->t->data;
    int notify = e->t->notify;
    if (flags & TF_DIRTY) sty_notify(pf, ss, e, &GUID_NOTIFICATION_PRIVATE_CHORD_, 0, start + off, flags);
    int seek = (flags & TF_SEEK) && !(flags & TF_FLUSH);
    int hr = DM_OK;
    const dms_chord *last = NULL;
    for (const dms_chord *c = T->chords; c && hr >= 0; c = c->next) {
        if (c->t < start) {
            if (seek) last = c;
            continue;
        }
        if (end <= c->t) {
            if (last) {
                sty_notify(pf, ss, e, &GUID_NOTIFICATION_PRIVATE_CHORD_, 0, off + start, flags);
                if (notify) hr = sty_notify(pf, ss, e, &GUID_NOTIFICATION_CHORD_, 0, off + start, flags);
            }
            return hr;
        }
        if (last) {
            sty_notify(pf, ss, e, &GUID_NOTIFICATION_PRIVATE_CHORD_, 0, start + off, flags);
            if (notify) hr = sty_notify(pf, ss, e, &GUID_NOTIFICATION_CHORD_, 0, start + off, flags);
            last = NULL;
        }
        if (hr >= 0) {
            sty_notify(pf, ss, e, &GUID_NOTIFICATION_PRIVATE_CHORD_, 0, c->t + off, flags);
            if (notify) hr = sty_notify(pf, ss, e, &GUID_NOTIFICATION_CHORD_, 0, c->t + off, flags);
        }
    }
    return hr;          /* (a chord remembered before start with none after it is never told) */
}

/* 77403593: GUID_NOTIFICATION_COMMAND for the commands in [start, end) (seeking, the one in effect at start);
 * dwNotificationOption 1 for an embellishment, 0 for a groove */
static int cmd_play(dm_perf *pf, dm_segstate *ss, sstrack *e, mtime_t start, mtime_t end, mtime_t off, uint32_t flags)
{
    const dms_cmdtrack *T = e->t->data;
    if (!e->t->notify) return DM_OK;
    int seek = (flags & TF_SEEK) && !(flags & TF_FLUSH);
    int hr = DM_OK;
    const dms_command *last = NULL;
    for (const dms_command *c = T->cmds; c && hr >= 0; c = c->next) {
        if (c->t < start) {
            if (seek) last = c;
            continue;
        }
        if (end <= c->t) {
            if (last) hr = sty_notify(pf, ss, e, &GUID_NOTIFICATION_COMMAND_, last->command != 0, start + off, flags);
            return hr;
        }
        if (last) {
            hr = sty_notify(pf, ss, e, &GUID_NOTIFICATION_COMMAND_, last->command != 0, start + off, flags);
            last = NULL;
        }
        if (hr >= 0) hr = sty_notify(pf, ss, e, &GUID_NOTIFICATION_COMMAND_, c->command != 0, c->t + off, flags);
    }
    return hr;
}

static int sty_play(dm_perf *pf, dm_segstate *ss, sstrack *e, mtime_t start, mtime_t end, mtime_t off, uint32_t flags)
{
    switch (e->t->kind) {
    case TRK_CHORD: return chord_play(pf, ss, e, start, end, off, flags);
    case TRK_COMMAND: return cmd_play(pf, ss, e, start, end, off, flags);
    case TRK_MUTE: return DMUS_S_END;       /* 77415388 */
    case TRK_STYLE: return stytrack_play(pf, ss, e, start, end, off, flags);
    case TRK_MOTIF: return motiftrack_play(pf, ss, e, start, end, off, flags);
    case TRK_PATTERN: return patterntrack_play(pf, ss, e, start, end, off, flags);
    case TRK_CHORDMAP: return DM_OK;      /* 7728bd46 */
    case TRK_SIGNPOST:                  /* 7728aeaf: recomposing at the start or a loop (PLAY_COMPOSE / LOOP_COMPOSE) */
        if ((flags & (TF_START | TF_LOOP)) && (flags & 0x200) && !(flags & TF_PLAY_OFF) && ss->seg) {
            dmc_recompose(ss->seg, pf->apprand ? dm_app_rand_state() : &pf->rnd, pf->apprand);
            if (e->t->notify) sty_notify(pf, ss, e, &GUID_NOTIFICATION_RECOMPOSE_, 0, start + off, flags);   /* 7728ab6e */
        }
        return DM_OK;
    }
    return DM_OK;
}

static int sty_getparam_state(const dm_track *t, void *state, int prm, mtime_t mt, mtime_t *next, void *data)
{
    if (t->kind == TRK_STYLE) return stytrack_getparam(t, state, prm, mt, next, data);
    if (t->kind == TRK_PATTERN) return patterntrack_getparam(t, state, prm, mt, next, data);
    return sty_getparam(t, prm, mt, next, data);
}
