# Shared by tools/vita/*.sh (sourced, not executed).
# Pin a dated tag for reproducible builds; see docs/vita/DEV_SETUP.md before bumping it.
VITASDK_IMAGE="${VITASDK_IMAGE:-vitasdk/vitasdk:2026.08-20260925}"
VITA_ROOT=$(cd "$(dirname "$0")/../.." && pwd)

# macOS uses Apple's `container` CLI, everything else docker or podman. Override: CONTAINER_RUNTIME.
vita_runtime() {
    if [ -n "${CONTAINER_RUNTIME:-}" ]; then echo "$CONTAINER_RUNTIME"
    elif [ "$(uname -s)" = Darwin ]; then echo container
    elif command -v docker >/dev/null 2>&1; then echo docker
    elif command -v podman >/dev/null 2>&1; then echo podman
    else echo "no container runtime found (container, docker or podman)" >&2; return 1; fi
}

# Path relative to the repo root (the container only sees the repo).
vita_rel() { python3 -c 'import os,sys;print(os.path.relpath(os.path.realpath(sys.argv[1]),sys.argv[2]))' "$1" "$VITA_ROOT"; }
