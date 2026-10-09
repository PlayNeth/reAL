"""Measure eaxtest recordings: per slot the direct click level and the reverb tail.
usage: eaxtest_analyze.py rec.raw|rec.wav eaxtest.txt   (raw = float32 stereo 48 kHz)"""
import sys, re, numpy as np

def load(path):
    if path.endswith('.wav'):
        import wave
        w = wave.open(path); n = w.getnframes(); d = w.readframes(n)
        a = np.frombuffer(d, dtype=np.int16 if w.getsampwidth() == 2 else np.float32).reshape(-1, w.getnchannels())
        a = a.astype(np.float64)
        if w.getsampwidth() == 2: a /= 32768
        return a, w.getframerate()
    return np.fromfile(path, dtype=np.float32).reshape(-1, 2).astype(np.float64), 48000

x, rate = load(sys.argv[1])
labels = [m.group(1) for m in re.finditer(r'^SLOT t=[\d.]+ (.*)$', open(sys.argv[2]).read(), re.M)]
mono = np.abs(x).max(axis=1)
onsets, i = [], 0
while i < len(mono):
    if mono[i] > 0.02:
        onsets.append(i); i += int(1.2 * rate)
    else: i += 1
def band(sig, lo, hi):
    f = np.fft.rfft(sig * np.hanning(len(sig))); fr = np.fft.rfftfreq(len(sig), 1 / rate)
    return float((np.abs(f[(fr >= lo) & (fr < hi)]) ** 2).sum())
print(f'{len(onsets)} clicks found for {len(labels)} slots')
res = {}
for k, o in enumerate(onsets[:len(labels)]):
    direct = x[o:o + int(0.004 * rate)]
    tail = x[o + int(0.06 * rate):o + int(1.2 * rate)]
    pk = 20 * np.log10(np.abs(direct).max() + 1e-12)
    te = 10 * np.log10((tail ** 2).sum() + 1e-20)
    hf = band(tail[:, 0], 3000, 8000) / (band(tail[:, 0], 100, 1000) + 1e-30)
    dhf = band(x[o - 32:o + 480, 0], 3000, 8000) / (band(x[o - 32:o + 480, 0], 100, 1000) + 1e-30)
    res[labels[k]] = (pk, te, hf, dhf)
    print(f'{labels[k]:24s} direct peak {pk:7.2f} dB  direct HF/LF {10*np.log10(dhf+1e-30):6.1f} dB  '
          f'tail energy {te:7.2f} dB  tail HF/LF {10*np.log10(hf+1e-30):6.1f} dB')
