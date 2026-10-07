#include "real.h"
#include <stdio.h>
#include <stdarg.h>

static CRITICAL_SECTION s_log_cs;
static FILE *s_log;
static int s_inited;

void rl_log_init(void)
{
    if (s_inited) return;
    InitializeCriticalSection(&s_log_cs);
    s_inited = 1;
    if (g_cfg.log_level > 0 || g_cfg.log_3d || g_cfg.log_eax)
        s_log = fopen(g_cfg.log_file, "w");
}

void rl_log(int level, const char *fmt, ...)
{
    (void)level;
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
