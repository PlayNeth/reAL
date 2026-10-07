#!/usr/bin/env python3
"""Build reAL_hrtf.bin from hrtf_extract.py output.

usage: hrtf_build.py measured.npz reAL_hrtf.bin [taps]

* normalises each response to the 2D path (the sync clicks are unity gain)
* repairs directions that were contaminated or stale (identical to the previous slot) from their
  left/right mirror image, or by interpolating neighbours on the same ring
* splits every response into a per-ear onset delay + `taps` (default 64) aligned coefficients

File format (little endian):
  "RLHRTF01" | u32 rate | u32 taps | u32 n_rings
  per ring: f32 elevation, u32 n_az            (azimuths evenly spaced from 0, clockwise)
  per direction, rings in order, azimuth ascending:
      f32 delay_left, f32 delay_right (samples), f32 left[taps], f32 right[taps]
The data is derived from the DirectSound implementation that was measured: keep it private.
"""
import struct
import sys

import numpy as np

PRE = 0.002
MARGIN = 2           # samples kept before the detected onset


def main():
    src, dst = sys.argv[1], sys.argv[2]
    taps = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    d = np.load(src)
    irs, meta, bad, fs = d["irs"].astype(np.float64), d["meta"], d["bad"].copy(), int(d["fs"])
    pre = int(PRE * fs)
    ref_peak = np.abs(irs[0]).max()                  # first sync click (2D path, unity)
    irs = irs / ref_peak

    # stale = sample-identical to the previous slot (implementation hadn't applied the new position)
    stale = np.zeros(len(irs), bool)
    for k in range(1, len(irs)):
        if np.abs(irs[k] - irs[k - 1]).max() < 1e-3 * max(np.abs(irs[k]).max(), 1e-9):
            stale[k] = True
    good = ~(bad | stale)

    pos = {}
    for k, (az, el, dist) in enumerate(meta):
        if np.isnan(az) or dist != 1:
            continue
        key = (int(round(el)), int(round(az)) % 360)
        if key not in pos or (good[k] and not good[pos[key]]):
            pos[key] = k

    rings = sorted({e for e, _ in pos})
    table, repaired = {}, []
    for e in rings:
        azs = sorted(a for ee, a in pos if ee == e)
        for a in azs:
            k = pos[(e, a)]
            if good[k]:
                table[(e, a)] = irs[k]
    for e in rings:
        azs = sorted(a for ee, a in pos if ee == e)
        for a in azs:
            if (e, a) in table:
                continue
            m = (e, (360 - a) % 360)
            if m in table and m != (e, a):
                table[(e, a)] = table[m][::-1].copy()          # mirror: swap ears
                repaired.append(f"el {e} az {a} (mirror)")
    for e in rings:
        azs = sorted(a for ee, a in pos if ee == e)
        step = 360 // len(azs) if len(azs) > 1 else 360
        for a in azs:
            if (e, a) in table:
                continue
            lo, hi = (e, (a - step) % 360), (e, (a + step) % 360)
            if lo in table and hi in table:
                table[(e, a)] = 0.5 * (table[lo] + table[hi])
                repaired.append(f"el {e} az {a} (interpolated)")
            else:
                raise SystemExit(f"cannot repair el {e} az {a}")
    print(f"{sum(bad)} contaminated, {sum(stale)} stale; repaired: {', '.join(repaired) or 'none'}")

    out = bytearray(b"RLHRTF01")
    out += struct.pack("<III", fs, taps, len(rings))
    body = bytearray()
    for e in rings:
        azs = sorted(a for ee, a in pos if ee == e)
        out += struct.pack("<fI", float(e), len(azs))
        for a in azs:
            ir = table[(e, a)]
            delays, coefs = [], []
            for ch in (0, 1):
                h = ir[ch]
                thr = 0.1 * np.abs(h).max()
                on = int(np.flatnonzero(np.abs(h) > thr)[0]) - MARGIN
                on = max(on, 0)
                c = h[on:on + taps]
                if len(c) < taps:
                    c = np.pad(c, (0, taps - len(c)))
                delays.append(float(on - (pre - MARGIN)))       # relative to the click
                coefs.append(c.astype(np.float32))
            body += struct.pack("<ff", max(delays[0], 0.0), max(delays[1], 0.0))
            body += coefs[0].tobytes() + coefs[1].tobytes()
    open(dst, "wb").write(bytes(out + body))
    n = sum(len([a for ee, a in pos if ee == e]) for e in rings)
    print(f"wrote {dst}: {fs} Hz, {taps} taps, rings {rings}, {n} directions, {len(out) + len(body)} bytes")


if __name__ == "__main__":
    main()
