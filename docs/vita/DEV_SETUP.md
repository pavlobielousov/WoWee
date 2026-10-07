# Vita dev environment

How to build, run, log and debug WoWee for the PS Vita. Everything here lives in `docs/vita/` and
`tools/vita/`; no upstream file is touched (see [FORK_POLICY.md](FORK_POLICY.md)).

**Verification status.** Sections marked *(verified)* were run on the maintainer's Mac (Apple
Silicon, `container` CLI, Vita3K v0.2.1). Sections marked *(unverified)* need a Vita or Vita3K and have not been
run yet; the acceptance checklist at the bottom is where they get confirmed.

## 1. Toolchain

### Option A: container image (recommended) *(verified: build)*

The image `vitasdk/vitasdk` is multi-arch (`linux/amd64`, `linux/arm64`), so it runs natively on
Apple Silicon. `$VITASDK` and `PATH` are already set inside it.

- **Pin a dated tag** in scripts and CI: currently `vitasdk/vitasdk:2026.08-20260925`
  (GCC 15.2.0, `arm-vita-eabi`). The floating `2026.08` is rebuilt as packages move; use it only for
  experiments. Dated tags are listed at https://hub.docker.com/r/vitasdk/vitasdk/tags.
- Runtime: macOS uses Apple's `container` CLI (`container system start` once per boot); Linux and CI
  use `docker` or `podman`. Override with `CONTAINER_RUNTIME=...`, and the image with `VITASDK_IMAGE=...`.

```sh
tools/vita/build.sh                                   # repo root: stub build-vita/wowee.vpk (VITA-4)
SRC=tools/vita/devcheck BUILD=build-vita/devcheck tools/vita/build.sh   # the dev-loop test app
```

The root build (`cmake/vita/Vita.cmake`, hooked from the root `CMakeLists.txt` when `VITA` is set) produces
`build-vita/wowee.vpk`, title ID `WOWE00001`. Since VITA-6 that is the shared startup path (section 12), not a stub: it logs and exits.
Smoke test: `tools/vita/vita3k_macos.sh build-vita/wowee.vpk WOWE00001 --seconds 15` *(verified)*. The ELF for
crash symbolization is `build-vita/wowee` (built with `-g`). The title ID is **not checked against VitaDB**
(its list API returned nothing from here); art in `resources/vita/sce_sys/` is plain placeholder.

What the toolchain and its libraries can and cannot do (C++20, threads, `std::filesystem`, printf, OpenSSL, per-dependency
verdicts) is in [DEPENDENCIES.md](DEPENDENCIES.md); `tools/vita/depcheck/` is the probe behind it.

`build-vita/` is not in the upstream `.gitignore` (shared file, left alone). Add it locally:
`echo 'build-vita/' >> .git/info/exclude`.

### Option B: native `vdpm` *(unverified)*

```sh
git clone https://github.com/vitasdk/vdpm && cd vdpm
./bootstrap-vitasdk.sh
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH
./install-all.sh
```

Then `cmake -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake ...`. Prefer Option A: it
pins the exact toolchain and does not touch the host.

## 2. Hardware

- A hacked Vita: firmware 3.60 or 3.65 with Enso is the recommended baseline (HENkaku / h-encore /
  Trinity also work). A PS TV runs homebrew too.
- **VitaShell**: FTP server (press SELECT; port **1337**) and VPK installer.
- **SD2Vita** adapter + microSD for the game data (several GB; see VITA-24).
- Optional: **vitacompanion** plugin (FTP plus a command server on port 1338 to launch/kill apps from
  the PC).
- **Clocks for measurements:** record the stock clocks, CPU 444 MHz, GPU 222 MHz, bus 222 MHz, and do
  not install an overclocking plugin (PSVshell etc.) unless a measurement needs it. If you do, write
  the clocks next to the numbers.

## 3. Emulator: Vita3K *(verified: install, run, log, crash)*

Vita3K is for **smoke tests only** (does it boot, does the log appear, does it crash where expected).
It does **not** replace hardware: its speed and memory behaviour say nothing about the real device, so
every performance or memory number must come from a Vita.

It is fully scriptable from the command line, with no clicking. The driver is macOS-only (it refuses to run
elsewhere); a Linux/CI equivalent would be a separate `vita3k_linux.sh`, not written yet.

```sh
tools/vita/build.sh ...                      # build the VPK (section 1)
tools/vita/vita3k_macos.sh build-vita/devcheck/devcheck.vpk DEVC00001 --seconds 16 \
    --cmd crash --elf build-vita/devcheck/devcheck
```

`vita3k_macos.sh` installs the VPK, launches it with `-r <TITLEID>`, optionally drops a command into the
app's `ux0:data/wowee/devcheck.cmd` after `--cmd-after` seconds (`quit` or `crash`), stops the emulator
and writes `build-vita/logs/<TITLEID>.app.log` (the app's own log file) and `vita3k.log` (the emulator
log). It reads the emulator's `pref-path` from `config.yml`, so it finds `ux0:` wherever it lives; set
`VITA3K_BIN` if the binary is not in the default place.

**Screenshots:** add `--screenshot S` to capture the emulator window S seconds after launch to
`build-vita/logs/<TITLEID>.png`. Only that window is captured (`tools/vita/macos_window_id.swift` finds its
window id for `screencapture -l`), never the whole screen. The terminal needs Screen Recording permission
(System Settings > Privacy & Security). devcheck draws nothing, so its shot is an empty window; it proves
the capture path, and becomes useful once a renderer exists.

**Crashes:** the emulator has no core dump. It prints `Invalid read/write ... PC: 0x810004d6 LR: ...` in
its log, and `tools/vita/symbolize_emu.sh <vita3k.log> <elf>` (called by `vita3k_macos.sh --elf`) turns that
into `main at main.c:101` with `arm-vita-eabi-addr2line`. Real-device dumps use `parse_core.sh` (section 6).

**Deeper crash inspection: gdb at the crash PC** *(verified on devcheck)*. The emulator log only gives PC, LR and
registers. Vita3K has a gdbstub (`gdbstub: true` in `config.yml`, port 2159) and the VitaSDK image has
`arm-vita-eabi-gdb`, but on this build (v0.2.1) the stub does not report a crash to gdb, so gdb cannot simply
"catch" it. Instead, run in two passes:

```sh
# pass 1: crash it; the emulator log gets "PC: 0x810004d6"
tools/vita/vita3k_macos.sh build-vita/devcheck/devcheck.vpk DEVC00001 --cmd crash --elf build-vita/devcheck/devcheck
# pass 2: same run under the stub with a breakpoint at that PC; gdb stops just before the faulting instruction
tools/vita/gdb_at_pc.sh DEVC00001 build-vita/devcheck/devcheck --cmd crash
```

Pass 2 prints the thread list, `bt full` (locals of every frame), all registers, the next instructions and 32 stack
words, and keeps the whole session in `build-vita/logs/gdb_at_pc.txt`. `--pc` overrides the PC taken from the log;
`--skip N` ignores the first N hits when the crashing line also runs earlier without crashing; exit status is
0 (stopped), 3 (could not connect) or 4 (breakpoint never hit). It uses a throwaway copy of `config.yml`, so your
own Vita3K settings are not touched.

Limits (this build; upstream PR Vita3K#3879 improves the stub but was still open when checked): needs a
**repeatable** crash; Ctrl-C, single-step (`stepi`) and detach do not work, so a session can only break and inspect;
the backtrace is only as deep as the stub's unwinding (one caller frame was verified); connections occasionally time
out (the script retries); the stub listens on all interfaces, so only run it on a trusted network. The stub also
cannot detach, so a finished session leaves a TCP socket on port 2159 that blocks the next session; a second run
started soon afterwards waits for it to clear (up to a few minutes) and says so.

Quirks found while scripting it:
- Vita3K ignores SIGTERM while an app runs, so the script escalates to `kill -9` (only on the instance it
  started; a Vita3K you have open yourself is left alone).
- `Vita3K <file.vpk>` installs but its "auto-boot" did not start the app, so install and run are two steps.
- The log level in `config.yml` is trace, so the emulator log gets large; the app log stays small.
- The emulator window opens on the desktop while a script runs (no headless mode used here).
- Starting from nothing works: with no app installed, `vita3k_macos.sh` installs the VPK itself.

## 4. Deploy loop *(unverified on hardware)*

Install `devcheck.vpk` (or later the WoWee VPK) once with VitaShell, then iterate by replacing only
`eboot.bin` over FTP:

```sh
export VITA_IP=192.168.1.50                      # shown by VitaShell
tools/vita/deploy.sh build-vita/devcheck/eboot.bin DEVC00001
VITA_RESTART=1 tools/vita/deploy.sh ...          # with vitacompanion: kill and relaunch the app
tools/vita/deploy.sh --logs                      # fetch ux0:data/wowee/* into build-vita/logs/
```

## 5. Logging

Decision: **two sinks, both always available.**

1. **File** `ux0:data/wowee/<app>.log` (WoWee: `wowee.log`): survives crashes, fetched over FTP.
2. **UDP net-logger** (optional): each line is also sent as one UDP datagram. devcheck reads
   `ux0:data/wowee/loghost.txt` (`<ip>[:port]`); **WoWee reads `WOWEE_LOG_UDP=<ip>[:port]` from
   `ux0:data/wowee/env.txt`** (section 12), default port 9999. On the dev machine: `tools/vita/logsink.sh`
   (a thin `nc -ul`). UDP is lossy by design; the file is the source of truth.

`tools/vita/devcheck/main.c` is the reference implementation for devcheck; WoWee's own sink is the `__vita__` arm
in `src/core/logger.cpp` calling `src/platform/vita/vita_log_sink.cpp`.

## 6. Crash dumps

Homebrew crashes write a `*.psp2dmp` core dump (gzip) to `ux0:data`. Symbolize with
[`vita-parse-core`](https://github.com/xyzz/vita-parse-core), which needs the **unstripped ELF from
the same build** as the `eboot.bin` that crashed (the ELF target, not `eboot.bin` or `.velf`), built
with `-g`.

```sh
tools/vita/deploy.sh --logs     # or copy the dump from ux0:data with VitaShell
tools/vita/parse_core.sh path/to/core.psp2dmp build-vita/devcheck/devcheck
```

The script runs the tool in the VitaSDK image (it needs the toolchain's `objdump` and `addr2line`),
pinned to commit `644b5f0`. Notes from setting it up *(verified)*: the image has no pip, and the tool
needs `pyelftools<=0.29` (0.30 removed `elftools.common.py3compat`), so the script installs
`python3-pip` and `pyelftools==0.29` on each run. **Keep the ELF of every build you deploy**; a dump
cannot be symbolized without it.

## 7. Test server

Use a local AzerothCore 3.3.5a (optionally vmangos 1.12 for Vanilla) in Docker on the LAN; the Vita
connects to the host's LAN address, port 3724. Setup reference: [../server-setup.md](../server-setup.md)
and https://www.azerothcore.org/wiki/. Create the account and raise it to GM level for teleporting:

```
account create vita <password>
account set gmlevel vita 3 -1
```

(AzerothCore worldserver console; `.tele`, `.go xyz` etc. are then available in game.) *(unverified:
no server has been brought up for this project yet.)*

## 8. Acceptance checklist (VITA-2)

- [x] `tools/vita/build.sh` builds `devcheck.vpk` + `eboot.bin` in the pinned image on macOS (arm64).
- [ ] *(deferred)* devcheck installed and run on **hardware**; log fetched over FTP and via `logsink.sh`.
- [x] devcheck installed, run, logged and driven (`quit`/`crash`) on **Vita3K** by `vita3k_macos.sh`; the
      forced null write is symbolized to `main.c:101` by `symbolize_emu.sh`.
- [ ] *(deferred)* TRIANGLE crash on hardware produces a `.psp2dmp`; `parse_core.sh` symbolizes it against
      `build-vita/devcheck/devcheck` and shows `main` / `main.c` at the faulting line.
- [ ] *(deferred)* Local AzerothCore reachable from the Vita's network (the emulator can reach it too).

## 9. Desktop build check (VITA-36) *(verified)*

Run this before a PR that changes shared C++ logic (VITA-5, VITA-12; not needed for small gated edits). It builds
and tests the Linux desktop client in a container, so nothing is installed on the host.

```sh
tools/vita/desktop_check.sh all        # image if missing, configure, build, ctest (output in build-desktop/)
tools/vita/desktop_check.sh build      # incremental rebuild; also: image, configure, test, shell
```

- Image `wowee-desktop-builder` is built from `tools/vita/desktop-builder.Dockerfile`, a copy of upstream's
  `container/builder-linux.Dockerfile` plus `ca-certificates` (upstream's file fails to clone SDL3 under
  `--no-install-recommends`: "server certificate verification failed"). Re-sync it by hand if upstream's changes.
  Upstream's own `container/run-linux.sh` is Docker-only, copies the tree, clones the FSR SDKs and uses LTO, so it is not used.
- Apple `container` VMs default to 1 GB and 4 CPUs; the script asks for `MEMORY=10G`, `CPUS=8`.
- `extern/imgui` and `extern/vk-bootstrap` are initialised on the host by the script if empty.
- Add `build-desktop/` to `.git/info/exclude` (the shared `.gitignore` is left alone).
- **Baseline** (Apple Silicon, linux/arm64 container, Release, Ninja, 10 GB / 8 CPUs, `vita` at `247ee1ae`):
  configure + full build + ctest took **340 s** after the image existed; **218 of 218 tests passed**. Any failure
  that is not in this baseline is a regression.


## 10. Running the unit tests on a 32-bit target (VITA-37) *(verified: armhf under qemu-user, Apple Silicon, container)*

**Decision: cross-build the test targets for armhf (ARMv7, hard float, ILP32; the Vita's ABI) in the existing arm64 Linux
container and run them with `qemu-arm` (qemu-user).** It works today, needs no host install, and found a real 32-bit bug
on the first run. Implemented as `tools/vita/check32.sh` in VITA-38.

Measured 2026-10-02 (Apple Silicon Mac, `container` VM with 10 GB / 8 CPUs; clean state):

| Step | Result |
|---|---|
| Image `wowee-armhf` (`tools/vita/check32/Dockerfile`, on top of `wowee-desktop-builder`) | 44 s to build. gcc/g++ 13.3 cross compilers, `qemu-user`, armhf OpenSSL and zlib (ports.ubuntu.com via `dpkg --add-architecture armhf`), SDL3 built for armhf without a video backend. Image size not measured. |
| Cross-configure of the whole tree (`-DWOWEE_BUILD_TESTS=ON`) | about 2 s |
| Build of all 212 `test_*` targets | about 2 min (8 CPUs, cold) |
| `ctest -j8` under `qemu-arm` (without `sweep_guard`) | 17 s wall; slowest `settings_apply_on_load` 16 s, `rt_bvh` 5 s |
| Result | **213 of 217 pass**; the 4 failures are listed below. After VITA-39/VITA-40 (`warden_reloc` fixed, one test added): 215 of 218, the 3 left are not 32-bit results |
| Sanity | binaries are `ELF32 ARM`, `sizeof(size_t) == 4`; running one directly fails with "Exec format error", so the emulator is needed |

What the 4 failures are:
- `warden_reloc`: **a real 32-bit bug, fixed in VITA-39.** `wardenRelocTargetFits` (`include/game/warden_module.hpp:70`) computes `static_cast<size_t>(target) + 4u <= moduleSize`; with a 32-bit `size_t`, `target = 0xFFFFFFFF` wraps to 3 and passes the bounds check, so an attacker-supplied Warden relocation could write at `image + 0xFFFFFFFF`. The test already covers it; it only fails on a 32-bit build. Not fixed in the spike item (Warden is dropped on Vita, but the code is shared and affects any 32-bit build); fixed in VITA-39 with `core::rangeFits`, which also covers the import table check (`wardenImportTableFits`, same wrap with `importCount * 8`).
- `extract_progress`: needs StormLib, which the image does not have for armhf (and the Vita does not need it).
- `framexml_compiles`, `addon_xml_compiles`: not a 32-bit result. Their helper `framexml_compile_check` is not a `test_*` target, so the spike never built it, and they need `Data/interface`, which is absent (skipped, exit 77, on a desktop with data).
- `sweep_guard` was excluded: it runs host Python sweeps, is not 32-bit relevant and exceeds a 120 s timeout there.

Run it (`tools/vita/check32.sh`, VITA-38; same shape as `desktop_check.sh`):

```sh
tools/vita/check32.sh all          # image if missing (builds wowee-desktop-builder first if needed), configure, build, test
tools/vita/check32.sh test         # ctest only; CTEST_ARGS="-R warden" narrows it
tools/vita/check32.sh shell        # a shell in the container; build output is build-armhf/ (add it to .git/info/exclude)
```

**Baseline (2026-10-02, `vita` at VITA-39, from a clean state):** 213 test executables built, **217 tests, 0 failed**, 2 skipped (`framexml_compiles`, `addon_xml_compiles`: exit 77 because `Data/interface` is absent, the same as on a desktop without game data). ctest takes about 18 s (slowest `settings_apply_on_load`, 17 s); an incremental build with nothing to do is under 1 s. Any failure is a regression. The script exits non-zero on a failing test (checked by breaking one on purpose: exit 8).

Excluded on purpose, in `EXCLUDE_TESTS` in the script: `extract_progress` (an asset-extraction test; needs an armhf StormLib the image does not have, not used on the Vita) and `sweep_guard` (host Python sweeps, not 32-bit relevant, exceeds a 120 s timeout under emulation). The targets to build are read from `ctest --show-only=json-v1`, so a new test needs no edit; helper executables such as `framexml_compile_check` are built too.

The Dockerfile pins the host's apt sources to `dpkg --print-architecture`, so it should also build on an amd64 Linux runner (armhf packages come from ports.ubuntu.com either way). **Verified on arm64 only**; the CI job (VITA-31) is where that gets checked.

Pitfalls found (all are encoded in `tools/vita/check32/` and `check32.sh`):
- CMake silently picks the **arm64** `libssl`, `libz` and `libvulkan` from `/usr/lib/aarch64-linux-gnu` unless their armhf paths are given explicitly; the link then fails with "file format not recognized". The root CMake also makes the 23 packet tests link `Vulkan::Vulkan`, so an empty armhf `libvulkan.so` stub is used (no Vulkan code is compiled or run in those tests).
- The configure fails on SDL3 until an armhf SDL3 exists. SDL refuses to configure with no video backend unless `-DSDL_UNIX_CONSOLE_BUILD=ON`.
- With `dpkg --add-architecture armhf` the arm64 apt sources must be pinned to `Architectures: arm64`, otherwise `apt-get update` looks for armhf on the wrong mirror.

What this does **not** cover:
- Only GPU-free tests run; Vulkan code cannot be compiled for 32-bit (see VITA-3), so rendering is untested. 212 of the 218 desktop tests are `test_*` targets and almost all of them link only Catch2 plus a handful of sources, so the covered set is large.
- Linux armhf glibc is not newlib: newlib-only traps (`%zu`, `std::filesystem` on `ux0:`) do not show up here. Those remain Vita3K and compile-only territory.
- qemu-user does not model the Vita's memory limit, CPU speed or threading limits.
- Measured on Apple Silicon only; Apple CPUs have no AArch32 mode, which is why emulation is needed at all.

**Compile-only widening (`-Wconversion`)**, `WARN_FLAGS=-Wconversion OUT=build-vita/sweep-conv tools/vita/depcheck/sweep.sh auth network core game pipeline addons audio` (57 s, VitaSDK GCC 15.2; the sweep now accepts `WARN_FLAGS` and `OUT`): 253 unique warnings, of which only 18 are 64-bit to 32-bit truncations. The rest is width-independent noise (`int` to `float` 120+ incl. miniaudio, `int` to `uint8_t`/`uint16_t` about 90). Do not enable it as a gate. **Triage (VITA-40).** All 18 sites the sweep flags as 64-to-32 were read; **none is a real 32-bit bug**, so none was changed:
- `spline_packet.cpp:195,333`: the counts are capped first (`pointCount <= 1000`, `pc <= 256`), so the product fits.
- `network/packet.cpp:39`: `writeUInt64` splits a value with `& 0xFFFFFFFF`; intentional.
- `audio_engine.cpp:142,152`: the buffer is capped at 60 s of audio (`sampleRate * 60` frames).
- `packet_parsers_classic.cpp:1295,1296`: `uint64_t` money/COD to the 32-bit wire field. Narrows on 64-bit too (not a 32-bit finding); the gold cap is below 2^32 copper.
- `streamoff` to `streamsize` in `music_manager.cpp:140`, `auth/integrity.cpp:23`, `warden_handler.cpp:106`, `warden_memory.cpp:720`, `loose_file_reader.cpp:22` and 5 in `asset_manager.cpp`: the buffer is sized with `size_t(size)` and then read with `size`; the two only differ for a file of 2 GiB or more, which cannot be loaded into a Vita's RAM anyway. A bad read count there fails the stream; it never overruns the buffer.
- `vanilla_crypt.cpp:17`, `auth_packets.cpp:66`: `size_type` narrowed to a byte/short; not width-specific.

**`-Wconversion` is the wrong detector.** The bug class that matters, a bounds check `size_t(x) + n > size` or `size_t(count) * k > size`, gives no warning at all. A grep for that shape over the non-UI, non-rendering sources found 25 candidates. Most are safe (`uint8` or capped counts: `packet_parsers_classic.cpp:552,626`, `packet_parsers_tbc.cpp:167`, `spline_packet.cpp:253`, `social_handler.cpp:393`, `world_packets_world.cpp:1537`). The real ones, all taking offsets or counts from the file or packet, are fixed with `core::rangeFits` (`include/core/size_utils.hpp`, compares by subtraction): `m2_loader.cpp` (`readArray`, `readString`, particle and ribbon emitter ranges) and `m2_color_track.hpp` (4 checks, plus a `uint32` address sum that wrapped on every target). Their comment about `offset=0xFFFFFFFF` was fixed for 64-bit only. `warden_module.cpp:71,1087` is VITA-39. Tests: `test_size_utils`, and new cases in `test_m2_color_track` that failed on armhf before the fix. `readArray` and `readString` are in an anonymous namespace and are covered through the helper, not directly.

**Not tried:** i386 (`-m32`) and the Rosetta question. Step 3 succeeded, so the plan said to skip it; the claim that Rosetta for Linux cannot run 32-bit x86 remains unverified. armhf is also the closer match to the Vita (same ILP32, alignment and hard-float ABI), so i386 would only be a fallback.

**Fallbacks if armhf ever becomes impractical** (not needed now): run chosen tests as a Vita app on Vita3K like `tools/vita/depcheck/`, which tests newlib but gives pass/fail only through the log; or rely on the compile-only sweep. That would lose real execution of the 200+ Catch2 tests.

## 11. Device paths and `std::filesystem` (VITA-35) *(verified on Vita3K; hardware is VITA-34)*

`std::filesystem` does not know the `ux0:` / `uma0:` / `app0:` roots. Measured by `tools/vita/depcheck` (section `devpath`):

| Call | On a device path | Rule |
|---|---|---|
| `absolute()`, `weakly_canonical()` | `app0:/ux0:data/x` under cwd `app0:`; an **error** (`ec=2`, and a throw in the overload without `ec`) after a chdir to `ux0:` | never call on a device path; use `platform::vita::resolveDevicePath()` |
| `canonical()` on an **existing** relative file | correct (`app0:/eboot.bin`, `ux0:/data/...`) | fine |
| `current_path() / "rel"` | correct (`app0:/assets/x`) | fine |
| `fs::equivalent(a, b)` | **true for two different directories** | do not trust it on Vita (VITA-41) |
| `remove_all()` on a non-empty tree | never returns | never call it; use `platform::vita::removeTree()` |
| `space()` | nonsense (4294967295 MB) | do not use; a `sceIoDevctl` helper waits for VITA-25 |
| `create_directories`, `rename`, `remove`, `file_size`, directory iteration, `current_path("ux0:...")` | work | fine |

The cwd is `app0:` at launch and `ux0:/data/...` (note the slash) after a chdir.

`include/platform/vita/device_path.hpp` (header-only, pure string work except `removeTree`): `hasDeviceRoot`, `normalizeDevicePath`, `resolveDevicePath(p[, cwd])`, `removeTree`. It always emits `dev:/rest`, resolves relative paths against `current_path()`, and stops `..` at the device root, so two spellings of one file give one string (`opcode_table.cpp` uses that as its cycle-detection key). Host test, no SDK needed:

```sh
c++ -std=c++20 -Wall -Wextra -Werror -Iinclude tools/vita/depcheck/devpath_test.cpp -o /tmp/devpath_test && /tmp/devpath_test
```

The Vita arms are in `game/opcode_table.cpp` and `addons/addon_manager.cpp`. The other call sites in the VITA-35 list were probed and need no change: `auth_screen.cpp` (both sites), `config_paths.cpp` (the `current == root` comparison can miss on spelling, `app0:` vs `app0:/`, which only repeats a harmless chdir), `zone_manager.cpp` (`canonical()` of an existing file works), and `core/window.cpp:35` is macOS-only code.

## 12. Platform layer: startup, env.txt, paths, logging (VITA-6) *(verified on Vita3K; hardware is VITA-34)*

`src/platform/vita/` + `include/platform/vita/vita_platform.hpp`. The root build (`cmake/vita/Vita.cmake`) compiles the shared
`src/main.cpp`, `core/logger.cpp` and `core/config_paths.cpp` with it. `main()` calls `platform::vita::initProcess()` first, runs the
shared startup path, then `logStartupReport()` instead of building the `Application` (that needs the Vulkan renderer: VITA-9 and
VITA-12), and returns normally, so the log is flushed by the static destructors.

| Piece | Where | Notes |
|---|---|---|
| Heap and stack | `vita_main.cpp` | `_newlib_heap_size_user` = 192 MB, `sceUserMainThreadStackSize` = 4 MB (**only in effect with the `-Wl,-u,sceUserMainThreadStackSize` link option, added by VITA-8; before that the main thread had 256 KB on hardware**, see section 13). **Compile-time** (newlib reads them before `main`), so env.txt cannot change them: `cmake -DWOWEE_VITA_HEAP_MB=.. -DWOWEE_VITA_STACK_MB=..`. The real budget comes from VITA-23. |
| Clocks, modules | `initProcess()` | ARM 444, bus 222, GPU 222, xbar 166 MHz; `SCE_SYSMODULE_NET`, `sceNetInit` (256 KB static buffer), `sceNetCtlInit`. Return codes are logged, not acted on. |
| env.txt | `vita_env.cpp` | `ux0:data/wowee/env.txt`, `KEY=VALUE` lines applied with `setenv`, so `core/env.hpp` and every `getenv` switch work unchanged. LF or CRLF, `#` comments, blanks trimmed, optional `export `, optional quotes. The startup report lists the **keys** only (values may be credentials). Host test: `c++ -std=c++20 -Wall -Wextra -Werror -Iinclude tools/vita/depcheck/envfile_test.cpp src/platform/vita/vita_env.cpp -o /tmp/envfile_test && /tmp/envfile_test`. |
| Data root | `data_paths.hpp` (`userDataRoot`), `initProcess()` | Default `ux0:data/wowee/Data` (set as `WOW_DATA_PATH` unless env.txt sets it). For SD2Vita or USB: `WOW_DATA_PATH=uma0:data/wowee/Data` in env.txt. |
| Config root | `config_paths.cpp` | `ux0:data/wowee/config`; `WOWEE_CONFIG_ROOT` still wins. `getExecutableDir()` is empty on Vita, so there is no portable mode. |
| Log file | `logger.cpp` | `ux0:data/wowee/wowee.log` (the `logs/` beside the working directory is skipped: `app0:` is read-only on a device). UDP sink as in section 5. |
| Crash handling | none needed | A Vita build defines neither `__linux__` nor `__APPLE__`, so `main.cpp` takes the no-backtrace `signal()` path. Use core dumps (section 6). |
| Clock | none needed | `std::chrono::steady_clock` works (VITA-3). `system_clock` returned 1970 timestamps on Vita3K, so log timestamps are not wall time there (check on hardware, VITA-34). |

**Thread-safe statics are broken in libstdc++ on the Vita; `vita_cxa_guard.cpp` replaces them.** With `-Wl,-u,pthread_cancel` (needed for
`std::thread`) the first function-local static with a non-trivial initialiser crashed in `__cxa_guard_acquire` (`pthread_mutex_lock` reads
address 0), and once past that `__cxa_guard_release` failed in `pthread_cond_broadcast` (`__concurrence_lock_error` / `_broadcast_error` in the
emulator log). Without the option the guards work but `std::thread` throws. WoWee is full of Meyers singletons (`Logger::getInstance` is the
first), so a small lock-free `__cxa_guard_acquire/release/abort` (one CAS on the guard word itself, `sceKernelDelayThread` while waiting) is linked
into the executable and wins over libstdc++. Verified on Vita3K with 3 threads racing a slow initialiser: constructor ran once. **Any other Vita
executable that uses function-local statics and `std::thread` needs the same file.** Not seen on hardware yet (VITA-34).
**The guard is 32 bits on this target (VITA-42).** The compiler's guard variable on ARM EABI is 4 bytes (`_ZGV...` is `.size 4`, `sizeof(__cxxabiv1::__guard)` is 4),
not the 8 of the generic Itanium ABI. The first version of `vita_cxa_guard.cpp` assumed 8 and kept the owner thread id in a second word, which was the
*neighbouring variable*: it wrote a thread id into it, and waited forever if that word was non-zero. Which variable sat next to a guard depends on the
section layout, so it showed up as the logger hanging when an executable was built without `-ffunction-sections`/`--gc-sections`. The state (initialised,
busy, owner's low 16 thread-id bits) now lives in the one word, and a `static_assert` fails the build if the guard size ever differs. The threadcheck
probe has a `guard_race` check (`guard_race.inits=1 values_ok=3`).

Test (Vita3K): put an `env.txt` in `<pref-path>/ux0/data/wowee/` (for example `WOW_DATA_PATH=uma0:data/wowee/Data`, `WOWEE_LOG_UDP=127.0.0.1:9999`), then
`tools/vita/vita3k_macos.sh build-vita/wowee.vpk WOWE00001 --seconds 12`; the app log shows the version line, heap and clocks, the env.txt keys and the
resolved data and config roots. `vita3k_macos.sh` deletes `*.log` in that directory before a run but leaves `env.txt`, so delete your test `env.txt` afterwards.

Not built yet, so no Vita arms yet: `core/open_url.cpp` (shells out to `xdg-open`), `https_get.cpp` / `update_check.cpp`, `screen_recorder.cpp`,
`platform/process.hpp` (POSIX `kill`/`waitpid`) and `memory_monitor.cpp` (`<sys/sysinfo.h>`, VITA-23). They are outside the Vita source list, which
is how they are "compiled out"; give each its arm in the item that adds it to the list.

## 13. Threads: budget, cores, priorities, stacks (VITA-8) *(probe verified on Vita3K and on a real Vita Slim; the client-level layout is VITA-34)*

`include/core/thread_budget.hpp` (platform-neutral; off the Vita it returns what the caller computed and does nothing) and
`src/platform/vita/vita_threads.cpp`. Two calls: `core::platformWorkerCount(role, desktopValue)` for how many threads (or tasks in flight) a
role gets, and `core::enterThread(role)`, the first line of a thread body, which pins the thread to its cores and sets its priority.

| Role | Count | Cores (`SCE_KERNEL_CPU_MASK_USER_*`) | Priority (lower runs first) |
|---|---|---|---|
| main | 1 | 0 | 160 (system default) |
| network pump | 1 | 1 | 112 |
| frame workers (`ThreadPool::frameWorkers`) | 1 | 1+2 | 176 |
| I/O workers (`ioWorkers`) | 1 | 1+2 | 184 |
| terrain workers | 1 | 2 | 184 |
| world preload | 1 | 2 | 188 |
| update check | 1 | 2 | 188 |
| watchdog | **0** (no thread: it only releases a desktop mouse grab) | - | - |
| async creature / game object / equipment loads (in flight) | 1 each | 2 | 184 / 184 / 186 |

The NPC composite pre-decode (`entity_spawner.cpp`, one launch per new humanoid display id, no in-flight limit of its own) goes through `core::launchBackground` (`thread_pool.hpp`): `std::async` on the desktop, the single I/O worker on the Vita. Not capped: the login backdrop decode (one launch), Warden (dropped on the Vita) and the detached normal-map threads in `character_renderer` (Vulkan renderer).

No role ever gets the system core (core 3, `SCE_KERNEL_CPU_MASK_SYSTEM`). `WOWEE_TERRAIN_WORKERS` is not overridden on the Vita: it is
applied inside `computeTerrainWorkerCount` before the budget is asked, and the budget still caps it at 1, so an env.txt value has no effect
(`m2_renderer`, `wmo_renderer` and `character_renderer` keep their own `hardware_concurrency` formulas, because the Vulkan renderer does not
build on the Vita and `rendering/` is left alone). The priorities are a first guess and need tuning on hardware.

**Stacks.** pthread-embedded gives every `std::thread` a **32 KB stack** and libstdc++ never asks for more (measured, Vita3K). The ADT / M2 /
WMO / BLP parsers run on worker threads, so `vita_threads.cpp` defines `__wrap_pthread_create` and the link has `-Wl,--wrap=pthread_create`
(`cmake/vita/Vita.cmake`): any stack below `WOWEE_VITA_THREAD_STACK_KB` (default 512, a compile definition) is raised to it, a bigger request is
left alone. **Every Vita executable that starts threads needs this link option** (like `-Wl,-u,pthread_cancel` and `vita_cxa_guard.cpp`).
Cost: 512 KB per thread of app memory, about 4 MB for the layout above. `std::async` tasks get it too.

**Probe: `tools/vita/threadcheck/`** (own CMake project, links the real `vita_threads.cpp` and the platform layer). Build and run:
`SRC=tools/vita/threadcheck BUILD=build-vita/threadcheck tools/vita/build.sh`, then
`tools/vita/vita3k_macos.sh build-vita/threadcheck/threadcheck.vpk THRC00001 --seconds 14`; results are in `build-vita/logs/THRC00001.app.log`
(`threadcheck.log`: raw kernel values per thread and per role) and the `Vita thread:` lines in `wowee.log`. A file
`ux0:data/wowee/threadcheck.recurse` in the Vita3K data dir adds the stack-exhaustion test (ends in a crash on purpose; the last
`recurse.depth_kb` line is how deep it got). Delete `vita3k*.log` after each run.

Measured on Vita3K (`[v3k]`; **the same on hardware is VITA-34**):

| Fact | Value |
|---|---|
| `std::thread::hardware_concurrency()` | 0 |
| Default stack: main thread / `std::thread` / raw pthread with no attr | 256 KB reported (the app sets 4 MB through `sceUserMainThreadStackSize`, the emulator still reports 256 KB) / **32 KB** / 32 KB |
| With the wrap: `std::thread` stack, and recursion in one | 512 KB; 1 KB frames reached depth 512, then the process ended |
| Default priority: main / new `std::thread` | 160 / the creator's priority (159 to 191 seen) |
| `sceKernelChangeThreadCpuAffinityMask` with one user core, two cores, `USER_ALL` | succeeds, read back exactly (`0x10000`, `0x20000`, `0x40000`, `0x70000`) |
| Same call with `SCE_KERNEL_CPU_MASK_SYSTEM` (core 3) | **refused**, `0x80028025`, mask unchanged |
| A new thread's initial affinity | 0 (= no restriction, `cpu_now` reads `0x70000`) |
| `sceKernelChangeThreadPriority` from inside the thread | succeeds (96, 112, 176 ... read back) |
| `pthread_attr_getstacksize` of a fresh attr | 0 (so "below the wanted size" must include 0) |

Vita3K does not schedule on real cores, so **only the API results are meaningful there, not that the threads run where they were told**.

**Measured on a real Vita Slim (`[hw]`, 2026-10-02, threadcheck run over vitacompanion, VPK installed by hand in VitaShell; logs in `build-vita/logs-hw/`).** Same as Vita3K, except where noted:

| Fact | Result |
|---|---|
| `hardware_concurrency()` | **0** |
| `std::thread` stack with the wrapper | 512 KB; recursion with 1 KB frames reached depth 480, then the process crashed at the limit (so the usable stack matches). The unwrapped 32 KB default was **not** re-measured on hardware (the probe is always built with the wrapper) |
| Affinity: one core, two cores, `USER_ALL`; priority 0x60 and back | all succeed and read back |
| `SCE_KERNEL_CPU_MASK_SYSTEM` (core 3) | **refused**, `0x80028025`, mask unchanged |
| Every role in the table above | cores, priority and 512 KB stack read back exactly as planned; the watchdog count is 0 |
| Main thread at start | priority 160, affinity **`0x70000`** (Vita3K: 0) |
| **Main thread stack** | **256 KB, not the 4 MB asked for**, until `-Wl,-u,sceUserMainThreadStackSize` was added: nothing references the symbol, so `--gc-sections` removed it from the ELF. Recursion on the main thread crashed at about 256 KB. With the option: recursion reached depth 4000 KB (1 KB frames) before the limit, and the info call reports 4096 KB. **Every Vita executable that sets the stack needs that option** (`Vita.cmake`, threadcheck) |
| **Core placement** (busy threads, throughput vs one core) | one thread 100%; two on the **same** core 100%; two on cores 1 and 2 **201%**; three on cores 0-2 **303%**; four unrestricted **303%**, with and without an affinity call. So pinning works, threads on different cores run in parallel, and the app never gets a fourth core |
| New thread's initial priority | the creator's (159 in the probe), affinity 0 |

Still open on hardware: the priorities under real load (they are guesses) and the 30-minute session without unbounded thread creation. Both need the running client (VITA-9,
VITA-10) and stay tasks on VITA-34. (`cpu_now` in the probe is the affinity mask read back, not the core in use; the placement rows above are what show where threads run. A busy thread at priority 159 starves a main thread at 160 on the same core: the placement test raises main's priority first, a hang found on the first hardware run.) The deliberate-crash run leaves a `psp2core-*.psp2dmp` in `ux0:data/`; delete it when done.

**Resolved (VITA-42): the logger hang without `-ffunction-sections -fdata-sections -Wl,--gc-sections` was the guard bug above, not the flags.** An executable built
without them stopped at its first `LOG_*` call (right after "writing the log to ..."); gdb (`gdb_at_pc.sh` with a breakpoint on `sceKernelDelayThread`)
showed it waiting in `__cxa_guard_acquire` for the `static const std::string home` guard in `Logger::emitLineLocked`. With the 32-bit guard the threadcheck
probe logs with the flags dropped (`-DTHREADCHECK_SECTIONS=`) as well. `Vita.cmake` keeps the flags for size; they are no longer needed for correctness. Still needed
for correctness: `-Wl,-u,sceUserMainThreadStackSize` (else `--gc-sections` drops the stack-size symbol). The hang is Vita3K only so far; hardware check is on VITA-34.

## 14. wowee_core: the renderer-free core (VITA-9 audit, VITA-47) *(builds and links on desktop and for the Vita; `wowee_headless`, section 15, is the first program that runs it)*

`wowee_core` is the part of the client that needs no renderer, window or UI toolkit: `auth`, `network`, `math`, `pipeline`, `game`,
`audio` (managers), the clean part of `core/` (`logger`, `config_paths`, `memory_monitor`, `app_clock`) and three files that live in
`rendering/` and `ui/` but need only std/glm/logger (`animation_ids`, `emote_registry`, `framexml_takeover`). `docs/vita/HEADLESS_AUDIT.md`
says why and how each seam was cut. `src/addons/` is deliberately not in it (VITA-49).

**How it is defined.** `cmake/wowee_core.cmake` (fork-only) globs those directories, so the 450-file `WOWEE_SOURCES` list in the root
`CMakeLists.txt` is untouched and a file upstream adds under one of them joins the core automatically (and fails the link check if it
reaches the renderer). It makes `wowee_core_objects`, `wowee_core` (static) and `wowee_core_link_check`, an executable that links
**every** object, so an undefined reference is a build error. Desktop: off by default, `-DWOWEE_BUILD_CORE=ON` (the client does not use it);
Vita: always built by `cmake/vita/Vita.cmake`, which also holds `wowee_vita_executable()`, the one place that lists the Vita link
libraries and options (`pthread`, `-u pthread_cancel`, `--wrap=pthread_create`, `-u sceUserMainThreadStackSize`, the platform sources).

**What replaces what.** Definitions the core needs that live in files it does not have:
`src/platform/headless/stb_image_impl.cpp` (stb_image's implementation, which the desktop keeps in `rendering/loading_screen.cpp`),
`src/platform/headless/cvar_defaults.cpp` (`addons::storedCVarValue`: no store, every setting is its default; **drop it when `addons/`
joins the core**, both define the function). Option `WOWEE_CORE_NULL_AUDIO` (always ON on the Vita) builds
`src/platform/vita/audio_engine_null.cpp` instead of `audio_engine.cpp` (VITA-46). On the Vita, Warden is `src/platform/vita/warden_stub.cpp`
instead of `warden_module.cpp` + `warden_emulator.cpp` (OpenSSL 3 and unicorn are not available): `load()` refuses, which the handler already
treats as "no module". `memory_monitor.cpp` has a Vita arm (its "total RAM" is the newlib heap; the real budget is VITA-23).

**Checks.**
- Desktop: `tools/vita/core_check.sh` (and `NULL_AUDIO=1 tools/vita/core_check.sh`) in the desktop-builder container: builds and links
  `wowee_core_link_check`, then reads ninja's dependency records and fails if any core object reaches a Vulkan, SDL, ImGui, VMA or Lua
  header (253 objects, none does). Own build directory (`build-core`, `build-core-null`; add to `.git/info/exclude`).
- Vita: `JOBS=5 tools/vita/build.sh` builds everything including `wowee_core_link_check` (an ELF, not in the VPK) with the Vita compiler,
  where there are no Vulkan headers at all, which is stronger than `sweep.sh`. About 30 minutes cold. **`build.sh` now gives the container
  10 GB and 8 CPUs** (Apple's default 1 GB thrashed and never finished); eight parallel `-O2 -g` compiles of the game sources were killed for
  memory even then, hence `JOBS=5`.

## 15. wowee_headless: the console client (VITA-48) *(desktop: verified against a LAN AzerothCore; Vita build is PR 3, verified on Vita3K up to the network)*

`wowee_headless` (`tools/headless/`, fork-only) is a console program that uses only `wowee_core`. It does by hand what `Application` and the
login, realm and character screens do: sync and load the expansion tables (`syncClientTables`, `ExpansionRegistry`, opcode and update-field
tables, packet parsers, DBC layouts), authenticate (`AuthHandler`, `ClientInfo` filled as `AuthScreen::beginAuthAttempt` does), connect to the
chosen realm's world server and wait for the character list, which the world server requests by itself after world auth. PR 1 stops there;
PR 2 enters the world as character N and logs chat and entities until a run limit; the Vita build is PR 3 (below). No `AssetManager` is built (`services.assetManager` is null).

**Build and run (desktop, in the container; the host has no toolchain).**

```sh
# configure + build, same flags as core_check.sh (build-core is git-ignored locally)
container run --rm --memory 10G --cpus 8 --entrypoint /bin/bash -v "$PWD:/workspace" -w /workspace wowee-desktop-builder -c \
  'cmake -S . -B build-core -G Ninja -DCMAKE_BUILD_TYPE=Release -DWOWEE_BUILD_TESTS=ON -DWOWEE_BUILD_CORE=ON && cmake --build build-core --target wowee_headless'
# run from the repo root so ./Data/expansions (the client's own tables) is found
container run --rm --env-file ~/wowee-headless.env --entrypoint /bin/bash -v "$PWD:/workspace" -w /workspace wowee-desktop-builder -c build-core/bin/wowee_headless
```

**Settings** are environment variables only (no command line: the Vita has only `env.txt`, and `ps` shows arguments). Keep them in a file
outside the repo; there is no default server.

| Variable | Meaning |
|---|---|
| `WOWEE_HEADLESS_HOST`, `_PORT` | auth server (port default 3724) |
| `WOWEE_HEADLESS_ACCOUNT`, `_PASSWORD` | credentials (never logged, never committed) |
| `WOWEE_HEADLESS_EXPANSION` | profile id: `classic`, `tbc`, `wotlk`, `turtle`, ... |
| `WOWEE_HEADLESS_REALM` | realm name (default: first in the list) |
| `WOWEE_HEADLESS_CHARACTER` | index in the character list to enter as (default 0) |
| `WOWEE_HEADLESS_SECONDS` | how long to stay in the world, logging, before a clean disconnect (default 20) |
| `WOWEE_HEADLESS_TIMEOUT` | seconds each phase may take (default 30) |
| `WOW_DATA_PATH` | data root holding `expansions/<id>/` (default `./Data`) |
| `WOWEE_REALM_HOST_OVERRIDE` | replaces the host in the realm list (AzerothCore advertises `127.0.0.1`, which inside a container is the container) |

**Exit codes**: 0 ok, 2 missing/bad option, 3 tables (no `expansions/`, unknown expansion id, a table or parser would not load), 4 TCP connect
failed (auth or world), 5 login refused / PIN or authenticator needed / bad session key, 6 world login failed or realm not found, 7 a phase
timed out. Each prints an `[ERROR]` line before the code.

**Verified (desktop container, 2026-10-03):** closed port (`127.0.0.1:1`) exit 4 "Connection refused"; unknown expansion `nope` exit 3;
`WOW_DATA_PATH` of an empty directory exit 3; no options exit 2. Live (LAN AzerothCore 3.3.5a, `wotlk`, 2026-10-03): auth with protocol 8, realm list, world login and character list all work (exit 0; the
account had no characters). The auth handler does not ask for the realm list by itself after login (the realm screen does), so the driver
sends `requestRealmList()` once the state is `AUTHENTICATED`. With no `AssetManager` (`services.assetManager` null) the handler ran
through world auth and the character list without a crash. Warden: the server's module is refused on Linux ("Cannot execute Windows x86
code"), the server did not drop the connection in the few seconds this ran. `WOWEE_LOG_LEVEL=debug` shows the packet-level log.
`WOWEE_REALM_HOST_OVERRIDE` was not needed here (the realm advertises the LAN address).

**World entry and log (PR 2, LAN AzerothCore, 2026-10-03):** `setActiveCharacterGuid` + `selectCharacter`, then `IN_WORLD` arrives by itself (no
world-entry callback). The driver then prints, at WARNING level so the default log level shows them: `[chat <type>] sender: text` for each new
`getChatHistory()` entry, `[entity +] type N guid G "name" at x,y,z` when an entity appears in `getEntityManager()` (checked every 0.5 s),
`[entity =]` when its name arrives later, `[entity -]` when it leaves. A 25 s run as a level-1 character in Northshire logged about 90 creatures
(wolves, guards, rabbits) and exited 0 with both sockets disconnected. Without an `AssetManager` the world handlers ran without a crash; one
warning ("Quest login resync timed out") is the only anomaly. A character index past the list exits 2.

**Vita build (PR 3).** `JOBS=5 tools/vita/build.sh` now also produces `build-vita/wowee_headless.vpk` (title ID `WOWH00001`, `-DWOWEE_VITA_HEADLESS_TITLEID`), next
to `wowee.vpk`. The target is in `cmake/vita/Vita.cmake` (links `wowee_core` + `ssl crypto z pthread`, `wowee_vita_executable()`); the same
`tools/headless/main.cpp` runs, with a `__vita__` arm that calls `platform::vita::initProcess()` first. There is no command line: put the
`WOWEE_HEADLESS_*` settings in `ux0:data/wowee/env.txt` (`KEY=VALUE`, see section 12). The VPK carries the client's own tables (about 300 KB,
`Data/expansions/<id>/*.json`) under `app0:Data`, and on the Vita the driver reads them from there (`installRoot = dataRoot = app0:Data`) because
`syncClientTables` only updates expansions a data root already holds. The log is `ux0:data/wowee/*.log` (or UDP, `WOWEE_LOG_UDP`); the `[chat]` and
`[entity]` lines are log lines, there is no console. **Trap:** `vita_create_vpk` is a macro that keeps its `-a` file list and title ID in variables
across calls, so a second VPK in one project inherits the first one's files (a duplicate `icon0.png` error); `Vita.cmake` unsets them between calls.

Vita3K smoke test (2026-10-03): `tools/vita/vita3k_macos.sh build-vita/wowee_headless.vpk WOWH00001 --seconds 45` with a test `env.txt` in
`<pref-path>/ux0/data/wowee/` (delete it afterwards; it holds credentials). **Verified against the LAN AzerothCore:** it finds the four
expansions in `app0:Data`, loads the wotlk tables (1306 opcodes, 79 update fields, 40 DBC layouts), authenticates, receives the realm list, passes world
auth, lists the character, enters the world as `Vitatester`, logs the MOTD and channel joins and about 95 creatures, and disconnects cleanly after
`WOWEE_HEADLESS_SECONDS`. Warden's module load fails (the Vita stub refuses by design) and the server did not drop the connection. **Trap:** the first run
failed with `Host is unreachable` (and `No data` for 127.0.0.1): macOS asks Vita3K.app for "Local Network" permission on the first LAN connection,
and until it is granted (System Settings, Privacy & Security, Local Network) the emulator cannot reach any LAN host. Vita3K proves the code path,
not Wi-Fi, speed or memory: the real-device run is a VITA-34 task.

## 16. Networking on the Vita (VITA-7) *(Vita3K and a real Vita Slim, quiet zone; the busy-capital run is VITA-50)*

`src/network` runs on the Vita through `net_platform.hpp`'s POSIX branch: newlib maps BSD sockets to sceNet after `initProcess()` (section 12) has called
`sceNetInit`. `tools/vita/depcheck` (section `sockets`, needs `python3 tools/vita/depcheck/echo_server.py` on the Mac; `depcheck_host.txt` in
`ux0:data/wowee` names its LAN IP, and Vita3K.app needs macOS "Local Network" permission, section 15) measured what that branch relies on:

| Call | Vita3K result | `net_platform.hpp` expects |
|---|---|---|
| non-blocking `connect` | -1, `errno` 119 | `EINPROGRESS` (newlib 119) ok |
| non-blocking `recv`, nothing to read | -1, `errno` 11 | `EAGAIN`/`EWOULDBLOCK` (11) ok |
| `recv` after the peer closed | 0 | 0 means closed ok |
| `send` after the peer closed | succeeds twice (no `EPIPE`) | the emulator's host stack; real device unknown, VITA-34 |
| `getsockopt(SO_ERROR)`, refused | **61** (newlib `ECONNREFUSED` is 111) | the code only tests `!= 0`; the message used `strerror`, which named the wrong error |
| `sceNetCtlInetGetState` | 3 = connected | |

So `errno` values are newlib's and need no arm; only `SO_ERROR` is in sceNet numbers. Two small gated additions in `net_platform.hpp`
(`include/platform/vita/net_state.hpp` holds the logic): `net::socketErrorString()` (the two `SO_ERROR` sites use it; elsewhere it is `errorString`;
on the Vita 61 reads "connection refused", the other BSD numbers 50/51/54/60/64/65 are mapped by convention and unverified) and a Wi-Fi check at the top of
`openResolvedSocket` that fails with "Wi-Fi is not connected (network state N)" instead of a bare connect error. A failed state query does not block
the connection. Not testable on Vita3K: a disconnected state. Still open (hardware): the async pump at 444 MHz with `WOWEE_NET_ASYNC_PUMP` 1 vs 0 (needs
a busy area), DNS (`getaddrinfo("localhost")` works, a real name is untested), `EPIPE`, and the 30-minute session.

**On the real Vita (2026-10-03, Slim, Wi-Fi to a LAN AzerothCore 3.3.5a, `wowee_headless` as the load).**
- DepCheck's socket section gives the same results as Vita3K (table above); `send` after a peer close succeeds with no `EPIPE` there too; `SO_ERROR` for a refused connection is 61.
- Login, realm list, world entry, chat and entity log work (about 90 creatures around a level-1 character in Northshire). The network pump thread runs on core 1 at priority 112 as VITA-8 laid out.
- **Quiet-zone session, 30 minutes (13:42 to 14:12), no disconnect, no crash:** heap in use 415 to 417 KB (`mallinfo`; flat, no leak seen, but only what newlib's malloc reports), arena 448 to 456 KB, game update 0.14 ms average with a few 6 to 16 ms outliers, CPU 2.0 % main + 2.4 % network pump = 4.4 % of one core, constant over the 29 one-minute windows. The slowest packet handlers were 17 to 46 ms (`SMSG_WARDEN_DATA`, `SMSG_AUTH_RESPONSE`, `SMSG_LOGIN_VERIFY_WORLD`). `WOWEE_NET_ASYNC_PUMP=0` (90 s): 2.2 % on the main thread and no pump thread, so in a quiet area the async thread roughly doubles the CPU. A busy area is not measured yet (VITA-50).
- **Traps found on hardware.** (1) `sceKernelGetThreadRunStatus` **data-aborts** (crash dump, PC in `SceLibKernel`) even with valid arguments: do not call it. Per-thread CPU comes from `SceKernelThreadInfo::runClocks` (`tools/headless/run_stats.hpp`, thread ids from `platform::vita::registeredThreads()`). (2) A program that takes no input is suspended by the system after about 90 seconds (the screen turns off): call `sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT)` about once a second (`RunStats::sample()` does). (3) The file log is buffered and its tail is lost when the app dies: for a long or crashing run use the UDP sink (`WOWEE_LOG_UDP=<mac ip>:9999` in `env.txt`, `tools/vita/logsink.sh`; a datagram has no newline, split on the timestamp). (4) The working directory at launch is the read-only `app0:`: the headless `main()` changes to `ux0:data/wowee` so `./warden_cache` can be created. (5) `tools/vita/parse_core.sh` now works with Python 3 (it patched one line of the pinned tool); the dump's "disassembly at PC" is wrong when PC is in a system module, read the registers and the LR section instead.
- How the hardware runs were done: the user installs the VPK once in VitaShell; `eboot.bin` is then replaced over FTP (`ux0:app/<TITLEID>/eboot.bin`), `env.txt` goes to `ux0:data/wowee/`, `destroy` then `launch <TITLEID>` over port 1338 starts it. Open: DNS for a real hostname (numeric IPs only so far).

## 17. Collision split: files, not classes (VITA-51, ADR-001 decision 4) *(WMO and M2 done)*

The CPU collision queries of the renderers live in files of their own, moved unchanged from the Vulkan renderer's `.cpp`: `src/rendering/wmo_renderer_collision.cpp`
(22 functions, 1,357 lines: floor height, wall sweep, containment, raycast, the floor cache, the per-group triangle grid, the instance spatial index, `findContainingGroup`).
The M2 renderer's are in `src/rendering/m2_renderer_collision.cpp` (10 `M2Renderer` functions that were in `m2_renderer_instance.cpp`, plus the per-model `CollisionMesh` methods that were in `m2_renderer.cpp`, and the thread-local scratch buffers). They never touch the GPU, so the Vita compiles the same files against its own declarations of `WMORenderer` and `M2Renderer` (VITA-52). Two helpers had to leave shared homes to make that true:
`QueryTimer` (was in `vk_frame_data.hpp`, now `include/rendering/query_timer.hpp`) and the ray helpers (`include/rendering/wmo_ray_helpers.hpp`, `static` became `inline`, used by the
collision file and by the debug dump left in `wmo_renderer.cpp`). `debugDumpGroupsAtPosition` and `gatherLavaLights` stay: they read GPU-side members.

**Checks (each takes seconds, except the desktop build).**
- `tools/vita/check_move.py <rev>:<old file> <new files...> [--allow ...] [--allow-lost ...]`: proves a refactor only moved code. Compares the code lines (and `///` doc comments) that left the old file with those that
  appeared in the new ones; any other difference is listed and fails. Several `--old` files are summed (the M2 move came out of two files). Example for the WMO split: `tools/vita/check_move.py origin/vita:src/rendering/wmo_renderer.cpp src/rendering/wmo_renderer.cpp src/rendering/wmo_renderer_collision.cpp include/rendering/wmo_ray_helpers.hpp --allow ...`
  (the allowed differences are the new files' namespace lines and the two `static` to `inline` lines). The result for this split: PURE MOVE.
- `tools/vita/collision_check.sh` (WMO and M2): compiles each `*_collision.cpp` with the VitaSDK compiler, no Vulkan headers on the path, against a copy of its header that `tools/vita/collision_check/make_shadow.py` strips of every Vulkan
  declaration (so the copy cannot drift from upstream's header; a dropped `struct X {` takes its body with it, and each header lists what else to drop, such as `ShadowParamsSet` and `ParticleGroupKey`, in the script). If a collision file ever names a GPU member or a Vulkan type, this fails. It is a stand-in for the real shadow headers of VITA-52 and the first thing to re-run after an upstream sync.
- `tools/vita/desktop_check.sh all` (220/220) and `tools/vita/core_check.sh`.

**Trap:** the leading comment of a function is part of it. A first split left a doc comment and a constant (`kWMOGroupIndoor`) behind because upstream had glued them between a function and its comment; `check_move.py` and the Vita compile found it. New files must be added to **both** source lists in the root `CMakeLists.txt` (the client and the editor tool).

## 18. Shadow headers: the Vita's view of the renderer (VITA-52, ADR-001) *(mechanism and checks done; `src/core` and `src/ui` still wait for VITA-12)*

The 28 sources outside `rendering/` that talk to the renderer (and the headers they include) keep `#include "rendering/renderer.hpp"`, and so on. The Vita build puts `cmake/vita/shadow/` **first** on the include path
(`wowee_vita_shadow` in `cmake/vita/Vita.cmake`, an INTERFACE target to link before anything else), so those includes resolve to the Vita's copies: upstream's own header with every declaration that mentions Vulkan deleted
and everything else exactly as written. 22 headers (`renderer`, `character_renderer`, `wmo_renderer`, `m2_renderer`, `minimap`, `post_process_pipeline`, `sky_system`, `terrain_renderer`, ...) plus `vk_context.hpp`, which is **poisoned**:
including it in a Vita build stops with `#error ... (VITA-12)`, naming the item that removes the dependency instead of failing on a missing `vulkan.h`.

**The directory is `cmake/vita/shadow/`, not under `include/`, on purpose: upstream's own sweeps (`duplicate_block_check.py`, `unused_member_check.py`, run by the `sweep_guard` test) scan `include/` and `src/` and flagged the generated copies as duplicated code.** **The copies are generated, never edited:** `tools/vita/gen_shadow.py` writes them (the table `HEADERS` in the script lists, per header, the GPU-only types that carry no Vulkan token and must be dropped too, such as `GPUPerFrameData` or `ParticleGroupKey`);
`tools/vita/gen_shadow.py --check` fails if a committed copy differs from what upstream's header produces now. **After every upstream sync: run the check, regenerate, commit.** A hand-edited copy would drift from upstream silently.
When a source needs a method that is missing, the Vita compile names it; the fix is the Vita implementation (VITA-13/18/19/20 in `src/rendering/gl/`), not the copy.

**`tools/vita/shadow_check.sh`** (about 3 minutes; Vita compiler, no Vulkan headers anywhere): (1) copies match upstream, (2) `renderer.hpp` resolves into the shadow directory and reaches no Vulkan header, `vk_context.hpp` is poisoned, (3) a sweep of `src/core` and `src/ui`
(`SHADOW=1 OUT=build-vita/sweep-shadow tools/vita/depcheck/sweep.sh core ui`) where a source may fail **only** for a reason VITA-12 removes; anything else fails the script. It writes the worklist to `build-vita/sweep-shadow/vita12_worklist.txt`.

**Where it stands (VITA-12 done 2026-10-03):** **27 of 28 files in `src/core` and 63 of 64 in `src/ui` parse** with the Vita compiler against the shadow headers and no Vulkan header anywhere; the two that do not are
`src/core/window.cpp` (the desktop window, which owns the Vulkan surface; the Vita has its own implementation of `core::Window`) and `src/ui/map_window.cpp` (a second OS window with its own swapchain, desktop only). `tools/vita/shadow_check.sh` expects exactly those two
and fails on anything else; `tools/vita/vulkan_leak_check.sh` (the item's own grep, with the same two files and two header/comment cases allowlisted with reasons) reads **0 files**. How the interface reaches the renderer now, all Vulkan-free (VITA-12, `include/rendering/`):
`ui_texture.hpp` (`UiTexture`, a strong 64-bit id for a texture ImGui can draw, and `IUiTextureService`: upload, generation, upload batches; `core::Window::getUiTextureService()`), `imgui_backend.hpp` (`IImGuiBackend`: init, new frame, shutdown;
`core::Window::getImGuiBackend()`), `gpu_texture.hpp` (`GpuTexture`, an alias of `VkTexture` on the Vulkan renderer), and plain methods on `Renderer` and `Window` for what the shell did through the Vulkan context (`isDeviceLost`, `waitIdle`, `setMsaaSamples(int)`,
`getMaxMsaaSamples`, `beginUploadBatch`, `releaseSurface`, `restoreSurface`, `markSwapchainDirty`, `setAnisotropyLimit`, ...). The Vulkan implementations are `vk_ui_texture_service.hpp/.cpp`, `vk_imgui_backend.hpp/.cpp` (with `toUiTexture`/`toDescriptorSet`, round-trip tested in `tests/test_ui_texture.cpp`).
The Vita implements the same interfaces over vitaGL (VITA-13 and on). Two traps found on the way, both in `gen_shadow.py`'s stripper and fixed: a Vulkan word in a block comment must not delete the declaration after it, and GPU-only types (`AllocatedImage`) have to be listed per header.

## 19. GlProbe: vitaGL, DXT, FBO scaling and ImGui (VITA-13) *(measured on a real Vita Slim; the emulator cannot check pixels; raw logs in `docs/vita/measurements/`)*

`tools/vita/glprobe/` is a small app (title `GLPR00001`) that answers the vitaGL questions ADR-001 left open with `PASS`/`FAIL` lines in `ux0:data/wowee/glprobe.log` (and `sceClibPrintf`, which Vita3K copies into its own log).
It uses only the fixed-function pipeline and reads pixels back with `glReadPixels`, so it needs no screenshot. `ux0:data/wowee/glprobe.mask` (hex: 1 clear, 2 DXT, 4 FBO, 8 ImGui, 16 frame loop, 32 DXT3/DXT5 variants) runs a subset; `glprobe.init` (0 to 3) picks the init call.
`SRC=tools/vita/glprobe BUILD=build-vita/glprobe tools/vita/build.sh`. Raw device log: `docs/vita/measurements/glprobe_device_2026-10-03.log`.

**Results on the real Vita (2026-10-03, ADR-001 questions in bold):**
- **vitaGL initialises** (`vglInitExtended(0, 960, 544, 24 MB threshold)`); `GL_RENDERER` `SGX543MP4+`, `GL_VERSION` `OpenGL ES 2.0 VitaGL`; `GL_EXT_texture_compression_s3tc` is advertised. Pools after init: CDRAM 96 MB total (about 81 MB free), RAM pool 88 MB (about 51 MB free), PHYCONT 26 MB (free), newlib heap 128 MB.
- **DXT textures upload compressed and sample correctly: DXT1, DXT3 and DXT5** *(CORRECTION 2026-10-04, VITA-14: true only by luck of upload history. The SDK vitaGL copies each compressed level with an asynchronous hardware transfer from a temporary buffer the next upload can reuse, so a DXT texture can sample as zero, and a back-to-back batch of uploads can even hang; section 22. Every statement below that DXT works holds only with the patched vitaGL, and the "DXT5 after mipped DXT1" quirk of this section is this bug, not stale sampler state.)*, 4x4 to 64x64, with `GL_CLAMP_TO_EDGE`, `GL_REPEAT` and `GL_MIRRORED_REPEAT`, every block read back exactly. **A DXT1 second mip level uploads and is selected when the texture is minified** (so mip skipping and mip chains from BLP work the standard way).
- **One quirk, deterministic:** a DXT5 texture created right after a *mipmapped* DXT1 texture was drawn samples as (0,0,0,0) (the first test order, mask 2); every DXT5 case alone passes (mask 32). Looks like stale per-texture-unit state in vitaGL (the `use_mips` flag); not a format problem. Treat "bind a non-mipmapped texture after a mipmapped one" as suspect and re-test with the real renderer.
- **The 640x368 scene in an FBO scaled to 960x544 works** (FBO complete, content correct before and after the scale), the base for the ADR's resolution plan. **ImGui 1.92.6 draws through a 60-line fixed-function renderer** (font atlas, window body, text pixels); "no shader" was a claim in an earlier version of this section and is **wrong**: vitaGL's fixed-function path compiles a generated shader at run time for every new state combination (below). The stock `imgui_impl_opengl2.cpp` does not compile against vitaGL's headers (`glOrtho`, `glPushAttrib`, `glGetTexEnviv`, `GL_TEXTURE_BINDING_2D` missing).
- A frame loop of a trivial scene runs at **16.66 ms per frame: vsync-locked 60 fps**; says nothing about a real scene (VITA-18 and on measure that).

**Draw-call cost and the shader path (real Vita, ARM 444 / GPU 222 / bus 222 MHz, 2026-10-03; `docs/vita/measurements/glprobe_device_2026-10-03_bench_shaders.log`).**
- **Draw calls, fixed-function path, CPU time to issue (mask 64):** a plain `glDrawArrays` costs about **2.2 microseconds**; with a `glBindTexture` between two textures each draw **3.0**; with a texture switch and a `glColor4f` **4.0**; re-pointing `glVertexPointer`/`glTexCoordPointer` per draw **2.6**; the same quads in **one batched draw 0.3 per quad** (7x cheaper). It scales linearly (100 to 2000 draws: 0.2 to 4.3 ms for the plain case, 5.9 ms with a texture switch). Tiny quads stayed vsync-locked (16.68 ms), so this is CPU submission cost only, with no fill-rate cost, and with the fixed-function shaders already cached (the first draw of each new state combination costs a compile, see the end of this section).
  Budget: 1000 draws with a texture switch is about 3 ms of render-thread time, 2000 about 6 ms; at 30 fps (33 ms) that leaves most of the frame, but the programmable path costs more per draw (uniform uploads, not yet measured): **batch by material and cull before drawing**, as the ADR says.
- **Shader path (mask 128): GLSL ES 1.00 compiles at run time and works.** A representative terrain shader (two texture layers blended by an alpha map, directional light, fog) translates, links and draws the right colour; an M2-style shader with `discard` too; **the stock `imgui_impl_opengl3` (GLES2 profile) runs** with one shim: **vitaGL has no `glDetachShader`**, the backend calls it, so the app defines an empty one (`extern "C" void glDetachShader(GLuint, GLuint) {}` in `glprobe/main.cpp`).
- **The cost is the compile, at link time:** about **330 to 365 ms per program** with the SDK's vitaGL (vertex and fragment compile calls take 0 ms; `glLinkProgram` does the work), and compiling the same source again is **not** faster there (the SDK build has no shader cache). With a vitaGL built from source with `HAVE_SHADER_CACHE=1` the hit is **165 to 182 ms**, only about **twice as fast**. ImGui's first frame (its own two shaders) takes 323 ms uncached, 162 ms cached. **So 14 shaders are about 5 s of start-up uncached and 2.5 s cached, and every material permutation costs the same again**: the ADR's run-time-compile strategy needs either precompiled shaders (`glShaderBinary` with GXP binaries, untested; `vglGetShaderBinary` can dump what the translator made) or lazy compilation behind a loading screen, and a hard cap on permutations (VITA-14).
- **PRECOMPILED SHADERS ARE THE ANSWER TO THE COMPILE COST (measured, mask 256).** Compile the program once, dump both shaders with `vglGetShaderBinary(shader, bufSize, &length, buffer)` (the terrain vertex shader is **448 bytes**, the fragment shader **520**), and later load them with `glShaderBinary(1, &shader, 0, data, length)` into fresh shader objects, attach, `glLinkProgram`:
  **`glShaderBinary` x2 0.02 ms, link 0.03 ms**, against 353 ms to compile the same source (about 10,000 times faster), in a fresh process that had compiled no GLSL, reading the two files in 5.4 ms; the program draws the right colour. The binary is vitaGL's own serialization (uniform indices plus the GXP), so it must come from the same vitaGL build that loads it. 14 shaders would load in about 0.5 ms instead of 5 s.
- **Correction: the fixed-function path also compiles at run time.** `ffp.c` generates a Cg source per combination of GL state, compiles it with the shader compiler and caches the GXP in `ux0:data/shader_cache/v*/` by a hash of the state. First draw of a new combination, measured (mask 512): texture + alpha test **153 ms**, + linear fog **164 ms**, + fog + blend **168 ms**, **one light 645 ms**; the same draws from the cache (second run) take 2.6 to 4.7 ms. (The plain textured draw and blend were already cached by the earlier runs.) So the UI and anything else on the fixed-function path pays the compiler on first use, and ships best with its cache files or its own shaders.
- **How long a first-run shader compile can take (heavier shaders, mask 1024; real Vita, 444 MHz; every one compiled and linked, no GLSL errors):** compile time depends on the work in the shader, not only its length, and **the skinned character is the worst at about 1.07 s**.

  | Shader (GLSL ES 1.00, written as stand-ins for the planned set) | Source chars | Compile |
  |---|---|---|
  | ImGui's two shaders together (stock backend) | small | 323 ms |
  | Two-layer terrain (reference) | 800 | 357 ms |
  | Sky dome (gradient, sun glow) | 595 | 375 ms |
  | WMO group (lightmap, vertex colour, 4 lights, fog, alpha test) | 1455 | 573 ms |
  | Stress (8 lights with specular, 6 samples) | 1574 | 643 ms |
  | Water (waves, fresnel, two normal layers) | 1411 | 749 ms |
  | **Skinned character** (4 bone influences from a uniform array, 4 lights, specular, env, fog) | 1949 | **1069 ms** |

  About 330 ms of every compile is fixed overhead; the rest follows ALU and branching work (loops, `pow`, matrix building from a uniform array), not characters of source. **Plan on 0.4 s for a plain shader, 0.6 to 0.8 s for lit or water-like ones, 1 s or more for a skinned one** (real WoW character shaders have more layers than this stand-in, so 1.5 s is not unreasonable).
  A plausible v1 set (2 terrain, 3 static M2, 3 skinned M2, 3 skinned character variants, 2 WMO, water, sky, 4 small UI/minimap/overlay) is **about 12 s** on the first run; **40 programs of mixed weight about 30 s**; and **every skinned permutation costs a second or more**, so the cap on permutations matters most there. At the 333 MHz a plain app starts with, add a third. Not tested: compiling on a second thread (vitaGL serialises GL calls, and the compiler's state is global, so assume the compile is serial and blocks the GL thread).
- **Does the client need `libshacccg.suprx` at all?** Read from vitaGL's source (`gxm.c`, not run): `vglInit*` tries to start the compiler and **carries on if it cannot**; only `glCompileShader`/`glLinkProgram` of GLSL and new fixed-function combinations then fail. So an app that loads only precompiled binaries (GLSL via `glShaderBinary`, UI via its own shader or a shipped cache) may run on a console **without** the file. **Untested on a device without it** (the user's console has it; renaming a system file to test was not done): VITA-14 should test it, since it removes the biggest user-facing prerequisite.
- **vitaGL's garbage-collector thread** is created with the priority and affinity given by `vglSetupGarbageCollector(priority, affinity)`, called **before** `vglInit*` (default priority `0x10000100`, affinity 0 = all cores; from the source, not measured): call it with the thread budget's values (VITA-8) so the thread does not compete with the pump. The SDK's library also shows a **vitaGL boot splash** (a "vitaGL Splashscreen" thread in the crash dump); a vitaGL built with `NO_SPLASHSCREEN=1` does not.
- Building vitaGL from source: `git clone https://github.com/Rinnegatamante/vitaGL`, then in the VitaSDK container `make -j6 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1` (about 2 minutes; the cache then needs `vglSetShaderCachePath(...)` before init, and creates 512 sub-directories on first use, which takes several seconds); the probe links it with `-DVITAGL_CUSTOM=<dir with libvitaGL.a and include/vitaGL.h>`.
- **Link trap:** vita-elf-create failed ("Cannot allocate N bytes for SCE data at end of segment 0; segment 1 overlaps") once the probe grew; `-ffunction-sections -fdata-sections -Wl,--gc-sections` (as in `cmake/vita/Vita.cmake`) fixed it.

**Vita3K v0.2.1 (macOS) runs the probe up to the pixels:** init and GL queries work (pool sizes are the emulator's numbers), but `glReadPixels` returns zeros, so the pixel tests fail there and mean nothing. (An earlier version of this section said the emulator could not run vitaGL at all: wrong, see the next trap. vitaGL's own stock sample, which loads a PVRTC texture, does abort the emulator; unexplained.)
`libshacccg.suprx` is needed (it loads at init); on Vita3K it is copied from the Vita into the emulator's `ur0/data/`.

**Traps found:**
- **`vglInit*` returns `GL_TRUE` only if the requested resolution had to be lowered (`res_fallback` in vitaGL's `vgl.c`) and `GL_FALSE` on a normal success.** Do not treat the return value as success; check state (`vglMemTotal(VGL_MEM_VRAM) > 0`, `glGetString(GL_VERSION)`). This cost the first version of the probe an afternoon.
- newlib's `printf` drops `%zu` (the known trap): `vglMemFree()` lines printed `zu KB` until cast to `unsigned long long`.
- `imgui.cpp` needs `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS` on the Vita (`fork`/`execvp`/`waitpid`); the link needs `SceShaccCgExt taihen_stub` beside `vitashark`; vitaGL's headers do not include `GLES2/gl2ext.h`, so the S3TC constants are defined in the probe.
- `build-vita/logs/vita3k.log` contains binary bytes: use `grep -a`.

**Running a test app on the real Vita without reinstalling it (done 2026-10-03, no physical access).** The user installs one VPK once; any other test app can then be swapped into that slot over FTP and the original restored:
`quit <TITLEID>` (vitacompanion port 1338; **`destroy` does not exist**, `help` lists the commands, `nosleep` and `screen` also exist) so the file is not locked (a running app gives FTP error 550), back up `ux0:/app/<TITLEID>/eboot.bin` and keep its SHA-256,
upload the new `eboot.bin`, put the settings in `ux0:data/wowee/`, `launch <TITLEID>`, read the log over FTP, `quit` again, and **restore and compare the SHA-256** when done. Safest order: smallest test first, the next only if the device still answers on 1338 (a GPU hang is the one failure that would need a physical button).


## 20. wowee_client: the application shell on the Vita (VITA-52) *(runs on Vita3K up to the first-run screen; the real device is VITA-13)*

`wowee_client` (`cmake/vita/wowee_client.cmake`, VPK `wowee_client.vpk`, title `WOWC00001`) is the shared `Application` built for the Vita: `src/core` and `src/ui` and `src/addons` as they are, the Vulkan-free half of `src/rendering`, over `wowee_core`, with the Vita's own `core::Window` and renderer skeletons. `src/main.cpp` runs the `Application` when `WOWEE_VITA_CLIENT` is defined (the older `wowee` target is still the startup-path stub).
Build: `JOBS=5 tools/vita/build.sh` (all targets), or in the container `cmake --build build-vita --target wowee_client.vpk-vpk`. Run: `tools/vita/vita3k_macos.sh build-vita/wowee_client.vpk WOWC00001 --seconds 40`.

**How SDL3 and vitaGL share the display (decision, read from the SDK's SDL3 and measured on Vita3K).** vitaGL owns the display: `vglInitExtended` brings up sceGxm and the framebuffers. SDL3 is initialised with `SDL_INIT_VIDEO | SDL_INIT_EVENTS` and the window is created with **no graphics flag**; the SDK's SDL3 has the Vita video driver (touch, keyboard, window handle) and a GXM *renderer*, but the renderer only starts when someone creates an `SDL_Renderer`, which nothing here does, and its GL-over-PVR path is behind `SDL_WINDOW_OPENGL`, which is not set. `ImGui_ImplSDL3_InitForOther` feeds input. `src/platform/vita/vita_window.cpp` is the Vita `core::Window` (a separate file selected in CMake, no `#if` ladder in `window.cpp`); `include/core/window.hpp` is shared and unedited, and fixes the names `rendering::VkContext`, `VkUiTextureService` and `VkImGuiBackend` through forward declarations and `unique_ptr`s, so the Vita file defines classes with those names (the GL context marker, a GL-texture `IUiTextureService`, an `IImGuiBackend` over `imgui_impl_opengl3`). "Vk" is the shared header's spelling, not Vulkan.

**What the link needed (~230 undefined symbols the first time).** All were methods of the 14 renderer classes the shared code calls. They are skeletons in `src/rendering/gl/*_gl.cpp` (no-ops; generated once from the shadow headers' declarations, now ordinary source, edited by VITA-13/18/19/20 as the real renderer arrives). `Renderer` itself does real work: `beginFrame` clears, `endFrame` draws ImGui's draw data and calls `vglSwapBuffers`, `LoadingScreen::render` draws a plain progress screen. GPU-only classes that `Renderer` holds by `unique_ptr` and the shared code never calls (`RtScene`, `HiZSystem`, `VolumetricFog`, ... ) are **opaque** shadow headers (`_opaque()` in `tools/vita/gen_shadow.py`) with constructors and destructors in `gl/gpu_only_gl.cpp`; destructors of the others are in `gl/destructors_gl.cpp`. Vita stand-ins for desktop-only pieces: `map_window_stub.cpp` (never opens), `open_url_vita.cpp` (no browser), `process_shim.cpp` (`waitpid`).
**When upstream adds a call:** the Vita compile or link names the missing method. Declare it in the shadow header only by regenerating (`gen_shadow.py`), and define it in the matching `gl/*_gl.cpp`; never edit a generated shadow header.

**Addons are in the client, not in `wowee_core`:** `Application` needs the Lua API, and all 13 `src/addons` files parse with the Vita compiler against the shadow headers and link (user decision 2026-10-02: `wowee_core` v1 excludes them; the client target is a different library, so that holds; VITA-49 still owns moving them into a library and removing `cvar_defaults.cpp` from the core).

**Vita3K smoke test and how to read it.** The log file `ux0:data/wowee/wowee.log` stayed empty in this build even with `WOWEE_LOG_FLUSH_MS=0` (unexplained; the earlier stub wrote it): use the UDP sink: `WOWEE_LOG_UDP=127.0.0.1:9999` in `env.txt`, `nc -u -l 9999 > udp.log` (datagrams carry no newline: `sed 's/\[1970/\n[1970/g'`), remove `env.txt` afterwards. Pass criterion: `Vita frames presented: N` (warning level, every 300 frames) keeps counting; measured 2,100+ frames in 40 s at about 55 fps on the emulator (no number for the device), after one 1 s first frame (corrected 2026-10-04: that was the SDK vitaGL's boot splash, not shaders, section 21). Without game data the client reaches the first-run / login screen; it cannot show pixels on Vita3K (`glReadPixels` is zeros), so the visual check is the user's on the device (VITA-13).

**On-screen keyboard (VITA-55, verified on the real Vita).** SDL's own Vita keyboard cannot be given initial text (its `sceImeDialog` always opens empty and the result is appended to the field), so it is switched off (`SDL_HINT_ENABLE_SCREEN_KEYBOARD=0`, `Window::initialize`) and the client opens the system dialog itself (`src/platform/vita/vita_ime.cpp`, `include/platform/vita/ime_dialog.hpp`, link `SceIme_stub`) with the field's text and a password mode. Two traps: (1) **vitaGL draws the dialog only if the frame is presented with `vglSwapBuffers(GL_TRUE)` while it is active**, otherwise it opens invisibly and swallows all touch input (`Renderer::endFrame` passes `imeActive()`); (2) **the pre-game fields are PaperUI controls, not ImGui text boxes**, so ImGui's state knows nothing about them: `PaperUI::field` has a `#ifdef __vita__` call to `imeRequest(text, password)` when a field is tapped (the only shared-file edit of this item). On Enter the backend sends Ctrl+A, Delete and the new text to ImGui's input queue, so PaperUI's normal editing path applies; Cancel changes nothing. Not covered: the interface (FrameXML) edit boxes that `Application::run` handles, and digits-only fields (VITA-55 follow-up if they misbehave).

**First numbers from the device (VITA-13, 2026-10-04).** `wowee_client` on the real Vita: login screen at ~59 fps (vsync-locked, empty scene), first frame 1 s (the SDK vitaGL's boot splash, see section 21; first written up wrongly as ImGui's shaders), vitaGL pools after init: CDRAM 72 of 96 MB free, **RAM pool 6 of 11 MB free** (the 192 MB newlib heap takes the RAM vitaGL would otherwise get; GlProbe had 88 MB with a 128 MB heap), PHYCONT 4.7 of 26 MB. The pool lines are logged at warning level (the default level hides INFO). Touch works; the D-pad and buttons do nothing on the PaperUI login screen (VITA-17).


## 21. Shader cache and the vitaGL build (VITA-53) *(verified on the real Vita; the manifest and progress screen are still to come)*

**Correction first.** The 1 s first frame seen on the device (`SLOW render stage 'beginFrame': 1000 ms`, which is only `glClear`) was **the SDK vitaGL's boot splash, not ImGui's shaders** (measured: it is there with a warm shader cache, and gone with a vitaGL built `NO_SPLASHSCREEN=1`). `tools/vita/build_vitagl.sh` builds vitaGL from source in the container (pinned commit, output `build-vita/vitagl/`); pass `-DVITAGL_CUSTOM=build-vita/vitagl` to CMake to link it. Without it the client still works, with the splash.

**The cache.** `src/rendering/gl/shader_cache.cpp` sits in front of vitaGL's shader calls through the linker (`-Wl,--wrap=glCreateShader,glShaderSource,glCompileShader,glAttachShader,glLinkProgram,glDeleteShader` on `wowee_client`), so every GL user, the stock ImGui backend included, is covered with no change: a shader whose binary is cached is loaded with `glShaderBinary` (0.05 ms); after a program links, the shaders vitaGL compiled are dumped with `vglGetShaderBinary` and written to `ux0:data/wowee/shaders/<key>.bin` through a `.tmp` file and a rename. The key is two FNV-1a hashes of stage + full source + a build tag, the tag being a hash of the linked `libvitaGL.a` plus the entry format version (`include/platform/vita/shader_cache_logic.hpp`; host test `tools/vita/depcheck/shadercache_test.cpp`: `c++ -std=c++20 -Iinclude tools/vita/depcheck/shadercache_test.cpp`). An entry that fails its magic, version, length or checksum is a miss and is deleted. `WOWEE_SHADER_REBUILD=1` in `env.txt` clears the cache at start. Log lines (warning level): `Shader cache: program N compiled in X ms, K stored`; nothing is logged for a hit.
**Measured on the real Vita (ImGui's program, 2026-10-04):** cold cache: compiled in 331 to 342 ms, 2 binaries stored; warm cache: nothing compiled, window init to first frame 58 ms. (With the custom vitaGL the RAM pool after init is 13 MB, 8.6 MB free, against 11 MB with the SDK's: the 192 MB newlib heap still decides, see section 20.)

**The first-run build and its progress screen (VITA-53 part 2, measured on the real Vita 2026-10-04).** `Window::initialize` calls `rendering::gl::buildShaders()` (`src/rendering/gl/shader_build.cpp`) before the interface starts. The manifest (`shader_manifest.cpp`) lists every program with a cost class; **today it holds only developer stand-ins** (GlProbe's terrain, M2, sky, WMO, water, skinned character and a stress shader), used only with `WOWEE_SHADER_SELFTEST=1` in `env.txt`, until VITA-14 puts the real shaders there and makes the list unconditional. The loop: for each program, if both shaders are cached build it silently (no frame); otherwise draw a frame ("Compiling <name>", "n of N", about s left, determinate bar), compile, `sceKernelPowerTick`, pump SDL events, next. The interface's own program is built first through a temporary ImGui context (the screen's text needs it); before it exists the screen is a bar made of scissored clears, because the fixed-function path would compile a shader of its own. The weights and the time-left estimate are `Progress` in `shader_cache_logic.hpp` (the estimate is scaled by how slow the real compiles are against the guesses; computed from the logged times of the 8-program run it is within 3 % after the first third). If a program fails: with `ur0:data/libshacccg.suprx` missing a message screen says so and waits for a button or a touch; otherwise a message names the program and the log. The stale-file sweep runs only after a run without failures, so **every program the renderer can build must be in the manifest** or its cache entry is deleted at the next start.
**Numbers, 8 programs (ImGui + 7 stand-ins):** cold: 4.3 s in total, longest single stall **1.09 s** (the skinned stand-in); warm: **72 ms, nothing compiled, no frame drawn**. The user watched the cold run: the bar moved, names showed, nothing looked frozen. A spinner cannot animate during a compile (one blocking call on the one GL thread), so there is none; the "n of N" counter changes before every program.
**Compiler optimisation level matters (new):** with no call at all vitaGL's default compiled the skinned stand-in in 2.1 s (5.8 s for the set); `SHARK_OPT_DEFAULT` (O2) takes 1.05 s (4.3 s); SLOW (O0) 1.15 s, SAFE (O1) 0.97 s, FAST and UNSAFE 2.1 s like the default. The client now sets O2 and the level is part of every cache key; `WOWEE_SHARK_OPT=0..4` in `env.txt` overrides it. **Not measured: whether O2 code runs slower on the GPU than the default's** (VITA-14's fill-rate test).
**Resume (device):** the cache was emptied over FTP, the app killed by `quit` once 8 and 10 entries existed (mid-compile), and relaunched: 4 compiled + 4 cached, then 3 + 5, ending with 15 valid entries and no `.tmp` file each time. Procedure: `DELE` the files in `ux0:data/wowee/shaders` over FTP, `launch`, poll the file count, `quit`, `launch` again, read `Shader build` in `wowee.log`.
**Bugs found and fixed on the way:** the sweep deleted the interface's entries on warm runs, because the stock ImGui backend creates its program lazily (fix: `ImGui_ImplOpenGL3_CreateDeviceObjects()` in the build step); an interruption test that polled before the cache was emptied tested nothing (empty the cache first).


## 22. The renderer's first shaders, and a vitaGL bug in compressed textures (VITA-14) *(measured on the real Vita, 2026-10-04)*

**Programs** (`src/rendering/gl/shader_sources.cpp`, GLSL ES 1.00 embedded as strings, variants from one source with a `#define` prefix; listed in `shader_manifest.cpp`; rows with costs in `docs/vita/SHADERS.md`): terrain with 0, 1, 2 or 3 layers over the base (the three alpha maps of a chunk packed into one RGBA texture, `uAlpha`), and static M2 in three kinds (opaque, alpha test, blend). Lighting and fog are per vertex, upstream's own terms (ambient + Lambert; linear fog, additive blends fade the fog to black as `m2.frag` does for `blendMode >= 3`). Cut from upstream's shaders: shadows, the 64 local lights, volumetric fog, RT lighting, normal-from-derivatives bump, the alpha seam blur, foliage sway, specular, SSBO instancing. `linkProgram()` (`gl_program.cpp`) is the one place a program is built; a `ProgramDef` carries its attribute names in location order and an optional compiler level. Skinned shaders are VITA-20.
vitaGL facts read from its source: vertex uniform limit **128** vec4, fragment **16** vec4, varyings **8**, texture units **16**, `GL_LUMINANCE`/`GL_ALPHA` are supported; its GLSL translator has a preprocessor (`#define`/`#if` work, 4 terrain variants from one source); attributes are bound after the shaders are attached (a loaded binary looks the name up in the binary, a translated shader applies the binding at link).

**The self-test** (`WOWEE_GL_SELFTEST=1` in `env.txt`, with `WOWEE_LOG_FLUSH_MS=0`; `src/rendering/gl/shader_selftest.cpp`): 20 pixel read-back checks (every terrain variant with DXT1 mipped base and DXT5 layers, lighting, fog, depth test, M2 opaque/alpha-test/blend/additive-fog, a DXT upload probe), the GL limits, swap-paced fill rates and the per-draw CPU cost. `WOWEE_GL_TEST_ORDER=1` creates the textures after the probe instead of before; both orders must pass. **6 of 6 launches pass 20/20 (cold and warm, both orders) with the patched vitaGL.**

**The vitaGL bug (found by the warm run, fixed by `tools/vita/patches/vitagl-compressed-cpu-swizzle.patch`).** With the SDK's or an unpatched vitaGL, the terrain checks passed on a cold run and failed on a warm one, DXT5 with mips sampled as zero, the base DXT1 sampled black or at 23 %, results flipped between builds, and about every second launch stopped inside the texture uploads. `gpu_alloc_compressed_texture` (vitaGL `gpu_utils.c`) uploads DXT1/3/5 levels with `sceGxmTransferCopy` from a temporary buffer (`gpu_alloc_mapped_temp`); the next upload reuses that buffer before the transfer has read it (delays, `glFinish` and `sceGxmTransferFinish` did not help: the hazard is between back-to-back uploads, which is why a probe that uploads, draws and deletes one texture at a time never saw it; 2x2 and 1x1 levels also issue zero-size transfers). The patch forces the CPU swizzle path for every compressed upload (UBC1, UBC2, UBC3). `tools/vita/build_vitagl.sh` applies it; **`wowee_client` now requires that build** (`build-vita/vitagl`, configure fails otherwise; `-DWOWEE_ALLOW_SDK_VITAGL=ON` overrides). The patch is a local fix, not upstream's; its upload cost is not measured (VITA-15). This also explains GlProbe's "DXT5 after a mipped DXT1 samples as zero" quirk.
Debug lesson: **the logger buffers warnings; a launch that "hangs" may be a launch that got further and lost its last lines**. Use `WOWEE_LOG_FLUSH_MS=0` for any run where a stall is possible, and count a launch that does not reach its end line as a failure (a retry loop hid these hangs for a while).

**Numbers (device, 444 MHz ARM, GPU 222):**
- **Shader compile** (cold, 8 programs: the interface's + 7 renderer programs): 2.9 s in total, longest stall **593 ms**; warm 75 ms.
- **Fill rate** (additive-blended full-screen passes; opaque stacked passes cost almost nothing on this tile-based GPU, hidden surfaces are removed, so blending is what forces shading). Per full-screen pass, 960x544: terrain base **1.25 ms** (420 Mpix/s), +1 layer 1.93, +2 layers 2.61, +3 layers **3.08 ms**; M2 opaque 1.38, M2 blend 1.38-1.41; **M2 alpha test (discard) 3.10 ms at O2, 2.16 ms at O3**. At 640x368 in an FBO: base 0.52, 1 layer 0.90, 2 layers 1.17, 3 layers 1.46, M2 0.75, alpha test 1.40 (O2) / 1.01 (O3). The 640x368 scene costs 0.42-0.47 of the 960x544 one (pixel ratio 0.47): the FBO route scales as expected. Not measured: the extra pass that scales the FBO to the screen.
- **Compiler level: GPU speed is the same at O2 and O3 except for `discard`** (alpha test 30 % faster at O3); O3 compiles up to twice as slowly (the SDK vitaGL's own default is O3 / `SHARK_OPT_FAST`, which is what produced the 2.1 s compile in section 21). So the client default is O2, and only the M2 alpha-test program is compiled at O3 (`ProgramDef::compilerLevel`, in the cache key).
- **Draw cost on the programmable path**, a new `uModel` uniform every draw, client-side vertex arrays: **7.2-7.6 microseconds per draw** (fixed-function was 2.2-4; budget about 7.5 ms per 1000 draws). An upper bound until vertex buffers exist.


## 23. Gamepad navigation of the pre-game screens, and the ELF padding trap (VITA-17) *(verified on the real Vita: D-pad moves, Cross opens the keyboard; the rest of the buttons are still to be checked)*

**Why a new mechanism.** The login, realm and character screens are PaperUI controls (hit-tested against the mouse pointer), not ImGui widgets, so ImGui's gamepad navigation had nothing to walk through and the buttons did nothing. `include/ui/paper_nav.hpp` / `src/ui/paper_nav.cpp` moves the *pointer* instead: every PaperUI control announces its rectangle when it asks `hovered(a, b)`; D-pad or left stick move the pointer to the nearest control in that direction (distance-based, sideways moves stay in their row, up/down take any control further on); **Cross presses where the pointer is** (a left click: a field raises the keyboard with its text, VITA-55, a dropdown opens); **Circle is Escape** (closes a dropdown, goes back), **Start is Enter**. While a dropdown is open its rows are the only targets. The control under the pointer already draws as hovered, and a gold ring marks it while the pad is the one driving. It reads ImGui's gamepad keys (`ImGuiKey_Gamepad*`), which the SDL3 backend feeds from the pad; `GamepadControls` keeps `NavEnableGamepad` on outside the world. The geometry (`pickNext`, `normaliseNavRects`, `navCurrent`) is host-tested: `c++ -std=c++20 -DPAPER_NAV_GEOMETRY_ONLY -Iinclude tools/vita/depcheck/papernav_test.cpp src/ui/paper_nav.cpp`. **Shared files touched, all `#ifdef __vita__`:** `include/ui/paper_ui.hpp` (include + one member), `src/ui/paper_ui.cpp` (`hovered()` registers the rect, `begin()` clears, `end()` runs the navigation); desktop is unchanged. Generic enough to offer upstream later (VITA-33).
**The ELF padding trap.** `vita-elf-create` appends about 4.7 KB of SCE import data after the text segment and fails with "Cannot allocate N bytes for SCE data at end of segment 0; segment 1 overlaps" when the data segment, aligned to 64 KB, starts less than that after the end of the code and constants. It is a coin flip on code size: VITA-17's few hundred lines tipped the client over it (GlProbe hit it too). A larger page alignment (`-Wl,-z,max-page-size=0x100000`) turns it into "Layout error: overlapping sections". The fix is a padding array in the read-only segment (`src/platform/vita/vita_text_pad.cpp`, `WOWEE_VITA_TEXT_PAD_KB`, default 40): when the error returns after code growth, change that number by 16 or 32 and rebuild. A `const` array at namespace scope has internal linkage in C++ unless declared `extern` first, and `-Wl,-u` then keeps it; the first attempt silently produced no padding for that reason.
**Deploy hygiene (new `deploy.sh`-style routine):** quit the app, wait about 9 s, upload `eboot.bin`, **download it again and compare SHA-256** (a damaged upload shows as error C1-2738-0 on the console), launch, and check the log's first timestamp is new. The Vita's clock is not the Mac's clock.


## 24. Memory: the extended mode, the 288 MB heap, and what the world costs (VITA-23) *(measured on the real Vita, 2026-10-04)*

**Entering the world.** With the full WotLK extraction on the card (201,606 files, 19 GB, copied over USB with `rsync`; the 33 MB `manifest.json` indexes in about 0.1 s), the client logs in, lists the character and enters the world on the real Vita. The first attempt ran out of memory **at world entry**: `preloadDBCCaches()` loads `Spell.dbc` and its name cache on top of the 64 MB the client already holds at the login screen, and the 192 MB heap was gone about 4 s into the `SMSG_LOGIN_VERIFY_WORLD` handler (`std::bad_alloc`, caught per packet, the client then limped on half-broken).
**Extended memory mode.** Without it the heap cannot go above roughly 192-200 MB. Setting **`ATTRIBUTE2=12` in `param.sfo`** (`-d ATTRIBUTE2=12` in `VITA_MKSFOEX_FLAGS`, with the UNSAFE flag the eboot already has) lets the client start with a **288 MB** newlib heap and gives vitaGL a larger RAM pool (26 MB, 13 MB before). **The attribute is read when the app is installed**: swapping `param.sfo` and `eboot.bin` over FTP did nothing; the VPK has to be reinstalled in VitaShell once. (My first attempts at 208-300 MB were judged by a log line that printed a stale constant, so only the failed 300 MB start - a crash in the exception allocator - and the final 288 MB success are valid.) `wowee_client` uses `WOWEE_VITA_CLIENT_HEAP_MB` (288); the other Vita executables keep 192 and need no reinstall. `memory_monitor.cpp` reads the real heap size from `_newlib_heap_size_user`.
**Numbers (heap in use, from `mallinfo`, logged every 300 frames; `Vita frames presented: N, heap in use X MB of Y MB arena`):** login screen **63 MB** (fonts, DBC layouts, UI); in the world with the FrameXML interface off **204 MB** (+141 MB: the DBC caches, the Lua state and the Blizzard addons that load anyway); the stock FrameXML interface (139 files) **does not fit**: it grew the heap from 64 MB to the whole 288 MB before it finished loading (Lua "not enough memory" at `ContainerFrame.xml`, then `bad_alloc`). So **the client turns FrameXML off by default on the Vita** (`WOWEE_LOAD_FRAMEXML=0`, set in `vita_main.cpp` unless `env.txt` says otherwise) until VITA-28 makes it fit; the in-world screen has no HUD yet. Timeline of the entry: login to character list a few seconds, Enter World to the world-verify handler 4.6 s, a 25 s warmup that waits for terrain (not implemented yet), the interface and addons about 8 s: about 40 s to a blank world, running at about 56 fps.
**Tools added.** (1) An allocation-failure hook (`std::set_new_handler` in `vita_main.cpp`): when `new` fails it writes `heap in use / free / arena` to `ux0:data/wowee/alloc_failed.txt`, which says whether it is real exhaustion. (2) A **throw trace** (`vita_throw_trace.cpp`, linked with `--wrap=__cxa_throw`): the stack of every `length_error`, `bad_alloc` and similar throw goes to `ux0:data/wowee/throw_trace.txt` as offsets from the module base; resolve with `arm-vita-eabi-addr2line -f -C -i -e build-vita/wowee_client $((0x81000000+offset))`. (3) `tools/vita/parse_core.sh` now gives the container 8 GB (the client's 380 MB debug ELF killed `objdump` in a default 1 GB VM); a crash dump of an uncaught exception shows `abort()` and `_kill_r`, and the `bad_alloc`/`__cxa_throw` frames in the stack.
**Also found:** the Lua unit API (`UnitName("player")`, `UnitAura`, ...) throws and catches hundreds of `std::invalid_argument` per second from `std::stoul` used as control flow (only with FrameXML on); the first frame of the login screen takes 0.8 s and loading the 5 interface fonts 18 s (startup is about 22 s); assets the client opens relative to its folder (`assets/krayonsignin.png`, `Wowee.png`, ...) are now packaged in the VPK (`app0:assets`); `HOME` defaults to `ux0:data/wowee` so the Warden cache (`$HOME/.local/share/wowee/warden_cache`) lands on the card.
**Screenshots from the device (VITA-17).** `Renderer::captureScreenshot(path)` is real now (`src/rendering/gl/renderer_gl.cpp`): it reads the framebuffer after the interface is drawn and before the swap, and writes a PNG with stb. `WOWEE_SHOT_FRAME=<n>` in `env.txt` takes one at frame n to `ux0:data/wowee/shot.png`; download it over FTP (`curl ftp://<ip>:1337/ux0:/data/wowee/shot.png -o shot.png`) and look at it: the way to see what the Vita draws without a camera. `glReadPixels` works on the device (not on Vita3K). First look (login screen): the card is 310x375 px of 960x544 at the code's scale of 0.72; a trial boost of 1.2 gave 372x445 and was judged better by the user, but "more options" then overflows the screen, so the scale work is **postponed** (a quality-of-life item; needs a layout that fits the expanded card, and the character screen needs about 620 units of height).


## 25. Terrain on the Vita: first light (VITA-18, VITA-15) *(real Vita, 2026-10-04; `docs/vita/measurements/terrain_first_light_2026-10-04.png`)*

**What is drawn.** Elwynn Forest around Northshire, streamed and rendered on the Vita: grass, dirt and rock with their blended texture layers, hills and distant mountains. `TerrainManager` is upstream's own (`terrain_manager.cpp` compiled as it is, two Vita gates: no upload batches, no M2/WMO re-initialisation), so ADT parsing, mesh building and tile streaming on the worker thread are the real code; only the GPU half is new: `src/rendering/gl/terrain_renderer_gl.cpp` (one vertex and one index buffer per tile, 16-bit indices; one packed RGBA alpha texture per chunk, layers 1-3 in r, g, b, as `uAlpha` of the VITA-14 terrain shader; frustum and distance culling per chunk; four programs by layer count) and `gl_texture.cpp` (BLP to GL: DXT1/3/5 stay compressed with their mips, palettised BLPs go up as RGBA8; the top mip levels can be skipped, `TextureCache::setSkipMips`). The `Renderer` skeleton now owns the camera, the camera controller and the terrain (initialize, `loadTestTerrain`, update, `renderWorld` with an OpenGL projection built with `perspectiveRH_NO`, the camera's own Vulkan matrix is used only for frustum culling).
**New plumbing.** The generated shadow headers can carry Vita-only members (`EXTRA_MEMBERS` in `tools/vita/gen_shadow.py`: `TerrainRenderer` gets `glInitialize`, `glRender`, a private `Gl` state). `gl/scene_params.hpp` is what the renderers receive from the frame (camera, light, fog).
**Traps found.** (1) **`std::filesystem::exists(path)` throws on the Vita when a parent directory is missing**: newlib turns the sceIo error into EINVAL, the throwing overload only treats "not found" as false, and 33 call sites use it; on the tile worker thread the exception killed the app (`std::terminate`). `vita_stat_fix.cpp` (`-Wl,--wrap=stat`) reports a failed stat with EINVAL as ENOENT. (2) The world loader asks for a **load radius of 4 (81 tiles)**; the Vita ran out of memory after 13. `TerrainManager::setLoadRadius` is clamped to 1 on the Vita (3x3 tiles) and `Renderer::getTerrainLoadRadius()` says so (two gated lines in `terrain_manager.hpp`). (3) The throw trace now also logs `filesystem_error` and `system_error` with their message (the path and the error), which found (1) at once.
**Memory (heap in use, `mallinfo`):** login screen 76 MB; entering the world adds about 130 MB of caches before any tile (DBC name caches, Lua state, addon manager: this is the baseline to attack, VITA-23); each terrain tile then adds about 3-13 MB on the heap plus about 6 MB of GPU buffers and alpha maps (5.9 MB per tile measured: 37,120 vertices x 40 bytes, 16-bit indices, 256 packed alpha maps of 16 KB); with the 3x3 tiles the heap sits at **255-261 MB of 288**, so there is almost no headroom. Frame rate over the terrain: about 55 fps in the log (300 frames per 5.4 s).
**Seen in the first picture, not fixed yet:** no sky or fog colour (the clear colour is a dark navy), the camera sits at the edge of the loaded tiles so the ground ends in a straight line, grazing-angle texture smearing on the foreground (no anisotropic filtering), no character, doodads, water or buildings, no controls tuned for the Vita yet.
**Tools:** `ux0:data/wowee/shot.cmd` (any content, uploaded over FTP) takes a screenshot into `shot.png` within half a second, in any state, no restart; `Terrain tile [x,y] uploaded (heap in use N MB)` lines in the log.


## 26. World-entry memory (VITA-23)

Measured on the real Vita, 288 MB heap, `WOWEE_LOG_HEAP=1` (adds `{heap N KB}` from `mallinfo` to every log line; Vita only).

- The desktop file cache has a 256 MB floor (`AssetManager::setupFileCacheBudget`), almost the whole Vita heap, and kept every file read as raw bytes. The Vita arm caps it at 8 MB.
- Peak heap from login to terrain: 262 MB before, 184 MB after (5 terrain tiles, ~51 fps). Spell.dbc still costs ~93 MB while loading (raw file 49 MB plus the parsed copy) and ~50 MB stays resident.
- Trap: building `wowee_client` does not regenerate `client.bin`; build the `wowee_client.vpk-vpk` target before `deploy.sh`, and check the log line `file cache: N MB` (or a first-line marker) to know which binary ran.
- Still large: manifest 43 MB / 16 s (VITA-25), ItemDisplayInfo 13-20 MB, Spell.dbc resident.


## 27. Path depth limit (VITA-19)

**sceIo refuses any path with more than 10 components below the device name** (`ux0:a/b/c/d/e/f/g/h/i/j` works, one more fails with `0x80010016`, which looks exactly like "does not exist"). Measured on the real Vita with a chain of nested folders. It applies to `sceIoOpen`, `sceIoGetstat` and everything built on them, but not to directory listing (`sceIoDopen` lists the files, `stat` on them fails), and the FTP server of vitacompanion is not subject to it, so the files look fine from the Mac. Symptom: `Manifest entry exists but file unreadable` / `Texture not found` for files that are on the card (about 5000 failed reads in one world entry, props and textures drawn white or missing).

- The default data root `ux0:data/wowee/Data` costs 5 components (`data/wowee/Data/expansions/wotlk`), leaving 5 for the game's own tree; the extraction goes up to 9 (7 or more for about 5000 files). Everything under `world/azeroth/*/passivedoodads/<model>/` is already 6.
- **Put the data at `ux0:/expansions/<id>/...` and set `WOW_DATA_PATH=ux0:` in `ux0:data/wowee/env.txt`.** That leaves 8 levels; only 18 files (15 `world/wmo`, 3 inside the Windows launcher app) are still too deep. Verified: `Assets: Wrath of the Lich King - 201,592 files` from `ux0:/expansions/wotlk`. Moving the folder on the card is instant (`RNFR`/`RNTO` over FTP, same filesystem).
- The startup log warns when the data root is too deep (`Vita: the data root is N levels deep ...`).
- Trap while finding this: a diagnostic that stats every file on the main thread at start-up blocks the app on the progress screen for many minutes; do such scans on a thread or from the Mac.

## 28. Doodads and the memory of a prepared tile (VITA-19)

- Static M2 doodads draw (`src/rendering/gl/m2_renderer_gl.cpp`): per-model VBO/IBO, base mesh only, three program classes (opaque, alpha test, blend), frustum + distance culling (60 yards plus 40 per yard of radius, never past the fog), no collision, animation, particles or texture scrolling yet.
- **A prepared tile can take 100 MB.** Goldshire tile [32,48] (8 WMOs, 6500 WMO doodads, 1500 M2 placements) went from heap 129 to 235 MB while only being prepared. Until the WMO renderer exists the Vita skips WMO preparation (`prepareTile`), loads creature models not at all (`processCreatureSpawnQueue`) and lets one tile finalize at a time (`processReadyTiles`, `maxReadyQueueSize_` 1). Peak after that: 198 MB of 288 with 9 tiles. The WMO renderer must bring its own memory plan (parse, upload, drop the CPU copy per group).
- Before the data move of section 27 these reads were failing, so the heap looked healthy at 190 MB; a failing read hides the memory a feature needs.

## 29. Frame time with buildings (VITA-19)

Goldshire church, camera still, measured on the real Vita with `WOWEE_LOG_LEVEL=info` (the `frame budget` and `GL stats` lines).

| change | fps (ms) |
|---|---|
| terrain + WMO + M2, view distance 1200, full resolution | 19.5 (51) |
| 3D at 0.75 of the screen (off-screen target, scaled up) | 21.5 (47) |
| + view distance 500 yards (fog 225..475) | **32.4 (31)** |
| same, back at full resolution (`WOWEE_RENDER_SCALE=1`) | **33-35 (29-30)**, the new default |

- The cost was draw submission: about 1800 draws a frame (926 terrain chunks, 553 WMO batches, 349 M2 batches) at about 10 us each, 18 ms of CPU. A short view distance cuts chunks, buildings and doodads together. `WOWEE_VIEW_DISTANCE` (default 500) and `WOWEE_RENDER_SCALE` (default 1; 0.75 gave nothing once the distance was short, `src/rendering/gl/scene_target.cpp`) are the knobs.
- **Test one renderer alone and it looks free** because the display is capped at 60 fps (16.7 ms): a renderer that costs 12 ms hides under it. Compare against the sum, or look at `GL stats <name>` (CPU per frame and draw calls, logged every 300 frames by `gl_stats.hpp`).
- Live switches without a restart: `ux0:data/wowee/gl.cfg` with the words `noterrain nowmo nom2` (read every 60 frames, delete the file to reset; do not forget it: it hid the buildings in one test).
- **WoWee turns the camera round by itself after 2 idle minutes** (`idleOrbit`, re-applied from the saved settings by the game screen): it moves the picture under a test. The Vita renderer forces it off each frame.
- Did not help: bigger sceGxm parameter/VDM/vertex/fragment buffers (`WOWEE_GXM_*` knobs, no change in fps, and they took 16 MB of video memory).
- **GPU memory is full** in the Goldshire area: vitaGL's pools (video 96 MB, RAM 25, physically contiguous 26) read 0 MB free after the third tile. Terrain holds 53 MB (the alpha maps are RGBA8, 4.2 MB a tile), buildings 43 MB of buffers and 29 MB of textures, doodads 5 MB. Not yet shown to cost frame time, but there is no headroom for water, characters or more tiles. To do: RGBA4444 alpha maps, packed vertices, texture mip skipping, drop far tiles' buffers.
- The link can fail with "Cannot allocate N bytes for SCE data" after code growth; the padding is now 56 KB (section 23).

## 30. Building collision (VITA-57)

The upstream collision code (`wmo_renderer_collision.cpp`) reads the shadow class's `loadedModels` / `instances` / `spatialGrid`; the GL renderer fills them (`wmo_renderer_gl.cpp`), so floors, walls and stairs of buildings work. Verified on the device: stopped by the church wall, walking inside it. M2 props (trees, fences) are not solid yet.

- **Memory decides the design.** Building the collision grids for every loaded building took the heap to 294 of 288 MB (a 25-second-old game). Collision is therefore built on a worker thread only for the buildings whose bounding box is within 120 yards of the player, one at a time, and dropped when no instance of the model is within 320 yards (`glUpdateCollision`). It re-reads and parses the WMO from the card (about a second) and keeps one group's raw data at a time. It refuses to start below 60 MB of free heap and stops below 25 MB.
- **Skip giant models.** `STORMWIND.WMO` is one model with 286 groups and 727,741 triangles; its grid took the last of the heap, and its box (a sphere test said "near") covers a whole district. Models above 90,000 triangles get no collision on the Vita for now.
- **A moving transport must not rebuild the index.** `setInstanceTransform` first called `rebuildSpatialIndex()`; for a boat or an elevator that is every frame, and with the Stormwind box in thousands of cells it cost 108 ms a frame (7 fps, all inside `gameHandler->update`). It uses `refileBounds` now. The same trap exists for any per-frame transform.
- The terrain tile cache (parsed tiles kept for reloading) is 4 MB on the Vita instead of ~36 MB.
- The log shows `Collision cost per frame` (camera controller and WMO queries: 0.1 ms and 0.05 ms in 2 calls) and `WMO collision built: <path> N groups, N triangles`.
- Result at the church: 50-56 fps, heap peak 236 MB.

### 30.1 Prop (M2) collision and the player (VITA-57 part 2)

Props collide: fences, posts, tree trunks, barrels, crates (verified on the device).

- **The camera was a free camera.** The controller is attached to a player through `setFollowTarget`, which the desktop does when the character model spawns. The Vita has no character renderer, so nothing ever attached it, and the controller ran in its free-walk mode: that mode asks for walls only (`sweepAgainstWalls(..., includeDoodads=false)`), so props were skipped, and `Renderer::getCharacterPosition()` never moved, so the position sent to the server never changed either. `Renderer::update` now attaches a position-only character (feet 1.2 yards under the camera) once the first terrain tile is loaded and zooms in to first person. Log line: `Camera attached to a position-only character`. Found with a counter at the sweep (`skipped(noDoodads) 600 of 600`); a probe that calls `checkCollision` directly looked fine, which is why that was not enough.
- **Mirror only what can block.** The M2 collision tables hold the models (classification flags from `classifyM2Model`, the authored collision mesh) and the instances whose model is not clutter, effect, trap or no-block: 2400 of 9000 instances.
- **Upstream pushes props out by 0.015 yards a step** (0.02 in total per instance): a nudge that slows a walker down. On the Vita the push resolves the overlap (up to 0.30 a step, 0.6 per instance), a gated edit in `m2_renderer_collision.cpp`.
- **Stormwind is skipped on the Vita** (`terrain_manager.cpp`, models over 64 groups): parsed it was ~100 MB of heap and 20 s on the first tile, plus 43 MB of GPU buffers. Heap peak went from 275 MB (out of memory) to 200 MB.

## 31. GPU memory (VITA-23)

vitaGL's pools (video 96 MB, RAM 25, physically contiguous 26) read `vram 0/96 ram 0/25 phycont 0/26` free around Goldshire, all three full, in the log line `vitaGL memory (free/total MB)`. Measured on the device, before and after:

| | before | after |
|---|---|---|
| terrain buffers + blend maps (9 tiles) | 53 MB | 32.5 MB |
| buildings, buffers | 43 MB (with Stormwind) | 12 MB (Stormwind skipped, DEV_SETUP 30.1) |
| buildings, textures | 28.9 MB | 28.6 MB |
| doodads, textures + buffers | 4.4 + 0.8 MB | same |
| pools free (video / RAM / contiguous) | 0 / 0 / 0 | 2 / 9 / 4 |

- **Blend maps at 4 bits a channel** (`uploadRgba4444`, 2 bytes a texel): 20 MB saved, no visible difference on the device (sixteen steps of blend). `WOWEE_ALPHA8=1` in env.txt keeps eight bits to compare.
- **A texture size cap does nothing here** (`WOWEE_TEXTURE_MAX`, default 256 for buildings and doodads, 0 = keep all): WoW's building textures are already 256 and smaller, 330 of them are 28 MB. Left in as a knob.
- Video memory is still full; the spill goes to RAM and contiguous RAM, which have about 13 MB free between them. That is the headroom for water and characters; more needs unloading what is behind the player (buffers and textures of far buildings) or packed vertices (terrain 40 -> 28 bytes a vertex would save about 4 MB).

## 32. Water (VITA-21)

Terrain and building water draw and the swimming code finds it (`src/rendering/gl/water_renderer_gl.cpp`). Verified on the device: colour and transparency, the level against the banks, wading and swimming, 55-57 fps.

- **The surfaces are upstream's.** `loadFromTerrain` / `loadFromWMO` keep one `WaterSurface` per chunk layer (the shadow class's `surfaces`), so the queries the camera controller uses (`getWaterHeightAt`, `getNearestWaterHeightAt`, `getWaterTypeAt`, `isWmoWaterAt`) are upstream's code over them. 315 surfaces for nine tiles around Goldshire.
- **Drawn as one blended mesh per tile** (or per building): 24 bytes a vertex, 16-bit indices, one draw call each, after the doodads, depth write off. A flat colour per liquid type (the colours and alphas of `getLiquidColor/Alpha`), a slow two-sine shimmer (0.88 to 1.0 of the colour) from the world position and the time, fog. Not done: reflection, refraction, foam, geometry waves, the merging of upstream (`MERGE_THRESHOLD`), the Stormwind-area height correction.
- **GPU memory:** the meshes are small enough not to move the vitaGL pool numbers (video 1/96, RAM 9/25, contiguous 4/26 MB free, unchanged).

## 33. Characters and creatures, phases A and B (VITA-20)

NPCs and creatures draw (`src/rendering/gl/character_renderer_gl.cpp`): standing in the bind pose of their model, with the static-M2 programs (no new shader, so nothing new for the shader manifest). Verified on the device at Goldshire (screenshot: Brother Danil, Dermot Johns and Godric Rothgar at their stalls, with skin and clothes).

- **What it draws.** Per batch the code copies upstream's `CharacterRenderer::render`: the geoset filter (`setActiveGeosets`; an empty set hides groups 15, 17 and 18; a filter that matches nothing draws everything), the texture of a batch chosen by texture type and slot override (a type-1 skin override applies to the skin groups only; hair wins), opaque and alpha-tested first, then blended. Not done: bones and animation (arms hang out in the T-pose), character texture composition (humanoids get their base skin only: no face, hair or armour layers; baked NPC textures work), weapons, helms, mounts, the player model.
- **`GpuTexture` is a tiny class** (a GL texture id) created per path; the white texture is the "no texture" marker the shared code compares against.
- **Limits and switches** (`env.txt`): `WOWEE_CHAR_MAX` (closest N drawn, default 24), `WOWEE_CHAR_RANGE` (yards, default 150, never past the fog), `WOWEE_CHAR_ANIM_RADIUS` (animation clock, default 120), `WOWEE_TEXTURE_MAX` (256). `gl.cfg`: `nochar`. Log: `GL stats Characters`, `GL memory Characters`, one `Character model` line per distinct model (bones per batch included: the human needs at most 46 bones in one batch, a wolf 29, a rabbit 18, so a 35-matrix GPU palette does not hold a human).
- **Model data is shared per M2 file.** The creature code loads a model once per display id (eight guards in eight skins are eight loads). The first run heap-peaked at 275 MB of 288 and took 1.4 to 2.4 s a spawn; sharing the vertex buffers and the `M2Model` between ids with the same name and sizes (textures stay per id) brought the heap to 190-215 MB, and `loadModel` of a shared model to 0.06 ms.
- **The big cost was the weapons, not the models.** `EntitySpawner::tryAttachCreatureVirtualWeapons` read each weapon's M2 and textures from the card on the main thread (0.7 to 2.3 s a creature, found by timing each phase of `spawnOnlineCreature`) and then the renderer refused the attach. On the Vita it returns false at once until weapons are drawn (phase D).
- **Measured on the device:** spawns 0.1 to 0.4 s (hair and skin textures still decode on the main thread, 40 to 280 ms each), about 35 fps at the church with 13 characters drawn (`Characters` 2 ms CPU a frame, 237 draw calls), for about a minute after world entry 4 to 8 fps while the terrain doodads and creatures stream in. GPU pools are full: vram 0/96, RAM 1/25, contiguous 4/26 free; characters took 9 MB of textures and 0.5 MB of buffers.
- **The player is not spawned on the Vita** (`Application::spawnPlayerCharacter` returns early under `__vita__` until phase C): the position-only character of DEV_SETUP 30.1 still carries the camera.

### 33.1 Animation (phase B)

- **The Stand pose is baked once per model.** At load the shared vertex buffer is skinned with frame 0 of animation 0, so nobody stands with arms out (the bind pose). Everyone beyond the animated set draws from it for free.
- **The closest few are skinned on the CPU every frame** (`WOWEE_CHAR_ANIM_MAX`, default 4, at most 8; `WOWEE_CHAR_ANIM_DIST` yards, default 25). Bone matrices are upstream's (`getBoneTransform` with `m2_track_sampler.hpp`, the pivot bracket makes a bind position go straight through them); only the vertices of the batches the geoset filter draws are skinned, NEON, up to four bones a vertex. Each animated character has its own vertex buffer (a slot, two buffers a slot so the GPU never reads the one being written), always drawn from offset 0. Re-skinned every frame under 6 yards, every 2nd up to 15, every 3rd beyond; in between it draws the buffer written last.
- **GPU skinning was not tried:** the human needs up to 46 bones in one batch and 138 in all, over what a 128-vec4 vertex uniform budget holds even at two vec4 a bone, and the vertices reference global bone indices (per-batch remapping would duplicate vertices). CPU skinning with the budget above is the design until a measurement says otherwise.
- **Measured on the device** (Goldshire, 3-4 characters animated, 4,000 vertices a frame): animation 3.7 ms a frame (bones 0.6, skinning 1.8, upload 1.1 in 15 `glBufferSubData` calls), about 30 fps; the frame rate without animation was 35. The `GL memory Characters` log line carries the split. First versions: 9.9 ms (glm per vertex), 6.8 (NEON with scalar operands), 0.45 microseconds a vertex now: **moving a float between the VFP and NEON units costs a Cortex-A9 about twenty cycles**, so the loop keeps the weights, positions and normals in NEON registers (`vmlaq_lane_f32`, vector stores). Ranges of drawn vertices closer than 48 are merged into one upload.
- **Two bugs worth knowing.** (1) A shared vertex buffer with a different start offset for each character (`glVertexAttribPointer` with `base + offsetof`) drew every character but the first one distorted into a cone on the Vita (found by the user noticing that the closest character was always right); one buffer per slot at offset 0 fixed it. (2) Eyelid blink bones (global-sequence scale between 0 and 1) are in every human: a bone matrix with a zero determinant is normal.
- **Live switches** (`gl.cfg`): `noanim` draws everyone from the baked pose, `animall` skins up to the slot count regardless of distance. **GPU pools are at 0-1 MB free in all three** (vram 0/96, RAM 0/25, contiguous 1/26) with the slot buffers (8 x 256 KB) and 9-10 MB of character textures: the next feature needs memory freed first.
- **Build trap:** this code growth needed `-DWOWEE_VITA_TEXT_PAD_KB=72` (the cache holds the number; DEV_SETUP 23).

### 33.2 The heap size decides the GPU RAM pool (VITA-20, measured on the device)

- **Every MB less of newlib heap is a MB more for vitaGL's RAM pool** (the extended memory mode gives the app a fixed amount; newlib takes its heap first and `vglInitExtended` takes what is left minus the 24 MB threshold). `WOWEE_VITA_CLIENT_HEAP_MB` 288 -> 256: RAM pool 25.6 -> 57 MB, and **the physically contiguous pool is no longer spent at start** (4 of 26 MB free at the login screen before, 25 of 26 now: vitaGL's own buffers had spilled into it). Free GPU memory at the login screen: 92 MB -> 128 MB; in Goldshire with all the characters: vram 0, RAM 8 of 57, phycont 25 of 26 free, **33 MB against about 3 MB**. No reinstall needed (the extended mode is already in the VPK); the frame rate is the same.
- **The cost is heap headroom:** the heap peaked at 223 MB (in use) of the 256 MB arena in Goldshire with 20 characters; Stormwind-sized areas, FrameXML (does not fit anyway) and more models will eat into the 33 MB left. If a crash with `bad_alloc` or Lua "not enough memory" appears, this is the first number to suspect: raise `WOWEE_VITA_CLIENT_HEAP_MB` by 16 (cmake cache variable; the pool shrinks by the same).
- **vitaGL itself uses about 54 MB at start** (CDRAM 12 MB, RAM 15 MB, phycont 22 MB at the old heap size): sceGxm's parameter buffer (default 16 MB), vertex ring, display buffers, depth buffer, the shader patcher. `WOWEE_GXM_PARAM_MB` and the other `WOWEE_GXM_*` knobs shrink them; not tried yet (a too small parameter buffer drops geometry in busy scenes).

### 33.3 Texture memory (VITA-20, measured on the device)

- **Most texture memory was decoded RGBA8, and the size cap never touched it.** `TextureCache` cut only block-compressed (DXT) textures; palettised and uncompressed BLPs arrive decoded as RGBA8 and were uploaded as they were: **17.8 of the building cache's 20.7 MB were 53 RGBA textures, and 8.2 of the characters' 9.2 MB were 8 baked NPC textures of 1 MB each** (the log lines `GL memory WMO/M2/Characters` now say `RGBA N KB in M`).
- **Now** (`uploadBlp`, `maxDimension`): an RGBA image is halved (2x2 box) until it fits the cap, stored as **16 bits a texel (RGB565 when every alpha is 255, RGBA4444 otherwise)** and given a mip chain (they had none, so they shimmered). Measured: building textures **28.6 MB -> 5.2 MB** (cap 128), characters **9.3 -> 2.1 MB**, free GPU memory in Goldshire **vram 22 / RAM 16 of 57 / phycont 25** (63 MB; 33 MB before this step, about 3 MB before DEV_SETUP 33.2). No visible change on the church wall, the NPCs' skin or clothes; no colour swap.
- **Caps** (`env.txt`): `WOWEE_WMO_TEXTURE_MAX` (buildings, default **128**; 256 costs 14 MB more for no change I could see at Goldshire), `WOWEE_TEXTURE_MAX` (doodads, 256), `WOWEE_CHAR_TEXTURE_MAX` (characters, 256).
- **A crash seen once** with the 128 cap on the first run (21:43, a data abort in a worker thread inside newlib's `mallinfo` path at world entry, heap 187 MB of 256, `psp2dmp` kept on the card); the same build ran clean twice afterwards and the cap code reads safely, so it is logged here as an unexplained intermittent: if it comes back, check for a race around the `{heap}` log suffix (`WOWEE_LOG_HEAP=1` calls `mallinfo` from every thread's log line).

### 33.4 Terrain tiles only while something of them can be seen (VITA-20)

- `TerrainManager` (shared file, `__vita__` arms in `streamTiles()` and `update()`, `vitaTileOutOfReach()`): a tile is queued only when some part of it is within **95% of the view distance + 40 yards** of the camera (`WOWEE_VIEW_DISTANCE`, default 500, as the renderer's fog: everything past it is fog colour); a loaded tile 150 yards further out than that (hysteresis) is queued for unload; the camera's own tile never is. The old 3x3 square is the upper bound.
- **Measured at the Goldshire stalls:** 8 tiles instead of 9: terrain GPU buffers 32.5 -> 28.7 MB, building buffers 12.1 -> 6.3 MB (the far tile's buildings are not loaded either), heap peak 229 -> 219 MB (steady 207), free GPU memory **vram 32 / RAM 17 of 57 / phycont 25 = 74 MB**, 32 fps. The picture shows no holes when turning round (user, on the device). **Not tested:** walking across tile borders (the loads and unloads while moving), and a larger `WOWEE_VIEW_DISTANCE`.

### 33.5 Two bugs found by walking into Elwynn (VITA-20, measured on the device)

- **The UI stalled for 0.2 to 1.5 s while creatures and terrain loaded.** Timing the sections of `GameScreen::render` showed it was code between the cooldown tracker and the DPS meter: `addons::storedCVarValue()`, which the nameplate pass and the combat-text check call every frame, **opened and parsed `cvars.cfg` from the memory card on every call** whenever the key was not in the in-memory store (a setting nobody has set). On the Vita that open queues behind the asset loaders' reads and blocks the UI thread. The result is now memoized per key until the file changes (`g_cvarFileRevision`, bumped by load and save), in `src/addons/lua_system_api.cpp` (shared file; the fix helps every platform, an upstream candidate). After it no in-world UI frame was slow in the log.
- **Some big trees did not block the player.** `ELWYNNTREECANOPY01` ships a 52-triangle collision mesh of the trunk (a 14 x 16 yard shell, local z from -4 to 22), but the *instance's broad-phase box* comes from `getTightCollisionBounds()`, which only knows the visible vertices of the model, and a canopy has none below 3 yards above the ground: the query rejected the instance in its vertical test (`from.z + 2.5 < worldBoundsMin.z`) for anyone standing on the ground. The Vita's `mirrorCollisionInstance` / `setInstanceTransform` now widen the box with the authored mesh's own extents (`collisionLocalBounds` in `m2_renderer_gl.cpp`). Upstream's Vulkan renderer builds the same box the same way, so desktop has the same fault (an upstream candidate). Found with a one-shot diagnostic, now kept: put `where` in `ux0:data/wowee/gl.cfg` (it fires when the word appears) and the log lists the doodads within 8 yards of the camera with their model name, collision triangles, and the collision mesh against your feet; delete the file afterwards.
- Also learned: a model **with** an authored collision mesh collides through that mesh whatever the classifier says (`collisionNoBlock` is only consulted without one), so changing the name classification of a tree (I tried `canopyless`) cannot give or take its collision.

### 33.6 Loading off the main thread, a tile budget, and a regression of mine (VITA-20, measured on the device)

- **An out-of-memory crash while standing still** (`C2-12828-1`, `bad_alloc` at 260 MB of a 256 MB heap): 13 terrain tiles were resident. The reach rule of 33.4 loaded only what is within fog range, but a tile stayed until it was 150 yards past that, and with the 3x3 square, the corner cases and the walk across tile borders that kept up to 13 alive (about 10 MB of heap each). **Now at most 9 tiles are resident** (`kVitaMaxResidentTiles`; a tile queued only if `loaded + pending < 9`) and a tile unloads 60 yards after it leaves reach.
- **Asynchronous textures** (`gl::TextureCache::setAsync`, on for terrain, doodads and buildings): `get()` no longer reads a file. It returns a GL texture name at once (a 1x1 grey placeholder) and a worker (`ThreadPool::ioWorkers`) reads and decodes the BLP; `pump(2 ms)` in each renderer's `glRender` uploads it INTO THE SAME NAME (`uploadBlp(..., reuse)`), so everything that kept the name shows the real texture afterwards. A missing file still answers white (`fileExists` is a manifest lookup). Reading a BLP from the card on the main thread cost 25-280 ms and a tile or a crossroads asks for dozens: the log's `TERRAIN took 2212 ms`, `M2_MODELS took 1467 ms` and the 1-2 s `spawn/equipment queues` steps (gameobject models at the crossroads) were mostly that. **Characters** use their own version (`loadTexture` returns an object that draws nothing while `pending`, `update()` uploads within 3 ms a frame) and **the Stand pose of a model is skinned on a worker** (`computeBoneMatrices` and `skinVertices` are pure functions; the model draws nothing until it is uploaded; `unloadModelIfUnused` keeps a model until its bake is done, `clear()` waits for the workers). After the change the stutter after entering a new area was "better, a bit stuttery": 25-36 fps with single hitches of 100-900 ms (a creature spawn, `renderer->update`) left.
- **Walk-through buildings were my regression.** `WMORenderer::Gl::buildCollisionModelUnchecked` refused to start a building with less than 60 MB of heap free (a number for the 288 MB heap); with 256 MB and about 200 MB in use at world entry the blacksmith and the inn of Goldshire were skipped, and a skipped building was never tried again. Now 32 MB (it still stops below 25 MB mid-way), and a build that gave up for memory is retried after about 20 seconds (`Model::retryTick`).
- **Not done:** the 270 ms copy of a human's `M2Model` (156 sequences of bone tracks) when it is first registered, the remaining 100-900 ms single hitches, an unexplained rare worker-thread crash in `mallinfo` (33.3).

### 33.7 The player model and skin composition (phase C, VITA-20)

- **The player is drawn** (third person by default, hidden in first person by the camera controller as upstream). `Application::spawnPlayerCharacter` runs unchanged (no Vita arm any more): it loads the race and gender model as id 1, creates the instance, hands the camera over (`setCharacterFollow`) and the world loader sets the real position right after. The earlier idea of waiting for the position-only camera attach (`Renderer::update`) broke it: the spawn is called once, before the first terrain tile, so it never ran. With an instance the position-only character never attaches (it only does while the controller has no follow target).
- **`syncPlayerModel()`** (`renderer_gl.cpp`, file-local): each frame the instance takes the controller's position and facing (travel yaw while strafing, with the torso turned toward the facing, as upstream) and plays what the controller reports: Stand, Run (Walk when a model lacks it), WalkBackwards, ShuffleLeft/Right, Jump, Fall, Swim, SwimIdle. Upstream does this in `AnimationController::updateCharacterAnimation`, which also carries emotes, mounts, combat stances, spell casting and weapons: none of that is drawn yet (the Vita's `AnimationController` is still the stub file).
- **Skin composition** (`character_composite.cpp`, upstream's blending copied because `character_renderer.cpp` is Vulkan-only): base skin + face and underwear overlays + the eight equipment regions, 256 base atlases doubled when equipment is to be placed, the magenta "nothing here" key bled out. It runs on a worker (`ioWorkers`); the result goes up through `uploadBlp`'s compact RGBA path (565/4444, mips, cap 256). The returned `GpuTexture` is `pending` until then and draws **the texture it replaces** (`fallback`, set by `setTextureSlotOverride`) or nothing, so an equipment change does not blink the body. Humanoid NPCs get faces and clothes the same way.
- **Heap:** the world peaked at 239 MB of 256 after this (composites are 0.25 to 1 MB of RGBA each while blended, plus the decoded layers): raise `WOWEE_VITA_CLIENT_HEAP_MB` by 16 if `bad_alloc` comes back.
- **Not done:** weapons, helms, shoulders and cloaks (phase D), mounts, the Vita's emotes and combat animations, character select preview, hair/facial-hair variants beyond what `buildDefaultPlayerGeosets` picks.

### 33.8 Weapons and equipment on characters (phase D, part 1, VITA-20)

- **Attached items** (`attachWeapon`, `detachWeapon`, `getAttachmentTransform`, `removeInstance` taking its attachments with it): a weapon, helm or shoulder is an instance of its own model with `hasOverrideModelMatrix` and no position. Each frame `glRender` places it on a bone of its parent (`charModel * bone * T(offset) * localTransform`, upstream's formula; the bone is found as upstream does it: attachment lookup, then attachment id, then the key bones 26/27 for the hands, bone 0 for the head) and draws it with the parent. The bones are the character's own while it is being skinned (the animated slots) and the Stand pose's otherwise (`Gl::standBones`, kept by the Stand-pose bake). An item's additive batches (glow, flames) are skipped until effects are drawn. Not done: enchant glows (`attachWeaponEffect`), the sheath push, sheathing animations.
- **NPC weapons are on again** and no longer read on the main thread: `tryAttachCreatureVirtualWeapons` read and parsed the weapon M2 on the main thread (200 to 800 ms on the card the first time). On the Vita the first attempt for a weapon starts the read on a worker (`EntitySpawner::weaponM2Cache_`, at most 32 kept) and answers "not yet"; the retry loop the shared code already has attaches it a moment later.
- **The memory card is one queue** (measured): `AssetManager::resolveFile` falls back to a `stat()` of the card for any path that is not in the manifest, and the main thread asks `fileExists` for optional and missing files all the time (candidate textures, weapon paths, ADT tiles that were out of reach and so never marked as failed). While workers streamed reads each of those waited 100 to 500 ms (`streamTiles` took 0.5 s). The answer "not there" is now remembered (Vita only), and `streamTiles` does its cheap tests (reach, resident budget) before it asks about the file.
- **What is left of the stutter** at the Elwynn crossroads (about 20 fps for the first 80 to 100 seconds after arriving, then 34): single stalls of 100 to 960 ms in `beginFrame` and `endFrame` (the buffer swap waiting on the GPU or the driver), `addonWidgets` (Lua), `renderer->update`. No single texture upload takes more than 25 ms. It happens while many things load at once and goes away by itself; the cause on the GPU side is not found (needs a GPU profile; the first thing to try would be fewer simultaneous uploads).

### 33.9 The memory card is one queue: finding and removing the stalls (VITA-20, measured on the device)

The stutter at the Elwynn crossroads (about 20 fps for the first 80 to 100 seconds, stalls of 100 to 1000 ms in whatever stage ran) was never GPU or Lua: **the main thread was waiting on the memory card behind the worker threads' reads.** Method and result, because the same trap will come back:

- **Find it with the trace.** `WOWEE_IO_TRACE_MS=20` in env.txt logs every sceIo call the main thread makes that takes longer, with its path (`src/platform/vita/vita_io_trace.cpp`; the calls are caught with `-Wl,--wrap=sceIo*`, so newlib, the logger and `std::filesystem` all show up). Stage timers say where a frame stalled, not what it waited for. The first run, in a two-minute walk: **142 slow calls, 24 s of blocking** (30% of the main thread's time in the worst 10 s): 74 log writes (11 s, up to 1.1 s each), 38 stats (5.8 s), one-time DBC reads. A trace that logs from inside the wrapper deadlocks on the logger's own lock (it froze the shader screen once): reports are queued and written by the poll thread.
- **What fixed it, in order of effect:** (1) the log file is written by a thread of its own (`asyncLogWrite`, `vita_async_log.cpp`; the logger's Vita arm); (2) `AssetManager::resolveFile`: a path outside the manifest and a path in the **override directory** are stat()ed once (answers remembered), and **the override directory is checked for existence once** (`expansions/wotlk/override/` is not on this card; every lookup stat()ed into it); (3) the diagnostic polls (gl.cfg, shot.cmd) moved off the main thread (`beginFrame` and `endFrame` stalls of 100 to 960 ms); (4) **a gate for the workers**: every sceIo call of a thread other than the main thread takes a mutex, and a worker's read is cut into 64 KB pieces with the gate released between them, so at most one worker piece is ever in the card's queue ahead of the main thread; (5) `LooseFileReader::readFile` reads with sceIoOpen/Lseek/Read instead of an `ifstream`, which on the Vita's C library `stat()`s the file it just opened: one directory lookup less per file (a lookup in a big directory, such as the weapon models, is about 340 ms the first time).
- **Result** (same walk): slow card calls 142 -> 23 (and 21 of those are the one-time loads at world entry: `spell.dbc` 3.8 s, `itemdisplayinfo.dbc` 0.5 s, `WoW.exe` 0.6 s), **stage stalls of 100 ms or more 44 -> 4**, two of them at world load; frame rate 26-35 fps from the first minute, no 20 fps phase.
- **Not done:** the three one-time reads at world entry (about 5 s on the loading screen, on the main thread); `spell.dbc` is the one to move to a worker if the loading time matters.
- **Also learned:** the ELF padding needed `WOWEE_VITA_TEXT_PAD_KB=88` after this code growth (it is the default in `wowee_client.cmake` now).
