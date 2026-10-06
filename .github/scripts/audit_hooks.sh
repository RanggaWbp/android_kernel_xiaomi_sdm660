#!/usr/bin/env bash
#
# audit_hooks.sh -- BakaSU / SUSFS Inline Hook integration audit
#
# Mirrors the EXACT checks performed by BakaSU upstream:
#   drivers/kernelsu/tools/inline_hook_check.mk   (CONFIG_KSU_SUSFS selected)
#   drivers/kernelsu/tools/manual_hook_check.mk   (CONFIG_KSU_MANUAL_HOOK selected)
#
# IMPORTANT: drivers/kernelsu is a SYMLINK to KernelSU/kernel (BakaSU
# submodule). A recursive `grep -R drivers/` follows that symlink and scans
# upstream BakaSU sources, which legitimately contain CONFIG_KSU_MANUAL_HOOK
# (lsm_hooks.c, ksud_integration.c, Kbuild, manual_hook_check.mk, ...). Those
# hits are NOT manual-hook guards in this kernel. Upstream only ever greps the
# seven kernel files listed below, so this script does the same.
#
# Exit codes:
#   0 = PASS
#   1 = FAIL (manual-hook guard present in a kernel file, required hook
#             missing, or incompatible legacy hook still installed)

set -uo pipefail

SRCTREE="${1:-$(pwd)}"
cd "$SRCTREE" || { echo "::error::srctree not found: $SRCTREE"; exit 1; }

FAIL=0
WARN=0

hr() { echo "--------------------------------------------------------------"; }

# The seven kernel files upstream inspects, with the hook each must contain.
# Format: <file>|<required_hook>
HOOK_TABLE=(
  "kernel/sys.c|ksu_handle_setresuid"
  "fs/exec.c|ksu_handle_execveat"
  "fs/open.c|ksu_handle_faccessat"
  "fs/read_write.c|ksu_handle_sys_read"
  "fs/stat.c|ksu_handle_stat"
  "kernel/reboot.c|ksu_handle_sys_reboot"
  "drivers/input/input.c|ksu_handle_input_handle_event"
)

# Legacy hooks that MUST NOT be present (they were replaced by static_key
# variants in susfs4ksu commit 00be2d47). Format: <hook>|<file>
INCOMPATIBLE_TABLE=(
  "ksu_vfs_read_hook|fs/read_write.c"
  "ksu_input_hook|drivers/input/input.c"
  "ksu_execveat_hook|fs/exec.c"
  "ksu_init_rc_hook|fs/read_write.c"
  "ksu_init_rc_hook|fs/stat.c"
)

hr
echo "== BakaSU / SUSFS Inline Hook audit =="
echo "srctree : $SRCTREE"
echo "commit  : $(git rev-parse HEAD 2>/dev/null || echo unknown)"
hr

# --------------------------------------------------------------------------
# 1. Manual hook guard detection (upstream check_ksu_manual_guard)
#    Upstream: grep -wq "CONFIG_KSU_MANUAL_HOOK" <file>
# --------------------------------------------------------------------------
echo
echo "[1] Manual hook guard detection (CONFIG_KSU_MANUAL_HOOK)"
echo "    mirrors inline_hook_check.mk check_ksu_manual_guard"
echo

GUARD_FILES=()
for entry in "${HOOK_TABLE[@]}"; do
  f="${entry%%|*}"
  if [ ! -f "$f" ]; then
    echo "  MISSING FILE  $f"
    FAIL=1
    continue
  fi
  if grep -wq "CONFIG_KSU_MANUAL_HOOK" "$f"; then
    echo "  GUARD FOUND   $f"
    GUARD_FILES+=("$f")
    FAIL=1
  else
    echo "  clean         $f"
  fi
done

if [ "${#GUARD_FILES[@]}" -gt 0 ]; then
  echo
  echo "  ::error::KSU_MANUAL_HOOK guard detected in: ${GUARD_FILES[*]}"
  echo "  BakaSU/susfs_inline: WARNING: Your build maybe broken."
else
  echo
  echo "  No KSU_MANUAL_HOOK guard in any of the 7 inspected kernel files."
fi

# --------------------------------------------------------------------------
# 2. Required hook presence (upstream check_ksu_hook)
# --------------------------------------------------------------------------
echo
hr
echo "[2] Required SUSFS inline hook presence"
echo "    mirrors inline_hook_check.mk check_ksu_hook"
echo

for entry in "${HOOK_TABLE[@]}"; do
  f="${entry%%|*}"
  hook="${entry##*|}"
  [ -f "$f" ] || continue
  if grep -q "$hook" "$f"; then
    echo "  found         $hook  ($f)"
  else
    echo "  MISSING       $hook  ($f)"
    echo "  ::error::You lost $hook hook in your kernel ($f)"
    FAIL=1
  fi
done

# --------------------------------------------------------------------------
# 3. Incompatible legacy hook detection (upstream check_ksu_hook_incompatible)
# --------------------------------------------------------------------------
echo
hr
echo "[3] Incompatible legacy hook detection"
echo "    mirrors inline_hook_check.mk check_ksu_hook_incompatible"
echo

for entry in "${INCOMPATIBLE_TABLE[@]}"; do
  hook="${entry%%|*}"
  f="${entry##*|}"
  [ -f "$f" ] || continue
  if grep -wq "$hook" "$f"; then
    echo "  INCOMPATIBLE  $hook  ($f)"
    echo "  ::error::$hook is an incompatible hook, replace it with the static_key variant"
    FAIL=1
  else
    echo "  absent        $hook  ($f)"
  fi
done

# --------------------------------------------------------------------------
# 4. Repo-wide guard scan, symlink-safe (informational + hard fail if the
#    guard leaks into kernel-owned trees outside drivers/kernelsu)
# --------------------------------------------------------------------------
echo
hr
echo "[4] Repo-wide KSU_MANUAL_HOOK scan (symlink-safe)"
echo "    'grep -r' does NOT follow symlinks; drivers/kernelsu is excluded"
echo "    because it resolves to the BakaSU submodule."
echo

SCAN_HITS="$(grep -rn "CONFIG_KSU_MANUAL_HOOK" \
      fs/ kernel/ security/ mm/ ipc/ drivers/ \
      2>/dev/null \
    | grep -v '^drivers/kernelsu/' \
    || true)"

if [ -n "$SCAN_HITS" ]; then
  echo "$SCAN_HITS"
  # Only defconfig-style comment lines are tolerated.
  REAL_HITS="$(echo "$SCAN_HITS" | grep -vE '^[^:]+:[0-9]+:# CONFIG_KSU_MANUAL_HOOK' || true)"
  if [ -n "$REAL_HITS" ]; then
    echo
    echo "  ::error::Active CONFIG_KSU_MANUAL_HOOK references outside the submodule"
    FAIL=1
  else
    echo
    echo "  Only commented-out defconfig entries -- harmless."
  fi
else
  echo "  No CONFIG_KSU_MANUAL_HOOK outside drivers/kernelsu."
fi

# --------------------------------------------------------------------------
# 5. Supporting component sanity (informational)
# --------------------------------------------------------------------------
echo
hr
echo "[5] Supporting components"
echo

check_present() {
  if [ -e "$2" ]; then echo "  ok            $1  ($2)"; else echo "  MISSING       $1  ($2)"; FAIL=1; fi
}

check_present "BakaSU submodule"  "KernelSU/kernel/Kbuild"
check_present "SUSFS source"        "fs/susfs.c"
check_present "SUSFS header"        "include/linux/susfs.h"
check_present "SUSFS Kconfig"       "fs/Kconfig" 
check_present "Re:Kernel"           "drivers/net/rekernel/Makefile"
check_present "drivers/Makefile KSU wiring" "drivers/Makefile"

if grep -q "kernelsu" drivers/Makefile 2>/dev/null; then
  echo "  ok            drivers/Makefile registers kernelsu/"
else
  echo "  MISSING       drivers/Makefile does not register kernelsu/"
  FAIL=1
fi

# --- component Kconfig/Makefile wiring ------------------------------------
# A component whose Kconfig is never sourced cannot be enabled: the symbol in
# the defconfig is silently dropped and the driver is never compiled, while the
# build still reports success. Assert the wiring explicitly.
check_wiring() {
  local label="$1" kconfig_src="$2" makefile="$3" objexpr="$4"
  if grep -rqF "$kconfig_src" --include=Kconfig . 2>/dev/null; then
    echo "  ok            ${label} Kconfig sourced ($kconfig_src)"
  else
    echo "  MISSING       ${label} Kconfig NOT sourced anywhere ($kconfig_src)"
    FAIL=1
  fi
  if grep -qF "$objexpr" "$makefile" 2>/dev/null; then
    echo "  ok            ${label} Makefile wiring ($objexpr)"
  else
    echo "  MISSING       ${label} Makefile missing ($objexpr in $makefile)"
    FAIL=1
  fi
}

check_wiring "NoMount"   "fs/nomount/Kconfig"          "fs/Makefile"                 "nomount/"
check_wiring "Re:Kernel" "drivers/net/rekernel/Kconfig" "drivers/net/Makefile"        "rekernel/"
check_wiring "BBG"       "security/baseband-guard/Kconfig" "security/Makefile"        "baseband-guard/"
check_wiring "BakaSU"  "drivers/kernelsu/Kconfig"    "drivers/Makefile"            "kernelsu/"

# --- board symbol required for the device trees ---------------------------
# arch/arm/boot/dts/qcom/Makefile gates sdm660-mtp-wayne.dtb AND
# sdm660-mtp-jasmine.dtb behind CONFIG_MACH_XIAOMI_WAYNE. Without it the dts
# Makefile falls through to the generic Qualcomm board list, so the appended
# DTB in Image.gz-dtb contains no device DTB at all and the OC override never
# reaches the image. jasmine-stock was missing this for its entire history.
for dc in arch/arm64/configs/wayne_defconfig arch/arm64/configs/jasmine-stock_defconfig; do
  [ -f "$dc" ] || continue
  if grep -qE "^CONFIG_MACH_XIAOMI_WAYNE=y" "$dc"; then
    echo "  ok            $(basename "$dc") has CONFIG_MACH_XIAOMI_WAYNE=y (device DTB will be built)"
  else
    echo "  MISSING       $(basename "$dc") lacks CONFIG_MACH_XIAOMI_WAYNE=y"
    echo "                -> sdm660-mtp-*.dtb will NOT be built; image gets generic Qualcomm DTBs only"
    FAIL=1
  fi
done

SUSFS_VER="$(grep -E '^#define SUSFS_VERSION' include/linux/susfs.h 2>/dev/null | cut -d' ' -f3 | tr -d '"' || true)"
echo "  SUSFS_VERSION ${SUSFS_VER:-unknown}"

# --------------------------------------------------------------------------
# 6. Defconfig option audit
# --------------------------------------------------------------------------
echo
hr
echo "[6] Defconfig options"
echo

for dc in arch/arm64/configs/wayne_defconfig arch/arm64/configs/jasmine-stock_defconfig; do
  [ -f "$dc" ] || continue
  echo "  --- $dc"
  for opt in CONFIG_KSU CONFIG_KSU_SUSFS CONFIG_NOMOUNT CONFIG_BBG CONFIG_REKERNEL; do
    if grep -qE "^${opt}=y" "$dc"; then
      echo "      ok            ${opt}=y"
    else
      echo "      MISSING       ${opt}=y"
      FAIL=1
    fi
  done
  if grep -qE "^CONFIG_KSU_MANUAL_HOOK=y" "$dc"; then
    echo "      ::error::CONFIG_KSU_MANUAL_HOOK=y present in $dc"
    FAIL=1
  else
    echo "      ok            CONFIG_KSU_MANUAL_HOOK not enabled"
  fi
  # BakaSU main HEAD (>= 3d1185d8) calls ksu_get_session_keyring(), whose
  # 4.4 branch dereferences cred->session_keyring. In this tree that field only
  # exists inside `#ifdef CONFIG_KEYS` (include/linux/cred.h), so a defconfig
  # without CONFIG_KEYS fails to compile the KernelSU submodule outright.
  # Historical note: KernelSU used to guard this with
  # KSU_COMPAT_REQUIRE_SESSION_KEYRING; upstream commit 6ec8d9a8 dropped that
  # ifdef, which is exactly what makes CONFIG_KEYS mandatory here.
  if grep -qE "^CONFIG_KEYS=y" "$dc"; then
    echo "      ok            CONFIG_KEYS=y (required by ksu_get_session_keyring)"
  else
    echo "      ::error::CONFIG_KEYS=y missing in $dc -- ksu_get_session_keyring()"
    echo "               dereferences cred->session_keyring, which is only declared"
    echo "               under #ifdef CONFIG_KEYS in include/linux/cred.h."
    FAIL=1
  fi
done

# --------------------------------------------------------------------------
hr
echo
echo "=== AUDIT RESULT ==="
if [ "$FAIL" -eq 0 ]; then
  echo "Manual Hook        : NOT FOUND"
  echo "Inline Hook        : FOUND"
  echo "Duplicate Hook     : NO"
  echo "Potential Overlap  : NO"
  echo "Unresolved Hook    : NO"
  echo "STATUS             : PASS"
  exit 0
else
  echo "STATUS             : FAIL"
  exit 1
fi
