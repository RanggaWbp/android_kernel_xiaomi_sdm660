# CHANGELOG

## [Belum dirilis] — Update setelah pengujian hardware nyata

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
