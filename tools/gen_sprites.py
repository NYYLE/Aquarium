#!/usr/bin/env python3
"""Generate Aquarium watchface sprites (colour + 1-bit B&W) with Pillow.

For every logical image we emit two files in resources/images/:
  <name>.png       -> RGBA colour, used by basalt / chalk / emery
  <name>~bw.png    -> black/white/transparent, auto-picked by aplite / diorite

Fish are drawn facing RIGHT, then flipped to produce the LEFT variant, so the
C code just picks the _r or _l resource based on swim direction.

Run:  python tools/gen_sprites.py
"""

import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "resources", "images"))
os.makedirs(OUT, exist_ok=True)

CLEAR = (0, 0, 0, 0)
BLACK = (0, 0, 0, 255)
WHITE = (255, 255, 255, 255)

# species palettes: (body, fin)  -- goldfish + blue tang
SPECIES = {
    "a": ((255, 150, 20, 255), (235, 90, 0, 255)),
    "b": ((40, 110, 215, 255), (20, 70, 170, 255)),
}

# size key -> (width, height)
SIZES = {
    "baby": (11, 8),
    "med": (18, 12),
    "big": (28, 17),
}


def draw_fish(w, h, body, fin, mono):
    img = Image.new("RGBA", (w, h), CLEAR)
    d = ImageDraw.Draw(img)

    bcol = BLACK if mono else body
    fcol = BLACK if mono else fin

    tail_w = max(2, int(round(w * 0.28)))
    midy = h // 2

    # tail fin (triangle on the left)
    d.polygon([(0, midy), (tail_w, max(1, h // 5)),
               (tail_w, h - 1 - max(1, h // 5))], fill=fcol)

    # body
    d.ellipse([tail_w - 1, 1, w - 2, h - 2], fill=bcol)

    # top dorsal fin
    if h >= 10:
        fx0 = tail_w + (w - tail_w) // 4
        fx1 = fx0 + (w - tail_w) // 3
        d.polygon([(fx0, 2), (fx1, 2), ((fx0 + fx1) // 2, max(0, midy - h // 3))],
                  fill=fcol)

    # eye near the front (right side)
    eye_x = w - max(3, w // 5)
    eye_y = midy - max(1, h // 8)
    if w >= 16:
        d.ellipse([eye_x, eye_y, eye_x + 2, eye_y + 2], fill=WHITE)
        d.point((eye_x + 1, eye_y + 1), fill=BLACK)
    else:
        d.point((eye_x, eye_y), fill=WHITE if mono else BLACK)

    return img


def draw_plant(w, h, blades, mono):
    img = Image.new("RGBA", (w, h), CLEAR)
    d = ImageDraw.Draw(img)
    col = BLACK if mono else (30, 160, 70, 255)
    for i in range(blades):
        bx = int((i + 1) * w / (blades + 1))
        # a wavy blade rising from the bottom
        pts = []
        for t in range(0, h, 2):
            sway = int(2 * ((t // 3) % 3 - 1) * (t / h))
            pts.append((bx + sway, h - 1 - t))
        for (px, py) in pts:
            d.line([(bx, h - 1), (px, py)], fill=col, width=2)
    return img


def draw_rock(w, h, mono):
    img = Image.new("RGBA", (w, h), CLEAR)
    d = ImageDraw.Draw(img)
    base = BLACK if mono else (112, 110, 122, 255)
    hi = WHITE if mono else (168, 166, 182, 255)
    sh = BLACK if mono else (70, 68, 80, 255)

    def box(a, b, c, e):
        return [int(a), int(b), int(c), int(e)]

    # lumpy mass from a few overlapping ellipses
    d.ellipse(box(2, h * 0.35, w - 3, h - 1), fill=base)
    d.ellipse(box(w * 0.08, h * 0.18, w * 0.66, h - 1), fill=base)
    d.ellipse(box(w * 0.42, h * 0.30, w - 3, h - 1), fill=base)
    if not mono:
        d.ellipse(box(2, h * 0.62, w - 3, h - 1), fill=sh)          # base shadow
        d.ellipse(box(w * 0.1, h * 0.18, w * 0.6, h * 0.72), fill=base)
        d.ellipse(box(w * 0.2, h * 0.24, w * 0.33, h * 0.38), fill=hi)  # gloss
        d.ellipse(box(w * 0.52, h * 0.34, w * 0.62, h * 0.46), fill=hi)
    else:
        d.ellipse(box(w * 0.26, h * 0.30, w * 0.33, h * 0.40), fill=WHITE)
    return img


def draw_octopus(frame, mono):
    w, h = 36, 34
    img = Image.new("RGBA", (w, h), CLEAR)
    d = ImageDraw.Draw(img)
    body = BLACK if mono else (172, 96, 206, 255)
    dark = BLACK if mono else (132, 62, 168, 255)

    # mantle / head + body skirt
    d.ellipse([4, 1, w - 5, int(h * 0.60)], fill=body)
    d.rectangle([7, int(h * 0.40), w - 8, int(h * 0.58)], fill=body)

    # tentacles: amplitude grows downward, sway flips between the two frames
    n = 5
    top = int(h * 0.54)
    for i in range(n):
        bx = 8 + i * (w - 16) // (n - 1)
        phase = 1 if (i + frame) % 2 == 0 else -1
        for t in range(0, h - top):
            sway = int(phase * (t / max(1, h - top)) * 4)
            px, py = bx + sway, top + t
            d.ellipse([px - 1, py - 1, px + 1, py + 1], fill=body)

    if not mono:
        d.ellipse([8, int(h * 0.18), w - 9, int(h * 0.5)], fill=body)   # round head
        d.arc([10, 2, w - 11, int(h * 0.5)], 200, 340, fill=dark)       # subtle shade

    # eyes
    for ex in (int(w * 0.37), int(w * 0.60)):
        ey = int(h * 0.27)
        d.ellipse([ex - 3, ey - 3, ex + 2, ey + 2], fill=WHITE)
        d.point((ex - 1, ey), fill=BLACK)
    return img


def save(img, name):
    color_path = os.path.join(OUT, name + ".png")
    bw_path = os.path.join(OUT, name + "~bw.png")
    img["color"].save(color_path)
    img["bw"].save(bw_path)
    print("  ", os.path.basename(color_path), "+", os.path.basename(bw_path))


def main():
    print("Generating sprites into", OUT)
    for sp, (body, fin) in SPECIES.items():
        for sz, (w, h) in SIZES.items():
            color = draw_fish(w, h, body, fin, mono=False)
            bw = draw_fish(w, h, body, fin, mono=True)
            save({"color": color, "bw": bw}, "fish_%s_%s_r" % (sp, sz))
            save({"color": color.transpose(Image.FLIP_LEFT_RIGHT),
                  "bw": bw.transpose(Image.FLIP_LEFT_RIGHT)},
                 "fish_%s_%s_l" % (sp, sz))

    plants = [("plant_1", 16, 34, 3), ("plant_2", 22, 22, 4)]
    for name, w, h, blades in plants:
        save({"color": draw_plant(w, h, blades, mono=False),
              "bw": draw_plant(w, h, blades, mono=True)}, name)

    save({"color": draw_rock(44, 34, mono=False),
          "bw": draw_rock(44, 34, mono=True)}, "rock")

    for frame in (0, 1):
        save({"color": draw_octopus(frame, mono=False),
              "bw": draw_octopus(frame, mono=True)}, "octopus_f%d" % frame)

    print("Done.")


if __name__ == "__main__":
    main()
