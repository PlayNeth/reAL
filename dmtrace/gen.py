"""Generates dmtrace_gen.c: logging wrappers for every DirectMusic interface in the MinGW headers.
usage: python3 gen.py <mingw include dir> > dmtrace_gen.c"""
import re, sys
inc = sys.argv[1]
src = ''
for h in ('dmusicc.h', 'dmusici.h'):
    src += open(f'{inc}/{h}', encoding='latin1').read() + '\n'

# ---- GUID names
guids = []
gsrc = src + open(f'{inc}/dmusics.h', encoding='latin1').read() + open(f'{inc}/dmksctrl.h', encoding='latin1').read() if False else src + open(f'{inc}/dmusics.h', encoding='latin1').read()
for m in re.finditer(r'DEFINE_GUID\s*\(\s*(\w+)\s*,([^)]*)\)', gsrc):
    name, vals = m.group(1), [v.strip() for v in m.group(2).split(',')]
    if len(vals) == 11: guids.append((name, vals))
aliases = dict(re.findall(r'#define\s+(IID_\w+)\s+(IID_\w+)', src))

# ---- interfaces
ifaces = {}
order = []
for m in re.finditer(r'DECLARE_INTERFACE_\((\w+),\s*(\w+)\)\s*\{(.*?)\n\};', src, re.S):
    name, base, body = m.group(1), m.group(2), m.group(3)
    meths = []
    for mm in re.finditer(r'STDMETHOD(?:_\(\s*([\w ]+?)\s*,\s*(\w+)\s*\)|\((\w+)\))\s*\(\s*THIS_?\s*(.*?)\)\s*PURE\s*;', body, re.S):
        ret = mm.group(1) or 'HRESULT'
        mname = mm.group(2) or mm.group(3)
        args = mm.group(4).strip()
        params = []
        if args:
            for a in args.split(','):
                a = ' '.join(a.replace('FAR', '').replace('CONST', 'const').split())
                pm = re.match(r'(.*?)(\w+)\s*(\[\s*\w*\s*\])?$', a)
                t, n = pm.group(1).strip(), pm.group(2)
                if pm.group(3): t += '*'
                params.append((t.replace(' *', '*').replace('* ', '*'), n))
        meths.append((ret.strip(), mname, params))
    ifaces[name] = (base, meths)
    order.append(name)

# manual: IUnknown, IClassFactory, IReferenceClock
ifaces['IUnknown'] = ('', [('HRESULT', 'QueryInterface', [('REFIID', 'riid'), ('void**', 'ppv')]),
                           ('ULONG', 'AddRef', []), ('ULONG', 'Release', [])])
ifaces['IClassFactory'] = ('IUnknown', ifaces['IUnknown'][1] + [
    ('HRESULT', 'CreateInstance', [('IUnknown*', 'pOuter'), ('REFIID', 'riid'), ('void**', 'ppv')]),
    ('HRESULT', 'LockServer', [('BOOL', 'fLock')])])
ifaces['IReferenceClock'] = ('IUnknown', ifaces['IUnknown'][1] + [
    ('HRESULT', 'GetTime', [('REFERENCE_TIME*', 'pTime')]),
    ('HRESULT', 'AdviseTime', [('REFERENCE_TIME', 'baseTime'), ('REFERENCE_TIME', 'streamTime'), ('HANDLE', 'hEvent'), ('DWORD*', 'pdwAdviseCookie')]),
    ('HRESULT', 'AdvisePeriodic', [('REFERENCE_TIME', 'startTime'), ('REFERENCE_TIME', 'periodTime'), ('HANDLE', 'hSemaphore'), ('DWORD*', 'pdwAdviseCookie')]),
    ('HRESULT', 'Unadvise', [('DWORD', 'dwAdviseCookie')])])
U = ifaces['IUnknown'][1]
ifaces['IPersistStream'] = ('IUnknown', U + [('HRESULT', 'GetClassID', [('CLSID*', 'pClassID')]), ('HRESULT', 'IsDirty', []),
    ('HRESULT', 'Load', [('IStream*', 'pStm')]), ('HRESULT', 'Save', [('IStream*', 'pStm'), ('BOOL', 'fClearDirty')]),
    ('HRESULT', 'GetSizeMax', [('ULARGE_INTEGER*', 'pcbSize')])])
ifaces['IStream'] = ('IUnknown', U + [('HRESULT', 'Read', [('void*', 'pv'), ('ULONG', 'cb'), ('ULONG*', 'pcbRead')]),
    ('HRESULT', 'Write', [('const void*', 'pv'), ('ULONG', 'cb'), ('ULONG*', 'pcbWritten')]),
    ('HRESULT', 'Seek', [('LARGE_INTEGER', 'dlibMove'), ('DWORD', 'dwOrigin'), ('ULARGE_INTEGER*', 'plibNewPosition')]),
    ('HRESULT', 'SetSize', [('ULARGE_INTEGER', 'libNewSize')]),
    ('HRESULT', 'CopyTo', [('IStream*', 'pstm'), ('ULARGE_INTEGER', 'cb'), ('ULARGE_INTEGER*', 'pcbRead'), ('ULARGE_INTEGER*', 'pcbWritten')]),
    ('HRESULT', 'Commit', [('DWORD', 'grfCommitFlags')]), ('HRESULT', 'Revert', []),
    ('HRESULT', 'LockRegion', [('ULARGE_INTEGER', 'libOffset'), ('ULARGE_INTEGER', 'cb'), ('DWORD', 'dwLockType')]),
    ('HRESULT', 'UnlockRegion', [('ULARGE_INTEGER', 'libOffset'), ('ULARGE_INTEGER', 'cb'), ('DWORD', 'dwLockType')]),
    ('HRESULT', 'Stat', [('STATSTG*', 'pstatstg'), ('DWORD', 'grfStatFlag')]), ('HRESULT', 'Clone', [('IStream**', 'ppstm')])])
order += ['IUnknown', 'IClassFactory', 'IReferenceClock', 'IPersistStream', 'IStream']

def norm_iface(t):
    """interface name for a pointer type, the most derived of its family"""
    t = t.replace('struct ', '').replace('const ', '').strip()
    lp = {'LPDIRECTMUSIC': 'IDirectMusic', 'LPDIRECTMUSIC8': 'IDirectMusic8', 'LPDIRECTMUSICPORT': 'IDirectMusicPort',
          'LPDIRECTMUSICBUFFER': 'IDirectMusicBuffer', 'LPDIRECTMUSICINSTRUMENT': 'IDirectMusicInstrument',
          'LPDIRECTMUSICDOWNLOADEDINSTRUMENT': 'IDirectMusicDownloadedInstrument',
          'LPDIRECTMUSICCOLLECTION': 'IDirectMusicCollection', 'LPDIRECTMUSICDOWNLOAD': 'IDirectMusicDownload',
          'LPREFERENCECLOCK': 'IReferenceClock'}
    stars = t.count('*'); base = t.replace('*', '').strip()
    if base in lp: base, stars = lp[base], stars + 1
    return base, stars

def family(n):
    return n + '8' if n + '8' in ifaces else n

KNOWN = set(ifaces)
LEVEL2 = {'Seek', 'Stat', 'AddRef', 'Release', 'GetTime', 'IsPlaying', 'GetSegmentState', 'ReferenceToMusicTime', 'MusicToReferenceTime',
          'GetQueueTime', 'GetLatencyTime', 'GetResolvedTime', 'GetNotificationPMsg', 'FreePMsg', 'AllocPMsg', 'SendPMsg',
          'GetParam', 'GetSeek', 'GetStartPoint', 'GetStartTime', 'GetRepeats', 'GetLength', 'GetPrepareTime',
          'GetBumperLength', 'PlayBuffer', 'Read', 'GetNextEvent', 'GetUsedBytes', 'GetMaxBytes', 'GetBufferFormat',
          'GetRawBufferPtr', 'GetStartTime', 'Flush', 'TotalTime', 'PackStructured', 'PackUnstructured',
          'ResetReadPtr', 'SetStartTime', 'SetUsedBytes', 'MIDIToMusic', 'MusicToMIDI', 'TimeToRhythm', 'RhythmToTime',
          'GetLatencyClock', 'GetRunningStats', 'GetGlobalParam', 'GetObjectInPath', 'QueryInterface'}

INT32 = {'DWORD', 'LONG', 'ULONG', 'UINT', 'BOOL', 'MUSIC_TIME', 'WORD', 'BYTE', 'int', 'short', 'HRESULT', 'char',
         'SHORT', 'USHORT', 'INT', 'WCHAR'}
INT64 = {'unsigned long long', 'LARGE_INTEGER', 'ULARGE_INTEGER', 'REFERENCE_TIME', '__int64', 'LONGLONG', 'ULONGLONG', 'LONG64'}
FLT = {'float', 'FLOAT', 'double', 'DOUBLE'}
GUIDT = {'REFIID', 'REFGUID', 'REFCLSID', 'GUID*', 'LPGUID', 'const GUID*', 'LPCGUID', 'GUID const*', 'REFCLSID'}

def classify(meth, params):
    kinds = []
    last_iid = None
    for i, (t, n) in enumerate(params):
        tt = t.replace('struct ', '').strip()
        base, stars = norm_iface(tt)
        if tt in GUIDT:
            kinds.append(('G', None))
            if 'iid' in n.lower(): last_iid = i
        elif tt in ('void**', 'LPVOID*', 'PVOID*') and last_iid is not None:
            kinds.append(('Q', last_iid))
        elif base in KNOWN and stars == 1:
            kinds.append(('I', None))
        elif base in KNOWN and stars == 2:
            kinds.append(('O', family(base)))
        elif tt in ('WCHAR*', 'LPWSTR', 'LPCWSTR', 'const WCHAR*', 'WCHAR const*'):
            kinds.append(('S', None))
        elif tt in ('LPDMUS_OBJECTDESC', 'DMUS_OBJECTDESC*', 'LPDMUS_OBJECTDESC const'):
            kinds.append(('D', None))
        elif tt in ('DMUS_AUDIOPARAMS*',):
            kinds.append(('A', None))
        elif tt in ('LPDMUS_PORTPARAMS', 'DMUS_PORTPARAMS*', 'DMUS_PORTPARAMS8*'):
            kinds.append(('P', None))
        elif tt in INT32 or tt.startswith('DMUS_') and tt.endswith('FLAGS'):
            kinds.append(('i', None))
        elif tt in INT64:
            kinds.append(('l', None))
        elif tt in FLT:
            kinds.append(('f', None))
        elif tt.rstrip('*').strip() in INT32 and tt.endswith('*') and tt.count('*') == 1:
            kinds.append(('pi', None))
        elif tt.rstrip('*').strip() in INT64 and tt.count('*') == 1:
            kinds.append(('pl', None))
        elif n == 'pParam' and meth.startswith('Set'):
            kinds.append(('B', None))
        elif tt == 'VARIANT':
            kinds.append(('V', None))
        elif tt in ('DMUS_PMSG*',):
            kinds.append(('M', None))
        else:
            kinds.append(('p', None))
    return kinds

out = []
w = out.append
w('/* generated by gen.py from the MinGW DirectMusic headers - do not edit */')
w('#include "dmtrace.h"\n#include <stdio.h>\n#include <string.h>\n')
w('const dt_guidname dt_guids[] = {')
seen = set()
for name, v in guids:
    if name in seen: continue
    seen.add(name)
    w(f'    {{ {{ {v[0]}, {v[1]}, {v[2]}, {{ {", ".join(v[3:])} }} }}, "{name}" }},')
w('    { { 0, 0, 0, { 0 } }, NULL } };\n')

descs = []
for name in order:
    if name not in ifaces: continue
    base, meths = ifaces[name]
    if not meths: continue
    short = name.replace('IDirectMusic', 'DM').replace('IReferenceClock', 'Clock').replace('IClassFactory', 'Factory')
    fns = []
    for idx, (ret, mname, params) in enumerate(meths):
        kinds = classify(mname, params)
        fn = f'W_{name}_{mname}_{idx}'
        fns.append(fn)
        rtype = 'HRESULT' if ret == 'HRESULT' else ret
        isvoid = rtype == 'void'
        params = [('unsigned long long' if t in ('LARGE_INTEGER', 'ULARGE_INTEGER') else t, n) for t, n in params]
        decl = ', '.join([f'void *This'] + [f'{t} a{i}' for i, (t, n) in enumerate(params)])
        typs = ', '.join(['void *'] + [t for t, n in params])
        call = ', '.join(['This'] + [f'a{i}' for i in range(len(params))])
        lvl = 2 if mname in LEVEL2 else 1
        w(f'static {rtype} STDMETHODCALLTYPE {fn}({decl})')
        w('{')
        w(f'    typedef {rtype} (STDMETHODCALLTYPE *fn_t)({typs});')
        w(f'    fn_t f = (fn_t)dt_orig(This, {idx});')
        w(f'    int lg = dt_want(__builtin_return_address(0), {lvl});')
        w('    dt_line L; if (lg) {{ dt_begin(&L, This, "%s"); dt_fnaddr(&L, (void *)f); }}' % mname)
        for i, ((t, n), (k, x)) in enumerate(zip(params, kinds)):
            sep = '' if i == 0 else ', '
            if k == 'i': w(f'    if (lg) dt_int(&L, "{sep}", (long)(a{i}));')
            elif k == 'l': w(f'    if (lg) dt_i64(&L, "{sep}", (long long)(a{i}));')
            elif k == 'f': w(f'    if (lg) dt_flt(&L, "{sep}", (double)(a{i}));')
            elif k == 'G': w(f'    if (lg) dt_guid(&L, "{sep}", (const GUID *)(a{i}));')
            elif k == 'I':
                if t.replace('struct ','').strip() == 'IStream*': w(f'    if (a{i}) dt_hook((void *)(a{i}), &D_IStream);')
                w(f'    if (lg) dt_obj(&L, "{sep}", (void *)(a{i}));')
            elif k == 'S': w(f'    if (lg) dt_wstr(&L, "{sep}", (const WCHAR *)(a{i}));')
            elif k == 'D': w(f'    if (lg) dt_objdesc(&L, "{sep}", (const DMUS_OBJECTDESC *)(a{i}));')
            elif k == 'A': w(f'    if (lg) dt_audioparams(&L, "{sep}", (const DMUS_AUDIOPARAMS *)(a{i}));')
            elif k == 'P': w(f'    if (lg) dt_portparams(&L, "{sep}", (const DMUS_PORTPARAMS8 *)(a{i}));')
            elif k == 'B': w(f'    if (lg) dt_bytes(&L, "{sep}", (const void *)(a{i}), 16);')
            elif k == 'V': w(f'    if (lg) dt_variant(&L, "{sep}", &(a{i}));')
            elif k == 'M': w(f'    if (lg) dt_pmsg(&L, "{sep}", (const DMUS_PMSG *)(a{i}));')
            elif k in ('O', 'Q', 'pi', 'pl'): w(f'    if (lg) dt_str(&L, "{sep}&");')
            else: w(f'    if (lg) dt_ptr(&L, "{sep}", (const void *)(uintptr_t)(a{i}));')
        if isvoid:
            w(f'    f({call});')
        else:
            w(f'    {rtype} r = f({call});')
        ok = 'SUCCEEDED(r)' if rtype == 'HRESULT' else '1'
        for i, ((t, n), (k, x)) in enumerate(zip(params, kinds)):
            if k == 'O': w(f'    if ({ok} && a{i} && *(void **)(a{i})) dt_hook(*(void **)(a{i}), &D_{x});')
            elif k == 'Q': w(f'    if ({ok} && a{i} && *(void **)(a{i})) dt_hook_iid(*(void **)(a{i}), (const GUID *)(a{x}));')
        if mname == 'Release' and not isvoid: w('    if (r == 0) dt_forget(This);')
        w('    if (lg) {')
        outs = [(i, k) for i, (k, x) in enumerate(kinds) if k in ('O', 'Q', 'pi', 'pl')]
        for i, k in outs:
            if k in ('O', 'Q'): w(f'        dt_out_obj(&L, "{params[i][1]}", (void **)(a{i}));')
            elif k == 'pi': w(f'        dt_out_int(&L, "{params[i][1]}", (const void *)(a{i}), sizeof(*(a{i})));')
            else: w(f'        dt_out_i64(&L, "{params[i][1]}", (const void *)(a{i}));')
        for i, (k, x) in enumerate(kinds):
            if k == 'D' and mname in ('GetDescriptor', 'ParseDescriptor', 'EnumObject'):
                w(f'        dt_out_objdesc(&L, (const DMUS_OBJECTDESC *)(a{i}));')
        if name == 'IStream' and mname == 'Read': w('        { dt_line t; t.n = 0; dt_bytes(&t, "", a0, a1 < 16 ? (int)a1 : 16); dt_str(&L, ""); if (t.n) { char *o = L.o + L.on; snprintf(o, sizeof(L.o) - L.on, " data=%s", t.b); L.on += (int)strlen(o); } }')
        if isvoid: w('        dt_end(&L, "");')
        elif rtype == 'HRESULT': w('        dt_end_hr(&L, r);')
        else: w('        dt_end_ul(&L, (unsigned long)r);')
        w('    }')
        if not isvoid: w('    return r;')
        w('}\n')
    w(f'static void *const T_{name}[] = {{ {", ".join(fns)} }};')
    descs.append((name, short))

for name, short in descs:
    w(f'const dt_desc D_{name} = {{ "{name}", "{short}", {len(ifaces[name][1])}, T_{name} }};')
w('')
# IID -> desc
w('const dt_iidmap dt_iids[] = {')
iidnames = [g[0] for g in guids if g[0].startswith('IID_')]
for iid in iidnames:
    n = iid[4:]
    if n in ifaces: w(f'    {{ &{iid}, &D_{family(n)} }},')
for a, b in aliases.items():
    if a[4:] not in ifaces and b[4:] in ifaces and b in iidnames: pass
w('    { &IID_IPersistStream, &D_IPersistStream }, { &IID_IStream, &D_IStream },')
w('    { &IID_IUnknown, &D_IUnknown }, { &IID_IClassFactory, &D_IClassFactory }, { &IID_IReferenceClock, &D_IReferenceClock },')
w('    { NULL, NULL } };')
err = open(f'{inc}/dmerror.h', encoding='latin1').read()
w('const dt_hrname dt_hrs[] = {')
for n, kind, code in re.findall(r'#define\s+(DMUS_[ES]_\w+)\s+MAKE_DMHRESULT(SUCCESS|ERROR)\((0x[0-9A-Fa-f]+)\)', err):
    v = (0x80000000 if kind == 'ERROR' else 0) | (0x878 << 16) | (0x1000 + int(code, 16))
    w(f'    {{ (HRESULT)0x{v:08x}, "{n}" }},')
w('    { 0, NULL } };')
print('\n'.join(out))
# header with extern descs
hdr = ['/* generated */'] + [f'extern const dt_desc D_{n};' for n, s in descs]
open('dmtrace_descs.h', 'w').write('\n'.join(hdr) + '\n')
