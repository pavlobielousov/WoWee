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
# Files that are Vulkan on purpose, each with the reason. A file goes here only when it IS the desktop Vulkan
# platform or a comment, never because it was tedious to convert.
#   src/pipeline/m2_loader.cpp  a comment on why an index range is checked ("vkCmdDrawIndexed does not check").
#   src/core/window.cpp         the desktop window: it creates the Vulkan surface and context. The Vita has its own
#                               implementation of the same core::Window class (include/core/window.hpp).
#   include/core/window.hpp     forward declarations of rendering/'s own classes (VkContext, VkUiTextureService,
#                               VkImGuiBackend) and unique_ptr members of them: no Vulkan header is included.
#   src/ui/map_window.cpp, include/ui/map_window.hpp
#                               the detached map: a second OS window with its own swapchain and its own ImGui Vulkan
#                               backend. Desktop only; not built for the Vita.
ALLOW="src/pipeline/m2_loader.cpp src/core/window.cpp include/core/window.hpp src/ui/map_window.cpp include/ui/map_window.hpp"
# Lines that are only a comment (//, ///, * or /*) do not affect a build; they are counted apart, not as leaks.
CODE='^[^:]*:[0-9]*:[[:space:]]*([^/*[:space:]]|/[^/*])'
hits() { grep -rnE "$PAT" $DIRS 2>/dev/null | grep -E "$CODE" | grep -vE "^[^:]*:[0-9]*:[[:space:]]*(//|/\*|\*)" | grep -vE "^($(echo $ALLOW | tr ' ' '|')):"; }
files=$(hits | cut -d: -f1 | sort -u)
for a in $ALLOW; do files=$(printf '%s\n' "$files" | grep -vx "$a"); done
if [ "${1:-}" = "--lines" ]; then
    hits | sort | grep -v "^src/pipeline/m2_loader.cpp:"
else
    for f in $files; do printf '%4d %s\n' "$(hits | grep -c "^$f:")" "$f"; done | sort -rn
fi
n=$(printf '%s\n' "$files" | grep -c . || true)
total=$(hits | grep -vc "^src/pipeline/m2_loader.cpp:" || true)
comments=$(grep -rnE "$PAT" $DIRS 2>/dev/null | grep -cE "^[^:]*:[0-9]*:[[:space:]]*(//|/\*|\*)" || true)
echo "---"
echo "$n file(s), $total line(s) of code outside rendering/ still mention Vulkan ($comments comment-only mention(s) ignored)"
[ "$n" = 0 ]
