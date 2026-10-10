/* winmm MIDI / MCI hooks.
 *
 * Games older than DirectMusic (Claw, ...) play music through winmm: the midiOut / midiStream API or the MCI
 * sequencer.  reAL is loaded into those games anyway (dsound.dll), so it hooks winmm's functions in place, like the
 * COM hooks for DirectMusic, instead of needing a winmm.dll proxy.
 *
 * miditrace=true: every call is logged with its arguments and result and passed on unchanged.  Calls winmm makes
 * internally while serving another one (the MCI sequencer driving midiOut, a stream thread) are indented. */
#include "real.h"
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

static __thread int t_depth;
#define IND (_d ? "    " : "")
#define ENTER() int _d = t_depth++
#define LEAVE() (t_depth--)

static void hexs(char *out, int n, const BYTE *p, DWORD len)
{
    int o = 0;
    out[0] = 0;
    for (DWORD i = 0; i < len && o + 4 < n; i++) o += snprintf(out + o, n - o, "%02x ", p[i]);
    if (len * 3 >= (DWORD)n - 4 && o + 4 < n) snprintf(out + o, n - o, "...");
}

/* ------------------------------------------------------------------ midiOut */
static UINT (WINAPI *o_getnumdevs)(void);
static MMRESULT (WINAPI *o_getcapsA)(UINT_PTR, LPMIDIOUTCAPSA, UINT);
static MMRESULT (WINAPI *o_getcapsW)(UINT_PTR, LPMIDIOUTCAPSW, UINT);
static MMRESULT (WINAPI *o_open)(LPHMIDIOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD);
static MMRESULT (WINAPI *o_close)(HMIDIOUT);
static MMRESULT (WINAPI *o_short)(HMIDIOUT, DWORD);
static MMRESULT (WINAPI *o_long)(HMIDIOUT, LPMIDIHDR, UINT);
static MMRESULT (WINAPI *o_prep)(HMIDIOUT, LPMIDIHDR, UINT);
static MMRESULT (WINAPI *o_unprep)(HMIDIOUT, LPMIDIHDR, UINT);
static MMRESULT (WINAPI *o_reset)(HMIDIOUT);
static MMRESULT (WINAPI *o_setvol)(HMIDIOUT, DWORD);
static MMRESULT (WINAPI *o_getvol)(HMIDIOUT, LPDWORD);
static MMRESULT (WINAPI *o_message)(HMIDIOUT, UINT, DWORD_PTR, DWORD_PTR);
static MMRESULT (WINAPI *o_cache)(HMIDIOUT, UINT, LPWORD, UINT);

static UINT WINAPI h_getnumdevs(void)
{
    ENTER(); UINT r = o_getnumdevs(); LEAVE();
    LOG_INFO("midi: %smidiOutGetNumDevs() -> %u", IND, r);
    return r;
}
static MMRESULT WINAPI h_getcapsA(UINT_PTR id, LPMIDIOUTCAPSA c, UINT sz)
{
    ENTER(); MMRESULT r = o_getcapsA(id, c, sz); LEAVE();
    LOG_INFO("midi: %smidiOutGetDevCapsA(%ld) -> %u%s%s tech %u voices %u notes %u chanmask %04x support %lx", IND, (long)id, r,
             r ? "" : " ", r ? "" : c->szPname, r ? 0 : c->wTechnology, r ? 0 : c->wVoices, r ? 0 : c->wNotes,
             r ? 0 : c->wChannelMask, r ? 0UL : (unsigned long)c->dwSupport);
    return r;
}
static MMRESULT WINAPI h_getcapsW(UINT_PTR id, LPMIDIOUTCAPSW c, UINT sz)
{
    ENTER(); MMRESULT r = o_getcapsW(id, c, sz); LEAVE();
    char name[64] = "";
    if (!r) WideCharToMultiByte(CP_ACP, 0, c->szPname, -1, name, sizeof(name), NULL, NULL);
    LOG_INFO("midi: %smidiOutGetDevCapsW(%ld) -> %u %s", IND, (long)id, r, name);
    return r;
}
static MMRESULT WINAPI h_open(LPHMIDIOUT ph, UINT id, DWORD_PTR cb, DWORD_PTR inst, DWORD fl)
{
    ENTER(); MMRESULT r = o_open(ph, id, cb, inst, fl); LEAVE();
    LOG_INFO("midi: %smidiOutOpen(dev %d, callback %p, inst %p, flags %lx) -> %u handle %p", IND, (int)id, (void *)cb,
             (void *)inst, (unsigned long)fl, r, ph ? (void *)*ph : NULL);
    return r;
}
static MMRESULT WINAPI h_close(HMIDIOUT h)
{
    ENTER(); MMRESULT r = o_close(h); LEAVE();
    LOG_INFO("midi: %smidiOutClose(%p) -> %u", IND, (void *)h, r);
    return r;
}
static MMRESULT WINAPI h_short(HMIDIOUT h, DWORD m)
{
    ENTER(); MMRESULT r = o_short(h, m); LEAVE();
    LOG_INFO("midi: %sshort %p %02x %02x %02x%s", IND, (void *)h, (unsigned)(m & 0xff), (unsigned)((m >> 8) & 0xff),
             (unsigned)((m >> 16) & 0xff), r ? " FAILED" : "");
    return r;
}
static MMRESULT WINAPI h_long(HMIDIOUT h, LPMIDIHDR hd, UINT sz)
{
    char x[200];
    if (hd) hexs(x, sizeof(x), (const BYTE *)hd->lpData, hd->dwBufferLength); else x[0] = 0;
    ENTER(); MMRESULT r = o_long(h, hd, sz); LEAVE();
    LOG_INFO("midi: %smidiOutLongMsg(%p, %lu bytes) -> %u: %s", IND, (void *)h, hd ? (unsigned long)hd->dwBufferLength : 0UL, r, x);
    return r;
}
static MMRESULT WINAPI h_prep(HMIDIOUT h, LPMIDIHDR hd, UINT sz)
{
    ENTER(); MMRESULT r = o_prep(h, hd, sz); LEAVE();
    LOG_INFO("midi: %smidiOutPrepareHeader(%p, %p) -> %u", IND, (void *)h, (void *)hd, r);
    return r;
}
static MMRESULT WINAPI h_unprep(HMIDIOUT h, LPMIDIHDR hd, UINT sz)
{
    ENTER(); MMRESULT r = o_unprep(h, hd, sz); LEAVE();
    LOG_INFO("midi: %smidiOutUnprepareHeader(%p, %p) -> %u", IND, (void *)h, (void *)hd, r);
    return r;
}
static MMRESULT WINAPI h_reset(HMIDIOUT h)
{
    ENTER(); MMRESULT r = o_reset(h); LEAVE();
    LOG_INFO("midi: %smidiOutReset(%p) -> %u", IND, (void *)h, r);
    return r;
}
static MMRESULT WINAPI h_setvol(HMIDIOUT h, DWORD v)
{
    ENTER(); MMRESULT r = o_setvol(h, v); LEAVE();
    LOG_INFO("midi: %smidiOutSetVolume(%p, %08lx) -> %u", IND, (void *)h, (unsigned long)v, r);
    return r;
}
static MMRESULT WINAPI h_getvol(HMIDIOUT h, LPDWORD v)
{
    ENTER(); MMRESULT r = o_getvol(h, v); LEAVE();
    LOG_INFO("midi: %smidiOutGetVolume(%p) -> %u %08lx", IND, (void *)h, r, v && !r ? (unsigned long)*v : 0UL);
    return r;
}
static MMRESULT WINAPI h_message(HMIDIOUT h, UINT m, DWORD_PTR a, DWORD_PTR b)
{
    ENTER(); MMRESULT r = o_message(h, m, a, b); LEAVE();
    LOG_INFO("midi: %smidiOutMessage(%p, %u, %p, %p) -> %u", IND, (void *)h, m, (void *)a, (void *)b, r);
    return r;
}
static MMRESULT WINAPI h_cache(HMIDIOUT h, UINT bank, LPWORD p, UINT fl)
{
    ENTER(); MMRESULT r = o_cache(h, bank, p, fl); LEAVE();
    LOG_INFO("midi: %smidiOutCachePatches(%p, bank %u, flags %x) -> %u", IND, (void *)h, bank, fl, r);
    return r;
}

/* ------------------------------------------------------------------ midiStream */
static MMRESULT (WINAPI *s_open)(LPHMIDISTRM, LPUINT, DWORD, DWORD_PTR, DWORD_PTR, DWORD);
static MMRESULT (WINAPI *s_out)(HMIDISTRM, LPMIDIHDR, UINT);
static MMRESULT (WINAPI *s_restart)(HMIDISTRM);
static MMRESULT (WINAPI *s_pause)(HMIDISTRM);
static MMRESULT (WINAPI *s_stop)(HMIDISTRM);
static MMRESULT (WINAPI *s_close)(HMIDISTRM);
static MMRESULT (WINAPI *s_prop)(HMIDISTRM, LPBYTE, DWORD);
static MMRESULT (WINAPI *s_pos)(HMIDISTRM, LPMMTIME, UINT);

static MMRESULT WINAPI hs_open(LPHMIDISTRM ph, LPUINT dev, DWORD n, DWORD_PTR cb, DWORD_PTR inst, DWORD fl)
{
    UINT d = dev ? *dev : 0;
    ENTER(); MMRESULT r = s_open(ph, dev, n, cb, inst, fl); LEAVE();
    LOG_INFO("midi: %smidiStreamOpen(dev %d, %lu, callback %p, flags %lx) -> %u handle %p", IND, (int)d, (unsigned long)n,
             (void *)cb, (unsigned long)fl, r, ph ? (void *)*ph : NULL);
    return r;
}
static MMRESULT WINAPI hs_out(HMIDISTRM h, LPMIDIHDR hd, UINT sz)
{
    char x[200];
    if (hd) hexs(x, sizeof(x), (const BYTE *)hd->lpData, hd->dwBytesRecorded < 48 ? hd->dwBytesRecorded : 48); else x[0] = 0;
    ENTER(); MMRESULT r = s_out(h, hd, sz); LEAVE();
    LOG_INFO("midi: %smidiStreamOut(%p, %lu bytes) -> %u: %s", IND, (void *)h, hd ? (unsigned long)hd->dwBytesRecorded : 0UL, r, x);
    return r;
}
#define SIMPLE(name, fn, label) \
    static MMRESULT WINAPI name(HMIDISTRM h) { ENTER(); MMRESULT r = fn(h); LEAVE(); \
        LOG_INFO("midi: %s" label "(%p) -> %u", IND, (void *)h, r); return r; }
SIMPLE(hs_restart, s_restart, "midiStreamRestart")
SIMPLE(hs_pause, s_pause, "midiStreamPause")
SIMPLE(hs_stop, s_stop, "midiStreamStop")
SIMPLE(hs_close, s_close, "midiStreamClose")
static MMRESULT WINAPI hs_prop(HMIDISTRM h, LPBYTE p, DWORD fl)
{
    ENTER(); MMRESULT r = s_prop(h, p, fl); LEAVE();
    unsigned long v = p ? (unsigned long)((DWORD *)p)[1] : 0;
    LOG_INFO("midi: %smidiStreamProperty(%p, flags %lx) -> %u value %lu", IND, (void *)h, (unsigned long)fl, r, v);
    return r;
}
static MMRESULT WINAPI hs_pos(HMIDISTRM h, LPMMTIME t, UINT sz)
{
    ENTER(); MMRESULT r = s_pos(h, t, sz); LEAVE();
    if (g_cfg.log_level >= 3) LOG_INFO("midi: %smidiStreamPosition(%p) -> %u", IND, (void *)h, r);
    return r;
}

/* ------------------------------------------------------------------ MCI */
static MCIERROR (WINAPI *m_cmdA)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
static MCIERROR (WINAPI *m_cmdW)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
static MCIERROR (WINAPI *m_strA)(LPCSTR, LPSTR, UINT, HWND);
static MCIERROR (WINAPI *m_strW)(LPCWSTR, LPWSTR, UINT, HWND);

static const char *mci_name(UINT m)
{
    switch (m) {
    case MCI_OPEN: return "OPEN"; case MCI_CLOSE: return "CLOSE"; case MCI_PLAY: return "PLAY"; case MCI_STOP: return "STOP";
    case MCI_PAUSE: return "PAUSE"; case MCI_RESUME: return "RESUME"; case MCI_SEEK: return "SEEK"; case MCI_STATUS: return "STATUS";
    case MCI_SET: return "SET"; case MCI_GETDEVCAPS: return "GETDEVCAPS"; case MCI_INFO: return "INFO"; case MCI_SYSINFO: return "SYSINFO";
    default: return "?";
    }
}
static void mci_log(const char *fn, MCIDEVICEID id, UINT m, DWORD_PTR fl, DWORD_PTR p, MCIERROR r, int wide, int _d)
{
    char extra[300] = "";
    if (m == MCI_OPEN && p) {
        char t[MAX_PATH] = "", e[MAX_PATH] = "";
        if (wide) {
            MCI_OPEN_PARMSW *o = (MCI_OPEN_PARMSW *)p;
            if ((fl & MCI_OPEN_TYPE) && !(fl & MCI_OPEN_TYPE_ID) && o->lpstrDeviceType) WideCharToMultiByte(CP_ACP, 0, o->lpstrDeviceType, -1, t, MAX_PATH, NULL, NULL);
            if ((fl & MCI_OPEN_ELEMENT) && o->lpstrElementName) WideCharToMultiByte(CP_ACP, 0, o->lpstrElementName, -1, e, MAX_PATH, NULL, NULL);
            snprintf(extra, sizeof(extra), " type '%s' element '%s' -> id %u", t, e, o->wDeviceID);
        } else {
            MCI_OPEN_PARMSA *o = (MCI_OPEN_PARMSA *)p;
            if ((fl & MCI_OPEN_TYPE) && !(fl & MCI_OPEN_TYPE_ID) && o->lpstrDeviceType) lstrcpynA(t, o->lpstrDeviceType, MAX_PATH);
            if ((fl & MCI_OPEN_TYPE_ID)) snprintf(t, sizeof(t), "#%lx", (unsigned long)(DWORD_PTR)o->lpstrDeviceType);
            if ((fl & MCI_OPEN_ELEMENT) && o->lpstrElementName) lstrcpynA(e, o->lpstrElementName, MAX_PATH);
            snprintf(extra, sizeof(extra), " type '%s' element '%s' -> id %u", t, e, o->wDeviceID);
        }
    } else if (m == MCI_PLAY && p && (fl & (MCI_FROM | MCI_TO))) {
        MCI_PLAY_PARMS *pp = (MCI_PLAY_PARMS *)p;
        snprintf(extra, sizeof(extra), " from %lu to %lu", (unsigned long)pp->dwFrom, (unsigned long)pp->dwTo);
    } else if (m == MCI_STATUS && p) {
        MCI_STATUS_PARMS *sp = (MCI_STATUS_PARMS *)p;
        snprintf(extra, sizeof(extra), " item %lu -> %lu", (unsigned long)sp->dwItem, (unsigned long)sp->dwReturn);
    } else if (m == MCI_SET && p) {
        MCI_SET_PARMS *sp = (MCI_SET_PARMS *)p;
        snprintf(extra, sizeof(extra), " timeformat %lu", (unsigned long)sp->dwTimeFormat);
    }
    if (m == MCI_STATUS && g_cfg.log_level < 3 && !r) return;      /* polled constantly */
    LOG_INFO("midi: %s%s(dev %u, %s %#x, flags %lx)%s -> %lu", IND, fn, id, mci_name(m), m, (unsigned long)fl, extra, (unsigned long)r);
}
static MCIERROR WINAPI hm_cmdA(MCIDEVICEID id, UINT m, DWORD_PTR fl, DWORD_PTR p)
{
    ENTER(); MCIERROR r = m_cmdA(id, m, fl, p); LEAVE();
    mci_log("mciSendCommandA", id, m, fl, p, r, 0, _d);
    return r;
}
static MCIERROR WINAPI hm_cmdW(MCIDEVICEID id, UINT m, DWORD_PTR fl, DWORD_PTR p)
{
    ENTER(); MCIERROR r = m_cmdW(id, m, fl, p); LEAVE();
    mci_log("mciSendCommandW", id, m, fl, p, r, 1, _d);
    return r;
}
static MCIERROR WINAPI hm_strA(LPCSTR cmd, LPSTR ret, UINT n, HWND w)
{
    ENTER(); MCIERROR r = m_strA(cmd, ret, n, w); LEAVE();
    if (g_cfg.log_level >= 3 || r || !cmd || _strnicmp(cmd, "status", 6))
        LOG_INFO("midi: %smciSendStringA(\"%s\") -> %lu \"%s\"", IND, cmd ? cmd : "", (unsigned long)r, ret && n && !r ? ret : "");
    return r;
}
static MCIERROR WINAPI hm_strW(LPCWSTR cmd, LPWSTR ret, UINT n, HWND w)
{
    ENTER(); MCIERROR r = m_strW(cmd, ret, n, w); LEAVE();
    char c[300] = "", o[200] = "";
    if (cmd) WideCharToMultiByte(CP_ACP, 0, cmd, -1, c, sizeof(c), NULL, NULL);
    if (ret && n && !r) WideCharToMultiByte(CP_ACP, 0, ret, -1, o, sizeof(o), NULL, NULL);
    if (g_cfg.log_level >= 3 || r || _strnicmp(c, "status", 6))
        LOG_INFO("midi: %smciSendStringW(\"%s\") -> %lu \"%s\"", IND, c, (unsigned long)r, o);
    return r;
}

/* ------------------------------------------------------------------ install */
#define HOOK(name, var, fn) do { void *f = (void *)GetProcAddress(mm, name); \
        if (f) { void *t = rl_hotpatch(f, (void *)(fn), name); if (t) *(void **)&(var) = t; } } while (0)

void rl_midi_hook(int can_load)
{
    static int done;
    if (!g_cfg.enabled || !g_cfg.midi_trace || done) return;
    HMODULE mm = GetModuleHandleA("winmm.dll");
    if (!mm && can_load) mm = LoadLibraryA("winmm.dll");
    if (!mm) return;
    done = 1;
    HMODULE pin;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)rl_midi_hook, &pin);
    LOG_INFO("midi: tracing winmm MIDI and MCI calls");
    HOOK("midiOutGetNumDevs", o_getnumdevs, h_getnumdevs);
    HOOK("midiOutGetDevCapsA", o_getcapsA, h_getcapsA);
    HOOK("midiOutGetDevCapsW", o_getcapsW, h_getcapsW);
    HOOK("midiOutOpen", o_open, h_open);
    HOOK("midiOutClose", o_close, h_close);
    HOOK("midiOutShortMsg", o_short, h_short);
    HOOK("midiOutLongMsg", o_long, h_long);
    HOOK("midiOutPrepareHeader", o_prep, h_prep);
    HOOK("midiOutUnprepareHeader", o_unprep, h_unprep);
    HOOK("midiOutReset", o_reset, h_reset);
    HOOK("midiOutSetVolume", o_setvol, h_setvol);
    HOOK("midiOutGetVolume", o_getvol, h_getvol);
    HOOK("midiOutMessage", o_message, h_message);
    HOOK("midiOutCachePatches", o_cache, h_cache);
    HOOK("midiStreamOpen", s_open, hs_open);
    HOOK("midiStreamOut", s_out, hs_out);
    HOOK("midiStreamRestart", s_restart, hs_restart);
    HOOK("midiStreamPause", s_pause, hs_pause);
    HOOK("midiStreamStop", s_stop, hs_stop);
    HOOK("midiStreamClose", s_close, hs_close);
    HOOK("midiStreamProperty", s_prop, hs_prop);
    HOOK("midiStreamPosition", s_pos, hs_pos);
    HOOK("mciSendCommandA", m_cmdA, hm_cmdA);
    HOOK("mciSendCommandW", m_cmdW, hm_cmdW);
    HOOK("mciSendStringA", m_strA, hm_strA);
    HOOK("mciSendStringW", m_strW, hm_strW);
}
