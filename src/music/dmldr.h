/* reDM: DirectMusic's loader (dmloader.dll's CLoader) and container (CContainer), and the pieces of them the other
 * objects use: the loader's streams (IDirectMusicGetLoader), descriptors, and reading a form from a stream. */
#ifndef REAL_DMLDR_H
#define REAL_DMLDR_H
#define COBJMACROS
#include <windows.h>
#include <dmusici.h>

HRESULT rm_loader_create(REFIID riid, void **out);
HRESULT rm_container_create(REFIID riid, void **out);

/* the loader of a stream the loader gave (IDirectMusicGetLoader; NULL for a stream of the application's own) */
IDirectMusicLoader8 *rm_stream_loader(IStream *s);
/* the file a stream of the loader's reads (NULL for any other) */
const WCHAR *rm_stream_path(IStream *s);
/* the form at the stream's position ('RIFF'/'LIST' chunk with its header; anything else to the end of the stream) */
BOOL rm_read_form(IStream *s, BYTE **data, DWORD *size);

/* the DirectMusic objects' descriptor (dmime 7731342b / 77313530 and the same in every DLL): GetDescriptor gives
 * what the object keeps with its class; SetDescriptor keeps the object GUID, name, category, file name, version,
 * date (and the loaded flag where `loaded`), and answers S_FALSE, leaving the descriptor's flags to what it took,
 * when given more */
HRESULT rm_desc_get(const DMUS_OBJECTDESC *mine, REFCLSID cls, DMUS_OBJECTDESC *out);
HRESULT rm_desc_set(DMUS_OBJECTDESC *mine, DMUS_OBJECTDESC *in, BOOL loaded);
/* ParseDescriptor of a RIFF form `form` (dmime 773136a4 / 77312ba4): 'guid', 'date', 'cat ', 'vers' and the
 * UNFO list's UNAM; the descriptor's flags start from none, then the class; E_FAIL for another form */
HRESULT rm_desc_parse(IStream *s, DMUS_OBJECTDESC *d, DWORD form, REFCLSID cls);
/* the same from a form in memory (the data after the form type) */
void rm_desc_from_form(const BYTE *p, DWORD n, DMUS_OBJECTDESC *d);

#endif
