/* reAL configuration.
 *
 * Sources, lowest to highest priority:
 *   1. %WINDIR%\reAL.ini             keys outside sections and [default]: the machine's, for every game
 *   2. %APPDATA%\reAL\reAL.ini       the same of the user's own file (no administrator needed to write it;
 *                                    reALcfg keeps the settings for every game there)
 *   3. %WINDIR%\reAL.ini, then %APPDATA%\reAL\reAL.ini   [<game>.exe]  (case-insensitive): what one game needs
 *                                    otherwise, which the settings for every game do not undo
 *   4. <dir of this dll>\dsound.ini   (ALchemy-compatible flat key=value file; reAL keys work too)
 *   5. <dir of the game exe>\reAL.ini (keys outside sections, [default] and [<game>.exe], in the file's order)
 *   6. environment: REAL_LOG=<0-3>, REAL_INI=<path to an extra ini, highest priority>
 */
#include "real.h"
#include "eax4dsp.h"
#ifndef REAL_BUILD
#define REAL_BUILD "unknown build"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

rl_config g_cfg;

static void defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.buffers = 4;
    g_cfg.duration = 25;
    g_cfg.max_voices = 128;
    g_cfg.enabled = 1;
    g_cfg.hrtf_mode = 1;
    g_cfg.resampler = RL_INTERP_SINC;
    g_cfg.midi_rate = 22050;
    g_cfg.midi_reverb = 1;
    g_cfg.waveout = 1;
    g_cfg.fixes = 1;
    g_cfg.dither = 1;
    g_cfg.headphones = -1;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static int is_true(const char *v)
{
    return !_stricmp(v, "true") || !_stricmp(v, "1") || !_stricmp(v, "yes") || !_stricmp(v, "on");
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static const char *s_cur_file, *s_cur_section;
static char s_music_from[MAX_PATH + 64] = "default";

static void apply(const char *key, const char *val)
{
    /* ALchemy keys (same names & clamps as the original dsound.dll) */
    if      (!_stricmp(key, "buffers"))       g_cfg.buffers = clampi(atoi(val), 2, 64);
    else if (!_stricmp(key, "duration"))      g_cfg.duration = clampi(atoi(val), 5, 500);
    else if (!_stricmp(key, "maxvoicecount")) g_cfg.max_voices = clampi(atoi(val), 16, 1024);
    else if (!_stricmp(key, "disabledirectmusic")) g_cfg.disable_dmusic = is_true(val);
    else if (!_stricmp(key, "logdirectsound")) { if (is_true(val) && g_cfg.log_level < 2) g_cfg.log_level = 2; }
    else if (!_stricmp(key, "logdirectsound2d") || !_stricmp(key, "logdirectsound2dstreaming")) {
        if (is_true(val)) g_cfg.log_level = 3;
    }
    else if (!_stricmp(key, "logdirectsound3d") || !_stricmp(key, "logdirectsoundlistener")) g_cfg.log_3d = is_true(val);
    else if (!_stricmp(key, "logdirectsoundeax")) g_cfg.log_eax = is_true(val);
    else if (!_stricmp(key, "disablenativeal") || !_stricmp(key, "logdirectsoundtiminginfo") ||
             !_stricmp(key, "logstarvation") || !_stricmp(key, "preset") ||
             !_stricmp(key, "regpath") || !_stricmp(key, "gamepath") || !_stricmp(key, "subdir") ||
             !_stricmp(key, "rootdirinstalloption")) { /* accepted, meaningless for reAL */ }
    /* reAL keys */
    else if (!_stricmp(key, "enabled"))    g_cfg.enabled = is_true(val);
    else if (!_stricmp(key, "latency"))    g_cfg.latency_ms = clampi(atoi(val), 0, 500);
    else if (!_stricmp(key, "outrate"))
        g_cfg.out_rate = !_stricmp(val, "game") ? -1 : !_stricmp(val, "device") ? 0 :
                         atoi(val) >= 8000 && atoi(val) <= 192000 ? atoi(val) : 0;
    else if (!_stricmp(key, "log"))        g_cfg.log_level = clampi(atoi(val), 0, 3);
    else if (!_stricmp(key, "log3d"))      g_cfg.log_3d = is_true(val);
    else if (!_stricmp(key, "fixes"))      g_cfg.fixes = is_true(val);
    else if (!_stricmp(key, "dither"))     g_cfg.dither = is_true(val);
    else if (!_stricmp(key, "outbits"))    g_cfg.out_bits = atoi(val) == 16 ? 16 : 0;
    else if (!_stricmp(key, "logeax"))     g_cfg.log_eax = is_true(val);
    else if (!_stricmp(key, "logfile"))    lstrcpynA(g_cfg.log_file, val, MAX_PATH);
    else if (!_stricmp(key, "realdsound")) lstrcpynA(g_cfg.real_dsound, val, MAX_PATH);
    else if (!_stricmp(key, "redm") || !_stricmp(key, "music")) {       /* (music: its name before 2.0) */
        g_cfg.music = is_true(val);
        if (s_cur_file) snprintf(s_music_from, sizeof(s_music_from), "%s%s%s%s", s_cur_file,
                                 s_cur_section && s_cur_section[0] ? " [" : "", s_cur_section ? s_cur_section : "",
                                 s_cur_section && s_cur_section[0] ? "]" : "");
    }
    else if (!_stricmp(key, "miditrace"))  g_cfg.midi_trace = is_true(val);
    else if (!_stricmp(key, "midi"))       g_cfg.midi = is_true(val);
    else if (!_stricmp(key, "midirate"))   g_cfg.midi_rate = !_stricmp(val, "auto") ? 0 : clampi(atoi(val), 11025, 48000);
    else if (!_stricmp(key, "midireverb")) g_cfg.midi_reverb = is_true(val);
    else if (!_stricmp(key, "waveout"))    g_cfg.waveout = !_stricmp(val, "pcm") ? 2 : is_true(val);
    else if (!_stricmp(key, "gmdls"))      lstrcpynA(g_cfg.gm_dls, val, MAX_PATH);
    else if (!_stricmp(key, "resampler"))
        g_cfg.resampler = (!_stricmp(val, "linear") || !_stricmp(val, "alchemy")) ? RL_INTERP_LINEAR : RL_INTERP_SINC;
    else if (!_stricmp(key, "speakers")) {
        g_cfg.headphones = -1;
        if (!_stricmp(val, "headphones") || !_stricmp(val, "headphone")) { g_cfg.speakers = RL_LAYOUT_STEREO; g_cfg.headphones = 1; }
        else if (!_stricmp(val, "stereo") || !_stricmp(val, "2.0") || !_stricmp(val, "2")) { g_cfg.speakers = RL_LAYOUT_STEREO; g_cfg.headphones = 0; }
        else if (!_stricmp(val, "quad") || !_stricmp(val, "4.0") || !_stricmp(val, "4")) g_cfg.speakers = RL_LAYOUT_QUAD;
        else if (!_stricmp(val, "5.1") || !_stricmp(val, "6")) g_cfg.speakers = RL_LAYOUT_51;
        else if (!_stricmp(val, "7.1") || !_stricmp(val, "8")) g_cfg.speakers = RL_LAYOUT_71;
        else g_cfg.speakers = 0;             /* auto */
    }
    else if (!_stricmp(key, "cmss"))       g_cfg.cmss = is_true(val);
    else if (!_stricmp(key, "cmssintensity")) g_cfg.cmss_intensity = clampi(atoi(val), -100, 99);
    else if (!_stricmp(key, "hrtf")) {
        if (!_stricmp(val, "off") || !_stricmp(val, "false") || !_stricmp(val, "0")) g_cfg.hrtf_mode = 0;
        else if (!_stricmp(val, "auto") || !_stricmp(val, "on") || !_stricmp(val, "true") || !_stricmp(val, "1")) g_cfg.hrtf_mode = 1;
        else if (!_stricmp(val, "builtin")) { g_cfg.hrtf_mode = 2; g_cfg.hrtf_path[0] = 0; }
        else { g_cfg.hrtf_mode = 1; lstrcpynA(g_cfg.hrtf_path, val, MAX_PATH); }
    }
}

/* exe_section: section name that also applies ("" = only global keys).
 * Keys outside any section and in [default] always apply. */
/* a file's keys applied.  which: 0 all that hold for this game, in the file's order; 1 those for every game alone
 * (outside sections and [default]); 2 this game's section alone.  0: there is no such file */
static int load_stream(FILE *f, const char *path, const char *exe_section, int which)
{
    char section[128] = "";
    if (!f) return 0;
    s_cur_file = path;
    s_cur_section = section;
    char line[1024];
    int active = which != 2; /* outside a section: global */
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) *e = 0;
            const char *name = s + 1;
            lstrcpynA(section, name, sizeof(section));
            active = (which != 2 && !_stricmp(name, "default")) || (which != 1 && exe_section[0] && !_stricmp(name, exe_section));
            continue;
        }
        if (!active) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        apply(trim(s), trim(eq + 1));
    }
    fclose(f);
    s_cur_file = s_cur_section = NULL;
    return 1;
}
static int load_file(const char *path, const char *exe_section, int which)
{
    return load_stream(fopen(path, "r"), path, exe_section, which);
}

/* %APPDATA%\reAL\reAL.ini: opened by its wide name (a user's name may hold letters the system's code page has
 * not); `shown` is the name for the log (MAX_PATH + 32 chars) */
static int load_user(char *shown, const char *exe_section, int which)
{
    WCHAR w[MAX_PATH + 32];
    const DWORD len = GetEnvironmentVariableW(L"APPDATA", w, MAX_PATH);
    if (!len || len >= MAX_PATH) return 0;
    lstrcatW(w, L"\\reAL\\reAL.ini");
    if (!WideCharToMultiByte(CP_ACP, 0, w, -1, shown, MAX_PATH + 32, NULL, NULL)) lstrcpyA(shown, "%APPDATA%\\reAL\\reAL.ini");
    return load_stream(_wfopen(w, L"r"), shown, exe_section, which);
}

static void dir_of(char *path)
{
    char *p = strrchr(path, '\\');
    if (!p) p = strrchr(path, '/');
    if (p) p[1] = 0; else path[0] = 0;
}

static char s_loaded[5][MAX_PATH];

void rl_config_load(void)
{
    char exe[MAX_PATH], exe_name[MAX_PATH], path[MAX_PATH + 32];
    defaults();

    exe[0] = 0;
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    exe[MAX_PATH - 1] = 0;          /* (a path cut at MAX_PATH has no terminator on Windows XP) */
    const char *base = strrchr(exe, '\\');
    lstrcpynA(exe_name, base ? base + 1 : exe, MAX_PATH);

    /* the machine's file and the user's: what they have for every game, then what they have for this one */
    int n = 0;
    char machine[MAX_PATH + 32], user[MAX_PATH + 32];
    UINT len = GetWindowsDirectoryA(machine, MAX_PATH);
    if (len && len < MAX_PATH) lstrcatA(machine, "\\reAL.ini"); else machine[0] = 0;
    if (machine[0] && load_file(machine, exe_name, 1)) lstrcpynA(s_loaded[n++], machine, MAX_PATH);
    if (load_user(user, exe_name, 1)) lstrcpynA(s_loaded[n++], user, MAX_PATH);
    if (machine[0]) load_file(machine, exe_name, 2);
    load_user(user, exe_name, 2);


    path[0] = 0;
    GetModuleFileNameA(g_self, path, MAX_PATH);
    path[MAX_PATH - 1] = 0;
    dir_of(path);
    lstrcatA(path, "dsound.ini");
    if (load_file(path, exe_name, 0)) lstrcpynA(s_loaded[n++], path, MAX_PATH);

    lstrcpynA(path, exe, MAX_PATH);
    dir_of(path);
    lstrcatA(path, "reAL.ini");
    if (load_file(path, exe_name, 0)) lstrcpynA(s_loaded[n++], path, MAX_PATH);

    if (GetEnvironmentVariableA("REAL_INI", path, MAX_PATH) && load_file(path, exe_name, 0))
        lstrcpynA(s_loaded[n++], path, MAX_PATH);
    if (GetEnvironmentVariableA("REAL_NOFIXES", NULL, 0)) g_cfg.fixes = 0;
    eax4dsp_fixes = eax_fixes = g_cfg.fixes;    /* (the EAX code is built without the config: eax4dsp.h, eax.h) */
    if (GetEnvironmentVariableA("REAL_LOG", path, MAX_PATH))
        g_cfg.log_level = clampi(atoi(path), 0, 3);

    if (!g_cfg.latency_ms) {
        /* ALchemy's default queue is buffers*duration = 100 ms; we mix directly into WASAPI,
         * so half of that is plenty and still very safe under Wine. */
        g_cfg.latency_ms = clampi(g_cfg.buffers * g_cfg.duration / 2, 10, 500);
    }

    if (!g_cfg.log_file[0]) {
        lstrcpynA(g_cfg.log_file, exe, MAX_PATH);
        dir_of(g_cfg.log_file);
        /* (a folder whose path leaves no room for the name: the log next to nothing, in the current folder) */
        if (lstrlenA(g_cfg.log_file) > MAX_PATH - 9) g_cfg.log_file[0] = 0;
        lstrcatA(g_cfg.log_file, "reAL.log");
    }

    rl_log_init();
    /* (reALcfg finds the version in the file by this mark: a version resource cannot be relied on for it, since
     * Wine answers for a file named dsound.dll with its own DLL's) */
    static const char mark[] = "reAL-version:" REAL_VERSION;
    LOG_INFO("reAL %s (" REAL_BUILD ") loaded into %s", mark + sizeof("reAL-version:") - 1, exe);
    for (int i = 0; i < n; i++) LOG_INFO("  config: %s", s_loaded[i]);
    char outrate[16];
    if (g_cfg.out_rate > 0) snprintf(outrate, sizeof(outrate), "%d", g_cfg.out_rate);
    else lstrcpynA(outrate, g_cfg.out_rate ? "game" : "device", sizeof(outrate));
    LOG_INFO("  enabled=%d latency=%dms outrate=%s maxvoicecount=%d disabledirectmusic=%d hrtf=%s resampler=%s log=%d",
             g_cfg.enabled, g_cfg.latency_ms, outrate, g_cfg.max_voices, g_cfg.disable_dmusic,
             !g_cfg.hrtf_mode ? "off" : g_cfg.hrtf_mode == 2 ? "builtin" : g_cfg.hrtf_path[0] ? g_cfg.hrtf_path : "auto",
             g_cfg.resampler == RL_INTERP_SINC ? "sinc" : "linear", g_cfg.log_level);
    LOG_INFO("  fixes=%d outbits=%s dither=%d", g_cfg.fixes, g_cfg.out_bits == 16 ? "16" : "device", g_cfg.dither);
    {
        char self[MAX_PATH];
        GetModuleFileNameA(g_self, self, MAX_PATH);
        LOG_INFO("  module: %s (pid %lu)", self, (unsigned long)GetCurrentProcessId());
    }
    if (g_cfg.log_level > 0) rl_crash_logger();
    if (g_cfg.speakers)
        LOG_INFO("  speakers=%s", g_cfg.speakers == RL_LAYOUT_71 ? "7.1" : g_cfg.speakers == RL_LAYOUT_51 ? "5.1" :
                 g_cfg.speakers == RL_LAYOUT_QUAD ? "quad" : g_cfg.headphones == 1 ? "headphones" : "stereo");
    if (g_cfg.cmss)
        LOG_INFO("  cmss=on cmssintensity=%d", g_cfg.cmss_intensity);
    LOG_INFO("  redm=%d (set by %s) gmdls=%s", g_cfg.music, s_music_from, g_cfg.gm_dls[0] ? g_cfg.gm_dls : "(system gm.dls)");
    if (g_cfg.midi || g_cfg.midi_trace)
        LOG_INFO("  midi=%d midirate=%s%.0d midireverb=%d miditrace=%d", g_cfg.midi, g_cfg.midi_rate ? "" : "auto",
                 g_cfg.midi_rate, g_cfg.midi_reverb, g_cfg.midi_trace);
}
