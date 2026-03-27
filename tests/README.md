# Vulkan Video Test Framework

A comprehensive testing framework for Vulkan Video codec implementations, supporting both encoding and decoding operations across H.264, H.265, AV1, and VP9 codecs.

## Quick Start

```bash
# Build the project first (from repository root)
cmake -B BUILD -DCMAKE_BUILD_TYPE=Release
cmake --build BUILD --parallel

# Run all tests (auto-downloads test samples)
python3 tests/vvs_test_runner.py

# Run only H.264 decoder tests
python3 tests/vvs_test_runner.py --decoder-only --codec h264

# Run only encoder tests with verbose output
python3 tests/vvs_test_runner.py --encoder-only --verbose

# List all available test samples
python3 tests/vvs_test_runner.py --list-samples

# Run a specific test by name
python3 tests/vvs_test_runner.py --test "h264_4k_main"
```

## Table of Contents

- [Quick Start](#quick-start)
- [Framework Components](#framework-components)
- [Usage Examples](#usage-examples)
- [Command Line Options](#command-line-options)
- [Configuration Reference](#configuration-reference)
  - [Decode Samples Format](#decode-samples-format)
  - [Encode Samples Format](#encode-samples-format)
  - [Test Skip List](#test-skip-list)
- [Advanced Topics](#advanced-topics)
  - [Fluster Test Suite Compatibility](#fluster-test-suite-compatibility)
  - [MD5 Verification](#md5-verification)
  - [Asset Management](#asset-management)
- [Unit Tests](#unit-tests)

---

## Framework Components

### Core Scripts

- **`vvs_test_runner.py`** - Unified test entry point that runs encoder, decoder, or both tests
- **`libs/video_test_framework_encode.py`** - Encoder test framework classes (library module)
- **`libs/video_test_framework_decode.py`** - Decoder test framework classes (library module)


### Configuration Files

- **`encode_samples.json`** - Encoder test definitions with YUV input files
- **`decode_samples.json`** - Decoder test definitions with codec samples
- **`skipped_samples.json`** - Test skipped list with conditions for skipping tests

### Unit Tests

The `unit_tests/` directory contains pytest-based tests that validate the framework itself without running actual video encoding/decoding.

#### Running Unit Tests

```bash
# Run all unit tests
pytest tests/unit_tests/ -v

# Run specific test file
pytest tests/unit_tests/test_cli.py -v

# Run tests matching a pattern
pytest tests/unit_tests/ -v -k "skip"
```

#### Test Coverage

| File | Description |
|------|-------------|
| `test_cli.py` | CLI argument parsing and option handling |
| `test_skip_list.py` | Skip list pattern matching and filtering |
| `test_filter_suite.py` | Test suite filtering by codec, pattern, and skip rules |
| `test_sample_configs.py` | Sample configuration classes (DecodeTestSample, EncodeTestSample) |
| `test_status_determination.py` | Return code to test status mapping |
| `test_utils.py` | Utility functions (file hashing, checksum verification) |



### Decode Samples Format

The `decode_samples.json` file defines decoder test cases:

```json
{
  "samples": [
    {
      "name": "h264_4k_main",
      "codec": "h264",
      "description": "Test H.264 decoding with 4K main profile sample",
      "expected_output_md5": "716a1a1999bd67ed129b07c749351859",
      "source_url": "https://storage.googleapis.com/vulkan-video-samples/avc/4k_26_ibp_main.h264",
      "source_checksum": "1b6c2fa6ea7cb8fac8064036d8729f668e913ea7cf3860009924789b8edf042f",
      "source_filepath": "video/avc/4k_26_ibp_main.h264"
    }
  ]
}
```

#### Decode Sample Fields

| Field | Required | Description |
|-------|----------|-------------|
| `name` | Yes | Unique test identifier (used with `--test` option) |
| `codec` | Yes | Codec type: `h264`, `h265`, `av1`, `vp9` |
| `description` | No | Human-readable test description |
| `expected_output_md5` | No | MD5 hash of expected decoded YUV output for verification |
| `source_url` | Yes | URL to download the test sample |
| `source_checksum` | Yes | SHA256 checksum of the source file |
| `source_filepath` | Yes | Relative path where the file is stored in `resources/` |

#### Adding New Decode Samples

To add a new decode test sample to `decode_samples.json`:

1. **Prepare the video file** - Ensure you have the codec sample file (e.g., `.h264`, `.h265`, `.ivf`, `.obu`)

2. **Calculate the SHA256 checksum** of the source file:
   ```bash
   sha256sum your_video_file.h264
   ```

3. **Generate the expected MD5 output** using the `generate_sample_md5.py` script:
   ```bash
   # For a local file
   python3 tests/generate_sample_md5.py path/to/your_video_file.h264

   # For multiple files
   python3 tests/generate_sample_md5.py video1.h264 video2.h265 video3.ivf
   ```

   The script will:
   - Use ffmpeg to decode the video and calculate the MD5 hash of the raw YUV output
   - Display the result in a format ready to copy into your JSON configuration
   - Handle multiple files in a single run

   Example output:
   ```
   Calculating MD5 for: clip-a.h264
   ✓ MD5: 9a629a99a9870197022265fcb1fc4bb2

   ======================================================================
   RESULTS
   ======================================================================
   clip-a.h264:
     "expected_output_md5": "9a629a99a9870197022265fcb1fc4bb2"
   ```

4. **Upload the file** to a publicly accessible URL (e.g., cloud storage)

5. **Add the entry** to `decode_samples.json`:
   ```json
   {
     "name": "your_test_name",
     "codec": "h264",
     "description": "Brief description of the test case",
     "expected_output_md5": "MD5_from_generate_sample_md5.py",
     "source_url": "https://storage.example.com/path/to/file.h264",
     "source_checksum": "SHA256_from_sha256sum",
     "source_filepath": "video/avc/your_file.h264"
   }
   ```

6. **Test the new entry**:
   ```bash
   # Run only your new test
   python3 tests/vvs_test_runner.py --decoder-only --test "your_test_name"

   # Verify MD5 validation works
   python3 tests/vvs_test_runner.py --decoder-only --test "your_test_name" --verbose
   ```

**Notes:**
- The `expected_output_md5` field is optional but highly recommended for validation
- The framework will automatically download the file from `source_url` to `resources/{source_filepath}`
- Use descriptive names that indicate codec, resolution, or specific features being tested
- The `source_checksum` ensures file integrity after download

### Encode Samples Format

The `encode_samples.json` file defines encoder test cases:

```json
{
  "samples": [
    {
      "name": "h264_main_profile",
      "codec": "h264",
      "profile": "main",
      "extra_args": null,
      "description": "Test H.264 Main profile encoding",
      "width": 352,
      "height": 288,
      "source_url": "https://storage.googleapis.com/vulkan-video-samples/yuv/352x288_15_i420.yuv",
      "source_checksum": "6e0e1a026717237f9546dfbd29d5e2ebbad0a993cdab38921bb43291a464ccd4",
      "source_filepath": "video/yuv/352x288_15_i420.yuv"
    }
  ]
}
```

#### Encode Sample Fields

| Field | Required | Description |
|-------|----------|-------------|
| `name` | Yes | Unique test identifier (used with `--test` option) |
| `codec` | Yes | Codec type: `h264`, `h265`, `av1` |
| `profile` | No | Encoding profile (e.g., `baseline`, `main`, `high`, `high444`, `main10`) |
| `extra_args` | No | Array of extra command-line arguments for the encoder |
| `description` | No | Human-readable test description |
| `width` | Yes | Input video width in pixels |
| `height` | Yes | Input video height in pixels |
| `source_url` | Yes | URL to download the YUV input file |
| `source_checksum` | Yes | SHA256 checksum of the source file |
| `source_filepath` | Yes | Relative path where the file is stored in `resources/` |

#### Extra Arguments Example

```json
{
  "name": "h264_cbr_ratecontrol",
  "codec": "h264",
  "profile": "high",
  "extra_args": [
    "--rateControlMode", "cbr",
    "--averageBitrate", "2000000"
  ],
  "description": "Test H.264 CBR rate control",
  "width": 352,
  "height": 288,
  "source_url": "...",
  "source_checksum": "...",
  "source_filepath": "video/yuv/352x288_15_i420.yuv"
}
```

#### Adding New Encode Samples

To add a new encode test sample to `encode_samples.json`:

1. **Prepare the input file** - The encoder supports two input formats:

   **Raw YUV (I420 format):**
   ```bash
   # Convert video to raw YUV I420 format
   ffmpeg -i input_video.mp4 -pix_fmt yuv420p -f rawvideo output_352x288.yuv

   # For 10-bit content (P010 format)
   ffmpeg -i input_video.mp4 -pix_fmt p010le -f rawvideo output_1920x1080_10bit.yuv
   ```

   **Y4M (YUV4MPEG2) format** (recommended - includes dimension metadata):
   ```bash
   ffmpeg -i input_video.mp4 -pix_fmt yuv420p output.y4m
   ```

2. **Note the video dimensions** - For raw YUV files, you must track the width and height separately (YUV files don't contain dimension metadata). Include dimensions in the filename for clarity (e.g., `352x288_15_i420.yuv`). For Y4M files, the encoder automatically parses dimensions from the file header.

3. **Calculate the SHA256 checksum** of the source file:
   ```bash
   sha256sum your_input_file.yuv
   # or
   sha256sum your_input_file.y4m
   ```

4. **Upload the file** to a publicly accessible URL (e.g., cloud storage)

5. **Add the entry** to `encode_samples.json`:
   ```json
   {
     "name": "your_test_name",
     "codec": "h264",
     "profile": "main",
     "extra_args": null,
     "description": "Brief description of the test case",
     "width": 352,
     "height": 288,
     "source_url": "https://storage.example.com/path/to/file.yuv",
     "source_checksum": "SHA256_from_sha256sum",
     "source_filepath": "video/yuv/your_file.yuv"
   }
   ```

   **Note:** For Y4M files, `width` and `height` are still required in the JSON for framework validation, but the encoder will use dimensions from the Y4M header.

6. **Test the new entry**:
   ```bash
   # Run only your new test
   python3 tests/vvs_test_runner.py --encoder-only --test "your_test_name"

   # With verbose output to see encoder command
   python3 tests/vvs_test_runner.py --encoder-only --test "your_test_name" --verbose
   ```

**Notes:**
- The framework will automatically download the file from `source_url` to `resources/{source_filepath}`
- Use descriptive names that indicate codec, profile, resolution, or specific features being tested
- The `source_checksum` ensures file integrity after download
- The `extra_args` field allows passing additional encoder arguments for specific test scenarios (e.g., rate control modes, GOP settings)
- Unlike decode tests, encode tests don't use MD5 verification since encoder output varies based on implementation
- Supported codecs for encoding are: `h264`, `h265`, `av1` (VP9 encoding is not supported)
- Common profiles: `baseline`, `main`, `high` (H.264), `main`, `main10` (H.265/AV1)
- **Validation:** By default, the encoder test framework runs a decode pass on the encoded output to verify the bitstream is valid and decodable. This can be disabled with `--no-validate-with-decoder`

### Test Skip List

The framework uses a skip list system to skip tests that are known to fail on specific platforms or GPU drivers. By default, tests listed in `skipped_samples.json` are skipped.

#### Skip List Format

```json
{
  "version": "1.0",
  "description": "Test skip list for Vulkan Video Samples test framework",
  "skipped_tests": [
    {
      "name": "av1_basic_10bit",
      "type": "decode",
      "format": "vvs",
      "drivers": ["all"],
      "platforms": ["all"],
      "reproduction": "always",
      "reason": "10-bit AV1 decoding not yet supported",
      "bug_url": "",
      "date_added": "2025-01-27"
    }
  ]
}
```

#### Skip List Fields

| Field | Required | Values | Description |
|-------|----------|--------|-------------|
| `name` | Yes | test name or pattern | Supports wildcards (e.g., `av1_*_10bit`) |
| `type` | Yes | `decode`, `encode` | Test type (decoder or encoder test) |
| `format` | Yes | `vvs`, `fluster`, `soothe` | Test suite format |
| `drivers` | Yes | array | GPU drivers to skip: `all`, `nvidia`, `nvk`, `intel`, `anv`, `amd`, `radv` |
| `platforms` | Yes | array | OS platforms: `all`, `windows`, `linux` (defined but not enforced) |
| `reproduction` | Yes | `always`, `flaky` | Whether failure is consistent |
| `reason` | No | free text | Human-readable explanation |
| `bug_url` | No | URL | Link to tracking issue |
| `date_added` | No | YYYY-MM-DD | When the skip entry was added |

#### Driver Values

- `nvidia` - NVIDIA proprietary driver
- `nvk` - NVK (open-source NVIDIA via Mesa)
- `intel` - Intel proprietary/legacy driver
- `anv` - ANV (Intel Vulkan via Mesa)
- `amd` - AMD proprietary (AMDGPU-PRO)
- `radv` - RADV (AMD Vulkan via Mesa)
- `all` - Matches any driver

#### Skip List Examples

```bash
# Run tests ignoring the skip list (run all tests)
python3 vvs_test_runner.py --ignore-skip-list

# Run only skipped tests (useful for testing fixes)
python3 vvs_test_runner.py --only-skipped

# Use a custom skip list
python3 vvs_test_runner.py --skip-list my_skip_list.json
```


### Usage Examples

#### Run All Tests
```bash
python3 vvs_test_runner.py
```

#### Run Encoder Tests Only
```bash
python3 vvs_test_runner.py --encoder-only --codec h264
```

#### Run Specific Test Pattern
```bash
python3 vvs_test_runner.py --test "*baseline*" --verbose
```

#### Export Results to JSON
```bash
python3 vvs_test_runner.py --export-json results.json
```

### Command Line Options

#### Common Options (all frameworks)

- `--encoder PATH` / `-e` - Path to vk-video-enc-test executable
- `--decoder PATH` / `-d` - Path to vk-video-dec-test executable
- `--work-dir PATH` / `-w` - Working directory for test files
- `--codec {h264,h265,av1,vp9}` / `-c` - Filter by specific codec (encoder only supports h264,h265,av1)
- `--test PATTERN` / `-t` - Filter by test name pattern (supports wildcards)
- `--list-samples` - List all available test samples and exit
- `--verbose` / `-v` - Show detailed command execution
- `--keep-files` - Keep output artifacts (decoded/encoded files) for debugging
- `--no-auto-download` - Skip automatic download of missing/corrupt sample files
- `--export-json FILE` / `-j` - Export results to JSON file
- `--deviceID ID` - Vulkan device ID to use for testing (decimal or hex with 0x prefix)
- `--timeout SECONDS` - Per-test timeout in seconds (default: 120)
- `--skip-list FILE` - Path to custom skip list JSON file (default: skipped_samples.json)
- `--ignore-skip-list` - Ignore the skip list and run all tests
- `--only-skipped` - Run only skipped tests
- `--show-skipped` - Show skipped tests in summary output
- `--decode-test-suite FILE` - Path to custom decode test suite JSON file
- `--encode-test-suite FILE` - Path to custom encode test suite JSON file

#### Framework Selection

- `--encoder-only` - Run only encoder tests
- `--decoder-only` - Run only decoder tests

#### Decode-Specific Options

- `--decode-display` - Enable display output for decode tests (removes --noPresent flag)
- `--no-verify-md5` - Disable MD5 verification of decoded output

#### Encode-Specific Options

- `--no-validate-with-decoder` - Disable validation of encoder output with decoder
- `--decoder-args ARGS` - Additional arguments to pass to decoder during validation

---

## Configuration Reference

### Test Status Types

- **SUCCESS** - Test passed successfully
- **NOT_SUPPORTED** - Feature not supported by hardware/driver (exit code 69, EX_UNAVAILABLE)
- **CRASH** - Application crashed (exit code ±6, SIGABRT)
- **ERROR** - Other failure conditions

### Test Naming Convention

Tests are automatically prefixed with their type for display:
- **Decoder tests** - Prefixed with `decode_` (e.g., `decode_h264_4k_main`)
- **Encoder tests** - Prefixed with `encode_` (e.g., `encode_h264_main_profile`)

When filtering tests with `--test`, you can use either the base name or the prefixed name:
```bash
# These are equivalent - run a specific test by base name or full name
python3 vvs_test_runner.py --test "h264_4k_main"
python3 vvs_test_runner.py --test "decode_h264_4k_main"

# Run only decoder tests (using prefix)
python3 vvs_test_runner.py --test "decode_*"

# Run only H.264 encoder tests
python3 vvs_test_runner.py --test "encode_h264_*"

# Run all AV1 tests (both encode and decode)
python3 vvs_test_runner.py --test "av1_*"
```

---

## Advanced Topics

### Asset Management

The framework automatically downloads required test assets. Assets are cached in the `resources/` directory and verified by SHA256 checksums. Use `--no-auto-download` to disable this behavior.

### Fluster Test Suite Compatibility

The framework supports [Fluster](https://github.com/fluendo/fluster) test suite format for decoder tests. When a Fluster JSON file is provided via `--decode-test-suite`, the framework will:

- Automatically detect the Fluster format (presence of `test_vectors` field)
- Download and extract zip archives containing test vectors
- Convert test vectors to internal format with proper MD5 verification
- Extract files to `resources/fluster/{codec}/{suite_name}/`
- Cache extracted files to avoid re-downloading

Example usage:
```bash
# Use Fluster JVT-AVC_V1 test suite
python3 vvs_test_runner.py --decoder-only --decode-test-suite path/to/JVT-AVC_V1.json

# Filter specific tests from Fluster suite
python3 vvs_test_runner.py --decoder-only --decode-test-suite JVT-AVC_V1.json --test "*baseline*"
```

**Note**: Fluster format is only supported for decode tests, not encode tests.

### MD5 Verification

For decoder tests, the framework can verify the correctness of decoded output by comparing MD5 hashes:

- When `expected_output_md5` is specified in `decode_samples.json`, the decoder will validate that output raw YUV data has the md5 value.
- If hashes don't match, the test is marked as **ERROR** (failed)
- Use `--no-verify-md5` to disable MD5 verification
- MD5 values can be generated using: `ffmpeg -i input.h264 -f md5 -`

### Results Format

JSON export includes:
- Test summary with counts by status type
- Individual test results with timing and status information
- Support for both encoder and decoder test results in unified format

### Running Specific Test Types

Use `--encoder-only` or `--decoder-only` to run a single test type:

```bash
# List available samples
python3 vvs_test_runner.py --list-samples

# Encoder tests only
python3 vvs_test_runner.py --encoder-only

# Decoder tests only with display
python3 vvs_test_runner.py --decoder-only --decode-display
```
