#!/usr/bin/env python3
"""Make a Vulkan-free copy of a rendering header for the collision check (VITA-51).

    make_shadow.py <real header> <output header> [--drop NAME ...]

--drop NAME removes every declaration or include that mentions NAME as well: for GPU-only headers that
include Vulkan themselves (shadow_params.hpp) and the types they define (ShadowParamsSet).

Deletes every declaration that mentions Vulkan (Vk*, Vma*, VK_*, vk_*, vulkan) and the Vulkan includes,
keeping the rest of the class (the CPU members and the nested types) exactly as upstream wrote it, so
the copy cannot drift from the real header. The result is what a build without Vulkan sees as
`rendering/<header>`: it is a stand-in for the shadow headers of ADR-001 / VITA-52, used only to prove
that the *_collision.cpp files need nothing the GPU half owns.
"""
import re
import sys

TOKEN = re.compile(r'\b(Vk[A-Z]\w*|Vma\w*|VK_\w+|vk[A-Z_]\w*|vulkan\w*)\b|vulkan/|vk_mem_alloc|rendering/vk_|<vulkan')

def strip(lines):
    out, i = [], 0
    while i < len(lines):
        line = lines[i]
        code = re.sub(r'//.*', '', line)
        if TOKEN.search(code) or (line.lstrip().startswith('#include') and TOKEN.search(line)):
            # drop the whole declaration: continue until a line that ends the statement
            while i < len(lines) and not re.search(r'[;{}]\s*(//.*)?$', re.sub(r'/\*.*?\*/', '', lines[i])) \
                    and not lines[i].lstrip().startswith('#'):
                i += 1
            i += 1
            continue
        out.append(line)
        i += 1
    return out

args = sys.argv[1:]
extra = []
while '--drop' in args:
    i = args.index('--drop')
    extra.append(args[i + 1])
    del args[i:i + 2]
if extra:
    TOKEN = re.compile(TOKEN.pattern + '|' + '|'.join(re.escape(e) for e in extra))
src, dst = args[0], args[1]
text = open(src).read().split('\n')
open(dst, 'w').write('\n'.join(strip(text)))
