# VITA-12: what to run and look at on a machine with a GPU

VITA-12 changes how the interface and the application shell talk to the renderer (opaque texture handles, no Vulkan types outside
`rendering/`). The compiler proves the Vita side and the desktop tests prove the logic that has tests; **only a person looking at the
real client can prove the screens still draw, and only a GPU run can show the cost.** This page is for that person.
Nothing here needs the Vita.

## 0. Once: take the baseline

The baseline is the client **before** the VITA-12 changes: the fork's `vita` branch at commit `7916589a` (the start of VITA-12; if
later commits of other items are on `vita` by the time you run this, use `git log` to find the last commit before the first
`VITA-12:` commit and say which one you used).

```sh
git worktree add ../wowee-baseline 7916589a          # a second checkout, so the two builds do not mix
cd ../wowee-baseline && git submodule update --init --depth 1 extern/imgui extern/vk-bootstrap
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel   # as in BUILD_INSTRUCTIONS.md
```

Run it **in the scenes below with `WOWEE_FRAME_PROFILE=1`**, one run per scene, **at least 60 seconds each, not touching the mouse**
(the client logs a "frame budget over 10s" block every 10 seconds; the first is skipped as warm-up). Use the same window size, the same
machine and nothing else heavy running. Keep the log of each run (macOS: `~/Library/Logs/Wowee/`; elsewhere `WOWEE_LOG_STDOUT=1`
echoes it to the terminal) under a name like `baseline-login.log`.

| Scene | What to do |
|---|---|
| `login` | start the client, leave the login screen alone |
| `charselect` | log in to the LAN test server (`docs/server-setup.md`), stay on character select |
| `world` | enter the world and stand still outdoors, minimap and action bars visible |
| `world-ui` | the same spot with the bags, spellbook, talents and the world map (M) open |

Do the same with the build that has the change, then compare:

```sh
tools/vita/compare_frame_profile.py baseline-world.log new-world.log          # repeat for each scene
```

It prints both side by side and flags any stage more than 8 % (and more than 0.05 ms) slower; exit status 1 if anything is flagged.
**Run both builds twice before believing a flag** (machine noise is a few percent); an honest "no difference" is the expected answer.

## 1. Every PR: the screens to look at

Open the build of the PR and look at each screen below. For each, the question is the same: **does every icon, portrait and
background that the baseline shows still appear, in the right place, with the right picture?** A wrong or missing texture shows as
a blank square, a black square, another icon, or a crash.

- **Login screen:** the background image (the big one behind the form), the logo; resize the window and come back.
- **Realm list, character select, character create:** model previews and portraits where they exist.
- **Loading screen:** the loading background.
- **In the world, the HUD:** action bar icons (spells, items, macros), unit frames with portraits (yourself, a target), buff and
  debuff icons, the minimap and its markers, raid target icons on a target, the cast bar.
- **Bags / inventory:** item icons in every slot; hover an item for the tooltip (its icon too).
- **Spellbook and talents:** every spell icon; talent tab backgrounds.
- **Chat:** an item link in chat (its icon in the tooltip), raid target tags in text.
- **World map (M) and the settings panel (graphics tab: multisampling, shadows, view distance)**; change a setting and back.
- **Window behaviour:** minimise and restore, resize, toggle fullscreen; the screen must come back drawn.

Each PR in this series lists which of these it can affect, so you can skip the rest for that PR.

## 2. What to send back

For each PR: "screens OK" or a short list of what looked wrong (a screenshot helps), and the output of `compare_frame_profile.py` for the
scenes you ran.

## 3. Which change affects which screen (the VITA-12 PR)

So you can look first where it matters. Everything below is **meant to look and behave exactly as before**.

| Change | Where it shows |
|---|---|
| Icons and portraits are `UiTexture` ids instead of descriptor sets (same numbers, same ImGui calls) | every icon: action bars, bags, spellbook, talents, tooltips, chat links, raid target marks, cursor, buffs, cast bar, unit portraits, character select and create previews |
| The login background is uploaded through the shared upload instead of 150 lines of its own (**a real code change**) | the login screen's background picture; leave the app on it for a minute, resize, minimise and restore; quit cleanly |
| The widget renderer talks to a small upload service instead of the Vulkan context | everything drawn from the game's own interface files (FrameXML-style panels), their textures and their loading batches |
| ImGui's renderer backend is started, fed and stopped through an interface (same calls, same order) | **any UI at all**; start the client, quit it, and log out and in again; a crash on exit would be here |
| `Window`/`Renderer` methods replace direct context calls in the shell | minimise and restore the window (surface release/restore on phones is the same code), resize (swapchain rebuild), entering the world (the wait before the load screen ends), logging out |
| Antialiasing and texture filtering settings go through `Renderer`/`Window` | the settings panel: change antialiasing (note the "this GPU offers" clamp message on a card with a lower maximum), change texture filtering |
| Upload batching in the entity spawner goes through `Renderer` | NPCs and players appearing in a busy area (no stalls, same skins, cloaks, hair) |
| `GpuTexture` is an alias of `VkTexture`; one dead accessor removed | nothing visible; compile-time only |

The PR description lists, per file, what changed and why.

