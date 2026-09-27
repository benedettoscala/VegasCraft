"""Generates the Pip-Boy's Minecraft models and texture (assets/vegascraft): run after editing.

The model keeps the window plane the native side maps onto New Vegas' Pip-Boy screen (x 3..13,
y 4..12 at z 8.5, model units; NvArmPose.applyPipBoy). Around it: a casing, a bezel with the
labels, three buttons (STATS, ITEMS, DATA; lit when their menu is open), a knob, a ridged wheel,
a vented side and a lamp. The buttons and the knob are separate models, so the client can press
and turn them.
"""
import json
import os
import random
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fabric', 'src', 'main', 'resources', 'assets', 'vegascraft')
SIZE = 128
rnd = random.Random(3000)

# ---- texture atlas -------------------------------------------------------------------------
img = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
px = img.load()

def fill(x0, y0, x1, y1, c):
    for y in range(y0, y1):
        for x in range(x0, x1):
            px[x, y] = c

def shade(c, k):
    return tuple(max(0, min(255, int(v * k))) for v in c[:3]) + (255,)

def metal(x0, y0, x1, y1, base, wear=0.07):
    for y in range(y0, y1):
        for x in range(x0, x1):
            k = 1.0 + rnd.uniform(-wear, wear)
            if x == x0 or y == y0:
                k *= 1.15
            if x == x1 - 1 or y == y1 - 1:
                k *= 0.82
            px[x, y] = shade(base, k)

AMBER = (255, 182, 66, 255)
DIM = (176, 122, 38, 255)
R = {}

def region(name, x0, y0, x1, y1):
    R[name] = (x0, y0, x1, y1)

region('metal_dark', 0, 0, 32, 32); metal(0, 0, 32, 32, (58, 62, 58, 255))
region('metal_light', 32, 0, 64, 32); metal(32, 0, 64, 32, (92, 96, 88, 255))
# Bezel strips: the text is added below.
region('label_bottom', 0, 32, 112, 40); metal(0, 32, 112, 40, (46, 50, 46, 255), 0.04)
region('label_top', 0, 40, 112, 48); metal(0, 40, 112, 48, (46, 50, 46, 255), 0.04)
region('btn_off', 64, 0, 72, 8); fill(64, 0, 72, 8, (92, 44, 24, 255)); fill(65, 1, 71, 4, (130, 66, 34, 255))
region('btn_on', 72, 0, 80, 8); fill(72, 0, 80, 8, (255, 168, 54, 255)); fill(73, 1, 79, 4, (255, 222, 150, 255))
region('btn_side', 80, 0, 88, 8); fill(80, 0, 88, 8, (40, 42, 40, 255))
region('knob_top', 64, 8, 80, 24); metal(64, 8, 80, 24, (70, 74, 70, 255), 0.05)
for i in range(16):  # a pointer notch on the knob
    px[71, 8 + i if i < 7 else 8] = (255, 182, 66, 255) if i < 7 else px[71, 8]
region('knob_side', 80, 8, 96, 24); metal(80, 8, 96, 24, (44, 46, 44, 255), 0.05)
region('ridges', 96, 0, 128, 32)
for x in range(96, 128):
    for y in range(32):
        px[x, y] = shade((74, 78, 72, 255), 1.18 if (x - 96) % 4 < 2 else 0.78)
region('vents', 0, 48, 32, 80); metal(0, 48, 32, 80, (50, 54, 50, 255), 0.04)
for y in range(52, 78, 5):
    fill(2, y, 30, y + 2, (14, 16, 14, 255))
region('lamp', 64, 24, 72, 32); fill(64, 24, 72, 32, (255, 200, 90, 255)); fill(66, 26, 70, 30, (255, 244, 200, 255))

# 3x5 pixel font for the labels.
GLYPHS = {
    'S': ['111', '100', '111', '001', '111'], 'T': ['111', '010', '010', '010', '010'],
    'A': ['010', '101', '111', '101', '101'], 'I': ['111', '010', '010', '010', '111'],
    'E': ['111', '100', '110', '100', '111'], 'M': ['101', '111', '111', '101', '101'],
    'D': ['110', '101', '101', '101', '110'], 'P': ['110', '101', '110', '100', '100'],
    'B': ['110', '101', '110', '101', '110'], 'O': ['111', '101', '101', '101', '111'],
    'Y': ['101', '101', '010', '010', '010'], '3': ['111', '001', '011', '001', '111'],
    '0': ['111', '101', '101', '101', '111'], '-': ['000', '000', '111', '000', '000'], ' ': ['000'] * 5,
}

def text(rx0, ry0, x, y, s, color):
    for ch in s:
        g = GLYPHS[ch]
        for gy in range(5):
            for gx in range(3):
                if g[gy][gx] == '1':
                    px[rx0 + x + gx, ry0 + y + gy] = color
        x += 4

def centred(rx0, ry0, centre, s, color):
    text(rx0, ry0, int(centre - (len(s) * 4 - 1) / 2), 1, s, color)

# Button centres (model x): 5, 8, 11; the label strips span x 1..15 (8 texels per unit), 1 unit tall.
for cx, label in ((5, 'STATS'), (8, 'ITEMS'), (11, 'DATA')):
    centred(0, 32, (cx - 1) * 8, label, AMBER)
text(0, 40, 4, 1, 'PIP-BOY 3000', DIM)
img.save(os.path.join(ROOT, 'textures', 'item', 'pipboy.png'))

# ---- models --------------------------------------------------------------------------------
FACES = ('north', 'south', 'east', 'west', 'up', 'down')

def uv(name):
    x0, y0, x1, y1 = R[name]
    k = 16.0 / SIZE
    return [x0 * k, y0 * k, x1 * k, y1 * k]

def box(frm, to, tex, front=None, rotation=None):
    faces = {}
    for f in FACES:
        name = front if (f == 'south' and front) else tex
        faces[f] = {'uv': uv(name), 'texture': '#t'}
    e = {'from': frm, 'to': to, 'faces': faces}
    if rotation:
        e['rotation'] = rotation
    return e

def model(elements):
    return {'textures': {'t': 'vegascraft:item/pipboy', 'particle': 'vegascraft:item/pipboy'}, 'elements': elements}

def write_model(name, m):
    with open(os.path.join(ROOT, 'models', 'item', name + '.json'), 'w') as f:
        json.dump(m, f, indent=1)
    with open(os.path.join(ROOT, 'items', name + '.json'), 'w') as f:
        json.dump({'model': {'type': 'minecraft:model', 'model': 'vegascraft:item/' + name}}, f, indent=2)

body = [
    box([0.5, 1.4, 1.0], [15.5, 13.3, 8.0], 'metal_dark'),                          # casing
    box([2.0, 12.0, 8.0], [14.0, 13.3, 9.4], 'metal_light', 'label_top'),            # bezel, top (the name plate)
    box([2.0, 3.2, 8.0], [14.0, 4.0, 9.4], 'metal_light', 'label_bottom'),           # bezel, bottom (the labels)
    box([2.0, 4.0, 8.0], [3.0, 12.0, 9.4], 'metal_light'),                           # bezel, left
    box([13.0, 4.0, 8.0], [14.0, 12.0, 9.4], 'metal_light'),                         # bezel, right
    box([0.5, 1.4, 8.0], [15.5, 3.2, 8.8], 'metal_light'),                           # the buttons' panel
    box([-1.6, 5.0, 3.0], [0.5, 12.0, 7.0], 'metal_light', None),                    # wheel housing
    box([-2.4, 5.5, 3.5], [-1.6, 11.5, 6.5], 'ridges', 'ridges'),                    # the ridged wheel
    box([15.5, 2.5, 2.5], [16.9, 12.0, 7.0], 'vents', 'vents'),                      # vented side
    box([11.0, 13.3, 4.5], [13.5, 14.4, 7.5], 'lamp'),                               # lamp
]
write_model('pipboy', model(body))

for name, cx in (('stats', 5.0), ('items', 8.0), ('data', 11.0)):
    for lit in (False, True):
        e = box([cx - 1.2, 1.9, 8.8], [cx + 1.2, 3.0, 10.2], 'btn_side', 'btn_on' if lit else 'btn_off')
        write_model('pipboy_btn_' + name + ('_lit' if lit else ''), model([e]))

# The knob: an octagon (a square and the same turned 45 degrees), centre (0.5, 0.2), turned by the client.
KX, KY = 1.4, 2.4
knob = [
    box([KX - 1.0, KY - 1.0, 8.8], [KX + 1.0, KY + 1.0, 10.2], 'knob_side', 'knob_top'),
    box([KX - 1.0, KY - 1.0, 8.8], [KX + 1.0, KY + 1.0, 10.2], 'knob_side', 'knob_top', {'origin': [KX, KY, 8.8], 'axis': 'z', 'angle': 45}),
]
write_model('pipboy_knob', model(knob))
print('Pip-Boy models and texture written to', os.path.normpath(ROOT))
