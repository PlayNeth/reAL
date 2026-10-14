/* reDM: SoundFont 2 banks, converted to DLS Level 2 (see sf2dls.c) */
#ifndef REAL_SF2DLS_H
#define REAL_SF2DLS_H
#include <stddef.h>
#include <stdint.h>
/* a DLS file (malloc'd) for the soundfont, or NULL if it is not one; *ninstr: presets converted */
void *sf2_to_dls(const void *data, size_t size, size_t *outsize, int *ninstr);
#endif
