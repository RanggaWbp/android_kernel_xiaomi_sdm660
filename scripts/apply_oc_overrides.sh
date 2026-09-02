#!/usr/bin/env bash
# =============================================================================
# scripts/apply_oc_overrides.sh
#
# Menerjemahkan nilai di configs/*/*.conf menjadi override device-tree nyata,
# di-append ke akhir file DTS target (&label { ... }; override, teknik
# standar kernel utk menimpa properti node dari file dtsi yang di-include).
#
# HARUS dijalankan SETELAH patches/0001 & 0002 (jika dipakai) di-apply, dan
# SEBELUM compile. Tidak melakukan apapun untuk profile "normal".
#
# Usage: apply_oc_overrides.sh <path-to-conf> <path-to-kernel-source-root>
# =============================================================================
set -euo pipefail

CONF_FILE="${1:?Usage: apply_oc_overrides.sh <conf> <kernel-src-root>}"
SRC_ROOT="${2:?Usage: apply_oc_overrides.sh <conf> <kernel-src-root>}"

# shellcheck disable=SC1090
source "$CONF_FILE"

DTS_PATH="${SRC_ROOT}/${DTS_TARGET:?DTS_TARGET tidak ada di $CONF_FILE}"

if [[ ! -f "$DTS_PATH" ]]; then
    echo "ERROR: DTS target tidak ditemukan: $DTS_PATH"
    exit 1
fi

if [[ "${PROFILE:-normal}" == "normal" ]]; then
    echo "Profile 'normal' -- tidak ada override DT yang ditambahkan (baseline murni)."
    exit 0
fi

OVERRIDE_MARKER="/* === OC OVERRIDE (apply_oc_overrides.sh, profile=${PROFILE}) === */"

if grep -qF "$OVERRIDE_MARKER" "$DTS_PATH"; then
    echo "ERROR: DTS ini sudah pernah di-override sebelumnya (marker ditemukan). Batalkan agar tidak dobel."
    exit 1
fi

{
    echo ""
    echo "$OVERRIDE_MARKER"

    if [[ -n "${CPU_LITTLE_FORCE_SPEEDBIN:-}" ]]; then
        echo "&clock_cpu {"
        echo "	qcom,pwrcl-force-speedbin = <${CPU_LITTLE_FORCE_SPEEDBIN}>;"
        echo "};"
    fi

    if [[ -n "${CPU_BIG_FORCE_SPEEDBIN:-}" ]]; then
        echo "&clock_cpu {"
        echo "	qcom,perfcl-force-speedbin = <${CPU_BIG_FORCE_SPEEDBIN}>;"
        echo "};"
    fi

    if [[ -n "${GPU_FORCE_PWRLEVEL_BIN_EFUSE_RAW:-}" ]]; then
        echo "&msm_gpu {"
        echo "	qcom,gpu-force-speed-bin = <${GPU_FORCE_PWRLEVEL_BIN_EFUSE_RAW}>;"
        echo "};"
    fi
} >> "$DTS_PATH"

echo "OK: override DT untuk profile '${PROFILE}' ditambahkan ke ${DTS_PATH}"
echo "--- isi tambahan ---"
tail -n 20 "$DTS_PATH"
