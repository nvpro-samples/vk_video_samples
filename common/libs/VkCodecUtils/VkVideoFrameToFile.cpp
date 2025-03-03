/*
* Copyright 2024 NVIDIA Corporation.
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

#include <cstring>
#include <inttypes.h>
#include <vulkan/vulkan.h>
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"
#include "VulkanDeviceContext.h"
#include "VulkanDeviceMemoryImpl.h"
#include "VkImageResource.h"
#include "VulkanDecodedFrame.h"
#include "Helpers.h"
#include "VkVideoFrameOutput.h"

// CRC32 lookup table
static unsigned long Crc32Table[256] = {
    0x00000000,0x77073096,0xee0e612c,0x990951ba,
    0x076dc419,0x706af48f,0xe963a535,0x9e6495a3,
    0x0edb8832,0x79dcb8a4,0xe0d5e91e,0x97d2d988,
    0x09b64c2b,0x7eb17cbd,0xe7b82d07,0x90bf1d91,
    0x1db71064,0x6ab020f2,0xf3b97148,0x84be41de,
    0x1adad47d,0x6ddde4eb,0xf4d4b551,0x83d385c7,
    0x136c9856,0x646ba8c0,0xfd62f97a,0x8a65c9ec,
    0x14015c4f,0x63066cd9,0xfa0f3d63,0x8d080df5,
    0x3b6e20c8,0x4c69105e,0xd56041e4,0xa2677172,
    0x3c03e4d1,0x4b04d447,0xd20d85fd,0xa50ab56b,
    0x35b5a8fa,0x42b2986c,0xdbbbc9d6,0xacbcf940,
    0x32d86ce3,0x45df5c75,0xdcd60dcf,0xabd13d59,
    0x26d930ac,0x51de003a,0xc8d75180,0xbfd06116,
    0x21b4f4b5,0x56b3c423,0xcfba9599,0xb8bda50f,
    0x2802b89e,0x5f058808,0xc60cd9b2,0xb10be924,
    0x2f6f7c87,0x58684c11,0xc1611dab,0xb6662d3d,
    0x76dc4190,0x01db7106,0x98d220bc,0xefd5102a,
    0x71b18589,0x06b6b51f,0x9fbfe4a5,0xe8b8d433,
    0x7807c9a2,0x0f00f934,0x9609a88e,0xe10e9818,
    0x7f6a0dbb,0x086d3d2d,0x91646c97,0xe6635c01,
    0x6b6b51f4,0x1c6c6162,0x856530d8,0xf262004e,
    0x6c0695ed,0x1b01a57b,0x8208f4c1,0xf50fc457,
    0x65b0d9c6,0x12b7e950,0x8bbeb8ea,0xfcb9887c,
    0x62dd1ddf,0x15da2d49,0x8cd37cf3,0xfbd44c65,
    0x4db26158,0x3ab551ce,0xa3bc0074,0xd4bb30e2,
    0x4adfa541,0x3dd895d7,0xa4d1c46d,0xd3d6f4fb,
    0x4369e96a,0x346ed9fc,0xad678846,0xda60b8d0,
    0x44042d73,0x33031de5,0xaa0a4c5f,0xdd0d7cc9,
    0x5005713c,0x270241aa,0xbe0b1010,0xc90c2086,
    0x5768b525,0x206f85b3,0xb966d409,0xce61e49f,
    0x5edef90e,0x29d9c998,0xb0d09822,0xc7d7a8b4,
    0x59b33d17,0x2eb40d81,0xb7bd5c3b,0xc0ba6cad,
    0xedb88320,0x9abfb3b6,0x03b6e20c,0x74b1d29a,
    0xead54739,0x9dd277af,0x04db2615,0x73dc1683,
    0xe3630b12,0x94643b84,0x0d6d6a3e,0x7a6a5aa8,
    0xe40ecf0b,0x9309ff9d,0x0a00ae27,0x7d079eb1,
    0xf00f9344,0x8708a3d2,0x1e01f268,0x6906c2fe,
    0xf762575d,0x806567cb,0x196c3671,0x6e6b06e7,
    0xfed41b76,0x89d32be0,0x10da7a5a,0x67dd4acc,
    0xf9b9df6f,0x8ebeeff9,0x17b7be43,0x60b08ed5,
    0xd6d6a3e8,0xa1d1937e,0x38d8c2c4,0x4fdff252,
    0xd1bb67f1,0xa6bc5767,0x3fb506dd,0x48b2364b,
    0xd80d2bda,0xaf0a1b4c,0x36034af6,0x41047a60,
    0xdf60efc3,0xa867df55,0x316e8eef,0x4669be79,
    0xcb61b38c,0xbc66831a,0x256fd2a0,0x5268e236,
    0xcc0c7795,0xbb0b4703,0x220216b9,0x5505262f,
    0xc5ba3bbe,0xb2bd0b28,0x2bb45a92,0x5cb36a04,
    0xc2d7ffa7,0xb5d0cf31,0x2cd99e8b,0x5bdeae1d,
    0x9b64c2b0,0xec63f226,0x756aa39c,0x026d930a,
    0x9c0906a9,0xeb0e363f,0x72076785,0x05005713,
    0x95bf4a82,0xe2b87a14,0x7bb12bae,0x0cb61b38,
    0x92d28e9b,0xe5d5be0d,0x7cdcefb7,0x0bdbdf21,
    0x86d3d2d4,0xf1d4e242,0x68ddb3f8,0x1fda836e,
    0x81be16cd,0xf6b9265b,0x6fb077e1,0x18b74777,
    0x88085ae6,0xff0f6a70,0x66063bca,0x11010b5c,
    0x8f659eff,0xf862ae69,0x616bffd3,0x166ccf45,
    0xa00ae278,0xd70dd2ee,0x4e048354,0x3903b3c2,
    0xa7672661,0xd06016f7,0x4969474d,0x3e6e77db,
    0xaed16a4a,0xd9d65adc,0x40df0b66,0x37d83bf0,
    0xa9bcae53,0xdebb9ec5,0x47b2cf7f,0x30b5ffe9,
    0xbdbdf21c,0xcabac28a,0x53b39330,0x24b4a3a6,
    0xbad03605,0xcdd70693,0x54de5729,0x23d967bf,
    0xb3667a2e,0xc4614ab8,0x5d681b02,0x2a6f2b94,
    0xb40bbe37,0xc30c8ea1,0x5a05df1b,0x2d02ef8d
};

static void getCRC(uint32_t *checksum, const uint8_t *inputBytes, size_t length, unsigned long crcTable[]) {
    for (size_t i = 0; i < length; i += 1) {
        *checksum = crcTable[inputBytes[i] ^ (*checksum & 0xff)] ^ (*checksum >> 8);
    }
}

template<typename T>
static void CopyPlaneData(const uint8_t* pSrc, uint8_t* pDst,
                         size_t srcRowPitch, size_t dstRowPitch,
                         int32_t width, int32_t height,
                         size_t srcPixelStride = 1) {
    const T* src = reinterpret_cast<const T*>(pSrc);
    T* dst = reinterpret_cast<T*>(pDst);
    const size_t srcStride = srcRowPitch / sizeof(T);
    const size_t dstStride = dstRowPitch / sizeof(T);

    for (int32_t y = 0; y < height; y++) {
        if (srcPixelStride == 1) {
            memcpy(dst, src, width * sizeof(T));
        } else {
            for (int32_t x = 0; x < width; x++) {
                dst[x] = src[x * srcPixelStride];
            }
        }
        src += srcStride;
        dst += dstStride;
    }
}

class VkVideoFrameToFileImpl : public VkVideoFrameOutput {
public:
    VkVideoFrameToFileImpl(bool outputy4m,
                          bool outputcrcPerFrame,
                          const char* crcOutputFile,
                          const std::vector<uint32_t>& crcInitValue)
        : m_refCount(0)
        , m_outputFile(nullptr)
        , m_pLinearMemory(nullptr)
        , m_allocationSize(0)
        , m_firstFrame(true)
        , m_height(0)
        , m_width(0)
        , m_outputy4m(outputy4m)
        , m_outputcrcPerFrame(outputcrcPerFrame)
        , m_crcOutputFile(nullptr)
        , m_crcInitValue(crcInitValue)
        , m_crcAllocation() {
        if (crcOutputFile != nullptr) {
            m_crcOutputFile = fopen(crcOutputFile, "w");
            if (m_crcOutputFile && !m_crcInitValue.empty()) {
                m_crcAllocation.resize(m_crcInitValue.size());
                for (size_t i = 0; i < m_crcInitValue.size(); i += 1) {
                    m_crcAllocation[i] = m_crcInitValue[i];
                }
            }
        }
    }

    virtual ~VkVideoFrameToFileImpl() override {
        if (m_pLinearMemory) {
            delete[] m_pLinearMemory;
            m_pLinearMemory = nullptr;
        }

        if (m_outputFile) {
            fclose(m_outputFile);
            m_outputFile = nullptr;
        }

        if (m_crcOutputFile) {
            if (!m_crcAllocation.empty()) {
                fprintf(m_crcOutputFile, "CRC: ");
                for (size_t i = 0; i < m_crcInitValue.size(); i += 1) {
                    fprintf(m_crcOutputFile, "0x%08X ", m_crcAllocation[i]);
                }
                fprintf(m_crcOutputFile, "\n");
            }

            if (m_crcOutputFile != stdout) {
                fclose(m_crcOutputFile);
            }
            m_crcOutputFile = nullptr;
        }
    }

    virtual int32_t AddRef() override {
        return ++m_refCount;
    }

    virtual int32_t Release() override {
        uint32_t ret = --m_refCount;
        if (ret == 0) {
            delete this;
        }
        return ret;
    }

    virtual size_t OutputFrame(VulkanDecodedFrame* pFrame, const VulkanDeviceContext* vkDevCtx) override {
        if (!IsFileStreamValid()) {
            return (size_t)-1;
        }

        assert(pFrame != nullptr);

        VkSharedBaseObj<VkImageResourceView> imageResourceView;
        pFrame->imageViews[VulkanDecodedFrame::IMAGE_VIEW_TYPE_LINEAR].GetImageResourceView(imageResourceView);
        assert(!!imageResourceView);
        assert(pFrame->pictureIndex != -1);

        VkSharedBaseObj<VkImageResource> imageResource = imageResourceView->GetImageResource();
        uint8_t* pOutputBuffer = EnsureAllocation(vkDevCtx, imageResource);
        assert(pOutputBuffer != nullptr);

        assert((pFrame->displayWidth >= 0) && (pFrame->displayHeight >= 0));

        WaitAndGetStatus(vkDevCtx,
                        *vkDevCtx,
                        pFrame->frameCompleteFence,
                        pFrame->queryPool,
                        pFrame->startQueryId,
                        pFrame->pictureIndex, false, "frameCompleteFence");

        VkFormat format = imageResource->GetImageCreateInfo().format;
        const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(format);
        size_t usedBufferSize = ConvertFrameToNv12(vkDevCtx, pFrame->displayWidth, pFrame->displayHeight,
                                                  imageResource, pOutputBuffer, mpInfo);

        if (m_outputcrcPerFrame && m_crcOutputFile) {
            fprintf(m_crcOutputFile, "CRC Frame[%" PRId64 "]:", pFrame->displayOrder);
            for (size_t i = 0; i < m_crcInitValue.size(); i += 1) {
                uint32_t frameCrc = m_crcInitValue[i];
                getCRC(&frameCrc, pOutputBuffer, usedBufferSize, Crc32Table);
                fprintf(m_crcOutputFile, "0x%08X ", frameCrc);
            }
            fprintf(m_crcOutputFile, "\n");
            if (m_crcOutputFile != stdout) {
                fflush(m_crcOutputFile);
            }
        }

        if (!m_crcAllocation.empty()) {
            for (size_t i = 0; i < m_crcAllocation.size(); i += 1) {
                getCRC(&m_crcAllocation[i], pOutputBuffer, usedBufferSize, Crc32Table);
            }
        }

        if (m_outputy4m) {
            return WriteFrameToFileY4M(0, usedBufferSize, pFrame->displayWidth, pFrame->displayHeight, mpInfo);
        } else {
            return WriteDataToFile(0, usedBufferSize);
        }
    }

    FILE* AttachFile(const char* fileName) {
        if (m_outputFile) {
            fclose(m_outputFile);
            m_outputFile = nullptr;
        }

        if (fileName != nullptr) {
            m_outputFile = fopen(fileName, "wb");
            if (m_outputFile) {
                return m_outputFile;
            }
        }

        return nullptr;
    }

    bool IsFileStreamValid() const {
        return m_outputFile != nullptr;
    }

    operator bool() const {
        return IsFileStreamValid();
    }

    size_t WriteDataToFile(size_t offset, size_t size) {
        return fwrite(m_pLinearMemory + offset, size, 1, m_outputFile);
    }

    size_t GetMaxFrameSize() {
        return m_allocationSize;
    }

    size_t WriteFrameToFileY4M(size_t offset, size_t size, size_t width, size_t height,
                              const VkMpFormatInfo* mpInfo) {
        if (m_firstFrame != false) {
            m_firstFrame = false;
            fprintf(m_outputFile, "YUV4MPEG2 ");
            fprintf(m_outputFile, "W%i H%i ", (int)width, (int)height);
            m_height = height;
            m_width = width;
            fprintf(m_outputFile, "F24:1 ");
            fprintf(m_outputFile, "Ip ");
            fprintf(m_outputFile, "A1:1 ");
            if (mpInfo->planesLayout.secondaryPlaneSubsampledX == false) {
                fprintf(m_outputFile, "C444");
            } else {
                fprintf(m_outputFile, "C420");
            }

            if (mpInfo->planesLayout.bpp != YCBCRA_8BPP) {
                fprintf(m_outputFile, "p16");
            }

            fprintf(m_outputFile, "\n");
        }

        fprintf(m_outputFile, "FRAME");
        if ((m_width != width) || (m_height != height)) {
            fprintf(m_outputFile, " ");
            fprintf(m_outputFile, "W%i H%i", (int)width, (int)height);
            m_height = height;
            m_width = width;
        }

        fprintf(m_outputFile, "\n");
        return WriteDataToFile(offset, size);
    }

    size_t ConvertFrameToNv12(const VulkanDeviceContext* vkDevCtx, int32_t frameWidth, int32_t frameHeight,
                             VkSharedBaseObj<VkImageResource>& imageResource,
                             uint8_t* pOutBuffer, const VkMpFormatInfo* mpInfo) {
        size_t outputBufferSize = 0;
        VkDevice device   = imageResource->GetDevice();
        VkImage  srcImage = imageResource->GetImage();
        VkSharedBaseObj<VulkanDeviceMemoryImpl> srcImageDeviceMemory(imageResource->GetMemory());

        // Map the image and read the image data.
        VkDeviceSize imageOffset = imageResource->GetImageDeviceMemoryOffset();
        VkDeviceSize maxSize = 0;
        const uint8_t* readImagePtr = srcImageDeviceMemory->GetReadOnlyDataPtr(imageOffset, maxSize);
        assert(readImagePtr != nullptr);

        int32_t secondaryPlaneHeight = frameHeight;
        int32_t imageHeight = frameHeight;
        bool isUnnormalizedRgba = false;
        if (mpInfo && (mpInfo->planesLayout.layout == YCBCR_SINGLE_PLANE_UNNORMALIZED) && !(mpInfo->planesLayout.disjoint)) {
            isUnnormalizedRgba = true;
        }

        if (mpInfo && mpInfo->planesLayout.secondaryPlaneSubsampledY) {
            secondaryPlaneHeight /= 2;
        }

        VkImageSubresource subResource = {};
        VkSubresourceLayout layouts[3] = {};

        if (mpInfo && !isUnnormalizedRgba) {
            switch (mpInfo->planesLayout.layout) {
                case YCBCR_SINGLE_PLANE_UNNORMALIZED:
                case YCBCR_SINGLE_PLANE_INTERLEAVED:
                    subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                    vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[0]);
                    break;
                case YCBCR_SEMI_PLANAR_CBCR_INTERLEAVED:
                    subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                    vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[0]);
                    subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
                    vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[1]);
                    break;
                case YCBCR_PLANAR_CBCR_STRIDE_INTERLEAVED:
                case YCBCR_PLANAR_CBCR_BLOCK_JOINED:
                case YCBCR_PLANAR_STRIDE_PADDED:
                    subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
                    vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[0]);
                    subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
                    vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[1]);
                    subResource.aspectMask = VK_IMAGE_ASPECT_PLANE_2_BIT;
                    vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[2]);
                    break;
                default:
                    assert(0);
            }
        } else {
            vkDevCtx->GetImageSubresourceLayout(device, srcImage, &subResource, &layouts[0]);
        }

        const bool is8Bit = (mpInfo->planesLayout.bpp == YCBCRA_8BPP);
        const uint32_t bytesPerPixel = is8Bit ? 1 : 2;
        const uint32_t numPlanes = 3;

        // Calculate plane layouts for output buffer
        VkSubresourceLayout yuvPlaneLayouts[3] = {};
        yuvPlaneLayouts[0].offset = 0;
        yuvPlaneLayouts[0].rowPitch = frameWidth * bytesPerPixel;
        yuvPlaneLayouts[1].offset = yuvPlaneLayouts[0].rowPitch * frameHeight;
        yuvPlaneLayouts[1].rowPitch = frameWidth * bytesPerPixel;
        if (mpInfo && mpInfo->planesLayout.secondaryPlaneSubsampledX) {
            yuvPlaneLayouts[1].rowPitch /= 2;
        }
        yuvPlaneLayouts[2].offset = yuvPlaneLayouts[1].offset + (yuvPlaneLayouts[1].rowPitch * secondaryPlaneHeight);
        yuvPlaneLayouts[2].rowPitch = frameWidth * bytesPerPixel;
        if (mpInfo && mpInfo->planesLayout.secondaryPlaneSubsampledX) {
            yuvPlaneLayouts[2].rowPitch /= 2;
        }

        // Copy the luma plane
        const uint32_t numCompatiblePlanes = 1;
        for (uint32_t plane = 0; plane < numCompatiblePlanes; plane++) {
            const uint8_t* pSrc = readImagePtr + layouts[plane].offset;
            uint8_t* pDst = pOutBuffer + yuvPlaneLayouts[plane].offset;

            if (is8Bit) {
                CopyPlaneData<uint8_t>(pSrc, pDst, layouts[plane].rowPitch, yuvPlaneLayouts[plane].rowPitch,
                                      frameWidth, imageHeight);
            } else {
                CopyPlaneData<uint16_t>(pSrc, pDst, layouts[plane].rowPitch, yuvPlaneLayouts[plane].rowPitch,
                                       frameWidth, imageHeight);
            }
        }

        // Copy chroma planes
        for (uint32_t plane = numCompatiblePlanes; plane < numPlanes; plane++) {
            const uint32_t srcPlane = std::min(plane, mpInfo->planesLayout.numberOfExtraPlanes);
            uint8_t* pDst = pOutBuffer + yuvPlaneLayouts[plane].offset;
            const int32_t planeWidth = mpInfo->planesLayout.secondaryPlaneSubsampledX ? frameWidth / 2 : frameWidth;

            for (int32_t height = 0; height < secondaryPlaneHeight; height++) {
                const uint8_t* pSrc;
                if (srcPlane != plane) {
                    pSrc = readImagePtr + layouts[srcPlane].offset + ((plane - 1) * bytesPerPixel) + (layouts[srcPlane].rowPitch * height);
                } else {
                    pSrc = readImagePtr + layouts[srcPlane].offset + (layouts[srcPlane].rowPitch * height);
                }

                if (is8Bit) {
                    CopyPlaneData<uint8_t>(pSrc, pDst, layouts[srcPlane].rowPitch, yuvPlaneLayouts[plane].rowPitch,
                                           planeWidth, 1, 2);
                } else {
                    CopyPlaneData<uint16_t>(pSrc, pDst, layouts[srcPlane].rowPitch, yuvPlaneLayouts[plane].rowPitch,
                                            planeWidth, 1, 2);
                }
                pDst += yuvPlaneLayouts[plane].rowPitch;
            }
        }

        // Calculate total buffer size
        outputBufferSize = yuvPlaneLayouts[0].rowPitch * imageHeight;
        if (mpInfo->planesLayout.numberOfExtraPlanes >= 1) {
            outputBufferSize += yuvPlaneLayouts[1].rowPitch * secondaryPlaneHeight;
            outputBufferSize += yuvPlaneLayouts[2].rowPitch * secondaryPlaneHeight;
        }

        return outputBufferSize;
    }

private:
    uint8_t* EnsureAllocation(const VulkanDeviceContext* vkDevCtx,
                             VkSharedBaseObj<VkImageResource>& imageResource) {
        if (m_outputFile == nullptr) {
            return nullptr;
        }

        VkDeviceSize imageMemorySize = imageResource->GetImageDeviceMemorySize();

        if ((m_pLinearMemory == nullptr) || (imageMemorySize > m_allocationSize)) {
            if (m_outputFile) {
                fflush(m_outputFile);
            }

            if (m_pLinearMemory != nullptr) {
                delete[] m_pLinearMemory;
                m_pLinearMemory = nullptr;
            }

            m_allocationSize = (size_t)(imageMemorySize);
            m_pLinearMemory = new uint8_t[m_allocationSize];
            if (m_pLinearMemory == nullptr) {
                return nullptr;
            }
            assert(m_pLinearMemory != nullptr);
        }
        return m_pLinearMemory;
    }

private:
    std::atomic<int32_t>    m_refCount;
    FILE*    m_outputFile;
    uint8_t* m_pLinearMemory;
    size_t   m_allocationSize;
    bool     m_firstFrame;
    size_t   m_height;
    size_t   m_width;
    bool     m_outputy4m;
    bool     m_outputcrcPerFrame;
    FILE*    m_crcOutputFile;
    std::vector<uint32_t> m_crcInitValue;
    std::vector<uint32_t> m_crcAllocation;
};

// Define the static member for invalid instance
static VkSharedBaseObj<VkVideoFrameOutput> s_invalidFrameToFile;
VkSharedBaseObj<VkVideoFrameOutput>& VkVideoFrameOutput::invalidFrameToFile = s_invalidFrameToFile;

VkResult VkVideoFrameOutput::Create(const char* fileName,
                                   bool outputy4m,
                                   bool outputcrcPerFrame,
                                   const char* crcOutputFile,
                                   const std::vector<uint32_t>& crcInitValue,
                                   VkSharedBaseObj<VkVideoFrameOutput>& frameToFile) {
    VkVideoFrameToFileImpl* newFrameToFile = new VkVideoFrameToFileImpl(outputy4m, outputcrcPerFrame,
                                                                       crcOutputFile, crcInitValue);
    if (!newFrameToFile) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    FILE* outFile = newFrameToFile->AttachFile(fileName);
    if ((fileName != nullptr) && (outFile == nullptr)) {
        delete newFrameToFile;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    frameToFile = newFrameToFile;
    return VK_SUCCESS;
}
