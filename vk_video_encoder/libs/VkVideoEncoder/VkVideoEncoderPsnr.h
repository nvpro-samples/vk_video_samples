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

#ifndef _VKVIDEOENCODER_VKVIDEOENCODERPSNR_H_
#define _VKVIDEOENCODER_VKVIDEOENCODERPSNR_H_

#include <vector>
#include "VkCodecUtils/VkVideoRefCountBase.h"
#include "VkCodecUtils/VulkanVideoImagePool.h"
#include "VkVideoEncoder/VkEncoderConfig.h"
#include "vulkan/vulkan.h"

class VulkanDeviceContext;
namespace vkvideoencoder { struct VkVideoEncodeFrameInfo; }

/**
 * PSNR computation for video encoder: owns staging image pool, captures input,
 * records DPB->staging copy, and computes per-frame PSNR.
 * Managed by VkVideoEncoder via VkSharedBaseObj; use Create(), Configure(), Deinit(), Enabled().
 */
class VkVideoEncoderPsnr : public VkVideoRefCountBase {
public:
    struct FrameData {
        std::vector<uint8_t>                               psnrInputY;
        std::vector<uint8_t>                               psnrInputU;
        std::vector<uint8_t>                               psnrInputV;
        VkSharedBaseObj<VulkanVideoImagePoolNode>          psnrStagingImage;
    };

    static VkResult Create(VkSharedBaseObj<VkVideoEncoderPsnr>& psnr);
    bool Enabled() const { return (m_encoderConfig != nullptr) && m_encoderConfig->IsPsnrMetricsEnabled() && (m_psnrReconImagePool != nullptr); }

    VkResult Configure(const VulkanDeviceContext* vkDevCtx,
                       VkSharedBaseObj<EncoderConfig>& encoderConfig,
                       uint32_t maxEncodeQueueDepth,
                       VkFormat imageDpbFormat,
                       const VkExtent2D& imageExtent,
                       uint32_t encodeQueueFamilyIndex);

    void CaptureInput(void* encodeFrameInfo, const uint8_t* pInputFrameData);
    bool CaptureOutput(VkCommandBuffer cmdBuf, void* encodeFrameInfo);
    void ComputeFramePsnr(void* encodeFrameInfo);
    double GetAveragePsnrY() const;
    double GetAveragePsnrU() const;
    double GetAveragePsnrV() const;
    /** @deprecated Prefer GetAveragePsnrY(); identical return value. */
    double GetAveragePsnr() const;
    void Deinit();

public:
    ~VkVideoEncoderPsnr() override;

private:
    const VulkanDeviceContext* m_vkDevCtx = nullptr;
    VkSharedBaseObj<EncoderConfig> m_encoderConfig;
    VkResult m_initResult = VK_SUCCESS;
    uint32_t m_maxEncodeQueueDepth = 0;
    VkFormat m_imageDpbFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_imageExtent = {};
    uint32_t m_encodeQueueFamilyIndex = 0;

    VkSharedBaseObj<VulkanVideoImagePool> m_psnrReconImagePool;
    double m_psnrSum = 0.0;
    double m_psnrSumU = 0.0;
    double m_psnrSumV = 0.0;
    uint32_t m_psnrFrameCount = 0;
    std::vector<uint8_t> m_psnrReconY;
    std::vector<uint8_t> m_psnrReconU;
    std::vector<uint8_t> m_psnrReconV;
};

#endif /* _VKVIDEOENCODER_VKVIDEOENCODERPSNR_H_ */
