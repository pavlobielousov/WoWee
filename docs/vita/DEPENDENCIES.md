# Vita dependencies and C++20 audit (VITA-3)

What WoWee needs, what VitaSDK provides, and what has to be ported or dropped. Written before any
port code, so the later items (VITA-4 build, VITA-5 32-bit, VITA-9 headless core, VITA-11/13
renderer, VITA-29 audio) start from facts. Nothing here touches an upstream file; see
[FORK_POLICY.md](FORK_POLICY.md).

**Where each fact comes from.** Every finding carries a tag:

| Tag | Meaning |
|---|---|
| `[sdk]` | read from the pinned VitaSDK image, `vitasdk/vitasdk:2026.08-20260925` (headers, libs, compiler) |
| `[grep]` | search of this source tree (`vita` branch at `f23b5499`) |
| `[sweep]` | `-fsyntax-only` compile of the real sources with the Vita compiler (nothing linked or run) |
| `[v3k]` | executed by DepCheck on **Vita3K v0.2.1**. An emulator, so behaviour that depends on the OS or hardware is *not* confirmed |
| `[hw?]` | needs a real Vita; collected under VITA-34 |

## 1. Toolchain

| Item | Result | Source |
|---|---|---|
| Compiler | `arm-vita-eabi-g++ (GNU Tools for ARM Embedded Processors) 15.2.0` | `[sdk]` |
| Target defaults | `-march=armv7-a+simd -mfpu=neon -mfloat-abi=hard -mtune=cortex-a9`; `__ARM_NEON=1`, `__ARM_PCS_VFP=1` | `[sdk]` |
| Data model | ILP32: `sizeof(size_t) = sizeof(long) = sizeof(void*) = sizeof(wchar_t) = 4`; `long long` is 8 | `[v3k]` |
| CMake toolchain | `$VITASDK/share/vita.toolchain.cmake`: Release `-O3 -DNDEBUG`, always `-Wl,-q`, **no** `-std` flag. CMake's default `CXX_EXTENSIONS=ON` gives `-std=gnu++20` | `[sdk]` |
| `__cplusplus` | `202002` with `-std=gnu++20` | `[v3k]` |

## 2. C++20 features

WoWee sets `CMAKE_CXX_STANDARD 20` (`CMakeLists.txt:5`). The library headers for every C++20 feature
the tree could use are present in the SDK's libstdc++ 15 (`span ranges format stop_token bit latch barrier
semaphore source_location numbers compare syncstream filesystem expected generator`) `[sdk]`.
What matters is which of them WoWee uses, and whether those *work* on newlib, not just compile:

| Feature | Use in the tree `[grep]` | Result on Vita |
|---|---|---|
| `std::span` | 11 files (pipeline loaders, gamepad, animation) | works `[v3k]` |
| `requires` / concepts | one `if constexpr (requires {...})` in `pipeline/wowee_vertex_sanitize.hpp:49`; no `concept` declarations | works `[v3k]` |
| `<ranges>` / `std::views` | 7 files (`logger.cpp`, `widget_tree.cpp`, Lua inventory API) | works `[v3k]` |
| `<bit>` (`bit_cast`, popcount) | 4 files (`packet.cpp`, `entity_controller.cpp`, ...) | works `[v3k]` |
| designated initialisers | used | works `[v3k]` |
| `std::source_location` | 4 files, all Vulkan (`vk_utils`, `vk_texture`) | compiles; irrelevant (Vulkan is dropped) `[v3k]` |
| `std::filesystem` | **163 files** | compiles everywhere; **semantics broken for `ux0:` paths**, see gap G2 `[v3k]` |
| `std::jthread`, `stop_token` | none (a comment in `application.cpp:1333` says why) | not needed |
| `std::atomic::wait/notify` | none (only condition-variable notifies) | works anyway `[v3k]` |
| `<format>`, `consteval`, `constinit`, `<numbers>`, `<=>`, `<latch>`/`<barrier>`/`<semaphore>`, `<syncstream>`, `char8_t`, `std::chrono` calendar | none | not needed |
| `std::thread`, `mutex`, `condition_variable`, `shared_mutex`, `async`, `thread_local`, exceptions | used throughout | work once **G1** is fixed `[v3k]` |

**Verdict: no C++20 language or library gap blocks the port.** No polyfill header under
`include/platform/vita/compat/` is needed for the language itself. The gaps are in the runtime
(G1 to G3 below), not in the standard.

### Sweep: do the real sources parse?

`tools/vita/depcheck/sweep.sh` compiles every `src/**/*.cpp` (445 files) with `-fsyntax-only -std=gnu++20 -Wall -Wextra`
and WoWee's include paths, with desktop-only inputs stood in: the Khronos Vulkan headers (untracked, in
`build-vita/vulkan-headers`), the vendored ImGui and vk-bootstrap submodules, and a generated `version.hpp`.

| Directory | Files | With Vulkan headers | **Without** Vulkan headers | Notes |
|---|---|---|---|---|
| `auth`, `network`, `math` | 15 | 15 | **15** | The headless-core base (VITA-9): parses with no edit and no Vulkan `[sweep]` |
| `pipeline` | 170 | 170 | **170** | Asset loading is Vulkan-free, confirmed by the run without the headers `[sweep]` |
| `audio` | 13 | 13 | **13** | Vulkan-free |
| `addons` (Lua API) | 13 | 13 | 6 | 7 files reach `vulkan.h` through other headers |
| `core` | 28 | 27 | 13 | 14 reach `vulkan.h`; `memory_monitor.cpp` needs `<sys/sysinfo.h>` (G4) |
| `game` | 46 | 45 | 30 | 15 reach `vulkan.h`; `warden_module.cpp` needs OpenSSL 3 `param_build.h` (Warden is dropped) |
| `ui` | 64 | 64 | 25 | 39 reach `vulkan.h` (this is VITA-12's work) |
| `rendering` | 96 | 95 | 31 | 65 reach `vulkan.h`; `amd_fsr3_framegen_probe.cpp` needs an FSR3 SDK header (optional, off) |
| **Total** | **445** | **442** | **303** | 139 files stop at `vulkan/vulkan.h`; the other 3 failures are the ones named above |

The "without" column is the honest measure for a Vulkan-free build: **303 files already parse**, and the 139 that do not are exactly the Vulkan coupling VITA-12 has to remove
(`auth`, `network`, `pipeline`, `audio` are clean; the rest of `core`, `game`, `addons` and `ui` are not). The "with" column only says the *other* code in those files is fine.

Read this with two caveats. First, it is syntax only: it says nothing about linking or behaviour. Second,
the files that include Vulkan only get this far in the "with" column because of `-DVK_USE_64_BIT_PTR_DEFINES=1`. On a 32-bit
target Vulkan turns every non-dispatchable handle (`VkPipeline`, `VkPipelineLayout`, ...) into a plain
`uint64_t`, so (in 98 files) `rendering/vk_utils.hpp:41,47` (`destroy(VkDevice, VkPipeline&)` and
`destroy(VkDevice, VkPipelineLayout&)`) become the same overload and **do not compile**. That is one more reason
the Vulkan renderer must be excluded from the Vita build rather than shared (VITA-11/12), not merely
"not used".

32-bit warnings from the sweep (input for VITA-5), all `size_t` constants above 4 GiB that silently become 0:

| File | Constant |
|---|---|
| `include/rendering/wmo_renderer.hpp:861` | 8 GiB |
| `include/rendering/terrain_manager.hpp:478` | 8 GiB |
| `include/rendering/terrain_renderer.hpp:238` | 4 GiB |
| `src/pipeline/asset_manager.cpp:130` | 12 GiB (the only one outside `rendering/`) |

`core/memory_monitor.cpp:135` (`kHardCapBytes`, 16 GiB) is a fifth such constant; the sweep cannot report it because that file stops at the missing `<sys/sysinfo.h>` (G4).

Also `core/input.cpp:43,57,62` and `extern/miniaudio.h:19542` give "comparison always false" (`-Wtype-limits`).

## 3. Per-dependency verdict

Verdicts: **keep** (use as is), **package** (VitaSDK provides it), **vendored** (WoWee's own copy stays), **port**,
**drop**. SDK versions are what the pinned image contains `[sdk]`.

| Dependency | WoWee uses | VitaSDK has | Verdict | Notes |
|---|---|---|---|---|
| **SDL3** | events, gamepad, window | `libSDL3.a` **3.4.16** (+ `SDL3_image`, `SDL3_ttf`) | **package** | Headers are `<SDL3/SDL.h>` as WoWee expects. Android pins `release-3.2.24`, so Vita is newer. Whether SDL owns the display or vitaGL does is VITA-13's call |
| **OpenSSL** | SRP6, SHA1, HMAC, RC4, RAND, one TLS client (`https_get`) | `libssl`/`libcrypto` **1.1.1t-dev** | **package** | See **OpenSSL** below: all auth/network code is 1.1-compatible; only Warden needs 3.x |
| **zlib** | pipeline | `libz.a` **1.3.2** | **package** | round trip OK `[v3k]` |
| **glm** | everywhere | `libglm`, **1.0.3** (WoWee pins 1.0.1) | **package** | Header-only, NEON is a default so no flag needed. WoWee defines `GLM_FORCE_DEPTH_ZERO_TO_ONE` **globally** (`CMakeLists.txt:459`, Vulkan clip space). GL uses [-1,1]: matrices from the shared camera code are wrong for vitaGL unless the Vita build changes that define or compensates (VITA-11/13). Without the define, `glm::perspective` gives z_ndc 0.982 for a point at 10 m, as GL expects `[v3k]` |
| **Lua 5.1** | addon API, FrameXML | only **LuaJIT** (`libluajit-5.1.a`) | **vendored** | WoWee builds `extern/lua-5.1.5` as plain C with no `LUA_USE_*` defines. Ran a script and string formatting on Vita: `lua_Number` is 8 bytes (`double`), `ptrdiff_t` 4 `[v3k]`. LuaJIT is not an option (needs JIT; Lua 5.1 compatibility is intentional, `extern/VERSIONS.md`) |
| **nlohmann/json** | config, manifests | none | **vendored** | `extern/nlohmann`, header-only; parses `[sweep]` |
| **stb_image / stb_image_write** | textures, screenshots | only `stb_rect_pack`, `stb_textedit`, `stb_truetype` | **vendored** | header-only; parses `[sweep]` |
| **Dear ImGui** | all debug and many game panels | `libimgui` **1.61 WIP** (+ `imgui_impl_vitagl.h`) | **vendored** | WoWee vendors **1.92.6 WIP** (submodule). The SDK copy dates from 2018 and the API differs (`GetBackgroundDrawList`, `SetItemTooltip`, `AddKeyEvent` ... are missing). Keep WoWee's, write a small vitaGL backend (the stock OpenGL ES2 backend is the starting point). Note `ImGui::Text("%zu")` hits the printf gap G3. ImGui can route formatting through `stb_sprintf.h` (`IMGUI_USE_STB_SPRINTF`), but that header is in neither ImGui nor the SDK, so it would have to be vendored; casting the two non-rendering sites is simpler |
| **miniaudio** | `audio_engine.cpp` (all audio) | not present (SDK has OpenAL, SoLoud, SDL_mixer, libopus, vorbis, mpg123) | **port** | Compiles with its defaults `[sweep]`. It has no Vita backend; the engine uses `ma_engine_config.dataCallback` and `ma_backend`, which only exist when device I/O is on, so decoder-only (`MA_NO_DEVICE_IO`) does **not** build. Route: miniaudio's `MA_HAS_CUSTOM` backend feeding `sceAudioOut`. Decision in VITA-29 |
| **FFmpeg** | cinematics (per `CMakeLists.txt:417`) and `screen_recorder` | `libav*`, `libsw*` | **drop** | Optional already: CMake sets `HAVE_FFMPEG` only if found, and Android builds without it (`CMakeLists.txt:412-419`). In `src/`, only `core/screen_recorder.cpp` references libav directly. The libs exist if cinematics are ever wanted |
| **Vulkan, vk-bootstrap, VMA** | the renderer | none | **drop** on Vita | Replaced by vitaGL (VITA-11). See the 32-bit note in section 2 |
| **AMD FSR2/FSR3** | optional upscaler | none | **drop** | Already optional; `WOWEE_HAS_AMD_FSR2/FSR3_FRAMEGEN` default to 0 |
| **Unicorn** (Warden x86 emulation) | `warden_emulator.cpp` | none | **drop** | Optional in CMake (`find_library`, `HAVE_UNICORN`); `warden_emulator.cpp` parses without it `[sweep]`. `warden_module.cpp` also uses OpenSSL 3-only `OSSL_PARAM_BLD_*` (see OpenSSL) |
| **StormLib** | asset extraction tools only | none | **drop** | Not linked into the game; extraction stays on the desktop |
| **vitaGL** | (new) | `libvitaGL.a`, `vitaGL.h`, `vgl.h`, plus `libScePiglet`/`SceShaccCg` stubs | **add** | Already in the SDK image, so no vendoring. The SDK exposes no version macro; VITA-13 should record the vitaGL commit it builds against |
| curl, mbedtls, libzip, bullet, ... | not used | present | ignore | available, WoWee has no need |

### OpenSSL: checked in detail

The SDK's OpenSSL is **1.1.1**, while the desktop build takes whatever `find_package(OpenSSL)` finds (3.x today).
`[grep]` of every `openssl/` include and call in `src/`:

- `auth/`, `network/`, `game/` (except Warden) use only 1.1-era API: `SHA1`, `HMAC`, `EVP_sha1`, `BN_*`, `RC4`, `RAND_bytes`, `EVP_Digest`, `EVP_md5`. All compiled `[sweep]` and ran `[v3k]`: SHA1 vector, HMAC-SHA1, `BN_mod_exp`, RC4, `RAND_bytes` returned 1.
- `core/https_get.cpp` uses `SSL_*` calls that exist in 1.1.1; it is compiled out anyway (below).
- **Only `game/warden_module.cpp`** needs 3.x (`OSSL_PARAM_BLD_*`, `EVP_PKEY_fromdata`, `EVP_PKEY_CTX_new_from_name`, `core_names.h`). Warden is dropped on Vita, so this file is excluded; no shim needed.

So the item's expectation "OpenSSL: keep" holds, with the proviso that **Vita-built code must stay within the 1.1.1 API**. A future upstream change that uses a 3.x function in `auth/` or `network/` would break the Vita build; the sweep (or VITA-31 CI) will show it.

### HTTPS and open-URL

`src/core/https_get.cpp` (only caller: `core/update_check.cpp`) and `src/core/open_url.cpp` (callers: `ui/auth_screen.cpp`,
`ui/chat/chat_markup_renderer.cpp`, `addons/lua_system_api.cpp`) have no use on Vita (no browser, no update channel).
Both parse fine today `[sweep]`; `open_url` shells out with `xdg-open` on Linux, which must not be reached. Plan: a gated no-op
arm in those two functions (`#elif defined(__vita__)` returning failure) rather than excluding files, because five other files call them.

## 4. Threads

- **Core count.** `std::thread::hardware_concurrency()` returned **0** `[v3k]`. Whether a real Vita says 0, 3 or 4 is `[hw?]`.
- **Safe on 0 today.** The five call sites `[grep]` all cope with 0: `core/thread_pool.hpp:84` falls back to **4** (more than the 3 cores an
  app may use, so VITA-8 should pass an explicit count), `m2_renderer.cpp:504`, `wmo_renderer.cpp:192` and `character_renderer.cpp:342` fall back to 1
  (`hc > 1 ? hc - 1 : 1`), and `terrain_manager.cpp:116` skips its tuning when 0.
- **`std::thread` works only with the link flag in G1.**
- **Stack and affinity (VITA-8, `[v3k]`).** A `std::thread` gets a 32 KB stack; `pthread_create` is wrapped at link time to raise it (see `DEV_SETUP.md` section 13). `sceKernelChangeThreadCpuAffinityMask` and `sceKernelChangeThreadPriority` work and the system core (mask `0x80000`) is refused. Whether a real device behaves the same is `[hw?]`.
- 4 threads, `condition_variable`, `shared_mutex`, `std::async`, `thread_local`, and `atomic::wait/notify` all behave `[v3k]`.
- `thread_local` appears in 10 files `[grep]`. It works on a second thread; it uses emulated TLS from libgcc (the link needs `pthread_key_create`), which is the
  same dependency as G1.

## 5. Gaps found (with workarounds)

| # | Gap | Impact | Workaround | Where it is handled |
|---|---|---|---|---|
| **G1** | `std::thread` throws `Enable multithreading to use std::thread: Not owner`. GCC 15's `__gthread_active_p()` tests a *weak* reference to `pthread_cancel`. In a static link nothing pulls that `libpthread.a` member in, so the reference is 0. Raw `pthread_create` works; the link also needs `-lpthread` explicitly (undefined `pthread_*` otherwise) `[v3k]` | Every `std::thread` user in the game (thread pool, terrain workers, async pump) would throw on startup | Link `pthread` and force the symbol: `-Wl,-u,pthread_cancel` (`tools/vita/depcheck/CMakeLists.txt`). Verified: all thread tests pass with it. **Side effect found in VITA-6:** with that option the libstdc++ guards for function-local statics crash (`__cxa_guard_acquire`); `src/platform/vita/vita_cxa_guard.cpp` replaces them (`DEV_SETUP.md` section 12) | **VITA-4** (build), needs the same lines; every Vita executable also needs the guard file |
| **G2** | `std::filesystem` does not understand `ux0:` paths: `is_absolute("ux0:data/x")` is false, `absolute()` and `weakly_canonical()` return `app0:/ux0:data/x`. Also `fs::space()` reports 4294967295 MB, and **`fs::remove_all()` never returns** on a non-empty tree (spins on `sceIoRemove`, "Directory not empty") `[v3k]`. Create, rename, iterate, `file_size`, `relative`, `remove` and `current_path("ux0:...")` followed by a relative open all work | Eight call sites resolve paths: `ui/auth_screen.cpp:1008,1093`, `core/window.cpp:35`, `core/config_paths.cpp:26-29`, `addons/addon_manager.cpp:145-146`, `game/opcode_table.cpp:106`, `game/zone_manager.cpp:23`. None calls `remove_all` or `space` `[grep]`. 163 files use `std::filesystem`, so it has to be right | A Vita path helper (resolve against a known `ux0:` data root, never call `absolute`/`canonical` on a device path); a ban on `remove_all`. Re-check the three emulator-only oddities on hardware | **VITA-35** (done: `include/platform/vita/device_path.hpp`, details in `DEV_SETUP.md` section 11; also found `fs::equivalent` wrong, VITA-41); related VITA-6 (paths) |
| **G3** | newlib printf on Vita ignores the `z`, `t` and `j` length modifiers: `%zu` prints the letters `zu` and **does not consume its argument**, so every later argument shifts (`snprintf("%d|%zu|%d", 1, 2, 3)` gave `1|zu|2`). `%lld`, `%llu`, `%llx`, `%lu`, `PRIu64`, `%.2f`, `%g` are correct `[v3k]` | Wrong text, and the shifted arguments make a later `%s` read a number as a pointer: a crash, not just a typo. Seven sites `[grep]`: `ui/game_screen.cpp:1263`, `ui/combat_ui.cpp:776` and five in `rendering/performance_hud.cpp`. Two outside `rendering/` | Cast to `unsigned long` at the two non-rendering sites (an `upstream-candidate` 32-bit fix, VITA-5); ImGui's own `Text` calls are the two cases; a vendored `stb_sprintf.h` would be the global fix | **VITA-5** |
| **G4** | `<sys/sysinfo.h>` is missing, so `core/memory_monitor.cpp` does not compile; the 4, 8, 12 and 16 GiB `size_t` constants overflow (section 2; `memory_monitor.cpp:135` is the 16 GiB one) | Memory sizing is wrong or does not build | Vita arm in the `memory_monitor.cpp` platform ladder reading the real budget (VITA-23); widen the constants in section 2 | **VITA-5**, **VITA-23** |
| **G5** | Vulkan 32-bit handle typedefs make `vk_utils.hpp` overloads collide (section 2) | Only matters if Vulkan code were shared | Keep Vulkan files out of the Vita build | **VITA-11/12** |

Not a gap, but worth knowing for VITA-7 (networking): BSD sockets work through newlib once `sceNetInit` has run `[v3k]`.
`socket`, `setsockopt(TCP_NODELAY)`, `fcntl(O_NONBLOCK)`, `getaddrinfo` (numeric and `localhost`), non-blocking `connect`
(returns -1 with `errno == EINPROGRESS`), `select` for writability, `send` and `recv` all worked against a local echo server.
**But `getsockopt(SO_ERROR)` returns sceNet/BSD error numbers, not newlib's**: a refused connection gave `61`, while newlib's `ECONNREFUSED` is
`111`. `network/tcp_socket.cpp:55` and `world_socket.cpp:186` only test `SO_ERROR != 0`, which is fine; any code that compares it to a named
`E*` constant would not match.

## 6. DepCheck: the runtime probe

`tools/vita/depcheck/` is a small C++20 app (`main.cpp`) plus the vendored Lua, linked against OpenSSL, zlib, glm, pthread and the sceNet
stubs. It logs `PASS`/`FAIL`/`INFO` lines to `ux0:data/wowee/depcheck.log` and exits on its own. **A `FAIL` line is a finding, not a probe
bug**: the four `FAIL`s in the last run are exactly G3 (`%zu`, `%zd`, `%td`, `%jd`).

Last run on Vita3K v0.2.1 (emulator): **78 PASS, 5 FAIL** (four `%z*`/`%j*` printf, G3; one `fs::equivalent` true for two different directories, G2/VITA-41), then the deliberate `fs::remove_all` test, which hangs (G2). The 30 new PASS lines are the `devpath` section (VITA-35).
The 33 PASS lines of the first complete run covered the C++20 and library checks; the later runs added sockets, `current_path` and printf.

Reproduce:

```sh
# build (pinned container image, needs no host toolchain)
SRC=tools/vita/depcheck BUILD=build-vita/depcheck tools/vita/build.sh
# the socket test needs a TCP echo server on the dev machine (port 9998)
python3 tools/vita/depcheck/echo_server.py &
tools/vita/vita3k_macos.sh build-vita/depcheck/depcheck.vpk DEPC00001 --seconds 45 --elf build-vita/depcheck/depcheck
cat build-vita/logs/DEPC00001.app.log
# syntax sweep of every source file (about 1.5 min with 8 CPUs; see the notes in sweep.sh)
tools/vita/depcheck/sweep.sh            # or: tools/vita/depcheck/sweep.sh network auth
```

Notes for whoever repeats this:
- The sweep's container needs more than Apple `container`'s default 1 GB; `sweep.sh` asks for `--memory 10G --cpus 8` there (tunable with `MEMORY`, `CPUS`, `JOBS`).
  With the default, parallel `g++` on WoWee's headers thrashes and does not finish.
- `vita3k.log` grows to gigabytes during a long run (trace level). Delete it after use.
- The `fs::remove_all` test at the end hangs in the emulator by design; Vita3K is stopped by the script's timeout.
- To see past `vulkan/vulkan.h` in the sweep, clone `https://github.com/KhronosGroup/Vulkan-Headers` into `build-vita/vulkan-headers`; the vendored ImGui and
  vk-bootstrap submodules must be checked out (`git submodule update --init --depth 1 extern/imgui extern/vk-bootstrap`).

## 7. Not verified (and where it gets verified)

- Everything tagged `[v3k]` ran on an emulator. Values that depend on the OS or hardware (`hardware_concurrency`, `fs::space`, the `remove_all` hang,
  `SO_ERROR` numbers, DNS) need a real Vita: **VITA-34**.
- Link and runtime behaviour of the *real* WoWee sources is untested; the sweep proves only that they parse. Linking starts with VITA-4 and VITA-9.
- vitaGL's version, build flags and whether SDL3 or vitaGL owns the display are decided in VITA-13, not here.
- The audio path (miniaudio custom backend) is a plan, not a test (VITA-29).
- The sweep stood in desktop-only inputs (Vulkan headers, `VK_USE_64_BIT_PTR_DEFINES`); a Vulkan-free build of those files is VITA-12.
