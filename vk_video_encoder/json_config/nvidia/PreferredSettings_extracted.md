# PreferredSettings (2).xlsx – Extracted data

Extracted from the XLSX: **sheet1**. Columns B–F = tuning profiles (UHQ, HQ, LL, ULL, LOSSLESS). Row 1 = tuning name, rows 2–33 = parameter name (column A) and value per tuning (B–F).

## Tuning columns

| Col | Tuning |
|-----|--------|
| B   | UHQ (Ultra High Quality) |
| C   | HQ (High Quality) |
| D   | LL (Low Latency) |
| E   | ULL (Ultra Low Latency) |
| F   | LOSSLESS |

## Parameters (rows 2–33)

| Parameter (A) | UHQ (B) | HQ (C) | LL (D) | ULL (E) | LOSSLESS (F) |
|--------------|---------|--------|--------|---------|--------------|
| gopLength | 250.0 | 250.0 | INFINITE | INFINITE | 250.0 |
| RCMode | VBR | VBR | CBR | CBR | CONSTQP |
| frameFieldMode | FRAME | FRAME | FRAME | FRAME | FRAME |
| constQP.qpIntra | 25.0 | 25.0 | 25.0 | 25.0 | 0.0 |
| constQP.qpInterP | 28.0 | 28.0 | 28.0 | 28.0 | 0.0 |
| constQP.qpInterB | 31.0 | 31.0 | 31.0 | 31.0 | 0.0 |
| mvPrecision | QUARTER_PEL | QUARTER_PEL | QUARTER_PEL | QUARTER_PEL | QUARTER_PEL |
| level | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| tier | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| minCUSize | AUTOSELECT | AUTOSELECT (P6,P7: 1) | 16x16 | 16x16 | (special) |
| maxCUSize | 32x32 | 32x32 | 32x32 | 32x32 | 32x32 |
| frameIntervalP | 6.0 | P1:1, P2:1/2, P3:1/3, P4–P7:1/4 | 1.0 | 1.0 | P1:1, P2:2, P3:3, P4–P7:4 |
| useBFramesAsRef | (Turing+: each B ref) | (P1–P3: each B; P4–P7: middle B) | (each B ref) | (each B ref) | (same as HQ) |
| chromaFormatIDC | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| mochromeEncoding | 0 | 0 | 0 | 0 | 0 |
| videoFormat | UNSPECIFIED | UNSPECIFIED | UNSPECIFIED | UNSPECIFIED | UNSPECIFIED |
| colourMatrix | UNSPECIFIED | UNSPECIFIED | UNSPECIFIED | UNSPECIFIED | UNSPECIFIED |
| colourPrimaries | UNSPECIFIED | UNSPECIFIED | UNSPECIFIED | UNSPECIFIED | UNSPECIFIED |
| transferCharacteristics | UNSPECIFIED | UNSPECIFIED | UNSPECIFIED | UNSPECIFIED | UNSPECIFIED |
| enableLookahead | 1.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| loookaheadDepth | 25.0 | NA | NA | NA | NA |
| lookahedLevel | P1–P2: LEVEL_1, P3–P4: LEVEL_2, P5–P7: LEVEL_3 | NA | NA | NA | NA |
| tfLevel | TEMPORAL_FILTER_LEVEL_4 | NA | NA | NA | NA |
| lowDelayKeyFrameScale | NA | NA | 2.0 | 1.0 | NA |
| multiPass | NA | NA | DISABLED | QUARTER_RESOLUTION | NA |

## Mapping to our encoder JSON

| Excel / NVENC | Our JSON key | Notes |
|---------------|--------------|--------|
| gopLength 250 / INFINITE | gopLength | Use 250 or 0; 0 = use default (infinite GOP often = very large) |
| RCMode VBR/CBR/CONSTQP | rcMode | "vbr", "cbr", "cqp" |
| constQP.qpIntra/InterP/InterB | constQpI, constQpP, constQpB | 0 = lossless |
| frameIntervalP (B-frames) | bFrameCount | P1:1 → 1, P2:2, P3:3, P4–P7:4 for HQ; LL/ULL: 0 or 1 |
| enableLookahead | (not in schema) | Future: lookahead flag/depth |
| lowDelayKeyFrameScale | (not in schema) | CBR tuning |
| multiPass | (not in schema) | 2-pass mode |

Presets P1–P7 in the SDK control **frameIntervalP** (B-frames), **lookaheadLevel**, **useBFramesAsRef**, etc.; we map P1–P7 to **qualityPreset** 1–7.

---

## Sheet2 – Preset names (P1–P7) vs tuning

UHQ/HW/LOSSLESS (Turing vs PreTuring) and LL/ULL (Turing vs PreTuring):

| Preset | UHQ TuringOrLater | UHQ PreTuring | LL/ULL TuringOrLater | LL/ULL PreTuring |
|--------|-------------------|---------------|-----------------------|------------------|
| P1 | FASTEST | — | FASTEST | — |
| P2 | FASTEST | ULTRAFAST | FASTER | ULTRAFAST |
| P3 | FASTEST | FASTER | MEDIUM | FASTER |
| P4 | MEDIUM | — | MEDIUM1 | MEDIUM |
| P5 | SLOW | MEDIUM_SLOW | SLOWER | MEDIUM_SLOW |
| P6 | SLOWER | — | SLOWER | — |
| P7 | SLOWEST | — | SLOWEST | — |

---

## Sheet3 – Per-tuning params (HQ, LL, ULL, LOSSLESS)

| Parameter | HQ | LL | ULL | LOSSLESS |
|-----------|-----|-----|-----|----------|
| gopLength | 250 | INFINITE | 250 | 250 |
| frameIntervalP (B) | P1,P2: 1; P3: 2; P4–P7: 4 | 1 | 1 | P1–P3: 1; P4–P7: 4 |
| rateControlMode | VBR | CBR | CBR | CONSTQP |
| qpIntra / qpInterP / qpInterB | 25 / 28 / 31 | 25 / 28 / 31 | 25 / 28 / 31 | 0 / 0 / 0 |
| enableLookahead | P6,P7: 1 | — | — | P6,P7: 1 |
| lookaheadDepth | P6: 16; P7: 31−(frameIntervalP−1) | — | — | same |
| lowDelayKeyFrameScale | NA | 2.0 | 1.0 | — |
| multiPass | NA | DISABLED | QUARTER_RESOLUTION | NA |

---

## Sheet4 – Legacy NVENC preset mapping (P1–P7 → preset name)

HQ/LL/ULL/LOSSLESS × Turing vs PreTuring: P1→SUPERFAST/FASTEST, P2→FAST/SUPERFAST/…, P3→MEDIUM, P4→SLOW/SLOWER, P5→VERYSLOW_TURING/VERY_SLOW, P6→SLOWEST, P7→SLOWEST_1. (Exact columns in xlsx: B=HQ Turing, C=HQ PreTuring, D=LL/ULL Turing, E=LL/ULL PreTuring, F=LOSSLESS Turing, G=LOSSLESS PreTuring, H=LOSSLESS adjusted.)
