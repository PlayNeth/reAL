#define COBJMACROS
#include <windows.h>
#include <dmusici.h>
#include <dsound.h>
#include <stdio.h>
/* like MC2: dsound loaded dynamically, DirectMusic created, DirectSound released + FreeLibrary, reloaded */
int main(int argc,char**argv){ CoInitialize(NULL);
 HMODULE h=LoadLibraryA("dsound.dll"); HRESULT (WINAPI*dc)(LPCGUID,IDirectSound**,IUnknown*)=(void*)GetProcAddress(h,"DirectSoundCreate");
 IDirectSound *ds; dc(NULL,&ds,NULL);
 IDirectMusicPerformance8 *pf; HRESULT hr=CoCreateInstance(&CLSID_DirectMusicPerformance,NULL,CLSCTX_INPROC,&IID_IDirectMusicPerformance8,(void**)&pf); printf("perf %08lx\n",hr);
 hr=IDirectMusicPerformance8_InitAudio(pf,NULL,NULL,NULL,DMUS_APATH_SHARED_STEREOPLUSREVERB,128,32,NULL); printf("init %08lx\n",hr);
 IDirectSound_Release(ds); FreeLibrary(h); printf("freed\n");
 h=LoadLibraryA("dsound.dll"); dc=(void*)GetProcAddress(h,"DirectSoundCreate"); dc(NULL,&ds,NULL); printf("reloaded\n");
 Sleep(2000); IDirectMusicPerformance8_CloseDown(pf); IDirectMusicPerformance8_Release(pf); IDirectSound_Release(ds); printf("done\n"); return 0;}
