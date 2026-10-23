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
rm_port *rm_port_of(IDirectMusicPort *i);   /* one of reDM's ports, or NULL */
dm_host *rm_port_own_host(rm_port *p);      /* a CreatePort'ed port's own synth; NULL for a performance's */
DWORD rm_port_groups(rm_port *p);           /* GetNumChannelGroups */
/* IDirectMusic::CreatePort and CreateMusicBuffer */
HRESULT rm_port_create(REFCLSID cls, DMUS_PORTPARAMS *params, IDirectMusicPort **out, IUnknown *outer);
HRESULT rm_mbuf_create(const DMUS_BUFFERDESC *d, IDirectMusicBuffer **out, IUnknown *outer);
/* (dmcom.c) one of reDM's instrument objects: its collection (a reference), its patch there, the patch it downloads
 * as and its serial; 0 for another object */
int rm_instrument_get(IDirectMusicInstrument *i, dm_collection **c, uint32_t *orig, uint32_t *patch, LONG *serial);

#endif
