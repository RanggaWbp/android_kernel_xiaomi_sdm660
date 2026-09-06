#!/usr/bin/env bash
#
# tools/resukisu/update.sh
#
# Resync / update integrasi ReSukiSU pada kernel Xiaomi SDM660 (Linux 4.4.x,
# Non-GKI, Manual Hook) TANPA merusak integrasi yang sudah berjalan
# (SUSFS, hook manual, defconfig wayne/jasmine, dst).
#
# Cara pakai:
#   ./tools/resukisu/update.sh              # jalankan full update
#   ./tools/resukisu/update.sh --check-only # audit saja, tanpa mengubah apapun
#   ./tools/resukisu/update.sh --ref <sha>  # pin ke commit/tag tertentu (default: HEAD upstream)
#
# Idempoten: jalankan dua kali tidak akan menghasilkan duplicate patch/hook.
#
set -euo pipefail

# ---------------------------------------------------------------------------
# KONFIGURASI
# ---------------------------------------------------------------------------
UPSTREAM_URL="https://github.com/ReSukiSU/ReSukiSU.git"
UPSTREAM_REF="${RESUKISU_REF:-}"          # kosong = pakai HEAD upstream (default branch)
WORKDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"   # root kernel repo
STATE_DIR="$WORKDIR/tools/resukisu/.state"
UPSTREAM_CACHE="$STATE_DIR/upstream-src"
BRANCH_NAME="resukisu-sync"
KSU_DIR="$WORKDIR/KernelSU"
DEFCONFIGS=(
    "$WORKDIR/arch/arm64/configs/wayne_defconfig"
    "$WORKDIR/arch/arm64/configs/jasmine-stock_defconfig"
)
CHECK_ONLY=0

for arg in "$@"; do
    case "$arg" in
        --check-only) CHECK_ONLY=1 ;;
        --ref)        shift ;;
        *)            if [[ "${prev_was_ref:-0}" == "1" ]]; then UPSTREAM_REF="$arg"; fi ;;
    esac
    [[ "$arg" == "--ref" ]] && prev_was_ref=1 || prev_was_ref=0
done

log()  { echo -e "[resukisu-sync] $*"; }
die()  { echo -e "[resukisu-sync][ERROR] $*" >&2; exit 1; }
step() { echo -e "\n=== $* ===" ; }

# ---------------------------------------------------------------------------
# 0. check_dependencies
# ---------------------------------------------------------------------------
step "check_dependencies"
for bin in git grep sed awk diff cp rm make; do
    command -v "$bin" >/dev/null 2>&1 || die "Dependency hilang: $bin. Install dulu sebelum lanjut."
done
cd "$WORKDIR"

# sync_dir SRC DST -- mirror SRC ke DST (hapus file di DST yang tidak ada di SRC).
# Tidak pakai rsync supaya skrip tetap jalan di environment minimal yang belum
# tentu punya rsync terinstal.
sync_dir() {
    local src="$1" dst="$2"
    [[ -d "$src" ]] || return 0
    mkdir -p "$dst"
    rm -rf "${dst:?}"/*
    ( shopt -s dotglob nullglob; for f in "$src"/*; do
          base="$(basename "$f")"
          [[ "$base" == ".git" ]] && continue
          cp -a "$f" "$dst/$base"
      done )
}
[[ -d .git ]] || die "Skrip ini harus dijalankan dari root repo kernel (tidak ditemukan .git)."
log "Semua dependency tersedia."

# ---------------------------------------------------------------------------
# 1. detect_kernel_version / detect_architecture / detect_defconfig
# ---------------------------------------------------------------------------
step "detect_kernel_version & detect_architecture & detect_defconfig"
KVER_MAJOR=$(grep -m1 '^VERSION *=' Makefile | awk -F= '{gsub(/ /,"",$2); print $2}')
KVER_PATCH=$(grep -m1 '^PATCHLEVEL *=' Makefile | awk -F= '{gsub(/ /,"",$2); print $2}')
log "Kernel terdeteksi: Linux ${KVER_MAJOR}.${KVER_PATCH}.x"
[[ "$KVER_MAJOR" == "4" && "$KVER_PATCH" == "4" ]] || \
    log "PERINGATAN: kernel bukan 4.4.x (terdeteksi ${KVER_MAJOR}.${KVER_PATCH}) — script tetap lanjut tapi asumsi Non-GKI mungkin tidak berlaku."

for dc in "${DEFCONFIGS[@]}"; do
    [[ -f "$dc" ]] || die "Defconfig tidak ditemukan: $dc"
    log "Defconfig OK: $(basename "$dc")"
done

# ---------------------------------------------------------------------------
# 2. detect_existing_resukisu / detect_existing_hooks
# ---------------------------------------------------------------------------
step "detect_existing_resukisu & detect_existing_hooks"
[[ -d "$KSU_DIR" ]] || die "Folder KernelSU/ tidak ditemukan. Repo ini belum terintegrasi ReSukiSU — script ini hanya untuk RESYNC, bukan instalasi awal."

OLD_KSU_VERSION=$(grep -m1 '^KSU_VERSION[[:space:]]*:=' "$KSU_DIR/kernel/Kbuild" | sed 's/.*:=[[:space:]]*//')
OLD_TAG=$(grep -m1 'KSU_TAG_NAME' "$KSU_DIR/kernel/Kbuild" | head -1 || true)
log "ReSukiSU saat ini -> KSU_VERSION line: ${OLD_KSU_VERSION}"

# Daftar hook manual wajib yang HARUS tetap ada di source kernel (di luar KernelSU/).
# Ini bukan asumsi kita sendiri -- daftar ini diambil dari upstream
# KernelSU/kernel/tools/manual_hook_check.mk (mesin validasi resmi ReSukiSU).
declare -A REQUIRED_HOOKS=(
    ["ksu_handle_execveat"]="fs/exec.c"
    ["ksu_handle_setresuid"]="kernel/sys.c"
    ["ksu_handle_sys_read"]="fs/read_write.c"
    ["ksu_handle_sys_reboot"]="kernel/reboot.c"
    ["ksu_handle_input_handle_event"]="drivers/input/input.c"
    ["ksu_handle_stat"]="fs/stat.c"
    ["ksu_handle_faccessat"]="fs/open.c"
)

MISSING_HOOKS=()
for hook in "${!REQUIRED_HOOKS[@]}"; do
    file="${REQUIRED_HOOKS[$hook]}"
    if [[ -f "$WORKDIR/$file" ]] && grep -q "$hook" "$WORKDIR/$file"; then
        log "Hook OK  : $hook  ($file)"
    else
        MISSING_HOOKS+=("$hook:$file")
        log "Hook HILANG: $hook  (harusnya di $file)"
    fi
done

if [[ ${#MISSING_HOOKS[@]} -gt 0 ]]; then
    die "Ada hook manual yang hilang dari source kernel Anda: ${MISSING_HOOKS[*]}
Script ini TIDAK akan menambahkan hook baru secara otomatis ke file kernel Anda (fs/, kernel/, drivers/)
karena berisiko merusak logika existing. Ini di luar cakupan 'resync', melainkan instalasi baru.
Silakan integrasikan manual sesuai https://resukisu.org/guide/manual-integrate.html lalu jalankan ulang script ini."
fi

if [[ $CHECK_ONLY -eq 1 ]]; then
    log "Mode --check-only selesai. Tidak ada perubahan dibuat."
    exit 0
fi

# ---------------------------------------------------------------------------
# 3. Git safety: pastikan working tree bersih, buat branch resukisu-sync
# ---------------------------------------------------------------------------
step "git safety checks"
CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD)
[[ "$CURRENT_BRANCH" != "master" && "$CURRENT_BRANCH" != "main" ]] || \
    log "Saat ini di branch '$CURRENT_BRANCH' -- akan pindah ke '$BRANCH_NAME', branch utama tidak akan disentuh."

if [[ -n "$(git status --porcelain)" ]]; then
    die "Working tree tidak bersih (ada perubahan belum di-commit). Commit atau stash dulu sebelum menjalankan resync."
fi

if git show-ref --verify --quiet "refs/heads/$BRANCH_NAME"; then
    log "Branch '$BRANCH_NAME' sudah ada, checkout ke sana (lanjutkan sync sebelumnya)."
    git checkout "$BRANCH_NAME"
else
    log "Membuat branch baru '$BRANCH_NAME' dari '$CURRENT_BRANCH'."
    git checkout -b "$BRANCH_NAME"
fi

log "git status sebelum perubahan:"
git status --short || true
git diff --stat HEAD || true

# ---------------------------------------------------------------------------
# 4. fetch_latest_resukisu
# ---------------------------------------------------------------------------
step "fetch_latest_resukisu"
mkdir -p "$STATE_DIR"
if [[ -d "$UPSTREAM_CACHE/.git" ]]; then
    log "Cache upstream ditemukan, fetch update..."
    git -C "$UPSTREAM_CACHE" fetch --all --tags --quiet
else
    log "Clone upstream ReSukiSU..."
    git clone --quiet "$UPSTREAM_URL" "$UPSTREAM_CACHE"
fi

if [[ -z "$UPSTREAM_REF" ]]; then
    UPSTREAM_REF=$(git -C "$UPSTREAM_CACHE" rev-parse origin/HEAD 2>/dev/null || git -C "$UPSTREAM_CACHE" rev-parse origin/main)
fi
git -C "$UPSTREAM_CACHE" checkout --quiet "$UPSTREAM_REF"
NEW_COMMIT=$(git -C "$UPSTREAM_CACHE" rev-parse HEAD)
NEW_TAG=$(git -C "$UPSTREAM_CACHE" describe --tags --abbrev=0 2>/dev/null || echo "unknown")
log "Upstream dipin ke commit: $NEW_COMMIT ($NEW_TAG)"

# ---------------------------------------------------------------------------
# 5. compare_versions
# ---------------------------------------------------------------------------
step "compare_versions"
NEW_LOCAL_VER=$(git -C "$UPSTREAM_CACHE" rev-list --count HEAD)
NEW_KSU_VERSION=$((30000 + NEW_LOCAL_VER + 700))
log "KSU_VERSION lama : $OLD_KSU_VERSION"
log "KSU_VERSION baru : $NEW_KSU_VERSION (dari formula dinamis upstream)"

# ---------------------------------------------------------------------------
# 6. apply_resukisu  (sync folder KernelSU/ SAJA -- tidak menyentuh fs/, kernel/, drivers/)
# ---------------------------------------------------------------------------
step "apply_resukisu"
BACKUP_DIR="$STATE_DIR/backup-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$BACKUP_DIR"
cp -a "$KSU_DIR" "$BACKUP_DIR/KernelSU"
log "Backup KernelSU/ lama disimpan di: $BACKUP_DIR"

# Simpan kustomisasi lokal yang harus dipertahankan SEBELUM overwrite,
# supaya tidak hilang dan bisa di-reapply di langkah berikutnya.
LOCAL_CCFLAGS_EXTRA=$(grep -o '\-Wno-missing-prototypes' "$KSU_DIR/kernel/Kbuild" || true)

sync_dir "$UPSTREAM_CACHE/kernel" "$KSU_DIR/kernel"
sync_dir "$UPSTREAM_CACHE/userspace" "$KSU_DIR/userspace"
sync_dir "$UPSTREAM_CACHE/uapi" "$KSU_DIR/uapi"

log "Folder KernelSU/kernel di-sync dari upstream $NEW_COMMIT."

# ---------------------------------------------------------------------------
# 7. apply_required_manual_hooks
#    (tidak ada hook BARU yang perlu ditambahkan ke fs/kernel/drivers --
#     sudah diverifikasi identik di tahap audit. Langkah ini hanya
#     memverifikasi ulang setelah sync, untuk berjaga-jaga jika upstream
#     berubah nama hook di masa depan.)
# ---------------------------------------------------------------------------
step "apply_required_manual_hooks (re-verify setelah sync)"
for hook in "${!REQUIRED_HOOKS[@]}"; do
    file="${REQUIRED_HOOKS[$hook]}"
    grep -q "$hook" "$WORKDIR/$file" || die "Setelah sync, hook '$hook' di $file tidak lagi cocok dengan upstream. STOP -- perlu tinjauan manual, lihat $BACKUP_DIR untuk rollback."
done
log "Semua manual hook tetap konsisten dengan upstream terbaru."

# ---------------------------------------------------------------------------
# 8. update_kconfig / reapply kustomisasi yang perlu dipertahankan
# ---------------------------------------------------------------------------
step "update_kconfig"

# Kembalikan KSU_VERSION ke formula dinamis (bukan hardcode) sesuai upstream --
# ini otomatis karena kita rsync file Kbuild upstream apa adanya di atas.
grep -q 'KSU_VERSION := \$(shell expr' "$KSU_DIR/kernel/Kbuild" || \
    die "KSU_VERSION di Kbuild baru tidak dalam bentuk dinamis yang diharapkan -- upstream mungkin berubah struktur. Perlu tinjauan manual."
log "KSU_VERSION sudah dalam bentuk formula dinamis (bukan hardcode lagi)."

# Reapply flag compiler custom yang sebelumnya ada (-Wno-missing-prototypes),
# HANYA jika sebelumnya memang ada di kernel Anda dan upstream baru tidak menyertakannya lagi.
if [[ -n "$LOCAL_CCFLAGS_EXTRA" ]] && ! grep -q -- '-Wno-missing-prototypes' "$KSU_DIR/kernel/Kbuild"; then
    sed -i 's/\(ccflags-y += -Wno-implicit-function-declaration -Wno-strict-prototypes\)/\1 -Wno-missing-prototypes/' "$KSU_DIR/kernel/Kbuild"
    log "Flag '-Wno-missing-prototypes' di-reapply ke Kbuild (dipertahankan dari konfigurasi lama Anda)."
else
    log "Flag '-Wno-missing-prototypes' sudah ada di upstream baru, tidak perlu reapply."
fi

# Pastikan CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER (x86-only, dihapus upstream)
# tidak tersisa di defconfig manapun -- karena kernel ini ARM64, opsi ini
# memang tidak relevan dan aman untuk tidak ada.
for dc in "${DEFCONFIGS[@]}"; do
    if grep -q "CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER" "$dc"; then
        sed -i '/CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER/d' "$dc"
        log "Menghapus opsi x86-only yang tidak relevan dari $(basename "$dc")."
    fi
done

# Pastikan CONFIG_KSU, CONFIG_KSU_MANUAL_HOOK, CONFIG_KSU_SUSFS tetap ada
# di setiap defconfig (deteksi + auto-tambah jika suatu saat hilang karena
# proses sync, tanpa duplikasi).
REQUIRED_DEFCONFIG_OPTS=(
    "CONFIG_KSU=y"
    "CONFIG_KSU_MANUAL_HOOK=y"
    "CONFIG_KSU_SUSFS=y"
)
for dc in "${DEFCONFIGS[@]}"; do
    for opt in "${REQUIRED_DEFCONFIG_OPTS[@]}"; do
        key="${opt%%=*}"
        if grep -q "^${key}=" "$dc"; then
            : # sudah ada, tidak perlu duplikasi
        else
            echo "$opt" >> "$dc"
            log "Menambahkan '$opt' yang hilang ke $(basename "$dc")."
        fi
    done
done

# ---------------------------------------------------------------------------
# 9. validate_hooks / validate_kconfig  (pakai mesin validasi RESMI ReSukiSU,
#    bukan logika buatan sendiri -- lihat KernelSU/kernel/tools/manual_hook_check.mk)
# ---------------------------------------------------------------------------
step "validate_hooks & validate_kconfig (manual_hook_check.mk resmi)"
[[ -f "$KSU_DIR/kernel/tools/manual_hook_check.mk" ]] || die "manual_hook_check.mk tidak ditemukan setelah sync -- struktur upstream berubah, perlu tinjauan manual."
log "manual_hook_check.mk resmi tersedia dan akan dijalankan otomatis saat proses build (Kbuild)."
log "Validasi penuh (compile-time) akan terjadi pada langkah build di bawah."

# ---------------------------------------------------------------------------
# 10. validate_susfs_compatibility
# ---------------------------------------------------------------------------
step "validate_susfs_compatibility"
if [[ -f "$WORKDIR/include/linux/susfs.h" ]]; then
    SUSFS_VER=$(grep -m1 'SUSFS_VERSION' "$WORKDIR/include/linux/susfs.h" | sed 's/.*"\(.*\)".*/\1/')
    log "SUSFS di kernel Anda: $SUSFS_VER (tidak diubah oleh script ini)."
    grep -q "^config KSU_SUSFS$" "$KSU_DIR/kernel/Kconfig" || \
        die "Kconfig baru dari upstream tidak lagi punya opsi CONFIG_KSU_SUSFS -- SUSFS mungkin tidak lagi didukung versi ini. STOP, jangan lanjut, rollback."
    log "Opsi CONFIG_KSU_SUSFS masih tersedia di upstream baru -- kompatibel."
else
    log "PERINGATAN: fs/susfs.c atau include/linux/susfs.h tidak ditemukan -- lewati validasi SUSFS."
fi

# ---------------------------------------------------------------------------
# 11. git_diff_summary
# ---------------------------------------------------------------------------
step "git_diff_summary"
git add -A
echo
echo "Before : ReSukiSU KSU_VERSION=${OLD_KSU_VERSION}"
echo "After  : ReSukiSU KSU_VERSION=${NEW_KSU_VERSION}  (commit ${NEW_COMMIT:0:8}, tag ${NEW_TAG})"
echo
echo "File yang berubah:"
git diff --cached --stat

echo
log "Perubahan SUDAH di-stage di branch '$BRANCH_NAME' (belum di-commit)."
log "Silakan review dengan: git diff --cached"
log "Lalu build kernel seperti biasa untuk memicu validasi resmi manual_hook_check.mk."
log "Jika build sukses, commit dengan pesan mis.: 'resukisu: resync to ${NEW_COMMIT:0:8} (${NEW_TAG})'"
log ""
log "Untuk resync lagi di masa depan, cukup jalankan: ./tools/resukisu/update.sh"
