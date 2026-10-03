#!/bin/sh
# VITA-12's progress meter: which files outside rendering/ still mention Vulkan. The item's own acceptance grep,
#   grep -rlE "\bVk[A-Z]|vkCmd|vulkan" src/{ui,core,game,pipeline,addons} include/{ui,core,game,pipeline,addons}
# plus the raw VK_/vk-prefixed names it misses, with a count per file. Exit 0 only when nothing is left.
#   tools/vita/vulkan_leak_check.sh            # summary and the file list
#   tools/vita/vulkan_leak_check.sh --lines    # every offending line
# The Vita-side meter is tools/vita/shadow_check.sh (what the Vita compiler still stops at).
set -u
cd "$(dirname "$0")/../.."
DIRS="src/ui src/core src/game src/pipeline src/addons include/ui include/core include/game include/pipeline include/addons"
PAT='\bVk[A-Z]|\bVK_[A-Z_]+|\bvk[A-Z][A-Za-z]+|vkCmd|vulkan|ImGui_ImplVulkan|\bVma[A-Z]|SDL_Vulkan'
# Files whose only mention is a comment about the renderer, with the reason. A real use never goes here.
#   src/pipeline/m2_loader.cpp: a comment on why an index range is checked ("vkCmdDrawIndexed does not check").
ALLOW="src/pipeline/m2_loader.cpp"
files=$(grep -rlE "$PAT" $DIRS 2>/dev/null | sort)
for a in $ALLOW; do files=$(printf '%s\n' "$files" | grep -vx "$a"); done
if [ "${1:-}" = "--lines" ]; then
    grep -rnE "$PAT" $DIRS 2>/dev/null | sort | grep -v "^src/pipeline/m2_loader.cpp:"
else
    for f in $files; do printf '%4d %s\n' "$(grep -cE "$PAT" "$f")" "$f"; done | sort -rn
fi
n=$(printf '%s\n' "$files" | grep -c . || true)
total=$(for f in $files; do grep -cE "$PAT" "$f"; done | awk '{s+=$1} END {print s+0}')
echo "---"
echo "$n file(s), $total line(s) outside rendering/ still mention Vulkan"
[ "$n" = 0 ]
