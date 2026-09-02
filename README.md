# Custom OC Kernel — Mi 6X (wayne) & Mi A2 (jasmine_sprout), SDM660

Berbasis source: `RanggaWbp/android_kernel_xiaomi_sdm660` (fork dari `LineageOS-MI-A2-MI-6X/android_kernel_xiaomi_sdm660`)

> **Belum termasuk di paket ini:** `.github/workflows/build-kernel.yml` (menyusul terakhir, sesuai permintaan).

## Struktur

```
configs/
├── normal-build.conf              # baseline global, di-include semua profile "normal"
├── mi6x/
│   ├── normal.conf
│   ├── oc_balanced.conf
│   ├── oc_performance.conf
│   └── oc_extreme.conf
└── mia2/
    ├── normal.conf
    ├── oc_balanced.conf
    ├── oc_performance.conf
    └── oc_extreme.conf

patches/
├── 0001-clk-cpu-osm-add-opt-in-speedbin-override.patch   # CPU: opt-in force speed-bin (efuse override)
└── 0002-adreno-a5xx-add-opt-in-speedbin-override.patch   # GPU: opt-in force speed-bin (efuse override)

scripts/
└── validate_oc_config.sh          # static safety check, WAJIB lolos sebelum compile

docs/
└── FREQUENCY_VOLTAGE_TABLE.md     # tabel freq/voltage/thermal lengkap + status verifikasi
```

## Cara pakai (manual, sebelum ada workflow)

```bash
git clone https://github.com/RanggaWbp/android_kernel_xiaomi_sdm660.git kernel
cd kernel

# 1. Terapkan patch (WAJIB untuk profile oc_*, TIDAK PERLU untuk normal)
git apply ../patches/0001-clk-cpu-osm-add-opt-in-speedbin-override.patch
git apply ../patches/0002-adreno-a5xx-add-opt-in-speedbin-override.patch

# 2. Validasi config sebelum compile (script exit non-zero kalau invalid)
bash ../scripts/validate_oc_config.sh ../configs/mi6x/oc_performance.conf

# 3. Tambahkan property override ke board DTS sesuai config yang dipilih, contoh
#    untuk configs/mi6x/oc_performance.conf (lihat isi file utk nilai exact):
#      &clock_cpu {
#          qcom,perfcl-force-speedbin = <0>;
#      };
#      &msm_gpu {           /* atau node kgsl-3d0 yg sesuai */
#          qcom,gpu-force-speed-bin = <157>;
#      };

# 4. Build seperti biasa
make ARCH=arm64 wayne_defconfig   # atau jasmine-stock_defconfig utk Mi A2
make ARCH=arm64 -j$(nproc)
```

## Kenapa hanya 2 patch, bukan patch besar per-profile?

Semua profile OC (balanced/performance/extreme) memakai **tabel frekuensi/voltage yang SUDAH ADA** di source (`sdm660.dtsi`, `sdm660-gpu.dtsi`, `sdm660-regulator.dtsi`) — hanya berbeda *speed-bin mana yang dipilih*. Karena pemilihan bin normalnya otomatis dari efuse, satu-satunya perubahan kode yang genuinely diperlukan adalah mekanisme *opt-in override* itu sendiri (patch 0001 untuk CPU OSM, patch 0002 untuk GPU Adreno). Tidak ada frequency/voltage baru yang "ditambahkan" ke source manapun — lihat `docs/FREQUENCY_VOLTAGE_TABLE.md` untuk rinciannya.

## Risk level & apa artinya

| Level | Arti |
|---|---|
| — (normal) | Tidak ada override sama sekali. Baseline murni. |
| LOW-MODERATE | Override GPU satu tingkat (135→146 efuse). CPU tidak disentuh. |
| HIGH | Override CPU big cluster ke bin terbaik (bin0) + GPU. |
| VERY-HIGH | Override CPU (little+big) + GPU sekaligus ke bin0/tabel tertinggi. |

**Yang TIDAK pernah dilakukan di profile manapun:** menaikkan voltage ceiling regulator, mematikan `qcom,therm-reset-temp` (115°C emergency shutdown), menghapus hotplug/throttle thermal, atau menambah frequency yang tidak ada di tabel source manapun.

## Testing matrix (WAJIB sebelum status naik dari NOT VERIFIED → HARDWARE TESTED)

```
NORMAL → BOOT TEST → IDLE TEST → LIGHT LOAD → CPU STRESS → GPU STRESS →
SUSTAINED LOAD → REBOOT TEST → CHARGING TEST → THERMAL TEST
```

Kompilasi berhasil **bukan** bukti stabilitas. Setiap profile `oc_*` punya field `HARDWARE_TESTED` di file config-nya — jangan ubah ke `true` tanpa hasil pengujian nyata di unit fisik, dan idealnya di lebih dari satu unit (karena override ini cross-bin, hasil bisa berbeda antar chip individual meski model sama).

## Known gaps (jujur, belum tuntas)

- `msm-thermal.c` (driver, bukan cuma DT) belum diaudit baris-per-baris untuk memastikan tidak ada jalur lain yang bisa menonaktifkan proteksi di atas.
- Testing matrix di atas belum pernah dijalankan ke hardware fisik oleh siapapun untuk konfigurasi cross-bin ini — semua status "SOURCE VERIFIED" murni soal keberadaan angka di source, bukan soal aman-tidaknya di unit Anda secara spesifik.
