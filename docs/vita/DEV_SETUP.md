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
`build-vita/wowee.vpk`, title ID `WOWE00001`, a stub that writes `WoWee Vita` to `ux0:data/wowee/wowee.log`.
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
2. **UDP net-logger** (optional): if `ux0:data/wowee/loghost.txt` contains `<ip>[:port]` (default
   port 9999), each line is also sent as one UDP datagram. On the dev machine: `tools/vita/logsink.sh`
   (a thin `nc -ul`). UDP is lossy by design; the file is the source of truth.

`tools/vita/devcheck/main.c` is the reference implementation; the WoWee logger arm (a Vita arm in
`src/core/logger.cpp` calling code under `src/platform/vita/`) comes with the platform work.

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
on the first run. Follow-up implementation: VITA-38.

Measured 2026-10-02 (Apple Silicon Mac, `container` VM with 10 GB / 8 CPUs; clean state):

| Step | Result |
|---|---|
| Image `wowee-armhf` (`tools/vita/check32/Dockerfile`, on top of `wowee-desktop-builder`) | 44 s to build. gcc/g++ 13.3 cross compilers, `qemu-user`, armhf OpenSSL and zlib (ports.ubuntu.com via `dpkg --add-architecture armhf`), SDL3 built for armhf without a video backend. Image size not measured. |
| Cross-configure of the whole tree (`-DWOWEE_BUILD_TESTS=ON`) | about 2 s |
| Build of all 212 `test_*` targets | about 2 min (8 CPUs, cold) |
| `ctest -j8` under `qemu-arm` (without `sweep_guard`) | 17 s wall; slowest `settings_apply_on_load` 16 s, `rt_bvh` 5 s |
| Result | **213 of 217 pass**; the 4 failures are listed below |
| Sanity | binaries are `ELF32 ARM`, `sizeof(size_t) == 4`; running one directly fails with "Exec format error", so the emulator is needed |

What the 4 failures are:
- `warden_reloc`: **a real 32-bit bug.** `wardenRelocTargetFits` (`include/game/warden_module.hpp:70`) computes `static_cast<size_t>(target) + 4u <= moduleSize`; with a 32-bit `size_t`, `target = 0xFFFFFFFF` wraps to 3 and passes the bounds check, so an attacker-supplied Warden relocation could write at `image + 0xFFFFFFFF`. The test already covers it; it only fails on a 32-bit build. Not fixed in this item (Warden is dropped on Vita, but the code is shared and affects any 32-bit build): tracked as VITA-39.
- `extract_progress`: needs StormLib, which the image does not have for armhf (and the Vita does not need it).
- `framexml_compiles`, `addon_xml_compiles`: not a 32-bit result. Their helper `framexml_compile_check` is not a `test_*` target, so the spike never built it, and they need `Data/interface`, which is absent (skipped, exit 77, on a desktop with data).
- `sweep_guard` was excluded: it runs host Python sweeps, is not 32-bit relevant and exceeds a 120 s timeout there.

Reproduce (from the WoWee root; `desktop_check.sh image` first if `wowee-desktop-builder` is missing):

```sh
container build -f tools/vita/check32/Dockerfile -t wowee-armhf tools/vita/check32
container run --rm -i --memory 10G --cpus 8 --entrypoint /bin/bash -v "$PWD:/workspace" wowee-armhf -s < tools/vita/check32/run_spike.sh
```

Pitfalls found (all are encoded in the files above):
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
