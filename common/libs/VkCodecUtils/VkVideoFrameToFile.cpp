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
#include <string>
#include <inttypes.h>
#include <vulkan/vulkan.h>
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"
#include "VulkanDeviceContext.h"
#include "VulkanDeviceMemoryImpl.h"
#include "VkImageResource.h"
#include "VulkanDecodedFrame.h"
#include "Helpers.h"
#include "VkVideoFrameOutput.h"
#include "VkCodecUtils/VkVideoCrc.h"

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
        : m_outputFile(nullptr)
        , m_pLinearMemory(nullptr)
        , m_allocationSize(0)
        , m_firstFrame(true)
        , m_height(0)
        , m_width(0)
        , m_outputy4m(outputy4m)
        , m_frameRateNum(30)
        , m_frameRateDen(1)
        , m_crc() {
        const std::string crcPath = (crcOutputFile != nullptr) ? std::string(crcOutputFile) : std::string();
        m_crc.BeginCrcCalculation(crcInitValue, outputcrcPerFrame, crcPath);
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

        m_crc.EndCrcCalculation(true);
    }

    virtual void SetFrameRate(uint32_t frameRateNum, uint32_t frameRateDen) override {
        if (frameRateNum != 0 && frameRateDen != 0) {
            m_frameRateNum = frameRateNum;
            m_frameRateDen = frameRateDen;
        }
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

        // Wait for decode+filter to complete using timeline semaphore.
        // The TL semaphore value is stable (tied to decode order, not to a
        // reusable fence), so it cannot be reset by slot recycling.
        if (pFrame->frameCompleteSemaphore != VK_NULL_HANDLE &&
            pFrame->frameCompleteDoneSemValue > 0) {
            uint64_t preWaitValue = 0;
            vkDevCtx->GetSemaphoreCounterValue(*vkDevCtx, pFrame->frameCompleteSemaphore, &preWaitValue);
            VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                             nullptr, 0, 1,
                                             &pFrame->frameCompleteSemaphore,
                                             &pFrame->frameCompleteDoneSemValue };
            VkResult semResult = vkDevCtx->WaitSemaphores(*vkDevCtx, &waitInfo,
                                                           10ULL * 1000ULL * 1000ULL * 1000ULL /* 10s */);
            if (semResult != VK_SUCCESS) {
                uint64_t postWaitValue = 0;
                vkDevCtx->GetSemaphoreCounterValue(*vkDevCtx, pFrame->frameCompleteSemaphore, &postWaitValue);
                fprintf(stderr, "ERROR: Timeline semaphore wait for frame %d (displayOrder=%lld) "
                        "failed with result %d (expected TL=%llu, before=%llu, after=%llu)\n",
                        pFrame->pictureIndex, (long long)pFrame->displayOrder,
                        semResult, (unsigned long long)pFrame->frameCompleteDoneSemValue,
                        (unsigned long long)preWaitValue, (unsigned long long)postWaitValue);
            }
        } else if (pFrame->frameCompleteFence != VK_NULL_HANDLE) {
            // Fallback to fence if no TL semaphore available
            WaitAndGetStatus(vkDevCtx,
                            *vkDevCtx,
                            pFrame->frameCompleteFence,
                            pFrame->queryPool,
                            pFrame->startQueryId,
                            pFrame->pictureIndex, false, "frameCompleteFence");
        }
        // else: both semaphore and fence are null — caller (dump pool) already handled sync

        VkFormat format = imageResource->GetImageCreateInfo().format;
        const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(format);
        size_t usedBufferSize = ConvertFrameToNv12(vkDevCtx, pFrame->displayWidth, pFrame->displayHeight,
                                                  imageResource, pOutputBuffer, mpInfo);

        if (m_crc.Enabled()) {
            m_crc.UpdateCrc(pOutputBuffer, usedBufferSize);
            m_crc.SignalFrameEnd(static_cast<uint32_t>(pFrame->displayOrder));
        }

        int64_t writeResult;
        if (m_outputy4m) {
            writeResult = WriteFrameToFileY4M(0, usedBufferSize, pFrame->displayWidth, pFrame->displayHeight, mpInfo);
        } else {
            writeResult = WriteDataToFile(0, usedBufferSize);
        }

        // Signal consumer-done timeline semaphore so the decoder knows
        // this frame slot can be safely reused.
        if (pFrame->consumerCompleteSemaphore != VK_NULL_HANDLE &&
            pFrame->frameConsumerDoneSemValue > 0) {
            VkSemaphoreSignalInfo signalInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                                  nullptr,
                                                  pFrame->consumerCompleteSemaphore,
                                                  pFrame->frameConsumerDoneSemValue };
            vkDevCtx->SignalSemaphore(*vkDevCtx, &signalInfo);
            pFrame->hasConsummerSignalSemaphore = true;
        }

        return writeResult;
    }

    bool hasExtension(const char* fileName, const char* extension) {
        size_t fileLen = std::strlen(fileName);
        size_t extLen = std::strlen(extension);

        if (fileLen < extLen) {
            return false;
        }

        return std::strcmp(fileName + fileLen - extLen, extension) == 0;
    }

    FILE* AttachFile(const char* fileName, bool y4mFormat) {
        if (m_outputFile) {
            fclose(m_outputFile);
            m_outputFile = nullptr;
        }

        std::string fileNameWithModExt;
        // Check if the file does not have a y4m extension,
        // but y4m format is requested.
        if (y4mFormat && !hasExtension(fileName, ".y4m")) {
            std::cout << std::endl << "y4m output format is requested, ";
            std::cout << "but the output file's (" << fileName << ") extension isn't .y4m!"
                      << std::endl;
            fileNameWithModExt = fileName + std::string(".y4m");
            fileName = fileNameWithModExt.c_str();
        } else if ((y4mFormat == false) && !hasExtension(fileName, ".yuv")) {
            std::cout << std::endl << "Raw yuv output format is requested, ";
            std::cout << "but the output file's (" << fileName << ") extension isn't .yuv!"
                      << std::endl;
            fileNameWithModExt = fileName + std::string(".yuv");
            fileName = fileNameWithModExt.c_str();
        }

        if (fileName != nullptr) {
            m_outputFile = fopen(fileName, "wb");
            if (m_outputFile) {
#if !defined(VK_VIDEO_NO_STDOUT_INFO)
                std::cout << "Output file name is: " << fileName << std::endl;
#endif
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
            fprintf(m_outputFile, "F%u:%u ", (unsigned)m_frameRateNum, (unsigned)m_frameRateDen);
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
        assert(maxSize <= SIZE_MAX);  // Ensure we don't lose data in conversion

        int32_t secondaryPlaneWidth = frameWidth;
        int32_t secondaryPlaneHeight = frameHeight;
        int32_t imageHeight = frameHeight;
        bool isUnnormalizedRgba = false;
        if (mpInfo && (mpInfo->planesLayout.layout == YCBCR_SINGLE_PLANE_UNNORMALIZED) && !(mpInfo->planesLayout.disjoint)) {
            isUnnormalizedRgba = true;
        }

        if (mpInfo && mpInfo->planesLayout.secondaryPlaneSubsampledX) {
            secondaryPlaneWidth = (secondaryPlaneWidth + 1) / 2;
        }
        if (mpInfo && mpInfo->planesLayout.secondaryPlaneSubsampledY) {
            secondaryPlaneHeight = (secondaryPlaneHeight + 1) / 2;
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
        yuvPlaneLayouts[1].rowPitch = secondaryPlaneWidth * bytesPerPixel;
        yuvPlaneLayouts[2].offset = yuvPlaneLayouts[1].offset + (yuvPlaneLayouts[1].rowPitch * secondaryPlaneHeight);
        yuvPlaneLayouts[2].rowPitch = secondaryPlaneWidth * bytesPerPixel;

        // Copy the luma plane
        const uint32_t numCompatiblePlanes = 1;
        for (uint32_t plane = 0; plane < numCompatiblePlanes; plane++) {
            const uint8_t* pSrc = readImagePtr + static_cast<size_t>(layouts[plane].offset);
            uint8_t* pDst = pOutBuffer + static_cast<size_t>(yuvPlaneLayouts[plane].offset);

            if (is8Bit) {
                assert(layouts[plane].rowPitch <= SIZE_MAX);
                assert(yuvPlaneLayouts[plane].rowPitch <= SIZE_MAX);
                CopyPlaneData<uint8_t>(pSrc, pDst, static_cast<size_t>(layouts[plane].rowPitch), static_cast<size_t>(yuvPlaneLayouts[plane].rowPitch),
                                      frameWidth, imageHeight);
            } else {
                assert(layouts[plane].rowPitch <= SIZE_MAX);
                assert(yuvPlaneLayouts[plane].rowPitch <= SIZE_MAX);
                CopyPlaneData<uint16_t>(pSrc, pDst, static_cast<size_t>(layouts[plane].rowPitch), static_cast<size_t>(yuvPlaneLayouts[plane].rowPitch),
                                       frameWidth, imageHeight);
            }
        }

        // Copy chroma planes
        for (uint32_t plane = numCompatiblePlanes; plane < numPlanes; plane++) {
            const uint32_t srcPlane = std::min(plane, mpInfo->planesLayout.numberOfExtraPlanes);
            uint8_t* pDst = pOutBuffer + yuvPlaneLayouts[plane].offset;
            const int32_t planeWidth = mpInfo->planesLayout.secondaryPlaneSubsampledX ? (frameWidth + 1) / 2 : frameWidth;

            for (int32_t height = 0; height < secondaryPlaneHeight; height++) {
                const uint8_t* pSrc;
                if (srcPlane != plane) {
                    pSrc = readImagePtr + layouts[srcPlane].offset + ((plane - 1) * bytesPerPixel) + (layouts[srcPlane].rowPitch * height);
                } else {
                    pSrc = readImagePtr + layouts[srcPlane].offset + (layouts[srcPlane].rowPitch * height);
                }

                if (is8Bit) {
                    assert(layouts[srcPlane].rowPitch <= SIZE_MAX);
                    assert(yuvPlaneLayouts[plane].rowPitch <= SIZE_MAX);
                    CopyPlaneData<uint8_t>(pSrc, pDst, static_cast<size_t>(layouts[srcPlane].rowPitch), static_cast<size_t>(yuvPlaneLayouts[plane].rowPitch),
                                           planeWidth, 1, 2);
                } else {
                    assert(layouts[srcPlane].rowPitch <= SIZE_MAX);
                    assert(yuvPlaneLayouts[plane].rowPitch <= SIZE_MAX);
                    CopyPlaneData<uint16_t>(pSrc, pDst, static_cast<size_t>(layouts[srcPlane].rowPitch), static_cast<size_t>(yuvPlaneLayouts[plane].rowPitch),
                                            planeWidth, 1, 2);
                }
                pDst += yuvPlaneLayouts[plane].rowPitch;
            }
        }

        // Calculate total buffer size
        outputBufferSize = static_cast<size_t>(yuvPlaneLayouts[0].rowPitch * imageHeight);
        if (mpInfo->planesLayout.numberOfExtraPlanes >= 1) {
            outputBufferSize += static_cast<size_t>(yuvPlaneLayouts[1].rowPitch * secondaryPlaneHeight);
            outputBufferSize += static_cast<size_t>(yuvPlaneLayouts[2].rowPitch * secondaryPlaneHeight);
        }

        return outputBufferSize;
    }

    virtual size_t GetCrcValues(uint32_t* pCrcValues, size_t buffSize) const override {
        return m_crc.GetCrcValues(pCrcValues, buffSize);
    }

private:
    uint8_t* EnsureAllocation(const VulkanDeviceContext* vkDevCtx,
                             VkSharedBaseObj<VkImageResource>& imageResource) {
        if (m_outputFile == nullptr) {
            return nullptr;
        }

        VkDeviceSize imageMemorySize = imageResource->GetImageDeviceMemorySize();
        assert(imageMemorySize <= SIZE_MAX);  // Ensure we don't lose data in conversion

        if ((m_pLinearMemory == nullptr) || (imageMemorySize > m_allocationSize)) {
            if (m_outputFile) {
                fflush(m_outputFile);
            }

            if (m_pLinearMemory != nullptr) {
                delete[] m_pLinearMemory;
                m_pLinearMemory = nullptr;
            }

            m_allocationSize = static_cast<size_t>(imageMemorySize);
            m_pLinearMemory = new uint8_t[m_allocationSize];
            if (m_pLinearMemory == nullptr) {
                return nullptr;
            }
            assert(m_pLinearMemory != nullptr);
        }
        return m_pLinearMemory;
    }

private:
    FILE*    m_outputFile;
    uint8_t* m_pLinearMemory;
    size_t   m_allocationSize;
    bool     m_firstFrame;
    size_t   m_height;
    size_t   m_width;
    bool     m_outputy4m;
    uint32_t m_frameRateNum;
    uint32_t m_frameRateDen;
    VkVideoCrc m_crc;
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

    FILE* outFile = newFrameToFile->AttachFile(fileName, outputy4m);
    if ((fileName != nullptr) && (outFile == nullptr)) {
        delete newFrameToFile;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    frameToFile.reset(newFrameToFile);
    return VK_SUCCESS;
}
