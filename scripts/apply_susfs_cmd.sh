#!/usr/bin/env bash
# Apply the susfs CMD dispatch case to the KernelSU submodule.
#
# The KernelSU (BakaSU) submodule tracks Baka-SU/BakaSU upstream and must
# stay pristine -- we never fork it. The sus_path_uid_filter CMD
# (CMD_SUSFS_SET_SUS_PATH_UID_FILTER) is handled in
# kernel/supercall/dispatch.c, which upstream does not carry for this
# tree, so the case lives here as a patch applied at build time.
#
# Idempotent: re-running on an already-patched submodule is a no-op.
#
# Usage: scripts/apply_susfs_cmd.sh [submodule-path]
set -euo pipefail

SUB="${1:-KernelSU}"
PATCH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/scripts/susfs/susfs-dispatch-4.4.patch"

[ -f "$SUB/kernel/supercall/dispatch.c" ] || { echo "::error::$SUB/kernel/supercall/dispatch.c missing (submodule not checked out?)"; exit 1; }
[ -f "$PATCH" ] || { echo "::error::patch not found: $PATCH"; exit 1; }

if git -C "$SUB" apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "susfs CMD dispatch patch already applied -- skipping."
    exit 0
fi

if ! git -C "$SUB" apply --check "$PATCH" 2>/dev/null; then
    echo "::error::susfs CMD dispatch patch does not apply cleanly to $SUB"
    echo "Upstream moved -- regenerate with: git -C $SUB diff > scripts/susfs/susfs-dispatch-4.4.patch"
    exit 1
fi

git -C "$SUB" apply "$PATCH"
echo "Applied susfs CMD dispatch patch to $SUB ($(grep -c '^+' "$PATCH") added lines)."
