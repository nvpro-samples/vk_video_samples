/*
 * Copyright 2026 NVIDIA Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The verdict of encode_device_probe.h on a host with no Vulkan driver, which
// any host becomes with VK_DRIVER_FILES naming a manifest that does not exist:
// the probe finds no device, a library failure is then a SKIP (77), with
// VVS_REQUIRE_GPU it is a failure (1), and Fail() is always one. The gpu tests
// take their skip-or-fail decision from these functions, so this pins it down
// without a device.

#include "encode_device_probe.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what)
{
    std::printf("  %s  %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

void SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, (value != nullptr) ? value : "");
#else
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

}  // namespace

int main()
{
    SetEnv("VK_DRIVER_FILES", "encode-device-probe-no-such-driver.json");
    SetEnv("VK_ICD_FILENAMES", nullptr);
    SetEnv("VK_ADD_DRIVER_FILES", nullptr);

    std::string detail;
    const bool found = enctest::ProbeEncodeDevice(VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &detail);
    std::printf("probe: %s\n", detail.c_str());
    Check(!found, "with no driver the probe finds no device that encodes H.264");
    Check(!enctest::ProbeEncodeDevice(0, &detail), "nor any device with an encode queue");

#ifdef VVS_REQUIRE_GPU
    // Built for a rig: a missing device is never a skip.
    Check(enctest::NoDeviceOrFail("InitializeExt", VK_ERROR_INITIALIZATION_FAILED,
                                  VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) == 1,
          "with VVS_REQUIRE_GPU built in, a library failure with no device fails");
    // A missing layer is the machine's whatever VVS_REQUIRE_GPU says: the validation gate decides.
    Check(enctest::NoDeviceOrFail("InitializeExt", VK_ERROR_LAYER_NOT_PRESENT,
                                  VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) == enctest::kExitSkip,
          "a requested layer that is missing is a skip (77), also with VVS_REQUIRE_GPU");
#else
    SetEnv("VVS_REQUIRE_GPU", nullptr);
    Check(enctest::NoDeviceOrFail("InitializeExt", VK_ERROR_INITIALIZATION_FAILED,
                                  VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) == enctest::kExitSkip,
          "a library failure with no device is a skip (77)");
    Check(enctest::NoDeviceOrFail("no NVIDIA device", VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, 0x10DEu) ==
              enctest::kExitSkip,
          "so is one with no device of the vendor");
    SetEnv("VVS_REQUIRE_GPU", "1");
    Check(enctest::NoDeviceOrFail("InitializeExt", VK_ERROR_INITIALIZATION_FAILED,
                                  VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) == 1,
          "with VVS_REQUIRE_GPU=1 it fails (1)");
    // A missing layer is the machine's whatever VVS_REQUIRE_GPU says: the validation gate decides.
    Check(enctest::NoDeviceOrFail("InitializeExt", VK_ERROR_LAYER_NOT_PRESENT,
                                  VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) == enctest::kExitSkip,
          "a requested layer that is missing is a skip (77), also with VVS_REQUIRE_GPU");
    SetEnv("VVS_REQUIRE_GPU", "0");
    Check(enctest::NoDeviceOrFail("InitializeExt", VK_ERROR_INITIALIZATION_FAILED,
                                  VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) == enctest::kExitSkip,
          "VVS_REQUIRE_GPU=0 leaves it a skip");
#endif
    Check(enctest::Fail("a failure after the device exists") == 1, "Fail() is always 1");
    Check(enctest::Fail("RegisterImageResource failed, status", -3) == 1, "with a status too");

    std::printf("\n%d failures\n", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
