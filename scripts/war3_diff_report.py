#!/usr/bin/env python
"""Tables a `war3-diff.ps1` run: draw counts, pixel distances, a side-by-side sheet.

usage: python war3_diff_report.py <run dir>

Per model folder it reads `native*.raw`, `ours*.raw` and `oracle*.raw` (the
512x512 RGBA readbacks `--draw-trace-golden` writes), the `[dtrace]` summary
line out of each `.log`, and any `pstats_*` particle dumps; it writes
`report.md` and `sheet.png` (one row per model and view: native | ours | oracle)
into the run folder. Mean absolute difference is over RGB, 0..255, on the whole
frame; a black frame is reported, because a pair of empty frames is a perfect
match about nothing.
"""
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

SIZE = 512
DTRACE = re.compile(r'\[dtrace\] (\d+) draw\(s\) over (\d+) frame\(s\); kinds geoset=(\d+) '
                    r'particle=(\d+) ribbon=(\d+) corn=(\d+) sc2par=(\d+)')
EMITTER = re.compile(r'^\s*em\s+(\d+) out=\d+ alive=\s*(\d+) verts=\s*(\d+) centre=\(\s*([-\d.]+)\s+'
                     r'([-\d.]+)\s+([-\d.]+)\) extent=\(\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\) '
                     r'rgba=\(([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)\)')


def load_raw(path):
    if not os.path.exists(path):
        return None
    data = np.fromfile(path, dtype=np.uint8)
    if data.size != SIZE * SIZE * 4:
        return None
    return data.reshape(SIZE, SIZE, 4)


def dtrace(path):
    if not os.path.exists(path):
        return None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = DTRACE.search(line)
            if m:
                return tuple(int(g) for g in m.groups())
    return None


def mad(a, b):
    if a is None or b is None:
        return None
    return float(np.abs(a[..., :3].astype(np.int16) - b[..., :3].astype(np.int16)).mean())


def blank(img):
    return img is None or int(img[..., :3].max()) == 0


def emitter_rows(path):
    rows = []
    if not os.path.exists(path):
        return rows
    in_frame = False
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('[ptrace] frame'):
                in_frame = True
                continue
            if in_frame:
                m = EMITTER.match(line)
                if m:
                    g = m.groups()
                    rows.append({'em': int(g[0]), 'alive': int(g[1]), 'verts': int(g[2]),
                                 'extent': tuple(float(x) for x in g[6:9]),
                                 'rgba': tuple(float(x) for x in g[9:13])})
    return rows


def fmt(x):
    return '-' if x is None else f'{x:.2f}'


def main():
    run = sys.argv[1]
    models = sorted(d for d in os.listdir(run) if os.path.isdir(os.path.join(run, d)))
    lines = ['# war3-diff ' + os.path.basename(os.path.normpath(run)), '',
             '| model | view | draws n / o / b | sc2par o / b | particle n | ribbon n | n-o | o-b | n-b | blank |',
             '|---|---|---|---|---|---|---|---|---|---|']
    sheet_rows = []
    for model in models:
        folder = os.path.join(run, model)
        views = sorted({re.sub(r'^native|\.raw$', '', f) for f in os.listdir(folder)
                        if f.startswith('native') and f.endswith('.raw')})
        for view in views:
            n = load_raw(os.path.join(folder, f'native{view}.raw'))
            o = load_raw(os.path.join(folder, f'ours{view}.raw'))
            b = load_raw(os.path.join(folder, f'oracle{view}.raw'))
            tn = dtrace(os.path.join(folder, f'native{view}.log'))
            to = dtrace(os.path.join(folder, f'ours{view}.log'))
            tb = dtrace(os.path.join(folder, f'oracle{view}.log'))
            blanks = ','.join(k for k, img in (('n', n), ('o', o), ('b', b)) if blank(img))
            lines.append('| {} | {} | {} / {} / {} | {} / {} | {} | {} | {} | {} | {} | {} |'.format(
                model, view or '-', tn[0] if tn else '-', to[0] if to else '-', tb[0] if tb else '-',
                to[6] if to else '-', tb[6] if tb else '-', tn[3] if tn else '-', tn[4] if tn else '-',
                fmt(mad(n, o)), fmt(mad(o, b)), fmt(mad(n, b)), blanks or ''))
            sheet_rows.append((f'{model}{view}', n, o, b))

    stats = []
    for model in models:
        folder = os.path.join(run, model)
        for f in sorted(os.listdir(folder)):
            m = re.match(r'pstats_native_(\d+)\.txt$', f)
            if not m:
                continue
            frames = m.group(1)
            nat = emitter_rows(os.path.join(folder, f))
            ours = emitter_rows(os.path.join(folder, f'pstats_ours_{frames}.txt'))
            stats.append((model, frames, nat, ours))
    if stats:
        lines += ['', '## Particle statistics (last frame of each run)', '',
                  '| model | frames | emitters n / o | alive n / o | verts n / o | mean extent n | mean extent o |',
                  '|---|---|---|---|---|---|---|']
        for model, frames, nat, ours in stats:
            def total(rows, key):
                return sum(r[key] for r in rows)

            def extent(rows):
                live = [r['extent'] for r in rows if r['verts'] > 0]
                if not live:
                    return '-'
                return '({:.1f} {:.1f} {:.1f})'.format(*[sum(e[i] for e in live) / len(live) for i in range(3)])
            lines.append(f'| {model} | {frames} | {len(nat)} / {len(ours)} | {total(nat, "alive")} / '
                         f'{total(ours, "alive")} | {total(nat, "verts")} / {total(ours, "verts")} | '
                         f'{extent(nat)} | {extent(ours)} |')

    with open(os.path.join(run, 'report.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')

    if sheet_rows:
        thumb = 256
        label = 18
        sheet = Image.new('RGB', (thumb * 3, (thumb + label) * len(sheet_rows)), (30, 30, 30))
        draw = ImageDraw.Draw(sheet)
        for r, (name, *imgs) in enumerate(sheet_rows):
            y = r * (thumb + label)
            draw.text((4, y + 2), f'{name}   native | ours | oracle', fill=(230, 230, 230))
            for c, img in enumerate(imgs):
                if img is None:
                    continue
                tile = Image.fromarray(img[..., :3].copy(), 'RGB').resize((thumb, thumb))
                sheet.paste(tile, (c * thumb, y + label))
        sheet.save(os.path.join(run, 'sheet.png'))


if __name__ == '__main__':
    main()
