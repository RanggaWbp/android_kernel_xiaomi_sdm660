#!/usr/bin/env bash
# Keep the KernelSU/BakaSU submodule's SUSFS build enabled.
#
# Upstream commit 48fa4bb7 ("kernel: suspend susfs build") makes
# kernel/tools/susfs_compat.mk end with an unconditional
#
#     $(error Please wait susfs update.)
#
# Kbuild includes that file only when CONFIG_KSU_SUSFS is set, so with SUSFS
# enabled the build stops dead -- no source is even compiled. The guard exists
# because simonpunk's SUSFS has not caught up with the newest KernelSU hooks,
# not because the combination cannot compile here.
#
# The submodule tracks Baka-SU/BakaSU and stays pristine; the one-line change
# lives in scripts/ksu/ksu-susfs-unsuspend.patch and is applied at build time.
#
# Idempotent: re-running on an already-patched submodule is a no-op.
#
# Usage: scripts/apply_ksu_susfs_unsuspend.sh [submodule-path]
set -euo pipefail

SUB="${1:-KernelSU}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCH="$ROOT/scripts/ksu/ksu-susfs-unsuspend.patch"
TARGET="$SUB/kernel/tools/susfs_compat.mk"

[ -f "$TARGET" ] || { echo "::error::$TARGET missing (submodule not checked out?)"; exit 1; }
[ -f "$PATCH" ] || { echo "::error::patch not found: $PATCH"; exit 1; }

if ! grep -q 'error Please wait susfs update' "$TARGET"; then
    echo "KernelSU SUSFS guard already neutralised -- skipping."
    exit 0
fi

if ! git -C "$SUB" apply --check "$PATCH" 2>/dev/null; then
    echo "::error::SUSFS patch does not apply cleanly to $SUB"
    echo "Upstream moved -- regenerate with: git -C $SUB diff > scripts/ksu/ksu-susfs-unsuspend.patch"
    exit 1
fi

git -C "$SUB" apply "$PATCH"
echo "Neutralised the KernelSU SUSFS build guard in $SUB."
echo "NOTE: SUSFS is now compiled against KernelSU hooks upstream considers"
echo "      unverified. A mismatch shows up at runtime, not at build time."
