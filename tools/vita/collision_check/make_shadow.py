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

def strip(lines, token=None):
    tok = token or TOKEN
    out, i = [], 0
    while i < len(lines):
        line = lines[i]
        code = re.sub(r'//.*', '', line)
        if tok.search(code) or (line.lstrip().startswith('#include') and tok.search(line)):
            # drop the whole declaration: until the braces balance and the statement ends, so a
            # dropped `struct X {` takes its body with it
            depth = 0
            while i < len(lines):
                c = re.sub(r'/\*.*?\*/', '', re.sub(r'//.*', '', lines[i]))
                depth += c.count('{') - c.count('}')
                done = depth <= 0 and (re.search(r'[;}]\s*$', c) or lines[i].lstrip().startswith('#'))
                i += 1
                if done:
                    break
            continue
        out.append(line)
        i += 1
    return out

def make_token(extra):
    base = r'\b(Vk[A-Z]\w*|Vma\w*|VK_\w+|vk[A-Z_]\w*|vulkan\w*)\b|vulkan/|vk_mem_alloc|rendering/vk_|<vulkan'
    return re.compile(base + ''.join('|' + re.escape(e) for e in extra))


def main():
    args = sys.argv[1:]
    extra = []
    while '--drop' in args:
        i = args.index('--drop')
        extra.append(args[i + 1])
        del args[i:i + 2]
    token = make_token(extra)
    src, dst = args[0], args[1]
    text = open(src).read().split('\n')
    open(dst, 'w').write('\n'.join(strip(text, token)))



if __name__ == '__main__':
    main()
