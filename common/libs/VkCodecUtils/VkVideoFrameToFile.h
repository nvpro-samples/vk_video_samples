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

#ifndef _VKCODECUTILS_VKVIDEOFRAMETOFILE_H_
#define _VKCODECUTILS_VKVIDEOFRAMETOFILE_H_

class VkVideoFrameToFile {

public:

    VkVideoFrameToFile()
        : m_outputFile(),
          m_pLinearMemory()
        , m_allocationSize() {}

    ~VkVideoFrameToFile()
    {
        if (m_pLinearMemory) {
            delete[] m_pLinearMemory;
            m_pLinearMemory = nullptr;
        }

        if (m_outputFile) {
            fclose(m_outputFile);
            m_outputFile = nullptr;
        }
    }

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

            // Allocate the memory that will be dumped to file directly.
            m_allocationSize = (size_t)(imageMemorySize);
            m_pLinearMemory = new uint8_t[m_allocationSize];
            if (m_pLinearMemory == nullptr) {
                return nullptr;
            }
            assert(m_pLinearMemory != nullptr);
        }
        return m_pLinearMemory;
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

    bool IsFileStreamValid() const
    {
        return m_outputFile != nullptr;
    }

    operator bool() const {
        return IsFileStreamValid();
    }

    size_t WriteDataToFile(size_t offset, size_t size)
    {
        return fwrite(m_pLinearMemory + offset, size, 1, m_outputFile);
    }

    size_t GetMaxFrameSize() {
        return m_allocationSize;
    }

private:
    FILE*    m_outputFile;
    uint8_t* m_pLinearMemory;
    size_t   m_allocationSize;
};



#endif /* _VKCODECUTILS_VKVIDEOFRAMETOFILE_H_ */
