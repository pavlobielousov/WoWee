# ADR-001: Renderer architecture for the PS Vita (VITA-11)

Status: **accepted** (user, 2026-10-03). Decided on measurements of the tree and the device, not on a prototype: nothing here has drawn a triangle yet (that is VITA-13).

## Decision

1. **Option 1, a parallel Vita renderer behind the existing facade**, built on **vitaGL**. The Vulkan renderer in `src/rendering/` is never edited for the Vita. Nothing in `rendering/` is removed or reordered.
2. **The seam is the header, not the class.** The 31 files outside `rendering/` that talk to the renderer (28 sources: 12 in `src/core`, 15 in `src/ui`, 1 elsewhere; plus headers) keep their `#include "rendering/renderer.hpp"` and every call they make. For the Vita build only, a directory of **shadow headers** is placed first on the include path (`cmake/vita/shadow/rendering/renderer.hpp`, `.../character_renderer.hpp`, `.../wmo_renderer.hpp`, `.../m2_renderer.hpp`, ...), so the same include resolves to a small Vita declaration of `Renderer` and of each subsystem class. The implementations are new files in `src/rendering/gl/`. Upstream files change only where Vulkan itself leaks (VITA-12), and the compiler tells us when upstream adds a call the Vita header lacks (a loud, early failure at upstream sync, not a silent one).
3. **Reuse what contains no Vulkan, instead of rewriting it.** About half of the rendering headers (71 of 131) and 14K of the 65K lines of `src/rendering` are Vulkan-free, and they are the parts gameplay depends on: `CameraController` (3.8K lines), `AnimationController` (1.2K), `LightingManager`, `TerrainManager` (2.6K, 7 Vulkan mentions), the M2 classifier, the spell visuals, the world-map data. The Vita build compiles these as they are. Only the GPU half is new.
4. **Split the CPU collision half of the WMO and M2 renderers into source files of their own, moved unchanged** (`wmo_renderer_collision.cpp`, then `m2_renderer_collision.cpp`), instead of new classes that the renderers own and forward to. `CameraController` asks `WMORenderer` and `M2Renderer` for floor height, wall sweeps, "am I inside", and raycasts, and those two classes also own the GPU buffers. The query functions themselves never touch the GPU (checked for the WMO renderer: 22 functions, 1,357 lines, the only things shared with the GPU half were `QueryTimer`, which lived in a Vulkan header, and two file-local helpers). So the Vita build compiles the same `*_collision.cpp` against its shadow declaration of the class (same CPU members, GL fields where the Vulkan ones were). This is decision 2's seam applied one level down. A pure move is a smaller shared diff than the roughly 200 edit sites a forwarding class needs, it cannot change behaviour (`tools/vita/check_move.py` proves it line by line), and the tree already splits large renderers this way (`m2_renderer_*.cpp`). *(Revised during VITA-51, 2026-10-03; the first version of this ADR proposed Vulkan-free `WmoCollisionWorld` and `M2CollisionWorld` classes.)* Upstream-candidate.
5. **Shaders: hand-written GLSL ES 1.00, compiled at run time by vitaGL's translator, with the shader cache on.** Needs `libshacccg.suprx` on the console (present on the user's Vita, `ur0:/data/libshacccg.suprx`; every user needs it, as for most vitaGL homebrew; documented, not shipped). Precompiled `.gxp` is a later option, not v1.
6. **Resolution: scene at 640x368 by default, UI at native 960x544.** Configurable (640x368, 720x408, 960x544). Proven by the OpenMW port; to be re-measured in VITA-13 and VITA-22.
7. **Single GL thread.** All drawing and GL calls stay on the main thread; workers only decode (VITA-8 thread layout). Texture upload is a main-thread budgeted queue.

## Context: what was measured

**The facade is a subsystem broker, not a draw API.** Outside `rendering/`, those 31 files make about 700 calls through `Renderer` and 72 distinct `Renderer` methods are used. Most are accessors that hand out a subsystem, which is then called directly (counts are calls):

| Accessor | Calls | Files | Of which Vulkan-free class? |
|---|---|---|---|
| `getCameraController()` | 136 | 9 | yes (`camera_controller`, 0 Vulkan mentions) |
| `getCharacterRenderer()` | 88 | 13 | no (78 mentions): instances, animation state, attachments |
| `getAnimationController()` | 64 | 11 | yes |
| `getWMORenderer()` | 43 | 10 | no (52): floor/wall collision, floor cache |
| `getM2Renderer()` | 40 | 9 | no (110): collision, instances |
| `getTerrainManager()` | 37 | 6 | almost (7 mentions): height queries, streaming |
| `getCharacterPosition()` / `getCharacterInstanceId()` | 31 / 21 | 10 / 8 | facade state |
| `getCamera()` | 25 | 6 | yes |
| `getMinimap()` | 17 | 5 | no (30) |
| `getVkContext()` | 14 | 5 | **must disappear** (VITA-12) |
| `getPostProcessPipeline()` | 10 | 3 | no; every call is a quality setter, no-op on the Vita |

The rest are one- or two-call methods: lifecycle (`initialize`, `beginFrame`, `renderWorld`, `renderHUD`, `endFrame`, `update`, `shutdown`), settings setters for features the Vita does not have (MSAA, shadows, FSR/FSR2, grass, water refraction, sun shafts, volumetric fog, RT lighting, view distance), the character preview (`registerPreview`), recording and screenshots, the selection circle, zone name/id. Setters become empty inline functions in the shadow header. The full list is produced by the script in "How the numbers were made".

**Chained calls that matter** (they define what the Vita classes must really implement): `getCameraController()->` `clearMovementInputs` (9), `getFollowTargetMutable` (8), `suppressMovementFor`, `setExternalFollow`, `isMoving`, `isGrounded`; `getCharacterRenderer()->removeInstance` (5), `moveInstanceTo`, `playAnimation`, `getInstancePosition`, `getInstanceBounds`; `getTerrainManager()->getHeightAt` (4), `processReadyTiles`, `setStreamingEnabled`; `getWMORenderer()->getFloorHeight`, `setWMOOnlyMap`, `loadFloorCache`, `precomputeFloorCache`.

**Vulkan leaks outside `rendering/` into 43 files** (header and source): 17 in `include/ui`, 19 in `src/ui`, 1 in `include/core`, 7 in `src/core`, none in `game/` or `pipeline/` any more. By type: `VkDescriptorSet` 71 uses in 22 files (these are ImGui texture ids from `ImGui_ImplVulkan_AddTexture`), `VkTexture` 27 in 6 files, `VkContext` 16 in 9 files, `VkCommandBuffer` 5, `VkSampleCountFlagBits` 5 (settings), `ImGui_ImplVulkan_*` 9, the rest single uses in the texture loader and window code. So VITA-12 is mostly one mechanical change: an opaque `UiTexture` handle (an `ImTextureID`) in place of `VkDescriptorSet`, one `UiTextureLoader` seam in place of `VkTexture`/`VkContext` for uploads, and the window/ImGui init behind a small interface. The `rendering/` side keeps producing the same handle on desktop.

**Shaders.** 90 GLSL sources: 13 compute (culling, Hi-Z, FSR2, RT, sun shafts, volumetric fog: all cut), 77 vertex/fragment sources written for `#version 450` with descriptor sets (60 files), push constants (47), a 64-light per-frame block, a 3D fog volume, shadow samplers. None ports mechanically to GLSL ES 1.00 (no UBOs, no push constants, no `ivec`, no 3D textures). v1 needs about 14 new shaders: terrain, M2 static and skinned, WMO, character, simple water, sky and celestial, minimap, selection circle, a particle shader, UI (ImGui's own GLES2 shader), overlay/blit.

**Device facts** (VITA-6, VITA-8, VITA-10): 4 Cortex-A9 cores at 444 MHz, apps use 3; about 300-365 MB of the 512 MB for the app plus 128 MB CDRAM; the game core without any renderer or assets costs 4.4 % of one core and about 0.4 MB of heap in a quiet zone (VITA-50 adds a capital city). Library facts: SDK image has vitaGL, vitashark (the run-time shader compiler), Piglet (Sony's GLES2 library, `libScePiglet_stub`) and the SDK's ImGui is 1.61 against the vendored 1.92.6, so the vendored ImGui with its `imgui_impl_opengl3` (GLES2 path) is used, as VITA-12/13 can verify.

## Options considered

| | 1. Parallel renderer, shadow headers (chosen) | 2. RHI under the current renderer | 3. Native GXM |
|---|---|---|---|
| Edits to upstream `rendering/` | none | the whole 65K lines | none |
| Merge risk | low: new files plus a header search path; breaks loudly | very high; upstream would have to accept a rewrite | low |
| Effort to first pixels | smallest (VITA-13) | largest | large (own shader tooling) |
| Vulkan features on the Vita | simply absent | would need emulation or feature gates | absent |
| Risk | the shadow headers drift from upstream's API | the effort itself | runtime and tooling cost |

Option 2 is rejected on merge risk alone; it is also the only option that makes the desktop renderer worse for the Vita's sake. Option 3 stays open for hot paths later (vitaGL sits on GXM; mixing is possible). Piglet (Sony's GLES2) is the fallback if vitaGL's translator or draw overhead proves a problem in VITA-13; it is not measured.

Why not interfaces (the VITA-45 style) for the 31 files instead of shadow headers? Those seams worked for `game/` because only 5 small interfaces were needed. Here the surface is 72 methods plus about 100 chained ones across 9 subsystem classes: an interface layer would be an edit in every call site in shared files, the opposite of the fork rules. The shadow header is one new file per subsystem and no edit at the call sites.

## Feature cut list for v1

Cut: shadows, Hi-Z and all compute culling, FSR/FSR2/FSR3, post-process chain (a final blit only), volumetric fog, sun shafts, lens flare, grass, ray-traced lighting, MSAA (2x maybe later), water refraction/reflection, weather (rain/snow later, cheap), swim effects, footprints, the character preview render (later), screen recording. Kept: terrain with up to 3 texture layers (alpha-blended), M2 and WMO with diffuse textures and vertex lighting, skinned characters and creatures, a flat water plane with alpha, sky dome and sun/moon, minimap, quest markers, selection circle, UI.

## Constraints that shape the Vita renderer (not yet designed)

- **Memory:** CDRAM 128 MB is for render targets and textures; the budget and a texture residency policy are VITA-15/23. DXT textures upload as they are (the GPU samples them; to be confirmed with `glCompressedTexImage2D` in VITA-13/15), mips dropped by the Vita data profile (VITA-24).
- **Uniforms:** GLSL ES has no UBOs; vitaGL's `HAVE_GLSL_UBOS` is experimental, so per-frame data is plain uniforms. Bone matrices for GPU skinning must fit the vertex uniform limit (not yet measured): the choice between GPU skinning with few bones and CPU skinning of near characters belongs to VITA-20.
- **Draw calls:** draw-call count is the usual limit on a 444 MHz CPU (to be measured in VITA-13); batching by material and a distance cull come before beauty (VITA-18/19).
- **Threads:** GL on the main thread only. Decoding and mip generation on the VITA-8 workers. vitaGL's own garbage-collector thread must be placed with `core::enterThread`.

## Consequences for the board

- **VITA-12** shrinks and sharpens: opaque `UiTexture`, `UiTextureLoader`, window/ImGui init behind a seam; no `VkContext` or `Vk*` type in any `ui/` or `core/` file. Scope is the 43 files above, not 33.
- **VITA-51 (before VITA-13): collision split**: move the CPU queries of `WMORenderer` and `M2Renderer` (floor height, wall sweep, inside tests, raycast, floor caches) into `*_collision.cpp` files unchanged (desktop check must stay 220/220; `tools/vita/collision_check.sh` compiles them with the Vita compiler and no Vulkan headers). Upstream-candidate, size L.
- **VITA-52: `rendering/gl` shadow headers and Vita `Renderer` skeleton**: the include-path mechanism in `Vita.cmake`, the shadow headers with stubs for every call in the list, so the `ui/` and `core/` sources compile for the Vita (needs VITA-12). This is the "Application builds on the Vita" milestone and part of VITA-13.
- **VITA-13** (vitaGL bring-up) = window, context, ImGui (GLES2), first triangle, the DXT check, the GL thread rules, and a measured number for draw-call cost.
- **VITA-14** = the 14 shaders in GLSL ES 1.00 plus the run-time compile and shader cache; drop the "offline CG to GXP" task from v1.
- VITA-18..21 follow the cut list above; VITA-16 (HUD) can reuse `PerformanceHUD`, which is Vulkan-free.

## Open questions (to settle with a measurement, not an opinion)

1. Does vitaGL's translator accept the terrain and character shaders at an acceptable compile time and size (shader cache on)? (VITA-14)
2. Real draw-call cost per frame at 444 MHz through vitaGL. (VITA-13)
3. Vertex uniform limit and skinning cost. (VITA-20)
4. 640x368 scene plus native UI: is the second pass affordable in CDRAM and fill-rate? (VITA-13, VITA-22)
5. Piglet versus vitaGL, only if question 1 or 2 goes badly.

## How the numbers were made

Counts come from three throw-away scripts over the tree at `origin/vita` on 2026-10-03: calls on `Renderer` pointers (`getRenderer()->x`, `renderer->x`, `renderer_->x` and the chained `getX()->y`) outside `src/rendering` and `include/rendering`; Vulkan mentions per rendering header and source (regex on `vulkan`, `VkContext`, `vk_`, `Vk*`, `Vma*`); Vulkan types and includes in `ui/`, `core/`, `pipeline/`, `game/`. "Vulkan-free" means no such mention in the file, not that the whole transitive include set was checked; VITA-13 does that with the real compiler. The scripts are not kept in the repo; the method is above.

## Update 2026-10-03: what the device measured (VITA-13 GlProbe, `docs/vita/DEV_SETUP.md` section 19)
- Open question 1 (translator on the terrain and character shaders): a two-layer terrain shader and an M2 alpha-test shader translate and draw correctly, **but each program costs about 330 to 365 ms to compile (165 to 182 ms with a shader cache)**, so **decision 5 is refined: ship precompiled shader binaries as the normal path** (measured the same day: dumped with `vglGetShaderBinary` once, loaded with `glShaderBinary` in 0.02 ms plus 0.03 ms to link, against 353 ms to compile, binaries of about 500 bytes each); run-time GLSL compilation stays as the development path and as a fallback behind a loading screen, with a hard cap on permutations. **Correction to an earlier line of this note: fixed-function is not compiler-free** (vitaGL compiles a generated shader per new GL state combination at run time: 150 to 650 ms on first use, cached on disk afterwards), so the UI should either use its own shipped shaders or ship the cache. Whether the client can then run **without `libshacccg.suprx`** is read from vitaGL's source (init carries on without it) but untested (VITA-14).
- Open question 2 (draw-call cost): about 2.2 microseconds per draw, 3 to 4 with state changes, 0.3 per quad batched (fixed-function path, 444 MHz). Budget about 3 ms per 1000 draws.
- Open question 4 (640x368 scene plus native UI): the FBO and the scaled blit work and are correct; their cost in a real scene is not measured.
- DXT1/3/5 and DXT mips work as the decision assumed; one vitaGL state quirk is recorded in the DEV_SETUP section.
- First-run compile cost, measured with heavier stand-in shaders: 357 ms (terrain) to **1069 ms (skinned character)**, 573 to 749 ms for WMO and water; a plausible v1 set is about 12 s, 40 mixed programs about 30 s, and each skinned permutation is 1 s or more. This is why decision 5 ships precompiled binaries and caps permutations; a first-run compile on the user's console stays the fallback (a progress screen for 10 to 30 s, once).

