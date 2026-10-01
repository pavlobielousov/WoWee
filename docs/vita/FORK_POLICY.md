# Vita Fork Policy

The PlayStation Vita port lives in a fork (`pavlobielousov/WoWee`) of `Kelsidavis/WoWee`.
Upstream releases several times a month, refactors heavily, builds with `-Werror` and runs
200+ CTest tests. If the port spreads edits through shared files, every upstream pull becomes a
hand merge and the port falls behind for good. These rules keep the diff small, isolated and
easy to rebase. Every other Vita work item follows them.

## Branch model

| Branch | Purpose | Rules |
|---|---|---|
| `master` | Mirror of `upstream/master`. | Fast-forward only. Never commit to it. |
| `vita` | Integration branch for the port. | Based on an upstream release tag. Merge `upstream/master` into it; do not rebase it. |
| `issue/vita-<N>-<brief-title>` | One work item each. | Branch from `vita`, open a PR into `vita`. |

Feature branches follow the item code, e.g. `issue/vita-5-32-bit-clean`. One item per branch.

### Sync cadence

Merge `upstream/master` into `vita` at least once per upstream release (they tag `v3.1.x`).
Merge rather than rebase, so `vita` history stays stable for anyone who has branched from it.

```sh
git fetch upstream --tags
git switch master && git merge --ff-only upstream/master      # keep the mirror exact
tools/vita/upstream_diff_report.sh vita                       # note the shared-file footprint first
git switch vita && git merge <upstream tag>
```

Record each sync in the table below (tag, date, shared-file hunks reported by the script before
the merge, conflicts hit). A rising hunk count is the early warning that rule 2 is slipping.

| Date | Upstream tag | Shared files / hunks | Conflicts |
|---|---|---|---|
| 2026-10-02 | `v3.1.41` (`60af39c9`) | 0 / 0 (`vita` created from the tag) | none |

## Rules

1. **New code goes in new files.** Vita-only code lives under `src/platform/vita/`,
   `include/platform/vita/`, `src/rendering/gl/` (or the directory the renderer ADR, VITA-11,
   picks), `cmake/vita/`, `resources/vita/`, `tools/vita/`, `docs/vita/`, plus the CI file
   `.github/workflows/vita.yml`. Paths in this list cannot conflict with upstream.
2. **Shared files get minimal, gated edits.** Use `#if defined(__vita__)` (the define the
   VitaSDK GCC sets) or the CMake-provided `WOWEE_PLATFORM_VITA`. Add an arm to an existing
   `#if` ladder (`window.cpp`, `main.cpp`, `net_platform.hpp`, `memory_monitor.cpp`,
   `config_paths.cpp`, ...) rather than restructuring it. The arm calls into a Vita file; the
   logic does not live in the shared one.
3. **No drive-by reformatting, renames or reordering** in upstream files, and no moving code
   between files in a change that also edits it. Keep hunks small so `git merge` resolves them
   automatically.
4. **CMake:** the root `CMakeLists.txt` gets one hook that includes `cmake/vita/Vita.cmake`
   when building with the VitaSDK toolchain, as close to
   `if(VITA) include(cmake/vita/Vita.cmake) return() endif()` as practical. All Vita target
   logic lives in `cmake/vita/`. Upstream workflows are not edited; Vita CI is the new file
   `.github/workflows/vita.yml`.
5. **Platform-neutral fixes go upstream.** Anything that also helps upstream (32-bit fixes,
   thread-count helper, headless core, Vulkan-free UI, mip skipping, memory budget callbacks,
   data-tool options) is labelled `upstream-candidate` and offered to `Kelsidavis/WoWee` as a
   small PR once it is stable here. A fix that lands upstream is a fix we never merge again.
   Upstream prefers small, measured, evidence-backed changes (see `docs/plan-android.md`);
   write those PRs the same way.
6. **Every PR into `vita` states which upstream (shared) files it touches and why.** A PR that
   touches any shared file is marked `touches-shared`.

The desktop build must be unchanged by every Vita change: it still builds with `-Werror` and
`ctest` still passes.

## Measuring the footprint

```sh
tools/vita/upstream_diff_report.sh            # HEAD against upstream/master
tools/vita/upstream_diff_report.sh vita       # the integration branch
```

It lists changed files split into "Vita-only paths" and "shared files", with hunk counts, from
the merge base so new upstream commits are not counted as ours. The vita-only list in the script
mirrors rule 1; update both together.

On day one both lists are empty.

## Still to do

- CI guard: fail a PR that touches a shared file without the `touches-shared` label. Comes with
  VITA-31, because it needs the Vita workflow to live in.
- Labels (`touches-shared`, `upstream-candidate`) exist only once the repo has them for PRs.
