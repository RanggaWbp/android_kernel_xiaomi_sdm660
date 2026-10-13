#!/usr/bin/env bash
# Apply the Linux 4.4 compatibility shim to the NoMount submodule.
#
# The NoMount submodule tracks maxsteeel/nomount upstream and must stay
# pristine -- we never fork it. Upstream targets modern kernels; this tree
# is 4.4, so the shim lives here as a patch and is applied at build time.
#
# Idempotent: re-running on an already-patched submodule is a no-op.
#
# Usage: scripts/apply_nomount_compat.sh [submodule-path]
set -euo pipefail

SUB="${1:-NoMount}"
PATCH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/scripts/nomount/nomount-4.4-compat.patch"

[ -d "$SUB/kernel/src" ] || { echo "::error::$SUB/kernel/src missing (submodule not checked out?)"; exit 1; }
[ -f "$PATCH" ] || { echo "::error::patch not found: $PATCH"; exit 1; }

if git -C "$SUB" apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "NoMount 4.4 compat patch already applied -- skipping."
    exit 0
fi

if ! git -C "$SUB" apply --check "$PATCH" 2>/dev/null; then
    echo "::error::NoMount 4.4 compat patch does not apply cleanly to $SUB"
    echo "Upstream moved -- regenerate with: git -C $SUB diff > patches/nomount-4.4-compat.patch"
    exit 1
fi

git -C "$SUB" apply "$PATCH"
echo "Applied NoMount 4.4 compat patch to $SUB ($(grep -c '^+' "$PATCH") added lines)."
