#!/usr/bin/env python3
"""Turn an hrtfprobe recording into per-position impulse responses.

usage: hrtf_extract.py recording.wav hrtfprobe.txt out.npz
Prints ITD / ILD per azimuth at 0 degrees elevation as a sanity summary.
"""
import re
import sys

import numpy as np

PRE, POST = 0.002, 0.020      # window around each click (s)


def read_wav(path):
    """Minimal RIFF reader: PCM 16/24/32 and IEEE float, tolerant of truncated/unfinished headers."""
    import struct
    d = open(path, "rb").read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        raise SystemExit("not a WAV file")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        cid, size = d[pos:pos + 4], struct.unpack("<I", d[pos + 4:pos + 8])[0]
        body = pos + 8
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", d[body:body + 16])
            if fmt[0] == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: real tag is in the subformat GUID
                fmt = (struct.unpack("<H", d[body + 24:body + 26])[0],) + fmt[1:]
        elif cid == b"data":
            end = len(d) if size in (0, 0xFFFFFFFF) or body + size > len(d) else body + size
            data = d[body:end]
            break
        pos = body + size + (size & 1)
    if not fmt or data is None:
        raise SystemExit("no fmt/data chunk")
    tag, ch, fs, _, _, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(data[:len(data) // 4 * 4], "<f4").astype(np.float64)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(data[:len(data) // 2 * 2], "<i2").astype(np.float64) / 32768.0
    elif tag == 1 and bits == 32:
        x = np.frombuffer(data[:len(data) // 4 * 4], "<i4").astype(np.float64) / 2147483648.0
    elif tag == 1 and bits == 24:
        b = np.frombuffer(data[:len(data) // 3 * 3], np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v & 0x800000, v - 0x1000000, v).astype(np.float64) / 8388608.0
    else:
        raise SystemExit(f"unsupported WAV format tag {tag} / {bits} bit")
    n = len(x) // ch * ch
    return x[:n].reshape(-1, ch)[:, :2], fs


def find_events(x, fs, thr):  # (unused now; kept for recordings without a schedule)
    env = np.abs(x).max(axis=1)
    quiet = int(0.15 * fs)
    ev, last = [], -quiet
    hot = np.flatnonzero(env > thr)
    for i in hot:
        if i - last >= quiet:
            ev.append(i)
        last = i
    return ev


def main():
    wav, sched, out = sys.argv[1:4]
    x, fs = read_wav(wav)
    labels = [l.split(" ", 2)[2].strip() for l in open(sched) if l.startswith("SLOT")]
    times = [float(re.match(r"SLOT t=([\d.]+)", l).group(1)) for l in open(sched) if l.startswith("SLOT")]
    env = np.abs(x).max(axis=1)
    first = int(np.flatnonzero(env > env.max() * 0.3)[0])     # first sync click
    off = first / fs - times[0] / 1000.0
    ev, bad = [], []
    for t in times:                                           # schedule alignment, refine on the peak
        c = int((t / 1000.0 + off) * fs)
        a0, a1 = c - int(0.03 * fs), c + int(0.03 * fs)
        i = a0 + int(np.argmax(env[a0:a1]))
        if env[i] > env.max() * 0.005:
            off = i / fs - t / 1000.0                         # follow clock drift between timer and audio
        ev.append(i)
        pre_e = (x[i - int(0.02 * fs): i - int(0.002 * fs)] ** 2).sum()
        head = (x[i - 96: i + int(0.005 * fs)] ** 2).sum()
        bad.append(pre_e > head * 1e-3)                       # something else was playing
    print(f"{len(ev)} clicks aligned from the schedule (fs {fs}); {sum(bad)} contaminated: "
          + ", ".join(l for l, b in zip(labels, bad) if b))
    pre, post = int(PRE * fs), int(POST * fs)
    irs, meta = [], []
    for i, lab in zip(ev, labels):
        seg = x[max(0, i - pre): i + post]
        if len(seg) < pre + post:
            seg = np.pad(seg, ((0, pre + post - len(seg)), (0, 0)))
        irs.append(seg.T)
        m = re.match(r"pos az (-?\d+) el (-?\d+) dist ([\d.]+)", lab)
        meta.append((float(m.group(1)), float(m.group(2)), float(m.group(3))) if m else (np.nan, np.nan, np.nan))
    irs = np.array(irs)
    meta = np.array(meta)
    ref = irs[0]                     # 2D sync click = calibration
    refgain = np.sqrt((ref ** 2).sum() / 2)
    np.savez(out, irs=irs, meta=meta, fs=fs, labels=np.array(labels), refgain=refgain, bad=np.array(bad))
    print(f"saved {out}: {irs.shape}  (2D reference rms-energy {refgain:.4f})")

    print("\n az   ITD(us)  ILD(dB)  level(dB re 2D)   [elevation 0, distance 1]")
    for k, (az, el, d) in enumerate(meta):
        if el != 0 or d != 1 or bad[k]:
            continue
        l, r = irs[k]
        cc = np.correlate(l, r, "full")
        el_ = (l ** 2).sum(); er_ = (r ** 2).sum()
        lag = (np.argmax(cc) - (len(l) - 1)) / fs * 1e6 if min(el_, er_) > 1e-9 * max(el_, er_) else float("nan")
        ild = 10 * np.log10((er_ + 1e-20) / (el_ + 1e-20))
        lvl = 10 * np.log10(((el_ + er_) / 2) / (refgain ** 2) + 1e-20)
        ild = max(-60.0, min(60.0, ild))
        print(f"{int(az):4d} {lag:8.0f} {ild:8.1f} {lvl:12.1f}")


if __name__ == "__main__":
    main()
