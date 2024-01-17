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

#include "NvPictureBuffer.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"

VkResult NvPictureBuffer::createVideoQueries(uint32_t numSlots, nvvk::Context* deviceInfo, const VkVideoProfileInfoKHR* pEncodeProfile)
{
    VkQueryPoolCreateInfo queryPoolCreateInfo = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryPoolCreateInfo.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_BITSTREAM_BUFFER_RANGE_KHR;
    queryPoolCreateInfo.queryCount = numSlots * 2;
    queryPoolCreateInfo.pNext = pEncodeProfile;

    return vkCreateQueryPool(deviceInfo->m_device, &queryPoolCreateInfo, NULL, &m_queryPool);
}

void NvPictureBuffer::initImageLayout(VkCommandBuffer cmdBuf, Picture* picture, VkImageLayout layout)
{
    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    nvvk::cmdBarrierImageLayout(cmdBuf, picture->m_image.image, picture->m_imageLayout,
                                layout, range);

    picture->m_imageLayout = layout;
}

void NvPictureBuffer::initReferenceFramePool(uint32_t                   numImages,
        VkFormat                   imageFormat,
        nvvk::ResourceAllocator*   rAlloc)
{
    VkImageCreateInfo tmpImgCreateInfo;

    tmpImgCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    tmpImgCreateInfo.pNext = m_videoProfile.GetProfile();
    tmpImgCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    tmpImgCreateInfo.format = imageFormat;
    tmpImgCreateInfo.extent = { m_extent.width, m_extent.height, 1 };
    tmpImgCreateInfo.mipLevels = 1;
    tmpImgCreateInfo.arrayLayers = 1;
    tmpImgCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    tmpImgCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    tmpImgCreateInfo.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR; // DPB ONLY
    tmpImgCreateInfo.sharingMode = VK_SHARING_MODE_CONCURRENT; // VK_SHARING_MODE_EXCLUSIVE here makes it not check for queueFamily
    tmpImgCreateInfo.queueFamilyIndexCount = 1;
    tmpImgCreateInfo.pQueueFamilyIndices = &m_queueFamilyIndex;
    tmpImgCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    tmpImgCreateInfo.flags = 0;

    m_dpbSize = numImages;

    for(uint32_t i = 0; i < numImages; i++) {
        nvvk::Image currentRefImage = rAlloc->createImage(tmpImgCreateInfo);
        assert(currentRefImage.image != VK_NULL_HANDLE);
        VkImageViewCreateInfo currentRefImageViewCreateInfo = nvvk::makeImage2DViewCreateInfo(currentRefImage.image, imageFormat);
        nvvk::Texture currentRefImageView = rAlloc->createTexture(currentRefImage, currentRefImageViewCreateInfo);
        assert(currentRefImageView.descriptor.imageView != VK_NULL_HANDLE);
        Picture refPic(currentRefImage, currentRefImageView, tmpImgCreateInfo.initialLayout);
        m_dpb[i] = refPic;
    }

}

void NvPictureBuffer::prepareReferenceImages(VkCommandBuffer cmdBuf)
{
    for (size_t i = 0; i < m_dpbSize; i++) {
        initImageLayout(cmdBuf, &m_dpb[i], VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR);
    }
}

void NvPictureBuffer::getReferenceFrameResourcesByIndex(int8_t dpbSlotIdx,
        VkVideoPictureResourceInfoKHR* pictureResources)
{
    Picture* refPic = &m_dpb[dpbSlotIdx];

    pictureResources->imageViewBinding = refPic->m_imageView.descriptor.imageView;
    pictureResources->codedOffset = { 0, 0 };
    pictureResources->codedExtent = m_extent;
    pictureResources->baseArrayLayer = 0;
}

int32_t NvPictureBuffer::initFramePool( nvvk::Context* ctx,
                                        const VkVideoProfileInfoKHR* pEncodeProfile,
                                        uint32_t                 numImages,
                                        VkFormat                 imageFormat,
                                        uint32_t                 maxImageWidth,
                                        uint32_t                 maxImageHeight,
                                        uint32_t                 fullImageSize,
                                        VkImageTiling            tiling,
                                        VkImageUsageFlags        usage,
                                        nvvk::ResourceAllocatorDedicated* rAlloc,
                                        nvvk::CommandPool*       cmdPoolVideoEncode,
                                        uint32_t                 queueFamilyIndex)
{
    m_pCtx = ctx;
    if (m_queryPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(m_pCtx->m_device, m_queryPool, NULL);
        m_queryPool = VK_NULL_HANDLE;
    }

    m_videoProfile.InitFromProfile(pEncodeProfile);

    if (numImages && pEncodeProfile) {
        VkResult result = createVideoQueries(numImages, m_pCtx, pEncodeProfile);
        if (result != VK_SUCCESS) {
            return 0;
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

        return initFrame(numImages,
                         m_pCtx->m_device,
                         &m_imageCreateInfo,
                         rAlloc,
                         cmdPoolVideoEncode,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         0);

    }
    else {
        deinitFramePool();
    }

    return 0;
}

void NvPictureBuffer::prepareInputImages(VkCommandBuffer cmdBuf)
{
    for (size_t i = 0; i < m_frameBufferSize; i++) {
        initImageLayout(cmdBuf, &m_encodeFrameData[i].m_picture,
                        VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR);
    }
}

void NvPictureBuffer::getFrameResourcesByIndex( int8_t encodeFrameSlotIdx,
        VkVideoPictureResourceInfoKHR* pictureResources)
{
    pictureResources->imageViewBinding = m_encodeFrameData[encodeFrameSlotIdx].m_picture.m_imageView.descriptor.imageView;
    assert(pictureResources->sType == VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR);
    pictureResources->codedOffset = { 0, 0 };
    pictureResources->codedExtent = m_extent;
    pictureResources->baseArrayLayer = 0;
}

VkQueryPool NvPictureBuffer::getQueryPool()
{
    return m_queryPool;
}

EncodeFrameData* NvPictureBuffer::getEncodeFrameData(uint32_t index)
{
    assert(index < m_frameBufferSize);
    return &m_encodeFrameData[index];
}

size_t NvPictureBuffer::size()
{
    return m_frameBufferSize;
}

void NvPictureBuffer::deinitFramePool()
{
    if (m_queryPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(m_pCtx->m_device, m_queryPool, NULL);
        m_queryPool = VK_NULL_HANDLE;
    }

    for (uint32_t ndx = 0; ndx < m_frameBufferSize; ndx++) {
        m_encodeFrameData[ndx].deinitFramePool(m_resAlloc);
    }
    m_frameBufferSize = 0;
}

void NvPictureBuffer::deinitReferenceFramePool()
{
    for (uint32_t ndx = 0; ndx < m_dpbSize; ndx++) {
        m_resAlloc->destroy(m_dpb[ndx].m_imageView);

        // Destroying the Texture also frees the Image.
        m_dpb[ndx].m_image = nvvk::Image();
    }
    m_dpbSize = 0;
}

void EncodeFrameData::deinitFramePool(nvvk::ResourceAllocatorDedicated* rAlloc)
{
    if (m_device == VK_NULL_HANDLE) {
        return;
    }

    if (m_frameCompleteFence != VK_NULL_HANDLE) {
        vkDestroyFence(m_device, m_frameCompleteFence, nullptr);
        m_frameCompleteFence = VK_NULL_HANDLE;
    }

    if (m_frameConsumerDoneFence != VK_NULL_HANDLE) {
        vkDestroyFence(m_device, m_frameConsumerDoneFence, nullptr);
        m_frameConsumerDoneFence = VK_NULL_HANDLE;
    }

    if (m_frameEncodedSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device, m_frameEncodedSemaphore, nullptr);
        m_frameEncodedSemaphore = VK_NULL_HANDLE;
    }

    if (m_frameProducerDoneSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device, m_frameProducerDoneSemaphore, nullptr);
        m_frameProducerDoneSemaphore = VK_NULL_HANDLE;
    }

    rAlloc->destroy(m_inputStagingBuffer);

    rAlloc->destroy(m_outBitstreamBuffer);

    rAlloc->destroy(m_picture.m_imageView);

    // Destroying the Texture also frees the Image.
    m_picture.m_image = nvvk::Image();
}

int32_t NvPictureBuffer::configRefPics(uint8_t distBetweenAnchors, uint8_t distanceBetweenIntras, uint32_t currentPoc, uint8_t currentEncodeFrameIdx)
{
    if(!m_encodeFrameData[currentEncodeFrameIdx].m_usedDpbMask) { // if mask is not 0 then reset it
        for(int32_t i=0; i<DECODED_PICTURE_BUFFER_SIZE; i++) {
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

void NvPictureBuffer::addRefPic(uint8_t inImageIdx, int8_t dpbIdx, uint32_t poc)
{
    uint8_t refCount = m_encodeFrameData[inImageIdx].m_refCount;
    if(refCount < DECODED_PICTURE_BUFFER_SIZE) {
        m_encodeFrameData[inImageIdx].m_RefPics[refCount].m_dpbIdx = dpbIdx;
        m_encodeFrameData[inImageIdx].m_RefPics[refCount].m_poc = poc;
        m_encodeFrameData[inImageIdx].m_refCount++;
    }
}

void NvPictureBuffer::removeRefPic(uint8_t inImageIdx)
{
    uint8_t refCount = m_encodeFrameData[inImageIdx].m_refCount;
    if(refCount > 0) {
        m_encodeFrameData[inImageIdx].m_RefPics[refCount].m_dpbIdx = -1;
        m_encodeFrameData[inImageIdx].m_RefPics[refCount].m_poc = -1;
        m_encodeFrameData[inImageIdx].m_refCount--;
    }
}

uint32_t NvPictureBuffer::initFrame(uint32_t numImages,
                                    VkDevice dev,
                                    const VkImageCreateInfo* pImageCreateInfo,
                                    nvvk::ResourceAllocatorDedicated* rAlloc,
                                    nvvk::CommandPool* cmdPoolVideoEncode,
                                    VkMemoryPropertyFlags requiredMemProps,
                                    int32_t initWithPattern,
                                    VkExternalMemoryHandleTypeFlagBitsKHR exportMemHandleTypes)
{
    m_frameBufferSize = numImages;

    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFenceCreateInfo fenceFrameCompleteInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    // The fence waited on for the first frame should be signaled.
    fenceFrameCompleteInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

    m_resAlloc = rAlloc;

    for (uint8_t imageIndex = 0; imageIndex < numImages; imageIndex++) {
        m_encodeFrameData[imageIndex].m_device = dev;
        m_encodeFrameData[imageIndex].m_extent = m_extent;
        m_encodeFrameData[imageIndex].m_queueFamilyIndex = m_queueFamilyIndex;
        m_encodeFrameData[imageIndex].m_videoProfile = m_videoProfile;
        // m_encodeFrameData[imageIndex].m_resAlloc = rAlloc;
        m_encodeFrameData[imageIndex].m_picture.m_image = rAlloc->createImage(*pImageCreateInfo);
        VkImageViewCreateInfo infoEncodedImageView = nvvk::makeImage2DViewCreateInfo(m_encodeFrameData[imageIndex].m_picture.m_image.image,
                pImageCreateInfo->format);
        m_encodeFrameData[imageIndex].m_picture.m_imageView = rAlloc->createTexture(m_encodeFrameData[imageIndex].m_picture.m_image,
                infoEncodedImageView);
        VkResult result = vkCreateFence(dev, &fenceFrameCompleteInfo, nullptr, &m_encodeFrameData[imageIndex].m_frameCompleteFence);
        result = vkCreateFence(dev, &fenceInfo, nullptr, &m_encodeFrameData[imageIndex].m_frameConsumerDoneFence);
        assert(result == VK_SUCCESS);
        result = vkCreateSemaphore(dev, &semInfo, nullptr, &m_encodeFrameData[imageIndex].m_frameEncodedSemaphore);
        assert(result == VK_SUCCESS);
        result = vkCreateSemaphore(dev, &semInfo, nullptr, &m_encodeFrameData[imageIndex].m_frameProducerDoneSemaphore);
        assert(result == VK_SUCCESS);

        VkBufferCreateInfo outBitstreamCreateInfo = nvvk::makeBufferCreateInfo(m_maxBitstreamSize, VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR);
        m_encodeFrameData[imageIndex].m_outBitstreamBuffer = rAlloc->createBuffer(outBitstreamCreateInfo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); //FLAGS - map buffer to host

        VkBufferCreateInfo stagingBufferCreateInfo = nvvk::makeBufferCreateInfo(m_fullImageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        m_encodeFrameData[imageIndex].m_inputStagingBuffer = rAlloc->createBuffer(stagingBufferCreateInfo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        m_encodeFrameData[imageIndex].m_cmdBufVideoEncode = cmdPoolVideoEncode->createCommandBuffer();
    }

    return (uint32_t)m_frameBufferSize;
}

VkResult NvPictureBuffer::copyToVkImage(uint32_t index, uint32_t bufferOffset, VkCommandBuffer cmdBuf)
{
    EncodeFrameData* currentEncodeFrameData = &m_encodeFrameData[index];
    Picture* picture = &currentEncodeFrameData->m_picture;

    VkImage inputImage = picture->m_image.image;
    VkBuffer inputStaging = currentEncodeFrameData->m_inputStagingBuffer.buffer;

    uint32_t width = m_imageCreateInfo.extent.width;
    uint32_t height = m_imageCreateInfo.extent.height;

    const VkMpFormatInfo *formatInfo = YcbcrVkFormatInfo(m_imageFormat);

    // This is to be used for the image memory barriers, if they are needed.
    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    VkBufferImageCopy region = {};
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = 0;
    region.imageOffset.y = 0;
    region.imageOffset.z = 0;
    region.imageExtent.depth = 1;

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

    if (picture->m_imageLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        // Transition the layout to the desired one.
        nvvk::cmdBarrierImageLayout(cmdBuf, inputImage, picture->m_imageLayout,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, range);
    }

    vkCmdCopyBufferToImage(cmdBuf, inputStaging, inputImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)copyRegions.size(), copyRegions.data());

    if (picture->m_imageLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        // Restore the original image layout
        nvvk::cmdBarrierImageLayout(cmdBuf, inputImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    picture->m_imageLayout, range);
    }


    return VK_SUCCESS;
}

VkResult NvPictureBuffer::copyToBuffer(VkImage* image, VkBuffer* buffer, VkImageLayout layout,
                                       std::vector<VkBufferImageCopy> &copyRegions, VkCommandBuffer* cmdBuf)
{
    VkResult result = VK_SUCCESS;

    VkCommandBufferBeginInfo cmd_buf_info = VkCommandBufferBeginInfo();
    cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmd_buf_info.pNext = NULL;
    cmd_buf_info.flags = 0;
    cmd_buf_info.pInheritanceInfo = NULL;
    vkBeginCommandBuffer(*cmdBuf, &cmd_buf_info);

    vkCmdCopyImageToBuffer(*cmdBuf, *image, layout, *buffer, (uint32_t)copyRegions.size(), copyRegions.data());

    vkEndCommandBuffer(*cmdBuf);

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
    vkGetDeviceQueue(m_pCtx->m_device, 0 /* graphics queue family */, 0, &graphicsQueue);

    result = vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to submit clear command\n");
        return result;
    }

    return VK_SUCCESS;
}

// create vkbuffer CPU visible
// check CNvEncoder::PrepareInputFrame function
// use convertYUVpitchtoNV12 function to convert the yuvInput[3] to the right format
// write input to srcBufferPtr (vkbuffer mapped pointer)

VkResult NvPictureBuffer::copyToVkBuffer(VkBuffer yuvInput, VkImage image, uint32_t width, uint32_t height, VkCommandBuffer cmdBuf)
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

    copyToBuffer(&image, &yuvInput, VK_IMAGE_LAYOUT_GENERAL, copyRegions, &cmdBuf);

    return VK_SUCCESS;
}
