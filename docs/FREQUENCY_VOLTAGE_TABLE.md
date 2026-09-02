# Tabel Frequency/Voltage — SDM660 (wayne & jasmine_sprout)

**Source:** `RanggaWbp/android_kernel_xiaomi_sdm660` (branch `master`)
Semua nilai di bawah **SOURCE VERIFIED** langsung dari:
- `arch/arm/boot/dts/qcom/sdm660.dtsi` (node `clock_cpu`, driver `qcom,clk-cpu-osm`)
- `arch/arm/boot/dts/qcom/sdm660-gpu.dtsi` (node `qcom,gpu-pwrlevel-bins`)
- `drivers/clk/qcom/clk-cpu-osm.c` (mekanisme pemilihan speed-bin via efuse)

wayne (Mi 6X) dan jasmine_sprout (Mi A2) meng-include file dtsi yang **sama persis** untuk clock CPU/GPU (`#include "sdm660.dtsi"` dan GPU dtsi yang sama) — jadi tabel di bawah berlaku untuk kedua device. Perbedaan device hanya di panel/sensor/regulator periferal, bukan di sini.

## 1. CPU — Little cluster (pwrcl)

| Freq (MHz) | Voltage corner | Speed-bin tersedia |
|---|---|---|
| 300 | 1 | semua bin |
| 633.6 | 1 | semua bin |
| 902.4 | 1 | semua bin |
| 1113.6 | 2 | semua bin |
| 1401.6 | 2 | semua bin |
| 1536.0 | 2 | semua bin |
| 1612.8 | 2 | **hanya bin3** (top freq bin3) |
| 1747.2 | 2 | bin0, bin1, bin4 |
| 1843.2 | 3 | **bin0, bin1, bin4 (top freq)** |

Unit uji Anda (live monitor: 1843MHz) = **bin0/1/4**.

## 2. CPU — Big cluster (perfcl)

| Freq (MHz) | Voltage corner | Speed-bin tersedia |
|---|---|---|
| 300 | 1 | semua bin |
| 1113.6 | 1 | semua bin |
| 1401.6 | 2 | semua bin |
| 1747.2 | 2 | semua bin |
| 1804.8 | 2 | **hanya bin3 (top)** |
| 1958.4 | 2 | bin0, bin1, **bin4 (top)** |
| 2150.4 | 2 | bin0, bin1 |
| 2208.0 | 3 | **bin1 (top)** — cocok dengan unit uji Anda |
| 2457.6 | 3 | **bin0 (top)** — best-binned silicon only |

## 3. GPU — Adreno 512 (`qcom,gpu-pwrlevel-bins`, dipilih via efuse GPU terpisah dari efuse CPU)

Catatan: nilai `qcom,speed-bin` GPU adalah efuse mentah (bukan indeks 0/1/2/3 sederhana seperti CPU).

| Freq tertinggi (MHz) | `qcom,speed-bin` (efuse raw) | Keterangan |
|---|---|---|
| 750 | `0` | Best-binned |
| 750 | `157` | |
| 700 | `146` | |
| 647 | `135` | **Kemungkinan bin unit uji Anda** (cocok dgn live monitor: max 647MHz) |
| — | `78`, `90`, `122` | Varian tabel lain (belum diaudit detail freq-nya, ada di sdm660-gpu.dtsi) |

Tabel freq lengkap per level (SVS/NOM/TURBO) untuk semua bin di atas identik pada level menengah ke bawah (19.2/160/266/370/465/588MHz) — hanya level teratas yang berbeda per bin, sesuai tabel di atas.

## 4. Voltage aktual (µV) per corner — SOURCE VERIFIED

**Sumber:** `arch/arm/boot/dts/qcom/sdm660-regulator.dtsi` (node `apc0_pwrcl_vreg`, `apc1_perfcl_vreg`, `gfx_vreg_corner`). Ini adalah rentang closed-loop CPR (Core Power Reduction) — voltage aktual diatur otomatis oleh hardware CPR di antara floor dan ceiling ini, bukan nilai tetap.

**CPU little cluster (apc0_pwrcl_vreg), corner 1–8:**
| Corner | Voltage floor (µV) | Voltage ceiling (µV) |
|---|---|---|
| 1–3 | 588000–596000 | 724000 |
| 4 | 652000 | 788000 |
| 5 | 712000 | 868000 |
| 6–8 | 744000–844000 | 1068000 |

**CPU big cluster (apc1_perfcl_vreg), corner 1–7:**
| Corner | Voltage floor (µV) | Voltage ceiling (µV) |
|---|---|---|
| 1–2 | 588000–596000 | 724000 |
| 3 | 652000 | 788000 |
| 4 | 712000 | 868000 |
| 5–6 | 744000–784000 | 988000 |
| 7 | 844000 | 1068000 |

**GPU (gfx_vreg_corner), corner 0–6:**
| Corner | Voltage floor (µV) | Voltage ceiling (µV) |
|---|---|---|
| 0–1 | 504000 | 585000–645000 |
| 2 | 596000 | 725000 |
| 3 | 652000 | 790000 |
| 4 | 712000 | 870000 |
| 5 | 744000 | 925000 |
| 6 | 1070000 | 1070000 |

➡️ Batas voltage untuk profile extreme (corner 7/8 CPU, corner 6 GPU) **tidak melebihi ceiling di atas** — semua profile OC di repo ini murni memilih ulang titik freq/corner yang sudah ada, bukan menaikkan ceiling regulator.

## 5. Thermal limits — SOURCE VERIFIED

**Sumber:** `arch/arm/boot/dts/qcom/msm8998.dtsi`, node `qcom,msm-thermal`

| Parameter | Nilai | Keterangan |
|---|---|---|
| `qcom,therm-reset-temp` | **115°C** | Emergency shutdown — WAJIB tetap aktif di semua profile |
| `qcom,core-limit-temp` | 70°C | Mulai throttle frequency |
| `qcom,core-temp-hysteresis` | 10°C | Hysteresis sebelum unthrottle |
| `qcom,hotplug-temp` | 105°C | Core mulai di-offline |
| `qcom,hotplug-temp-hysteresis` | 20°C | Hysteresis sebelum core online lagi |
| `qcom,vdd-restriction-temp` | 5°C | Batas bawah (cold) untuk vdd restriction |
| `qcom,poll-ms` | 100ms | Interval polling sensor |

**Aturan tegas untuk SEMUA profile termasuk extreme:** ke-6 parameter di atas tidak boleh diubah/dihapus dari DTS manapun. `validate_oc_config.sh` menolak build jika `THERMAL_BYPASS=true` atau `THERMAL_PROTECTION!=enabled`.

## 6. Status verifikasi final

| Item | Status |
|---|---|
| Tabel CPU/GPU frequency (bagian 1–3) | **SOURCE VERIFIED** |
| Voltage floor/ceiling per corner (bagian 4) | **SOURCE VERIFIED** |
| Thermal limits (bagian 5) | **SOURCE VERIFIED** |
| Stabilitas profile balanced/performance/extreme di hardware nyata | **NOT VERIFIED** — wajib lolos testing matrix (lihat README) sebelum boleh diklaim "HARDWARE TESTED" |

## 7. Perbedaan Normal vs Balanced vs Performance vs Extreme

| Aspek | Normal | Balanced OC | Performance OC | Extreme OC |
|---|---|---|---|---|
| CPU little max | stock (bin apa adanya) | stock (tidak diubah) | stock (tidak diubah) | force bin0 → 1843.2MHz (**sama dgn stock bin1**, tanpa gain) |
| CPU big max | stock (bin apa adanya) | stock (tidak diubah) | **force bin0 → 2457.6MHz** | force bin0 → 2457.6MHz |
| GPU max | stock (647MHz jika efuse=135) | force efuse=146 → **700MHz** | force efuse=157 → 700MHz (tabel top 750, dibatasi 700 via GPU_MAX_MHZ) | force efuse=0 → **750MHz** |
| Cross-bin? | Tidak | Ya (GPU saja) | Ya (CPU big + GPU) | Ya (CPU penuh + GPU penuh) |
| Risk level | — | LOW-MODERATE | HIGH | VERY-HIGH |
| Thermal protection | aktif | aktif | aktif | aktif (wajib, tidak boleh dimatikan) |
| Butuh patch driver? | Tidak | Ya (`0002-adreno-a5xx-*`) | Ya (`0001` + `0002`) | Ya (`0001` + `0002`) |
| Boleh diklaim "stabil"? | Ya (baseline) | Tidak sampai lolos testing matrix | Tidak sampai lolos testing matrix | Tidak sampai lolos testing matrix |

**Catatan penting:** karena little cluster sudah berada di frekuensi maksimum source pada bin0/1/4, profile Extreme *tidak* memberi keuntungan tambahan pada little cluster dibanding kondisi stock bin1 — ini dilaporkan apa adanya, bukan disamarkan sebagai "gain OC".
