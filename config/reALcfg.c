/* reALcfg - reAL's configurator.
 *
 * What ALchemy's program does for ALchemy.  Plain Win32, one exe with nothing beside it but the DLLs it installs;
 * it runs on Windows and under Wine.
 *
 * Games: a list of games; reAL's dsound.dll (and winmm.dll) put into a game's folder and taken out again (under
 * Wine the game's DLL overrides are set as well); and what the game needs otherwise than the rest, kept in the
 * reAL.ini of its folder.
 * Sound, reDM, reAL: reAL's settings for every game, kept in the user's own %APPDATA%\reAL\reAL.ini; and the two
 * COM registrations, both of the DLLs beside this program (DirectSound made through COM pointed at reAL; reDM as
 * DirectMusic's server).
 *
 * Not yet: a database of known games, per-game sections of the user's file.
 *
 * Colours are the system's throughout (a dark scheme under Wine is followed; the visual styles, which would draw
 * light controls on it, are then left off), the widths come from the system's font, and every size is scaled to
 * the screen's dots per inch as the program starts (a second screen of another scale is Windows' to stretch). */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define MAXGAMES 64
/* the wordmark's marine blue, and the lighter one that reads on a dark ground */
#define BLUE       RGB(0x0B, 0x5C, 0x8E)
#define BLUE_LIGHT RGB(0x4F, 0xA8, 0xDB)
enum { ID_LIST = 100, ID_ADD, ID_REMOVE, ID_INSTALL, ID_UNINSTALL, ID_WINMM, ID_REG, ID_UNREG, ID_COM, ID_UNCOM, ID_SAVE, ID_SAVEGAME,
       ID_STATUS, ID_GROUP, ID_TAB, ID_SET0 = 200, ID_BROWSE0 = 300, ID_GAME0 = 400 };
enum { PAGE_GAMES = 1, PAGE_SOUND, PAGE_REDM, PAGE_REAL, NPAGES = PAGE_REAL };

static HINSTANCE s_inst;
static HWND s_wnd, s_list, s_status, s_gamebox, s_tab, s_focus;
/* every control: the page it is on (0: all) and when it shows (0: always; 1: with a game picked; 2: with none) */
static struct { HWND h; int page, when; } s_ctl[256];
static int s_nctl, s_page = PAGE_GAMES, s_mk_page, s_mk_when;
static int s_dpi = 96;                  /* the screen's dots per inch: sizes below are for 96 and go through S() */
static int s_sysdpi = 96;               /* (the system's own, which its font is made for; s_dpi is less where the screen is small) */
#define S(v) MulDiv((v), s_dpi, 96)
static int s_dark;                      /* the system's colours are light on dark (a dark scheme, under Wine as a rule) */
static HFONT s_font, s_logo;
static WCHAR s_dir[MAX_PATH];           /* where the configurator is, and the DLLs it installs */
static WCHAR s_appdir[MAX_PATH];        /* %APPDATA%\reAL: the user's settings for every game (reAL.ini) and the list of games */
static WCHAR s_ini[MAX_PATH];
static WCHAR s_base[MAX_PATH];          /* %WINDIR%\reAL.ini, which reAL reads before the user's */
static WCHAR s_version[32];             /* the configurator's own */
static WCHAR s_games[MAXGAMES][MAX_PATH];
static int s_ngames;
static int s_wine;
static int s_cut;                       /* a path did not fit (fmt) since this was last cleared */
static int s_homeless;                  /* no %APPDATA% to keep the user's files in */

/* reAL's settings, for every game: keys of the [default] section of s_ini.  One left empty, or as it is without the
 * user's file, is taken out of the file */
enum { T_CHECK, T_LIST, T_COMBO, T_EDIT, T_PATH };  /* a checkbox; a list of values; the same with room to type; a number or text; a file */
typedef struct setting {
    int page, type;
    const WCHAR *key, *label;
    const WCHAR *options;               /* T_LIST, T_COMBO: the values, \0 between them */
    const WCHAR *def;                   /* what reAL does without the key */
    const WCHAR *filter;                /* a Browse button: the file dialog's filter */
    const WCHAR *cue;                   /* T_PATH: what an empty field means */
    HWND ctl;
} setting;
static setting s_set[] = {
    { PAGE_SOUND, T_LIST,  L"speakers",  L"Speakers", L"auto\0stereo\0headphones\0quad\0" L"5.1\0" L"7.1\0", L"auto", NULL, NULL, NULL },
    { PAGE_SOUND, T_COMBO, L"hrtf",      L"HRTF (headphones)", L"auto\0builtin\0off\0", L"auto", L"HRTF sets (*.bin;*.sofa)\0*.bin;*.sofa\0All files\0*.*\0", NULL, NULL },
    { PAGE_SOUND, T_LIST,  L"resampler", L"Resampler", L"sinc\0linear\0", L"sinc", NULL, NULL, NULL },
    { PAGE_SOUND, T_COMBO, L"outrate",   L"Output rate", L"device\0game\0" L"44100\0" L"48000\0", L"device", NULL, NULL, NULL },
    { PAGE_SOUND, T_LIST,  L"outbits",   L"Output bits", L"device\0" L"16\0", L"device", NULL, NULL, NULL },
    { PAGE_SOUND, T_EDIT,  L"latency",   L"Output buffer, ms (0: by Buffers and Duration)", NULL, L"0", NULL, NULL, NULL },
    { PAGE_SOUND, T_EDIT,  L"maxvoicecount", L"Voices at once (16 to 1024)", NULL, L"128", NULL, NULL, NULL },
    { PAGE_SOUND, T_LIST,  L"waveout",   L"waveOut through reAL", L"true\0pcm\0false\0", L"true", NULL, NULL, NULL },
    { PAGE_SOUND, T_CHECK, L"cmss",      L"CMSS-3D (stereo sound spread over the speakers)", NULL, L"false", NULL, NULL, NULL },
    { PAGE_SOUND, T_EDIT,  L"cmssintensity", L"CMSS-3D intensity (-100 to 99)", NULL, L"0", NULL, NULL, NULL },

    { PAGE_REDM,  T_CHECK, L"redm",      L"Play DirectMusic with reDM", NULL, L"false", NULL, NULL, NULL },
    { PAGE_REDM, T_CHECK, L"midi",      L"Play Windows MIDI (midiOut, MCI) with reDM", NULL, L"false", NULL, NULL, NULL },
    { PAGE_REDM, T_CHECK, L"midireverb", L"Reverb on MIDI music", NULL, L"true", NULL, NULL, NULL },
    { PAGE_REDM, T_CHECK, L"disabledirectmusic", L"Leave DirectMusic alone (ALchemy's DisableDirectMusic)", NULL, L"false", NULL, NULL, NULL },
    { PAGE_REDM, T_PATH,  L"gmdls",     L"Instruments (DLS or SoundFont)", NULL, L"", L"Instrument sets (*.dls;*.sf2)\0*.dls;*.sf2\0All files\0*.*\0",
      L"gm.dls, Windows' own", NULL },
    { PAGE_REDM, T_CHECK, L"miditrace", L"Log every MIDI and MCI call the game makes", NULL, L"false", NULL, NULL, NULL },
    { PAGE_REDM, T_COMBO, L"midirate",  L"MIDI synthesizer rate", L"22050\0auto\0" L"44100\0" L"48000\0", L"22050", NULL, NULL, NULL },

    { PAGE_REAL,  T_CHECK, L"enabled",   L"reAL enabled (off: the system's DirectSound)", NULL, L"true", NULL, NULL, NULL },
    { PAGE_REAL,  T_CHECK, L"fixes",     L"Fix what ALchemy and Microsoft's DirectSound get wrong", NULL, L"true", NULL, NULL, NULL },
    { PAGE_REAL,  T_CHECK, L"dither",    L"Dither 16-bit output", NULL, L"true", NULL, NULL, NULL },
    { PAGE_REAL,  T_LIST,  L"log",       L"Log level", L"0\0" L"1\0" L"2\0" L"3\0", L"0", NULL, NULL, NULL },
    { PAGE_REAL,  T_EDIT,  L"logfile",   L"Log file", NULL, L"", NULL, L"reAL.log, beside the game", NULL },
    { PAGE_REAL,  T_CHECK, L"log3d",     L"Log 3D calls", NULL, L"false", NULL, NULL, NULL },
    { PAGE_REAL,  T_CHECK, L"logeax",    L"Log EAX calls", NULL, L"false", NULL, NULL, NULL },
    { PAGE_REAL,  T_EDIT,  L"buffers",   L"ALchemy's Buffers (2 to 64)", NULL, L"4", NULL, NULL, NULL },
    { PAGE_REAL,  T_EDIT,  L"duration",  L"ALchemy's Duration, ms (5 to 500)", NULL, L"25", NULL, NULL, NULL },
    { PAGE_REAL,  T_PATH,  L"realdsound", L"System dsound.dll to pass to", NULL, L"", L"dsound.dll\0*.dll\0", L"the system's own", NULL },
};
#define NSET (int)(sizeof(s_set) / sizeof(*s_set))

/* what a game needs otherwise than the rest: keys of the [default] section of the reAL.ini in its folder, which
 * reAL reads after the user's.  The first choice of each leaves the key out */
#define AS_ALL L"(all games)"
static struct gameset { const WCHAR *key, *label, *options; HWND ctl; } s_gset[] = {
    { L"redm",      L"DirectMusic with reDM", L"true\0false\0", NULL },
    { L"midi",      L"Windows MIDI with reDM", L"true\0false\0", NULL },
    { L"waveout",   L"waveOut through reAL", L"true\0pcm\0false\0", NULL },
    { L"speakers",  L"Speakers", L"auto\0stereo\0headphones\0quad\0" L"5.1\0" L"7.1\0", NULL },
    { L"hrtf",      L"HRTF (headphones)", L"auto\0builtin\0off\0", NULL },
    { L"resampler", L"Resampler", L"sinc\0linear\0", NULL },
    { L"outrate",   L"Output rate", L"device\0game\0" L"44100\0" L"48000\0", NULL },
    { L"fixes",     L"reAL's fixes", L"true\0false\0", NULL },
    { L"enabled",   L"reAL enabled", L"true\0false\0", NULL },
};
#define NGSET (int)(sizeof(s_gset) / sizeof(*s_gset))

/* (always ended, whatever the length) */
static void fmt(WCHAR *out, size_t n, const WCHAR *f, ...)
{
    va_list ap;
    va_start(ap, f);
    const int len = _vsnwprintf(out, n, f, ap);
    va_end(ap);
    if (len < 0 || (size_t)len >= n) s_cut = 1;
    out[n - 1] = 0;
}
static void status(const WCHAR *f, ...)
{
    WCHAR b[600];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf(b, 600, f, ap);
    va_end(ap);
    b[599] = 0;
    SetWindowTextW(s_status, b);
}
/* "error 5" as the user needs it */
static const WCHAR *why(DWORD e)
{
    static WCHAR b[160];
    if (e == ERROR_ACCESS_DENIED) return L"access denied: is the game running, or does its folder need the configurator run as administrator?";
    if (e == ERROR_SHARING_VIOLATION) return L"the file is in use: is the game running?";
    fmt(b, 160, L"error %lu", e);
    return b;
}

static void folder_of(const WCHAR *exe, WCHAR *out)
{
    lstrcpynW(out, exe, MAX_PATH);
    WCHAR *s = wcsrchr(out, L'\\');
    if (s) *s = 0;
}
static const WCHAR *name_of(const WCHAR *exe)
{
    const WCHAR *s = wcsrchr(exe, L'\\');
    return s ? s + 1 : exe;
}
static int exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

/* reAL reads its ini files in the system's code page: can the text be written in it without loss?  (UTF-8 as the
 * code page loses nothing, and its conversion takes neither the flag nor the question) */
static int plain(const WCHAR *t)
{
    BOOL lost = FALSE;
    char b[4 * MAX_PATH];
    if (!*t || GetACP() == CP_UTF8) return 1;
    return WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, t, -1, b, sizeof(b), NULL, &lost) && !lost;
}

/* a 32-bit x86 program?  (reAL is 32-bit: its DLLs in a 64-bit game's folder would only stop the game from
 * starting.)  0: a program of another kind (64-bit, as a rule); -1: not a program at all, or unreadable */
static int is_32bit(const WCHAR *exe)
{
    BYTE h[0x40], pe[6];
    DWORD got = 0;
    int r = -1;
    HANDLE f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    if (ReadFile(f, h, sizeof(h), &got, NULL) && got == sizeof(h) && h[0] == 'M' && h[1] == 'Z' &&
        SetFilePointer(f, (LONG)(h[0x3c] | h[0x3d] << 8 | h[0x3e] << 16 | (DWORD)h[0x3f] << 24), NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        ReadFile(f, pe, sizeof(pe), &got, NULL) && got == sizeof(pe) && pe[0] == 'P' && pe[1] == 'E' && !pe[2] && !pe[3])
        r = (pe[4] | pe[5] << 8) == 0x014c;
    CloseHandle(f);
    return r;
}

/* the configurator's own version, from its version resource ("2.0.0").  0: it has none */
static int version_of(const WCHAR *path, WCHAR *out, size_t n)
{
    DWORD zero, size = GetFileVersionInfoSizeW(path, &zero);
    void *b = size ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;
    VS_FIXEDFILEINFO *v;
    UINT len;
    int ok = 0;
    if (b && GetFileVersionInfoW(path, 0, size, b) && VerQueryValueW(b, L"\\", (void **)&v, &len) && len >= sizeof(*v)) {
        fmt(out, n, L"%u.%u.%u", HIWORD(v->dwFileVersionMS), LOWORD(v->dwFileVersionMS), HIWORD(v->dwFileVersionLS));
        ok = 1;
    }
    if (b) HeapFree(GetProcessHeap(), 0, b);
    return ok;
}

/* where in a file the text `mark` is; what follows it, up to its 0, into `after` (if given).  0: not there */
static int find_mark(const WCHAR *path, const char *mark, char *after, size_t n)
{
    const size_t len = strlen(mark);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD size = GetFileSize(f, NULL), got = 0;
    int found = 0;
    char *b = size && size < (16u << 20) ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;    /* (reAL's are 2 MB) */
    if (b && ReadFile(f, b, size, &got, NULL))
        for (DWORD i = 0; i + len <= got && !found; i++) {
            if (b[i] != mark[0] || memcmp(b + i, mark, len)) continue;
            found = 1;
            if (!after) break;
            size_t k = 0;
            for (DWORD at = i + (DWORD)len; at < got && b[at] && k + 1 < n; at++) after[k++] = b[at];
            after[k] = 0;
        }
    if (b) HeapFree(GetProcessHeap(), 0, b);
    CloseHandle(f);
    return found;
}
/* is the file one of reAL's DLLs?  (Both carry the first name, the one exporting it, the other asking for it; a
 * dsound.dll from before 1.7.1 has only the second, as install.sh knows it by.) */
static int is_real(const WCHAR *path) { return find_mark(path, "reAL_winmm_bind", NULL, 0) || find_mark(path, "reAL.ini", NULL, 0); }
/* which reAL a dsound.dll is ("2.0.0"); 0: one from before 2.0, which does not say.  (Not from its version resource:
 * Wine answers for a file named dsound.dll with its own DLL's.) */
static int real_version(const WCHAR *path, WCHAR *out, size_t n)
{
    char v[32];
    if (!find_mark(path, "reAL-version:", v, sizeof(v)) || !v[0]) return 0;
    size_t k = 0;
    for (; v[k] && k + 1 < n; k++)
        out[k] = (v[k] >= '0' && v[k] <= '9') || (v[k] >= 'a' && v[k] <= 'z') || (v[k] >= 'A' && v[k] <= 'Z') || v[k] == '.' || v[k] == '-'
                 ? (WCHAR)v[k] : L'?';
    out[k] = 0;
    return 1;
}

/* ------------------------------------------------------------------ the list of games */
/* (the list is the configurator's own file, beside the user's reAL.ini; made as UTF-16, which the profile functions
 * then keep, so that a game's path can hold any letters) */
static void ini_path(WCHAR *out)
{
    fmt(out, MAX_PATH, L"%ls\\reALcfg.ini", s_appdir);
    if (exists(out)) return;
    CreateDirectoryW(s_appdir, NULL);
    HANDLE f = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put;
    WriteFile(f, "\xff\xfe", 2, &put, NULL);
    CloseHandle(f);
}
static void games_load(void)
{
    WCHAR ini[MAX_PATH], key[16];
    ini_path(ini);
    s_ngames = 0;
    for (int i = 0; i < MAXGAMES; i++) {
        fmt(key, 16, L"game%d", i);
        if (!GetPrivateProfileStringW(L"games", key, L"", s_games[s_ngames], MAX_PATH, ini)) break;
        s_ngames++;
    }
}
static void games_save(void)
{
    WCHAR ini[MAX_PATH], key[16];
    ini_path(ini);
    WritePrivateProfileStringW(L"games", NULL, NULL, ini);
    for (int i = 0; i < s_ngames; i++) {
        fmt(key, 16, L"game%d", i);
        WritePrivateProfileStringW(L"games", key, s_games[i], ini);
    }
}
static const WCHAR *state_of(const WCHAR *exe)
{
    static WCHAR b[96];
    WCHAR dir[MAX_PATH], ds[MAX_PATH], mm[MAX_PATH], v[32];
    folder_of(exe, dir);
    fmt(ds, MAX_PATH, L"%ls\\dsound.dll", dir);
    fmt(mm, MAX_PATH, L"%ls\\winmm.dll", dir);
    if (!exists(exe)) return L"game not found";
    if (is_real(ds)) {
        if (!real_version(ds, v, 32)) lstrcpyW(v, L"before 2.0");
        fmt(b, 96, L"reAL %ls%ls", v, is_real(mm) ? L", with winmm.dll" : L"");
        return b;
    }
    return exists(ds) ? L"another dsound.dll" : L"not installed";
}
static void list_fill(int select)
{
    ListView_DeleteAllItems(s_list);
    for (int i = 0; i < s_ngames; i++) {
        WCHAR dir[MAX_PATH];
        folder_of(s_games[i], dir);
        LVITEMW it = { .mask = LVIF_TEXT, .iItem = i, .pszText = (WCHAR *)name_of(s_games[i]) };
        ListView_InsertItem(s_list, &it);
        ListView_SetItemText(s_list, i, 1, (WCHAR *)state_of(s_games[i]));
        ListView_SetItemText(s_list, i, 2, dir);
    }
    if (select >= 0 && select < s_ngames) ListView_SetItemState(s_list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}
static int selected(void) { return ListView_GetNextItem(s_list, -1, LVNI_SELECTED); }

/* ------------------------------------------------------------------ settings */
/* what shows: the page's controls, and of those the ones for a game picked or for none (a hint in their place) */
static void show_page(int g)
{
    for (int i = 0; i < s_nctl; i++) {
        const int on = (!s_ctl[i].page || s_ctl[i].page == s_page) &&
                       (!s_ctl[i].when || (s_ctl[i].when == 1 ? g >= 0 : g < 0));
        ShowWindow(s_ctl[i].h, on ? SW_SHOW : SW_HIDE);
    }
}
static void fill(HWND combo, const WCHAR *options)
{
    for (const WCHAR *o = options; *o; o += lstrlenW(o) + 1) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)o);
}
static int truth(const WCHAR *v) { return !lstrcmpiW(v, L"true") || !lstrcmpiW(v, L"1") || !lstrcmpiW(v, L"yes") || !lstrcmpiW(v, L"on"); }
/* a value into a combo: its entry picked, or (not one of the list's) typed in or added */
static void combo_set(HWND c, const WCHAR *v, int typed)
{
    LRESULT at = SendMessageW(c, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)v);
    if (at == CB_ERR && typed) { SetWindowTextW(c, v); return; }
    if (at == CB_ERR) at = SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)v);
    SendMessageW(c, CB_SETCURSEL, (WPARAM)at, 0);
}

/* a key of a file's [default] section; 0: it is not there (an empty value is: reAL takes "gmdls=" as none).
 * (Through Windows' profile functions, which for a hand-written file do not read quite as reAL does: of a key
 * written twice they take the first and reAL the last, and they take quotes off a value.) */
static int ini_get(const WCHAR *ini, const WCHAR *key, WCHAR *v)
{
    GetPrivateProfileStringW(L"default", key, L"\x01", v, MAX_PATH, ini);
    if (v[0] != 1) return 1;
    v[0] = 0;
    return 0;
}
/* the same, with reDM's switch under its name before 2.0 as well (reAL takes whichever comes last; the newer name
 * is taken here, should a file have both) */
static int ini_get_old(const WCHAR *ini, const WCHAR *key, WCHAR *v)
{
    return ini_get(ini, key, v) || (!lstrcmpW(key, L"redm") && ini_get(ini, L"music", v));
}
/* a key set, or (v NULL) taken out; with redm, the old name goes */
static int ini_put(const WCHAR *ini, const WCHAR *key, const WCHAR *v)
{
    if (!lstrcmpW(key, L"redm")) WritePrivateProfileStringW(L"default", L"music", NULL, ini);
    return WritePrivateProfileStringW(L"default", key, v, ini) != 0;
}
/* a typed value as it goes into a file: no quotes around it (reAL would take them for part of it) */
static void unquote(WCHAR *v)
{
    const int n = lstrlenW(v);
    if (n >= 2 && v[0] == L'"' && v[n - 1] == L'"') { memmove(v, v + 1, (size_t)(n - 2) * sizeof(WCHAR)); v[n - 2] = 0; }
}

/* the settings for every game.  Under the user's file lies the machine's (%WINDIR%\reAL.ini, which install.sh writes
 * in a Wine prefix): what a key is there, or reAL's own default, is what holds without the user's, so that is what
 * shows for a key the user's file has not, and a key set to just that (or emptied) is not kept in it */
static void base_value(const setting *s, WCHAR *v)
{
    if (!ini_get_old(s_base, s->key, v)) lstrcpynW(v, s->def, MAX_PATH);
}
static void settings_load(void)
{
    WCHAR v[MAX_PATH];
    for (int i = 0; i < NSET; i++) {
        setting *s = &s_set[i];
        if (!ini_get_old(s_ini, s->key, v)) base_value(s, v);
        if (s->type == T_CHECK) SendMessageW(s->ctl, BM_SETCHECK, truth(v) ? BST_CHECKED : BST_UNCHECKED, 0);
        else if (s->type == T_LIST || s->type == T_COMBO) {
            SendMessageW(s->ctl, CB_RESETCONTENT, 0, 0);
            fill(s->ctl, s->options);
            combo_set(s->ctl, v, s->type == T_COMBO);
        } else SetWindowTextW(s->ctl, v);
    }
}
static void settings_save(void)
{
    WCHAR v[MAX_PATH], base[MAX_PATH], had[MAX_PATH];
    if (s_homeless) { status(L"There is no %%APPDATA%% folder to keep settings for every game in: nothing saved"); return; }
    CreateDirectoryW(s_appdir, NULL);
    for (int i = 0; i < NSET; i++) {
        setting *s = &s_set[i];
        if (s->type == T_CHECK) continue;
        GetWindowTextW(s->ctl, v, MAX_PATH);
        if (!plain(v)) {
            status(L"\"%ls\" has letters this Windows' code page cannot write, and reAL would not find it: nothing saved", v);
            return;
        }
    }
    DWORD err = 0;
    for (int i = 0; i < NSET && !err; i++) {
        setting *s = &s_set[i];
        if (s->type == T_CHECK) lstrcpyW(v, SendMessageW(s->ctl, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"true" : L"false");
        else { GetWindowTextW(s->ctl, v, MAX_PATH); unquote(v); }
        base_value(s, base);
        const int same = !v[0] || (s->type == T_CHECK ? truth(v) == truth(base) : !lstrcmpiW(v, base));
        if (same && !ini_get_old(s_ini, s->key, had)) continue;
        if (!ini_put(s_ini, s->key, same ? NULL : v)) err = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    }
    if (err) status(L"Could not write %ls (%ls)", s_ini, why(err));
    else status(L"Saved for every game: %ls", s_ini);
    settings_load();        /* (an emptied field shows what holds now) */
}
static void browse(int i)
{
    setting *s = &s_set[i];
    WCHAR file[MAX_PATH] = L"";
    OPENFILENAMEW o = { .lStructSize = sizeof(o), .hwndOwner = s_wnd, .lpstrFilter = s->filter, .lpstrFile = file, .nMaxFile = MAX_PATH,
                        .lpstrTitle = s->label, .Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR };
    if (!GetOpenFileNameW(&o)) return;
    SetWindowTextW(s->ctl, file);
    if (!plain(file)) status(L"That path has letters this Windows' code page cannot write: reAL would not find the file. Move or rename it.");
}

/* what the picked game has otherwise */
static int game_ini(int g, WCHAR *ini)
{
    WCHAR dir[MAX_PATH];
    s_cut = 0;
    folder_of(s_games[g], dir);
    fmt(ini, MAX_PATH, L"%ls\\reAL.ini", dir);
    return !s_cut;
}
/* the box says whether this game has reAL's winmm.dll */
static void refresh_winmm(int g)
{
    WCHAR dir[MAX_PATH], mm[MAX_PATH];
    folder_of(s_games[g], dir);
    fmt(mm, MAX_PATH, L"%ls\\winmm.dll", dir);
    CheckDlgButton(s_wnd, ID_WINMM, is_real(mm) ? BST_CHECKED : BST_UNCHECKED);
}
static void game_load(int g)
{
    WCHAR ini[MAX_PATH], v[MAX_PATH], title[MAX_PATH + 60];
    show_page(g);
    if (g < 0) { SetWindowTextW(s_gamebox, L"What a game needs otherwise than the rest"); return; }
    game_ini(g, ini);
    fmt(title, MAX_PATH + 60, L"What %ls needs otherwise than the rest (its reAL.ini)", name_of(s_games[g]));
    SetWindowTextW(s_gamebox, title);
    for (int i = 0; i < NGSET; i++) {
        SendMessageW(s_gset[i].ctl, CB_RESETCONTENT, 0, 0);
        SendMessageW(s_gset[i].ctl, CB_ADDSTRING, 0, (LPARAM)AS_ALL);
        fill(s_gset[i].ctl, s_gset[i].options);
        if (ini_get_old(ini, s_gset[i].key, v) && v[0]) combo_set(s_gset[i].ctl, v, 0); else SendMessageW(s_gset[i].ctl, CB_SETCURSEL, 0, 0);
    }
    refresh_winmm(g);
}
static void game_save(int g)
{
    WCHAR ini[MAX_PATH], v[MAX_PATH], had[MAX_PATH];
    if (g < 0) return;
    if (!game_ini(g, ini)) { status(L"The game's folder has too long a name"); return; }
    int any = 0;
    DWORD err = 0;
    for (int i = 0; i < NGSET && !err; i++) {
        const int all = SendMessageW(s_gset[i].ctl, CB_GETCURSEL, 0, 0) == 0;
        GetWindowTextW(s_gset[i].ctl, v, MAX_PATH);
        any |= !all;
        /* (nothing of its own to take out: the file is left as it is, or unmade) */
        if (all && !ini_get_old(ini, s_gset[i].key, had)) continue;
        if (!ini_put(ini, s_gset[i].key, all ? NULL : v)) err = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    }
    if (err) status(L"Could not write %ls (%ls)", ini, why(err));
    else if (any) status(L"Saved %ls", ini);
    else status(L"%ls is as all games are", name_of(s_games[g]));
}

/* ------------------------------------------------------------------ installing */
static const WCHAR *const s_dll[2] = { L"dsound", L"winmm" };

/* is reAL's `dll` in the folder of another game of the list whose exe has the same name as this one's? */
static int shared_name(int g, const WCHAR *dll)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    for (int i = 0; i < s_ngames; i++) {
        if (i == g || lstrcmpiW(name_of(s_games[i]), name_of(s_games[g]))) continue;
        folder_of(s_games[i], dir);
        fmt(path, MAX_PATH, L"%ls\\%ls.dll", dir, dll);
        if (is_real(path)) return 1;
    }
    return 0;
}
/* a registry key with nothing in it removed (never one that holds a value or a key: RegDeleteKey would take the
 * values with it) */
static void drop_if_empty(const WCHAR *key)
{
    HKEY k;
    DWORD values = 1, subkeys = 1;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_QUERY_VALUE, &k)) return;
    const LONG e = RegQueryInfoKeyW(k, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    if (!e && !values && !subkeys) RegDeleteKeyW(HKEY_CURRENT_USER, key);
}
/* under Wine: the game loads a DLL from its folder only with an override, which is kept by the exe's name alone.
 * One that is there already (the user's, or install.sh's) is left as it is and stays when reAL goes; one made here
 * is noted in the configurator's file and goes with reAL, unless it has been changed since or another game of the
 * same name still needs it.  Setting: 1 if one was there already that may keep reAL from loading (`was`: it) */
static int wine_override(int g, const WCHAR *dll, int on, WCHAR *was)
{
    static const WCHAR ours_is[] = L"native,builtin";
    WCHAR key[MAX_PATH + 64], note[MAX_PATH + 16], ini[MAX_PATH], v[64] = L"";
    DWORD size = sizeof(v) - sizeof(WCHAR), type = 0;
    HKEY k;
    int other = 0;
    if (!s_wine) return 0;
    ini_path(ini);
    fmt(key, MAX_PATH + 64, L"Software\\Wine\\AppDefaults\\%ls\\DllOverrides", name_of(s_games[g]));
    fmt(note, MAX_PATH + 16, L"%ls|%ls", name_of(s_games[g]), dll);
    if (on) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &k, NULL)) return 0;
        const LONG e = RegQueryValueExW(k, dll, NULL, &type, (BYTE *)v, &size);
        if (e == ERROR_FILE_NOT_FOUND) {
            RegSetValueExW(k, dll, 0, REG_SZ, (const BYTE *)ours_is, sizeof(ours_is));
            WritePrivateProfileStringW(L"overrides", note, L"1", ini);
        } else if (e || type != REG_SZ || (lstrcmpiW(v, ours_is) && lstrcmpiW(v, L"native"))) {
            other = 1;
            if (was) lstrcpynW(was, e || type != REG_SZ ? L"?" : v, 64);
        }
        RegCloseKey(k);
        return other;
    }
    if (!GetPrivateProfileIntW(L"overrides", note, 0, ini) || shared_name(g, dll)) return 0;
    WritePrivateProfileStringW(L"overrides", note, NULL, ini);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &k)) return 0;
    /* (still as it was made: changed since, it is the user's) */
    if (!RegQueryValueExW(k, dll, NULL, &type, (BYTE *)v, &size) && type == REG_SZ && !lstrcmpiW(v, ours_is)) RegDeleteValueW(k, dll);
    RegCloseKey(k);
    drop_if_empty(key);
    fmt(key, MAX_PATH + 64, L"Software\\Wine\\AppDefaults\\%ls", name_of(s_games[g]));
    drop_if_empty(key);
    return 0;
}

/* a file that may be read-only (games off a CD) made writable, to be written over or removed */
static void unlock(const WCHAR *path)
{
    const DWORD a = GetFileAttributesW(path);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(path, a & ~FILE_ATTRIBUTE_READONLY);
}

static void install(int g, int with_winmm)
{
    WCHAR dir[MAX_PATH], src[2][MAX_PATH], dst[2][MAX_PATH], bak[2][MAX_PATH], was[64] = L"";
    int todo[2], blocked = 0;
    if (g < 0) return;
    folder_of(s_games[g], dir);
    const int bits = is_32bit(s_games[g]);
    if (bits < 0) { status(L"%ls is not there, or is not a program", s_games[g]); return; }
    if (!bits) { status(L"%ls is not a 32-bit program (64-bit, as a rule). reAL is 32-bit: its DLLs would only keep the game from starting.", name_of(s_games[g])); return; }
    if (!lstrcmpiW(dir, s_dir)) { status(L"%ls is in the configurator's own folder: reAL's DLLs are beside it already", name_of(s_games[g])); return; }
    /* (everything looked at before anything is touched.  A winmm.dll of reAL's that the game has already is brought
     * up to date with dsound.dll, box or no box: the two go together) */
    s_cut = 0;
    for (int d = 0; d < 2; d++) {
        fmt(src[d], MAX_PATH, L"%ls\\%ls.dll", s_dir, s_dll[d]);
        fmt(dst[d], MAX_PATH, L"%ls\\%ls.dll", dir, s_dll[d]);
        fmt(bak[d], MAX_PATH, L"%ls.reAL-backup", dst[d]);
        todo[d] = d == 0 || with_winmm || is_real(dst[d]);
        if (!todo[d]) continue;
        if (s_cut) { status(L"The game's folder has too long a name"); return; }
        if (!is_real(src[d])) { status(L"reAL's %ls.dll is not beside the configurator (%ls)", s_dll[d], src[d]); return; }
        if (exists(dst[d]) && !is_real(dst[d]) && exists(bak[d])) {
            status(L"%ls.dll and %ls.dll.reAL-backup in %ls are both the game's own: remove one, the configurator will not choose", s_dll[d], s_dll[d], dir);
            return;
        }
    }
    for (int d = 0; d < 2; d++) {
        if (!todo[d]) continue;
        /* (a DLL of someone else's there is kept, to be put back; and put back at once if reAL's cannot be copied) */
        const int foreign = exists(dst[d]) && !is_real(dst[d]);
        if (foreign && !MoveFileW(dst[d], bak[d])) { status(L"Could not set aside the game's own %ls.dll (%ls)", s_dll[d], why(GetLastError())); return; }
        unlock(dst[d]);
        if (!CopyFileW(src[d], dst[d], FALSE)) {
            const DWORD err = GetLastError();
            if (foreign) MoveFileW(bak[d], dst[d]);
            status(L"Could not copy %ls.dll (%ls) to %ls", s_dll[d], why(err), dir);
            return;
        }
        if (wine_override(g, s_dll[d], 1, was)) blocked = d + 1;
    }
    if (blocked) status(L"Installed in %ls, but Wine has %ls=%ls set for %ls already: left as it is, and reAL may not load", dir, s_dll[blocked - 1], was, name_of(s_games[g]));
    else status(L"Installed in %ls%ls", dir, s_wine ? L" (with Wine's overrides for the game)" : L"");
}

static void uninstall(int g)
{
    WCHAR dir[MAX_PATH], dst[MAX_PATH], bak[MAX_PATH];
    int removed = 0, back = 0;
    if (g < 0) return;
    folder_of(s_games[g], dir);
    for (int d = 0; d < 2; d++) {
        s_cut = 0;
        fmt(dst, MAX_PATH, L"%ls\\%ls.dll", dir, s_dll[d]);
        fmt(bak, MAX_PATH, L"%ls.reAL-backup", dst);
        if (s_cut) { status(L"The game's folder has too long a name"); return; }
        if (is_real(dst)) {
            unlock(dst);
            if (!DeleteFileW(dst)) { status(L"Could not remove %ls.dll (%ls) from %ls", s_dll[d], why(GetLastError()), dir); return; }
            wine_override(g, s_dll[d], 0, NULL);
            removed++;
        }
        /* (the game's own, set aside: back in its place, also where an install was cut short) */
        if (!exists(dst) && exists(bak)) {
            if (!MoveFileW(bak, dst)) { status(L"Could not put the game's own %ls.dll back (%ls): it is %ls", s_dll[d], why(GetLastError()), bak); return; }
            back++;
        }
    }
    if (!removed && !back) status(L"reAL is not installed in %ls", dir);
    else status(L"Removed from %ls (its reAL.ini is left)%ls", dir, back ? L"; the game's own DLL is back" : L"");
}

/* reDM as DirectMusic's server, for a game that creates DirectMusic before it loads dsound.dll: a copy of
 * dsound.dll under the name reDM.dll registers itself, through regsvr32 (in a process of its own: reAL's DLL would
 * make itself at home in this one), which on Windows is started with the administrator's rights the machine's
 * classes need.  Its exit code says how it went */
static void redm_register(int on)
{
    WCHAR dll[MAX_PATH], redm[MAX_PATH], args[MAX_PATH + 16];
    s_cut = 0;
    fmt(dll, MAX_PATH, L"%ls\\dsound.dll", s_dir);
    fmt(redm, MAX_PATH, L"%ls\\reDM.dll", s_dir);
    fmt(args, MAX_PATH + 16, L"/s %ls\"%ls\"", on ? L"" : L"/u ", redm);
    if (s_cut) { status(L"The configurator's folder has too long a name"); return; }
    if (on) {
        if (!is_real(dll)) { status(L"reAL's dsound.dll is not beside the configurator"); return; }
        unlock(redm);
        if (!CopyFileW(dll, redm, FALSE)) { status(L"Could not make reDM.dll (%ls) in %ls", why(GetLastError()), s_dir); return; }
    } else if (!exists(redm)) { status(L"There is no reDM.dll beside the configurator: nothing was registered from here"); return; }
    SHELLEXECUTEINFOW x = { .cbSize = sizeof(x), .fMask = SEE_MASK_NOCLOSEPROCESS, .hwnd = s_wnd, .lpVerb = s_wine ? L"open" : L"runas",
                            .lpFile = L"regsvr32.exe", .lpParameters = args, .nShow = SW_HIDE };
    if (!ShellExecuteExW(&x) || !x.hProcess) {
        status(GetLastError() == ERROR_CANCELLED ? L"Not done: the administrator's rights were not given" : L"Could not start regsvr32");
        return;
    }
    DWORD code = 1;
    const DWORD waited = WaitForSingleObject(x.hProcess, 30000);
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(x.hProcess, &code);
    CloseHandle(x.hProcess);
    if (waited != WAIT_OBJECT_0) status(L"regsvr32 has not finished after half a minute: it is not known how it went");
    else if (code) status(L"regsvr32 could not %ls reDM.dll (it gave %lu)", on ? L"register" : L"unregister", code);
    else status(on ? L"%ls is DirectMusic's server now (register again after putting a newer reAL here)"
                   : L"DirectMusic's server is what it was before %ls", redm);
}

/* DirectSound made through COM (CoCreateInstance; Hot Wheels: Beat That!, for one) loads Windows' own dsound.dll
 * and never sees the one beside the game.  The two classes pointed at the reAL beside this program, for this Windows
 * user: what tools/dsound-com-redirect.reg does by hand.  It holds for every 32-bit program of the user's that does
 * the same (few do), not for programs run as administrator, and not under Wine, whose COM ignores per-user classes.
 * (This program is 32-bit: Windows keeps these keys under Wow6432Node for it, where 32-bit games look.) */
static const WCHAR *const s_clsid[2] = { L"{3901CC3F-84B5-4FA4-BA35-AA8172B8A09B}", L"{47D4D946-62E8-11CF-93BC-444553540000}" };
static void com_redirect(int on)
{
    WCHAR dll[MAX_PATH], key[160], sub[200], now[MAX_PATH], kept[MAX_PATH] = L"";
    if (s_wine) { status(L"Not under Wine: its COM ignores per-user classes (the game's DLL override does the work there)"); return; }
    s_cut = 0;
    fmt(dll, MAX_PATH, L"%ls\\dsound.dll", s_dir);
    if (s_cut) { status(L"The configurator's folder has too long a name"); return; }
    if (on && !is_real(dll)) { status(L"reAL's dsound.dll is not beside the configurator"); return; }
    LONG err = 0;
    int left = 0, gone = 0;
    for (int c = 0; c < 2; c++) {
        fmt(key, 160, L"Software\\Classes\\CLSID\\%ls", s_clsid[c]);
        fmt(sub, 200, L"%ls\\InprocServer32", key);
        if (!on) {
            /* (only what points at a reAL, or at a file that is gone: someone else's redirect is theirs, and so is
             * one that cannot be made out) */
            DWORD size = sizeof(now);
            now[0] = 0;
            const LONG got = RegGetValueW(HKEY_CURRENT_USER, sub, NULL, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, now, &size);
            if (got == ERROR_FILE_NOT_FOUND) continue;
            const int full = !got && now[0] && now[1] == L':' && now[2] == L'\\';
            if (!(full && (is_real(now) || !exists(now)))) {
                left = 1;
                lstrcpynW(kept, got ? L"something that cannot be read" : now, MAX_PATH);
                continue;
            }
            const LONG e = RegDeleteTreeW(HKEY_CURRENT_USER, key);
            if (e && e != ERROR_FILE_NOT_FOUND) err = e; else gone = 1;
            continue;
        }
        static const WCHAR *const name[2] = { L"DirectSound 8 (redirected to reAL)", L"DirectSound (redirected to reAL)" };
        HKEY k, in;
        LONG e = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
        if (!e) {
            RegSetValueExW(k, NULL, 0, REG_SZ, (const BYTE *)name[c], (lstrlenW(name[c]) + 1) * sizeof(WCHAR));
            e = RegCreateKeyExW(k, L"InprocServer32", 0, NULL, 0, KEY_WRITE, NULL, &in, NULL);
            if (!e) {
                e = RegSetValueExW(in, NULL, 0, REG_SZ, (const BYTE *)dll, (lstrlenW(dll) + 1) * sizeof(WCHAR));
                RegSetValueExW(in, L"ThreadingModel", 0, REG_SZ, (const BYTE *)L"Both", sizeof(L"Both"));
                RegCloseKey(in);
            }
            RegCloseKey(k);
        }
        if (e) err = e;
    }
    if (err) status(L"The registry would not have it (error %ld)", err);
    else if (left) status(L"DirectSound through COM points at %ls, which is not reAL's: left alone%ls", kept, gone ? L" (the other class was reAL's and is undone)" : L"");
    else if (on) status(L"DirectSound through COM now comes from %ls, for this Windows user (undo it before moving or removing that folder)", dll);
    else status(L"DirectSound through COM is Windows' own again");
}

static void add_game(void)
{
    WCHAR file[MAX_PATH] = L"";
    OPENFILENAMEW o = { .lStructSize = sizeof(o), .hwndOwner = s_wnd, .lpstrFilter = L"Games (*.exe)\0*.exe\0All files\0*.*\0",
                        .lpstrFile = file, .nMaxFile = MAX_PATH, .lpstrTitle = L"Pick the game's exe",
                        .Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR };
    if (!GetOpenFileNameW(&o)) return;
    for (int i = 0; i < s_ngames; i++) if (!lstrcmpiW(s_games[i], file)) { list_fill(i); return; }
    if (s_ngames == MAXGAMES) { status(L"The list is full"); return; }
    if (is_32bit(file) == 0) { status(L"%ls is not a 32-bit program (64-bit, as a rule): reAL is 32-bit and cannot serve it", name_of(file)); return; }
    lstrcpynW(s_games[s_ngames++], file, MAX_PATH);
    games_save();
    list_fill(s_ngames - 1);
}

/* ------------------------------------------------------------------ the window */
static HWND make(const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id)
{
    const int edged = !lstrcmpW(cls, WC_LISTVIEWW) || !lstrcmpW(cls, L"EDIT");
    HWND c = CreateWindowExW(edged ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             s_wnd, (HMENU)(INT_PTR)id, s_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    if (s_dark) SetWindowTheme(c, L"", L"");        /* (see wWinMain) */
    if (s_nctl < (int)(sizeof(s_ctl) / sizeof(*s_ctl))) { s_ctl[s_nctl].h = c; s_ctl[s_nctl].page = s_mk_page; s_ctl[s_nctl++].when = s_mk_when; }
    return c;
}
/* (in the layout's units, as make() takes them) */
static int text_width(const WCHAR *t)
{
    HDC dc = GetDC(s_wnd);
    HFONT old = SelectObject(dc, s_font);
    SIZE z = { 0, 0 };
    GetTextExtentPoint32W(dc, t, lstrlenW(t), &z);
    SelectObject(dc, old);
    ReleaseDC(s_wnd, dc);
    return MulDiv(z.cx, 96, s_dpi) + 1;
}

#define TOP 114         /* where a page starts, under the tabs */
#define ROW 30

static void create(void)
{
    NONCLIENTMETRICSW m = { .cbSize = sizeof(m) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(m), &m, 0);
    m.lfMessageFont.lfHeight = MulDiv(m.lfMessageFont.lfHeight, s_dpi, s_sysdpi);
    s_font = CreateFontIndirectW(&m.lfMessageFont);
    LOGFONTW lf = m.lfMessageFont;
    lf.lfHeight = -S(44); lf.lfWeight = FW_BOLD;
    s_logo = CreateFontIndirectW(&lf);

    /* the tabs are a strip; each page's controls lie under it */
    s_mk_page = 0; s_mk_when = 0;
    s_tab = make(WC_TABCONTROLW, L"", WS_TABSTOP | TCS_FOCUSNEVER, 12, 80, 776, 24, ID_TAB);
    static const WCHAR *const tabs[NPAGES] = { L"Games", L"Sound", L"reDM", L"reAL" };
    for (int i = 0; i < NPAGES; i++) {
        TCITEMW t = { .mask = TCIF_TEXT, .pszText = (WCHAR *)tabs[i] };
        SendMessageW(s_tab, TCM_INSERTITEMW, i, (LPARAM)&t);
    }

    /* Games: the list, installing, and the picked game's own settings */
    s_mk_page = PAGE_GAMES;
    s_list = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, 12, TOP, 776, 132, ID_LIST);
    ListView_SetExtendedListViewStyle(s_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    static const struct { const WCHAR *t; int w; } col[3] = { { L"Game", 150 }, { L"reAL", 200 }, { L"Folder", 396 } };
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW c = { .mask = LVCF_TEXT | LVCF_WIDTH, .pszText = (WCHAR *)col[i].t, .cx = S(col[i].w) };
        ListView_InsertColumn(s_list, i, &c);
    }
    make(L"BUTTON", L"Add game...", WS_TABSTOP, 12, 254, 96, 28, ID_ADD);
    make(L"BUTTON", L"Remove from list", WS_TABSTOP, 114, 254, 120, 28, ID_REMOVE);
    make(L"BUTTON", L"Install reAL", WS_TABSTOP, 256, 254, 100, 28, ID_INSTALL);
    make(L"BUTTON", L"Uninstall", WS_TABSTOP, 362, 254, 84, 28, ID_UNINSTALL);
    make(L"BUTTON", L"with winmm.dll (waveOut games)", WS_TABSTOP | BS_AUTOCHECKBOX, 458, 258, text_width(L"with winmm.dll (waveOut games)") + 28, 20, ID_WINMM);
    s_gamebox = make(L"BUTTON", L"", BS_GROUPBOX, 12, 292, 776, 176, ID_GROUP);
    s_mk_when = 2;
    make(L"STATIC", L"Add a game and pick it in the list. What it needs otherwise than the other games shows here; the settings for "
         L"all games are on the other pages.", SS_LEFT, 28, 318, 744, 36, -1);
    s_mk_when = 1;
    {
        int labelw = 0;
        for (int i = 0; i < NGSET; i++) if (text_width(s_gset[i].label) + 10 > labelw) labelw = text_width(s_gset[i].label) + 10;
        const int pitch = 744 / 3, combow = pitch - labelw - 14 < 80 ? 80 : pitch - labelw - 14;
        for (int i = 0; i < NGSET; i++) {
            const int x = 28 + (i / 3) * pitch, y = 316 + (i % 3) * ROW;
            make(L"STATIC", s_gset[i].label, SS_LEFT, x, y + 4, labelw, 18, -1);
            s_gset[i].ctl = make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x + labelw, y, combow, 200, ID_GAME0 + i);
            SendMessageW(s_gset[i].ctl, CB_ADDSTRING, 0, (LPARAM)AS_ALL);
            fill(s_gset[i].ctl, s_gset[i].options);
        }
        make(L"BUTTON", L"Save for this game", WS_TABSTOP, 646, 428, 130, 28, ID_SAVEGAME);
    }

    /* Sound, reDM, reAL: the settings for every game, a row each in two columns (the checkboxes after the rest), as
     * wide as the labels are in the system's font */
    s_mk_when = 0;
    for (int page = PAGE_SOUND; page <= PAGE_REAL; page++) {
        s_mk_page = page;
        int n = 0, labelw = 0, top = TOP + 26;
        for (int i = 0; i < NSET; i++) {
            if (s_set[i].page != page) continue;
            n++;
            if (s_set[i].type != T_CHECK && text_width(s_set[i].label) + 10 > labelw) labelw = text_width(s_set[i].label) + 10;
        }
        const int rows = (n + 1) / 2, height = 26 + rows * ROW + 46 + (page == PAGE_REAL ? 22 : 0);
        make(L"BUTTON", L"", BS_GROUPBOX, 12, TOP, 776, height, -1);
        /* (said once, at the foot of each of these pages, and not in every box) */
        make(L"STATIC", L"These settings are for every game. What one game needs otherwise is on the Games page.", SS_LEFT, 12, 496, 776, 18, -1);
        if (page == PAGE_REAL) {
            make(L"STATIC", L"Seldom touched. Without the fixes reAL behaves as ALchemy and Microsoft's DirectSound do, which is for "
                 L"comparing with them.", SS_LEFT, 28, top - 2, 744, 18, -1);
            top += 22;
        }
        int k = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < NSET; i++) {
                setting *s = &s_set[i];
                if (s->page != page || (s->type == T_CHECK) != pass) continue;
                const int x = 28 + (k / rows) * 372, y = top + (k % rows) * ROW, cw = 356 - labelw - (s->filter ? 34 : 0);
                k++;
                if (s->type == T_CHECK) {
                    s->ctl = make(L"BUTTON", s->label, WS_TABSTOP | BS_AUTOCHECKBOX, x, y + 2, 356, 20, ID_SET0 + i);
                    continue;
                }
                make(L"STATIC", s->label, SS_LEFT, x, y + 4, labelw, 18, -1);
                if (s->type == T_LIST || s->type == T_COMBO) {
                    s->ctl = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | (s->type == T_LIST ? CBS_DROPDOWNLIST : CBS_DROPDOWN | CBS_AUTOHSCROLL),
                                  x + labelw, y, cw, 200, ID_SET0 + i);
                    fill(s->ctl, s->options);
                } else {
                    s->ctl = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, x + labelw, y, cw, 23, ID_SET0 + i);
                    if (s->cue) SendMessageW(s->ctl, EM_SETCUEBANNER, TRUE, (LPARAM)s->cue);
                }
                if (s->filter) make(L"BUTTON", L"...", WS_TABSTOP, x + labelw + cw + 4, y - 1, 30, 25, ID_BROWSE0 + i);
            }
        make(L"BUTTON", L"Save", WS_TABSTOP, 676, TOP + height - 40, 100, 28, ID_SAVE);

        if (page != PAGE_REAL) continue;
        /* the two COM registrations (the user's, whichever the game), their buttons after the longer of the labels */
        static const WCHAR *const l1 = L"DirectSound made through COM, from the reAL beside this program:",
                           *const l2 = L"DirectMusic made before dsound.dll is loaded (reDM.dll):";
        const int lw = (text_width(l1) > text_width(l2) ? text_width(l1) : text_width(l2)) + 12, y = TOP + height + 8;
        make(L"BUTTON", L"Registered for this Windows user", BS_GROUPBOX, 12, y, 776, 96, -1);
        make(L"STATIC", l1, SS_LEFT, 28, y + 28, lw, 18, -1);
        EnableWindow(make(L"BUTTON", L"Redirect", WS_TABSTOP, 28 + lw, y + 22, 84, 28, ID_COM), !s_wine);
        EnableWindow(make(L"BUTTON", L"Undo", WS_TABSTOP, 118 + lw, y + 22, 84, 28, ID_UNCOM), !s_wine);
        if (s_wine) make(L"STATIC", L"(Windows only)", SS_LEFT, 212 + lw, y + 28, 120, 18, -1);
        make(L"STATIC", l2, SS_LEFT, 28, y + 60, lw, 18, -1);
        make(L"BUTTON", L"Register", WS_TABSTOP, 28 + lw, y + 54, 84, 28, ID_REG);
        make(L"BUTTON", L"Unregister", WS_TABSTOP, 118 + lw, y + 54, 84, 28, ID_UNREG);
    }
    s_mk_page = 0;
    s_status = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 12, 520, 776, 36, ID_STATUS);    /* (two lines) */

    SendMessageW(s_tab, TCM_SETCURSEL, (WPARAM)(s_page - 1), 0);
    games_load();
    list_fill(s_ngames ? 0 : -1);
    settings_load();
    game_load(selected());
    status(L"The DLLs installed are the ones beside this program: %ls", s_dir);
}

/* the header: the wordmark (re in ink, AL in marine blue, a bar under it) and what this is */
static void paint(HWND w)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT band = { 0, 0, S(800), S(72) };
    const COLORREF ink = GetSysColor(COLOR_WINDOWTEXT), accent = s_dark ? BLUE_LIGHT : BLUE;
    HBRUSH blue = CreateSolidBrush(accent);
    FillRect(dc, &band, GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    HFONT old = SelectObject(dc, s_logo);
    SIZE re;
    GetTextExtentPoint32W(dc, L"re", 2, &re);
    SIZE al;
    GetTextExtentPoint32W(dc, L"AL", 2, &al);
    SetTextColor(dc, ink);
    TextOutW(dc, S(16), S(4), L"re", 2);
    SetTextColor(dc, accent);
    TextOutW(dc, S(16) + re.cx, S(4), L"AL", 2);
    RECT bar = { S(16), S(60), S(16) + re.cx + al.cx, S(64) };
    FillRect(dc, &bar, blue);
    SelectObject(dc, s_font);
    SetTextColor(dc, ink);
    WCHAR what[64];
    fmt(what, 64, L"Configurator %ls", s_version);
    TextOutW(dc, S(40) + re.cx + al.cx, S(20), what, lstrlenW(what));
    const WCHAR *where = s_wine ? L"Running under Wine: installing also sets the game's DLL overrides" : L"Running on Windows";
    TextOutW(dc, S(40) + re.cx + al.cx, S(40), where, lstrlenW(where));
    SelectObject(dc, old);
    DeleteObject(blue);
    EndPaint(w, &ps);
}

static LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: s_wnd = w; create(); return 0;
    case WM_PAINT: paint(w); return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->idFrom == ID_TAB && ((NMHDR *)lp)->code == TCN_SELCHANGE) {
            s_page = (int)SendMessageW(s_tab, TCM_GETCURSEL, 0, 0) + 1;
            show_page(selected());
            if (!IsWindowVisible(GetFocus())) SetFocus(s_tab);      /* (the keyboard not left on a page that is gone) */
            return 0;
        }
        if (((NMHDR *)lp)->idFrom == ID_LIST && ((NMHDR *)lp)->code == LVN_ITEMCHANGED &&
            (((NMLISTVIEW *)lp)->uChanged & LVIF_STATE)) game_load(selected());
        return 0;
    case WM_COMMAND: {
        const int g = selected(), id = LOWORD(wp);
        if (id >= ID_BROWSE0 && id < ID_BROWSE0 + NSET) { browse(id - ID_BROWSE0); return 0; }
        switch (id) {
        case ID_ADD: add_game(); break;
        case ID_REMOVE:
            if (g < 0) break;
            memmove(s_games[g], s_games[g + 1], (size_t)(s_ngames - g - 1) * sizeof(s_games[0]));
            s_ngames--;
            games_save();
            list_fill(g < s_ngames ? g : s_ngames - 1);
            game_load(selected());
            status(L"Taken off the list (nothing in the game's folder was touched)");
            break;
        case ID_INSTALL:
            if (g < 0) { status(L"Pick a game first"); break; }
            install(g, IsDlgButtonChecked(w, ID_WINMM) == BST_CHECKED);
            ListView_SetItemText(s_list, g, 1, (WCHAR *)state_of(s_games[g]));
            refresh_winmm(g);
            break;
        case ID_UNINSTALL:
            if (g < 0) { status(L"Pick a game first"); break; }
            uninstall(g);
            ListView_SetItemText(s_list, g, 1, (WCHAR *)state_of(s_games[g]));
            refresh_winmm(g);
            break;
        case ID_SAVE: settings_save(); break;
        case ID_SAVEGAME: game_save(g); break;
        case ID_REG: redm_register(1); break;
        case ID_UNREG: redm_register(0); break;
        case ID_COM: com_redirect(1); break;
        case ID_UNCOM: com_redirect(0); break;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_SYSCOLORCHANGE:
        for (int i = 0; i < s_nctl; i++) SendMessageW(s_ctl[i].h, WM_SYSCOLORCHANGE, wp, lp);
        InvalidateRect(w, NULL, TRUE);
        return 0;
    /* (the control that had the keyboard has it again when the window comes back) */
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) s_focus = GetFocus();
        else if (!HIWORD(wp) && s_focus && IsWindowVisible(s_focus)) { SetFocus(s_focus); return 0; }
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    (void)prev;
    /* (/sound, /redm, /real: to open on that page) */
    if (cmd && !lstrcmpiW(cmd, L"/sound")) s_page = PAGE_SOUND;
    else if (cmd && !lstrcmpiW(cmd, L"/redm")) s_page = PAGE_REDM;
    else if (cmd && !lstrcmpiW(cmd, L"/real")) s_page = PAGE_REAL;
    s_inst = inst;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);
    GetModuleFileNameW(NULL, s_dir, MAX_PATH);
    s_dir[MAX_PATH - 1] = 0;
    WCHAR *s = wcsrchr(s_dir, L'\\');
    if (s) *s = 0;
    WCHAR appdata[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    /* (without one the list of games is kept beside the program, and settings for every game cannot be) */
    if (n && n < MAX_PATH - 24) fmt(s_appdir, MAX_PATH, L"%ls\\reAL", appdata);
    else { lstrcpynW(s_appdir, s_dir, MAX_PATH); s_homeless = 1; }
    fmt(s_ini, MAX_PATH, L"%ls\\reAL.ini", s_appdir);
    WCHAR win[MAX_PATH] = L"", self[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH - 12);
    fmt(s_base, MAX_PATH, L"%ls\\reAL.ini", win);
    GetModuleFileNameW(NULL, self, MAX_PATH);
    self[MAX_PATH - 1] = 0;
    if (!version_of(self, s_version, 32)) s_version[0] = 0;
    s_wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != NULL;
    const COLORREF face = GetSysColor(COLOR_BTNFACE);
    s_dark = GetRValue(face) * 3 + GetGValue(face) * 6 + GetBValue(face) < 1280;       /* (half of full brightness) */
    /* (a dark colour scheme: the visual styles would draw light controls on it; the plain ones follow the colours) */
    if (s_dark) SetThemeAppProperties(STAP_ALLOW_NONCLIENT);

    /* (the manifest says the program sizes itself for the screen's scale: Windows then does not stretch it, blurred) */
    HDC screen = GetDC(NULL);
    s_dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(NULL, screen);
    if (s_dpi < 96) s_dpi = 96;
    s_sysdpi = s_dpi;
    /* (the window cannot be resized: on a screen too small for it at this scale, a smaller scale that fits) */
    RECT area = { 0, 0, 0, 0 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    while (s_dpi > 96 && area.bottom > area.top &&
           (S(564) + S(40) > area.bottom - area.top || S(800) + S(16) > area.right - area.left)) s_dpi -= 12;
    WNDCLASSW wc = { .lpfnWndProc = proc, .hInstance = inst, .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
                     .hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)),
                     .hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1), .lpszClassName = L"reALcfg" };
    RegisterClassW(&wc);
    RECT r = { 0, 0, S(800), S(564) };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    HWND w = CreateWindowW(L"reALcfg", L"reAL Configurator", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           NULL, NULL, inst, NULL);
    if (!w) return 1;
    ShowWindow(w, show);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(w, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
