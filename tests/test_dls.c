/* test_dls <file.dls> <out.bin>: download every instrument (enumeration order) the way dmgt synth does and write the
 * buffers in DMGT_DLDUMP format ('DMDL', size, buffer), for comparison with dmusic's (tools/dmgt/dlcanon.py) */
#include "../src/music/dmdls.h"
#include <stdio.h>
#include <stdlib.h>

static FILE *g_out;
static int sink_dl(void *ctx, const void *buf, uint32_t size, void **h)
{
    uint32_t tag = 0x4c444d44;
    fwrite(&tag, 4, 1, g_out); fwrite(&size, 4, 1, g_out); fwrite(buf, 1, size, g_out);
    *h = (void *)1;
    return 0;
}
static void sink_ul(void *ctx, void *h) {}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) return 1;
    fclose(f);
    dm_collection *c = dls_load(b, n, 0);
    if (!c) { fprintf(stderr, "load failed\n"); return 1; }
    g_out = fopen(argv[2], "wb");
    dls_sink s = { sink_dl, sink_ul };
    dls_port *p = dls_port_new(&s, NULL);
    uint32_t patch;
    for (int i = 0; dls_enum(c, i, &patch, NULL) == 0; i++) {
        int hr;
        if (!dls_port_download(p, c, patch, 0, 0, NULL, 0, &hr)) fprintf(stderr, "instrument %d: %08x\n", i, hr);
    }
    fclose(g_out);
    return 0;
}
