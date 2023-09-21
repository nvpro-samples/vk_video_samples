/*
 * Copyright 2022 NVIDIA Corporation.
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

#include "VkEncoderPictureBuffer.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"

VkResult VkEncoderPictureBuffer::CreateVideoQueries(uint32_t numSlots, const VulkanDeviceContext* vkDevCtx, const VkVideoProfileInfoKHR* pEncodeProfile)
{
    VkQueryPoolVideoEncodeFeedbackCreateInfoKHR encodeFeedbackCreateInfo =
         {VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR};

     encodeFeedbackCreateInfo.pNext = pEncodeProfile;
     encodeFeedbackCreateInfo.encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
                                                    VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;

    VkQueryPoolCreateInfo queryPoolCreateInfo = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryPoolCreateInfo.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
    queryPoolCreateInfo.queryCount = numSlots * 2;
    queryPoolCreateInfo.pNext = &encodeFeedbackCreateInfo;

    return m_vkDevCtx->CreateQueryPool(*vkDevCtx, &queryPoolCreateInfo, NULL, &m_queryPool);
}

VkImageLayout VkEncoderPictureBuffer::TransitionLayout(VkCommandBuffer cmdBuf,
                                                       VkSharedBaseObj<VkImageResourceView>& imageView,
                                                       VkImageLayout layout)
{
    uint32_t baseArrayLayer = 0;
    const VkImageMemoryBarrier2KHR imageBarrier = {

            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR, // VkStructureType sType
            nullptr, // const void*     pNext
            VK_PIPELINE_STAGE_2_NONE_KHR, // VkPipelineStageFlags2KHR srcStageMask
            0, // VkAccessFlags2KHR        srcAccessMask
            VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, // VkPipelineStageFlags2KHR dstStageMask;
            VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR, // VkAccessFlags   dstAccessMask
            VK_IMAGE_LAYOUT_UNDEFINED, // VkImageLayout   oldLayout
            layout, // VkImageLayout   newLayout
            VK_QUEUE_FAMILY_IGNORED, // uint32_t        srcQueueFamilyIndex
            (uint32_t)m_vkDevCtx->GetVideoEncodeQueueFamilyIdx(), // uint32_t   dstQueueFamilyIndex
            imageView->GetImageResource()->GetImage(), // VkImage         image;
            {
                // VkImageSubresourceRange   subresourceRange
                VK_IMAGE_ASPECT_COLOR_BIT, // VkImageAspectFlags aspectMask
                0, // uint32_t           baseMipLevel
                1, // uint32_t           levelCount
                baseArrayLayer, // uint32_t           baseArrayLayer
                1, // uint32_t           layerCount;
            },
    };

    const VkDependencyInfoKHR dependencyInfo = {
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
        nullptr,
        VK_DEPENDENCY_BY_REGION_BIT,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &imageBarrier,
    };
    m_vkDevCtx->CmdPipelineBarrier2KHR(cmdBuf, &dependencyInfo);

    return layout;
}

VkResult VkEncoderPictureBuffer::InitReferenceFramePool(uint32_t numImages,
                                                        VkFormat imageFormat,
                                                        VkMemoryPropertyFlags memoryPropertyFlags)
{
    VkImageCreateInfo imageCreateInfo;

    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.pNext = m_videoProfile.GetProfile();
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = imageFormat;
    imageCreateInfo.extent = { m_extent.width, m_extent.height, 1 };
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR; // DPB ONLY
    imageCreateInfo.sharingMode = VK_SHARING_MODE_CONCURRENT; // VK_SHARING_MODE_EXCLUSIVE here makes it not check for queueFamily
    imageCreateInfo.queueFamilyIndexCount = 1;
    imageCreateInfo.pQueueFamilyIndices = &m_queueFamilyIndex;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCreateInfo.flags = 0;

    m_dpbSize = numImages;

    for(uint32_t i = 0; i < numImages; i++) {

        VkSharedBaseObj<VkImageResource> imageResource;
        VkResult result = VkImageResource::Create(m_vkDevCtx,
                                                  &imageCreateInfo,
                                                  memoryPropertyFlags,
                                                  imageResource);
        if (result != VK_SUCCESS) {
            return result;
        }

        uint32_t baseArrayLayer = 0;
        VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                                                     baseArrayLayer, 1 };
        result = VkImageResourceView::Create(m_vkDevCtx, imageResource,
                                             subresourceRange, m_dpb[i]);
        if (result != VK_SUCCESS) {
            return result;
        }
    }
    return VK_SUCCESS;
}

void VkEncoderPictureBuffer::PrepareReferenceImages(VkCommandBuffer cmdBuf)
{
    for (size_t i = 0; i < m_dpbSize; i++) {
        TransitionLayout(cmdBuf, m_dpb[i], VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR);
    }
}

void VkEncoderPictureBuffer::GetReferenceFrameResourcesByIndex(int8_t dpbSlotIdx,
        VkVideoPictureResourceInfoKHR* pictureResources)
{
    VkSharedBaseObj<VkImageResourceView>& refPic = m_dpb[dpbSlotIdx];

    pictureResources->imageViewBinding = refPic->GetImageView();
    pictureResources->codedOffset = { 0, 0 };
    pictureResources->codedExtent = m_extent;
    pictureResources->baseArrayLayer = 0;
}

VkResult VkEncoderPictureBuffer::InitFramePool(const VulkanDeviceContext* vkDevCtx,
                                              const VkVideoProfileInfoKHR* pEncodeProfile,
                                              uint32_t                 numImages,
                                              VkFormat                 imageFormat,
                                              uint32_t                 maxImageWidth,
                                              uint32_t                 maxImageHeight,
                                              uint32_t                 fullImageSize,
                                              VkImageTiling            tiling,
                                              VkImageUsageFlags        usage,
                                              uint32_t                 queueFamilyIndex)
{
    m_vkDevCtx = vkDevCtx;
    if (m_queryPool != VK_NULL_HANDLE) {
        m_vkDevCtx->DestroyQueryPool(*m_vkDevCtx, m_queryPool, NULL);
        m_queryPool = VK_NULL_HANDLE;
    }

    m_videoProfile.InitFromProfile(pEncodeProfile);

    if (numImages && pEncodeProfile) {
        VkResult result = CreateVideoQueries(numImages, m_vkDevCtx, pEncodeProfile);
        if (result != VK_SUCCESS) {
            return result;
        }
    }
    m_imageFormat = imageFormat;

    m_queueFamilyIndex = queueFamilyIndex;
    m_imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    m_imageCreateInfo.pNext = m_videoProfile.GetProfile();
    m_imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    m_imageCreateInfo.format = imageFormat;
    m_imageCreateInfo.extent = { maxImageWidth, maxImageHeight, 1 };
    m_imageCreateInfo.mipLevels = 1;
    m_imageCreateInfo.arrayLayers = 1;
    m_imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    m_imageCreateInfo.tiling = tiling;
    m_imageCreateInfo.usage = usage;
    m_imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    m_imageCreateInfo.queueFamilyIndexCount = 1;
    m_imageCreateInfo.pQueueFamilyIndices = &m_queueFamilyIndex;
    m_imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_imageCreateInfo.flags = 0;

    m_maxBitstreamSize = ((maxImageWidth > 3840) ? 8 : 4) * 1024 * 1024 /* 4MB or 8MB each for 8k use case */;

    if (numImages) {

        // m_extent is for the codedExtent, not the max image resolution
        m_extent.width = maxImageWidth;
        m_extent.height = maxImageHeight;
        m_fullImageSize = fullImageSize;

        return InitFrame(numImages,
                         vkDevCtx,
                         &m_imageCreateInfo,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    }
    else {
        DeinitFramePool();
    }

    return VK_SUCCESS;
}

void VkEncoderPictureBuffer::PrepareInputImages(VkCommandBuffer cmdBuf)
{
    for (size_t i = 0; i < m_frameBufferSize; i++) {
        TransitionLayout(cmdBuf, m_encodeFrameData[i].m_inputImageView,
                        VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR);
    }
}

void VkEncoderPictureBuffer::GetFrameResourcesByIndex( int8_t encodeFrameSlotIdx,
        VkVideoPictureResourceInfoKHR* pictureResources)
{
    pictureResources->imageViewBinding = m_encodeFrameData[encodeFrameSlotIdx].m_inputImageView->GetImageView();
    assert(pictureResources->sType == VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR);
    pictureResources->codedOffset = { 0, 0 };
    pictureResources->codedExtent = m_extent;
    pictureResources->baseArrayLayer = 0;
}

VkQueryPool VkEncoderPictureBuffer::GetQueryPool()
{
    return m_queryPool;
}

EncodeFrameData* VkEncoderPictureBuffer::GetEncodeFrameData(uint32_t index)
{
    assert(index < m_frameBufferSize);
    return &m_encodeFrameData[index];
}

void VkEncoderPictureBuffer::DeinitFramePool()
{
    if (m_queryPool != VK_NULL_HANDLE) {
        m_vkDevCtx->DestroyQueryPool(*m_vkDevCtx, m_queryPool, NULL);
        m_queryPool = VK_NULL_HANDLE;
    }

    for (uint32_t ndx = 0; ndx < m_frameBufferSize; ndx++) {
        m_encodeFrameData[ndx].DeinitFramePool();
    }
    m_frameBufferSize = 0;
}

void VkEncoderPictureBuffer::DeinitReferenceFramePool()
{
    for (uint32_t ndx = 0; ndx < m_dpbSize; ndx++) {
        m_dpb[ndx] = nullptr;
    }
    m_dpbSize = 0;
}

void EncodeFrameData::DeinitFramePool()
{
    if (m_vkDevCtx == VK_NULL_HANDLE) {
        return;
    }

    if (m_frameCompleteFence != VK_NULL_HANDLE) {
        m_vkDevCtx->DestroyFence(*m_vkDevCtx, m_frameCompleteFence, nullptr);
        m_frameCompleteFence = VK_NULL_HANDLE;
    }

    if (m_frameConsumerDoneFence != VK_NULL_HANDLE) {
        m_vkDevCtx->DestroyFence(*m_vkDevCtx, m_frameConsumerDoneFence, nullptr);
        m_frameConsumerDoneFence = VK_NULL_HANDLE;
    }

    if (m_frameEncodedSemaphore != VK_NULL_HANDLE) {
        m_vkDevCtx->DestroySemaphore(*m_vkDevCtx, m_frameEncodedSemaphore, nullptr);
        m_frameEncodedSemaphore = VK_NULL_HANDLE;
    }

    if (m_frameProducerDoneSemaphore != VK_NULL_HANDLE) {
        m_vkDevCtx->DestroySemaphore(*m_vkDevCtx, m_frameProducerDoneSemaphore, nullptr);
        m_frameProducerDoneSemaphore = VK_NULL_HANDLE;
    }

    m_linearInputImage = nullptr;

    m_outBitstreamBuffer = nullptr;

    m_inputImageView  = nullptr;
}

int32_t VkEncoderPictureBuffer::ConfigRefPics(uint8_t distBetweenAnchors, uint8_t distanceBetweenIntras, uint32_t currentPoc, uint8_t currentEncodeFrameIdx)
{
    if(!m_encodeFrameData[currentEncodeFrameIdx].m_usedDpbMask) { // if mask is not 0 then reset it
        for(int32_t i = 0; i < DECODED_PICTURE_BUFFER_SIZE; i++) {
            m_encodeFrameData[currentEncodeFrameIdx].m_RefPics[i].m_dpbIdx = -1;
            m_encodeFrameData[currentEncodeFrameIdx].m_RefPics[i].m_poc = -1;
        }
        m_encodeFrameData[currentEncodeFrameIdx].m_refCount = 0;
        m_encodeFrameData[currentEncodeFrameIdx].m_usedDpbMask = 0;
    }
    if(!distBetweenAnchors && distanceBetweenIntras == 1) { // Intra Only
        m_encodeFrameData[currentEncodeFrameIdx].m_RefPics[0].m_dpbIdx = 0;
        m_encodeFrameData[currentEncodeFrameIdx].m_RefPics[0].m_poc = currentPoc;
        m_encodeFrameData[currentEncodeFrameIdx].m_refCount = 1;
        m_encodeFrameData[currentEncodeFrameIdx].m_usedDpbMask += 1;
        return 0;
    }
    else {
        fprintf(stderr, "No support for P abd B frames!\n");
        return -1;
    }
}

void VkEncoderPictureBuffer::AddRefPic(uint8_t inImageIdx, int8_t dpbIdx, uint32_t poc)
{
    uint8_t refCount = m_encodeFrameData[inImageIdx].m_refCount;
    if(refCount < DECODED_PICTURE_BUFFER_SIZE) {
        m_encodeFrameData[inImageIdx].m_RefPics[refCount].m_dpbIdx = dpbIdx;
        m_encodeFrameData[inImageIdx].m_RefPics[refCount].m_poc = poc;
        m_encodeFrameData[inImageIdx].m_refCount++;
    }
}

void VkEncoderPictureBuffer::ReleaseRefPic(uint8_t inImageIdx)
{
    uint8_t refCount = m_encodeFrameData[inImageIdx].m_refCount;
    if(refCount > 0) {
        m_encodeFrameData[inImageIdx].m_RefPics[refCount].m_dpbIdx = -1;
        m_encodeFrameData[inImageIdx].m_RefPics[refCount].m_poc = -1;
        m_encodeFrameData[inImageIdx].m_refCount--;
    }
}

VkResult VkEncoderPictureBuffer::InitFrame(uint32_t numImages,
                                           const VulkanDeviceContext* vkDevCtx,
                                           const VkImageCreateInfo* pImageCreateInfo,
                                           VkMemoryPropertyFlags requiredMemProps)
{
    m_frameBufferSize = numImages;

    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFenceCreateInfo fenceFrameCompleteInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    // The fence waited on for the first frame should be signaled.
    fenceFrameCompleteInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

    VkResult result = m_commandBuffersSet.CreateCommandBufferPool(m_vkDevCtx, m_queueFamilyIndex, numImages);
    if (result != VK_SUCCESS) {
        assert(!"ERROR: CreateCommandBufferPool!");
        return result;
    }

    for (uint8_t imageIndex = 0; imageIndex < numImages; imageIndex++) {

        m_encodeFrameData[imageIndex].m_vkDevCtx = vkDevCtx;
        m_encodeFrameData[imageIndex].m_extent = m_extent;
        m_encodeFrameData[imageIndex].m_queueFamilyIndex = m_queueFamilyIndex;
        m_encodeFrameData[imageIndex].m_videoProfile = m_videoProfile;

        VkSharedBaseObj<VkImageResource> inputImageResource;
        result = VkImageResource::Create(vkDevCtx,
                                         pImageCreateInfo,
                                         requiredMemProps,
                                         inputImageResource);
        if (result != VK_SUCCESS) {
            return result;
        }

        VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        result = VkImageResourceView::Create(vkDevCtx, inputImageResource,
                                             subresourceRange,
                                             m_encodeFrameData[imageIndex].m_inputImageView);

        if (result != VK_SUCCESS) {
            return result;
        }

        VkImageCreateInfo linearImageCreateInfo(*pImageCreateInfo);
        linearImageCreateInfo.tiling = VK_IMAGE_TILING_LINEAR;
        linearImageCreateInfo.usage =
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | // copy from this image using transfer
                VK_IMAGE_USAGE_SAMPLED_BIT      | // sample from a texture for use by gfx (for debugging)
                VK_IMAGE_USAGE_STORAGE_BIT;       // copy from this image using compute
        VkSharedBaseObj<VkImageResource> linearInputImageResource;
        result = VkImageResource::Create(vkDevCtx,
                                         &linearImageCreateInfo,
                                         ( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT  |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
                                         linearInputImageResource);
        if (result != VK_SUCCESS) {
            return result;
        }

        result = VkImageResourceView::Create(vkDevCtx, linearInputImageResource,
                                             subresourceRange,
                                             m_encodeFrameData[imageIndex].m_linearInputImage);

        if (result != VK_SUCCESS) {
            return result;
        }

        result = m_vkDevCtx->CreateFence(*m_vkDevCtx, &fenceFrameCompleteInfo, nullptr, &m_encodeFrameData[imageIndex].m_frameCompleteFence);
        result = m_vkDevCtx->CreateFence(*m_vkDevCtx, &fenceInfo, nullptr, &m_encodeFrameData[imageIndex].m_frameConsumerDoneFence);
        assert(result == VK_SUCCESS);
        result = m_vkDevCtx->CreateSemaphore(*m_vkDevCtx, &semInfo, nullptr, &m_encodeFrameData[imageIndex].m_frameEncodedSemaphore);
        assert(result == VK_SUCCESS);
        result = m_vkDevCtx->CreateSemaphore(*m_vkDevCtx, &semInfo, nullptr, &m_encodeFrameData[imageIndex].m_frameProducerDoneSemaphore);
        assert(result == VK_SUCCESS);

        result = VkBufferResource::Create(m_vkDevCtx,
                                          VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          m_maxBitstreamSize,
                                          m_encodeFrameData[imageIndex].m_outBitstreamBuffer);
        if (result != VK_SUCCESS) {
            return result;
        }

        m_encodeFrameData[imageIndex].m_cmdBufVideoEncode = *m_commandBuffersSet.getCommandBuffer(imageIndex);
    }

    return VK_SUCCESS;
}

VkResult VkEncoderPictureBuffer::CopyToVkImage(uint32_t index, uint32_t bufferOffset,
                                               VkCommandBuffer cmdBuf)
{
    EncodeFrameData* currentEncodeFrameData = &m_encodeFrameData[index];
    VkSharedBaseObj<VkImageResourceView>& inputImageView = currentEncodeFrameData->m_inputImageView;

    VkImage inputImage = inputImageView->GetImageResource()->GetImage();
    // FIXME: we are not using a buffer for input staging anymore.
    VkBuffer inputStaging = nullptr; // currentEncodeFrameData->m_linearInputImage->GetBuffer();

    uint32_t width = m_imageCreateInfo.extent.width;
    uint32_t height = m_imageCreateInfo.extent.height;

    const VkMpFormatInfo *formatInfo = YcbcrVkFormatInfo(m_imageFormat);

    VkBufferImageCopy region = {};
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = 0;
    region.imageOffset.y = 0;
    region.imageOffset.z = 0;
    region.imageExtent.depth = 1;

    // FIXME regions
    std::vector<VkBufferImageCopy> copyRegions;
    for (uint32_t plane = 0; plane <= formatInfo->planesLayout.numberOfExtraPlanes; plane++) {
        uint32_t w = 0;
        uint32_t h = 0;

        if ((plane > 0) && formatInfo->planesLayout.secondaryPlaneSubsampledX) { // if subsampled on X divide width by 2
            w = (width + 1) / 2; // add 1 before division in case width is an odd number
        } else {
            w = width;
        }

        if ((plane > 0) && formatInfo->planesLayout.secondaryPlaneSubsampledY) { // if subsampled on Y divide height by 2
            h = (height + 1) / 2; // add 1 before division in case height is an odd number
        } else {
            h = height;
        }

        region.bufferOffset = bufferOffset;
        region.bufferRowLength =  w;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << plane;
        region.imageExtent.width = w;
        region.imageExtent.height = h;

        copyRegions.push_back(region);

        bufferOffset += w * h; // w * h is the size of the plane
    }

    if (currentEncodeFrameData->m_currentImageLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        // Transition the layout to TRANSFER_DST.
        TransitionLayout(cmdBuf, inputImageView, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }

    // Todo change to copy image to image
    m_vkDevCtx->CmdCopyBufferToImage(cmdBuf, inputStaging, inputImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)copyRegions.size(), copyRegions.data());

    if (currentEncodeFrameData->m_currentImageLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        // Restore the original image layout
        currentEncodeFrameData->m_currentImageLayout =
                TransitionLayout(cmdBuf, inputImageView, currentEncodeFrameData->m_currentImageLayout);
    }


    return VK_SUCCESS;
}

VkResult VkEncoderPictureBuffer::CopyToBuffer(VkImage* image, VkBuffer* buffer, VkImageLayout layout,
                                       std::vector<VkBufferImageCopy> &copyRegions, VkCommandBuffer* cmdBuf)
{
    VkResult result = VK_SUCCESS;

    VkCommandBufferBeginInfo cmd_buf_info = VkCommandBufferBeginInfo();
    cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmd_buf_info.pNext = NULL;
    cmd_buf_info.flags = 0;
    cmd_buf_info.pInheritanceInfo = NULL;
    m_vkDevCtx->BeginCommandBuffer(*cmdBuf, &cmd_buf_info);

    m_vkDevCtx->CmdCopyImageToBuffer(*cmdBuf, *image, layout, *buffer, (uint32_t)copyRegions.size(), copyRegions.data());

    m_vkDevCtx->EndCommandBuffer(*cmdBuf);

    VkSubmitInfo submitInfo = VkSubmitInfo();
    submitInfo.pNext = NULL;
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 0;
    submitInfo.pWaitSemaphores = NULL;
    submitInfo.pWaitDstStageMask = NULL;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = cmdBuf;
    submitInfo.signalSemaphoreCount = 0;
    submitInfo.pSignalSemaphores = NULL;

    VkQueue graphicsQueue;
    m_vkDevCtx->GetDeviceQueue(*m_vkDevCtx, 0 /* graphics queue family */, 0, &graphicsQueue);

    result = m_vkDevCtx->QueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to submit clear command\n");
        return result;
    }

    return VK_SUCCESS;
}

VkResult VkEncoderPictureBuffer::CopyLinearToOptimalImage(VkCommandBuffer& commandBuffer,
                             VkSharedBaseObj<VkImageResourceView>& srcImageView,
                             VkSharedBaseObj<VkImageResourceView>& dstImageView,
                             uint32_t srcCopyArrayLayer,
                             uint32_t dstCopyArrayLayer,
                             VkImageLayout srcImageLayout,
                             VkImageLayout dstImageLayout)

{

    const VkSharedBaseObj<VkImageResource>& srcImageResource = srcImageView->GetImageResource();
    const VkSharedBaseObj<VkImageResource>& dstImageResource = dstImageView->GetImageResource();

    const VkFormat format = srcImageResource->GetImageCreateInfo().format;

    // Bind memory for the image.
    const VkMpFormatInfo* mpInfo = YcbcrVkFormatInfo(format);

    // Currently formats that have more than 2 output planes are not supported. 444 formats have a shared CbCr planes in all current tests
    assert((mpInfo->vkPlaneFormat[2] == VK_FORMAT_UNDEFINED) && (mpInfo->vkPlaneFormat[3] == VK_FORMAT_UNDEFINED));

    // Copy src buffer to image.
    VkImageCopy copyRegion[3];
    memset(&copyRegion, 0, sizeof(copyRegion));
    copyRegion[0].extent = srcImageResource->GetImageCreateInfo().extent;
    copyRegion[0].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    copyRegion[0].srcSubresource.mipLevel = 0;
    copyRegion[0].srcSubresource.baseArrayLayer = srcCopyArrayLayer;
    copyRegion[0].srcSubresource.layerCount = 1;
    copyRegion[0].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    copyRegion[0].dstSubresource.mipLevel = 0;
    copyRegion[0].dstSubresource.baseArrayLayer = dstCopyArrayLayer;
    copyRegion[0].dstSubresource.layerCount = 1;
    copyRegion[1].extent.width = copyRegion[0].extent.width;
    if (mpInfo->planesLayout.secondaryPlaneSubsampledX != 0) {
        copyRegion[1].extent.width /= 2;
    }

    copyRegion[1].extent.height = copyRegion[0].extent.height;
    if (mpInfo->planesLayout.secondaryPlaneSubsampledY != 0) {
        copyRegion[1].extent.height /= 2;
    }

    copyRegion[1].extent.depth = 1;
    copyRegion[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    copyRegion[1].srcSubresource.mipLevel = 0;
    copyRegion[1].srcSubresource.baseArrayLayer = srcCopyArrayLayer;
    copyRegion[1].srcSubresource.layerCount = 1;
    copyRegion[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    copyRegion[1].dstSubresource.mipLevel = 0;
    copyRegion[1].dstSubresource.baseArrayLayer = dstCopyArrayLayer;
    copyRegion[1].dstSubresource.layerCount = 1;

    m_vkDevCtx->CmdCopyImage(commandBuffer, srcImageResource->GetImage(), srcImageLayout,
                             dstImageResource->GetImage(), dstImageLayout,
                             (uint32_t)2, copyRegion);

    {
        VkMemoryBarrier memoryBarrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        m_vkDevCtx->CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               1, &memoryBarrier, 0,
                                0, 0, 0);
    }

    return VK_SUCCESS;
}

// create vkbuffer CPU visible
// check CNvEncoder::PrepareInputFrame function
// use convertYUVpitchtoNV12 function to convert the yuvInput[3] to the right format
// write input to srcBufferPtr (vkbuffer mapped pointer)

VkResult VkEncoderPictureBuffer::CopyToVkBuffer(VkBuffer yuvInput, VkImage image, uint32_t width, uint32_t height, VkCommandBuffer cmdBuf)
{
    const VkMpFormatInfo *formatInfo = YcbcrVkFormatInfo(m_imageFormat);

    VkBufferImageCopy region = {};
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = 0;
    region.imageOffset.y = 0;
    region.imageOffset.z = 0;
    region.imageExtent.depth = 1;

    uint32_t bufferOffset = 0;
    std::vector<VkBufferImageCopy> copyRegions;

    for (uint32_t plane = 0; plane <= formatInfo->planesLayout.numberOfExtraPlanes; plane++) {
        uint32_t w = 0;
        uint32_t h = 0;

        if ((plane > 0) && formatInfo->planesLayout.secondaryPlaneSubsampledX) { // if subsampled on X divide width by 2
            w = (width + 1) / 2; // add 1 before division in case width is an odd number
        } else {
            w = width;
        }

        if ((plane > 0) && formatInfo->planesLayout.secondaryPlaneSubsampledY) { // if subsampled on Y divide height by 2
            h = (height + 1) / 2; // add 1 before division in case height is an odd number
        } else {
            h = height;
        }

        region.bufferOffset = bufferOffset;
        region.bufferRowLength =  w;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << plane;
        region.imageExtent.width = w;
        region.imageExtent.height = h;

        copyRegions.push_back(region);

        bufferOffset += w * h; // w * h is the size of the plane
    }

    CopyToBuffer(&image, &yuvInput, VK_IMAGE_LAYOUT_GENERAL, copyRegions, &cmdBuf);

    return VK_SUCCESS;
}
