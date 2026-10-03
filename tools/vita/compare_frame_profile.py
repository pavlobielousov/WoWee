#!/usr/bin/env python3
"""Compare two WOWEE_FRAME_PROFILE logs (VITA-12): a baseline run and a run with a change.

    tools/vita/compare_frame_profile.py <baseline.log> <new.log> [--tolerance 8]

Reads the lines the client writes every 10 seconds under WOWEE_FRAME_PROFILE=1:
    frame budget over 10s: 600 frames, 60.0 fps, 16.6ms each
      stageName: 0.42ms/frame avg, 3.1ms worst, 2% of the window
Averages each stage over all windows of a log (the first window is skipped: start-up), then prints both side by side
with the change. A stage is flagged when it is slower by more than --tolerance percent AND by more than 0.05 ms.
Exit status 1 if the whole frame or any stage is flagged. Run both logs in the same scene on the same machine; a
change inside the machine's own run-to-run noise (usually a few percent) is not a finding: repeat both runs.
"""
import re
import sys
from collections import defaultdict

HEAD = re.compile(r'frame budget over (\d+)s: (\d+) frames, ([\d.]+) fps, ([\d.]+)ms each')
STAGE = re.compile(r'^\s*(?:\[[^\]]*\]\s*)*(?:\[\w+\s*\]\s*)?\s*([A-Za-z0-9_ ./:+-]+?): ([\d.]+)ms/frame avg, ([\d.]+)ms worst')


def parse(path):
    windows = []  # each: {'frame': ms, 'fps': fps, 'stages': {name: (avg, worst)}}
    cur = None
    for line in open(path, errors='replace'):
        m = HEAD.search(line)
        if m:
            cur = {'frame': float(m.group(4)), 'fps': float(m.group(3)), 'stages': {}}
            windows.append(cur)
            continue
        m = STAGE.match(line.rstrip('\n'))
        if m and cur is not None:
            cur['stages'][m.group(1).strip()] = (float(m.group(2)), float(m.group(3)))
    return windows[1:] if len(windows) > 2 else windows


def summarize(windows):
    if not windows:
        return None
    n = len(windows)
    frame = sum(w['frame'] for w in windows) / n
    fps = sum(w['fps'] for w in windows) / n
    acc = defaultdict(lambda: [0.0, 0.0, 0])
    for w in windows:
        for name, (avg, worst) in w['stages'].items():
            a = acc[name]
            a[0] += avg
            a[1] = max(a[1], worst)
            a[2] += 1
    stages = {k: (v[0] / v[2], v[1]) for k, v in acc.items()}
    return {'windows': n, 'frame': frame, 'fps': fps, 'stages': stages}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    tol = 8.0
    if '--tolerance' in sys.argv:
        tol = float(sys.argv[sys.argv.index('--tolerance') + 1])
        args = [a for a in args if a != str(tol) and a != sys.argv[sys.argv.index('--tolerance') + 1]]
    if len(args) != 2:
        print(__doc__)
        return 2
    base, new = summarize(parse(args[0])), summarize(parse(args[1]))
    if not base or not new:
        print('no "frame budget" lines found: run with WOWEE_FRAME_PROFILE=1 for at least 30 seconds')
        return 2
    flagged = False
    print(f'baseline: {base["windows"]} window(s), {base["fps"]:.1f} fps, {base["frame"]:.2f} ms/frame')
    print(f'new     : {new["windows"]} window(s), {new["fps"]:.1f} fps, {new["frame"]:.2f} ms/frame')
    d = (new['frame'] - base['frame']) / base['frame'] * 100 if base['frame'] else 0
    mark = ''
    if d > tol and new['frame'] - base['frame'] > 0.05:
        mark = '   <-- SLOWER'
        flagged = True
    print(f'whole frame: {base["frame"]:.2f} -> {new["frame"]:.2f} ms ({d:+.1f} %){mark}')
    print(f'{"stage":34} {"base ms":>9} {"new ms":>9} {"change":>9} {"worst base":>11} {"worst new":>10}')
    names = sorted(set(base['stages']) | set(new['stages']), key=lambda k: -base['stages'].get(k, (0, 0))[0])
    for name in names:
        b = base['stages'].get(name)
        n = new['stages'].get(name)
        if b is None or n is None:
            print(f'{name[:34]:34} {"-" if b is None else f"{b[0]:.3f}":>9} {"-" if n is None else f"{n[0]:.3f}":>9}   (stage only in one log)')
            continue
        pct = (n[0] - b[0]) / b[0] * 100 if b[0] else 0
        flag = ''
        if pct > tol and n[0] - b[0] > 0.05:
            flag = '  <-- SLOWER'
            flagged = True
        print(f'{name[:34]:34} {b[0]:9.3f} {n[0]:9.3f} {pct:+8.1f}% {b[1]:11.2f} {n[1]:10.2f}{flag}')
    print('RESULT: ' + ('something is slower than the tolerance: repeat both runs before believing it' if flagged else 'within tolerance'))
    return 1 if flagged else 0


if __name__ == '__main__':
    sys.exit(main())
