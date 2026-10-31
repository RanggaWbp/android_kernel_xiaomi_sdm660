#!/usr/bin/env bash
# Enable the priority UID filter command in the KernelSU/BakaSU submodule.
#
# Upstream BakaSU does not have the CMD_SUSFS_SET_PRIORITY_UID_FILTER
# dispatch case; it comes from the SUSFS 4.4 patch (JackA1ltman).
# The submodule tracks Baka-SU/BakaSU and stays pristine; the change
# lives in scripts/ksu/ksu-susfs-priority-uid-filter.patch and is applied
# at build time.
#
# Idempotent: re-running on an already-patched submodule is a no-op.
#
# Usage: scripts/apply_ksu_susfs_priority_uid_filter.sh [submodule-path]
set -euo pipefail

SUB="${1:-KernelSU}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCH="$ROOT/scripts/ksu/ksu-susfs-priority-uid-filter.patch"
TARGET="$SUB/kernel/supercall/dispatch.c"

[ -f "$TARGET" ] || { echo "::error::$TARGET missing (submodule not checked out?)"; exit 1; }
[ -f "$PATCH" ] || { echo "::error::patch not found: $PATCH"; exit 1; }

if grep -q 'CMD_SUSFS_SET_PRIORITY_UID_FILTER' "$TARGET"; then
    echo "Priority UID filter dispatch already applied -- skipping."
    exit 0
fi

if ! git -C "$SUB" apply --check "$PATCH" 2>/dev/null; then
    echo "::error::priority-uid-filter patch does not apply cleanly to $SUB"
    echo "Upstream moved -- regenerate with: git -C $SUB diff > scripts/ksu/ksu-susfs-priority-uid-filter.patch"
    exit 1
fi

git -C "$SUB" apply "$PATCH"
echo "Applied priority UID filter dispatch in $SUB."
