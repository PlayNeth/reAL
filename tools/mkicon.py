"""config/reALcfg.ico: the configurator's icon, from the wordmark (the AL of reAL in white on its marine blue, the bar
under it), drawn at each size and not scaled down.  The sizes up to 64 are stored as plain 32-bit bitmaps, which
every Windows and every resource compiler reads, and 256 as PNG, as Windows has it.

    python3 tools/mkicon.py [a bold sans font file]      (needs Pillow; Inter Bold was used)"""
import sys, os, io, struct
from PIL import Image, ImageDraw, ImageFont
FONT = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/fonts/opentype/inter/Inter-Bold.otf'
BLUE = (0x0B, 0x5C, 0x8E, 255)
def draw(n):
    k = 4                                   # (drawn four times as large and brought down: smooth edges)
    im = Image.new('RGBA', (n * k, n * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, n * k - 1, n * k - 1], radius=n * k * 0.19, fill=BLUE)
    f = ImageFont.truetype(FONT, int(n * k * 0.56))
    l, t, r, b = d.textbbox((0, 0), 'AL', font=f)
    w, h = r - l, b - t
    bar = max(k, round(n * k * 0.07))
    gap = round(n * k * 0.08)
    top = (n * k - (h + gap + bar)) / 2
    x = (n * k - w) / 2
    d.text((x - l, top - t), 'AL', font=f, fill=(255, 255, 255, 255))
    d.rectangle([x, top + h + gap, x + w, top + h + gap + bar - 1], fill=(255, 255, 255, 255))
    return im.resize((n, n), Image.LANCZOS)
def entry(im):
    n = im.size[0]
    if n >= 256:
        b = io.BytesIO()
        im.save(b, format='PNG')
        return b.getvalue()
    # a DIB: its header (the height doubled, for the mask that follows), the rows bottom up as BGRA, then a mask
    # of 1 bit a pixel, rows padded to 32 bits (all 0: the alpha says what shows)
    rows = b''.join(bytes(c for px in [im.getpixel((x, y)) for x in range(n)] for c in (px[2], px[1], px[0], px[3]))
                    for y in range(n - 1, -1, -1))
    mask = bytes(((n + 31) // 32) * 4) * n
    return struct.pack('<IiiHHIIiiII', 40, n, 2 * n, 1, 32, 0, len(rows) + len(mask), 0, 0, 0, 0) + rows + mask
sizes = [16, 20, 24, 32, 40, 48, 64, 256]
data = [entry(draw(n)) for n in sizes]
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'config', 'reALcfg.ico')
with open(out, 'wb') as f:
    f.write(struct.pack('<HHH', 0, 1, len(sizes)))
    at = 6 + 16 * len(sizes)
    for n, d in zip(sizes, data):
        f.write(struct.pack('<BBBBHHII', n & 255, n & 255, 0, 0, 1, 32, len(d), at))
        at += len(d)
    for d in data: f.write(d)
print(out, os.path.getsize(out), 'bytes')
