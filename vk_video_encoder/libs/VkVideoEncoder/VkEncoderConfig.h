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
#include <string>
#include <cstdlib>
#include <cerrno>
#include <atomic>
#include <limits>
#include <algorithm>   // std::min, for the input sample-alignment probe
#include "mio/mio.hpp"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"   // PackedYcbcrFormatDesc()
#include "vk_video/vulkan_video_codecs_common.h"
#include "vk_video/vulkan_video_codec_h264std.h"
#include "vk_video/vulkan_video_codec_h265std.h"
#include "vk_video/vulkan_video_codec_av1std.h"
#include "vulkan/vulkan.h"
#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "VkVideoEncoder/VkVideoEncoderDef.h"
#include "VkVideoEncoder/VkVideoGopStructure.h"
#include "VkVideoEncoder/VkVideoEncoderHdrMetadata.h"
#include "VkVideoCore/VkVideoCoreProfile.h"
#include "VkVideoCore/VulkanVideoCapabilities.h"
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
#include "VkCodecUtils/VulkanFilterYuvCompute.h"
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED

#undef max

struct EncoderConfigH264;
struct EncoderConfigH265;
struct EncoderConfigAV1;
class VulkanDeviceContext;

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

// The colour model the input samples are in. An enum rather than a boolean
// because it is one of several models and a new one is a new enumerator, not
// a second flag.
enum class VkEncColorSpace : uint32_t {
    kYCbCr = 0,
    kRGB   = 1,
};

#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
// Which conversion the preprocess compute filter has to perform: the colour
// model the input samples are DECLARED to carry, against the format the device
// accepts as an encode source.
//
// The mechanism choice is the library's -- query the device first, use
// hardware if it exists, compute if it does not.
// This is its second half: once the answer is "compute", this says WHICH
// compute.
//
// EACH SIDE IS ANSWERED BY WHAT THE SURFACE MEANS, not by which format table
// happens to place its enumerant. The packed 4:4:4 Y'CbCr layouts have no
// Vulkan format of their own and ride RGBA ones (PackedYcbcrFormatDesc names
// them), so the enumerant alone cannot tell one of them from an ordinary
// R'G'B' image -- on either side:
//
//   - the INPUT side reads the declared colour model. That declaration is the
//     only thing that separates a packed Y'CbCr frame from an R'G'B' one, and
//     carrying it is what EncoderInputImageParameters::colorSpace is for.
//   - the ENCODE SOURCE carries no declaration -- it is a format the device
//     named -- so it is read from both Y'CbCr format tables, the multi-planar
//     one and the packed 4:4:4 one. Asking only the first calls a packed
//     encode source R'G'B' and routes a Y'CbCr input through the inverse
//     matrix, which writes R, G and B into the channels the encoder reads as
//     Cr, Cb and Y. That produces a full-frame wrong picture and no error at
//     all, because the inverse conversion's own output format is the same
//     enumerant the packed encode source is spelled with.
//
// YCBCRCOPY for a YCbCr->YCbCr pair is the filter's own contract, from
// VulkanFilterYuvCompute.h: YCBCRCOPY is the compute-based copy that performs
// format, plane-count and bit-depth conversion between two YCbCr formats,
// explicitly contrasted there with the XFER_* transfer modes, which "must
// have matching plane counts". A 3-plane I420 source into a 2-plane NV12
// destination is exactly that contrast, so it is YCBCRCOPY and not a
// transfer.
static inline VulkanFilterYuvCompute::FilterType VkEncDeriveFilterType(
    VkEncColorSpace inputColorSpace, VkFormat encodeSourceFormat)
{
    const bool inputIsYcbcr  = (inputColorSpace == VkEncColorSpace::kYCbCr);
    const bool outputIsYcbcr =
        (YcbcrVkFormatInfo(encodeSourceFormat) != nullptr) ||
        (PackedYcbcrFormatDesc(encodeSourceFormat) != nullptr);
    if (!inputIsYcbcr && outputIsYcbcr) {
        return VulkanFilterYuvCompute::RGBA2YCBCR;
    }
    if (inputIsYcbcr && !outputIsYcbcr) {
        return VulkanFilterYuvCompute::YCBCR2RGBA;
    }
    // YCbCr -> YCbCr, including the identity. YCBCRCOPY is a compute pass
    // either way; the plane-count and bit-depth handling it carries is what
    // the 3-plane -> 2-plane case needs, and the identity is what a file
    // input whose layout the device already accepts takes.
    return VulkanFilterYuvCompute::YCBCRCOPY;
}
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED

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
    , colorSpace(VkEncColorSpace::kYCbCr)
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

    /**
     * @brief The colour model of the input samples.
     *
     * kRGB means |vkFormat| is AUTHORITATIVE: VerifyInputs() carries it
     * through rather than re-deriving it, and lays the image out as one
     * 4-byte-per-pixel plane. kYCbCr means |vkFormat| is DERIVED, from
     * chroma subsampling, bit depth and plane count.
     *
     * The caller of this struct decides which; this header knows nothing of
     * the input-format taxonomy and only carries the conclusion.
     */
    VkEncColorSpace colorSpace;

    bool VerifyInputs()
    {
        if ((width == 0) || (height == 0)) {
            fprintf(stderr, "Invalid input width (%d) and/or height(%d) parameters!", width, height);
            return false;
        }

        // RGBA: one interleaved plane of 4 bytes per pixel, and a vkFormat the
        // caller already chose. Everything below this block describes a Y'CbCr
        // image -- planar, semi-planar or packed, with the chroma planes
        // subsampled by |chromaSubsampling| -- and none of that describes an
        // RGBA image. It must therefore be reached before the single-plane
        // arm below, which reads |chromaSubsampling| and would refuse an RGBA
        // image for carrying the default 4:2:0 value it never uses.
        if (colorSpace == VkEncColorSpace::kRGB) {
            if (vkFormat == VK_FORMAT_UNDEFINED) {
                fprintf(stderr, "Input marked RGBA but vkFormat is UNDEFINED!");
                return false;
            }
            numPlanes = 1;
            const uint32_t rgbaRowPitch = 4 * width;
            if (planeLayouts[0].rowPitch < rgbaRowPitch) {
                planeLayouts[0].rowPitch = rgbaRowPitch;
            }
            if (planeLayouts[0].size < (planeLayouts[0].rowPitch * height)) {
                planeLayouts[0].size = planeLayouts[0].rowPitch * height;
            }
            planeLayouts[1] = VkSubresourceLayout{};
            planeLayouts[2] = VkSubresourceLayout{};
            fullImageSize = (uint64_t)planeLayouts[0].size;
            // vkFormat is DELIBERATELY left alone -- see the field comment.
            return true;
        }

        // Packed 4:4:4 (AYUV / Y410) is SINGLE-plane and interleaved: one 32-bit texel
        // carries A,Y,Cb,Cr for one pixel, so the whole pixel is 4 bytes regardless of
        // whether the components are 8-bit (AYUV) or 10-bit (Y410, packed 10-in-32).
        // The per-component (bpp+7)/8 arithmetic below is wrong for it in both directions.
        const bool isPackedSinglePlane = (numPlanes == 1);
        if (isPackedSinglePlane &&
            (chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR)) {
            fprintf(stderr, "Single-plane (packed) input requires --inputChromaSubsampling 444; "
                            "packed 4:2:2 is not supported here.\n");
            return false;
        }

        uint32_t bytesPerPixel = isPackedSinglePlane ? 4u : ((bpp + 7) / 8);
        if (!isPackedSinglePlane && ((bytesPerPixel < 1) || (bytesPerPixel > 2))) {
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
                                                        VkVideoCoreProfile::PlaneLayoutFromPlaneCount(numPlanes));

        if (vkFormat == VK_FORMAT_UNDEFINED) {
            fprintf(stderr, "Invalid input parameters!");
            return false;
        }

        return true;
    }
};

class EncoderInputFileHandler
{
    // Why is this header size so big?
    static constexpr size_t Y4M_MAX_BUFF_SIZE = 8192;

public:
    EncoderInputFileHandler(bool verbose = false)
    : m_fileName{}
    , m_fileHandle()
    , m_currFrameOffset()
    , m_Y4MHeaderOffset(0)
    , m_memMapedFile()
    , m_frameSize()
    , m_maxFrameCount()
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

    const uint8_t* GetMappedPtr(size_t frameOffset)
    {
        assert(m_memMapedFile.is_mapped());
        assert(frameOffset < (uint64_t)m_memMapedFile.mapped_length());

        return m_memMapedFile.data() + frameOffset;
    }

    bool ParseY4mHeader (uint32_t *width, uint32_t *height, uint32_t *fps_n, uint32_t *fps_d)
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
                        if (!ParseY4mInt ((char *) & header[j], width)) {
                            goto beach;
                        }
                        break;
                    case 'H':
                        if (!ParseY4mInt ((char *) & header[j], height)) {
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

                        if (!ParseY4mInt ((char *) & header[j], &num)) {
                            goto beach;
                        }
                        while ((header[j] != ':') && (j < i)) {
                            j++;
                        }
                        if (!ParseY4mInt ((char *) & header[j], &den)) {
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

    uint32_t SkipY4mFrameHeader (uint64_t offset)
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

    size_t GetCurrFrameOffset()
    {
        uint64_t offset = m_currFrameOffset;
        if (m_Y4MHeaderOffset) {

            if (offset == 0) {
                offset += m_Y4MHeaderOffset;
            }
            offset += SkipY4mFrameHeader(offset);
        }

        assert(offset <= std::numeric_limits<size_t>::max());
        return static_cast<size_t>(offset);
    }

    // Advances frame pointer with one frame.
    size_t AdvanceFrameOffset(uint64_t currOffset = 0) {

        if (m_frameSize == 0) {
            // Geometry is not set
            return 0;
        }

        uint64_t offset = currOffset;

        offset += m_frameSize;

        const uint64_t mappedLength = (uint64_t)m_memMapedFile.mapped_length();
        if (!(mappedLength >= (offset + m_frameSize))) {
            // reset back to the beginning of the stream
            offset = 0;
            if (m_Y4MHeaderOffset) {
                offset += m_Y4MHeaderOffset;
            }
        }

        assert(offset <= std::numeric_limits<size_t>::max());
        m_currFrameOffset = static_cast<size_t>(offset);

        return m_currFrameOffset;
    }

    size_t ResetFrameOffset(uint64_t frameNum = 0)
    {

        if (m_frameSize == 0) {
            // Geometry is not set
            return 0;
        }

        uint64_t offset = 0;

        if (m_Y4MHeaderOffset) {
            offset += m_Y4MHeaderOffset;
        }

        while (frameNum--) {
            if (m_Y4MHeaderOffset) {
                // FIXME: (TZ) this function is terribly inefficient and needs fixing.
                offset += SkipY4mFrameHeader(offset);
            }
            offset += m_frameSize;
        }

        const uint64_t mappedLength = (uint64_t)m_memMapedFile.mapped_length();
        if (!(mappedLength >= (offset + m_frameSize))) {
            printf("File overflow at fileOffset %lld\n", (long long unsigned int)offset);
            assert(!"Input file overflow");
            return 0;
        }

        assert(offset <= std::numeric_limits<size_t>::max());
        m_currFrameOffset = static_cast<size_t>(offset);

        return m_currFrameOffset;
    }

    // Sets frame geometry and reset the stream offset
    uint32_t SetFrameGeometry(uint32_t width, uint32_t height, uint8_t bpp,
                              VkVideoChromaSubsamplingFlagBitsKHR chromaSubsampling)
    {
        uint8_t numBytes = (uint8_t)(bpp + 7) / 8;
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

        m_frameSize = (uint32_t)(width * height * numBytes * samplingFactor);

        m_currFrameOffset = 0;

        return m_frameSize;
    }

     // Public: callers outside this class need the mapped size to bound a read of the
     // file -- EncoderConfig's input sample-alignment probe reads a prefix of frame 0.
     size_t GetFileSize() const {
         return m_memMapedFile.length();
     }

    uint32_t GetMaxFrameCount() const
    {
        if(m_frameSize) {
            return (uint32_t)(GetFileSize() / m_frameSize);
        }

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

    static inline bool
    ParseY4mInt (const char * str, uint32_t * out_value_ptr)
    {
      if (!str) {
        return false;
      }
      str += 1;
      if (*str == '\0') {
        return false;
      }
      char* end = nullptr;
      unsigned long value = strtoul(str, &end, 10);
      if (end == str || value == 0) {
        return false;
      }
      *out_value_ptr = value;
      return true;
    }

private:
    char  m_fileName[256];
    FILE* m_fileHandle;
    size_t m_currFrameOffset;
    uint64_t m_Y4MHeaderOffset;
    mio::basic_mmap<mio::access_mode::read, uint8_t> m_memMapedFile;
    uint32_t m_frameSize;
    uint32_t m_maxFrameCount;
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

    uint32_t GetFrameCount(uint32_t width, uint32_t height, const VkExtent2D& qpMapTexelSize)
    {
        uint32_t widthQpMapTexels  = (width  + qpMapTexelSize.width  - 1) / qpMapTexelSize.width;
        uint32_t heightQpMapTexels = (height + qpMapTexelSize.height - 1) / qpMapTexelSize.height;

        uint32_t frameSize = widthQpMapTexels * heightQpMapTexels;

        if (frameSize)
            return (uint32_t)(GetFileSize() / frameSize);

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
    enum IntraRefreshMode {
        REFRESH_NONE,
        REFRESH_PER_PARTITION,
        REFRESH_BLOCK_ROWS,
        REFRESH_BLOCK_COLUMNS,
        REFRESH_BLOCKS
    };

    enum { ZERO_GOP_FRAME_COUNT = 0 };
    enum { ZERO_GOP_IDR_PERIOD  = 0 };
    enum { CONSECUTIVE_B_FRAME_COUNT_MAX_VALUE = UINT8_MAX};

public:
    std::string appName;
    vk::DeviceUuidUtils deviceUUID;
    int32_t  deviceId;
    int32_t  queueId;
    VkVideoCodecOperationFlagBitsKHR codec;
    bool useDpbArray;
    uint32_t numInputImages;
    EncoderInputImageParameters input;
    uint8_t  encodeBitDepthLuma;
    uint8_t  encodeBitDepthChroma;
    uint8_t  encodeNumPlanes;
    // Prefer the packed 4:4:4 encode-source format (AYUV / Y410) when the driver
    // advertises both representations for the profile.
    //
    // THE PREMISE IS UNVERIFIED ON THE CURRENT DRIVER, and saying so is the point:
    // it may have been true of an older one. On every driver this project has
    // measured, each CAPS_OK (codec, profile) pair returns EXACTLY ONE encode-source
    // format, so the "lists BOTH" case below has not been observed and the ordering
    // claim with it. It is left standing rather than rewritten into "the driver lists
    // one format", because THAT is a per-driver fact and not a contract either -- and
    // the option has to keep working on a driver that does list both.
    //
    // For a 4:4:4 profile the driver lists BOTH the 2-plane form and the packed form,
    // and the listing order is not guaranteed, so anything that takes the driver's
    // first entry -- or that matches only against the input FILE's layout -- can never
    // reach packed.
    // This makes the choice explicit and independent of the input: the encoder's compute
    // filter converts from whatever the source format and plane layout are to the
    // selected encode-source format, so packed 4:4:4 becomes reachable from any input.
    // Ignored on profiles that have no packed representation (4:2:0, 4:2:2).
    bool     preferPackedYcbcr;
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
    VkVideoEncodeUsageFlagsKHR encodeUsageHints;
    VkVideoEncodeContentFlagsKHR encodeContentHints;
    VkVideoEncodeTuningModeKHR tuningMode;
    VkVideoCoreProfile videoCoreProfile;
    VkVideoCapabilitiesKHR videoCapabilities;
    VkVideoEncodeCapabilitiesKHR videoEncodeCapabilities;
    VkVideoEncodeQuantizationMapCapabilitiesKHR quantizationMapCapabilities;
    VkVideoEncodeIntraRefreshCapabilitiesKHR intraRefreshCapabilities;
    VkVideoEncodeQualityLevelPropertiesKHR qualityLevelProperties;
    VkVideoEncodeRateControlModeFlagBitsKHR rateControlMode;
    uint32_t averageBitrate; // bits/sec (e.g., 5000000 = 5 Mbps)
    uint32_t maxBitrate;     // bits/sec
    uint32_t hrdBitrate;
    uint32_t vbvBufferSize;     // Specifies the VBV(HRD) buffer size. in bits. Set 0 to use the default VBV buffer size.
    uint32_t vbvInitialDelay;   // Specifies the VBV(HRD) initial delay in bits. Set 0 to use the default VBV initial delay.
    uint32_t frameRateNumerator;
    uint32_t frameRateDenominator;

    int32_t  minQp;
    int32_t  maxQp;
    // Caller-provided markers for the two fields above, set by the direct
    // binder and the --minQp/--maxQp args. The codec configs' derived
    // VkVideoEncode*QpKHR members are what rate control actually reads;
    // InitDeviceCapabilities uses these markers to tell a requested clamp
    // from the -1 sentinel / default-20 fallback.
    uint32_t minQpSet : 1;
    uint32_t maxQpSet : 1;
    // The same marker for constQp, set only by the direct binder, which
    // resolves all three QPs before handing the config over: there an
    // explicit 0 is a lossless request, not an unset field, and
    // InitDeviceCapabilities must not substitute preferredConstantQp for
    // it. The argv path leaves this down, keeping 0-means-unset semantics.
    uint32_t constQpSet : 1;
    ConstQpSettings constQp;

    uint32_t enableQpMap : 1;
    QpMapMode qpMapMode;

    VkVideoGopStructure gopStructure;
    int8_t dpbCount;

    // Parameters related to intra-refresh
    bool enableIntraRefresh;
    uint32_t intraRefreshCycleDuration;
    IntraRefreshMode intraRefreshMode;
    uint32_t intraRefreshCycleRestartIndex;
    uint32_t intraRefreshSkippedStartIndex;

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

    // THE INPUT SIDE. The VuiParameters block above states what the BITSTREAM
    // advertises; these state what the caller's own samples carry, as bound
    // from the chained VkVideoEncoderInputColourInfo. They are separate
    // members rather than a reinterpretation of the block above because they
    // answer a different question: the RGBA->Y'CbCr filter's matrix is a
    // function of the INPUT's primaries, and only the absence of any primaries
    // conversion in this library made reading the output field's value give
    // the same answer.
    //
    // inputColourChainPresent is what distinguishes "absent" from "present and
    // zero"; no value field can, because 0 is UNDECLARED on every axis.
    uint8_t  inputColourPrimaries;
    uint8_t  inputTransferCharacteristics;
    uint8_t  inputMatrixCoefficients;
    // VkVideoEncoderRangeDeclaration, carried as a plain integer so this
    // header takes no dependency on the ext one.
    uint8_t  inputRange;
    uint32_t inputColourChainPresent : 1;

    // HDR10 static metadata. Zero-initialized by its own member
    // initializers, so a config that never touches it emits no SEI and no
    // metadata OBU -- absence is the default and it is a real absence, not a
    // mastering display of all zeros.
    EncoderHdrStaticMetadata hdrMetadata;

    EncoderInputFileHandler inputFileHandler;
    EncoderOutputFileHandler outputFileHandler;
    EncoderQpMapFileHandler qpMapFileHandler;

#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    // WHICH conversion the preprocess compute filter performs. Owned by the
    // LIBRARY, not by any caller: VkVideoEncoder::InitEncoder overwrites it
    // from input.colorSpace and the encode-source format the device reported,
    // immediately before creating the filter (VkEncDeriveFilterType). No CLI
    // flag, no JSON key and no embeddable-API field reaches it, deliberately:
    // the mechanism choice belongs inside the library, where the device
    // capabilities are known. The initialiser below is only
    // what an uninitialised config reads as.
    VulkanFilterYuvCompute::FilterType filterType;
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED

    // Adaptive Quantization (AQ) parameters
    // Range: [-1.0, 1.0] valid, 0.0 = default/midpoint, < -1.0 (e.g., -2.0) = disabled
    // If spatialAQStrength >= -1.0, spatial AQ is enabled
    // If temporalAQStrength >= -1.0, temporal AQ is enabled
    // If both >= -1.0, combined mode (ratio determines mix)
    uint32_t enableAQ : 1;
    float spatialAQStrength;  // [-1.0, 1.0] normalized, 0.0 = default, < -1.0 = disabled
    float temporalAQStrength; // [-1.0, 1.0] normalized, 0.0 = default, < -1.0 = disabled
    std::string aqDumpDir;    // Directory for AQ dump files (default: "./aqDump")

    uint32_t validate : 1;
    uint32_t validateVerbose : 1;
    uint32_t verbose : 1;
    uint32_t verboseFrameStruct : 1;
    uint32_t verboseMsg : 1;
    uint32_t enableFramePresent : 1;
    uint32_t enableFrameDirectModePresent : 1;
    uint32_t enableHwLoadBalancing : 1;
    uint32_t noDeviceFallback : 1;
    uint32_t selectVideoWithComputeQueue : 1;
    // Skip fwrite to outputFileHandler when set; the
    // encoder captures bitstream bytes in m_capturedBitstreams
    // for the Ext API to drain via TryPopCapturedBitstream().
    uint32_t disableFileOutput : 1;
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    uint32_t enablePreprocessComputeFilter : 1;
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    uint32_t repeatInputFrames : 1;
    // enablePictureRowColReplication
    // 0: row and column replication is disabled;
    // 1: (default) replicate the last row and column to the padding area;
    // 2: replicate only one row and one column to the padding area;
    uint32_t enablePictureRowColReplication : 2;
    uint32_t enableOutOfOrderRecording : 1; // Testing only - don't use for production!
    uint32_t enablePsnrMetrics : 1;
    std::vector<uint32_t> crcInitValue;  // initialize crc values
    uint32_t disableEncodeParameterOptimizations : 1;
    uint32_t asyncAssembly : 1;
    uint32_t assemblyThreadCount;
    uint32_t outputCrcPerFrame : 1;
    std::string crcOutputFileName;

    bool IsPsnrMetricsEnabled() const { return enablePsnrMetrics != 0; }

    // ---- Colour contract for the RGBA->YCbCr preprocess filter ------------
    //
    // Both are defined in VkEncoderConfig.cpp, both are IDEMPOTENT, and both
    // are deliberately reachable from two layers: the ext config binder,
    // which has no device and can therefore refuse before the caller has
    // allocated a frame pool, and VkVideoEncoder::InitEncoder, which is the
    // only gate the argv/JSON path passes through. ONE implementation, so the
    // two layers cannot answer differently.

    // Resolve matrix_coefficients to the sampler-conversion model the filter
    // reads its matrix out of. Returns false when the DECLARED code point
    // names a matrix this filter cannot produce, having printed the reason;
    // the caller must then fail initialization. May REWRITE
    // matrix_coefficients when the declared code point NAMES NO MATRIX
    // (2 = Unspecified): it derives one from colour_primaries, applies that,
    // and writes it back, so the label the bitstream carries matches the
    // pixels that were written. It never rewrites a matrix the caller DID
    // name -- that is honoured or refused.
    //
    // Call ONLY when the filter will actually apply an RGB->YCbCr matrix. A
    // YCbCr->YCbCr copy applies no matrix at all, so refusing a code point
    // there would reject a configuration that is entirely correct.
    bool ResolveRgbToYcbcrMatrix(VkSamplerYcbcrModelConversion* outModel);

    // CC-1: the matrix an UNNAMED colour description resolves to, derived
    // from the declared primaries. Static and public so the ext-filter suite
    // can walk it as a table against the Chromium side's copy of the same
    // rule. See the CC-1 block above ResolveRgbToYcbcrMatrix.
    static uint8_t DeriveMatrixFromPrimaries(uint8_t primaries);

    // Signal the chroma siting the filter's 2x2 box average actually
    // produces, so the H.26x VUI describes the samples that were written
    // rather than the decoder's default. Same call-only-for-the-RGB-arm rule.
    void ApplyPreprocessFilterChromaSiting();

    // Compile-safe accessor for the build-gated preprocess-filter flag:
    // callers can branch on it without carrying the gate macro themselves
    // (the member only exists when the compute filter is compiled in).
    bool IsPreprocessComputeFilterEnabled() const {
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
        return enablePreprocessComputeFilter != 0;
#else
        return false;
#endif
    }
    int32_t  drmFormatModifierIndex; // -1 = disabled (OPTIMAL), >= 0 = index into non-linear modifier list
    uint64_t selectedDrmFormatModifier; // resolved modifier value (set during InitEncoder)

    EncoderConfig()
    : appName()
    , deviceId(-1)
    , queueId(0)
    , codec(VK_VIDEO_CODEC_OPERATION_NONE_KHR)
    , useDpbArray(false)
    , numInputImages(DEFAULT_NUM_INPUT_IMAGES)
    , input()
    , encodeBitDepthLuma(0)
    , encodeBitDepthChroma(0)
    , encodeNumPlanes(2)
    , preferPackedYcbcr(false)
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
    , encodeUsageHints(VK_VIDEO_ENCODE_USAGE_DEFAULT_KHR)
    , encodeContentHints(VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR)
    , tuningMode(VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR)
    , videoCoreProfile(codec, encodeChromaSubsampling, encodeBitDepthLuma, encodeBitDepthChroma)
    , videoCapabilities()
    , videoEncodeCapabilities()
    , quantizationMapCapabilities()
    , intraRefreshCapabilities()
    , rateControlMode(VK_VIDEO_ENCODE_RATE_CONTROL_MODE_FLAG_BITS_MAX_ENUM_KHR)
    , averageBitrate()
    , maxBitrate()
    , hrdBitrate(maxBitrate)
    , vbvBufferSize(0)
    , vbvInitialDelay(0)
    , frameRateNumerator()
    , frameRateDenominator()
    , minQp(-1)
    , maxQp(-1)
    , minQpSet(0)
    , maxQpSet(0)
    , constQpSet(0)
    , constQp()
    , enableQpMap(false)
    , qpMapMode(DELTA_QP_MAP)
    , gopStructure(ZERO_GOP_FRAME_COUNT,
                   ZERO_GOP_IDR_PERIOD,
                   CONSECUTIVE_B_FRAME_COUNT_MAX_VALUE,
                   DEFAULT_TEMPORAL_LAYER_COUNT)
    , dpbCount(8)
    , enableIntraRefresh(false)
    , intraRefreshCycleDuration(0)
    , intraRefreshMode(REFRESH_NONE)
    , intraRefreshCycleRestartIndex(0)
    , intraRefreshSkippedStartIndex(0)
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
    , inputColourPrimaries()
    , inputTransferCharacteristics()
    , inputMatrixCoefficients()
    , inputRange()
    , inputColourChainPresent()
    , inputFileHandler()
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    // Placeholder only -- InitEncoder derives the real value from the input
    // and encode-source formats. See the member's declaration.
    , filterType(VulkanFilterYuvCompute::YCBCRCOPY)
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    , enableAQ(VK_FALSE)
    , spatialAQStrength(-2.0f)   // < -1.0 means disabled
    , temporalAQStrength(-2.0f)  // < -1.0 means disabled
    , aqDumpDir("./aqDump")
    , validate(false)
    , validateVerbose(false)
    , verbose(false)
    , verboseFrameStruct(false)
    , verboseMsg(false)
    , enableFramePresent(false)
    , enableFrameDirectModePresent(false)
    , enableHwLoadBalancing(false)
    , noDeviceFallback(false)
    , selectVideoWithComputeQueue(false)
    , disableFileOutput(false)
#ifdef VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    , enablePreprocessComputeFilter(true)
#endif  // VK_VIDEO_SAMPLES_COMPUTE_FILTER_SUPPORTED
    , repeatInputFrames(false)
    , enablePictureRowColReplication(1)
    , enableOutOfOrderRecording(false)
    , enablePsnrMetrics(false)
    , disableEncodeParameterOptimizations(false)
    , asyncAssembly(true)
    , assemblyThreadCount(2)
    , outputCrcPerFrame(false)
    , crcOutputFileName()
    , drmFormatModifierIndex(-1)
    , selectedDrmFormatModifier(0)
    { }

    virtual ~EncoderConfig() {}

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

    // What only a device can answer, for the decisions in FinalizeConfig() that
    // are properties of the hardware rather than of the command line.
    //
    // Passed as a POINTER that may be null, because FinalizeConfig() runs on two
    // paths: argv parsing, which happens before any device exists, and the
    // embedding host, which has already probed one. Null means "no device yet"
    // -- every field below keeps its command-line answer, which is the behaviour
    // the demo has. A caller that HAS probed supplies this and the same function
    // reaches a device-correct answer, so there is one tail rather than a second
    // one that callers must remember to run.
    struct DeviceCapabilities {
        // VkPhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR::videoEncodeIntraRefresh.
        // Intra refresh is a per-device feature, so a configuration that asks for
        // it on a device that lacks it is refused here rather than at the point
        // the session is created.
        //
        // Defaulted, because this structure is filled field by field by a
        // caller that has probed a device: a member left out of that
        // assignment must read as "the device does not have it", not as
        // whatever the stack held.
        bool     intraRefreshSupported = false;
    };

    // Derived-defaults / validation tail shared by ParseArguments and the
    // direct-binding (no-argv) configuration path: input-geometry checks,
    // default-output handling, encode-size clamps and defaults, minQp
    // default, block alignment, qpMap / intra-refresh validation.
    //
    // |deviceCaps| is optional; see DeviceCapabilities for what changes when it
    // is supplied. Returns 0 on success, -1 on a validation failure.
    int FinalizeConfig(const DeviceCapabilities* deviceCaps = nullptr);

    // Codec-typed factory WITHOUT argv parsing: creates the codec subclass
    // and sets |codec|. The caller assigns fields directly, then runs
    // FinalizeConfig() + InitializeParameters() -- the same pipeline
    // CreateCodecConfig drives after ParseArguments.
    static VkResult CreateCodecConfigDirect(
        VkVideoCodecOperationFlagBitsKHR codecOperation,
        VkSharedBaseObj<EncoderConfig>& encoderConfig);

    // Load base config from JSON file (encoder_config.schema.json). JSON is processed first;
    // command-line args passed to ParseArguments override. Returns 0 on success, -1 on error.
    int LoadFromJsonFile(const char* path);

    virtual int DoParseArguments(int argc, const char *argv[]) {
        if (argc > 0) {
            std::cout << "Invalid parameters: ";
            for (int i = 0; i < argc; i++) {
                std::cout << argv[i] << " ";
            }
            std::cout << std::endl;
        }
        return 0;
    };

    // Returns the left-shift needed to move the input file's 10/12-bit samples into the
    // high bits of a 16-bit word, by inspecting the file rather than assuming a
    // convention. Returns the documented default (16 - bpp) when the data cannot
    // distinguish the two, so behaviour is unchanged for anything this cannot read.
    int8_t DetectInputMsbShift()
    {
        const int8_t defaultShift = (int8_t)(16 - input.bpp);

        if (!inputFileHandler.HasFileName() || (inputFileHandler.GetFileSize() < 2)) {
            return defaultShift;
        }

        // Luma is the first plane in every layout this applies to, so a prefix of the
        // file is luma regardless of plane count. Cap the scan: alignment is a property
        // of the encoding, so a prefix settles it.
        const size_t lumaSamples = (size_t)input.width * input.height;
        const size_t maxSamples  = (lumaSamples > 0) ? lumaSamples : (64u * 1024u);
        size_t numSamples = std::min(maxSamples, inputFileHandler.GetFileSize() / 2);
        if (numSamples == 0) {
            return defaultShift;
        }

        const uint8_t* pBytes = inputFileHandler.GetMappedPtr(0);
        if (pBytes == nullptr) {
            return defaultShift;
        }

        const uint16_t lowMask  = (uint16_t)((1u << (16 - input.bpp)) - 1u); // bottom spare bits
        const uint16_t highMask = (uint16_t)~((1u << input.bpp) - 1u);       // top spare bits

        bool allLowBitsClear  = true;   // => samples already left-aligned (P010-style)
        bool allHighBitsClear = true;   // => samples right-aligned  (yuv420p10le-style)

        for (size_t i = 0; i < numSamples; i++) {
            // Little-endian 16-bit sample; both conventions are LE in practice.
            const uint16_t sample = (uint16_t)(pBytes[2 * i] | (pBytes[2 * i + 1] << 8));
            if (sample & lowMask)  { allLowBitsClear  = false; }
            if (sample & highMask) { allHighBitsClear = false; }
            if (!allLowBitsClear && !allHighBitsClear) {
                break;  // neither convention fits; stop early
            }
        }

        // Only ONE of these two is a reliable signal, and the asymmetry matters.
        //
        // "All high bits clear" proves the data is right-aligned: no sample uses a bit
        // above the bit depth, so left-shifting is safe and necessary.
        //
        // "All low bits clear" does NOT have to hold for left-aligned data. In P010 the
        // bottom (16 - bpp) bits are explicitly UNDEFINED, and real P010 surfaces carry
        // whatever the hardware left there -- a renderer dump of a 10-bit surface spans
        // the full 16-bit range with noise in the low 6 bits. Requiring them to be clear
        // would therefore make genuine P010 match NEITHER convention and fall through to
        // the default shift, which multiplies every sample by 64 and saturates the frame
        // to white: a structurally valid bitstream of flat, near-constant content that
        // no PSNR comparison against the source can pass.
        if (allHighBitsClear && !allLowBitsClear) {
            if (verbose) {
                printf("Input: %d-bit samples are LSB-aligned; msbShift=%d\n",
                       input.bpp, defaultShift);
            }
            return defaultShift;
        }

        if (!allHighBitsClear) {
            // Some sample sets a bit above the bit depth, so the data cannot be
            // right-aligned -- whether or not the spare low bits happen to be clear.
            // Shifting could only overflow, so do not.
            if (verbose) {
                printf("Input: %d-bit samples already occupy the high bits "
                       "(P010-style, low bits undefined); msbShift=0\n", input.bpp);
            }
            return 0;
        }

        // Both clear: degenerate (all-zero) data that carries no evidence either way.
        // Keep the documented default; --msbShift overrides it.
        return defaultShift;
    }

    virtual VkResult InitializeParameters()
    {
        if (!input.VerifyInputs()) {
            return VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR;
        }

        // Deal with the input shift values, if not explicitly set.
        if (input.msbShift == -1) {

            if (PackedYcbcrFormatDesc(input.vkFormat) != nullptr) {

                // Packed 4:4:4 (AYUV / Y410 / Y416) stores each component in its own
                // bit-field of one word -- Y410 puts 10-bit U, Y and V at [9:0], [19:10]
                // and [29:20] of a 32-bit pixel. There is no 16-bit container with spare
                // low bits to left-align into, and VK_FORMAT_A2B10G10R10_UNORM_PACK32
                // already normalises each field over its full 10-bit range. Applying the
                // 16-bit MSB shift here saturates every sample to white, so a packed
                // format always takes a shift of zero.
                input.msbShift = 0;

            } else if (input.bpp > 8) {

                // Only apply the shift for higher bit-depth formats (10/12-bit)
                assert ((input.bpp == 10) || (input.bpp == 12));

                // The destination VkFormat (G10X6.../G12X4...) always carries its samples
                // in the HIGH bits of a 16-bit word, so the shift is entirely a property
                // of how the SOURCE file stores them, and both conventions are in wide use:
                //
                //   yuv420p10le & friends : values 0..1023 right-aligned  -> shift 16-bpp
                //   P010 / P210 / P410    : values left-aligned already   -> shift 0
                //
                // The two layouts are byte-identical in size and plane count, so the file
                // name and geometry cannot tell them apart. Assuming right-aligned and
                // shifting anyway turns a P010 file into a flat white frame.
                //
                // But it does not have to be guessed: the two are mutually exclusive in the
                // bits. Right-aligned data has the top (16-bpp) bits clear on every sample;
                // left-aligned data has the bottom (16-bpp) bits clear on every sample. Both
                // hold only for all-zero data. So detect it, and fall back to the documented
                // default when the answer is ambiguous.
                input.msbShift = DetectInputMsbShift();

                assert ((input.msbShift == 0) || (input.msbShift == 6) || (input.msbShift == 4));

            } else {

                input.msbShift = 0;

            }
        }

        // THE ENCODE-SIDE GEOMETRY, DERIVED IN ONE PLACE AND BEFORE ANYTHING
        // READS IT.
        //
        // encodeChromaSubsampling and encodeBitDepthLuma/Chroma describe the
        // BITSTREAM, and the input fields describe the caller's buffer. They
        // are separate fields so that the two can differ -- a chroma
        // resampler or a device-driven depth downgrade is what would make
        // them -- and today the encode side is simply derived from the input
        // side, here.
        //
        // THE DEPTH MUST NOT BE DERIVED IN InitVideoProfile(), which runs at
        // session creation, LATER than the codec arms' InitProfileLevel() --
        // and InitProfileLevel is where the level and tier are selected. So
        // EncoderConfigH265::GetCpbVclFactor(), which reads
        // encodeBitDepthLuma/Chroma for ITU-T H.265 Table A.8's depth term,
        // read zero at the level-selection call site and the real depth at
        // the InitRateControl() call site: one function, two answers, inside
        // one configuration. A 10-bit 4:4:4 stream selected its level with
        // the 8-bit factor 2000 and then sized its default CPB with 2500.
        //
        // The zero-means-unset guards are kept: an explicit encode depth, if
        // one is ever set before this runs, is a request and not a default.
        encodeChromaSubsampling = input.chromaSubsampling;

        if (encodeBitDepthLuma == 0) {
            encodeBitDepthLuma = input.bpp;
        }
        if (encodeBitDepthChroma == 0) {
            encodeBitDepthChroma = encodeBitDepthLuma;
        }

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

    // Returns the codec-specific profile identifier (must be set by InitProfileLevel first)
    virtual uint32_t GetCodecProfile() = 0;

    virtual int8_t InitDpbCount() { return 16; };

    virtual bool InitRateControl();

    virtual uint8_t GetMaxBFrameCount() { return 0;}

    virtual bool IntraRefreshWithBFramesAllowed() { return false; }
};

// Create codec configuration for H.264 encoder
VkResult CreateCodecConfigH264(int argc, char *argv[], VkSharedBaseObj<EncoderConfig>& encoderConfig);
// Create codec configuration for H.265 encoder
VkResult CreateCodecConfigH265(int argc, char *argv[], VkSharedBaseObj<EncoderConfig>& encoderConfig);
// Create codec configuration for AV1 encoder
VkResult CreateCodecConfigAV1(int artc, char *argv[], VkSharedBaseObj<EncoderConfig>& encoderConfig);

#endif /* VKVIDEOENCODER_VKENCODERCONFIG_H_ */
