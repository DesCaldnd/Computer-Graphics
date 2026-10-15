# VRS — material for the coordinator to merge (2026-10-15)

Everything else of the area is already in place: `docs/dev/modules/render_vrs.md` (final), `render.md` §9c,
`rhi.md` "Variable rate shading", guide chapter 27 step 5 (new; profiling became step 6) and the Shading table of
chapter 26 (`r.VRS`, `.Threshold`, `.MaxRate` rows + "По умолч. ≠ High" note).

## 1. `docs/dev/perf.md` — new section (put it inside "NVIDIA RTX 3080 (2026-10)", after the ray tracing table)

### Variable rate shading — `ShowcaseVrs.StationsQualityAndPerformance` (`OX_VRS_PERF=1 OX_VULKAN_VALIDATION=0`)

`ox_showcase_tests`, RelWithDebInfo, validation off, 2026-10-14 23:53–00:12 under the GPU lock (other agents'
functional GPU tests may have run at the same time). Every row is a fresh headless engine: station in photo mode at
quality High with the VRS cvars overridden, fixed 1/60 s step, medians of frames 121–180 (GPU frame, the passes that
get the rate image = "affected", the VRS passes), quality of frame 181 against the r.VRS-off frame of the same set
(HUD FPS counter masked). TAA on. "pan" = camera yawing 0.6°/frame from frame 60, compared with the panning Off run.
τ = `r.VRS.Threshold`, 2×2 / 4×4 = `r.VRS.MaxRate`. Presets: Low = τ 0.05, 4×4; Medium = τ 0.03, 2×2; High / Ultra
off (τ 0.02, 2×2 if switched on). Outliers: 06_World 1080p Off frame (2.85 ms against 2.61 in the repeat) and the
02_Materials 1080p Foveated run (a 5 ms hitch, VRS passes 0.18 ms) — read the affected-pass column there.
Reproduce: `OX_VRS_PERF=1 OX_VULKAN_VALIDATION=0 OX_VRS_OUT=E:/... ox_showcase_tests --gtest_filter=ShowcaseVrs.*`
(~20 min, ~1 GB of PNGs); raw CSV of this run: `E:/Development/OxwaldEngine/vrs_perf/results_v4.csv`.

| station | size | configuration | GPU frame ms | VRS passes ms | affected passes ms | coarse tiles | PSNR dB | SSIM | FLIP |
|---|---|---|---|---|---|---|---|---|---|
| 01_Lighting | 1080p | Off | 2.75 | — | 0.68 | — | — | — | — |
| 01_Lighting | 1080p | Off again (noise) | 2.75 (-0 %) | 0.00 | 0.67 (-0.00) | 0 % | ∞ | 1.0000 | 0.0000 |
| 01_Lighting | 1080p | τ 0.02, 2×2 (High/Ultra if on) | 2.66 (-3 %) | 0.04 | 0.54 (-0.13) | 73 % | 46.9 | 0.9953 | 0.0086 |
| 01_Lighting | 1080p | τ 0.03, 2×2 (**Medium**) | 2.62 (-5 %) | 0.04 | 0.51 (-0.17) | 81 % | 46.4 | 0.9946 | 0.0093 |
| 01_Lighting | 1080p | τ 0.05, 2×2 | 2.61 (-5 %) | 0.04 | 0.49 (-0.18) | 84 % | 45.8 | 0.9935 | 0.0102 |
| 01_Lighting | 1080p | τ 0.05, 4×4 (**Low**) | 2.58 (-6 %) | 0.04 | 0.46 (-0.21) | 84 % | 41.1 | 0.9872 | 0.0158 |
| 01_Lighting | 1080p | τ 0.08, 2×2 (old High) | 2.58 (-6 %) | 0.04 | 0.48 (-0.19) | 84 % | 45.2 | 0.9926 | 0.0106 |
| 01_Lighting | 1080p | Motion, 4×4 | 2.77 (+1 %) | 0.04 | 0.67 (-0.01) | 0 % | ∞ | 1.0000 | 0.0000 |
| 01_Lighting | 1080p | Foveated, 4×4 | 2.69 (-2 %) | 0.04 | 0.57 (-0.10) | 55 % | 42.9 | 0.9928 | 0.0111 |
| 01_Lighting | 1080p | pan: Off | 2.58 | — | 0.53 | — | — | — | — |
| 01_Lighting | 1080p | pan: τ 0.03, 2×2 | 2.42 (-6 %) | 0.04 | 0.33 (-0.20) | 85 % | 51.2 | 0.9985 | 0.0091 |
| 01_Lighting | 1080p | pan: Motion, 4×4 | 2.45 (-5 %) | 0.04 | 0.35 (-0.18) | 69 % | 55.6 | 0.9991 | 0.0084 |
| 01_Lighting | 4K | Off | 7.63 | — | 1.98 | — | — | — | — |
| 01_Lighting | 4K | Off again (noise) | 7.69 (+1 %) | 0.00 | 1.99 (+0.01) | 0 % | ∞ | 1.0000 | 0.0000 |
| 01_Lighting | 4K | τ 0.02, 2×2 (High/Ultra if on) | 7.11 (-7 %) | 0.13 | 1.36 (-0.62) | 81 % | 49.8 | 0.9972 | 0.0066 |
| 01_Lighting | 4K | τ 0.03, 2×2 (**Medium**) | 7.04 (-8 %) | 0.13 | 1.30 (-0.68) | 84 % | 49.3 | 0.9966 | 0.0071 |
| 01_Lighting | 4K | τ 0.05, 2×2 | 7.02 (-8 %) | 0.13 | 1.24 (-0.74) | 85 % | 48.5 | 0.9955 | 0.0078 |
| 01_Lighting | 4K | τ 0.05, 4×4 (**Low**) | 7.03 (-8 %) | 0.13 | 1.13 (-0.86) | 85 % | 43.4 | 0.9903 | 0.0129 |
| 01_Lighting | 4K | τ 0.08, 2×2 (old High) | 6.96 (-9 %) | 0.13 | 1.24 (-0.75) | 86 % | 48.1 | 0.9952 | 0.0080 |
| 01_Lighting | 4K | Motion, 4×4 | 7.84 (+3 %) | 0.13 | 1.98 (-0.01) | 0 % | ∞ | 1.0000 | 0.0000 |
| 01_Lighting | 4K | Foveated, 4×4 | 7.35 (-4 %) | 0.13 | 1.65 (-0.33) | 54 % | 45.8 | 0.9954 | 0.0076 |
| 01_Lighting | 4K | pan: Off | 7.47 | — | 1.64 | — | — | — | — |
| 01_Lighting | 4K | pan: τ 0.03, 2×2 | 6.88 (-8 %) | 0.13 | 0.97 (-0.68) | 87 % | 54.8 | 0.9990 | 0.0071 |
| 01_Lighting | 4K | pan: Motion, 4×4 | 6.91 (-7 %) | 0.13 | 1.03 (-0.62) | 69 % | 56.7 | 0.9993 | 0.0071 |
| 02_Materials | 1080p | Off | 2.56 | — | 0.21 | — | — | — | — |
| 02_Materials | 1080p | Off again (noise) | 2.57 (+0 %) | 0.00 | 0.21 (+0.00) | 0 % | 66.6 | 0.9999 | 0.0015 |
| 02_Materials | 1080p | τ 0.02, 2×2 (High/Ultra if on) | 2.60 (+2 %) | 0.07 | 0.20 (-0.01) | 32 % | 56.4 | 0.9991 | 0.0044 |
| 02_Materials | 1080p | τ 0.03, 2×2 (**Medium**) | 2.59 (+1 %) | 0.07 | 0.19 (-0.02) | 45 % | 48.9 | 0.9951 | 0.0076 |
| 02_Materials | 1080p | τ 0.05, 2×2 | 2.56 (-0 %) | 0.07 | 0.16 (-0.05) | 69 % | 41.1 | 0.9742 | 0.0173 |
| 02_Materials | 1080p | τ 0.05, 4×4 (**Low**) | 2.56 (-0 %) | 0.07 | 0.16 (-0.05) | 69 % | 39.9 | 0.9636 | 0.0206 |
| 02_Materials | 1080p | τ 0.08, 2×2 (old High) | 2.54 (-1 %) | 0.07 | 0.15 (-0.07) | 81 % | 37.8 | 0.9536 | 0.0254 |
| 02_Materials | 1080p | Motion, 4×4 | 2.64 (+3 %) | 0.07 | 0.21 (-0.00) | 0 % | 68.6 | 1.0000 | 0.0009 |
| 02_Materials | 1080p | Foveated, 4×4 | 7.86 (+207 %) | 0.18 | 0.43 (+0.21) | 55 % | 34.8 | 0.9450 | 0.0250 |
| 02_Materials | 1080p | pan: Off | 2.29 | — | 0.15 | — | — | — | — |
| 02_Materials | 1080p | pan: τ 0.03, 2×2 | 2.29 (+0 %) | 0.04 | 0.10 (-0.05) | 81 % | 45.5 | 0.9894 | 0.0190 |
| 02_Materials | 1080p | pan: Motion, 4×4 | 2.25 (-2 %) | 0.04 | 0.10 (-0.05) | 49 % | 51.4 | 0.9979 | 0.0132 |
| 02_Materials | 4K | Off | 6.68 | — | 0.70 | — | — | — | — |
| 02_Materials | 4K | Off again (noise) | 6.58 (-2 %) | 0.00 | 0.69 (-0.01) | 0 % | ∞ | 1.0000 | 0.0000 |
| 02_Materials | 4K | τ 0.02, 2×2 (High/Ultra if on) | 6.73 (+1 %) | 0.24 | 0.59 (-0.11) | 51 % | 53.1 | 0.9977 | 0.0066 |
| 02_Materials | 4K | τ 0.03, 2×2 (**Medium**) | 6.72 (+0 %) | 0.24 | 0.53 (-0.17) | 65 % | 48.1 | 0.9934 | 0.0097 |
| 02_Materials | 4K | τ 0.05, 2×2 | 6.54 (-2 %) | 0.24 | 0.45 (-0.25) | 80 % | 42.9 | 0.9820 | 0.0152 |
| 02_Materials | 4K | τ 0.05, 4×4 (**Low**) | 6.56 (-2 %) | 0.23 | 0.42 (-0.28) | 80 % | 40.8 | 0.9677 | 0.0218 |
| 02_Materials | 4K | τ 0.08, 2×2 (old High) | 6.61 (-1 %) | 0.24 | 0.41 (-0.29) | 85 % | 40.3 | 0.9723 | 0.0195 |
| 02_Materials | 4K | Motion, 4×4 | 6.91 (+3 %) | 0.24 | 0.70 (-0.00) | 0 % | 74.7 | 1.0000 | 0.0002 |
| 02_Materials | 4K | Foveated, 4×4 | 6.67 (-0 %) | 0.24 | 0.48 (-0.22) | 54 % | 37.4 | 0.9637 | 0.0187 |
| 02_Materials | 4K | pan: Off | 6.22 | — | 0.53 | — | — | — | — |
| 02_Materials | 4K | pan: τ 0.03, 2×2 | 6.02 (-3 %) | 0.13 | 0.30 (-0.23) | 86 % | 47.4 | 0.9926 | 0.0156 |
| 02_Materials | 4K | pan: Motion, 4×4 | 6.08 (-2 %) | 0.13 | 0.30 (-0.23) | 48 % | 50.2 | 0.9963 | 0.0126 |
| 04_Volumetrics | 1080p | Off | 3.57 | — | 0.37 | — | — | — | — |
| 04_Volumetrics | 1080p | Off again (noise) | 3.55 (-1 %) | 0.00 | 0.37 (-0.00) | 0 % | ∞ | 1.0000 | 0.0000 |
| 04_Volumetrics | 1080p | τ 0.02, 2×2 (High/Ultra if on) | 3.45 (-3 %) | 0.04 | 0.22 (-0.14) | 82 % | 52.0 | 0.9972 | 0.0085 |
| 04_Volumetrics | 1080p | τ 0.03, 2×2 (**Medium**) | 3.45 (-3 %) | 0.04 | 0.22 (-0.15) | 85 % | 49.9 | 0.9960 | 0.0094 |
| 04_Volumetrics | 1080p | τ 0.05, 2×2 | 3.43 (-4 %) | 0.04 | 0.21 (-0.16) | 87 % | 46.2 | 0.9930 | 0.0112 |
| 04_Volumetrics | 1080p | τ 0.05, 4×4 (**Low**) | 3.41 (-4 %) | 0.04 | 0.19 (-0.18) | 87 % | 43.9 | 0.9872 | 0.0157 |
| 04_Volumetrics | 1080p | τ 0.08, 2×2 (old High) | 3.47 (-3 %) | 0.04 | 0.21 (-0.16) | 87 % | 43.4 | 0.9904 | 0.0130 |
| 04_Volumetrics | 1080p | Motion, 4×4 | 3.91 (+10 %) | 0.04 | 0.36 (-0.01) | 0 % | ∞ | 1.0000 | 0.0000 |
| 04_Volumetrics | 1080p | Foveated, 4×4 | 3.49 (-2 %) | 0.04 | 0.25 (-0.11) | 55 % | 39.1 | 0.9863 | 0.0139 |
| 04_Volumetrics | 1080p | pan: Off | 3.27 | — | 0.26 | — | — | — | — |
| 04_Volumetrics | 1080p | pan: τ 0.03, 2×2 | 3.20 (-2 %) | 0.04 | 0.15 (-0.12) | 86 % | 49.3 | 0.9977 | 0.0102 |
| 04_Volumetrics | 1080p | pan: Motion, 4×4 | 3.24 (-1 %) | 0.04 | 0.17 (-0.09) | 68 % | 49.2 | 0.9989 | 0.0091 |
| 04_Volumetrics | 4K | Off | 8.61 | — | 1.26 | — | — | — | — |
| 04_Volumetrics | 4K | Off again (noise) | 8.53 (-1 %) | 0.00 | 1.26 (+0.00) | 0 % | ∞ | 1.0000 | 0.0000 |
| 04_Volumetrics | 4K | τ 0.02, 2×2 (High/Ultra if on) | 8.76 (+2 %) | 0.14 | 0.65 (-0.61) | 86 % | 53.6 | 0.9976 | 0.0074 |
| 04_Volumetrics | 4K | τ 0.03, 2×2 (**Medium**) | 7.98 (-7 %) | 0.14 | 0.64 (-0.62) | 87 % | 51.8 | 0.9968 | 0.0081 |
| 04_Volumetrics | 4K | τ 0.05, 2×2 | 7.92 (-8 %) | 0.14 | 0.63 (-0.64) | 87 % | 49.6 | 0.9958 | 0.0089 |
| 04_Volumetrics | 4K | τ 0.05, 4×4 (**Low**) | 7.91 (-8 %) | 0.14 | 0.53 (-0.74) | 87 % | 46.4 | 0.9908 | 0.0125 |
| 04_Volumetrics | 4K | τ 0.08, 2×2 (old High) | 7.87 (-9 %) | 0.14 | 0.62 (-0.65) | 87 % | 47.5 | 0.9950 | 0.0097 |
| 04_Volumetrics | 4K | Motion, 4×4 | 8.55 (-1 %) | 0.14 | 1.24 (-0.02) | 0 % | ∞ | 1.0000 | 0.0000 |
| 04_Volumetrics | 4K | Foveated, 4×4 | 8.27 (-4 %) | 0.14 | 0.82 (-0.44) | 54 % | 43.7 | 0.9930 | 0.0093 |
| 04_Volumetrics | 4K | pan: Off | 8.15 | — | 1.00 | — | — | — | — |
| 04_Volumetrics | 4K | pan: τ 0.03, 2×2 | 7.70 (-6 %) | 0.13 | 0.50 (-0.51) | 87 % | 51.4 | 0.9986 | 0.0070 |
| 04_Volumetrics | 4K | pan: Motion, 4×4 | 7.75 (-5 %) | 0.13 | 0.56 (-0.45) | 67 % | 52.6 | 0.9994 | 0.0060 |
| 05_Water | 1080p | Off | 2.85 | — | 0.68 | — | — | — | — |
| 05_Water | 1080p | Off again (noise) | 2.85 (+0 %) | 0.00 | 0.68 (+0.00) | 0 % | 72.4 | 1.0000 | 0.0007 |
| 05_Water | 1080p | τ 0.02, 2×2 (High/Ultra if on) | 2.83 (-0 %) | 0.07 | 0.59 (-0.09) | 51 % | 54.8 | 0.9986 | 0.0062 |
| 05_Water | 1080p | τ 0.03, 2×2 (**Medium**) | 2.78 (-2 %) | 0.07 | 0.54 (-0.14) | 63 % | 50.1 | 0.9975 | 0.0088 |
| 05_Water | 1080p | τ 0.05, 2×2 | 2.72 (-5 %) | 0.07 | 0.48 (-0.19) | 74 % | 45.6 | 0.9947 | 0.0118 |
| 05_Water | 1080p | τ 0.05, 4×4 (**Low**) | 2.71 (-5 %) | 0.07 | 0.47 (-0.20) | 74 % | 44.6 | 0.9921 | 0.0141 |
| 05_Water | 1080p | τ 0.08, 2×2 (old High) | 2.68 (-6 %) | 0.07 | 0.44 (-0.23) | 82 % | 41.8 | 0.9898 | 0.0161 |
| 05_Water | 1080p | Motion, 4×4 | 2.94 (+3 %) | 0.07 | 0.68 (+0.01) | 0 % | 82.9 | 1.0000 | 0.0000 |
| 05_Water | 1080p | Foveated, 4×4 | 2.73 (-4 %) | 0.07 | 0.48 (-0.20) | 55 % | 33.2 | 0.9624 | 0.0247 |
| 05_Water | 1080p | pan: Off | 3.15 | — | 0.67 | — | — | — | — |
| 05_Water | 1080p | pan: τ 0.03, 2×2 | 2.99 (-5 %) | 0.07 | 0.47 (-0.21) | 80 % | 46.5 | 0.9973 | 0.0149 |
| 05_Water | 1080p | pan: Motion, 4×4 | 2.97 (-6 %) | 0.07 | 0.43 (-0.25) | 63 % | 45.7 | 0.9975 | 0.0149 |
| 05_Water | 4K | Off | 8.37 | — | 2.36 | — | — | — | — |
| 05_Water | 4K | Off again (noise) | 8.29 (-1 %) | 0.00 | 2.38 (+0.02) | 0 % | 84.5 | 1.0000 | 0.0000 |
| 05_Water | 4K | τ 0.02, 2×2 (High/Ultra if on) | 7.91 (-6 %) | 0.24 | 1.74 (-0.62) | 66 % | 53.4 | 0.9983 | 0.0068 |
| 05_Water | 4K | τ 0.03, 2×2 (**Medium**) | 7.62 (-9 %) | 0.24 | 1.56 (-0.80) | 74 % | 50.1 | 0.9971 | 0.0082 |
| 05_Water | 4K | τ 0.05, 2×2 | 7.53 (-10 %) | 0.24 | 1.42 (-0.94) | 81 % | 46.3 | 0.9943 | 0.0102 |
| 05_Water | 4K | τ 0.05, 4×4 (**Low**) | 7.91 (-6 %) | 0.23 | 1.31 (-1.05) | 81 % | 44.9 | 0.9908 | 0.0139 |
| 05_Water | 4K | τ 0.08, 2×2 (old High) | 7.54 (-10 %) | 0.24 | 1.30 (-1.06) | 86 % | 43.0 | 0.9905 | 0.0129 |
| 05_Water | 4K | Motion, 4×4 | 8.79 (+5 %) | 0.24 | 2.34 (-0.02) | 0 % | 80.5 | 1.0000 | 0.0001 |
| 05_Water | 4K | Foveated, 4×4 | 7.86 (-6 %) | 0.24 | 1.52 (-0.84) | 54 % | 35.6 | 0.9769 | 0.0173 |
| 05_Water | 4K | pan: Off | 9.23 | — | 2.29 | — | — | — | — |
| 05_Water | 4K | pan: τ 0.03, 2×2 | 8.53 (-8 %) | 0.24 | 1.32 (-0.97) | 86 % | 50.9 | 0.9988 | 0.0110 |
| 05_Water | 4K | pan: Motion, 4×4 | 8.21 (-11 %) | 0.24 | 1.19 (-1.10) | 63 % | 49.3 | 0.9987 | 0.0112 |
| 06_World | 1080p | Off | 2.85 | — | 0.60 | — | — | — | — |
| 06_World | 1080p | Off again (noise) | 2.61 (-9 %) | 0.00 | 0.59 (-0.00) | 0 % | 80.2 | 1.0000 | 0.0000 |
| 06_World | 1080p | τ 0.02, 2×2 (High/Ultra if on) | 2.60 (-9 %) | 0.04 | 0.54 (-0.06) | 34 % | 51.9 | 0.9962 | 0.0068 |
| 06_World | 1080p | τ 0.03, 2×2 (**Medium**) | 2.56 (-10 %) | 0.04 | 0.50 (-0.10) | 48 % | 46.7 | 0.9904 | 0.0114 |
| 06_World | 1080p | τ 0.05, 2×2 | 2.51 (-12 %) | 0.04 | 0.44 (-0.16) | 64 % | 41.6 | 0.9799 | 0.0188 |
| 06_World | 1080p | τ 0.05, 4×4 (**Low**) | 2.47 (-13 %) | 0.04 | 0.42 (-0.18) | 64 % | 39.8 | 0.9663 | 0.0246 |
| 06_World | 1080p | τ 0.08, 2×2 (old High) | 2.45 (-14 %) | 0.04 | 0.40 (-0.20) | 76 % | 38.9 | 0.9711 | 0.0264 |
| 06_World | 1080p | Motion, 4×4 | 2.68 (-6 %) | 0.04 | 0.60 (+0.01) | 0 % | 86.7 | 1.0000 | 0.0000 |
| 06_World | 1080p | Foveated, 4×4 | 2.48 (-13 %) | 0.04 | 0.43 (-0.17) | 55 % | 35.2 | 0.9602 | 0.0297 |
| 06_World | 1080p | pan: Off | 2.77 | — | 0.56 | — | — | — | — |
| 06_World | 1080p | pan: τ 0.03, 2×2 | 2.61 (-6 %) | 0.04 | 0.37 (-0.19) | 79 % | 44.9 | 0.9939 | 0.0285 |
| 06_World | 1080p | pan: Motion, 4×4 | 2.62 (-5 %) | 0.04 | 0.38 (-0.18) | 81 % | 41.5 | 0.9939 | 0.0321 |
| 06_World | 4K | Off | 7.44 | — | 1.94 | — | — | — | — |
| 06_World | 4K | Off again (noise) | 7.53 (+1 %) | 0.00 | 1.95 (+0.01) | 0 % | ∞ | 1.0000 | 0.0000 |
| 06_World | 4K | τ 0.02, 2×2 (High/Ultra if on) | 7.13 (-4 %) | 0.13 | 1.53 (-0.42) | 50 % | 48.2 | 0.9903 | 0.0110 |
| 06_World | 4K | τ 0.03, 2×2 (**Medium**) | 6.98 (-6 %) | 0.13 | 1.35 (-0.60) | 64 % | 44.6 | 0.9822 | 0.0160 |
| 06_World | 4K | τ 0.05, 2×2 | 6.79 (-9 %) | 0.13 | 1.21 (-0.73) | 74 % | 42.0 | 0.9733 | 0.0216 |
| 06_World | 4K | τ 0.05, 4×4 (**Low**) | 6.55 (-12 %) | 0.13 | 1.06 (-0.89) | 74 % | 39.1 | 0.9479 | 0.0315 |
| 06_World | 4K | τ 0.08, 2×2 (old High) | 6.62 (-11 %) | 0.13 | 1.13 (-0.82) | 81 % | 40.3 | 0.9672 | 0.0260 |
| 06_World | 4K | Motion, 4×4 | 7.57 (+2 %) | 0.13 | 1.94 (+0.00) | 0 % | 90.5 | 1.0000 | 0.0000 |
| 06_World | 4K | Foveated, 4×4 | 6.84 (-8 %) | 0.13 | 1.30 (-0.65) | 54 % | 37.7 | 0.9637 | 0.0253 |
| 06_World | 4K | pan: Off | 8.12 | — | 1.83 | — | — | — | — |
| 06_World | 4K | pan: τ 0.03, 2×2 | 7.30 (-10 %) | 0.13 | 1.04 (-0.79) | 86 % | 44.6 | 0.9939 | 0.0291 |
| 06_World | 4K | pan: Motion, 4×4 | 7.27 (-11 %) | 0.13 | 1.01 (-0.82) | 81 % | 39.3 | 0.9894 | 0.0344 |

VRS passes per frame: `VRS.Generate` 0.01 ms (1080p) / 0.03 ms (4K), `VRS.Analyze` 0.03 / 0.10 ms, plus
`VRS.AnalyzeOpaque` 0.03 / 0.10 ms in frames with refraction (02 glass, 05 water). Before the subgroup reduction one
analysis cost 0.42 ms at 4K.

Conclusions: Medium −2…−10 % at 1080p is mostly noise-level (the forward passes of the showcase cost 0.2–0.7 ms at
1080p on this GPU), 4K −6…−9 % except the glass station (±0: the second analysis eats the gain). Quality at Medium:
PSNR 44.6–51.8 dB, FLIP 0.007–0.016; Low (4×4) up to −12 % at 4K with PSNR 39–46 dB and visibly softer textures.
The first defaults (on at High with τ 0.08) were barely faster than τ 0.05 and lost texture detail (02 1080p PSNR
37.8 dB, FLIP 0.025). `Motion` mode: nothing on a static camera, as much as Adaptive τ 0.03 when panning.
`Foveated`: −2…−13 % but PSNR 33–46 dB (for HMDs).

## 2. CHANGELOG.md (Russian, an `[Unreleased]` entry in the file's style)

### Переменная частота шейдинга (`r.VRS*`, глава 27, `docs/dev/modules/render_vrs.md`)
- Фича `VariableRateShading` (`VK_KHR_fragment_shading_rate`, карта частоты по плиткам 16×16 px): режимы
  `Adaptive` (содержимое + движение + DoF, по Yang et al. 2019), `Motion`, `Foveated`, `DebugOverlay`;
  гистерезис, периодический перезамер плиток (`r.VRS.RefreshPeriod`), отдельная статистика непрозрачной картинки
  для кадров с преломлением; карта подключается только к цветовым пассам из `r.VRS.Passes` (prepass, тени, пост,
  UI — никогда).
- Группа Shading: VRS включён на Low (порог 0.05, до 4×4) и Medium (0.03, до 2×2), выключен на High / Ultra и в
  голом `Renderer`. Замеры на RTX 3080 (станции Showcase): Medium −6…−9 % GPU в 4K при PSNR 45–52 дБ, Low до −12 %.
- rhi (аддитивно): `DeviceCaps` для shading rate, `DeviceDesc::enableFragmentShadingRate`,
  `TextureUsage::ShadingRateAttachment`, `Access::ShadingRateRead`, `PassBuilder::shadingRate`,
  `RenderGraph::pass(i)` / `passType` / `passRenderExtent` / `passShadingRate`, `RenderingDesc::shadingRate`,
  `GraphicsPipelineDesc::variableRateShading`.
- Тесты: `vrs_cpu_tests` (9), `VrsTest.*` / `RenderTest.Vrs*` (9 + 1 opt-in), замеры `ShowcaseVrs.*` (opt-in).

### Исправлено
- VRS был включён по умолчанию (порог 0.08) и менял все эталонные картинки рендера: блочный пол в
  `GoldenTest.ManyPointLightsClustered`, размытое преломление в `TranslucencyTest.RefractiveSphereAbsorption` и ещё
  6 тестов. Кроме смены значений по умолчанию исправлены три ошибки оценки: грубые плитки занижали собственную
  детализацию (деление на шаг фрагмента → на его корень), плитка «залипала» в грубом режиме из-за текстур на мип
  крупнее (периодический перезамер), опак за преломляющим стеклом огрублялся по статистике стекла. Анализ кадра
  ускорен в 4 раза (0.42 → 0.10 мс в 4K).

## 3. `docs/guide/README.md` — chapter 27 line (append to the description)

| [27. GPU-driven рендеринг и производительность](27-gpu-driven-performance.md) | GPU-отсечение, Hi-Z, LOD и меш-леты, async compute, стриминг мипов, параллельная запись команд, переменная частота шейдинга (VRS), `RenderStats`, Tracy, замеры |

## 4. Not VRS (for the other areas)
- `GpuDrivenTest.SteadyStateExtractAndDrawListsDoNotAllocate` (gpu_driven_tests.cpp:621, "serial extract allocates"):
  fails with r.VRS off and with the extension disabled too. Cause: `extractDestruction()` in
  `engine/render/src/features/destruction/destruction_bridge.cpp` constructs a `std::unordered_set` on every
  extract (MSVC allocates its sentinel node and bucket array even for an empty set) — destruction area.
- Test isolation: `TranslucencyTest::SetUp` sets the Effects / Shading / Reflections groups to High and never
  restores them, so every later test in `ox_render_gpu_tests` runs with those presets (this is how VRS reached the
  other golden tests). Shared test infrastructure; a `RenderTest::TearDown` that restores the groups would fix it.
