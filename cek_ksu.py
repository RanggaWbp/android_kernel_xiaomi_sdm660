#!/usr/bin/env python3
"""
Skrip pengecekan integrasi BakaSU (KernelSU fork) untuk
android_kernel_xiaomi_sdm660.

Cara pakai:
    cd /path/ke/kernel/anda   # folder yang berisi Makefile, fs/, KernelSU/, dst
    python3 cek_ksu.py

Skrip akan:
  1. Update submodule KernelSU ke commit terbaru (branch default remote)
  2. Cek semua manual hook wajib (stat, execve, faccessat, reboot, setuid, sys_read)
  3. Cek export simbol SELinux (write_op, sel_handle_status_ops, dst)
  4. Cek defconfig jasmine-stock & wayne (CONFIG_KSU*, KALLSYMS_ALL, duplikat baris)

Tidak butuh koneksi internet selain saat update submodule (git fetch).
"""

import os
import re
import subprocess
import sys

REPO = os.getcwd()

def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd or REPO, shell=True,
                           capture_output=True, text=True)

def read(path):
    full = os.path.join(REPO, path)
    if not os.path.isfile(full):
        return None
    with open(full, "r", errors="ignore") as f:
        return f.read()

OK = "\033[92m[OK]\033[0m"
WARN = "\033[93m[PERIKSA]\033[0m"
FAIL = "\033[91m[HILANG]\033[0m"
INFO = "\033[96m[INFO]\033[0m"

report = []

def log(status, judul, detail=""):
    report.append((status, judul, detail))

def section(title):
    print(f"\n== {title} ==")

# ---------------------------------------------------------------
# 1. Update submodule KernelSU
# ---------------------------------------------------------------
section("1. Update submodule KernelSU (BakaSU)")

ks_path = os.path.join(REPO, "KernelSU")
if not os.path.isdir(ks_path):
    print(f"{FAIL} Folder KernelSU/ tidak ditemukan di {REPO}. Jalankan skrip ini di root kernel, dan pastikan `git submodule update --init` sudah pernah dijalankan.")
    sys.exit(1)

old_commit = run("git rev-parse HEAD", cwd=ks_path).stdout.strip()

# cari branch default remote (biasanya main/master)
r = run("git remote show origin", cwd=ks_path)
m = re.search(r"HEAD branch:\s*(\S+)", r.stdout)
default_branch = m.group(1) if m else "master"

run("git fetch origin", cwd=ks_path)
co = run(f"git checkout {default_branch}", cwd=ks_path)
pull = run(f"git pull origin {default_branch}", cwd=ks_path)
new_commit = run("git rev-parse HEAD", cwd=ks_path).stdout.strip()

if old_commit == new_commit:
    print(f"{OK} Submodule KernelSU sudah paling baru ({new_commit[:7]}).")
else:
    print(f"{OK} Submodule KernelSU diupdate: {old_commit[:7]} -> {new_commit[:7]}")
    print(f"{INFO} Jangan lupa commit perubahan submodule ini di repo utama:")
    print(f"   git add KernelSU && git commit -m \"chore: update KernelSU submodule to {new_commit[:7]}\"")

if pull.returncode != 0:
    print(f"{WARN} 'git pull' submodule menghasilkan pesan berikut (cek manual):\n{pull.stderr.strip()}")

# ---------------------------------------------------------------
# helper pencocokan
# ---------------------------------------------------------------
def has(content, pattern):
    return bool(re.search(pattern, content)) if content else False

# ---------------------------------------------------------------
# 2. Manual hooks wajib
# ---------------------------------------------------------------
section("2. Manual hooks (wajib menurut bakasu.org/guide/manual-integrate.html)")

# --- stat hook ---
stat_c = read("fs/stat.c")
if stat_c is None:
    log(FAIL, "fs/stat.c tidak ditemukan")
else:
    ok_stat  = has(stat_c, r"ksu_handle_stat\s*\(")
    ok_fstat = has(stat_c, r"ksu_handle_newfstat_ret\s*\(") or has(stat_c, r"ksu_handle_vfs_fstat\s*\(")
    if ok_stat:
        log(OK, "stat hook: ksu_handle_stat() terpasang di fs/stat.c")
    else:
        log(FAIL, "stat hook: ksu_handle_stat() TIDAK ditemukan di fs/stat.c")
    if ok_fstat:
        log(OK, "stat hook (return value): ksu_handle_newfstat_ret/vfs_fstat terpasang")
    else:
        log(WARN, "stat hook (return value): ksu_handle_newfstat_ret()/ksu_handle_vfs_fstat() tidak ditemukan — cek manual apakah versi BakaSU baru masih butuh ini")

# --- execve hook ---
exec_c = read("fs/exec.c")
if exec_c is None:
    log(FAIL, "fs/exec.c tidak ditemukan")
else:
    has_std      = has(exec_c, r"ksu_handle_execveat\s*\(") and has(exec_c, r"ksu_handle_post_execveat\s*\(")
    has_sucompat = has(exec_c, r"ksu_handle_execveat_sucompat\s*\(") and has(exec_c, r"ksu_handle_post_execveat_sucompat\s*\(")
    guarded_manual_hook = has(exec_c, r"#ifdef\s+CONFIG_KSU_MANUAL_HOOK") or has(exec_c, r"#if\s+defined\(CONFIG_KSU_MANUAL_HOOK\)")
    guarded_susfs_only  = has(exec_c, r"#ifdef\s+CONFIG_KSU_SUSFS") and not guarded_manual_hook

    if has_std or has_sucompat:
        variant = "standar (ksu_handle_execveat/post_execveat)" if has_std else "sucompat (ksu_handle_execveat_sucompat/post_execveat_sucompat)"
        log(OK, f"execve hook: terpasang, varian {variant}")
    else:
        log(FAIL, "execve hook: ksu_handle_execveat(_sucompat) TIDAK ditemukan di fs/exec.c")

    if guarded_susfs_only:
        log(WARN,
            "execve hook hanya dibungkus '#ifdef CONFIG_KSU_SUSFS', bukan 'CONFIG_KSU_MANUAL_HOOK'",
            "Ini artinya kalau suatu saat CONFIG_KSU_SUSFS dimatikan, hook root via execve ikut hilang meski "
            "CONFIG_KSU_MANUAL_HOOK tetap aktif. Sesuai defconfig Anda saat ini KSU_SUSFS=y jadi masih jalan, "
            "tapi ini beda dari pola resmi di panduan (harusnya #ifdef CONFIG_KSU_MANUAL_HOOK). Aman untuk "
            "dibiarkan HANYA jika Anda yakin CONFIG_KSU_SUSFS akan selalu aktif berbarengan dengan KSU_MANUAL_HOOK.")

# --- faccessat hook ---
open_c = read("fs/open.c")
if open_c is None:
    log(FAIL, "fs/open.c tidak ditemukan")
else:
    if has(open_c, r"ksu_handle_faccessat\s*\("):
        log(OK, "faccessat hook: ksu_handle_faccessat() terpasang di fs/open.c")
    else:
        log(FAIL, "faccessat hook: ksu_handle_faccessat() TIDAK ditemukan di fs/open.c")

# --- sys_reboot hook ---
reboot_c = read("kernel/reboot.c")
sys_c    = read("kernel/sys.c")
reboot_hooked = has(reboot_c, r"ksu_handle_sys_reboot\s*\(") or has(sys_c, r"ksu_handle_sys_reboot\s*\(")
if reboot_hooked:
    where = "kernel/reboot.c" if has(reboot_c, r"ksu_handle_sys_reboot") else "kernel/sys.c"
    log(OK, f"sys_reboot hook: ksu_handle_sys_reboot() terpasang di {where}")
else:
    log(FAIL, "sys_reboot hook: ksu_handle_sys_reboot() TIDAK ditemukan di kernel/reboot.c maupun kernel/sys.c")

# --- setresuid hook (6.8+ only, kernel ini 4.4 jadi biasanya tidak wajib) ---
if has(sys_c, r"ksu_handle_setresuid\s*\("):
    log(OK, "setresuid hook: ksu_handle_setresuid() terpasang di kernel/sys.c (bonus, tidak wajib di kernel 4.4)")
else:
    log(INFO, "setresuid hook tidak ada — WAJAR, hook ini hanya diperlukan untuk kernel 6.8+, kernel Anda 4.4")

# --- sys_read hook (6.8+ only) ---
rw_c = read("fs/read_write.c")
if has(rw_c, r"ksu_handle_sys_read\s*\("):
    log(OK, "sys_read hook: ksu_handle_sys_read() terpasang (bonus, tidak wajib di kernel 4.4)")
else:
    log(INFO, "sys_read hook tidak ada — WAJAR untuk kernel 4.4 (hanya wajib di kernel 6.8+)")

# --- input hook (opsional, biasanya auto) ---
input_c = read("drivers/input/input.c")
if has(input_c, r"ksu_handle_input_handle_event\s*\("):
    log(OK, "input hook: manual hook terpasang di drivers/input/input.c")
else:
    log(INFO, "input hook manual tidak ada — tidak masalah selama CONFIG_KSU_MANUAL_HOOK_AUTO_INPUT_HOOK aktif/didukung")

# ---------------------------------------------------------------
# 3. Export simbol SELinux
# ---------------------------------------------------------------
section("3. Export simbol SELinux (wajib kalau CONFIG_KALLSYMS_ALL tidak aktif)")

selinuxfs_c = read("security/selinux/selinuxfs.c")
status_c    = read("security/selinux/ss/status.c")
services_c  = read("security/selinux/ss/services.c")
hooks_c     = read("security/selinux/hooks.c")

def check_export(content, pattern_static, pattern_export, nama, file_):
    if content is None:
        log(WARN, f"{nama}: file {file_} tidak ditemukan")
        return
    if re.search(pattern_static, content):
        log(FAIL, f"{nama} MASIH static di {file_}", "Perlu hapus kata 'static' sesuai panduan manual-integrate.")
    elif re.search(pattern_export, content):
        log(OK, f"{nama} sudah di-export (tidak static) di {file_}")
    else:
        log(WARN, f"{nama}: definisi tidak ditemukan sama sekali di {file_} (mungkin nama/struktur beda di kernel ini)")

check_export(selinuxfs_c,
             r"static\s+ssize_t\s*\(\*write_op\[\]\)",
             r"(?<!static\s)ssize_t\s*\(\*write_op\[\]\)",
             "write_op[]", "security/selinux/selinuxfs.c")

check_export(selinuxfs_c,
             r"static\s+const\s+struct\s+file_operations\s+sel_handle_status_ops",
             r"(?<!static\s)(?<!static\s\s)const\s+struct\s+file_operations\s+sel_handle_status_ops",
             "sel_handle_status_ops", "security/selinux/selinuxfs.c")

check_export(selinuxfs_c,
             r"static\s+DEFINE_MUTEX\(sel_mutex\)",
             r"(?m)^DEFINE_MUTEX\(sel_mutex\)",
             "sel_mutex", "security/selinux/selinuxfs.c")

check_export(status_c,
             r"static\s+struct\s+page\s*\*\s*selinux_status_page",
             r"(?m)^struct\s+page\s*\*\s*selinux_status_page",
             "selinux_status_page", "security/selinux/ss/status.c")

check_export(status_c,
             r"static\s+DEFINE_MUTEX\(selinux_status_lock\)",
             r"(?m)^DEFINE_MUTEX\(selinux_status_lock\)",
             "selinux_status_lock", "security/selinux/ss/status.c")

check_export(services_c,
             r"static\s+DEFINE_RWLOCK\(policy_rwlock\)",
             r"(?m)^DEFINE_RWLOCK\(policy_rwlock\)",
             "policy_rwlock", "security/selinux/ss/services.c")

# selinux_ops hanya untuk kernel 4.2-, kernel ini 4.4 jadi info saja
if has(hooks_c, r"static\s+struct\s+security_operations\s+selinux_ops"):
    log(WARN, "selinux_ops masih static di security/selinux/hooks.c", "Hanya WAJIB untuk kernel <4.2, kernel Anda 4.4 jadi kemungkinan tidak dipakai — cek dulu apakah struct ini masih dipakai skema hook LSM di kernel ini.")
else:
    log(INFO, "selinux_ops: tidak ditemukan versi static lama — wajar untuk kernel 4.4 (LSM sudah pakai skema baru)")

# security_dump_masked_av / context_struct_compute_av: hanya wajib 6.6+, kernel 4.4 -> info saja
log(INFO, "security_dump_masked_av & context_struct_compute_av masih static — WAJAR, itu cuma wajib untuk kernel 6.6 ke atas")

# ---------------------------------------------------------------
# 4. Defconfig jasmine & wayne
# ---------------------------------------------------------------
section("4. Defconfig jasmine-stock & wayne")

defconfigs = {
    "jasmine-stock": "arch/arm64/configs/jasmine-stock_defconfig",
    "wayne": "arch/arm64/configs/wayne_defconfig",
}

required_keys = ["CONFIG_KSU", "CONFIG_KSU_MANUAL_HOOK"]

for name, path in defconfigs.items():
    content = read(path)
    print(f"\n--- {name} ({path}) ---")
    if content is None:
        log(FAIL, f"{name}: file defconfig tidak ditemukan di {path}")
        continue

    lines = content.splitlines()

    for key in required_keys:
        if re.search(rf"^{re.escape(key)}=y", content, re.M):
            log(OK, f"{name}: {key}=y ada")
        else:
            log(FAIL, f"{name}: {key}=y TIDAK ADA")

    if re.search(r"^CONFIG_KALLSYMS_ALL=y", content, re.M):
        log(OK, f"{name}: CONFIG_KALLSYMS_ALL=y aktif (export simbol SELinux manual jadi opsional)")
    else:
        log(WARN, f"{name}: CONFIG_KALLSYMS_ALL tidak aktif -> export simbol SELinux di atas WAJIB dibereskan")

    if not re.search(r"^CONFIG_KALLSYMS=y", content, re.M):
        log(WARN, f"{name}: baris 'CONFIG_KALLSYMS=y' tidak ditemukan secara eksplisit",
            "CONFIG_KALLSYMS_ALL butuh CONFIG_KALLSYMS. Jika tidak di-set di tempat lain (base defconfig arch), tambahkan 'CONFIG_KALLSYMS=y' secara eksplisit.")

    # cari duplikat baris config di file ini
    seen = {}
    dup_found = []
    for i, ln in enumerate(lines, 1):
        s = ln.strip()
        if not s or s.startswith("#") and "is not set" not in s:
            continue
        key_match = re.match(r"^(?:# )?(CONFIG_[A-Z0-9_]+)", s)
        if key_match:
            k = key_match.group(1)
            seen.setdefault(k, []).append(i)
    for k, ln_nums in seen.items():
        if len(ln_nums) > 1:
            dup_found.append((k, ln_nums))

    if dup_found:
        for k, ln_nums in dup_found:
            log(WARN, f"{name}: '{k}' muncul {len(ln_nums)}x (baris {ln_nums})", "Duplikat baris config, tidak fatal tapi sebaiknya dirapikan.")
    else:
        log(OK, f"{name}: tidak ada baris CONFIG_KSU* yang duplikat")

# ---------------------------------------------------------------
# 5. Ringkasan
# ---------------------------------------------------------------
section("RINGKASAN")

fail_count = sum(1 for s, *_ in report if s == FAIL)
warn_count = sum(1 for s, *_ in report if s == WARN)
ok_count   = sum(1 for s, *_ in report if s == OK)

for status, judul, detail in report:
    print(f"{status} {judul}")
    if detail:
        print(f"      -> {detail}")

print(f"\nTotal: {ok_count} OK, {warn_count} perlu diperiksa, {fail_count} hilang/wajib diperbaiki.")
if fail_count:
    print("Ada item berstatus [HILANG] yang menurut dokumentasi BakaSU WAJIB ada, kompilasi kemungkinan besar akan gagal / root tidak berfungsi kalau tidak dibereskan.")
else:
    print("Tidak ada hook wajib yang hilang. Item [PERIKSA] sifatnya rekomendasi/perlu ditinjau manual, bukan kesalahan pasti.")
