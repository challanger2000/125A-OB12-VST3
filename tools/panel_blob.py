# Front panel vectors from manual page 7 into a compact file for the Direct2D view.
# The art belongs to the manual, so the result stays beside the ROMs, not in this repo.
# usage: panel_blob.py <manual.pdf> <out file>
# Format, little endian: "OB12" u32 runs, f32 width, f32 height; per run: u32 argb, u8 mode
# (1 fill, 2 fill even-odd, 3 stroke, 4 push clip, 5 pop clip), f32 stroke width, u32 commands; per command: u8 kind
# (0 move, 1 line, 2 cubic, 3 close) and 1, 1, 3 or 0 points of two f32. Consecutive paths of one
# style share a run, so Direct2D builds far fewer geometries.
import struct, sys
import fitz

CLIP = fitz.Rect(68, 186, 540, 312)
page = fitz.open(sys.argv[1])[6]

def argb(color, opacity):
    r, g, b = [int(round(c * 255)) for c in color]
    a = int(round((1 if opacity is None else opacity) * 255))
    return a << 24 | r << 16 | g << 8 | b

def path(items):
    cmds, last = [], None
    def move(p):
        nonlocal last
        if last is None or abs(p.x - last.x) > 1e-4 or abs(p.y - last.y) > 1e-4:
            cmds.append((0, [p]))
    for it in items:
        kind = it[0]
        if kind == 'l':
            move(it[1]); cmds.append((1, [it[2]])); last = it[2]
        elif kind == 'c':
            move(it[1]); cmds.append((2, [it[2], it[3], it[4]])); last = it[4]
        elif kind == 're':
            r = it[1]
            cmds.append((0, [r.tl])); cmds.append((1, [r.tr])); cmds.append((1, [r.br])); cmds.append((1, [r.bl])); cmds.append((3, []))
            last = None
        elif kind == 'qu':
            q = it[1]
            cmds.append((0, [q.ul])); cmds.append((1, [q.ur])); cmds.append((1, [q.lr])); cmds.append((1, [q.ll])); cmds.append((3, []))
            last = None
    return cmds

runs = []
stack = []  # levels of open clips

def pop_to(level):
    while stack and stack[-1] >= level:
        stack.pop()
        runs.append(((0, 5, 0.0), []))

for g in page.get_drawings(extended=True):
    level = g.get('level', 0)
    kind = g.get('type')
    if kind == 'group':
        continue
    pop_to(level)
    if kind == 'clip':
        stack.append(level)
        runs.append(((0, 4, 0.0), path(g['items'])))
        continue
    if not g['rect'].intersects(CLIP):
        continue
    cmds = path(g['items'])
    styles = []
    if g.get('fill') is not None and 'f' in kind:
        styles.append((argb(g['fill'], g.get('fill_opacity')), 2 if g.get('even_odd') else 1, 0.0))
    if g.get('color') is not None and 's' in kind:
        styles.append((argb(g['color'], g.get('stroke_opacity')), 3, float(g.get('width') or 0.5)))
    for style in styles:
        if runs and runs[-1][0] == style:
            runs[-1][1].extend(cmds)
        else:
            runs.append((style, list(cmds)))
pop_to(0)
# Clips with nothing inside cost time for nothing.
kept = []
for r in runs:
    if r[0][1] == 5 and kept and kept[-1][0][1] == 4:
        kept.pop()
    else:
        kept.append(r)
runs = kept

blob = bytearray(b'OB12' + struct.pack('<Iff', len(runs), CLIP.width, CLIP.height))
total = 0
for (color, mode, width), cmds in runs:
    blob += struct.pack('<IBfI', color, mode, width, len(cmds))
    total += len(cmds)
    for kind, points in cmds:
        blob.append(kind)
        for p in points:
            blob += struct.pack('<ff', p.x - CLIP.x0, p.y - CLIP.y0)
open(sys.argv[2], 'wb').write(blob)
print('runs', len(runs), 'commands', total, 'bytes', len(blob))
