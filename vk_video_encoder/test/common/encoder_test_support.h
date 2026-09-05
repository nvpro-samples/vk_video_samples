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

#ifndef _ENCODER_TEST_SUPPORT_H_
#define _ENCODER_TEST_SUPPORT_H_

// What every test driving the encoder interface needs, and nothing about any
// one test.
//
// THREE VERDICTS, NOT TWO. A check that could not be made is not a check that
// passed. Skip() says the machine could not answer -- no device, no importable
// format -- and is counted separately, so a run on a machine that can measure
// nothing reports that rather than a clean sheet.

#include "vulkan_video_encoder.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace enctest {

//=============================================================================
// REPORTING
//=============================================================================

class Report {
public:
    void Check(bool ok, const char* what)
    {
        ++m_checks;
        if (ok) {
            fprintf(stderr, "  ok    %s\n", what);
        } else {
            ++m_failures;
            fprintf(stderr, "  FAIL  %s\n", what);
        }
    }

    void Skip(const char* what, const char* why)
    {
        ++m_skipped;
        fprintf(stderr, "  SKIP  %s (%s)\n", what, why);
    }

    int Summarise(const char* name) const
    {
        fprintf(stderr, "\n%s: %u checks, %u failures, %u skipped\n",
                name, m_checks, m_failures, m_skipped);
        if (m_failures != 0) {
            return 1;
        }
        // A RUN THAT MEASURED NOTHING IS NOT A PASS, and the exit code has to
        // carry that or the suite goes green without asserting anything.
        //
        // Zero checks with a skip recorded is a SKIP and reports itself as one
        // (ctest SKIP_RETURN_CODE 77, set on the test). Zero checks with no
        // skip recorded is a harness that ran nothing and did not say why,
        // which is a failure: it is indistinguishable at the exit code from a
        // suite whose every assertion was deleted.
        if (m_checks == 0) {
            return (m_skipped != 0) ? 77 : 1;
        }
        return 0;
    }

    unsigned failures() const { return m_failures; }
    unsigned skipped() const { return m_skipped; }

private:
    unsigned m_checks = 0;
    unsigned m_failures = 0;
    unsigned m_skipped = 0;
};

//=============================================================================
// DEVICE ENTRY POINTS
//
// Loaded through the loader the session was built with, so a test reaches the
// same driver the encoder does rather than whichever one is first on the path.
//=============================================================================

struct DeviceFns {
    PFN_vkGetDeviceProcAddr                 GetDeviceProcAddr = nullptr;
    PFN_vkCreateImage                       CreateImage = nullptr;
    PFN_vkDestroyImage                      DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements        GetImageMemoryRequirements = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkAllocateMemory                    AllocateMemory = nullptr;
    PFN_vkFreeMemory                        FreeMemory = nullptr;
    PFN_vkBindImageMemory                   BindImageMemory = nullptr;
    PFN_vkMapMemory                         MapMemory = nullptr;
    PFN_vkUnmapMemory                       UnmapMemory = nullptr;
    PFN_vkGetImageSubresourceLayout         GetImageSubresourceLayout = nullptr;
    PFN_vkDeviceWaitIdle                    DeviceWaitIdle = nullptr;

    bool Load(const vk::video::enc::Ref<vk::video::enc::IDeviceBinding>& binding)
    {
        if (!binding) {
            return false;
        }
        PFN_vkGetInstanceProcAddr gipa = binding->GetInstanceProcAddr();
        const VkInstance instance = binding->Instance();
        const VkDevice device = binding->Device();
        if (gipa == nullptr || instance == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
            return false;
        }
        GetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            gipa(instance, "vkGetDeviceProcAddr"));
        GetPhysicalDeviceMemoryProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
                gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
        if (GetDeviceProcAddr == nullptr || GetPhysicalDeviceMemoryProperties == nullptr) {
            return false;
        }
#define V1_LOAD(name) \
    name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name))
        V1_LOAD(CreateImage);
        V1_LOAD(DestroyImage);
        V1_LOAD(GetImageMemoryRequirements);
        V1_LOAD(AllocateMemory);
        V1_LOAD(FreeMemory);
        V1_LOAD(BindImageMemory);
        V1_LOAD(MapMemory);
        V1_LOAD(UnmapMemory);
        V1_LOAD(GetImageSubresourceLayout);
        V1_LOAD(DeviceWaitIdle);
#undef V1_LOAD
        return CreateImage && DestroyImage && GetImageMemoryRequirements &&
               AllocateMemory && FreeMemory && BindImageMemory && MapMemory &&
               UnmapMemory && GetImageSubresourceLayout && DeviceWaitIdle;
    }
};

//=============================================================================
// AN INPUT IMAGE THE HOST CAN WRITE
//
// LINEAR and host-visible, so the pattern is written by mapping it. That is
// not a shortcut: a session created without an adopted device exposes no queue
// family, so there is no queue on which a staging copy could be submitted.
// Mapping needs none.
//=============================================================================

class HostImage {
public:
    HostImage(const DeviceFns& fns, VkPhysicalDevice phys, VkDevice device,
              uint32_t width, uint32_t height)
        : m_fns(fns), m_phys(phys), m_device(device),
          m_width(width), m_height(height) { }

    ~HostImage() { Destroy(); }

    HostImage(const HostImage&) = delete;
    HostImage& operator=(const HostImage&) = delete;

    bool Create(const char** why)
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        ci.extent        = {m_width, m_height, 1};
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_LINEAR;
        ci.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        // Written from the host before anything else touches it, so its
        // contents must survive the first transition.
        ci.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

        if (m_fns.CreateImage(m_device, &ci, nullptr, &m_image) != VK_SUCCESS) {
            *why = "vkCreateImage refused a linear NV12 image";
            return false;
        }

        VkMemoryRequirements req{};
        m_fns.GetImageMemoryRequirements(m_device, m_image, &req);
        VkPhysicalDeviceMemoryProperties props{};
        m_fns.GetPhysicalDeviceMemoryProperties(m_phys, &props);

        const VkMemoryPropertyFlags want =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t typeIndex = UINT32_MAX;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if (((req.memoryTypeBits & (1u << i)) != 0) &&
                ((props.memoryTypes[i].propertyFlags & want) == want)) {
                typeIndex = i;
                break;
            }
        }
        if (typeIndex == UINT32_MAX) {
            *why = "no host-visible memory type accepts a linear NV12 image";
            return false;
        }

        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = typeIndex;
        if (m_fns.AllocateMemory(m_device, &ai, nullptr, &m_memory) != VK_SUCCESS) {
            *why = "vkAllocateMemory failed";
            return false;
        }
        if (m_fns.BindImageMemory(m_device, m_image, m_memory, 0) != VK_SUCCESS) {
            *why = "vkBindImageMemory failed";
            return false;
        }
        m_size = req.size;
        return true;
    }

    // A moving pattern, so successive frames differ and the encoder has
    // something to predict. A flat image lets a broken submission path still
    // produce plausible output.
    bool WritePattern(uint32_t frameIndex, const char** why)
    {
        VkImageSubresource luma{VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0};
        VkImageSubresource chroma{VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0};
        VkSubresourceLayout lumaLayout{}, chromaLayout{};
        m_fns.GetImageSubresourceLayout(m_device, m_image, &luma, &lumaLayout);
        m_fns.GetImageSubresourceLayout(m_device, m_image, &chroma, &chromaLayout);

        void* mapped = nullptr;
        if (m_fns.MapMemory(m_device, m_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            *why = "vkMapMemory failed";
            return false;
        }
        uint8_t* base = static_cast<uint8_t*>(mapped);
        const uint32_t bar = (frameIndex * 24) % m_width;
        for (uint32_t y = 0; y < m_height; ++y) {
            uint8_t* row = base + lumaLayout.offset + y * lumaLayout.rowPitch;
            for (uint32_t x = 0; x < m_width; ++x) {
                const bool inBar = (x >= bar) && (x < bar + 24);
                row[x] = inBar ? 235 : static_cast<uint8_t>(16 + (x * 200) / m_width);
            }
        }
        for (uint32_t y = 0; y < m_height / 2; ++y) {
            uint8_t* row = base + chromaLayout.offset + y * chromaLayout.rowPitch;
            for (uint32_t x = 0; x < m_width / 2; ++x) {
                row[2 * x]     = 128;
                row[2 * x + 1] = 128;
            }
        }
        m_fns.UnmapMemory(m_device, m_memory);
        return true;
    }

    void Destroy()
    {
        if (m_image != VK_NULL_HANDLE) {
            m_fns.DestroyImage(m_device, m_image, nullptr);
            m_image = VK_NULL_HANDLE;
        }
        if (m_memory != VK_NULL_HANDLE) {
            m_fns.FreeMemory(m_device, m_memory, nullptr);
            m_memory = VK_NULL_HANDLE;
        }
    }

    // How this image is described to the interface. The usage and creation
    // flags must match vkCreateImage exactly: the library cannot read them
    // back from a VkImage and uses them to decide how the frame reaches the
    // encoder.
    vk::video::enc::ExternalImage Describe() const
    {
        vk::video::enc::ExternalImage image;
        image.handleType    = vk::video::enc::ExternalHandleType::VkImageHandle;
        image.existingImage = m_image;
        image.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        image.width         = m_width;
        image.height        = m_height;
        image.tiling        = VK_IMAGE_TILING_LINEAR;
        image.layout        = VK_IMAGE_LAYOUT_PREINITIALIZED;
        image.colorModel    = vk::video::enc::ColorModel::YCbCr;
        image.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image.createFlags   = 0;
        image.residency     = vk::video::enc::Residency::Local;
        image.allocationSize = m_size;
        return image;
    }

    VkImage Image() const { return m_image; }

private:
    const DeviceFns& m_fns;
    VkPhysicalDevice m_phys;
    VkDevice         m_device;
    uint32_t         m_width;
    uint32_t         m_height;
    VkImage          m_image  = VK_NULL_HANDLE;
    VkDeviceMemory   m_memory = VK_NULL_HANDLE;
    uint64_t         m_size   = 0;
};

//=============================================================================
// A CONFIGURED SESSION
//
// The shape every test here starts from: a platform on the default device, a
// rate-controlled H.264 configuration at the requested size, and the roles.
// Returns false when the machine cannot answer, with |why| set -- which the
// caller reports as a skip rather than a failure.
//=============================================================================

struct Session {
    vk::video::enc::Ref<vk::video::enc::IEncoderPlatform>   platform;
    vk::video::enc::Ref<vk::video::enc::IEncoderConfig>     config;
    vk::video::enc::Ref<vk::video::enc::IEncoderSession>    session;
    vk::video::enc::Ref<vk::video::enc::IFrameSubmitter>    submitter;
    vk::video::enc::Ref<vk::video::enc::IBitstreamSource>   bitstream;
    vk::video::enc::Ref<vk::video::enc::ICompletionSignal>  completion;
    vk::video::enc::Ref<vk::video::enc::IResourceRegistry>  registry;
    vk::video::enc::Ref<vk::video::enc::IDeviceBinding>     binding;
    vk::video::enc::Ref<vk::video::enc::IDiagnostics>       diagnostics;

    bool Open(uint32_t width, uint32_t height, const char** why)
    {
        using namespace vk::video::enc;

        PlatformCreateInfo info;
        info.silenceStdio = true;
        if (VkEncCreatePlatform(info, platform) != VK_SUCCESS || !platform) {
            *why = "no Vulkan encode platform";
            return false;
        }
        if (platform->Caps()->Codecs().empty()) {
            *why = "no encode-capable device";
            return false;
        }

        Expected<Ref<IEncoderConfig>> made =
            platform->CreateConfig(Codec::H264, Profile::H264Main);
        if (!made) {
            *why = made.status().detail();
            return false;
        }
        config = *made;

        RateControl rate;
        rate.mode           = RateControlMode::Cbr;
        rate.averageBitrate = 2 * 1000 * 1000;
        rate.maxBitrate     = 2 * 1000 * 1000;
        config->SetCodedExtent(width, height)
               .SetFrameRate(30, 1)
               .SetInputFormat(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, ColorModel::YCbCr)
               .SetRateControl(rate);

        Expected<Ref<IEncoderSession>> opened = platform->CreateSession(config);
        if (!opened) {
            *why = opened.status().detail();
            return false;
        }
        session     = *opened;
        submitter   = Query<IFrameSubmitter>(session);
        bitstream   = Query<IBitstreamSource>(session);
        completion  = Query<ICompletionSignal>(session);
        registry    = Query<IResourceRegistry>(session);
        binding     = Query<IDeviceBinding>(session);
        diagnostics = Query<IDiagnostics>(session);
        if (!submitter || !bitstream || !registry || !binding) {
            *why = "the session does not offer the roles a test needs";
            return false;
        }
        return true;
    }

    // Take everything ready, returning how many frames were retired.
    uint32_t DrainRetrievable()
    {
        uint32_t retired = 0;
        for (;;) {
            vk::video::enc::Expected<vk::video::enc::EncodedFrame> got =
                bitstream->AcquireNext();
            if (!got) {
                break;
            }
            bitstream->Release(got->frameId);
            ++retired;
        }
        return retired;
    }
};

}  // namespace enctest

#endif /* _ENCODER_TEST_SUPPORT_H_ */
