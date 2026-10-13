#!/usr/bin/python3 -I
"""asm2c.py <dsdmo.dll> <out.c> <func>...: translate x86/x87 functions of XP's dsdmo.dll (32-bit, MSVC) into C,
instruction by instruction, for a bit-exact port of the I3DL2 reverb's process kernels.

The generated code runs on an emulated 32-bit machine (`i3m`): memory is an arena addressed by 32-bit offsets,
the integer registers and flags are variables, and the x87 register stack is held as `long double` - which on
x86 is the x87's own format, so with the control word at 53-bit precision (as Windows runs MSVC code) every
operation rounds exactly as the original's.  Loads from the DLL's constant data (`ds:addr`) become literals.
Calls to other translated functions are direct C calls through the emulated stack.  Only the instruction forms the
kernels use are handled; anything else stops the translation."""
import re, struct, subprocess, sys

dll, out = sys.argv[1], sys.argv[2]
funcs = [int(f, 16) for f in sys.argv[3:] if not f.startswith('@')]
# @va:len - DLL data the code reads through computed addresses (vtables, tables), copied into the arena
data = [tuple(int(x, 16) for x in a[1:].split(':')) for a in sys.argv[3:] if a.startswith('@')]
# functions implemented by hand (i3dl2.c): the C runtime's thunks and CParams' value store
STUBS = {0x6ca66c7e: '_ftol', 0x6ca59bd6: '_CIpow', 0x6ca59dd8: 'operator new', 0x6ca59be2: 'operator delete',
         0x6ca58e70: 'CParams store', 0x6ca58de0: 'CParams::GetParam'}
raw = open(dll, 'rb').read()
pe = struct.unpack_from('<I', raw, 0x3c)[0]
base = struct.unpack_from('<I', raw, pe + 0x34)[0]
ns = struct.unpack_from('<H', raw, pe + 6)[0]
so = pe + 0x18 + struct.unpack_from('<H', raw, pe + 0x14)[0]
secs = [struct.unpack_from('<8sIIII', raw, so + 40 * i) for i in range(ns)]
def off(va):
    r = va - base
    for n, vs, v, rs, ro in secs:
        if v <= r < v + max(vs, rs): return r - v + ro
    raise ValueError(hex(va))
def cfloat(va):
    f = struct.unpack_from('<f', raw, off(va))[0]
    return '%sf' % repr(f) if f == f else 'NAN'
def cdouble(va):
    return repr(struct.unpack_from('<d', raw, off(va))[0])

# disassembly with raw bytes; continuation lines (long encodings) are joined
dis = subprocess.run(['objdump', '-d', '-M', 'intel', dll], capture_output=True, text=True).stdout.split('\n')
ins = {}       # addr -> (bytes, mnemonic, operands)
order = []
last = None
for l in dis:
    m = re.match(r'\s*([0-9a-f]+):\t([0-9a-f ]+?)\s*(?:\t(\S+)\s*(.*))?$', l)
    if not m: continue
    a = int(m.group(1), 16); b = bytes.fromhex(m.group(2).replace(' ', ''))
    if m.group(3) is None:
        if last is not None:
            pb, mn, op = ins[last]; ins[last] = (pb + b, mn, op)
        continue
    ins[a] = (b, m.group(3), m.group(4).split('  ;')[0].strip()); order.append(a); last = a
addrs = sorted(ins)
jtabs = set()
absdisp = set()     # the DLL's addresses indexed by registers: their data has to be copied (@va:len)

R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
R8 = {'al': ('eax', 0), 'cl': ('ecx', 0), 'dl': ('edx', 0), 'bl': ('ebx', 0),
      'ah': ('eax', 8), 'ch': ('ecx', 8), 'dh': ('edx', 8), 'bh': ('ebx', 8)}

def addr_expr(s):
    """[base+index*scale+disp] -> C expression of the 32-bit address"""
    s = s.strip()
    m = re.match(r'(BYTE|WORD|DWORD|QWORD|TBYTE) PTR (?:ds:)?(.*)$', s)
    size = None
    if m: size = m.group(1); s = m.group(2)
    if s.startswith('ds:'): s = s[3:]
    if s.startswith('0x') and not s.startswith('['): return size, None, int(s, 16)
    inner = s.strip('[]')
    terms = re.findall(r'[+-]?[^+-]+', inner)
    parts = []
    for t in terms:
        sign = '-' if t.startswith('-') else '+'
        t = t.lstrip('+-')
        if '*' in t:
            r, k = t.split('*'); parts.append('%s(c->%s*%su)' % (sign, r, k))
        elif t in R32: parts.append('%sc->%s' % (sign, t))
        else: parts.append('%s0x%xu' % (sign, int(t, 16)))
    if any(t in R32 or '*' in t for t in (x.lstrip('+-') for x in terms)):
        for t in terms:
            t = t.lstrip('+-')
            if t.startswith('0x') and int(t, 16) >= base: absdisp.add(int(t, 16))
    e = ''.join(parts).lstrip('+')
    return size, '(uint32_t)(%s)' % e, None

def mem(size, e):
    t = {'BYTE': 'M8', 'WORD': 'M16', 'DWORD': 'M32', 'QWORD': 'M64'}[size]
    return '%s(%s)' % (t, e)

def operand(s, size_hint=None):
    """an integer operand as (rvalue, lvalue-or-None, size)"""
    s = s.strip()
    if s in R32: return 'c->' + s, 'c->' + s, 32
    if s in R16: return '(c->%s & 0xffff)' % R16[s], ('W16', R16[s]), 16
    if s in R8:
        r, sh = R8[s]; return '((c->%s >> %d) & 0xff)' % (r, sh), ('W8', r, sh), 8
    if s.startswith('0x') or re.match(r'^-?\d+$', s): return '0x%xu' % (int(s, 16) & 0xffffffff), None, size_hint
    size, e, absaddr = addr_expr(s)
    if e is None: e = '0x%xu' % absaddr
    bits = {'BYTE': 8, 'WORD': 16, 'DWORD': 32}[size]
    m = mem(size, e)
    return m, m, bits

def store(lv, val, bits):
    if isinstance(lv, tuple):
        if lv[0] == 'W16': return 'c->%s = (c->%s & 0xffff0000u) | ((%s) & 0xffffu);' % (lv[1], lv[1], val)
        r, sh = lv[1], lv[2]
        return 'c->%s = (c->%s & ~(0xffu << %d)) | (((%s) & 0xffu) << %d);' % (r, r, sh, val, sh)
    if bits == 8 and lv.startswith('M8'): return '%s = (uint8_t)(%s);' % (lv, val)
    if bits == 16 and lv.startswith('M16'): return '%s = (uint16_t)(%s);' % (lv, val)
    return '%s = %s;' % (lv, val)

def st(i): return 'ST(%d)' % i

def fpu_mem(op):
    size, e, absaddr = addr_expr(op)
    if e is None:
        return {'DWORD': cfloat, 'QWORD': cdouble}[size](absaddr), size, None
    return {'DWORD': '(long double)MF(%s)' % e, 'QWORD': '(long double)MD(%s)' % e}[size], size, e

def jtable(a, op):
    """jmp DWORD PTR [reg*4+table]: the bound from the cmp reg,n / ja before it"""
    m = re.match(r'DWORD PTR \[(\w+)\*4\+0x([0-9a-f]+)\]', op)
    reg, tab = m.group(1), int(m.group(2), 16)
    jtabs.add(tab)
    i = addrs.index(a)
    for k in range(1, 4):
        pb, pmn, pop_ = ins[addrs[i - k]]
        if pmn == 'cmp' and pop_.startswith(reg + ','):
            n = int(pop_.split(',')[1], 16) + 1
            return reg, [struct.unpack_from('<I', raw, off(tab + 4 * j))[0] for j in range(n)]
    raise SystemExit('jump table bound not found at %x' % a)

def translate(fa):
    # the function's extent: from its start to the first ret not followed by more reachable code
    labels, body, todo, seen = set(), [], [fa], set()
    while todo:
        a = todo.pop()
        while a not in seen:
            seen.add(a)
            b, mn, op = ins[a]
            nxt = addrs[addrs.index(a) + 1]
            if mn.startswith('j'):
                if op.startswith('DWORD'):
                    for t in jtable(a, op)[1]: labels.add(t); todo.append(t)
                    break
                t = int(op, 16); labels.add(t); todo.append(t)
                if mn == 'jmp': break
            if mn == 'ret': break
            a = nxt
    code = []
    if min(seen) != fa:                 # a tail jump to code before the entry
        labels.add(fa); code.append('    goto L_%x;' % fa)
    for a in sorted(seen):
        b, mn, op = ins[a]
        ops = [o.strip() for o in re.split(r',(?![^\[]*\])', op)] if op else []
        L = []
        if a in labels: code.append('L_%x:' % a)
        if mn in ('nop', 'int3'): continue
        if mn == 'mov' and op in ('edi,edi',): continue
        if mn in ('mov', 'movzx', 'movsx'):
            rv, _, sb = operand(ops[1], 32)
            _, lv, db = operand(ops[0], 32)
            if mn == 'movsx': rv = '(uint32_t)(int32_t)(int%d_t)(%s)' % (sb, rv)
            L.append(store(lv, rv, db))
        elif mn == 'lea':
            size, e, absaddr = addr_expr(ops[1])
            L.append('c->%s = %s;' % (ops[0], e))
        elif mn in ('add', 'sub', 'cmp', 'and', 'xor', 'or', 'test'):
            a1, lv, db = operand(ops[0], 32)
            a2, _, _ = operand(ops[1], db)
            if db == 8: L.append('FLAGS8_%s(%s, %s);' % (mn.upper(), a1, a2))
            else: L.append('FLAGS_%s(%s, %s);' % (mn.upper(), a1, a2))
            if mn not in ('cmp', 'test'): L.append(store(lv, 'c->res', db))
        elif mn in ('inc', 'dec', 'neg', 'not'):
            a1, lv, db = operand(ops[0], 32)
            L.append('FLAGS_%s(%s);' % (mn.upper(), a1)); L.append(store(lv, 'c->res', db))
        elif mn in ('shl', 'shr', 'sar'):
            a1, lv, db = operand(ops[0], 32)
            n = int(ops[1], 16) if ops[1].startswith('0x') else (1 if ops[1] == '1' else int(ops[1]))
            L.append('FLAGS_%s(%s, %d);' % (mn.upper(), a1, n)); L.append(store(lv, 'c->res', db))
        elif mn == 'imul' and len(ops) == 1:
            a1, _, _ = operand(ops[0], 32)
            L.append('{ int64_t p = (int64_t)(int32_t)c->eax * (int32_t)%s; c->eax = (uint32_t)p; c->edx = (uint32_t)(p >> 32); '
                     'c->cf = c->of = p != (int32_t)p; }' % a1)
        elif mn == 'mul' and len(ops) == 1:
            a1, _, _ = operand(ops[0], 32)
            L.append('{ uint64_t p = (uint64_t)c->eax * %s; c->eax = (uint32_t)p; c->edx = (uint32_t)(p >> 32); '
                     'c->cf = c->of = c->edx != 0; }' % a1)
        elif mn == 'imul':
            a1, lv, _ = operand(ops[-2] if len(ops) == 3 else ops[0], 32)
            a2, _, _ = operand(ops[-1], 32)
            _, lv, _ = operand(ops[0], 32)
            L.append('{ int64_t p = (int64_t)(int32_t)%s * (int32_t)%s; c->cf = c->of = p != (int32_t)p; %s }'
                     % (a1, a2, store(lv, '(uint32_t)p', 32)))
        elif mn == 'div':
            a1, _, _ = operand(ops[0], 32)
            L.append('{ uint64_t n = ((uint64_t)c->edx << 32) | c->eax; uint32_t d = %s; c->eax = (uint32_t)(n / d); '
                     'c->edx = (uint32_t)(n %% d); }' % a1)
        elif mn.startswith('set'):
            cc = {'sete': 'c->zf', 'setne': '!c->zf', 'setl': 'c->sf != c->of', 'setg': '!c->zf && c->sf == c->of',
                  'setb': 'c->cf', 'setae': '!c->cf', 'seta': '!c->cf && !c->zf', 'setbe': 'c->cf || c->zf'}[mn]
            _, lv, db = operand(ops[0], 8); L.append(store(lv, '(%s) ? 1u : 0u' % cc, 8))
        elif mn == 'rep' and op.startswith('stos DWORD'):
            L.append('while (c->ecx) { M32(c->edi) = c->eax; c->edi += 4; c->ecx--; }')
        elif mn == 'rep' and op.startswith('stos BYTE'):
            L.append('while (c->ecx) { M8(c->edi) = (uint8_t)c->eax; c->edi += 1; c->ecx--; }')
        elif mn == 'cdq':
            L.append('c->edx = (c->eax & 0x80000000u) ? 0xffffffffu : 0;')
        elif mn == 'push':
            v, _, _ = operand(ops[0], 32); L.append('PUSH(%s);' % v)
        elif mn == 'pop':
            L.append('c->%s = POP();' % ops[0])
        elif mn == 'jmp' and op.startswith('DWORD'):
            reg, ts = jtable(a, op)
            L.append('switch (c->%s) { %s default: abort(); }' % (reg, ' '.join('case %d: goto L_%x;' % (j, t) for j, t in enumerate(ts))))
        elif mn.startswith('j'):
            t = int(op, 16)
            if mn == 'jmp': L.append('goto L_%x;' % t)
            else: L.append('if (%s) goto L_%x;' % ({'je': 'ZF', 'jne': '!ZF', 'jl': 'SF != OF', 'jge': 'SF == OF',
                    'jg': '!ZF && SF == OF', 'jle': 'ZF || SF != OF', 'js': 'SF', 'jns': '!SF', 'jb': 'CF', 'jae': '!CF',
                    'ja': '!CF && !ZF', 'jbe': 'CF || ZF', 'jp': 'PF', 'jnp': '!PF'}[mn].replace('ZF', 'c->zf').replace('SF', 'c->sf')
                    .replace('OF', 'c->of').replace('CF', 'c->cf').replace('PF', 'c->pf'), t))
        elif mn == 'call' and op.startswith('0x'):
            t = int(op, 16); L.append('PUSH(0x%xu); f_%x(c);' % (addrs[addrs.index(a) + 1], t)); called.add(t)
        elif mn == 'call':
            v, _, _ = operand(op, 32); L.append('PUSH(0x%xu); i3_call(c, %s);' % (addrs[addrs.index(a) + 1], v))
        elif mn == 'ret':
            n = int(op, 16) if op else 0
            L.append('c->esp += 4 + %d; return;' % n)
        elif mn.startswith('f'):
            b0, b1 = b[0], b[1] if len(b) > 1 else 0
            regform = b0 in (0xd8, 0xdc, 0xde) and b1 >= 0xc0
            if regform:
                i = b1 & 7; g = (b1 >> 3) & 7
                ops_ = {0: '+', 1: '*', 4: '-', 5: 'r-', 6: '/', 7: 'r/'}
                if b0 == 0xd8:      # ST0 = ST0 op STi
                    o = ops_[g]
                    L.append('ST(0) = %s;' % ('ST(%d) %s ST(0)' % (i, o[1]) if o.startswith('r') else 'ST(0) %s ST(%d)' % (o, i)))
                else:               # STi = STi op ST0 (DC/DE: E0 is the reversed subtract, E8 the plain one)
                    o = {0: '+', 1: '*', 4: 'r-', 5: '-', 6: 'r/', 7: '/'}[g]
                    L.append('ST(%d) = %s;' % (i, 'ST(0) %s ST(%d)' % (o[1], i) if o.startswith('r') else 'ST(%d) %s ST(0)' % (i, o)))
                    if b0 == 0xde: L.append('FPOP();')
            elif mn == 'fld':
                if ops[0].startswith('st'):
                    i = int(re.search(r'\d', ops[0]).group()); L.append('{ long double t = ST(%d); FPUSH(t); }' % i)
                else:
                    v, _, _ = fpu_mem(ops[0]); L.append('FPUSH(%s);' % v)
            elif mn == 'fild':
                size, e, _ = addr_expr(ops[0]); L.append('FPUSH((long double)(int32_t)M32(%s));' % e)
            elif mn in ('fst', 'fstp'):
                if ops[0].startswith('st'):
                    i = int(re.search(r'\d', ops[0]).group()); L.append('ST(%d) = ST(0);' % i)
                else:
                    size, e, _ = addr_expr(ops[0])
                    L.append({'DWORD': 'MF(%s) = (float)ST(0);', 'QWORD': 'MD(%s) = (double)ST(0);'}[size] % e)
                if mn == 'fstp': L.append('FPOP();')
            elif mn == 'fistp':
                size, e, _ = addr_expr(ops[0]); L.append('M32(%s) = (uint32_t)x87_fist(ST(0)); FPOP();' % e)
            elif mn in ('fiadd', 'fisub', 'fimul', 'fidiv'):
                size, e, _ = addr_expr(ops[0])
                L.append('ST(0) = ST(0) %s (long double)(int32_t)M32(%s);' % ({'fiadd': '+', 'fisub': '-', 'fimul': '*', 'fidiv': '/'}[mn], e))
            elif mn in ('fadd', 'fsub', 'fsubr', 'fmul', 'fdiv', 'fdivr') and not ops[0].startswith('st'):
                v, _, _ = fpu_mem(ops[0])
                o = {'fadd': 'ST(0) + %s', 'fsub': 'ST(0) - %s', 'fsubr': '%s - ST(0)', 'fmul': 'ST(0) * %s',
                     'fdiv': 'ST(0) / %s', 'fdivr': '%s / ST(0)'}[mn] % v
                L.append('ST(0) = %s;' % o)
            elif mn == 'fxch':
                i = int(re.search(r'\d', ops[0]).group()) if ops else 1
                L.append('{ long double t = ST(0); ST(0) = ST(%d); ST(%d) = t; }' % (i, i))
            elif mn in ('fcom', 'fcomp', 'fucom', 'fucomp') and (not ops or ops[0].startswith('st')):
                i = int(re.search(r'\d', ops[0]).group()) if ops else 1
                L.append('FCOM(ST(0), ST(%d));' % i)
                if mn.endswith('p'): L.append('FPOP();')
            elif mn in ('fcom', 'fcomp'):
                v, _, _ = fpu_mem(ops[0]); L.append('FCOM(ST(0), %s);' % v)
                if mn == 'fcomp': L.append('FPOP();')
            elif mn in ('fcompp', 'fucompp'):
                L.append('FCOM(ST(0), ST(1)); FPOP(); FPOP();')
            elif mn == 'fnstsw':
                L.append('c->eax = (c->eax & 0xffff0000u) | c->fsw;')
            elif mn == 'fcos': L.append('ST(0) = x87_cos(ST(0));')
            elif mn == 'fsqrt': L.append('ST(0) = x87_sqrt(ST(0));')
            elif mn == 'fldz': L.append('FPUSH(0.0L);')
            elif mn == 'fld1': L.append('FPUSH(1.0L);')
            elif mn == 'fchs': L.append('ST(0) = -ST(0);')
            elif mn == 'fabs': L.append('ST(0) = fabsl(ST(0));')
            else: raise SystemExit('unhandled %x %s %s' % (a, mn, op))
        else:
            raise SystemExit('unhandled %x %s %s' % (a, mn, op))
        code.append('    ' + ' '.join(L) + '   /* %x %s %s */' % (a, mn, op))
    return code

called = set()
bodies = [(f, translate(f)) for f in funcs]
missing = called - set(funcs) - set(STUBS)
uncovered = [a for a in absdisp if not any(va <= a < va + n for va, n in data) and a not in jtabs]
if uncovered: raise SystemExit('indexed DLL data not copied: ' + ' '.join('%x' % a for a in sorted(uncovered)))
if missing: raise SystemExit('calls to untranslated functions: ' + ' '.join('%x' % m for m in sorted(missing)))
o = open(out, 'w')
o.write('/* Generated by tools/i3dl2/asm2c.py from Windows XP SP3 dsdmo.dll (5.3.2600.5512): the I3DL2 reverb\'s process\n'
        ' * reverb, instruction for instruction (see i3dl2_machine.h).  Do not edit. */\n')
for f in funcs: o.write('static void f_%x(i3m *c);\n' % f)
for f, n in STUBS.items(): o.write('static void f_%x(i3m *c);   /* %s (i3dl2.c) */\n' % (f, n))
o.write('static void i3_call(i3m *c, uint32_t f)\n{\n    switch (f) {\n')
for f in list(funcs) + list(STUBS): o.write('    case 0x%xu: f_%x(c); break;\n' % (f, f))
o.write('    default: abort();\n    }\n}\n')
o.write('static const struct { uint32_t va, len; const uint8_t *d; } i3_data[] = {\n')
for va, n in data:
    b = raw[off(va):off(va) + n]
    o.write('    { 0x%xu, %d, (const uint8_t[]){ %s } },\n' % (va, n, ','.join(str(x) for x in b)))
o.write('};\n')
for f, body in bodies:
    o.write('\nstatic void f_%x(i3m *c)\n{\n' % f)
    o.write('\n'.join(body))
    o.write('\n}\n')
o.close()
