# NVIDIA-style encoder presets

This directory holds JSON encoder configs aligned with the **NVIDIA Video Codec SDK** preset and tuning model (see [Introducing NVIDIA Video Codec SDK 10 Presets](https://developer.nvidia.com/blog/introducing-video-codec-sdk-10-presets/) and [NVENC Video Encoder API Programming Guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html)).

## Tuning and presets

- **Tuning** (use case): High Quality, Low Latency, Ultra-low Latency, Lossless.
- **Presets**: P1 (fastest / lowest quality) through P7 (slowest / highest quality).  
  Our encoder maps these to `qualityPreset` 1–7; higher value = more quality, less speed.

## Files

| File | Tuning | Preset | Use case |
|------|--------|--------|----------|
| `high_quality_p1.json` … `high_quality_p7.json` | High quality | P1–P7 | Transcoding, archiving, OTT; latency-tolerant |
| `low_latency_p1.json` … `low_latency_p3.json` | Low latency | P1–P3 | Game streaming, conferencing, CBR |
| `ultra_low_latency_p1.json` | Ultra-low latency | P1 | Strictly bandwidth-constrained, minimal latency |
| `lossless.json` | Lossless | — | Lossless / constant-QP archiving |

Schema and field meanings: see parent directory `encoder_config.schema.json` and `encoder_config_defaults.md`.

## Usage

```bash
# Example: encode with high-quality P4 preset
./vulkan-video-enc-test --encoderConfig vk_video_encoder/json_config/nvidia/high_quality_p4.json -i input.yuv -o out.264 --inputWidth 1920 --inputHeight 1080 --codec h264
```

CLI arguments override values from the JSON file.
