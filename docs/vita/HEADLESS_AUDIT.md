# Headless core audit (VITA-9, step 1)

What `src/game/` and `src/addons/` reach into outside their own layer, classified by the seam each
needs. Read-only audit of `vita` at c3857a5b (2026-10-02), done by grep over includes and call
sites; nothing was compiled for this table. Counts are lines, not unique calls.

Seam classes: **(a)** notification or query the renderer/UI/audio should answer through an
interface the game owns; **(b)** shared data or pure helper that only lives in the wrong directory;
**(c)** a reach into the `Application` singleton to pass in explicitly.

## What already helps

`game::GameServices` (`include/game/game_services.hpp`) already carries `renderer`,
`audioCoordinator`, `assetManager` and `expansionRegistry` into `GameHandler` and its handlers
(`owner_.services()`). It only forward-declares `Renderer` and `AudioCoordinator`. So the call sites
are already funnelled through one struct; the work is replacing the concrete pointer types with
interfaces, not hunting hidden singletons. Only two `Application::getInstance()` uses remain in
`game/` (below).

## `game/` -> `rendering/`

| Dependency | Where | Class | Seam |
|---|---|---|---|
| `Renderer::getSpellVisualSystem()` -> `playSpellVisual`, cast/impact visuals | `spell_handler.cpp` (about 10 sites), `game_handler_packets.cpp:2421-2432` | a | `ISpellVisuals` observer: `play(visualId, pos, impact)` and friends |
| `Renderer::getCharacterPosition()`, `getCharacterInstanceId()`, `getCharacterRenderer()->getAttachmentTransform` | `spell_handler.cpp:345-398`, `game_handler_packets.cpp:2425` | a | player-pose query interface (position, hand attachment) |
| `getCameraController()->setIntoxication`, `getPostProcessPipeline()->setIntoxication` | `game_handler_callbacks.cpp:617`, `game_handler_packets.cpp:508` | a | `IScreenEffects::setIntoxication` |
| `setCharacterYaw`, camera facing | `game_handler_callbacks.cpp:1656-1675` | a | player-pose interface (set facing) |
| `WMORenderer` / `M2Renderer` instance calls (`setInstanceTransform`, `setInstanceHidden`, `setInstanceDoodadAnimation`, `getInstanceFloorHeight`, `getInstanceWorldBounds`, `instanceHasCollisionGeometry`, `setInstanceIsTransport`) | `transport_manager.cpp` (about 21 sites; holds `wmoRenderer_`, `m2Renderer_`) | a | `ITransportRenderTarget`; the largest single seam |
| `rendering::movement::kMaxStepUp` | `transport_manager.cpp:546-549` (`rendering/movement_limits.hpp`) | b | move constants to a neutral header (check the file is Vulkan-free first) |
| `AnimationController::getEmoteAnimByEmotesId`, `isStateEmoteById`, `getEmoteTextByDbcId` (static) | `chat_handler.cpp:216-221,1014`, `entity_controller.cpp:1261` | b | static helpers; split the pure part out of `rendering/animation_controller.hpp`, not verified that it is pure |
| `rendering::anim::validateAgainstDBC`, `animation/animation_ids.hpp` | `game_handler_callbacks.cpp:3592`, `game_handler.cpp`, `game_handler_packets.cpp` | b | enum/validator to a neutral header |
| `#include "rendering/renderer.hpp"` only | `combat_handler`, `social_handler`, `quest_handler`, `inventory_handler`, `chat_handler`, `game_handler.cpp` | none | include likely unused after the above; remove, no seam |

## `game/` -> `audio/`

About 34 includes, call sites by manager: `getUiSoundManager` 21, `getSpellSoundManager` 5,
`getCombatSoundManager` 2, `getActivitySoundManager` 1, `getPlayerVoiceManager` 1,
`getNpcVoiceManager` 1, reached through `services().audioCoordinator`. `include/game/spell_handler.hpp`
includes `audio/spell_sound_manager.hpp` (a header leak: a Vita consumer of `spell_handler.hpp`
needs miniaudio-free audio headers).

Class **a**. Two options, to decide in the audio seam item: (1) a null `AudioCoordinator` that the
Vita build constructs (audio classes keep their API, bodies empty; smallest diff in `game/`); (2) an
`IGameAudio` interface. Option 1 touches no `game/` file and matches VITA-29 (real backend later).
Recommended unless the audio headers themselves pull in miniaudio (`audio_engine.hpp` is the
suspect; not checked).

## `game/` -> `ui/`

`ui/framexml_takeover.hpp` includes only the standard library. `game/` uses four free functions:
`ui::frameXmlOwns(UiElement)` (5 sites, chat and gossip suppression), `ui::frameXmlRequestCheck()`
(`combat_handler.cpp:1394`), `ui::frameXmlNoteWorldEntry()` (`game_handler_callbacks.cpp:938`) and
the enum `ui::UiElement`. Class **a**/**b**: the header is already Vulkan-free; headless needs
the four functions to exist. Cheapest seam: provide them from a small `wowee_core` source (always
`false`/no-op) and leave the real ones in `ui/`. Not verified: where they are defined today.

## `game/` -> `core/application`

| Where | Class | Seam |
|---|---|---|
| `include/game/game_utils.hpp:13` `isActiveExpansion()` (header, so every includer sees `core/application.hpp`) | c | take `ExpansionRegistry*`, or read it from `GameServices`; the function is used from many files, so keep the signature and give it a registry accessor set at startup (like `update_field_table`'s global) |
| `warden_handler.cpp:453` `Application::getInstance().getExpansionRegistry()` | c | use `services_.expansionRegistry` (already there) |
| `#include "core/application.hpp"` in `spell_handler`, `social_handler`, `quest_handler`, `movement_handler`, `inventory_handler`, `game_handler.cpp`, `game_handler_packets.cpp`, `game_handler_callbacks.cpp`, `combat_handler` | none | probably leftovers: `grep` finds no other `Application` use in `game/`; remove and compile |

## `addons/`

| Dependency | Where | Class |
|---|---|---|
| `ui/widget_tree.hpp` (in headers `lua_engine.hpp`, `lua_api_helpers.hpp`) and `ui/*` helpers (`text_markup`, `ui_colors`, `plural_escape`, `link_hit`, `interface_fonts`, `settings_schema`, `display_modes`, `keybinding_manager`, `gamepad_controls`, `xml_parser`, `framexml_emitter`) | `lua_engine.cpp`, `lua_system_api.cpp`, `lua_action_api.cpp`, `addon_manager.cpp` | a/b: the Lua API is the FrameXML interface itself, so addons are not part of the first headless target; add them with VITA-12/28 |
| `core/window.hpp` | `lua_engine.cpp`, `lua_system_api.cpp`, `addon_manager.cpp` | c |
| 17 `audio/*` includes | `lua_system_api.cpp` | a, same null audio as above |
| `rendering/camera_controller`, `world_map/coordinate_projection`, `animation_controller` | `lua_system_api.cpp`, `lua_social_api.cpp` | a/b |

**Recommendation:** `wowee_core` v1 excludes `src/addons/` (the task list names "addons-logic",
but the coupling above is mostly the UI tree, not logic); it can follow once VITA-12 lands.
`addon_manager.cpp` already has `__vita__` arms for paths (VITA-35), which stay valid.

## `core/`

`core/` is not one layer: 16 of its sources touch SDL, Vulkan, `rendering/` or `ui/`
(`application`, `window`, `input`, `gamepad`, the `*_callback_handler`, `entity_spawner*`,
`appearance_composer`, `world_loader`). `wowee_core` takes only the clean ones: `logger`,
`config_paths`, `memory_monitor` (needs a Vita arm for `<sys/sysinfo.h>`), `app_clock`,
`frame_pacer`, `https_get`/`update_check` (not checked), plus the already-clean `math/`, `network/`,
`auth/`, `pipeline/`. The callback handlers are the desktop side of the observer pattern; the
headless driver implements the same callbacks itself.

## Warden

Five files (`warden_handler`, `warden_crypto`, `warden_module`, `warden_emulator`,
`warden_memory`). `warden_module.cpp` needs OpenSSL 3 and x86 emulation: compile it out under
`WOWEE_PLATFORM_VITA`. `warden_handler.cpp` is the caller; it already has a "no module" path.
Not checked: whether `warden_emulator`/`warden_memory` compile on 32-bit newlib (the VITA-3 sweep
only parses; `check32.sh` ran them on glibc).

## CMake facts for the carve

- One list, `WOWEE_SOURCES` (`CMakeLists.txt:613`), with `# Core`, `# Math`, `# Network`, `# Auth`,
  `# Game`, `# Audio`, `# Pipeline` sections; used by `add_executable(wowee ...)` (:1265), the
  Android `add_library(wowee SHARED ...)` (:1263), `WOWEE_HEADLESS_SOURCES` (:1713, a different
  thing, the client minus `main`) and `WOWEE_RT_PROBE_SOURCES` (:1763). Splitting must keep
  those four working.
- `-DWOWEE_HEADLESS` is not defined anywhere; the name is free but `WOWEE_HEADLESS_SOURCES` is not.
- The Vita build is separate: `cmake/vita/Vita.cmake` returns before the root file's targets, so
  `wowee_core` has to be added there from the same list, not copied.

## Proposed split of VITA-9 (to be created as draft items)

1. **Renderer seams** (class a, spell visuals, player pose, screen effects, transport). One PR per
   interface; `GameServices` holds interface pointers; `Renderer` implements them. Desktop unchanged.
2. **Neutral data/helpers** (class b): `movement_limits`, `animation_ids`, emote helpers, the four
   `frameXml*` stubs. Moves only, no logic change, separate PRs.
3. **`Application` removal from `game/`** (class c): `game_utils.hpp`, `warden_handler.cpp`, plus the
   unused includes. Smallest, and the best first PR.
4. **Audio null seam** (decision between null coordinator and interface).
5. **CMake carve + `Vita.cmake` hook + `memory_monitor` Vita arm + Warden compile-out + one shared
   CMake function for the Vita link options.**
6. **`wowee_headless` driver** (`tools/headless/`), desktop CI build, Vita build for VITA-10.

## Decisions and where the work went (2026-10-02)

- VITA-9 is the analysis only. The six steps above are items VITA-43 (Application removal),
  VITA-44 (neutral data), VITA-45 (renderer seams), VITA-46 (audio), VITA-47 (CMake carve) and
  VITA-48 (`wowee_headless`).
- **`wowee_core` v1 excludes `src/addons/`** (user, 2026-10-02): the coupling is the UI tree, not
  Lua logic. Handle it in VITA-49 after VITA-12 (Vulkan-free UI); re-run this audit for `addons/`
  first. Do not pull addons into VITA-47.
- Not decided: option names, and whether upstream is approached (the user has not agreed, see VITA-33).
