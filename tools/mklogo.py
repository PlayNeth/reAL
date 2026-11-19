"""docs/logo.png and docs/logo-dark.png: the wordmark for the README, as the configurator draws it in its header
(re in ink, AL in marine blue, a bar under both), on a transparent ground; the dark one for GitHub's dark theme,
with the lighter blue the configurator uses on a dark ground.

    python3 tools/mklogo.py [a bold sans font file]      (needs Pillow; Inter Bold was used)"""
import sys, os
from PIL import Image, ImageDraw, ImageFont
FONT = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/fonts/opentype/inter/Inter-Bold.otf'
def draw(ink, blue, path, size=176):
    k = 4                                   # (drawn four times as large and brought down: smooth edges)
    f = ImageFont.truetype(FONT, size * k)
    probe = ImageDraw.Draw(Image.new('RGBA', (1, 1)))
    rl, rt, rr, rb = probe.textbbox((0, 0), 're', font=f)
    al = probe.textbbox((0, 0), 'AL', font=f)
    re_w = probe.textlength('re', font=f)
    top = min(rt, al[1]); bottom = max(rb, al[3])
    pad = size * k // 12
    gap = round(size * k * 0.10); bar = round(size * k * 0.075)
    w = round(re_w + al[2]) - rl + 2 * pad
    h = bottom - top + gap + bar + 2 * pad
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x0, y0 = pad - rl, pad - top
    d.text((x0, y0), 're', font=f, fill=ink)
    d.text((x0 + re_w, y0), 'AL', font=f, fill=blue)
    y = pad + bottom - top + gap
    d.rectangle([pad, y, w - pad - 1, y + bar - 1], fill=blue)
    im.resize((w // k, h // k), Image.LANCZOS).save(path, optimize=True)
here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs')
draw((0x1F, 0x23, 0x28, 255), (0x0B, 0x5C, 0x8E, 255), os.path.join(here, 'logo.png'))
draw((0xE6, 0xED, 0xF3, 255), (0x4F, 0xA8, 0xDB, 255), os.path.join(here, 'logo-dark.png'))
