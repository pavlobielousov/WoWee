#!/bin/sh
# The Vita's shadow headers (VITA-52, ADR-001) and what is still in their way. Three checks, then a worklist:
#   1. tools/vita/gen_shadow.py --check: the committed shadow headers equal what upstream's headers produce now.
#   2. the shadow directory really wins: <rendering/renderer.hpp> resolves into it and pulls in no Vulkan header,
#      and <rendering/vk_context.hpp> stops with the VITA-12 error instead of reaching vulkan.h.
#   3. a Vita-compiler sweep of src/core and src/ui against them, no Vulkan headers at all. A source may fail
#      only for a reason VITA-12 removes (a ui/ or core/ header that includes vulkan.h, the vk_context.hpp error,
#      a public signature with a Vulkan type); any other error fails this script.
#   tools/vita/shadow_check.sh            # about 3 minutes
# Writes the worklist for VITA-12 to build-vita/sweep-shadow/vita12_worklist.txt.
set -eu

. "$(dirname "$0")/lib.sh"
runtime=$(vita_runtime)
cd "$VITA_ROOT"

python3 tools/vita/gen_shadow.py --check

OUT=build-vita/shadow_check
rm -rf "$OUT"; mkdir -p "$OUT"
cat > "$OUT/wins.cpp" <<'EOS'
#include "rendering/renderer.hpp"
int main() { return 0; }
EOS
cat > "$OUT/poison.cpp" <<'EOS'
#include "rendering/vk_context.hpp"
EOS
limits=""
[ "$runtime" = container ] && limits="--memory ${MEMORY:-6G} --cpus ${CPUS:-2}"
# shellcheck disable=SC2086
"$runtime" run --rm $limits -e OUT="$OUT" -v "$VITA_ROOT:/workspace" "$VITASDK_IMAGE" sh -c '
cd /workspace
F="-std=gnu++20 -DGLM_ENABLE_EXPERIMENTAL -DGLM_FORCE_DEPTH_ZERO_TO_ONE -Icmake/vita/shadow -Iinclude -Isrc -isystem extern"
# 2a: the shadow renderer.hpp is the one included, and nothing named *vulkan* / vk_* is
inc=$(arm-vita-eabi-g++ $F -fsyntax-only -H "$OUT/wins.cpp" 2>&1 | grep "^\.* .*\.h" || true)
echo "$inc" | grep -q "vita/shadow/rendering/renderer.hpp" || { echo "FAIL: renderer.hpp did not resolve into the shadow directory"; exit 1; }
if echo "$inc" | grep -qiE "vulkan|/vk_"; then echo "FAIL: a Vulkan header is reachable from the shadow renderer.hpp:"; echo "$inc" | grep -iE "vulkan|/vk_" | head -5; exit 1; fi
echo "OK    renderer.hpp resolves into the shadow directory, no Vulkan header reachable"
# 2b: the poison header stops with the explanatory error
if arm-vita-eabi-g++ $F -fsyntax-only "$OUT/poison.cpp" 2>&1 | grep -q "leaked outside rendering/ (VITA-12)"; then
    echo "OK    vk_context.hpp is poisoned (names VITA-12)"
else echo "FAIL: vk_context.hpp is not poisoned"; exit 1; fi' </dev/null 2>&1 | grep -v '^\[[0-9]/'

SHADOW=1 OUT=build-vita/sweep-shadow tools/vita/depcheck/sweep.sh core ui </dev/null 2>&1 | sed -n '/per directory/,/^$/p'

python3 - <<'PYEOF'
import glob, os, re, collections, sys
out = 'build-vita/sweep-shadow'
# Desktop-only implementations that are Vulkan on purpose (the same list, with reasons, as ALLOW in
# tools/vita/vulkan_leak_check.sh): the Vita has its own core::Window and no detached map window.
DESKTOP_ONLY = {'src/core/window.cpp', 'src/ui/map_window.cpp'}
direct = collections.defaultdict(list)   # header that includes vulkan.h -> sources behind it
poison, sigs, bad, expected = [], [], [], []
for log in sorted(glob.glob(f'{out}/logs/*.log')):
    t = open(log).read()
    src = os.path.basename(log)[:-4].replace('_', '/', 1)
    if 'error' not in t and 'fatal error' not in t:
        continue
    m = re.search(r'(\S+):\d+:\d+: fatal error: vulkan/vulkan.h', t)
    if m and (m.group(1).startswith(('include/ui/', 'include/core/', 'src/ui/', 'src/core/'))):
        direct[m.group(1)].append(src)
    elif 'leaked outside rendering/ (VITA-12)' in t:
        if os.path.basename(log)[:-4] in {d.replace('/', '_') for d in DESKTOP_ONLY}:
            expected.append(src)
        else:
            poison.append(src)
    else:
        e = re.search(r'error: (.*)', t)
        msg = e.group(1) if e else '?'
        # a public signature that carries a Vulkan type is stripped from the shadow header: VITA-12 too
        if re.search(r"has no member named '(get|set)\w*Texture\w*'|'VkTexture' is not a member|'VkDescriptorSet' is not a member|'VkContext'", msg):
            sigs.append((src, msg))
        else:
            bad.append((src, msg))
with open(f'{out}/vita12_worklist.txt', 'w') as f:
    f.write('Headers outside rendering/ that include vulkan.h (VITA-12 removes each):\n')
    for h, srcs in sorted(direct.items(), key=lambda kv: -len(kv[1])):
        f.write(f'  {h}: blocks {len(srcs)} source file(s)\n')
    f.write('\nSources that include rendering/vk_context.hpp:\n' + ''.join(f'  {s}\n' for s in poison))
    f.write('\nSources using a public method that carries a Vulkan type in its signature:\n' + ''.join(f'  {s}: {m}\n' for s, m in sigs))
print(f'VITA-12 worklist: {len(direct)} headers, {len(poison)} vk_context users, {len(sigs)} signature users; {len(expected)} desktop-only file(s) expected to fail -> {out}/vita12_worklist.txt')
if bad:
    print('FAIL: errors that are not VITA-12 work:')
    for s, m in bad:
        print(f'  {s}: {m[:120]}')
    sys.exit(1)
print('OK    every remaining failure is VITA-12 work' if (direct or poison or sigs) else 'OK    VITA-12 is done: the only files that do not parse are the desktop-only implementations')
PYEOF
