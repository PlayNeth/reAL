#!/usr/bin/env python3
"""Turn a `levelprobe clicks` recording into the measured.npz that hrtf_build.py reads.

usage: clicks_to_npz.py recording.f32 recording-clicks.txt measured.npz

The recording is what tests/levelprobe.c's `clicks` mode writes for one dsound.dll (the kit's run9.bat: ALchemy.f32
and ALchemy-clicks.txt): for each case in the text file's order, IR_LEN frames of 32-bit float left and right, IR_PRE
frames of them before the first sound.  The two 2D clicks give the path's own level (the first is the reference
hrtf_build.py divides by); the 3D clicks at the minimum distance are the directions; the repeats, the other distances
and the half-level ones are left out (they only show that the implementation is a fixed filter for each direction).
A direction the implementation rendered with the previous position (ALchemy now and then applies SetPosition on a
stopped buffer after its Play) is sample-identical to the slot before it, which hrtf_build.py repairs from its mirror.

The data is derived from the DirectSound implementation that was measured: keep it private.
"""
import re
import sys

import numpy as np

PRE = 0.002          # hrtf_build.py's: seconds kept before the click


def main():
    f32, txt, dst = sys.argv[1:4]
    lines = [l.rstrip("\r\n") for l in open(txt, encoding="latin1")]
    m = next(re.match(r"clicks: (\d+) frames a case before the first sound, (\d+) in all.* at (\d+) Hz", l) for l in lines if l.startswith("clicks:"))
    ir_pre, ir_len, fs = int(m.group(1)), int(m.group(2)), int(m.group(3))
    cases = [l for l in lines if l.startswith("click ")]
    a = np.fromfile(f32, dtype="<f4").reshape(-1, ir_len, 2)
    if len(cases) != a.shape[0]:
        raise SystemExit(f"{len(cases)} cases in {txt}, {a.shape[0]} in {f32}")
    pre = int(PRE * fs)
    irs, meta, bad = [], [], []
    for l, x in zip(cases, a):
        name = re.sub(r"\s+captured.*", "", l)[len("click "):]
        m = re.fullmatch(r"az (-?\d+) el (-?\d+)", name)
        if m:
            az, el, dist = float(m.group(1)), float(m.group(2)), 1.0
        elif name.startswith("2D"):
            az, el, dist = np.nan, np.nan, 0.0
        else:
            continue                                    # a repeat, another distance, half the level
        ir = np.zeros((2, ir_len + pre - ir_pre), np.float32)
        ir[:, pre - ir_pre:] = x.T                      # the first sound at `pre`
        irs.append(ir)
        meta.append((az, el, dist))
        bad.append("NOTHING HEARD" in l or "cut short" in l)
    np.savez(dst, irs=np.array(irs), meta=np.array(meta), bad=np.array(bad), fs=fs)
    print(f"wrote {dst}: {len(irs)} responses of {irs[0].shape[1]} samples at {fs} Hz, {sum(bad)} marked bad")


if __name__ == "__main__":
    main()
