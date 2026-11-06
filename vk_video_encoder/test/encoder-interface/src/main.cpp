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

// The encoder interface, driven the way a host drives it.
//
// The checks that need no device run unconditionally and are the ones that
// gate a build. The checks that need a real encode device are skipped, loudly,
// when there is none -- a suite that silently selects nothing must not pass.

#include "vulkan_video_encoder.h"
#include "encode_device_probe.h"

#include <cstdio>
#include <cstring>

using namespace vk::video::enc;

namespace {

int g_failures = 0;
int g_checks   = 0;
int g_skipped  = 0;

void Check(bool condition, const char* what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        fprintf(stderr, "  FAIL  %s\n", what);
    } else {
        fprintf(stderr, "  ok    %s\n", what);
    }
}

void Skip(const char* what, const char* why)
{
    ++g_skipped;
    fprintf(stderr, "  SKIP  %s (%s)\n", what, why);
}

int Summarise()
{
    fprintf(stderr, "\n%d checks, %d failures, %d skipped\n",
            g_checks, g_failures, g_skipped);
    return g_failures == 0 ? 0 : 1;
}

// The device-backed checks cannot run because the library has no device.
// Whose doing that is, is the probe's answer, not the library's: with no
// device that encodes H.264 the run is a SKIP (77) -- not a pass, since those
// checks never ran -- and with one it is the library's failure.
int NoDevice(const char* what, const char* why)
{
    std::string probe;
    const bool device =
        enctest::ProbeEncodeDevice(VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, &probe);
    const std::string detail = std::string(why) + "; probe: " + probe;
    if (device || enctest::RequireGpu()) {
        Check(false, detail.c_str());
    } else {
        Skip(what, detail.c_str());
    }
    return (Summarise() == 0) ? enctest::kExitSkip : 1;
}

//=============================================================================
// Checks that need no device
//=============================================================================

// Result must stay cheap: it is returned once per frame.
void TestResultIsCheap()
{
    Check(sizeof(Result) <= 2 * sizeof(void*), "Result fits in two words");

    Result ok;
    Check(ok.ok() && static_cast<bool>(ok), "a default Result is success");
    Check(ok.code() == ResultCode::Ok, "a default Result carries Ok");

    Result bad(ResultCode::InvalidArgument, "resolution unset");
    Check(!bad.ok() && !static_cast<bool>(bad), "an error Result is falsey");
    Check(bad == ResultCode::InvalidArgument, "the code compares directly");
    Check(strcmp(bad.detail(), "resolution unset") == 0, "the detail survives");
}

void TestExpectedCarriesEither()
{
    Expected<int> value(42);
    Check(static_cast<bool>(value) && *value == 42, "Expected carries a value");

    Expected<int> error(Result(ResultCode::NotReady, "not yet"));
    Check(!error, "Expected carries a failure");
    Check(error.status().code() == ResultCode::NotReady, "the failure keeps its code");
    Check(strcmp(error.status().detail(), "not yet") == 0, "the failure keeps its detail");
}

void TestArrayView()
{
    const uint32_t backing[4] = {10, 20, 30, 40};
    ArrayView<uint32_t> view(backing, 4);

    Check(view.size() == 4 && !view.empty(), "a view reports its size");
    Check(view[2] == 30, "a view indexes");

    uint32_t sum = 0;
    for (uint32_t v : view) {
        sum += v;
    }
    Check(sum == 100, "a view iterates");
    Check(ArrayView<uint32_t>().empty(), "a default view is empty");
}

// Format classification, which needs no device.
//
// These expectations are the tables a host would otherwise carry as its own
// format->bool predicates. Pinning them here is what lets a host delete that
// duplicate set and route the question to VkEncClassifyFormat instead: the
// answers stay asserted somewhere that can fail.
void TestFormatClassification()
{
    struct Row {
        VkFormat     format;
        ColorModel   model;
        bool         supported;
        InputPath    path;
        FilterAccess access;
        const char*  what;
    };
    static const Row kRows[] = {
        // Semi-planar 8-bit is what the encoder reads: no filter.
        {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, ColorModel::YCbCr, true,
         InputPath::Copy, FilterAccess::NoFilter, "NV12 is taken directly"},

        // Formats the encoder reaches only through the compute filter are
        // refused, as Validate refuses them: a submitted frame is read
        // directly or copied.
        {VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM, ColorModel::YCbCr, false,
         InputPath::Copy, FilterAccess::NoFilter, "I420 is refused"},
        {VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16, ColorModel::YCbCr,
         false, InputPath::Copy, FilterAccess::NoFilter, "I420 10-bit is refused"},
        {VK_FORMAT_R8G8B8A8_UNORM, ColorModel::Rgb, false,
         InputPath::Copy, FilterAccess::NoFilter, "RGBA is refused"},
        {VK_FORMAT_B8G8R8A8_UNORM, ColorModel::Rgb, false,
         InputPath::Copy, FilterAccess::NoFilter, "BGRA is refused"},
        {VK_FORMAT_A8B8G8R8_UNORM_PACK32, ColorModel::Rgb, false,
         InputPath::Copy, FilterAccess::NoFilter, "packed RGBA is refused"},

        // Not a picture format on any route.
        {VK_FORMAT_D32_SFLOAT, ColorModel::FromFormat, false,
         InputPath::Copy, FilterAccess::NoFilter, "a depth format is refused"},
    };

    for (const Row& row : kRows) {
        FormatRouting routing;
        const VkResult result = VkEncClassifyFormat(row.format, row.model, &routing);

        if (!row.supported) {
            Check(result != VK_SUCCESS && !routing.supported, row.what);
            continue;
        }
        const bool ok = (result == VK_SUCCESS) && routing.supported &&
                        (routing.path == row.path) &&
                        (routing.filterAccess == row.access);
        Check(ok, row.what);
        if (!ok) {
            fprintf(stderr, "        format=0x%x path=%u access=%u supported=%d\n",
                    static_cast<unsigned>(row.format),
                    static_cast<unsigned>(routing.path),
                    static_cast<unsigned>(routing.filterAccess),
                    static_cast<int>(routing.supported));
        }
    }

    // A null out-parameter is a caller bug, refused rather than dereferenced.
    Check(VkEncClassifyFormat(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
                              ColorModel::YCbCr, nullptr) != VK_SUCCESS,
          "classification refuses a null out-parameter");
}

//=============================================================================
// A real input frame
//
// The image is LINEAR and host-visible, and the pattern is written by mapping
// it. That is not a shortcut: the library owns the device here, and it exposes
// no queue family, so there is no queue on which a staging copy could be
// submitted. Mapping needs none.
//
// A linear image is not directly encodable, so this exercises the path a
// caller most often takes anyway -- the library stages it into an encodable
// image -- and proves the frame reaches the encoder.
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

    bool Load(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkDevice device)
    {
        if (gipa == nullptr || instance == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
            return false;
        }
        GetDeviceProcAddr =
            reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
        GetPhysicalDeviceMemoryProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
                gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
        if (GetDeviceProcAddr == nullptr || GetPhysicalDeviceMemoryProperties == nullptr) {
            return false;
        }
    #define V3_LOAD(name) \
        name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name))
        V3_LOAD(CreateImage);
        V3_LOAD(DestroyImage);
        V3_LOAD(GetImageMemoryRequirements);
        V3_LOAD(AllocateMemory);
        V3_LOAD(FreeMemory);
        V3_LOAD(BindImageMemory);
        V3_LOAD(MapMemory);
        V3_LOAD(UnmapMemory);
        V3_LOAD(GetImageSubresourceLayout);
        V3_LOAD(DeviceWaitIdle);
    #undef V3_LOAD
        return CreateImage && DestroyImage && GetImageMemoryRequirements &&
               AllocateMemory && FreeMemory && BindImageMemory && MapMemory &&
               UnmapMemory && GetImageSubresourceLayout && DeviceWaitIdle;
    }
};

const uint32_t kWidth  = 320;
const uint32_t kHeight = 240;

class HostImage {
public:
    HostImage(const DeviceFns& fns, VkPhysicalDevice phys, VkDevice device)
        : m_fns(fns), m_phys(phys), m_device(device) { }

    ~HostImage() { Destroy(); }

    HostImage(const HostImage&) = delete;
    HostImage& operator=(const HostImage&) = delete;

    bool Create(const char** why)
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        ci.extent        = {kWidth, kHeight, 1};
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
            *why = "vkCreateImage failed for a linear NV12 image";
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

    // A moving pattern, so successive frames are not identical and the encoder
    // has something to predict. A flat image would let a broken submit path
    // still produce plausible-looking output.
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

        const uint32_t bar = (frameIndex * 24) % kWidth;
        for (uint32_t y = 0; y < kHeight; ++y) {
            uint8_t* row = base + lumaLayout.offset + y * lumaLayout.rowPitch;
            for (uint32_t x = 0; x < kWidth; ++x) {
                const bool inBar = (x >= bar) && (x < bar + 24);
                row[x] = inBar ? 235 : static_cast<uint8_t>(16 + (x * 200) / kWidth);
            }
        }
        for (uint32_t y = 0; y < kHeight / 2; ++y) {
            uint8_t* row = base + chromaLayout.offset + y * chromaLayout.rowPitch;
            for (uint32_t x = 0; x < kWidth / 2; ++x) {
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

    // THE LAYOUT A FRAME DECLARES IS THE ONE THE IMAGE IS IN. The image is
    // created PREINITIALIZED, so its first frame declares that. The encoder
    // hands a staged image back in the layout the frame declared, or in
    // GENERAL where that layout is not one a barrier may transition to --
    // PREINITIALIZED is not -- so once a frame has been accepted the image is
    // in GENERAL, and every later frame declares GENERAL.
    void MarkSubmitted() { m_layout = VK_IMAGE_LAYOUT_GENERAL; }

    VkImage       Image()  const { return m_image; }
    uint64_t      Size()   const { return m_size; }
    VkImageLayout Layout() const { return m_layout; }

private:
    const DeviceFns& m_fns;
    VkPhysicalDevice m_phys;
    VkDevice         m_device;
    VkImage          m_image  = VK_NULL_HANDLE;
    VkDeviceMemory   m_memory = VK_NULL_HANDLE;
    uint64_t         m_size   = 0;
    VkImageLayout    m_layout = VK_IMAGE_LAYOUT_PREINITIALIZED;
};

// Describe a created image to the interface.
ExternalImage DescribeImage(const HostImage& image)
{
    ExternalImage described;
    described.handleType    = ExternalHandleType::VkImageHandle;
    described.existingImage = image.Image();
    described.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    described.width         = kWidth;
    described.height        = kHeight;
    described.tiling        = VK_IMAGE_TILING_LINEAR;
    described.layout        = image.Layout();
    described.colorModel    = ColorModel::YCbCr;
    // Must match vkCreateImage exactly: the library cannot read these back
    // from a VkImage handle and uses them to decide how to reach the encoder.
    described.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    described.createFlags   = 0;
    described.residency     = Residency::Local;
    described.allocationSize = image.Size();
    return described;
}

//=============================================================================
// Checks that need a device
//=============================================================================

// Every role a session offers, and the one it does not.
void TestSessionRoles(const Ref<IEncoderSession>& session)
{
    Check(Query<IFrameSubmitter>(session) != nullptr,   "IFrameSubmitter resolves");
    Check(Query<IBitstreamSource>(session) != nullptr,  "IBitstreamSource resolves");

    // A session knows the device it runs on. A caller holding only a session
    // must not have to keep the platform alive to ask.
    Ref<IDeviceBinding> bound = Query<IDeviceBinding>(session);
    Check(bound != nullptr, "IDeviceBinding resolves from the session");
    if (bound) {
        Check(bound->Device() != VK_NULL_HANDLE,
              "the session reports the device it runs on");
        Check(bound->GetInstanceProcAddr() != nullptr,
              "the session reports the loader it was built with");

        // The identity, not the handle. A host that adopted a device confirms
        // the encoder bound the one it meant, and two instances give different
        // handles for the same hardware.
        uint8_t uuid[VK_UUID_SIZE] = {};
        uint8_t zero[VK_UUID_SIZE] = {};
        const bool identified = bound->DeviceUuid(uuid);
        Check(identified, "the session identifies its device");
        Check(!identified || memcmp(uuid, zero, sizeof(uuid)) != 0,
              "the device identity is not all zero");
    }

    // EVERY ROLE THE INTERFACE DEFINES IS REACHABLE FROM A SESSION.
    //
    // Listed here rather than checked one by one above so that adding a role
    // to the interface and forgetting to answer its id fails here. That
    // failure is otherwise silent in the worst way: a host queries the role,
    // gets null, and takes whatever fallback it has -- which is how an
    // unreachable IDeviceBinding turned off a whole zero-copy input tier
    // while every frame still encoded correctly, by the slowest path.
    {
        struct RoleRow { const char* id; const char* name; };
        static const RoleRow kRoles[] = {
            {IEncoderSession::kId.data(),   "IEncoderSession"},
            {IFrameSubmitter::kId.data(),   "IFrameSubmitter"},
            {IBitstreamSource::kId.data(),  "IBitstreamSource"},
            {IDeviceBinding::kId.data(),    "IDeviceBinding"},
        };
        uint32_t reachable = 0;
        for (const RoleRow& role : kRoles) {
            if (session->QueryInterface(role.id) != nullptr) {
                ++reachable;
            } else {
                fprintf(stderr, "        unreachable role: %s\n", role.name);
            }
        }
        Check(reachable == sizeof(kRoles) / sizeof(kRoles[0]),
              "every role the interface defines is reachable from a session");
    }

    // A role the session vends outlives the caller's reference to the session:
    // the aliasing Ref shares the owner's count.
    Ref<IBitstreamSource> bitstream = Query<IBitstreamSource>(session);
    Ref<IEncoderSession>  local     = session;
    local.reset();
    Check(bitstream != nullptr && bitstream->Acquire(9999).status().code() !=
              ResultCode::Ok,
          "a role keeps its owner alive after the session ref is dropped");
}

void TestConfigRules(const Ref<IEncoderPlatform>& platform)
{
    // A configuration refuses a profile from another codec, before anything
    // is allocated.
    Expected<Ref<IEncoderConfig>> wrong = platform->CreateConfig(Codec::H264, Profile::H265Main);
    Check(!wrong && wrong.status().code() == ResultCode::UnsupportedProfile,
          "a profile from another codec is refused at CreateConfig");

    Expected<Ref<IEncoderConfig>> h264 = platform->CreateConfig(Codec::H264, Profile::H264Main);
    if (!h264) {
        Skip("H.264 configuration checks", h264.status().detail());
        return;
    }

    // An unconfigured configuration says why it is not usable, without a
    // session having been created.
    Result unset = (*h264)->Validate();
    Check(!unset && unset.code() == ResultCode::InvalidArgument,
          "an empty configuration fails Validate with a reason");

    (*h264)->SetCodedExtent(1920, 1080)
            .SetFrameRate(60, 1)
            .SetInputFormat(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, ColorModel::YCbCr);
    Check(static_cast<bool>((*h264)->Validate()), "a complete configuration validates");
    Check((*h264)->GetCodec() == Codec::H264, "the configuration remembers its codec");

    // The rule that matters: the library answers which input path a
    // configuration resolves to. NV12 is what the encoder reads, so no filter.
    Check((*h264)->ResolveInputPath() == InputPath::Copy,
          "NV12 resolves to a copy, not a compute filter");

    // HDR10 static metadata. H.265 and AV1 carry it; H.264 has no
    // mastering-display or content-light SEI, so Validate refuses it there
    // rather than accepting metadata the stream would drop.
    HdrMetadata meta;
    meta.contentLightLevelPresent  = true;
    meta.maxContentLightLevel      = 1000;
    meta.maxFrameAverageLightLevel = 400;

    Expected<Ref<IEncoderConfig>> h264Hdr = platform->CreateConfig(Codec::H264, Profile::H264Main);
    if (h264Hdr) {
        (*h264Hdr)->SetCodedExtent(1920, 1080)
                   .SetFrameRate(60, 1)
                   .SetInputFormat(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, ColorModel::YCbCr)
                   .SetHdrMetadata(meta);
        Result r = (*h264Hdr)->Validate();
        Check(!r && r.code() == ResultCode::UnsupportedFeature,
              "HDR10 metadata on an H.264 configuration is refused by Validate");
    }

    Expected<Ref<IEncoderConfig>> h265 = platform->CreateConfig(Codec::H265, Profile::H265Main10);
    if (h265) {
        (*h265)->SetCodedExtent(1920, 1080)
                .SetFrameRate(60, 1)
                .SetInputFormat(VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
                                ColorModel::YCbCr)
                .SetHdrMetadata(meta);
        Check(static_cast<bool>((*h265)->Validate()),
              "HDR10 metadata on an H.265 configuration validates");
    } else {
        Skip("H.265 HDR metadata check", h265.status().detail());
    }

    // A format the encoder cannot read on any path is refused by Validate,
    // not at session creation.
    Expected<Ref<IEncoderConfig>> bad = platform->CreateConfig(Codec::H264, Profile::H264Main);
    if (bad) {
        (*bad)->SetCodedExtent(320, 240)
               .SetFrameRate(30, 1)
               .SetInputFormat(VK_FORMAT_D32_SFLOAT, ColorModel::FromFormat);
        Result r = (*bad)->Validate();
        Check(!r && r.code() == ResultCode::UnsupportedFormat,
              "a depth format is refused with UnsupportedFormat");

        // So is a format the encoder reaches only through the compute
        // filter, which no submitted frame runs through.
        (*bad)->SetInputFormat(VK_FORMAT_R8G8B8A8_UNORM, ColorModel::Rgb);
        r = (*bad)->Validate();
        Check(!r && r.code() == ResultCode::UnsupportedFormat,
              "an RGBA input is refused with UnsupportedFormat");
    }
}

// Returns false when the platform exists but no device on it can encode,
// which is a property of the machine and not a result about the interface.
bool TestCaps(const Ref<IEncoderPlatform>& platform)
{
    Ref<IEncoderCaps> caps = platform->Caps();
    Check(caps != nullptr, "the platform vends capabilities");
    if (!caps) {
        return false;
    }

    ArrayView<Codec> codecs = caps->Codecs();
    fprintf(stderr, "        codecs advertised: %zu\n", codecs.size());
    if (codecs.empty()) {
        Skip("codec and profile enumeration", "no encode-capable device");
        return false;
    }

    for (Codec codec : codecs) {
        ArrayView<Profile> profiles = caps->Profiles(codec);
        fprintf(stderr, "        codec %u: %zu profiles\n",
                static_cast<unsigned>(codec), profiles.size());
    }

    // Enumeration is repeatable: a second call returns the same view rather
    // than re-querying into different storage.
    Check(caps->Codecs().data() == codecs.data(), "capability views are stable");
    return true;
}

//=============================================================================
// Reconfiguration
//=============================================================================

void TestReconfigure(const Ref<IEncoderPlatform>& platform,
                     const Ref<IEncoderSession>&  session,
                     const Ref<IEncoderConfig>&   config)
{
    // What moves: the rate. Handing back the very configuration the session
    // was built from, with the rate changed, is the ordinary case.
    RateControl faster;
    faster.mode           = RateControlMode::Cbr;
    faster.averageBitrate = 4 * 1000 * 1000;
    faster.maxBitrate     = 4 * 1000 * 1000;
    config->SetRateControl(faster);

    Result applied = session->Reconfigure(config);
    if (!applied) {
        Skip("reconfiguration", applied.detail());
        return;
    }
    Check(true, "a rate change is applied mid-stream");

    // A fresh configuration carrying nothing but the change must work too:
    // an unset field means "leave it alone", so the caller is not made to
    // restate the resolution and format it is not touching.
    Expected<Ref<IEncoderConfig>> minimal =
        platform->CreateConfig(config->GetCodec(), config->GetProfile());
    if (minimal) {
        RateControl slower;
        slower.mode           = RateControlMode::Cbr;
        slower.averageBitrate = 1500 * 1000;
        (*minimal)->SetRateControl(slower);
        Check(static_cast<bool>(session->Reconfigure(*minimal)),
              "a configuration carrying only the change is accepted");
    }

    // What does not move: a resolution change is refused, by name, and the
    // session keeps running.
    Expected<Ref<IEncoderConfig>> resized =
        platform->CreateConfig(config->GetCodec(), config->GetProfile());
    if (resized) {
        (*resized)->SetCodedExtent(640, 480);
        RateControl same;
        same.mode           = RateControlMode::Cbr;
        same.averageBitrate = 1500 * 1000;
        (*resized)->SetRateControl(same);

        Result refused = session->Reconfigure(*resized);
        Check(!refused && refused.code() == ResultCode::UnsupportedFeature,
              "a resolution change is refused rather than half-applied");
        fprintf(stderr, "        refusal named: %s\n", refused.detail());
        Check(strstr(refused.detail(), "width") != nullptr,
              "the refusal names the field that cannot change");
    }

    // The session still encodes after a refusal.
    Check(static_cast<bool>(session->Drain()),
          "the session still works after a refused reconfiguration");
}

// Frames in, bitstream out, through the interface only.
//
// Returns false only when the machine could not be asked -- a device that
// refuses a linear NV12 image is a property of the driver, not a result about
// the interface.
bool TestFrameTransfer(const Ref<IEncoderPlatform>& platform,
                       const Ref<IEncoderSession>&  session,
                       const Ref<IEncoderConfig>&   config)
{
    Ref<IDeviceBinding> binding = platform->DeviceBinding();
    if (!binding || binding->Device() == VK_NULL_HANDLE) {
        Skip("frame transfer", "no device binding");
        return false;
    }

    DeviceFns fns;
    if (!fns.Load(binding->GetInstanceProcAddr(), binding->Instance(), binding->Device())) {
        Skip("frame transfer", "device entry points unavailable");
        return false;
    }

    HostImage image(fns, binding->PhysicalDevice(), binding->Device());
    const char* why = "";
    if (!image.Create(&why)) {
        Skip("frame transfer", why);
        return false;
    }

    Ref<IFrameSubmitter>  submitter = Query<IFrameSubmitter>(session);
    Ref<IBitstreamSource> bitstream = Query<IBitstreamSource>(session);
    if (!submitter || !bitstream) {
        Skip("frame transfer", "the session does not offer submit and bitstream roles");
        return false;
    }

    const uint32_t kFrames = 8;

    // ---- inline submission ----
    uint32_t encoded = 0;
    uint32_t idrSeen = 0;
    size_t   totalBytes = 0;
    bool     firstIsIdr = false;
    bool     submitOk = true;

    for (uint32_t i = 0; i < kFrames; ++i) {
        if (!image.WritePattern(i, &why)) {
            Skip("frame transfer", why);
            return false;
        }
        FrameSubmit frame;
        frame.frameId     = 1000 + i;
        frame.pts         = i * 3000;
        frame.image       = DescribeImage(image);
        frame.forceIdr    = (i == 0);
        // The stream is not ended here. isLastFrame finalises the session, and
        // the checks below still have frames to send.

        Result r = submitter->SubmitFrame(frame);
        if (!r) {
            Check(false, "SubmitFrame accepts an inline image");
            fprintf(stderr, "        detail: %s\n", r.detail());
            submitOk = false;
            break;
        }
        image.MarkSubmitted();
    }
    if (!submitOk) {
        return true;   // a real failure, already counted
    }
    Check(true, "SubmitFrame accepts an inline image");

    Check(static_cast<bool>(session->Drain()), "Drain returns success");

    for (uint32_t i = 0; i < kFrames; ++i) {
        Expected<EncodedFrame> got = bitstream->AcquireNext();
        if (!got) {
            break;
        }
        if (encoded == 0) {
            firstIsIdr = got->isIdr;
        }
        if (got->isIdr) {
            ++idrSeen;
        }
        totalBytes += got->bitstream.size();
        ++encoded;
        bitstream->Release(got->frameId);
    }

    fprintf(stderr, "        %u frames encoded, %zu bitstream bytes, %u IDR\n",
            encoded, totalBytes, idrSeen);

    Check(encoded == kFrames, "every submitted frame comes back encoded");
    Check(totalBytes > 0, "the bitstream is not empty");
    Check(firstIsIdr, "the first frame is an IDR");

    // ---- a second batch: the drain left the session usable ----
    uint32_t secondEncoded = 0;
    bool     secondSubmitOk = true;
    for (uint32_t i = 0; i < 4; ++i) {
        if (!image.WritePattern(kFrames + i, &why)) {
            break;
        }
        FrameSubmit frame;
        frame.frameId = 2000 + i;
        frame.pts     = (kFrames + i) * 3000;
        frame.image   = DescribeImage(image);

        Result r = submitter->SubmitFrame(frame);
        if (!r) {
            Check(false, "SubmitFrame accepts a frame after a drain");
            fprintf(stderr, "        detail: %s\n", r.detail());
            secondSubmitOk = false;
            break;
        }
    }
    if (secondSubmitOk) {
        Check(true, "SubmitFrame accepts a frame after a drain");
        Check(static_cast<bool>(session->Drain()),
              "Drain leaves the session usable between batches");
        for (uint32_t i = 0; i < 4; ++i) {
            Expected<EncodedFrame> got = bitstream->AcquireNext();
            if (!got) {
                break;
            }
            ++secondEncoded;
            bitstream->Release(got->frameId);
        }
        fprintf(stderr, "        %u frames of the second batch encoded\n", secondEncoded);
        Check(secondEncoded == 4, "every frame of the second batch comes back encoded");
    }

    // Reconfiguration, while the session is live: it must run BEFORE the
    // stream is ended below, because a reconfiguration needs a session to
    // reconfigure.
    TestReconfigure(platform, session, config);

    // A frame with no image is refused, not silently encoded. Checked while
    // the session is live: once the stream has ended every submission is
    // refused, and a refusal for the wrong reason proves nothing.
    {
        FrameSubmit bogus;
        bogus.frameId = 3000;
        bogus.image   = DescribeImage(image);
        bogus.image.existingImage = VK_NULL_HANDLE;
        const Result refused = submitter->SubmitFrame(bogus);
        Check(!refused, "a frame with no image is refused");
        fprintf(stderr, "        no-image refusal: %s\n", refused.detail());
    }

    // Ending the stream is a real state change, so it is checked rather than
    // assumed: a submission after the last frame is refused.
    {
        FrameSubmit last;
        last.frameId     = 2999;
        last.image       = DescribeImage(image);
        last.isLastFrame = true;
        if (submitter->SubmitFrame(last)) {
            Check(static_cast<bool>(session->Finish()), "Finish ends the stream");
            Expected<EncodedFrame> tail = bitstream->AcquireNext();
            if (tail) {
                bitstream->Release(tail->frameId);
            }
            FrameSubmit after;
            after.frameId = 3001;
            after.image   = DescribeImage(image);
            Check(!submitter->SubmitFrame(after),
                  "a submission after the stream ends is refused");
        }
    }

    fns.DeviceWaitIdle(binding->Device());
    return true;
}

} // namespace

int main(int argc, const char** argv)
{
    (void)argc;
    (void)argv;

    fprintf(stderr, "-- checks that need no device --\n");
    TestResultIsCheap();
    TestExpectedCarriesEither();
    TestArrayView();
    TestFormatClassification();

    fprintf(stderr, "-- checks that need an encode device --\n");
    PlatformCreateInfo info;
    info.silenceStdio = false;

    Ref<IEncoderPlatform> platform;
    const VkResult created = VkEncCreatePlatform(info, platform);
    if (created != VK_SUCCESS || !platform) {
        return NoDevice("every device-backed check", "VkEncCreatePlatform failed");
    }

    Check(Query<IEncoderPlatform>(platform) != nullptr, "the platform answers its own id");
    Check(platform->DeviceBinding() != nullptr, "the platform vends a device binding");

    const bool haveEncodeDevice = TestCaps(platform);

    // The configuration rules are the library's own and hold with or without a
    // device, so they are checked either way.
    TestConfigRules(platform);

    if (!haveEncodeDevice) {
        return NoDevice("session role checks", "the platform reports no encode-capable device");
    }

    Expected<Ref<IEncoderConfig>> config =
        platform->CreateConfig(Codec::H264, Profile::H264Main);
    Check(static_cast<bool>(config), "an H.264 Main configuration is created");
    if (config) {
        // A rate-controlled session, because rateControlMode is immutable:
        // a session created without one cannot be moved into CBR later, and
        // the reconfiguration checks below are about the rate, not the mode.
        RateControl rate;
        rate.mode           = RateControlMode::Cbr;
        rate.averageBitrate = 2 * 1000 * 1000;
        rate.maxBitrate     = 2 * 1000 * 1000;

        (*config)->SetCodedExtent(320, 240)
                  .SetFrameRate(30, 1)
                  .SetInputFormat(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, ColorModel::YCbCr)
                  .SetRateControl(rate);
        Expected<Ref<IEncoderSession>> session = platform->CreateSession(*config);
        Check(static_cast<bool>(session), "a session is created on the encode device");
        if (session) {
            TestSessionRoles(*session);
            TestFrameTransfer(platform, *session, *config);
        } else {
            fprintf(stderr, "        %s\n", session.status().detail());
        }
    }

    return Summarise();
}
