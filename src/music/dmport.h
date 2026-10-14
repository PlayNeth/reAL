/* reDM: the synth port object and the sink object (see dmport.c) */
#ifndef REAL_DMPORT_H
#define REAL_DMPORT_H
#include <windows.h>
#include <dmusici.h>
#include "dmhost.h"

typedef struct rm_port rm_port;
extern const GUID RM_GUID_SynthPort;
rm_port *rm_port_new(dm_host *h);
void rm_port_detach(rm_port *p);            /* the performance closed down: the host is gone */
void rm_port_release(rm_port *p);
HRESULT rm_port_query(rm_port *p, REFIID riid, void **out);
HRESULT rm_port_sink(rm_port *p, REFIID riid, void **out);   /* the sink stage's object */
BOOL rm_port_is(IDirectMusicPort *i, rm_port *p);

#endif
