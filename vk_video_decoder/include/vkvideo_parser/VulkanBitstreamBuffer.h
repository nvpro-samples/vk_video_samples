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

#ifndef _PARSER_VULKANBITSTREAMBUFFER_H_
#define _PARSER_VULKANBITSTREAMBUFFER_H_

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include "VkVideoCore/VkVideoRefCountBase.h"
#include <vulkan_interfaces.h>

class VulkanBitstreamBuffer : public VkVideoRefCountBase
{

public:
    virtual size_t GetMaxSize() const = 0;
    virtual size_t GetOffsetAlignment() const = 0;
    virtual size_t GetSizeAlignment() const = 0;
    virtual size_t Resize(size_t newSize, size_t copySize = 0, size_t copyOffset = 0) = 0;

    virtual int64_t  MemsetData(uint32_t value, size_t offset, size_t size) = 0;
    virtual int64_t  CopyDataToBuffer(uint8_t *dstBuffer, size_t dstOffset,
                                      size_t srcOffset, size_t size) const = 0;
    virtual int64_t  CopyDataToBuffer(VkSharedBaseObj<VulkanBitstreamBuffer>& dstBuffer, size_t dstOffset,
                                      size_t srcOffset, size_t size) const = 0;
    virtual int64_t  CopyDataFromBuffer(const uint8_t *sourceBuffer, size_t srcOffset,
                                        size_t dstOffset, size_t size) = 0;
    virtual int64_t  CopyDataFromBuffer(const VkSharedBaseObj<VulkanBitstreamBuffer>& sourceBuffer, size_t srcOffset,
                                        size_t dstOffset, size_t size) = 0;
    virtual uint8_t* GetDataPtr(size_t offset, size_t &maxSize) = 0;
    virtual const uint8_t* GetReadOnlyDataPtr(size_t offset, size_t &maxSize) const = 0;

    virtual void FlushRange(size_t offset, size_t size)  const = 0;
    virtual void InvalidateRange(size_t offset, size_t size) const = 0;
    virtual VkBuffer GetBuffer() const = 0;
    virtual VkDeviceMemory GetDeviceMemory() const = 0;

    virtual uint32_t  AddStreamMarker(uint32_t streamOffset) = 0;
    virtual uint32_t  SetStreamMarker(uint32_t streamOffset, uint32_t index) = 0;
    virtual uint32_t  GetStreamMarker(uint32_t index) const = 0;
    virtual uint32_t  GetStreamMarkersCount() const = 0;
    virtual const uint32_t* GetStreamMarkersPtr(uint32_t startIndex, uint32_t& maxCount) const = 0;
    virtual uint32_t  ResetStreamMarkers() = 0;

protected:

    VulkanBitstreamBuffer() {}
    virtual ~VulkanBitstreamBuffer() {}
};

class VulkanBitstreamBufferStream {
public:

    VulkanBitstreamBufferStream ()
        : m_bitstreamBuffer()
        , m_pData()
        , m_maxSize()
        , m_maxAccessLocation()
        , m_numSlices()
    {}

    ~VulkanBitstreamBufferStream()
    {
        CommitBuffer();
        m_bitstreamBuffer = nullptr;
    }

    size_t CommitBuffer(size_t size = 0)
    {
        size_t commitSize = (size != 0) ? size : m_maxAccessLocation;
        if (commitSize && m_bitstreamBuffer) {
            m_bitstreamBuffer->FlushRange(0, commitSize);
            m_maxAccessLocation = 0;
        }
        return commitSize;
    }

    size_t SetBitstreamBuffer (VkSharedBaseObj<VulkanBitstreamBuffer>& bitstreamBuffer, bool resetStreamMarkers = true)
    {
        CommitBuffer();

        m_bitstreamBuffer = bitstreamBuffer;
        m_maxAccessLocation = 0;

        m_pData = m_bitstreamBuffer->GetDataPtr(0, m_maxSize);
        assert(m_pData);
        assert(m_maxSize);

        if (resetStreamMarkers) {
            ResetStreamMarkers();
        } else {
            m_numSlices = m_bitstreamBuffer->GetStreamMarkersCount();
        }

        return m_maxSize;

    }

    void ResetBitstreamBuffer()
    {
        CommitBuffer();
        m_bitstreamBuffer = nullptr;
        m_maxAccessLocation = 0;
        m_pData = nullptr;
    }

    size_t ResizeBitstreamBuffer(size_t newSize, size_t copySize = 0, size_t copyOffset = 0)
    {
        CommitBuffer();

        m_maxAccessLocation = 0;

        size_t retSize = m_bitstreamBuffer->Resize(newSize, copySize, copyOffset);
        if (!(retSize >= newSize)) {
            assert(!"Could not resize the bitstream buffer!");
            return retSize;
        }

        m_pData = m_bitstreamBuffer->GetDataPtr(0, m_maxSize);
        assert(m_pData);
        assert(m_maxSize);

        ResetStreamMarkers();

        return m_maxSize;
    }

    uint8_t& operator [](size_t indx) {     // write
        assert(m_pData);
        assert(indx < m_maxSize);
        m_maxAccessLocation = std::max(m_maxAccessLocation, indx);
        return m_pData[indx];
    }

    uint8_t operator [](size_t indx) const {  //read
        assert(m_pData);
        assert(indx < m_maxSize);
        // m_maxAccess = std::max(m_maxAccess, indx);
        return m_pData[indx];
    }

    operator bool() const
    {
        return (m_pData != nullptr) && (m_maxSize != 0) && !!m_bitstreamBuffer;
    }

    VkSharedBaseObj<VulkanBitstreamBuffer>&  GetBitstreamBuffer()
    {
        return m_bitstreamBuffer;
    }

    // startcode found at given slice offset
    bool HasSliceStartCodeAtOffset(size_t indx) const {
        assert(m_pData);
        assert(indx < m_maxSize);

        return ((m_pData[indx + 0] == 0x00) &&
                (m_pData[indx + 1] == 0x00) &&
                (m_pData[indx + 2] == 0x01));
    }

    size_t SetSliceStartCodeAtOffset(size_t indx) {
        assert(m_pData);
        assert(indx < m_maxSize);
        m_pData[indx + 0] = 0x00;
        m_pData[indx + 1] = 0x00;
        m_pData[indx + 2] = 0x01;
        return 3;
    }

    uint8_t* GetBitstreamPtr() {
        assert(m_pData);
        return m_pData;
    }

    size_t GetMaxSize() {
        return m_maxSize;
    }

    uint32_t GetStreamMarkersCount()
    {
        assert(m_bitstreamBuffer->GetStreamMarkersCount() == m_numSlices);
        return m_bitstreamBuffer->GetStreamMarkersCount();
    }

    uint32_t AddStreamMarker(uint32_t streamOffset)
    {
        m_numSlices++;
        return m_bitstreamBuffer->AddStreamMarker(streamOffset);
    }

    uint32_t ResetStreamMarkers()
    {
        m_numSlices = 0;
        return m_bitstreamBuffer->ResetStreamMarkers();
    }

private:
    VkSharedBaseObj<VulkanBitstreamBuffer>  m_bitstreamBuffer;
    uint8_t*                                m_pData;
    size_t                                  m_maxSize;
    size_t                                  m_maxAccessLocation;
    uint32_t                                m_numSlices;
};


#endif /* _PARSER_VULKANBITSTREAMBUFFER_H_ */
