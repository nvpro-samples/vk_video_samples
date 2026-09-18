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

#ifndef VKVIDEOENCODER_VKVIDEOENCODERHDRMETADATA_H_
#define VKVIDEOENCODER_VKVIDEOENCODERHDRMETADATA_H_

#include <cstddef>
#include <cstdint>

//=============================================================================
// HDR10 STATIC METADATA -- the two payloads, and the two bitstream shapes.
//
// This is the only place in the encoder that writes bits by hand, and that
// is deliberate: neither payload has a Vulkan Video std structure. There is
// no StdVideoEncodeH265SeiMastering..., no AV1 metadata OBU entry point, and
// GetEncodedVideoSessionParametersKHR emits parameter sets only. Every
// mastering-display / content-light symbol elsewhere in this tree is
// decoder-side or lives in scripts/vk.xml. So the bytes are built here and
// appended to the non-VCL header the driver produced.
//
// UNITS. The struct below carries SMPTE ST 2086 units, which is what the
// H.265 SEI codes directly and what a producer already has (Chromium's
// gfx::HdrMetadataSmpteSt2086, ffmpeg's AVMasteringDisplayMetadata):
//
//   chromaticity  increments of 0.00002   (value = coordinate * 50000)
//   luminance     increments of 0.0001 cd/m^2 (value = nits * 10000)
//   MaxCLL/MaxFALL  cd/m^2, integral
//
// AV1 DOES NOT USE THOSE UNITS, and that mismatch is the sharpest trap in
// this file. metadata_hdr_mdcv() codes
//
//   primary/white-point chromaticity  0.16 fixed point (value = coord * 65536)
//   luminance_max                     24.8 fixed point (value = nits * 256)
//   luminance_min                     18.14 fixed point (value = nits * 16384)
//
// so the AV1 builder converts and the H.265 builder does not.
//
// AV1 ALSO PERMUTES THE PRIMARIES: metadata_hdr_mdcv() indexes them RED,
// GREEN, BLUE where ST 2086 (and therefore the H.265 SEI) indexes them GREEN,
// BLUE, RED. Copying the array straight across produces a stream that parses
// cleanly and calls green red.
//
// Golden vectors pinning both payloads byte for byte live in
// test/encoder-ext-filter.
//=============================================================================

struct EncoderHdrStaticMetadata {
    // Each payload is independently present. A caller that knows MaxCLL and
    // nothing about the mastering display gets exactly one SEI message and
    // exactly one metadata OBU -- rather than a mastering display of all
    // zeros, which is a positive claim that the display is black.
    uint32_t masteringDisplayPresent  = 0;
    uint32_t contentLightLevelPresent = 0;

    // ST 2086 ORDER: GREEN, BLUE, RED. That is the H.265 SEI's own order
    // (D.3.28: "c equal to 0, 1 and 2 corresponding to the green, blue and
    // red colour primary"), so the H.265 builder writes this array straight
    // through. The AV1 builder does NOT -- metadata_hdr_mdcv() indexes red,
    // green, blue, and it permutes.
    uint16_t displayPrimaryX[3] = {0, 0, 0};
    uint16_t displayPrimaryY[3] = {0, 0, 0};
    uint16_t whitePointX = 0;
    uint16_t whitePointY = 0;
    uint32_t maxDisplayMasteringLuminance = 0;
    uint32_t minDisplayMasteringLuminance = 0;

    uint16_t maxContentLightLevel      = 0;
    uint16_t maxFrameAverageLightLevel = 0;

    bool Any() const {
        return (masteringDisplayPresent != 0) ||
               (contentLightLevelPresent != 0);
    }
};

// Build the H.265 PREFIX SEI NAL unit carrying mastering_display_colour_volume
// (payloadType 137) and/or content_light_level_info (payloadType 144), with a
// 4-byte start code and emulation-prevention bytes inserted, ready to be
// appended to the VPS/SPS/PPS the driver wrote.
//
// Returns the number of bytes written, or 0 if there was nothing to write.
// Returns 0 AND SETS *outTruncated if the payload did not fit in |capacity|:
// the caller must treat that as an error rather than encode without it. A
// dropped colour volume is invisible in every other observable the encoder
// has, which is exactly how this class of defect survives.
size_t VkEncBuildH265HdrSeiNal(const EncoderHdrStaticMetadata& metadata,
                               uint8_t* out, size_t capacity,
                               bool* outTruncated);

// Build the AV1 metadata OBU(s): METADATA_TYPE_HDR_MDCV (2) and/or
// METADATA_TYPE_HDR_CLL (1), each a complete OBU with obu_has_size_field set,
// ready to be appended to the sequence header OBU. Same return contract.
size_t VkEncBuildAv1HdrMetadataObus(const EncoderHdrStaticMetadata& metadata,
                                    uint8_t* out, size_t capacity,
                                    bool* outTruncated);

#endif  // VKVIDEOENCODER_VKVIDEOENCODERHDRMETADATA_H_
