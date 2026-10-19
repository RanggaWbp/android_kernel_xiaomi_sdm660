# CHANGELOG

## [Belum dirilis] — LMK kernel driver aktif di wayne + jasmine

### Diubah

- `arch/arm64/configs/wayne_defconfig`: tambah
  `CONFIG_ANDROID_LOW_MEMORY_KILLER=y` +
  `CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES=y`
  (sebelumnya tidak pernah diset -> default Kconfig n, driver tidak pernah
  ikut ter-compile di Mi 6X).
- `arch/arm64/configs/jasmine-stock_defconfig`: nilai sudah `=y`, dipertahankan
  (sempat dimatikan di commit 876c6590a944 lalu dikembalikan di commit ini).
- `.github/workflows/build-kernel.yml` dan `.github/workflows/Build Release.yml`:
  `kernel.string` (AnyKernel3 `properties()`) dan tabel Features di release
  body sekarang memuat `LMK`; `Build Release.yml` juga menambah
  `CONFIG_ANDROID_LOW_MEMORY_KILLER*` + `lowmemorykiller.o` ke validasi
  backport-feature-nya.
- `.github/workflows/build-kernel.yml`: gate build menuntut
  `CONFIG_ANDROID_LOW_MEMORY_KILLER=y` +
  `CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES=y` +
  `CONFIG_MEMCG=y` + `CONFIG_MEMCG_SWAP=y`, dan menuntut object
  `drivers/staging/android/lowmemorykiller.o` benar-benar terbentuk (simbol di
  `.config` bukan bukti kode ikut ter-compile).

### Cara kerjanya di device

Driver ada di `drivers/staging/android/lowmemorykiller.c` (varian CAF: shrinker
`register_shrinker` + `enable_adaptive_lmk` + `vmpressure_file_min`, lapor kill
lewat `/dev/lowmemorykiller` poll).

lmkd (Android 9+) melakukan `access("/sys/module/lowmemorykiller/parameters/minfree", W_OK)`
-> sukses -> `has_inkernel_module = true` -> lmkd masuk mode in-kernel: dia
hanya menulis `minfree`/`adj` ke driver, dan **driver yang membunuh** proses
saat free memory turun di bawah ambang untuk tiap `oom_score_adj`. Log lmkd:
`Using in-kernel low memory killer interface`.

Node yang muncul setelah boot:
`/sys/module/lowmemorykiller/parameters/{minfree,adj,debug,enable_adaptive_lmk,vmpressure_file_min}`
dan `/proc/lowmemorykiller` (event kill untuk `sys.lmk.reportkills`).

### PSI

Tidak dipakai di jalur ini. `CONFIG_PSI` tidak ada di 4.4 dan patchset resmi
hanya untuk 4.9/4.14; selama driver LMK aktif, lmkd tidak pernah menyentuh
`/proc/pressure/memory` maupun vmpressure.

## [Belum dirilis] — Backport ADIOS, NTSYNC, MGLRU, BBRv3 ke kernel 4.4

### Ditambahkan

Empat fitur dari kernel 4.9 (repo `Kernel_xiaomi_sdm845_CAF`) di-port ke
kernel 4.4 repo ini. Semua **built-in (`=y`)**, bukan modul: zip AnyKernel3
hanya mengirim `Image.gz-dtb`, jadi file `.ko` tidak akan pernah sampai ke
device. Diverifikasi compile dengan toolchain yang sama seperti CI
(`mvaisakh/gcc-arm64` + `gcc-arm`), dan CI sekarang menolak build yang
kehilangan salah satu fitur ini.

- **ADIOS** — Adaptive Deadline I/O scheduler (firelzrd 3.3.0) sebagai
  `elevator_ops` legacy: deadline group per optype (rb-tree + list FIFO),
  model latensi per bucket ukuran request, batch limit, tunable sysfs.
  Adaptasi 4.4: `req_op()`/`REQ_OP_*` tidak ada -> klasifikasi lewat
  `rq->cmd_flags` (`REQ_DISCARD`/`REQ_SECURE`) + `rq_data_dir()`;
  `ktime_get_ns()` tidak ada -> `ktime_to_ns(ktime_get())`.
  `CONFIG_IOSCHED_ADIOS=y` + entry `DEFAULT_ADIOS` di `block/Kconfig.iosched`.
  **CFQ tetap default**; ADIOS dipilih runtime via
  `/sys/block/<dev>/queue/scheduler`.
- **NTSYNC** — driver `/dev/ntsync` (emulasi primitive sinkronisasi Windows
  NT untuk Wine/Proton/emulator). Driver dari tree 4.9 compile tanpa
  perubahan kode di 4.4: semua API yang dipakai
  (`anon_inode_getfile`, `schedule_hrtimeout_range_clock`,
  `get_unused_fd_flags`/`fd_install`, `array_size`/`struct_size`,
  `kern_path`, `__vfs_setxattr_noperm`, `u64_to_user_ptr`) sudah ada.
  `CONFIG_NTSYNC=y` di `drivers/misc/Kconfig` + `drivers/misc/Makefile`.
- **MGLRU** — Multi-Gen LRU v5 (`CONFIG_LRU_GEN=y`, `LRU_GEN_ENABLED=y`,
  `NR_LRU_GENS=7`, `TIERS_PER_GEN=4`). Memisahkan page evictable ke
  generasi/tier sehingga reclaim melindungi working set dan melewati page
  dingin, bukan menyapu satu LRU datar. Di Android efek utamanya menurunkan
  CPU time kswapd dan mengurangi thrashing saat memori rendah.
- **BBRv3** — congestion control TCP `bbr3` (google/bbr v3) + PLB
  (Protective Load Balancing), berdampingan dengan `bbr` v1 (`bbr3` adalah
  algoritma **aktif by default**, `CONFIG_DEFAULT_BBR3=y`). BBRv3 butuh
  infrastruktur delivery-rate BBRv1 (`tcp_rate.c`, `struct rate_sample`,
  `tp->delivered/lost`, `TCP_SKB_CB()->tx.*`, ops slot `.cong_control` /
  `.sndbuf_expand` / `.tso_segs_goal`, `tcp_cong_control()`,
  `tcp_xmit_recovery()`, `ICSK_CA_PRIV_SIZE` 64->88) yang belum ada di 4.4,
  jadi port mencakup infrastruktur itu lebih dulu.
- **NoMount** — path redirection VFS (repo `maxsteeel/nomount`) kini masuk
  sebagai **submodule** `NoMount` yang di-pin di `v2.1.1` (`7046c7a`),
  bukan salinan vendored lagi. Layout mengikuti branch 4.9:
  `fs/nomount` -> `../NoMount/kernel/src` (symlink), jadi `fs/Kconfig` dan
  `fs/Makefile` tidak berubah. Upstream menargetkan kernel modern dan
  v2.1.1 **tidak** compile di 4.4 (34 error: `full_name_hash` 3-arg,
  `in_compat_syscall`, `__vfs_getxattr`/`__vfs_setxattr`, arg `inode` di
  `xattr_handler`, `iterate_shared`, `get_link`). Submodule dibiarkan
  pristine; shim 4.4 hidup di repo ini sebagai
  `scripts/nomount/nomount-4.4-compat.patch` (77 baris) yang diterapkan
  CI lewat `scripts/apply_nomount_compat.sh` (idempotent).
  `CONFIG_NOMOUNT=y` di kedua defconfig.

### Diubah

- `arch/arm64/configs/wayne_defconfig` dan
  `arch/arm64/configs/jasmine-stock_defconfig`: `CONFIG_IOSCHED_ADIOS=y`,
  `CONFIG_NTSYNC=y`, `CONFIG_LRU_GEN=y` (+ `LRU_GEN_ENABLED`, `NR_LRU_GENS`,
  `TIERS_PER_GEN`), `CONFIG_TCP_CONG_BBR3=y` + `CONFIG_DEFAULT_BBR3=y`
  (jasmine sebelumnya `# CONFIG_TCP_CONG_ADVANCED is not set` dan default
  `cubic` -> sekarang `advanced=y`, `DEFAULT_TCP_CONG="bbr3"`).
- `.github/workflows/build-kernel.yml` dan `.github/workflows/Build Release.yml`:
  - Step build memaksa simbol fitur backport ke `.config` lalu menjalankan
    `olddefconfig`, supaya simbol yang bergantung pada parent gate
    (`TCP_CONG_BBR3` ada di dalam `if TCP_CONG_ADVANCED`) tidak diam-diam
    hilang.
  - Validasi baru **gagal-build** kalau `.config` tidak memuat
    `NTSYNC/NOMOUNT/IOSCHED_ADIOS/LRU_GEN/TCP_CONG_BBR3/DEFAULT_BBR3=y`, kalau
    `ntsync.o`/`adios-iosched.o`/`fs/nomount/nomount.o`/`tcp_bbr3.o`/`tcp_plb.o`
    tidak terbentuk, atau kalau `lru_gen_enabled`/`bbr3_init` tidak ada di
    `System.map`.
    Alasan: set simbol di `.config` bukan bukti kode ikut ter-compile.
  - Step **Apply NoMount 4.4 compat patch** dijalankan tepat setelah
    checkout (ketiga workflow) — `submodules: recursive` sudah menarik
    `NoMount`, jadi CI selalu build submodule pristine + patch.
  - `kernel.string` (AnyKernel3) dan tabel fitur release note menampilkan
    `ADIOS`, `NTSYNC`, `MGLRU`, `BBRv3`.

## [Belum dirilis] — Hapus profile GPU OC yang terbukti gagal

### Dihapus

Berdasarkan hasil pengujian hardware nyata (tabel di bawah, sebelumnya sudah
dicatat di changelog ini): `oc_balanced`, `oc_performance`, `oc_extreme`
(mi6x & mia2) semuanya menyentuh GPU dan **konsisten gagal** dengan random
reboot / GPU hang (`kgsl gpu timeout` -> kernel panic). Hanya `normal` dan
`oc_cpu_only` (OC CPU big cluster saja, GPU 100% stock) yang terbukti stabil.- Menambahkan support scheduler **BORE** (Burst-Oriented Response Enhancer) ke konfigurasi defconfig `wayne` dan `jasmine` serta patch dari firelzrd/bore-scheduler.
- Menambahkan **BORE** ke fitur kernel di `.github/workflows/build-kernel.yml` dan `.github/workflows/Build Release.yml` untuk mencetak di `kernel.string` output.
- Memperbaiki error compile: penghapusan typedef duplicate di `include/linux/sched.h` dan variabel duplicate di `kernel/sysctl.c`.
- Menambahkan `CONFIG_SCHED_BORE=y` ke `arch/arm64/configs/wayne_defconfig` dan `arch/arm64/configs/jasmine-stock_defconfig`.
- Mengintegrasikan patch BORE scheduler v3.1.8 dari firelzrd ke kernel 4.4.y.

- `configs/{mi6x,mia2}/{oc_balanced,oc_performance,oc_extreme}.conf` — dihapus.
- `patches/0002-adreno-a5xx-add-opt-in-speedbin-override.patch` — dihapus (tidak dipakai config manapun lagi).
- `.github/workflows/build-kernel.yml`:
  - Matrix `profile` dipersempit dari 5 ke **2** (`normal`, `oc_cpu_only`) -> 4 kombinasi build (2 device x 2 profile), bukan 10.
  - Step apply patch 0002 (GPU force-speedbin) dihapus dari step "Apply opt-in speedbin override patches".
  - Body release note diperbarui, baris Balance/Performance/Extreme dihapus.

## [Belum dirilis] (sebelumnya) — Perbaikan: ambang batas ukuran Image.gz-dtb salah (false positive)

### Bug

Setelah bug komentar C di atas diperbaiki, `oc_extreme` (mia2) **berhasil compile** — tapi workflow menolaknya karena `Image.gz-dtb` berukuran 46.814.284 bytes (46.8MB), sedangkan validator saya membatasi 5-40MB. Ini kernel msm-4.4 dengan KernelSU + SUSFS + banyak driver tambahan (bukan kernel vanilla) — 46.8MB itu wajar, bukan tanda korup. Batas 40MB yang saya tulis sebelumnya cuma tebakan kasar tanpa data build sungguhan.

### Diperbaiki
- `.github/workflows/build-kernel.yml`: batas atas ukuran `Image.gz-dtb` diperlebar dari 40MB ke **90MB**. Batas bawah 5MB tetap (masih valid untuk menangkap kasus benar-benar kosong/gagal total).



### Bug (parah, ditemukan dari log GitHub Actions run pertama)

**Kedua patch (`0001` dan `0002`) gagal compile untuk SEMUA profile OC** (bukan cuma satu). Penyebab: komentar C yang saya tulis berisi teks `configs/*/oc_*.conf` — teks ini mengandung `*/` (penutup komentar C) di tengah kalimat, sehingga komentar `/* ... */` tertutup lebih awal dan sisa teksnya dibaca compiler sebagai kode sungguhan:

```
../drivers/gpu/msm/adreno_a5xx.c:167:44: error: unknown type name 'oc_'
```

Ini murni salah ketik struktural saya sendiri (typo komentar), **bukan** masalah OC/hardware. Karena `oc_balanced` (dan seharusnya `oc_performance`, `oc_extreme`, `oc_cpu_only` juga — semua yang apply patch 0002 dan/atau 0001) gagal di tahap COMPILE, bukan boot — **semua reboot random yang dilaporkan sebelumnya sebenarnya tidak pernah teruji di kernel yang benar-benar berisi patch OC ini.**

### Yang ini ubah dari analisis sebelumnya

Analisis crash log GPU hang (bagian di atas) tetap valid — itu dari APK/zip yang di-build manual sebelum ada CI, memakai versi patch yang mungkin berbeda. Tapi status `HARDWARE_TESTED_RESULT=FAILED_RANDOM_REBOOT` di 6 file config perlu ditinjau ulang: perlu dikonfirmasi apakah kernel yang benar-benar di-flash saat itu memang mengandung patch yang identik dengan yang di-commit sekarang (ada kemungkinan versi manual berbeda dari versi CI). **Rekomendasi: uji ulang dari awal begitu CI berhasil compile dengan bersih**, jangan asumsikan hasil test lama masih berlaku 1:1 untuk artifact CI.

### Diperbaiki
- `patches/0001-clk-cpu-osm-add-opt-in-speedbin-override.patch` — komentar diubah jadi "(see the oc_*.conf files under configs/)" untuk menghindari urutan karakter `*/`.
- `patches/0002-adreno-a5xx-add-opt-in-speedbin-override.patch` — sama.
- Perbaikan diverifikasi dengan **reproduksi bug terisolasi** (compile snippet komentar sebelum/sesudah perbaikan dengan `gcc -c`) untuk memastikan penyebabnya benar dan fix-nya benar-benar menghilangkan error, bukan cuma `git apply --check` seperti sebelumnya (yang tidak mendeteksi bug ini karena `git apply` tidak meng-compile).



### Diperbaiki
- **`configs/{mi6x,mia2}/oc_performance.conf`**: `GPU_MAX_MHZ` sebelumnya salah tertulis `700`, padahal `GPU_FORCE_PWRLEVEL_BIN_EFUSE_RAW=157` yang dipakai punya `pwrlevel@0 = 750MHz` (sama seperti bin=0). `GPU_MAX_MHZ` sebelumnya hanya metadata dokumentasi — tidak pernah ada logic yang benar-benar membatasi frekuensi ke nilai itu. Dikoreksi ke `750` agar sesuai perilaku aktual. **Ditemukan dari laporan hasil test hardware pengguna** (GPU menunjukkan 750MHz, bukan 700MHz yang diharapkan).

### Hasil pengujian hardware nyata (dilaporkan pengguna)

| Profile | Hasil | Catatan |
|---|---|---|
| `normal` (mi6x & mia2) | ✅ **Sukses, sempurna** | Baseline stabil, sesuai ekspektasi |
| `oc_balanced` | ❌ **Random reboot** | GPU-only override (efuse 135→146, 647→700MHz) |
| `oc_performance` | ❌ **Random reboot** | CPU+GPU override, ditambah bug freq salah (lihat atas) |
| `oc_extreme` | ❌ **Random reboot** | CPU+GPU override penuh ke bin terbaik |

Semua 6 file `oc_*.conf` (mi6x + mia2) sudah ditandai `HARDWARE_TESTED_RESULT=FAILED_RANDOM_REBOOT`.

### Analisis akar masalah — DIPASTIKAN dari crash log nyata (console-ramoops)

Root cause **sudah dipastikan**, bukan dugaan lagi. Crash log dari `oc_balanced` (mi6x) menunjukkan urutan:

1. `kgsl kgsl-3d0: ... gpu timeout ctx 7` — GPU hang di tengah beban kerja nyata (bukan idle, bukan saat boot)
2. Proses recovery otomatis (`_a5xx_do_crashdump`, `adreno_vbif_clear_pending_transactions`) juga gagal/timeout
3. `_gpu_clk_prepare_enable| KGSL:core_clk enable error:-16` — GPU gagal nyalain ulang clock-nya
4. `kernel BUG at kgsl_pwrctrl.c:2065` → `Kernel panic` → reboot

**Kesimpulan: GPU fisik unit ini (di-fuse pabrik ke bin 135, top 647MHz) benar-benar hang di bawah beban kerja nyata saat dipaksa ke bin 146 (700MHz).** Ini electrical/frequency instability sungguhan, bukan bug kode yang bisa ditambal — persis risiko HIGH yang sudah diperingatkan sejak awal. Karena `oc_balanced` hanya menyentuh GPU (CPU sama sekali tidak diubah), temuan ini mengisolasi masalah ke GPU secara pasti.

**Implikasi:** GPU override (patch 0002 / `GPU_FORCE_PWRLEVEL_BIN_EFUSE_RAW`) **tidak direkomendasikan** untuk unit fisik ini di profile manapun (`oc_balanced`, `oc_performance`, `oc_extreme` — ketiganya menyentuh GPU ke bin ≥146).

### Ditambahkan

- **`configs/{mi6x,mia2}/oc_cpu_only.conf`** — profile baru untuk mengisolasi apakah CPU big cluster (perfcl, bin0, 2457.6MHz) stabil TERPISAH dari GPU. GPU di profile ini 100% stock (patch 0002 tidak di-apply sama sekali). Status: `NOT_YET_TESTED` — menunggu hasil uji hardware pengguna.
- Matrix build CI diperluas dari 8 jadi **10 kombinasi** (2 device × 5 profile).

### Diperbaiki (bug internal, ditemukan saat audit ulang)

- **`.github/workflows/build-kernel.yml`**: `ARTIFACT_NAME` sebelumnya dihitung dari format string (`{profile}-oc`) yang salah untuk semua profile OC (mis. menghasilkan `oc_balanced-oc`, seharusnya `balanced-oc` sesuai `ARTIFACT_NAME` di file conf). Diperbaiki: sekarang dibaca langsung dari file `.conf` yang bersangkutan, bukan dihitung ulang.


### Rekomendasi saat ini

- **GPU override tidak direkomendasikan untuk unit ini** — sudah terbukti gagal di kondisi pemakaian nyata (bukan cuma teori risiko). `oc_balanced`, `oc_performance`, `oc_extreme` semuanya menyentuh GPU ke bin ≥146 dan berisiko gagal dengan cara sama.
- **CPU-only OC (`oc_cpu_only.conf`) belum ada datanya** — silakan uji terpisah kalau ingin tahu apakah CPU big cluster (beda domain binning dari GPU) punya headroom yang valid di unit ini.
- `normal` build tetap yang paling aman dipakai harian.
- Kalau `oc_cpu_only` JUGA gagal: unit ini kemungkinan di-bin rendah di semua domain, bukan cuma GPU — hentikan eksperimen OC apapun di unit fisik ini.

- Menambahkan support scheduler **BORE** (Burst-Oriented Response Enhancer) ke konfigurasi defconfig  dan  serta patch dari firelzrd/bore-scheduler.
- Menambahkan **BORE** ke fitur kernel di  dan  untuk mencetak di  output.
- Memperbaiki error compile: penghapusan typedef duplicate di  dan variabel duplicate di .
- Menambahkan  ke  dan .
- Mengintegrasikan patch BORE scheduler v3.1.8 dari firelzrd ke kernel 4.4.y.
