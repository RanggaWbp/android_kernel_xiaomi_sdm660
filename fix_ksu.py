#!/usr/bin/env python3
"""
Skrip AUTO-FIX untuk temuan dari cek_ksu.py.

Cara pakai:
    cd /path/ke/kernel/anda
    python3 fix_ksu.py

Yang dikerjakan otomatis:
  1. Hapus 'static' dari 5 simbol SELinux yang wajib di-export:
     write_op[], sel_handle_status_ops, sel_mutex,
     selinux_status_page, selinux_status_lock
  2. Tambah 'CONFIG_KALLSYMS=y' ke wayne_defconfig (kalau belum ada)
  3. Hapus baris duplikat CONFIG_KSU_MANUAL_HOOK_AUTO_INITRC_HOOK
     di jasmine-stock_defconfig & wayne_defconfig
  4. Hapus baris duplikat CONFIG_SND_SOC_PCM512 dan CONFIG_OVERLAY_FS
     di jasmine-stock_defconfig (hanya duplikat persis sama)

Yang TIDAK diubah otomatis (butuh keputusan Anda):
  - Pola '#ifdef CONFIG_KSU_SUSFS' vs 'CONFIG_KSU_MANUAL_HOOK' di fs/exec.c.
    Ini menyangkut logika hook, bukan sekadar rename, jadi skrip ini
    sengaja TIDAK menyentuhnya biar tidak merusak kompilasi Anda.
    Kalau mau, jalankan skrip ini dulu, coba build, dan laporkan
    balik ke sini kalau masih ada masalah terkait itu.

Setiap file yang diubah akan dibuatkan backup dengan akhiran '.bak_ksu'
di folder yang sama, SEBELUM diubah. Kalau ingin membatalkan semua
perubahan, tinggal:
    find . -name "*.bak_ksu" | while read f; do mv "$f" "${f%.bak_ksu}"; done
"""

import os
import re
import shutil

REPO = os.getcwd()

def path(p):
    return os.path.join(REPO, p)

def backup_and_read(p):
    full = path(p)
    if not os.path.isfile(full):
        print(f"[LEWAT] {p} tidak ditemukan, dilewati.")
        return None, full
    bak = full + ".bak_ksu"
    if not os.path.exists(bak):
        shutil.copy2(full, bak)
    with open(full, "r", errors="ignore") as f:
        return f.read(), full

def write(full, content):
    with open(full, "w") as f:
        f.write(content)

changed_files = []

def apply_regex_fix(p, pattern, replacement, label, count_expected=1):
    content, full = backup_and_read(p)
    if content is None:
        return
    new_content, n = re.subn(pattern, replacement, content, count=count_expected)
    if n == 0:
        print(f"[LEWAT] {label}: pola tidak ditemukan di {p} (mungkin sudah diperbaiki / beda struktur).")
        return
    write(full, new_content)
    changed_files.append(p)
    print(f"[FIX]   {label}: diperbaiki di {p} ({n}x)")

# ---------------------------------------------------------------
# 1. Hapus 'static' dari 5 simbol SELinux
# ---------------------------------------------------------------
print("== 1. Export simbol SELinux ==")

apply_regex_fix(
    "security/selinux/selinuxfs.c",
    r"static\s+ssize_t\s*\(\*write_op\[\]\)",
    "ssize_t (*write_op[])",
    "write_op[]"
)

apply_regex_fix(
    "security/selinux/selinuxfs.c",
    r"static\s+const\s+struct\s+file_operations\s+sel_handle_status_ops",
    "const struct file_operations sel_handle_status_ops",
    "sel_handle_status_ops"
)

apply_regex_fix(
    "security/selinux/selinuxfs.c",
    r"static\s+DEFINE_MUTEX\(sel_mutex\)",
    "DEFINE_MUTEX(sel_mutex)",
    "sel_mutex"
)

apply_regex_fix(
    "security/selinux/ss/status.c",
    r"static\s+struct\s+page\s*\*\s*selinux_status_page",
    "struct page *selinux_status_page",
    "selinux_status_page"
)

apply_regex_fix(
    "security/selinux/ss/status.c",
    r"static\s+DEFINE_MUTEX\(selinux_status_lock\)",
    "DEFINE_MUTEX(selinux_status_lock)",
    "selinux_status_lock"
)

# ---------------------------------------------------------------
# 2. Tambah CONFIG_KALLSYMS=y ke wayne_defconfig
# ---------------------------------------------------------------
print("\n== 2. CONFIG_KALLSYMS di wayne_defconfig ==")

content, full = backup_and_read("arch/arm64/configs/wayne_defconfig")
if content is not None:
    if re.search(r"^CONFIG_KALLSYMS=y", content, re.M):
        print("[LEWAT] CONFIG_KALLSYMS=y sudah ada di wayne_defconfig.")
    else:
        m = re.search(r"^CONFIG_KALLSYMS_ALL=y\s*$", content, re.M)
        if m:
            insert_at = m.start()
            new_content = content[:insert_at] + "CONFIG_KALLSYMS=y\n" + content[insert_at:]
            write(full, new_content)
            changed_files.append("arch/arm64/configs/wayne_defconfig")
            print("[FIX]   Ditambahkan 'CONFIG_KALLSYMS=y' tepat sebelum CONFIG_KALLSYMS_ALL=y di wayne_defconfig")
        else:
            print("[LEWAT] Baris CONFIG_KALLSYMS_ALL=y tidak ditemukan, tidak menambahkan apa pun (biar aman).")

# ---------------------------------------------------------------
# 3. Hapus duplikat baris CONFIG_KSU_MANUAL_HOOK_AUTO_INITRC_HOOK
# ---------------------------------------------------------------
print("\n== 3. Hapus baris config duplikat ==")

def dedup_exact_consecutive_or_nearby(p, needle_regex):
    content, full = backup_and_read(p)
    if content is None:
        return
    lines = content.split("\n")
    out = []
    seen_line_text = set()
    removed = 0
    for ln in lines:
        key = ln.strip()  # bandingkan tanpa peduli spasi di ujung baris
        if re.match(needle_regex, key):
            if key in seen_line_text:
                removed += 1
                continue
            seen_line_text.add(key)
        out.append(ln)
    if removed:
        write(full, "\n".join(out))
        changed_files.append(p)
        print(f"[FIX]   Dihapus {removed} baris duplikat identik '{needle_regex}' di {p}")
    else:
        print(f"[LEWAT] Tidak ada duplikat identik untuk '{needle_regex}' di {p}")

for defconf in ["arch/arm64/configs/jasmine-stock_defconfig", "arch/arm64/configs/wayne_defconfig"]:
    dedup_exact_consecutive_or_nearby(defconf, r"#?\s*CONFIG_KSU_MANUAL_HOOK_AUTO_INITRC_HOOK")

dedup_exact_consecutive_or_nearby("arch/arm64/configs/jasmine-stock_defconfig", r"#?\s*CONFIG_SND_SOC_PCM512")
dedup_exact_consecutive_or_nearby("arch/arm64/configs/jasmine-stock_defconfig", r"#?\s*CONFIG_OVERLAY_FS")

# ---------------------------------------------------------------
# Ringkasan
# ---------------------------------------------------------------
print("\n== RINGKASAN ==")
if changed_files:
    print("File yang diubah (backup asli ada di *.bak_ksu):")
    for f in sorted(set(changed_files)):
        print(f"  - {f}")
    print("\nSaran selanjutnya:")
    print("  1. Jalankan ulang cek_ksu.py untuk pastikan semua [HILANG] sudah jadi [OK]")
    print("  2. Coba build kernel seperti biasa")
    print("  3. Kalau build sukses:")
    print("     git add -A && git commit -m \"fix: export required SELinux symbols for BakaSU + defconfig cleanup\"")
else:
    print("Tidak ada perubahan yang dilakukan (semua sudah benar atau pola tidak cocok).")
