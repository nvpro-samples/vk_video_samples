# Encoder config JSON – requirements and implementation report

## Requirements

- JSON library and parsing **inside the encoder library** at `vk_video_encoder/libs`.
- JSON **loaded and parsed in the encoder config** (`VkEncoderConfig.cpp`).
- **Precedence:** JSON applied first; command-line arguments override.
- JSON library: BSD or Apache licensed (simdjson Apache-2.0 chosen).

## Done (task status)

1. **json_config/** (encoder library)
   - `encoder_config.schema.json` – schema for encoder config.
   - `encoder_config.example.json` – example config.
   - `encoder_config.default.json` – full default config.
   - `encoder_config_defaults.md` – parameter-to-default mapping.
   - `EncoderConfigJsonLoader.h` / `.cpp` – `LoadEncoderConfigFromJson(path, void* encoderConfig)` using simdjson ondemand API.

2. **libs/CMakeLists.txt**
   - FetchContent for **simdjson** (v3.6.2).
   - `json/EncoderConfigJsonLoader.cpp` added to encoder lib sources.
   - Include dir for `libs` (for `json/EncoderConfigJsonLoader.h`).
   - `simdjson::simdjson` linked to shared and static encoder libs.

3. **VkEncoderConfig**
   - `VkEncoderConfig.h`: added `int LoadFromJsonFile(const char* path);`
   - `VkEncoderConfig.cpp`: implemented `LoadFromJsonFile` → `LoadEncoderConfigFromJson(path, this)`; included `json/EncoderConfigJsonLoader.h`.
   - At start of `ParseArguments` loop: if `args[i] == "--encoderConfig"`, require next arg as path, call `LoadFromJsonFile(path)`, skip path in argv (do not push `--encoderConfig` or path to arglist). Help text updated for `--encoderConfig`.

4. **Encoder demo (vk-video-enc-test)**
   - Demo builds encoder sources in-tree (does not link encoder lib). Added `json/EncoderConfigJsonLoader.cpp` to demo sources and `simdjson::simdjson` to demo link libs so the executable resolves `LoadEncoderConfigFromJson`.

5. **EncoderConfigJsonLoader.cpp**
   - Fixed simdjson ondemand key type: `field.key().get()` expects `simdjson::ondemand::raw_json_string&`, not `std::string_view`. Switched to `raw_json_string key`; comparisons with string literals use `operator==(raw_json_string, string_view)`.

## Findings

- **Encoder demo** does not link the encoder shared/static lib; it compiles encoder (and now json loader) sources directly. So the JSON loader source and simdjson must be added to the demo target as well.
- **simdjson v3 ondemand**: `object` iteration yields `field` where `field.key()` returns `simdjson_result<raw_json_string>`. `.get(string_view&)` is not supported; use `.get(raw_json_string&)` and then compare with string literals or `string_view` via `raw_json_string`’s `operator==` / `is_equal()`.

## Usage

- Run encoder with: `--encoderConfig /path/to/config.json`. Subsequent CLI args override JSON.
- Schema and configs: `vk_video_encoder/json_config/` (encoder_config.schema.json, encoder_config.example.json, encoder_config.default.json).
