#!/usr/bin/env python3
"""native.py <corpus dir> <work dir> <seconds> <workers> [sgt|dep|dls]

Mutated DirectMusic files through reDM's native test programs built with AddressSanitizer (32-bit, as the DLL):

  SAN="-m32 -O1 -g -fno-omit-frame-pointer -fwrapv -fsanitize=address,bounds,pointer-overflow,integer-divide-by-zero -fno-sanitize-recover=all -std=gnu11"
  cc $SAN -o <work>/bin/test_perf tests/test_perf.c src/music/dmperf.c src/music/dmseg.c src/music/dmstyload.c \
     src/music/dmcomp.c src/music/dmcompose.c src/music/dmtheory.c src/music/dmwave.c src/music/dmsmf.c src/music/dmdls.c -lm
  cc $SAN -o <work>/bin/test_dls tests/test_dls.c src/music/dmdls.c

The corpus: segments with the styles and collections they name next to them (tools/sty/cases.py <dir> writes a set;
a game's .sgt and .dls files do as well).  Each run mutates a segment ("sgt"), a file a segment names ("dep") or a
collection ("dls") and plays it for a moment with the fixes on (TP_FIX=0x7f, as a game has them: without the guards the
original's own endless loops show up).  A sanitizer report or a run of over 20 seconds is kept (one per stack; up to 20 hangs a worker) in
<work>/crashes/<kind>_<hash>/ with the input and the report.  The COM layer (loader, scripts, containers, audiopath
configurations, composition) is tests/ldfuzz.c's, under Wine."""
import os, sys, random, subprocess, struct, shutil, time, multiprocessing, hashlib
CORP = FZ = BIN = OUT = None
INTS = [0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 255, 256, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe, 0xfffffff0, 0x40000000, 0x20000000]
def mutate(d, R):
    d = bytearray(d)
    if len(d) < 16: return bytes(d)
    for _ in range(R.choice([1, 1, 1, 2, 2, 3, 5, 8])):
        k = R.randrange(10)
        p = R.randrange(12, len(d))
        if k == 0: d[p] ^= 1 << R.randrange(8)
        elif k == 1: d[p] = R.choice([0, 1, 0x7f, 0x80, 0xff, R.randrange(256)])
        elif k in (2, 3) and p + 4 <= len(d):
            p &= ~1 if R.random() < 0.8 else ~0
            d[p:p + 4] = struct.pack('<I', R.choice(INTS))
        elif k == 4 and p + 2 <= len(d): d[p:p + 2] = struct.pack('<H', R.choice(INTS) & 0xffff)
        elif k == 5: del d[p:p + R.choice([1, 2, 4, 8, R.randrange(1, 64)])]
        elif k == 6: d[p:p] = bytes(R.randrange(256) for _ in range(R.choice([1, 2, 4, 8])))
        elif k == 7: d = d[:p]
        elif k == 8 and p + 8 <= len(d):
            q = R.randrange(12, len(d)); n = R.choice([4, 8, 16, 32]); d[p:p + n] = d[q:q + n]
        elif k == 9 and p + 4 <= len(d):
            v = struct.unpack_from('<I', d, p & ~1)[0]; v = (v + R.choice([-1, 1, -4, 4, -8, 8, 0x100, -0x100])) & 0xffffffff
            struct.pack_into('<I', d, p & ~1, v)
        if len(d) < 16: break
    return bytes(d)
def refs(sgt, names):
    out = []
    for n in names:
        if n.encode('utf-16-le') in sgt or n.upper().encode('utf-16-le') in sgt: out.append(n)
    return out
def worker(args):
    global CORP, FZ, BIN, OUT
    wid, secs, mode, seed, CORP, FZ = args
    BIN = FZ + '/bin'; OUT = FZ + '/crashes'
    R = random.Random(seed)
    wd = '%s/w%d' % (FZ, wid); shutil.rmtree(wd, ignore_errors=True); os.makedirs(wd)
    files = sorted(os.listdir(CORP)); data = {f: open(CORP + '/' + f, 'rb').read() for f in files}
    for f in files: os.symlink(CORP + '/' + f, wd + '/' + f)
    sgts = [f for f in files if f.endswith('.sgt')]; dlss = [f for f in files if f.endswith('.dls')]
    deps = {s: [n for n in refs(data[s], [f for f in files if not f.endswith('.sgt')])] for s in sgts}
    env = dict(os.environ, TP_GM='/nonexistent', TP_FIX='0x7f', ASAN_OPTIONS='detect_leaks=0:abort_on_error=0:allocator_may_return_null=1:max_allocation_size_mb=1024', UBSAN_OPTIONS='print_stacktrace=1')
    end = time.time() + secs; n = 0; found = 0
    while time.time() < end:
        n += 1
        m = mode or R.choice(['sgt', 'sgt', 'dep', 'dls', 'sgt'])
        restore = None
        if m == 'dls':
            f = R.choice(dlss); md = mutate(data[f], R); path = wd + '/fz.dls'; open(path, 'wb').write(md)
            cmd = [BIN + '/test_dls', path, '/dev/null']; tag = f
        else:
            s = R.choice(sgts)
            if m == 'dep' and deps[s]:
                f = R.choice(deps[s]); md = mutate(data[f], R); os.unlink(wd + '/' + f); open(wd + '/' + f, 'wb').write(md); restore = f
                path = wd + '/' + s; tag = s + '+' + f
            else:
                md = mutate(data[s], R); path = wd + '/fz.sgt'; open(path, 'wb').write(md); tag = s
            ex = {}
            r = R.random()
            if r < 0.15: ex['TP_CLONE'] = '%d:%d' % (R.choice([0, 0, 768, -5]), R.choice([0, 3072, 100000, -1]))
            elif r < 0.3: ex['TP_RELOAD'] = str(R.randrange(0, 300))
            elif r < 0.4: ex['TP_SMF'] = '1'
            cmd = [BIN + '/test_perf', path, '/dev/null', str(R.choice([100, 300, 1000, 3000])), R.choice(['0', '0', '0x80', '0x400', '0x1000'])]
            env2 = dict(env, **ex)
        try:
            p = subprocess.run(cmd, env=env2 if m != 'dls' else env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=20)
            err = p.stderr.decode('latin1', 'replace'); bad = 'ERROR: AddressSanitizer' in err or 'runtime error' in err or p.returncode < 0
            kind = 'crash'
        except subprocess.TimeoutExpired as e:
            err = 'TIMEOUT'; bad = True; kind = 'hang'
        if bad:
            lines = [l for l in err.split('\n') if l.strip().startswith('#') and ('src/music' in l or 'tests/' in l)][:3]
            first = [l for l in err.split('\n') if 'ERROR' in l or 'runtime error' in l][:1]
            key = hashlib.md5(('|'.join(l.split(' in ')[-1] for l in lines) + (first[0].split('runtime error')[-1][:60] if first and 'runtime' in first[0] else kind)).encode()).hexdigest()[:10]
            if kind == 'hang': key = '%d_%d' % (wid, found)       # (no stack to tell them apart: each kept, up to 20)
            d = '%s/%s_%s' % (OUT, kind, key)
            if not os.path.exists(d) and found < 20:
                os.makedirs(d, exist_ok=True); found += 1
                open(d + '/input_' + (restore or os.path.basename(path)), 'wb').write(md)
                open(d + '/info.txt', 'w').write('tag %s\ncmd %s\nenv %s\n\n%s' % (tag, ' '.join(cmd), ex if m != 'dls' else {}, err[:6000]))
        if restore: os.unlink(wd + '/' + restore); os.symlink(CORP + '/' + restore, wd + '/' + restore)
    return n, found
if __name__ == '__main__':
    if len(sys.argv) < 5: sys.exit(__doc__)
    corp, work = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    secs = int(sys.argv[3]); nw = int(sys.argv[4]); mode = sys.argv[5] if len(sys.argv) > 5 else None
    OUT = work + '/crashes'
    os.makedirs(OUT, exist_ok=True)
    with multiprocessing.Pool(nw) as pool:
        res = pool.map(worker, [(i, secs, mode, 1000 * int(time.time()) + i, corp, work) for i in range(nw)])
    print('runs', sum(r[0] for r in res), 'new', sum(r[1] for r in res), 'buckets', len(os.listdir(OUT)))
