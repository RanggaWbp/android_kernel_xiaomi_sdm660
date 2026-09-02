#!/usr/bin/env bash
# =============================================================================
# scripts/validate_oc_config.sh
#
# Static safety check untuk file configs/*/*.conf SEBELUM compile.
# Exit non-zero pada config invalid -- workflow GitHub Actions HARUS
# menghentikan build jika script ini gagal (lihat item 12/13 brief asli).
#
# Ini adalah STATIC check (syntax, batas nilai yang dikenal dari source).
# Ini BUKAN pengganti hardware testing -- lihat testing matrix di README.
# =============================================================================
set -euo pipefail

CONF_FILE="${1:?Usage: validate_oc_config.sh <path-to-conf>}"

if [[ ! -f "$CONF_FILE" ]]; then
    echo "ERROR: invalid OC configuration -- file not found: $CONF_FILE"
    exit 1
fi

# shellcheck disable=SC1090
source "$CONF_FILE"

fail() {
    echo "ERROR: invalid OC configuration -- $1"
    exit 1
}

# --- 1. Field wajib ---
for var in DEVICE CODENAME PROFILE ARTIFACT_NAME; do
    if [[ -z "${!var:-}" ]]; then
        fail "field wajib '$var' kosong/tidak ada di $CONF_FILE"
    fi
done

# --- 2. Device valid ---
case "$DEVICE" in
    mi6x|mia2) ;;
    *) fail "DEVICE tidak dikenal: '$DEVICE' (harus mi6x atau mia2)" ;;
esac

case "$CODENAME" in
    wayne|jasmine_sprout) ;;
    *) fail "CODENAME tidak dikenal: '$CODENAME'" ;;
esac

# Pastikan device <-> codename tidak tertukar (item 8 brief asli)
if [[ "$DEVICE" == "mi6x" && "$CODENAME" != "wayne" ]]; then
    fail "device/codename mismatch: mi6x harus 'wayne', dapat '$CODENAME'"
fi
if [[ "$DEVICE" == "mia2" && "$CODENAME" != "jasmine_sprout" ]]; then
    fail "device/codename mismatch: mia2 harus 'jasmine_sprout', dapat '$CODENAME'"
fi

# --- 3. Profile valid ---
case "$PROFILE" in
    normal|oc_balanced|oc_performance|oc_extreme|oc_cpu_only) ;;
    *) fail "PROFILE tidak dikenal: '$PROFILE'" ;;
esac

# --- 4. Thermal protection tidak boleh dimatikan, di profile manapun ---
if [[ "${THERMAL_PROTECTION:-enabled}" != "enabled" ]]; then
    fail "THERMAL_PROTECTION harus 'enabled' di semua profile (aturan #7/#15)"
fi
if [[ "${THERMAL_BYPASS:-false}" != "false" ]]; then
    fail "THERMAL_BYPASS harus 'false' -- tidak boleh menonaktifkan thermal shutdown"
fi

# --- 5. Normal build tidak boleh punya OC/voltage mod apapun ---
if [[ "$PROFILE" == "normal" ]]; then
    for var in CPU_OVERCLOCK GPU_OVERCLOCK VOLTAGE_MODIFICATION; do
        val="${!var:-false}"
        if [[ "$val" != "false" ]]; then
            fail "profile 'normal' tidak boleh set $var=true (harus baseline murni)"
        fi
    done
fi

# --- 6. Frequency CPU harus ada di tabel SOURCE (whitelist), tidak boleh sembarang angka ---
VALID_PWRCL_MHZ=(300 633.6 902.4 1113.6 1401.6 1536 1612.8 1747.2 1843.2)
VALID_PERFCL_MHZ=(300 1113.6 1401.6 1747.2 1804.8 1958.4 2150.4 2208 2457.6)

check_in_list() {
    local val="$1"; shift
    local x
    for x in "$@"; do
        [[ "$val" == "$x" ]] && return 0
    done
    return 1
}

if [[ "${CPU_LITTLE_MAX_MHZ:-stock}" != "stock" ]]; then
    check_in_list "$CPU_LITTLE_MAX_MHZ" "${VALID_PWRCL_MHZ[@]}" \
        || fail "CPU_LITTLE_MAX_MHZ=$CPU_LITTLE_MAX_MHZ tidak ada di tabel qcom,pwrcl-speedbinN-v0 (sdm660.dtsi) -- NOT VERIFIED, ditolak"
fi

if [[ "${CPU_BIG_MAX_MHZ:-stock}" != "stock" ]]; then
    check_in_list "$CPU_BIG_MAX_MHZ" "${VALID_PERFCL_MHZ[@]}" \
        || fail "CPU_BIG_MAX_MHZ=$CPU_BIG_MAX_MHZ tidak ada di tabel qcom,perfcl-speedbinN-v0 (sdm660.dtsi) -- NOT VERIFIED, ditolak"
fi

# --- 7. Frequency GPU harus ada di tabel SOURCE ---
VALID_GPU_MHZ=(19.2 160 266 370 465 588 647 700 750)
if [[ "${GPU_MAX_MHZ:-stock}" != "stock" ]]; then
    check_in_list "$GPU_MAX_MHZ" "${VALID_GPU_MHZ[@]}" \
        || fail "GPU_MAX_MHZ=$GPU_MAX_MHZ tidak ada di qcom,gpu-pwrlevel-bins (sdm660-gpu.dtsi) -- NOT VERIFIED, ditolak"
fi

# --- 8. Cross-bin profile wajib eksplisit acknowledge risiko ---
if [[ -n "${CPU_BIG_FORCE_SPEEDBIN:-}" || -n "${CPU_LITTLE_FORCE_SPEEDBIN:-}" || -n "${GPU_FORCE_PWRLEVEL_BIN_EFUSE_RAW:-}" ]]; then
    if [[ "${REQUIRES_ACKNOWLEDGEMENT:-false}" != "true" ]]; then
        fail "profile melakukan speedbin cross-bin override tapi REQUIRES_ACKNOWLEDGEMENT != true"
    fi
    if [[ "${RISK_LEVEL:-}" != "LOW-MODERATE" && "${RISK_LEVEL:-}" != "HIGH" && "${RISK_LEVEL:-}" != "VERY-HIGH" ]]; then
        fail "profile cross-bin wajib set RISK_LEVEL (LOW-MODERATE/HIGH/VERY-HIGH)"
    fi
fi

# --- 8b. Nilai qcom,speed-bin GPU harus salah satu efuse raw yang benar-benar
#          ada di sdm660-gpu.dtsi (0, 157, 146, 135, 78, 90, 122) -- BUKAN
#          indeks 0/1/2/3 sederhana. Nilai lain = mismatch -> GPU probe gagal.
VALID_GPU_EFUSE_RAW=(0 157 146 135 78 90 122)
if [[ -n "${GPU_FORCE_PWRLEVEL_BIN_EFUSE_RAW:-}" ]]; then
    check_in_list "$GPU_FORCE_PWRLEVEL_BIN_EFUSE_RAW" "${VALID_GPU_EFUSE_RAW[@]}" \
        || fail "GPU_FORCE_PWRLEVEL_BIN_EFUSE_RAW=$GPU_FORCE_PWRLEVEL_BIN_EFUSE_RAW bukan nilai qcom,speed-bin yang ada di sdm660-gpu.dtsi -- akan menyebabkan GPU probe gagal (-ENODEV)"
fi

# --- 9. Tidak boleh ada duplicate OPP/voltage di file yang sama ---
if [[ -n "${CPU_BIG_MAX_MHZ:-}" && -n "${CPU_LITTLE_MAX_MHZ:-}" && "$CPU_BIG_MAX_MHZ" == "$CPU_LITTLE_MAX_MHZ" && "$CPU_BIG_MAX_MHZ" != "stock" ]]; then
    fail "CPU_BIG_MAX_MHZ dan CPU_LITTLE_MAX_MHZ tidak boleh identik untuk nilai non-stock (indikasi copy-paste error)"
fi

# --- 10. Extreme profile wajib HARDWARE_TESTED=false sampai terbukti via testing matrix ---
if [[ "$PROFILE" == "oc_extreme" && "${HARDWARE_TESTED:-false}" == "true" ]]; then
    fail "oc_extreme tidak boleh HARDWARE_TESTED=true tanpa lampiran hasil testing matrix (lihat README)"
fi

echo "OK: $CONF_FILE valid (device=$DEVICE profile=$PROFILE risk=${RISK_LEVEL:-none})"
exit 0
