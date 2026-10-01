#!/usr/bin/env bash
# How far the Vita fork has drifted from upstream, split by where the change is.
#
# Vita-only paths are ours and cannot conflict with upstream. Shared files are
# upstream's, and every hunk in one is a potential merge conflict, so the shared
# list is the number to keep small. Run it before each upstream sync
# (docs/vita/FORK_POLICY.md).
#
#   tools/vita/upstream_diff_report.sh              HEAD against upstream/master
#   tools/vita/upstream_diff_report.sh vita         another ref against it
#   UPSTREAM=upstream/v3.1.41 tools/vita/upstream_diff_report.sh
#
# Compares from the merge base, so commits upstream has made since the last sync
# are not reported as ours. Exit status is 0 whatever it finds; it reports, it
# does not gate (the CI guard for that comes with VITA-31).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"

ref="${1:-HEAD}"
upstream="${UPSTREAM:-upstream/master}"

# Keep in step with rule 1 of docs/vita/FORK_POLICY.md.
vita_paths=(
  src/platform/vita/
  include/platform/vita/
  src/rendering/gl/
  cmake/vita/
  resources/vita/
  tools/vita/
  docs/vita/
  .github/workflows/vita.yml
)

if ! git rev-parse --verify --quiet "$upstream^{commit}" >/dev/null; then
  echo "no such ref: $upstream (git remote add upstream https://github.com/Kelsidavis/WoWee.git && git fetch upstream --tags)" >&2
  exit 1
fi
base="$(git merge-base "$upstream" "$ref")"

is_vita_only() {
  local f="$1" p
  for p in "${vita_paths[@]}"; do
    case "$f" in "$p"*) return 0 ;; esac
  done
  return 1
}

vita_rows=()
shared_rows=()
shared_hunks=0
vita_files=0
while IFS=$'\t' read -r added removed file; do
  [ -n "$file" ] || continue
  hunks="$(git diff -U0 "$base" "$ref" -- "$file" | grep -c '^@@' || true)"
  row="$(printf '%5s hunk(s)  +%-5s -%-5s %s' "$hunks" "$added" "$removed" "$file")"
  if is_vita_only "$file"; then
    vita_rows+=("$row")
    vita_files=$((vita_files + 1))
  else
    shared_rows+=("$row")
    shared_hunks=$((shared_hunks + hunks))
  fi
done < <(git diff --numstat --no-renames "$base" "$ref")

echo "ref:        $ref ($(git rev-parse --short "$ref"))"
echo "upstream:   $upstream ($(git rev-parse --short "$upstream"), $(git describe --tags --always "$upstream"))"
echo "merge base: $(git rev-parse --short "$base") ($(git describe --tags --always "$base"))"
echo
echo "Vita-only paths: $vita_files file(s)"
[ ${#vita_rows[@]} -eq 0 ] || printf '  %s\n' "${vita_rows[@]}"
echo
echo "Shared files: ${#shared_rows[@]} file(s), $shared_hunks hunk(s)"
[ ${#shared_rows[@]} -eq 0 ] || printf '  %s\n' "${shared_rows[@]}"
