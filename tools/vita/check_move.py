#!/usr/bin/env python3
"""Verify that a refactor only MOVED code (VITA-51).

    check_move.py <rev>:<old file> [--old <rev>:<old file> ...] <new file> [<new file> ...] [--allow 'regex' ...]
                  [--allow-lost 'regex' ...]

Several --old files are summed (code moved out of two files into one).

Compares the code lines that disappeared from the old file with the code lines that appeared in the
new files (the shrunk original counts as a new file too). Blank lines, `#include` lines and
`#pragma once` are ignored, and so are plain `//` comments (a new file's preamble is not code; `///`
doc comments are still compared). Lines ADDED that match an --allow regex are expected edits (for
example `static` turned into `inline`, namespace lines of a new file) and are reported, not failed.
A line LOST is failed unless named with --allow-lost (the old spelling of an edited line). Exit status 0 only if every removed line reappears
exactly as many times as it was removed and nothing else was added.

    tools/vita/check_move.py origin/vita:src/rendering/wmo_renderer.cpp \
        src/rendering/wmo_renderer.cpp src/rendering/wmo_renderer_collision.cpp include/rendering/wmo_ray_helpers.hpp
"""
import re
import subprocess
import sys
from collections import Counter

args = sys.argv[1:]
allow = []
allow_lost = []
while '--allow-lost' in args:
    i = args.index('--allow-lost')
    allow_lost.append(re.compile(args[i + 1]))
    del args[i:i + 2]
while '--allow' in args:
    i = args.index('--allow')
    allow.append(re.compile(args[i + 1]))
    del args[i:i + 2]
old_specs = [args[0]]
while '--old' in args:
    i = args.index('--old')
    old_specs.append(args[i + 1])
    del args[i:i + 2]
new_files = args[1:]
old = []
for spec in old_specs:
    rev, path = spec.split(':', 1)
    old += subprocess.run(['git', 'show', f'{rev}:{path}'], capture_output=True, text=True, check=True).stdout.split('\n')


def norm(lines):
    out = Counter()
    for l in lines:
        t = l.strip()
        if not t or t.startswith('#include') or t == '#pragma once':
            continue
        if t.startswith('//') and not t.startswith('///'):
            continue
        out[t] += 1
    return out


old_c = norm(old)
new_c = Counter()
for f in new_files:
    new_c += norm(open(f).read().split('\n'))
removed = old_c - new_c   # in old, missing from the new set
added = new_c - old_c     # in the new set, not in old
ok = True


def report(title, c):
    global ok
    bad = []
    for line, n in c.items():
        if any(a.search(line) for a in allow):
            print(f'  allowed  x{n}  {line[:100]}')
        else:
            bad.append((line, n))
    if bad:
        ok = False
        print(f'{title}: {sum(n for _, n in bad)} line(s)')
        for line, n in bad[:40]:
            print(f'  x{n}  {line[:110]}')


print(f'old {sum(old_c.values())} code lines, new {sum(new_c.values())}')
_allow, allow = allow, allow_lost
report('LOST (in the old file, nowhere in the new ones)', removed)
allow = _allow
report('ADDED (in the new files, not in the old one)', added)
print('PURE MOVE' if ok else 'NOT A PURE MOVE')
sys.exit(0 if ok else 1)
