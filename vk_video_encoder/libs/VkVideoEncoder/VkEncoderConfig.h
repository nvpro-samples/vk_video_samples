/*
 * Copyright 2023 NVIDIA Corporation.
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

#ifndef VKVIDEOENCODER_VKENCODERCONFIG_H_
#define VKVIDEOENCODER_VKENCODERCONFIG_H_

#include <assert.h>
#include <string.h>
#include <atomic>
#include "mio/mio.hpp"
#include "vk_video/vulkan_video_codecs_common.h"
#include "vk_video/vulkan_video_codec_h264std.h"
#include "vk_video/vulkan_video_codec_h265std.h"
#include "vk_video/vulkan_video_codec_av1std.h"
#include "vulkan/vulkan.h"
#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "VkVideoEncoder/VkVideoEncoderDef.h"
#include "VkVideoEncoder/VkVideoGopStructure.h"
#include "VkVideoCore/VkVideoCoreProfile.h"
#include "VkVideoCore/VulkanVideoCapabilities.h"
#include "VkCodecUtils/VulkanFilterYuvCompute.h"

struct EncoderConfigH264;
struct EncoderConfigH265;
struct EncoderConfigAV1;
class VulkanDeviceContext;

static const size_t Y4M_MAX_BUFF_SIZE = 8192;

static inline bool
parse_int (const char * str, uint32_t * out_value_ptr)
{
  uint32_t saved_errno;
  uint32_t value;
  bool ret;

  if (!str) {
    return false;
  }
  str += 1;
  if (*str == '\0') {
    return false;
  }

  saved_errno = errno;
  errno = 0;
  value = (uint32_t)strtol (str, NULL, 0);
  ret = (errno == 0);
  errno = saved_errno;
  if (value > 0 && value <= UINT32_MAX) {
    *out_value_ptr = value;
  } else {
    ret = false;
  }

  return ret;
}

static VkVideoComponentBitDepthFlagBitsKHR GetComponentBitDepthFlagBits(uint32_t bpp)
{
    switch (bpp) {
    case 8:
        return VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
        break;
    case 10:
        return VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
        break;
    case 12:
        return VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;
        break;
    default:
        return VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR;
        break;
    }
    return VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR;
};

struct EncoderInputImageParameters
{
    EncoderInputImageParameters()
    : width(0)
    , height(0)
    , bpp(8)
    , msbShift(-1)
    , chromaSubsampling(VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR)
    , numPlanes(3)
    , planeLayouts{}
    , fullImageSize(0)
    , vkFormat(VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM)
    {}

public:
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    int8_t   msbShift;
    VkVideoChromaSubsamplingFlagBitsKHR chromaSubsampling;
    uint32_t numPlanes;
    VkSubresourceLayout planeLayouts[3];
    uint64_t fullImageSize;
    VkFormat vkFormat;

    bool VerifyInputs()
    {
        if ((width == 0) || (height == 0)) {
            fprintf(stderr, "Invalid input width (%d) and/or height(%d) parameters!", width, height);
            return false;
        }

        uint32_t bytesPerPixel = (bpp + 7) / 8;
        if ((bytesPerPixel < 1) || (bytesPerPixel > 2)) {
            fprintf(stderr, "Invalid input bpp (%d) parameter!", bpp);
            return false;
        }

        VkDeviceSize offset = 0;
        for(uint32_t plane = 0; plane < numPlanes; plane++) {

            uint32_t planeStride = bytesPerPixel * width;
            uint32_t planeHeight = height;

            if (plane > 0) {
                switch (chromaSubsampling) {
                    case VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR:
                        planeStride = 0;
                        planeHeight = 0;
                        break;
                    case VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR:
                        planeStride = (planeStride + 1) / 2;
                        planeHeight = (planeHeight + 1) / 2;
                        break;
                    case VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR:
                        planeStride = (planeStride + 1) / 2;
                        break;
                    case VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR:
                    default:
                        break;
                }
            }

            if (planeLayouts[plane].rowPitch < (planeStride)) {
                planeLayouts[plane].rowPitch = planeStride;
            }

            if (planeLayouts[plane].size < (planeLayouts[plane].rowPitch * planeHeight)) {
                planeLayouts[plane].size = (planeLayouts[plane].rowPitch * planeHeight);
            }

            if (planeLayouts[plane].offset < offset) {
                planeLayouts[plane].offset = offset;
            }

            offset += planeLayouts[plane].size;
        }

        fullImageSize = (uint64_t)offset;

        vkFormat = VkVideoCoreProfile::CodecGetVkFormat(chromaSubsampling,
                                                        GetComponentBitDepthFlagBits(bpp),
                                                        (numPlanes == 2));

        if (vkFormat == VK_FORMAT_UNDEFINED) {
            fprintf(stderr, "Invalid input parameters!");
            return false;
        }

        return true;
    }
};

class EncoderInputFileHandler
{
public:
    EncoderInputFileHandler(bool verbose = false)
    : m_fileName{}
    , m_fileHandle()
    , m_Y4MHeaderOffset(0)
    , m_memMapedFile()
    , m_verbose(verbose)
    {

    }

    ~EncoderInputFileHandler()
    {
        Destroy();
    }

    void Destroy()
    {
        m_memMapedFile.unmap();

        if (m_fileHandle != nullptr) {
            if (fclose(m_fileHandle)) {
                fprintf(stderr, "Failed to close input file %s", m_fileName);
            }

            m_fileHandle = nullptr;
        }
    }

    bool HasFileName()
    {
        return m_fileName[0] != 0;
    }

    size_t SetFileName(const char* inputFileName)
    {
        Destroy();
        strcpy(m_fileName, inputFileName);
        return OpenFile();
    }

    bool HandleIsValid() const {
        return (m_fileHandle != nullptr);
    }

    bool FileIsValid() const {
        if (HandleIsValid()) {
            return true;
        }
        return (m_fileHandle != nullptr);
    }

    FILE* GetFileHandle() const {
        assert(m_fileHandle != nullptr);
        return m_fileHandle;
    }

    const uint8_t* GetMappedPtr(uint64_t frameSize, uint64_t frame_num)
    {
        assert(m_memMapedFile.is_mapped());
        uint64_t offset = 0;
        uint64_t frame_offset = 0;
        uint64_t frame_i = 0;

        if (m_Y4MHeaderOffset) {
            offset += m_Y4MHeaderOffset;
        }

        while (frame_i < frame_num) {
            if (m_Y4MHeaderOffset) {
                frame_offset = skipY4MFrameHeader(offset);
                offset += frame_offset;
            }
            frame_i++;
            offset += frameSize;
        }

        const uint64_t mappedLength = (uint64_t)m_memMapedFile.mapped_length();
        if (mappedLength < offset) {
            printf("File overflow at fileOffset %lld\n", (long long unsigned int)offset);
            assert(!"Input file overflow");
            return nullptr;
        }

        return m_memMapedFile.data() + offset;
    }

    bool parseY4M (uint32_t *width, uint32_t *height, uint32_t *fps_n, uint32_t *fps_d)
    {
        size_t i, j, s;
        int b;
        char header[Y4M_MAX_BUFF_SIZE];
        bool ret = false;

        memset (header, 0, Y4M_MAX_BUFF_SIZE);
        s = fread (header, 1, 9, m_fileHandle);
        if (s < 9 || memcmp (header, "YUV4MPEG2", 9) != 0) {
            goto beach;
        }

        for (i = 9; i < Y4M_MAX_BUFF_SIZE - 1; i++) {
            b = fgetc (m_fileHandle);
            if (b == EOF) {
                goto beach;
            }
            if (b == 0xa) {
                break;
            }
            header[i] = (char)b;
        }

        if (i == Y4M_MAX_BUFF_SIZE - 1) {
            goto beach;
        }

        j = 9;
        while (j < i) {
            if ((header[j] != 0x20) && (header[j - 1] == 0x20)) {
                switch (header[j]) {
                    case 'W':
                        if (!parse_int ((char *) & header[j], width)) {
                            goto beach;
                        }
                        break;
                    case 'H':
                        if (!parse_int ((char *) & header[j], height)) {
                            goto beach;
                        }
                        break;
                    case 'C':
                        break;
                    case 'I':
                        break;
                    case 'F':              /* frame rate ratio */
                    {
                        uint32_t num, den;

                        if (!parse_int ((char *) & header[j], &num)) {
                            goto beach;
                        }
                        while ((header[j] != ':') && (j < i)) {
                            j++;
                        }
                        if (!parse_int ((char *) & header[j], &den)) {
                            goto beach;
                        }

                        if (num <= 0 || den <= 0) {
                            *fps_n = 30;   /* default to 30 fps */
                            *fps_d = 1;
                        } else {
                            *fps_n = num;
                            *fps_d = den;
                        }
                        break;
                    }
                    case 'A':              /* sample aspect ration */
                        break;
                    case 'X':              /* metadata */
                        break;
                    default:
                        break;
                }
            }
            j++;
        }
        ret = true;
        m_Y4MHeaderOffset = j + 1;
beach:
        return ret;
    }

    uint32_t skipY4MFrameHeader (uint64_t offset)
    {
        uint32_t i;
        int b;
        uint8_t header[Y4M_MAX_BUFF_SIZE];
        size_t s;

        memset (header, 0, Y4M_MAX_BUFF_SIZE);
#if !defined(VK_USE_PLATFORM_WIN32_KHR)
        fseeko(m_fileHandle, static_cast<off_t>(offset), SEEK_SET);
#else
        fseek(m_fileHandle, static_cast<off_t>(offset), SEEK_SET);
#endif
        s = fread (header, 1, 5, m_fileHandle);
        if (s < 5) {
            return 0;
        }

        if (memcmp (header, "FRAME", 5) != 0) {
            return 0;
        }

        for (i = 5; i < Y4M_MAX_BUFF_SIZE - 1; i++) {
            b = fgetc (m_fileHandle);
            if (b == EOF) {
                return 0;
            }
            if (b == 0xa) {
                break;
            }
            header[i] = (char)b;
        }

        return i + 1;
    }

    uint32_t GetFrameCount(uint32_t width, uint32_t height, uint8_t bpp, VkVideoChromaSubsamplingFlagBitsKHR chromaSubsampling) {
        uint8_t nBytes = (uint8_t)(bpp + 7) / 8;
        double samplingFactor = 1.5; // Default for 420
        switch (chromaSubsampling)
        {
        case VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR:
            samplingFactor = 1.0; // Only Y component
            break;
        case VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR:
            samplingFactor = 1.5; // Y + 1/4 U + 1/4 V = 1.5
            break;
        case VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR:
            samplingFactor = 2.0; // Y + 1/2 U + 1/2 V = 2.0
            break;
        case VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR:
            samplingFactor = 3.0; // Full Y + full U + full V = 3.0
            break;
        default:
            assert(!"Unknown chroma subsampling");
            break;
        }
        uint32_t frameSize = (uint32_t)(width * height * nBytes * samplingFactor);

        if(frameSize)
            return (uint32_t)(GetFileSize()/frameSize);

        return 0;
    }

private:
    size_t OpenFile()
    {
        m_fileHandle = fopen(m_fileName, "rb");
        if (m_fileHandle == nullptr) {
            fprintf(stderr, "Failed to open input file %s", m_fileName);
            return 0;
        }

        std::error_code error;
        m_memMapedFile.map(m_fileName, 0, mio::map_entire_file, error);
        if (error) {
            fprintf(stderr, "Failed to map the input file %s", m_fileName);
            const auto& errmsg = error.message();
            std::printf("error mapping file: %s, exiting...\n", errmsg.c_str());
            return error.value();
        }

        if (m_verbose) {
            printf("Input file size is: %zd\n", m_memMapedFile.length());
        }

        return m_memMapedFile.length();
    }

    size_t GetFileSize() const {
        return m_memMapedFile.length();
    }

private:
    char  m_fileName[256];
    FILE* m_fileHandle;
    uint64_t m_Y4MHeaderOffset;
    mio::basic_mmap<mio::access_mode::read, uint8_t> m_memMapedFile;
    uint32_t m_verbose : 1;
};

class EncoderOutputFileHandler
{
public:
    EncoderOutputFileHandler()
    : m_fileName{}
    , m_fileHandle()
    {

    }

    ~EncoderOutputFileHandler()
    {
        Destroy();
    }

    void Destroy()
    {
        if (m_fileHandle != nullptr) {
            if(fclose(m_fileHandle)) {
                fprintf(stderr, "Failed to close output file %s", m_fileName);
            }

            m_fileHandle = nullptr;
        }
    }

    bool HasFileName()
    {
        return m_fileName[0] != 0;
    }

    size_t SetFileName(const char* inputFileName)
    {
        Destroy();
        strcpy(m_fileName, inputFileName);
        return OpenFile();
    }

    const char* GetFileName()
    {
        return m_fileName;
    }

    bool HandleIsValid() const {
        return (m_fileHandle != nullptr);
    }

    bool FileIsValid() const {
        if (HandleIsValid()) {
            return true;
        }
        return (m_fileHandle != nullptr);
    }

    FILE* GetFileHandle() const {
        assert(m_fileHandle != nullptr);
        return m_fileHandle;
    }


private:
    size_t OpenFile()
    {
        m_fileHandle = fopen(m_fileName, "wb");
        if (m_fileHandle == nullptr) {
            fprintf(stderr, "Failed to open output file %s", m_fileName);
            return 0;
        }

        return 1;
    }

private:
    char  m_fileName[256];
    FILE* m_fileHandle;
};


class EncoderQpMapFileHandler
{
public:
    EncoderQpMapFileHandler(bool verbose = false)
    : m_fileName{}
    , m_fileHandle()
    , m_memMapedFile()
    , m_verbose(verbose)
    {

    }

    ~EncoderQpMapFileHandler()
    {
        Destroy();
    }

    void Destroy()
    {
        m_memMapedFile.unmap();

        if (m_fileHandle != nullptr) {
            if(fclose(m_fileHandle)) {
                fprintf(stderr, "Failed to close input file %s", m_fileName);
            }

            m_fileHandle = nullptr;
        }
    }

    bool HasFileName()
    {
        return m_fileName[0] != 0;
    }

    size_t SetFileName(const char* inputFileName)
    {
        Destroy();
        strcpy(m_fileName, inputFileName);
        return OpenFile();
    }

    bool HandleIsValid() const {
        return (m_fileHandle != nullptr);
    }

    bool FileIsValid() const {
        if (HandleIsValid()) {
            return true;
        }
        return (m_fileHandle != nullptr);
    }

    FILE* GetFileHandle() const {
        assert(m_fileHandle != nullptr);
        return m_fileHandle;
    }

    const uint8_t* GetMappedPtr(uint64_t fileOffset)
    {
        assert(m_memMapedFile.is_mapped());

        const uint64_t mappedLength = (uint64_t)m_memMapedFile.mapped_length();
        if (mappedLength < fileOffset) {
            printf("File overflow at fileOffset %llu\n",  (unsigned long long int)fileOffset);
            assert(!"Input file overflow");
            return nullptr;
        }
        return m_memMapedFile.data() + fileOffset;
    }

private:
    size_t OpenFile()
    {
        m_fileHandle = fopen(m_fileName, "rb");
        if (m_fileHandle == nullptr) {
            fprintf(stderr, "Failed to open input file %s", m_fileName);
            return 0;
        }

        std::error_code error;
        m_memMapedFile.map(m_fileName, 0, mio::map_entire_file, error);
        if (error) {
            fprintf(stderr, "Failed to map the input file %s", m_fileName);
            const auto& errmsg = error.message();
            std::printf("error mapping file: %s, exiting...\n", errmsg.c_str());
            return error.value();
        }

        if (m_verbose) {
            printf("Input file size is: %zd\n", m_memMapedFile.length());
        }

        return m_memMapedFile.length();
    }

    size_t GetFileSize() const {
        return m_memMapedFile.length();
    }

private:
    char  m_fileName[256];
    FILE* m_fileHandle;
    mio::basic_mmap<mio::access_mode::read, uint8_t> m_memMapedFile;
    uint32_t m_verbose : 1;
};

struct EncoderConfig : public VkVideoRefCountBase {

    enum { DEFAULT_NUM_INPUT_IMAGES = 16 };
    enum { DEFAULT_GOP_FRAME_COUNT = 16 };
    enum { DEFAULT_GOP_IDR_PERIOD  = 60 };
    enum { DEFAULT_CONSECUTIVE_B_FRAME_COUNT = 3 };
    enum { DEFAULT_TEMPORAL_LAYER_COUNT = 1 };
    enum { DEFAULT_NUM_SLICES_PER_PICTURE = 4 };
    enum { DEFAULT_MAX_NUM_REF_FRAMES = 16 };
    enum QpMapMode { DELTA_QP_MAP, EMPHASIS_MAP };

    enum { ZERO_GOP_FRAME_COUNT = 0 };
    enum { ZERO_GOP_IDR_PERIOD  = 0 };
    enum { CONSECUTIVE_B_FRAME_COUNT_MAX_VALUE = UINT8_MAX};

private:
    std::atomic<int32_t> refCount;

public:
    std::string appName;
    vk::DeviceUuidUtils deviceUUID;
    int32_t  deviceId;
    int32_t  queueId;
    VkVideoCodecOperationFlagBitsKHR codec;
    bool useDpbArray;
    uint32_t videoProfileIdc;
    uint32_t numInputImages;
    EncoderInputImageParameters input;
    uint8_t  encodeBitDepthLuma;
    uint8_t  encodeBitDepthChroma;
    uint8_t  encodeNumPlanes;
    uint8_t  numBitstreamBuffersToPreallocate;
    VkVideoChromaSubsamplingFlagBitsKHR  encodeChromaSubsampling;
    uint32_t encodeOffsetX;
    uint32_t encodeOffsetY;
    uint32_t encodeWidth;
    uint32_t encodeHeight;
    uint32_t encodeAlignedWidth;
    uint32_t encodeAlignedHeight;
    uint32_t encodeMaxWidth;
    uint32_t encodeMaxHeight;
    uint32_t startFrame;
    uint32_t numFrames;
    uint32_t codecBlockAlignment;
    uint32_t qualityLevel;
    VkVideoEncodeTuningModeKHR tuningMode;
    VkVideoCoreProfile videoCoreProfile;
    VkVideoCapabilitiesKHR videoCapabilities;
    VkVideoEncodeCapabilitiesKHR videoEncodeCapabilities;
    VkVideoEncodeQuantizationMapCapabilitiesKHR quantizationMapCapabilities;
    VkVideoEncodeQualityLevelPropertiesKHR qualityLevelProperties;
    VkVideoEncodeRateControlModeFlagBitsKHR rateControlMode;
    uint32_t averageBitrate; // kbits/sec
    uint32_t maxBitrate;     // kbits/sec
    uint32_t hrdBitrate;
    uint32_t frameRateNumerator;
    uint32_t frameRateDenominator;

    int32_t  minQp;
    int32_t  maxQp;
    ConstQpSettings constQp;

    uint32_t enableQpMap : 1;
    QpMapMode qpMapMode;

    VkVideoGopStructure gopStructure;
    int8_t dpbCount;

    // Vulkan Input color space and transfer characteristics parameters
    VkSamplerYcbcrModelConversion              ycbcrModel;
    VkSamplerYcbcrRange                        ycbcrRange;
    VkComponentMapping                         components;
    VkChromaLocation                           xChromaOffset;
    VkChromaLocation                           yChromaOffset;

    // VuiParameters
    uint32_t darWidth;  // Specifies the display aspect ratio width.
    uint32_t darHeight; // Specifies the display aspect ratio height.
    uint32_t aspect_ratio_info_present_flag : 1;
    uint32_t overscan_info_present_flag : 1;
    uint32_t overscan_appropriate_flag : 1;
    uint32_t video_signal_type_present_flag : 1;
    uint32_t video_full_range_flag : 1;
    uint32_t color_description_present_flag : 1;
    uint32_t chroma_loc_info_present_flag : 1;
    uint32_t bitstream_restriction_flag : 1;
    uint8_t  video_format;
    uint8_t  colour_primaries;
    uint8_t  transfer_characteristics;
    uint8_t  matrix_coefficients;
    uint8_t  max_num_reorder_frames;
    uint8_t  max_dec_frame_buffering;
    uint8_t  chroma_sample_loc_type;

    EncoderInputFileHandler inputFileHandler;
    EncoderOutputFileHandler outputFileHandler;
    EncoderQpMapFileHandler qpMapFileHandler;

    VulkanFilterYuvCompute::FilterType filterType;

    uint32_t validate : 1;
    uint32_t validateVerbose : 1;
    uint32_t verbose : 1;
    uint32_t verboseFrameStruct : 1;
    uint32_t verboseMsg : 1;
    uint32_t enableFramePresent : 1;
    uint32_t enableFrameDirectModePresent : 1;
    uint32_t enableHwLoadBalancing : 1;
    uint32_t selectVideoWithComputeQueue : 1;
    uint32_t enablePreprocessComputeFilter : 1;
    // enablePictureRowColReplication
    // 0: row and column replication is disabled;
    // 1: (default) replicate the last row and column to the padding area;
    // 2: replicate only one row and one column to the padding area;
    uint32_t enablePictureRowColReplication : 2;
    uint32_t enableOutOfOrderRecording : 1; // Testing only - don't use for production!

    EncoderConfig()
    : refCount(0)
    , appName()
    , deviceId(-1)
    , queueId(0)
    , codec(VK_VIDEO_CODEC_OPERATION_NONE_KHR)
    , useDpbArray(false)
    , videoProfileIdc((uint32_t)-1)
    , numInputImages(DEFAULT_NUM_INPUT_IMAGES)
    , input()
    , encodeBitDepthLuma(0)
    , encodeBitDepthChroma(0)
    , encodeNumPlanes(2)
    , numBitstreamBuffersToPreallocate(8)
    , encodeChromaSubsampling(VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR)
    , encodeOffsetX(0)
    , encodeOffsetY(0)
    , encodeWidth(0)
    , encodeHeight(0)
    , encodeAlignedWidth(0)
    , encodeAlignedHeight(0)
    , encodeMaxWidth(0)
    , encodeMaxHeight(0)
    , startFrame(0)
    , numFrames(0)
    , codecBlockAlignment(16)
    , qualityLevel(0)
    , tuningMode(VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR)
    , videoCoreProfile(codec, encodeChromaSubsampling, encodeBitDepthLuma, encodeBitDepthChroma)
    , videoCapabilities()
    , videoEncodeCapabilities()
    , quantizationMapCapabilities()
    , rateControlMode(VK_VIDEO_ENCODE_RATE_CONTROL_MODE_FLAG_BITS_MAX_ENUM_KHR)
    , averageBitrate()
    , maxBitrate()
    , hrdBitrate(maxBitrate)
    , frameRateNumerator()
    , frameRateDenominator()
    , minQp(-1)
    , maxQp(-1)
    , constQp()
    , enableQpMap(false)
    , qpMapMode(DELTA_QP_MAP)
    , gopStructure(ZERO_GOP_FRAME_COUNT,
                   ZERO_GOP_IDR_PERIOD,
                   CONSECUTIVE_B_FRAME_COUNT_MAX_VALUE,
                   DEFAULT_TEMPORAL_LAYER_COUNT)
    , dpbCount(8)
    , ycbcrModel(VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709)
    , ycbcrRange(VK_SAMPLER_YCBCR_RANGE_ITU_FULL)
    , components{VK_COMPONENT_SWIZZLE_IDENTITY,
                 VK_COMPONENT_SWIZZLE_IDENTITY,
                 VK_COMPONENT_SWIZZLE_IDENTITY,
                 VK_COMPONENT_SWIZZLE_IDENTITY}
    , xChromaOffset(VK_CHROMA_LOCATION_MIDPOINT)
    , yChromaOffset(VK_CHROMA_LOCATION_MIDPOINT)
    , darWidth()
    , darHeight()
    , aspect_ratio_info_present_flag()
    , overscan_info_present_flag()
    , overscan_appropriate_flag()
    , video_signal_type_present_flag()
    , video_full_range_flag()
    , color_description_present_flag()
    , chroma_loc_info_present_flag()
    , bitstream_restriction_flag()
    , video_format()
    , colour_primaries()
    , transfer_characteristics()
    , matrix_coefficients()
    , max_num_reorder_frames()
    , max_dec_frame_buffering()
    , chroma_sample_loc_type()
    , inputFileHandler()
    , filterType(VulkanFilterYuvCompute::YCBCRCOPY)
    , validate(false)
    , validateVerbose(false)
    , verbose(false)
    , verboseFrameStruct(false)
    , verboseMsg(false)
    , enableFramePresent(false)
    , enableFrameDirectModePresent(false)
    , enableHwLoadBalancing(false)
    , selectVideoWithComputeQueue(false)
    , enablePreprocessComputeFilter(true)
    , enablePictureRowColReplication(1)
    , enableOutOfOrderRecording(false)
    { }

    virtual ~EncoderConfig() {}

    virtual int32_t AddRef()
    {
        return ++refCount;
    }

    virtual int32_t Release()
    {
        uint32_t ret = --refCount;
        // Destroy the device if ref-count reaches zero
        if (ret == 0) {
            delete this;
        }
        return ret;
    }

    virtual EncoderConfigH264* GetEncoderConfigh264() {
        return nullptr;
    }

    virtual EncoderConfigH265* GetEncoderConfigh265() {
        return nullptr;
    }

    virtual EncoderConfigAV1* GetEncoderConfigAV1() {
        return nullptr;
    }

    // Factory Function
    static VkResult CreateCodecConfig(int argc, const char *argv[], VkSharedBaseObj<EncoderConfig>& encoderConfig);

    void InitVideoProfile();

    int ParseArguments(int argc, const char *argv[]);

    virtual int DoParseArguments(int argc, const char *argv[]) {
        if (argc > 0) {
            std::cout << "Invalid paramters: ";
            for (int i = 0; i < argc; i++) {
                std::cout << argv[i] << " ";
            }
            std::cout << std::endl;
            return -1;
        }
        return 0;
    };

    virtual VkResult InitializeParameters()
    {
        if (!input.VerifyInputs()) {
            return VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR;
        }

        // Copy chroma subsampling from input to encoder config
        encodeChromaSubsampling = input.chromaSubsampling;

        if ((encodeWidth == 0) || (encodeWidth > input.width)) {
            encodeWidth = input.width;
        }

        if ((encodeHeight == 0) || (encodeHeight > input.height)) {
            encodeHeight = input.height;
        }

        return VK_SUCCESS;
    }

    // These functions should be overwritten from the codec-specific classes
    virtual VkResult InitDeviceCapabilities(const VulkanDeviceContext* vkDevCtx) { return VK_ERROR_INITIALIZATION_FAILED; };

    virtual uint32_t GetDefaultVideoProfileIdc() { return 0; };

    virtual int8_t InitDpbCount() { return 16; };

    virtual bool InitRateControl();

    virtual uint8_t GetMaxBFrameCount() { return 0;}
};

// Create codec configuration for H.264 encoder
VkResult CreateCodecConfigH264(int argc, char *argv[], VkSharedBaseObj<EncoderConfig>& encoderConfig);
// Create codec configuration for H.265 encoder
VkResult CreateCodecConfigH265(int argc, char *argv[], VkSharedBaseObj<EncoderConfig>& encoderConfig);
// Create codec configuration for AV1 encoder
VkResult CreateCodecConfigAV1(int artc, char *argv[], VkSharedBaseObj<EncoderConfig>& encoderConfig);

#endif /* VKVIDEOENCODER_VKENCODERCONFIG_H_ */
