"""genscript.py <collection.dls> <seed> [seconds] [density] > script
Random MIDI exercising the synth: notes on all channels with the collection's instruments, controllers
(mod wheel, volume, pan, expression, sustain, CC74, reverb/chorus sends, reset), pitch bend, RPN bend range,
channel pressure, all-notes-off."""
import sys, struct, random

def patches(path):
    d = open(path, 'rb').read(); out = []; i = 0
    while True:
        i = d.find(b'insh', i)
        if i < 0: break
        n, bank, prog = struct.unpack_from('<III', d, i + 8)
        out.append((bank, prog)); i += 4
    return out

pl = patches(sys.argv[1]); rnd = random.Random(int(sys.argv[2]))
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 6.0
dens = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
ev = []
chans = [c for c in range(16)]
for c in chans:
    bank, prog = rnd.choice(pl)
    ev.append((0, 0xb0 | c, (bank >> 8) & 0x7f, 0))
    ev.append((0, 0xb0 | c, 32, bank & 0x7f))
    ev.append((0, 0xc0 | c, prog, 0))
    if bank & 0x80000000 and c != 9: pass
t = 5.0
end = secs * 1000
held = {}
while t < end - 300:
    t += rnd.expovariate(dens / 40.0)
    c = rnd.choice(chans); r = rnd.random(); ms = int(t)
    if r < 0.45:
        n = rnd.randint(24, 100); v = rnd.randint(1, 127)
        ev.append((ms, 0x90 | c, n, v))
        ev.append((ms + rnd.randint(1, 1500), 0x80 | c, n, rnd.randint(0, 127)))
    elif r < 0.5: ev.append((ms, 0xe0 | c, rnd.randint(0, 127), rnd.randint(0, 127)))
    elif r < 0.55: ev.append((ms, 0xb0 | c, 1, rnd.randint(0, 127)))
    elif r < 0.6: ev.append((ms, 0xb0 | c, 7, rnd.randint(0, 127)))
    elif r < 0.65: ev.append((ms, 0xb0 | c, 10, rnd.randint(0, 127)))
    elif r < 0.7: ev.append((ms, 0xb0 | c, 11, rnd.randint(0, 127)))
    elif r < 0.74: ev.append((ms, 0xb0 | c, 64, rnd.choice([0, 127])))
    elif r < 0.78: ev.append((ms, 0xb0 | c, 74, rnd.randint(0, 127)))
    elif r < 0.8: ev.append((ms, 0xd0 | c, rnd.randint(0, 127), 0))
    elif r < 0.82:   # bend range via RPN 0
        ev += [(ms, 0xb0 | c, 101, 0), (ms, 0xb0 | c, 100, 0), (ms, 0xb0 | c, 6, rnd.randint(0, 24)), (ms, 0xb0 | c, 38, 0)]
    elif r < 0.84:   # fine/coarse tuning
        ev += [(ms, 0xb0 | c, 101, 0), (ms, 0xb0 | c, 100, rnd.choice([1, 2])), (ms, 0xb0 | c, 6, rnd.randint(32, 96)),
               (ms, 0xb0 | c, 38, rnd.randint(0, 127)), (ms, 0xb0 | c, 101, 127), (ms, 0xb0 | c, 100, 127)]
    elif r < 0.86: ev.append((ms, 0xb0 | c, 91, rnd.randint(0, 127)))
    elif r < 0.87: ev.append((ms, 0xb0 | c, 123, 0))
    elif r < 0.875: ev.append((ms, 0xb0 | c, 121, 0))
    elif r < 0.9:
        bank, prog = rnd.choice(pl)
        ev += [(ms, 0xb0 | c, 0, (bank >> 8) & 0x7f), (ms, 0xb0 | c, 32, bank & 0x7f), (ms, 0xc0 | c, prog, 0)]
ev.sort(key=lambda e: e[0])
for e in ev:
    if e[0] < end: print('%d %02x %d %d' % e)
print('%d 0' % int(end))
