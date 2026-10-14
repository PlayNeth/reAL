/* reDM: the tool graph object (dmime's CGraph, CLSID_DirectMusicGraph) */
#ifndef REAL_DMGRAPH_H
#define REAL_DMGRAPH_H
#include <windows.h>
#include <dmusici.h>

HRESULT rm_graph_create(REFIID riid, void **out);
/* a graph of ours (not an application's own implementation of IDirectMusicGraph) */
BOOL rm_graph_ours(IDirectMusicGraph *g);
/* 77303845: a copy for a segment state: the same tools, IDirectMusicTool8 ones cloned (ours only) */
HRESULT rm_graph_clone(IDirectMusicGraph *g, IDirectMusicGraph **out);
/* 77302ace: GetObjectInPath's tool stages: the dwIndex'th tool of the class (GUID_All_Objects: any) on the PChannel */
HRESULT rm_graph_find_tool(IDirectMusicGraph *g, DWORD pch, REFGUID cls, DWORD index, REFIID iid, void **out);
/* 77303a59: load a tool graph from a stream at its RIFF 'DMTG' chunk */
HRESULT rm_graph_load(IDirectMusicGraph *g, IStream *s);

#endif
