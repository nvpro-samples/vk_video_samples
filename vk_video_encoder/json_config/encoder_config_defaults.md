# Encoder config defaults (when not set from CLI or JSON)

Source: `VkEncoderConfig.h` (`EncoderConfig` ctor), `VkVideoEncoderDef.h` (`ConstQpSettings`), `VkVideoGopStructure.h`, and help strings in `VkEncoderConfig.cpp`.

| JSON key | EncoderConfig / GOP member | Default when not set |
|----------|----------------------------|----------------------|
| codec | codec | VK_VIDEO_CODEC_OPERATION_NONE_KHR (must be set; default file uses "h264") |
| outputPath | outputFileHandler | empty (no output file) |
| encodeWidth | encodeWidth | 0 |
| encodeHeight | encodeHeight | 0 |
| rcMode | rateControlMode | VK_VIDEO_ENCODE_RATE_CONTROL_MODE_FLAG_BITS_MAX_ENUM_KHR; CLI "cqp" → DISABLED |
| averageBitrate | averageBitrate | 0 |
| maxBitrate | maxBitrate | 0 |
| vbvBufferSize | vbvBufferSize | 0 |
| constQpI | constQp.qpIntra | 0 |
| constQpP | constQp.qpInterP | 0 |
| constQpB | constQp.qpInterB | 0 |
| qp | constQp (I/P) | -1 = auto; ConstQpSettings default 0,0,0 |
| minQp | minQp | -1 |
| maxQp | maxQp | -1 |
| gopLength | gopStructure.SetGopFrameCount | 16 (DEFAULT_GOP_FRAME_COUNT) |
| bFrameCount | gopStructure.SetConsecutiveBFrameCount | 3 (DEFAULT_CONSECUTIVE_B_FRAME_COUNT) |
| idrPeriod | gopStructure.SetIdrPeriod | 60 (DEFAULT_GOP_IDR_PERIOD) |
| closedGop | gopStructure.SetClosedGop | false (open GOP) |
| frameRateNum | frameRateNumerator | 0 (Y4M / input can override; 30 typical) |
| frameRateDen | frameRateDenominator | 0 (1 typical) |
| qualityPreset | qualityLevel | 0 |
| colourPrimaries | colour_primaries | 0 |
| transferCharacteristics | transfer_characteristics | 0 |
| matrixCoefficients | matrix_coefficients | 0 |
| videoFullRange | video_full_range_flag | false (0) |
| verbose | verbose | false |
| validate | validate | false |

**Files (in `vk_video_encoder/json_config/`):**

- `encoder_config.default.json` – all supported keys with the defaults above (codec set to "h264" so the config is valid).
- `encoder_config.example.json` – minimal example (one typical use case).
- `encoder_config.schema.json` – JSON schema for validation.

**Precedence:** JSON (e.g. from `--encoderConfig`) is applied first; then CLI arguments override.
