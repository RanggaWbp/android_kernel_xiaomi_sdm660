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

### Analisis akar masalah (belum tuntas, butuh crash log)

Ditemukan bahwa jumlah `qcom,gpu-pwrlevel@N` **berbeda antar speed-bin** di `sdm660-gpu.dtsi`:

| Speed-bin | Jumlah pwrlevel | Dipakai di profile |
|---|---|---|
| 135 (bin fisik asli unit) | 7 | `normal` (stock) |
| 146 | 8 | `oc_balanced` |
| 157 | 9 | `oc_performance` |
| 0 | 9 | `oc_extreme` |

Karena **`oc_balanced` yang hanya mengubah GPU (bin 135→146) saja sudah gagal**, ini mengisolasi masalah ke sisi GPU override (patch 0002 / mekanisme force-speed-bin GPU), bukan ke sisi CPU (patch 0001) — CPU sama sekali tidak disentuh di `oc_balanced`.

Dua hipotesis yang belum bisa dibedakan tanpa crash log nyata:
1. **Electrical instability** — silikon fisik unit ini (di-fuse ke bin 135) memang tidak divalidasi Qualcomm untuk voltage/frekuensi di bin manapun yang lebih tinggi.
2. **Structural bug** — ada tabel lain (kandidat: `qcom,msm-bus,num-cases = <14>` bus-bandwidth vote table) yang berasumsi jumlah pwrlevel tetap, sehingga pergantian jumlah level (7→8/9) menyebabkan akses array di luar batas.

**Status: menunggu `console-ramoops`/`last_kmsg` dari unit yang gagal untuk memastikan.**

### Rekomendasi sementara

- **JANGAN** lanjut flash ulang `oc_balanced`/`oc_performance`/`oc_extreme` ke unit fisik sampai root cause di atas dikonfirmasi lewat crash log — reboot berulang tanpa diagnosis berisiko memicu filesystem corruption (salah satu failure mode yang eksplisit ingin dihindari di brief awal).
- `normal` build aman dipakai sebagai daily driver sambil menunggu diagnosis.
