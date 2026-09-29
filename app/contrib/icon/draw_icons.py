#!/usr/bin/env python3
"""Draws the Glissando app icon, and the other designs it was picked from.

The glissando sign in a riveted chrome porthole, as the Chaotica console
would mount it: two notes joined by the wavy slide, the upper note's stem
pointing down, and Queen Arachnia lowering herself from it on a thread, her
hourglass in the console's one red (Chaotica::Colour::Alarm).

    python3 app/contrib/icon/draw_icons.py            # writes the icon set
    python3 app/contrib/icon/draw_icons.py --options  # previews the others here

Needs Pillow. The icon set is app/contrib/glissando{48,64,128,256}.png.
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageChops
import math
N = 1024
C = N / 2
SILVER = (226, 222, 210)
BONE = (238, 232, 216)
RED = (236, 64, 52)

def radial(size, inner, outer, cx=None, cy=None, r=None):
    cx = size / 2 if cx is None else cx; cy = size / 2 if cy is None else cy
    r = size / 2 if r is None else r
    im = Image.new('RGB', (size, size))
    px = im.load()
    for y in range(size):
        for x in range(size):
            t = min(1.0, math.hypot(x - cx, y - cy) / r)
            px[x, y] = tuple(int(inner[i] + (outer[i] - inner[i]) * t) for i in range(3))
    return im

def linear(size, top, bottom):
    im = Image.new('RGB', (size, size))
    d = ImageDraw.Draw(im)
    for y in range(size):
        t = y / (size - 1)
        d.line([(0, y), (size, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return im

def medallion():
    """A black glass face in a chrome bezel with eight rivets."""
    base = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    mask = Image.new('L', (N, N), 0)
    ImageDraw.Draw(mask).ellipse([16, 16, N - 16, N - 16], fill=255)
    chrome = linear(N, (250, 248, 240), (70, 68, 64)).convert('RGBA')
    base.paste(chrome, (0, 0), mask)
    inner = Image.new('L', (N, N), 0)
    ImageDraw.Draw(inner).ellipse([74, 74, N - 74, N - 74], fill=255)
    lip = linear(N, (40, 40, 40), (200, 198, 192)).convert('RGBA')
    base.paste(lip, (0, 0), inner)
    face = Image.new('L', (N, N), 0)
    ImageDraw.Draw(face).ellipse([92, 92, N - 92, N - 92], fill=255)
    glass = radial(N // 4, (38, 36, 36), (6, 6, 7), r=N // 8 * 1.05).resize((N, N), Image.BICUBIC).convert('RGBA')
    base.paste(glass, (0, 0), face)
    d = ImageDraw.Draw(base)
    for k in range(8):
        a = math.pi / 8 + k * math.pi / 4
        x = C + math.cos(a) * (C - 45); y = C + math.sin(a) * (C - 45)
        d.ellipse([x - 13, y - 13, x + 13, y + 13], fill=(120, 118, 112))
        d.ellipse([x - 9, y - 11, x + 7, y + 5], fill=(236, 232, 222))
    return base, face

def glow(layer, radius, strength=1.0):
    blurred = layer.filter(ImageFilter.GaussianBlur(radius))
    if strength != 1.0:
        a = blurred.split()[3].point(lambda v: min(255, int(v * strength)))
        blurred.putalpha(a)
    return blurred

def compose(art, face):
    base, _ = medallion()
    clipped = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    clipped.paste(art, (0, 0), ImageChops.multiply(art.split()[3], face))
    g = glow(clipped, 18, 0.9)
    base.alpha_composite(g)
    base.alpha_composite(clipped)
    # a sheen across the glass
    sheen = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(sheen).ellipse([170, 120, N - 260, 470], fill=(255, 255, 255, 22))
    sheen = sheen.filter(ImageFilter.GaussianBlur(40))
    s2 = Image.new('RGBA', (N, N), (0, 0, 0, 0)); s2.paste(sheen, (0, 0), face)
    base.alpha_composite(s2)
    return base

def notehead(d, x, y, w=62, h=44, tilt=-25, fill=SILVER):
    pts = []
    for k in range(40):
        t = 2 * math.pi * k / 40
        ex, ey = w * math.cos(t), h * math.sin(t)
        a = math.radians(tilt)
        pts.append((x + ex * math.cos(a) - ey * math.sin(a), y + ex * math.sin(a) + ey * math.cos(a)))
    d.polygon(pts, fill=fill)

def spider(d, cx, cy, s=1.0, jewel=True, legs_as_notes=True):
    # Eight legs, one per note of the scale: each a jointed arc ending in a note head,
    # longer as the note rises.
    for side in (-1, 1):
        for i in range(4):
            th = math.radians(-48 + i * 32)          # front legs forward, back legs back
            hip = (cx + side * 40 * s, cy - 40 * s + i * 26 * s)
            l1, l2 = 150 * s, (170 + 14 * i) * s
            ka = th - math.radians(40)                # the knee rides high
            knee = (hip[0] + side * math.cos(ka) * l1, hip[1] + math.sin(ka) * l1)
            fa = th + math.radians(42)
            foot = (knee[0] + side * math.cos(fa) * l2, knee[1] + math.sin(fa) * l2)
            d.line([hip, knee], fill=SILVER, width=int(19 * s))
            d.line([knee, foot], fill=SILVER, width=int(14 * s))
            d.ellipse([knee[0] - 10 * s, knee[1] - 10 * s, knee[0] + 10 * s, knee[1] + 10 * s], fill=SILVER)
            if legs_as_notes:
                notehead(d, foot[0], foot[1], 30 * s, 21 * s, tilt=-25)
    # body
    d.ellipse([cx - 62 * s, cy - 118 * s, cx + 62 * s, cy + 6 * s], fill=SILVER)          # cephalothorax
    d.ellipse([cx - 92 * s, cy - 10 * s, cx + 92 * s, cy + 200 * s], fill=SILVER)          # abdomen
    if jewel:
        # Arachnia's hourglass, in the console's one red
        hx, hy = cx, cy + 95 * s
        d.polygon([(hx - 36 * s, hy - 56 * s), (hx + 36 * s, hy - 56 * s), (hx, hy)], fill=RED)
        d.polygon([(hx - 36 * s, hy + 56 * s), (hx + 36 * s, hy + 56 * s), (hx, hy)], fill=RED)
    # eyes
    for ex in (-22, 22):
        d.ellipse([cx + ex * s - 10 * s, cy - 92 * s, cx + ex * s + 10 * s, cy - 72 * s], fill=(20, 20, 20))

def option_crest():
    base, face = medallion()
    art = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(art)
    # deco sunburst behind the queen
    for k in range(24):
        a = 2 * math.pi * k / 24
        w = math.radians(3.2)
        d.polygon([(C, C + 10), (C + math.cos(a - w) * 470, C + 10 + math.sin(a - w) * 470),
                   (C + math.cos(a + w) * 470, C + 10 + math.sin(a + w) * 470)], fill=(255, 255, 255, 26))
    spider(d, C, C - 30, 0.95)
    return compose(art, face)

def wavy(d, p0, p1, amp, waves, width, fill):
    (x0, y0), (x1, y1) = p0, p1
    L = math.hypot(x1 - x0, y1 - y0); ux, uy = (x1 - x0) / L, (y1 - y0) / L
    pts = []
    for k in range(200):
        t = k / 199
        o = amp * math.sin(t * waves * 2 * math.pi) * math.sin(t * math.pi) ** 0.3
        pts.append((x0 + ux * L * t - uy * o, y0 + uy * L * t + ux * o))
    d.line(pts, fill=fill, width=width, joint='curve')

def option_notation():
    base, face = medallion()
    art = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(art)
    # a staff, faint
    for k in range(5):
        y = 330 + k * 90
        d.line([(150, y), (N - 150, y)], fill=(255, 255, 255, 110), width=6)
    # two notes joined by the glissando line
    notehead(d, 320, 690, 70, 50)
    d.line([(384, 670), (384, 330)], fill=SILVER, width=16)
    notehead(d, 700, 330, 70, 50)
    d.line([(764, 310), (764, -10)], fill=SILVER, width=16)
    wavy(d, (400, 640), (650, 380), 26, 4.5, 16, BONE)
    return compose(art, face)

def option_web():
    base, face = medallion()
    art = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(art)
    cx, cy = C, C - 30
    spokes = 12
    ang = [2 * math.pi * k / spokes - math.pi / 2 for k in range(spokes)]
    for a in ang:
        d.line([(cx, cy), (cx + math.cos(a) * 520, cy + math.sin(a) * 520)], fill=(230, 226, 214, 150), width=6)
    # the spiral: every turn a rising chirp, sagging between spokes
    r = 60
    while r < 440:
        for k in range(spokes):
            a0, a1 = ang[k], ang[(k + 1) % spokes] + (2 * math.pi if k == spokes - 1 else 0)
            r0, r1 = r, r + 34 / spokes
            pts = []
            for j in range(12):
                t = j / 11
                a = a0 + (a1 - a0) * t
                rr = (r0 + (r1 - r0) * t) * (1 - 0.06 * math.sin(math.pi * t))
                pts.append((cx + math.cos(a) * rr, cy + math.sin(a) * rr))
            d.line(pts, fill=(236, 232, 222, 190), width=5)
            r = r1
    # the queen hangs from a thread
    d.line([(cx, cy), (cx, cy + 150)], fill=SILVER, width=5)
    spider(d, cx, cy + 230, 0.55, legs_as_notes=False)
    return compose(art, face)

def option_scope():
    base, face = medallion()
    art = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(art)
    for k in range(1, 8):
        x = 92 + k * (N - 184) / 8
        d.line([(x, 92), (x, N - 92)], fill=(255, 255, 255, 30), width=4)
        d.line([(92, x), (N - 92, x)], fill=(255, 255, 255, 30), width=4)
    # a glide-then-hold melody, as the waterfall shows it
    notes = [640, 520, 580, 420, 460, 330, 380, 260]
    x = 130; y = notes[0]; pts = [(x, y)]
    for n in notes[1:]:
        pts.append((x + 38, n)); x += 38
        pts.append((x + 38, n)); x += 38
    d.line(pts, fill=BONE, width=18, joint='curve')
    # and a finned rocket riding the last note
    rx, ry = x + 50, notes[-1] - 70
    d.polygon([(rx, ry - 70), (rx + 26, ry - 20), (rx + 26, ry + 50), (rx - 26, ry + 50), (rx - 26, ry - 20)], fill=SILVER)
    d.polygon([(rx - 26, ry + 10), (rx - 58, ry + 70), (rx - 26, ry + 50)], fill=SILVER)
    d.polygon([(rx + 26, ry + 10), (rx + 58, ry + 70), (rx + 26, ry + 50)], fill=SILVER)
    d.ellipse([rx - 11, ry - 16, rx + 11, ry + 6], fill=(20, 20, 20))
    d.polygon([(rx - 16, ry + 54), (rx + 16, ry + 54), (rx, ry + 110)], fill=(255, 150, 60))
    return compose(art, face)

def staff(d, alpha=90):
    for k in range(5):
        y = 290 + k * 95
        d.line([(140, y), (N - 140, y)], fill=(255, 255, 255, alpha), width=6)

def notes(d, hx=730, hy=290, stem_end=520):
    # low note, stem up on the right
    notehead(d, 300, 700, 70, 50)
    d.line([(364, 680), (364, 330)], fill=SILVER, width=16)
    # high note, stem down on the left
    notehead(d, hx, hy, 70, 50)
    sx = hx - 64
    d.line([(sx, hy + 12), (sx, stem_end)], fill=SILVER, width=16)
    wavy(d, (385, 640), (hx - 90, hy + 60), 24, 4.5, 15, BONE)
    return sx, stem_end

def arachnia(d, cx, cy, s, legs='hang', rot=0.0):
    """A small Arachnia, head down the thread (cy is her spinnerets' end)."""
    def R(x, y):
        a = math.radians(rot)
        dx, dy = x - cx, y - cy
        return (cx + dx * math.cos(a) - dy * math.sin(a), cy + dx * math.sin(a) + dy * math.cos(a))
    body = []
    for side in (-1, 1):
        for i in range(4):
            if legs == 'splay':
                th = math.radians(-70 + i * 40)
                knee = (cx + side * math.cos(th) * 95 * s, cy + 70 * s + math.sin(th) * 95 * s)
                foot = (knee[0] + side * math.cos(th + 0.5) * 80 * s, knee[1] + math.sin(th + 0.5) * 80 * s)
                hip = (cx + side * 22 * s, cy + 80 * s)
            elif legs == 'grip':
                th = math.radians(-60 + i * 30)
                hip = (cx + side * 22 * s, cy + 50 * s + i * 14 * s)
                knee = (hip[0] + side * 70 * s, hip[1] - 50 * s + i * 22 * s)
                foot = (cx + side * 12 * s, hip[1] - 95 * s + i * 30 * s)
            else:  # hang: legs splayed round her, bent at the knee
                th = math.radians(-55 + i * 36)
                hip = (cx + side * 20 * s, cy + 105 * s + i * 14 * s)
                ka = th - math.radians(38)
                knee = (hip[0] + side * math.cos(ka) * 85 * s, hip[1] + math.sin(ka) * 85 * s)
                fa = th + math.radians(48)
                foot = (knee[0] + side * math.cos(fa) * 95 * s, knee[1] + math.sin(fa) * 95 * s)
            d.line([R(*hip), R(*knee)], fill=SILVER, width=max(3, int(12 * s)))
            d.line([R(*knee), R(*foot)], fill=SILVER, width=max(3, int(9 * s)))
    # abdomen up (toward the thread), head down
    def ell(x0, y0, x1, y1, fill):
        pts = []
        for k in range(36):
            t = 2 * math.pi * k / 36
            pts.append(R((x0 + x1) / 2 + (x1 - x0) / 2 * math.cos(t), (y0 + y1) / 2 + (y1 - y0) / 2 * math.sin(t)))
        d.polygon(pts, fill=fill)
    ell(cx - 58 * s, cy, cx + 58 * s, cy + 130 * s, SILVER)             # abdomen
    ell(cx - 38 * s, cy + 118 * s, cx + 38 * s, cy + 196 * s, SILVER)    # cephalothorax
    hx, hy = cx, cy + 62 * s
    d.polygon([R(hx - 24 * s, hy - 38 * s), R(hx + 24 * s, hy - 38 * s), R(hx, hy)], fill=RED)
    d.polygon([R(hx - 24 * s, hy + 38 * s), R(hx + 24 * s, hy + 38 * s), R(hx, hy)], fill=RED)
    for ex in (-13, 13):
        ell(cx + ex * s - 7 * s, cy + 172 * s, cx + ex * s + 7 * s, cy + 186 * s, (20, 20, 20))

def option_arachnia_glissando(kind='thread'):
    """The glissando sign, the upper note's stem pointing down and Arachnia
    coming off its end: on a thread (the icon), clinging, snapped or on a long
    drop."""
    base, face = medallion()
    art = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(art)
    staff(d)
    if kind == 'thread':
        sx, sy = notes(d)
        d.line([(sx, sy), (sx, sy + 120)], fill=(236, 232, 222, 230), width=5)
        arachnia(d, sx, sy + 118, 0.95, 'hang')
    elif kind == 'cling':
        sx, sy = notes(d, stem_end=600)
        arachnia(d, sx, sy - 70, 1.0, 'grip')
    elif kind == 'fall':
        sx, sy = notes(d)
        # the snapped thread, and the queen tumbling
        d.line([(sx, sy), (sx + 6, sy + 50)], fill=(236, 232, 222, 200), width=5)
        for k in range(3):
            y = sy + 70 + k * 26
            d.line([(sx + 30 + k * 8, y), (sx + 30 + k * 8, y + 14)], fill=(236, 232, 222, 120 - 30 * k), width=4)
        arachnia(d, sx + 60, sy + 110, 0.9, 'splay', rot=28)
    elif kind == 'long':
        # a shorter stem and a long silk drop: she lands on the bottom line of the staff
        sx, sy = notes(d, stem_end=450)
        d.line([(sx, sy), (sx, sy + 190)], fill=(236, 232, 222, 230), width=5)
        arachnia(d, sx, sy + 188, 0.8, 'hang')
    return compose(art, face)


OPTIONS = [('A-crest', option_crest), ('B-notation', option_notation), ('C-web', option_web),
           ('D-scope', option_scope)] + [
    (f'B-{kind}', lambda kind=kind: option_arachnia_glissando(kind))
    for kind in ('thread', 'cling', 'fall', 'long')]

if __name__ == '__main__':
    if '--options' in sys.argv:
        for name, draw in OPTIONS:
            draw().save(f'{name}.png')
        sys.exit(0)
    app = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    icon = option_arachnia_glissando('thread')
    for size in (48, 64, 128, 256):
        icon.resize((size, size), Image.LANCZOS).save(
            os.path.join(app, 'contrib', f'glissando{size}x{size}.png'), optimize=True)
