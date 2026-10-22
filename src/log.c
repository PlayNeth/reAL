#include "real.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static CRITICAL_SECTION s_log_cs;
static FILE *s_log;
static int s_inited;

void rl_log_init(void)
{
    if (s_inited) return;
    InitializeCriticalSection(&s_log_cs);
    s_inited = 1;
    if (g_cfg.log_level > 0 || g_cfg.log_3d || g_cfg.log_eax) {
        /* a second copy of reAL in the same process (e.g. also installed as the system dsound.dll) appends
         * instead of truncating the first one's log */
        char name[64];
        snprintf(name, sizeof(name), "Local\\reAL.log.%lu", (unsigned long)GetCurrentProcessId());
        HANDLE m = CreateMutexA(NULL, FALSE, name);    /* (kept open: a load after a FreeLibrary appends too) */
        int second = m && GetLastError() == ERROR_ALREADY_EXISTS;
        s_log = fopen(g_cfg.log_file, second ? "a" : "w");
        /* buffered: the 3D and call logs run to thousands of lines a second, often from inside the device lock, and a
         * write per line (a syscall each, slow under Wine) stalled the game thread and the mixer.  Lines are flushed
         * at most every 100 ms; info and crash lines (levels 0 and 1) are flushed at once. */
        if (s_log) setvbuf(s_log, NULL, _IOFBF, 1 << 16);
    }
}

void rl_log(int level, const char *fmt, ...)
{
    if (!s_inited || !s_log) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n] = 0;

    EnterCriticalSection(&s_log_cs);
    fprintf(s_log, "[%8lu %04lx] %s\n", (unsigned long)GetTickCount(),
            (unsigned long)GetCurrentThreadId(), buf);
    static DWORD last;
    DWORD now = GetTickCount();
    if (level <= 1 || now - last >= 100) { fflush(s_log); last = now; }
    LeaveCriticalSection(&s_log_cs);
}

void rl_log_flush(void)
{
    if (!s_inited || !s_log) return;
    EnterCriticalSection(&s_log_cs);
    fflush(s_log);
    LeaveCriticalSection(&s_log_cs);
}

const char *rl_guid_str(const GUID *g, char buf[64])
{
    if (!g) return "(null)";
    snprintf(buf, 64, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
             (unsigned long)g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
             g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return buf;
}

/* ---------------------------------------------------------------- crash report
 * Faults are logged with module+offset and the return addresses found on the stack, so a crash in the field can
 * be located from reAL.log alone. */
static void addr_str(DWORD_PTR a, char *out, int n)
{
    HMODULE m = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m &&
        GetModuleFileNameA(m, path, MAX_PATH)) {
        const char *b = strrchr(path, '\\');
        snprintf(out, n, "%s+%lx", b ? b + 1 : path, (unsigned long)(a - (DWORD_PTR)m));
    } else snprintf(out, n, "%08lx", (unsigned long)a);
}

static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *ep)
{
    static LONG count;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!s_log) return EXCEPTION_CONTINUE_SEARCH;
    /* this sees first-chance exceptions too, some of which the game or Wine handle (probing reads, SEH):
     * report each address once so a repeated handled one does not use up the budget before a real crash */
    static void *seen[16];
    static LONG nseen;
    void *at = ep->ExceptionRecord->ExceptionAddress;
    for (LONG i = 0; i < nseen && i < 16; i++) if (seen[i] == at) return EXCEPTION_CONTINUE_SEARCH;
    LONG k = InterlockedIncrement(&nseen) - 1;
    if (k < 16) seen[k] = at;
    if (InterlockedIncrement(&count) > 6) return EXCEPTION_CONTINUE_SEARCH;
    char a[MAX_PATH + 32];
    addr_str((DWORD_PTR)ep->ExceptionRecord->ExceptionAddress, a, sizeof(a));
    CONTEXT *c = ep->ContextRecord;
#ifdef _M_IX86
    rl_log(0, "EXCEPTION %08lx (first chance) at %s (access %lu %08lx) eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
           code, a, (unsigned long)ep->ExceptionRecord->ExceptionInformation[0], (unsigned long)ep->ExceptionRecord->ExceptionInformation[1],
           c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    DWORD *sp = (DWORD *)c->Esp;
    int found = 0;
    for (int i = 0; i < 512 && found < 16; i++) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(sp + i, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) break;
        DWORD v = sp[i];
        if (VirtualQuery((void *)v, &mi, sizeof(mi)) && mi.State == MEM_COMMIT && (mi.Protect & 0xF0) && mi.Type == MEM_IMAGE) {
            addr_str(v, a, sizeof(a));
            rl_log(0, "  stack+%03x: %s", i * 4, a);
            found++;
        }
    }
#else
    (void)c;
    rl_log(0, "EXCEPTION %08lx at %s", code, a);
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

static PVOID s_veh;

void rl_crash_logger(void)
{
    if (!s_veh) s_veh = AddVectoredExceptionHandler(0, crash_handler);
}

/* the DLL going away.  The exception handler is the process's, not the DLL's: left registered after a FreeLibrary
 * (Need for Speed III frees and loads dsound.dll for each sound setting in its menu), the next exception anywhere in
 * the game - one it handles itself included - calls into unmapped memory and kills it, with nothing logged.  A
 * FreeLibrary also closes the log (the next load appends to it, see rl_log_init).  At process exit the other threads
 * are already gone and one may have died holding the log's lock, or the C runtime's lock on the file inside fprintf
 * (every write is under the log's lock): the last lines are flushed only if the lock is free, so exit cannot hang. */
void rl_log_shutdown(int process_exit)
{
    if (s_veh) { RemoveVectoredExceptionHandler(s_veh); s_veh = NULL; }
    if (!s_inited || !s_log) return;
    if (process_exit) {
        if (TryEnterCriticalSection(&s_log_cs)) { fflush(s_log); LeaveCriticalSection(&s_log_cs); }
        return;
    }
    EnterCriticalSection(&s_log_cs);
    fclose(s_log);
    s_log = NULL;
    LeaveCriticalSection(&s_log_cs);
}
