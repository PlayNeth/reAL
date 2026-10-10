/* reAL-Music COM objects: DirectMusic's loader, performance, segment and friends, created in place of Windows' own
 * when the application asks COM for them (see dllmain.c's CoCreateInstance hook). */
#ifndef REAL_DMCOM_H
#define REAL_DMCOM_H
#include <windows.h>
HRESULT rm_create(REFCLSID clsid, REFIID riid, void **out);
BOOL rm_handles(REFCLSID clsid);
#endif
