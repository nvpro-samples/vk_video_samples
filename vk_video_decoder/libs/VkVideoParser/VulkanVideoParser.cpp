/*
 * Copyright 2021 NVIDIA Corporation.
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

#include "VulkanVideoParser.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <queue> // std::queue

#include "VulkanVideoParserIf.h"
#include "NvVideoParser/nvVulkanVideoParser.h"
#include "NvVideoParser/nvVulkanVideoUtils.h"
#include "PictureBufferBase.h"
#include "VkCodecUtils/nvVideoProfile.h"
#include "StdVideoPictureParametersSet.h"

#undef min
#undef max

static const uint32_t topFieldShift = 0;
static const uint32_t topFieldMask = (1 << topFieldShift);
static const uint32_t bottomFieldShift = 1;
static const uint32_t bottomFieldMask = (1 << bottomFieldShift);
static const uint32_t fieldIsReferenceMask = (topFieldMask | bottomFieldMask);

static const uint32_t EXTRA_DPB_SLOTS = 1;
static const uint32_t MAX_DPB_SLOTS_PLUS_1 = 16 + EXTRA_DPB_SLOTS;

#define COPYFIELD(pout, pin, name) pout->name = pin->name

struct nvVideoDecodeH264DpbSlotInfo {
    VkVideoDecodeH264DpbSlotInfoEXT dpbSlotInfo;
    StdVideoDecodeH264ReferenceInfo stdReferenceInfo;

    nvVideoDecodeH264DpbSlotInfo()
        : dpbSlotInfo()
        , stdReferenceInfo()
    {
    }

    const VkVideoDecodeH264DpbSlotInfoEXT* Init(int8_t slotIndex)
    {
        assert((slotIndex >= 0) && (slotIndex < 17));
        dpbSlotInfo.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_EXT;
        dpbSlotInfo.pNext = NULL;
        dpbSlotInfo.pStdReferenceInfo = &stdReferenceInfo;
        return &dpbSlotInfo;
    }

    bool IsReference() const
    {
        return (dpbSlotInfo.pStdReferenceInfo == &stdReferenceInfo);
    }

    operator bool() const { return IsReference(); }
    void Invalidate() { memset(this, 0x00, sizeof(*this)); }
};

struct nvVideoDecodeH265DpbSlotInfo {
    VkVideoDecodeH265DpbSlotInfoEXT dpbSlotInfo;
    StdVideoDecodeH265ReferenceInfo stdReferenceInfo;

    nvVideoDecodeH265DpbSlotInfo()
        : dpbSlotInfo()
        , stdReferenceInfo()
    {
    }

    const VkVideoDecodeH265DpbSlotInfoEXT* Init(int8_t slotIndex)
    {
        assert((slotIndex >= 0) && (slotIndex < 16));
        dpbSlotInfo.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_EXT;
        dpbSlotInfo.pNext = NULL;
        dpbSlotInfo.pStdReferenceInfo = &stdReferenceInfo;
        return &dpbSlotInfo;
    }

    bool IsReference() const
    {
        return (dpbSlotInfo.pStdReferenceInfo == &stdReferenceInfo);
    }

    operator bool() const { return IsReference(); }

    void Invalidate() { memset(this, 0x00, sizeof(*this)); }
};
/******************************************************/
//! \struct nvVideoH264PicParameters
//! H.264 picture parameters
/******************************************************/
struct nvVideoH264PicParameters {
    enum { MAX_REF_PICTURES_LIST_ENTRIES = 16 };

    StdVideoDecodeH264PictureInfo stdPictureInfo;
    VkVideoDecodeH264PictureInfoEXT pictureInfo;
    VkVideoDecodeH264SessionParametersAddInfoEXT pictureParameters;
    VkVideoDecodeH264MvcEXT mvcInfo;
    nvVideoDecodeH264DpbSlotInfo currentDpbSlotInfo;
    nvVideoDecodeH264DpbSlotInfo dpbRefList[MAX_REF_PICTURES_LIST_ENTRIES];
};

/*******************************************************/
//! \struct nvVideoH265PicParameters
//! HEVC picture parameters
/*******************************************************/
struct nvVideoH265PicParameters {
    enum { MAX_REF_PICTURES_LIST_ENTRIES = 16 };

    StdVideoDecodeH265PictureInfo stdPictureInfo;
    VkVideoDecodeH265PictureInfoEXT pictureInfo;
    VkVideoDecodeH265SessionParametersAddInfoEXT pictureParameters;
    nvVideoDecodeH265DpbSlotInfo dpbRefList[MAX_REF_PICTURES_LIST_ENTRIES];
};

static vkPicBuffBase* GetPic(VkPicIf* pPicBuf)
{
    return (vkPicBuffBase*)pPicBuf;
}

// Keeps track of data associated with active internal reference frames
class DpbSlot {
public:
    bool isInUse() { return (m_reserved || m_inUse); }

    bool isAvailable() { return !isInUse(); }

    bool Invalidate()
    {
        bool wasInUse = isInUse();
        if (m_picBuf) {
            m_picBuf->Release();
            m_picBuf = NULL;
        }

        m_reserved = m_inUse = false;

        return wasInUse;
    }

    vkPicBuffBase* getPictureResource() { return m_picBuf; }

    vkPicBuffBase* setPictureResource(vkPicBuffBase* picBuf, int32_t age = 0)
    {
        vkPicBuffBase* oldPic = m_picBuf;

        if (picBuf) {
            picBuf->AddRef();
        }
        m_picBuf = picBuf;

        if (oldPic) {
            oldPic->Release();
        }

        m_pictureId = age;
        return oldPic;
    }

    void Reserve() { m_reserved = true; }

    void MarkInUse(int32_t age = 0)
    {
        m_pictureId = age;
        m_inUse = true;
    }

    int32_t getAge() { return m_pictureId; }

private:
    int32_t m_pictureId; // PictureID at map time (age)
    vkPicBuffBase* m_picBuf; // Associated resource
    int32_t m_reserved : 1;
    int32_t m_inUse : 1;
};

class DpbSlots {
public:
    DpbSlots(uint32_t dpbMaxSize)
        : m_dpbMaxSize(dpbMaxSize)
        , m_slotInUseMask(0)
        , m_dpb(m_dpbMaxSize)
        , m_dpbSlotsAvailable()
    {
        Init(dpbMaxSize);
    }

    int32_t Init(uint32_t newDpbMaxSize)
    {
        assert(newDpbMaxSize <= MAX_DPB_SLOTS_PLUS_1);

        Deinit();

        m_dpbMaxSize = newDpbMaxSize;

        m_dpb.resize(m_dpbMaxSize);

        for (uint32_t ndx = 0; ndx < m_dpbMaxSize; ndx++) {
            m_dpb[ndx].Invalidate();
        }

        for (uint8_t dpbIndx = 0; dpbIndx < m_dpbMaxSize; dpbIndx++) {
            m_dpbSlotsAvailable.push(dpbIndx);
        }

        return m_dpbMaxSize;
    }

    void Deinit()
    {
        for (uint32_t ndx = 0; ndx < m_dpbMaxSize; ndx++) {
            m_dpb[ndx].Invalidate();
        }

        while (!m_dpbSlotsAvailable.empty()) {
            m_dpbSlotsAvailable.pop();
        }

        m_dpbMaxSize = 0;
        m_slotInUseMask = 0;
    }

    ~DpbSlots() { Deinit(); }

    int8_t AllocateSlot()
    {
        if (m_dpbSlotsAvailable.empty()) {
            assert(!"No more h.264 slots are available");
            return -1;
        }
        int8_t slot = (int8_t)m_dpbSlotsAvailable.front();
        assert((slot >= 0) && ((uint8_t)slot < m_dpbMaxSize));
        m_slotInUseMask |= (1 << slot);
        m_dpbSlotsAvailable.pop();
        m_dpb[slot].Reserve();
        return slot;
    }

    void FreeSlot(int8_t slot)
    {
        assert((uint8_t)slot < m_dpbMaxSize);
        assert(m_dpb[slot].isInUse());
        assert(m_slotInUseMask & (1 << slot));

        m_dpb[slot].Invalidate();
        m_dpbSlotsAvailable.push(slot);
        m_slotInUseMask &= ~(1 << slot);
    }

    DpbSlot& operator[](uint32_t slot)
    {
        assert(slot < m_dpbMaxSize);
        return m_dpb[slot];
    }

    // Return the remapped index given an external decode render target index
    int8_t GetSlotOfPictureResource(vkPicBuffBase* pPic)
    {
        for (int8_t i = 0; i < (int8_t)m_dpbMaxSize; i++) {
            if ((m_slotInUseMask & (1 << i)) && m_dpb[i].isInUse() && (pPic == m_dpb[i].getPictureResource())) {
                return i;
            }
        }
        return -1; // not found
    }

    void MapPictureResource(vkPicBuffBase* pPic, int32_t dpbSlot,
        int32_t age = 0)
    {
        for (uint32_t slot = 0; slot < m_dpbMaxSize; slot++) {
            if ((uint8_t)slot == dpbSlot) {
                m_dpb[slot].setPictureResource(pPic, age);
            } else if (pPic) {
                if (m_dpb[slot].getPictureResource() == pPic) {
                    FreeSlot(slot);
                }
            }
        }
    }

    uint32_t getSlotInUseMask() { return m_slotInUseMask; }

    uint32_t getMaxSize() { return m_dpbMaxSize; }

private:
    uint32_t m_dpbMaxSize;
    uint32_t m_slotInUseMask;
    std::vector<DpbSlot> m_dpb;
    std::queue<uint8_t> m_dpbSlotsAvailable;
};

class VulkanVideoParser : public VkParserVideoDecodeClient,
                          public IVulkanVideoParser {
    friend class IVulkanVideoParser;

public:
    enum { MAX_FRM_CNT = 32 };
    enum { HEVC_MAX_DPB_SLOTS = 16 };
    enum { AVC_MAX_DPB_SLOTS = 17 };
    enum { MAX_REMAPPED_ENTRIES = 20 };

    // H.264 internal DPB structure
    typedef struct dpbH264Entry {
        int8_t dpbSlot;
        // bit0(used_for_reference)=1: top field used for reference,
        // bit1(used_for_reference)=1: bottom field used for reference
        uint32_t used_for_reference : 2;
        uint32_t is_long_term : 1; // 0 = short-term, 1 = long-term
        uint32_t is_non_existing : 1; // 1 = marked as non-existing
        uint32_t
            is_field_ref : 1; // set if unpaired field or complementary field pair
        union {
            int16_t FieldOrderCnt[2]; // h.264 : 2*32 [top/bottom].
            int32_t PicOrderCnt; // HEVC PicOrderCnt
        };
        union {
            int16_t FrameIdx; // : 16   short-term: FrameNum (16 bits), long-term:
                // LongTermFrameIdx (4 bits)
            int8_t originalDpbIndex; // Original Dpb source Index.
        };
        vkPicBuffBase* m_picBuff; // internal picture reference

        void setReferenceAndTopBoottomField(
            bool isReference, bool nonExisting, bool isLongTerm, bool isFieldRef,
            bool topFieldIsReference, bool bottomFieldIsReference, int16_t frameIdx,
            const int16_t fieldOrderCntList[2], vkPicBuffBase* picBuff)
        {
            is_non_existing = nonExisting;
            is_long_term = isLongTerm;
            is_field_ref = isFieldRef;
            if (isReference && isFieldRef) {
                used_for_reference = (bottomFieldIsReference << bottomFieldShift) | (topFieldIsReference << topFieldShift);
            } else {
                used_for_reference = isReference ? 3 : 0;
            }

            FrameIdx = frameIdx;

            FieldOrderCnt[0] = fieldOrderCntList[used_for_reference == 2]; // 0: for progressive and top reference; 1: for
                // bottom reference only.
            FieldOrderCnt[1] = fieldOrderCntList[used_for_reference != 1]; // 0: for top reference only;  1: for bottom
                // reference and progressive.

            dpbSlot = -1;
            m_picBuff = picBuff;
        }

        void setReference(bool isLongTerm, int32_t picOrderCnt,
            vkPicBuffBase* picBuff)
        {
            is_non_existing = (picBuff == NULL);
            is_long_term = isLongTerm;
            is_field_ref = false;
            used_for_reference = (picBuff != NULL) ? 3 : 0;

            PicOrderCnt = picOrderCnt;

            dpbSlot = -1;
            m_picBuff = picBuff;
            originalDpbIndex = -1;
        }

        bool isRef() { return (used_for_reference != 0); }

        StdVideoDecodeH264ReferenceInfoFlags getPictureFlag()
        {
            StdVideoDecodeH264ReferenceInfoFlags picFlags = StdVideoDecodeH264ReferenceInfoFlags();
            if (m_dumpParserData)
                std::cout << "\t\t Flags: ";

            if (used_for_reference) {
                if (m_dumpParserData)
                    std::cout << "FRAME_IS_REFERENCE ";
                // picFlags.is_reference = true;
            }

            if (is_long_term) {
                if (m_dumpParserData)
                    std::cout << "IS_LONG_TERM ";
                picFlags.is_long_term = true;
            }
            if (is_non_existing) {
                if (m_dumpParserData)
                    std::cout << "IS_NON_EXISTING ";
                picFlags.is_non_existing = true;
            }

            if (is_field_ref) {
                if (m_dumpParserData)
                    std::cout << "IS_FIELD ";
                // picFlags.field_pic_flag = true;
            }

            if (used_for_reference & topFieldMask) {
                if (m_dumpParserData)
                    std::cout << "TOP_FIELD_IS_REF ";
                picFlags.top_field_flag = true;
            }
            if (used_for_reference & bottomFieldMask) {
                if (m_dumpParserData)
                    std::cout << "BOTTOM_FIELD_IS_REF ";
                picFlags.bottom_field_flag = true;
            }

            return picFlags;
        }

        void setH264PictureData(nvVideoDecodeH264DpbSlotInfo* pDpbRefList,
            VkVideoReferenceSlotKHR* pReferenceSlots,
            uint32_t dpbEntryIdx, uint32_t dpbSlotIndex)
        {
            assert(dpbEntryIdx < AVC_MAX_DPB_SLOTS);
            assert(dpbSlotIndex < AVC_MAX_DPB_SLOTS);

            assert((dpbSlotIndex == (uint32_t)dpbSlot) || is_non_existing);
            pReferenceSlots[dpbEntryIdx].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_KHR;
            pReferenceSlots[dpbEntryIdx].slotIndex = dpbSlotIndex;
            pReferenceSlots[dpbEntryIdx].pNext = pDpbRefList[dpbEntryIdx].Init(dpbSlotIndex);

            StdVideoDecodeH264ReferenceInfo* pRefPicInfo = &pDpbRefList[dpbEntryIdx].stdReferenceInfo;
            pRefPicInfo->FrameNum = FrameIdx;
            if (m_dumpParserData) {
                std::cout << "\tdpbEntryIdx: " << dpbEntryIdx
                          << "dpbSlotIndex: " << dpbSlotIndex
                          << " FrameIdx: " << (int32_t)FrameIdx;
            }
            pRefPicInfo->flags = getPictureFlag();
            pRefPicInfo->PicOrderCnt[0] = FieldOrderCnt[0];
            pRefPicInfo->PicOrderCnt[1] = FieldOrderCnt[1];
            if (m_dumpParserData)
                std::cout << " fieldOrderCnt[0]: " << pRefPicInfo->PicOrderCnt[0]
                          << " fieldOrderCnt[1]: " << pRefPicInfo->PicOrderCnt[1]
                          << std::endl;
        }

        void setH265PictureData(nvVideoDecodeH265DpbSlotInfo* pDpbSlotInfo,
            VkVideoReferenceSlotKHR* pReferenceSlots,
            uint32_t dpbEntryIdx, uint32_t dpbSlotIndex)
        {
            assert(dpbEntryIdx < HEVC_MAX_DPB_SLOTS);
            assert(dpbSlotIndex < HEVC_MAX_DPB_SLOTS);
            assert(isRef());

            assert((dpbSlotIndex == (uint32_t)dpbSlot) || is_non_existing);
            pReferenceSlots[dpbEntryIdx].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_KHR;
            pReferenceSlots[dpbEntryIdx].slotIndex = dpbSlotIndex;
            pReferenceSlots[dpbEntryIdx].pNext = pDpbSlotInfo[dpbEntryIdx].Init(dpbSlotIndex);

            StdVideoDecodeH265ReferenceInfo* pRefPicInfo = &pDpbSlotInfo[dpbEntryIdx].stdReferenceInfo;
            pRefPicInfo->PicOrderCntVal = PicOrderCnt;
            pRefPicInfo->flags.is_long_term = is_long_term;
            pRefPicInfo->flags.is_non_existing = is_non_existing;

            if (m_dumpParserData) {
                std::cout << "\tdpbIndex: " << dpbSlotIndex
                          << " picOrderCntValList: " << PicOrderCnt;

                std::cout << "\t\t Flags: ";
                std::cout << "FRAME IS REFERENCE ";
                if (pRefPicInfo->flags.is_long_term) {
                    std::cout << "IS LONG TERM ";
                }
                if (pRefPicInfo->flags.is_non_existing) {
                    std::cout << "NON EXISTING ";
                }
                std::cout << std::endl;
            }
        }

    } dpbH264Entry;

    virtual int32_t AddRef();
    virtual int32_t Release();

    // INvVideoDecoderClient
    virtual VkResult ParseVideoData(VkParserSourceDataPacket* pPacket);

    // Interface to allow decoder to communicate with the client implementaitng
    // INvVideoDecoderClient

    virtual int32_t BeginSequence(
        const VkParserSequenceInfo* pnvsi); // Returns max number of reference frames (always
        // at least 2 for MPEG-2)
    virtual bool AllocPictureBuffer(
        VkPicIf** ppPicBuf); // Returns a new VkPicIf interface
    virtual bool DecodePicture(
        VkParserPictureData* pParserPictureData); // Called when a picture is ready to be decoded
    virtual bool DisplayPicture(
        VkPicIf* pPicBuf,
        int64_t llPTS); // Called when a picture is ready to be displayed
    virtual void UnhandledNALU(
        const uint8_t* pbData,
        int32_t cbData) {}; // Called for custom NAL parsing (not required)

    virtual uint32_t GetDecodeCaps()
    {
        uint32_t decode_caps = 0; // NVD_CAPS_MVC | NVD_CAPS_SVC; // !!!
        return decode_caps;
    };

    const IVulkanVideoDecoderHandler* GetDecoderHandler()
    {
        return m_pDecoderHandler;
    }

    IVulkanVideoFrameBufferParserCb* GetFrameBufferParserCb()
    {
        return m_pVideoFrameBuffer;
    }

    uint32_t GetNumNumDecodeSurfaces() { return m_maxNumDecodeSurfaces; }

protected:
    void Deinitialize();
    VkResult Initialize(IVulkanVideoDecoderHandler* pDecoderHandler,
        IVulkanVideoFrameBufferParserCb* pVideoFrameBuffer,
        bool outOfBandPictureParameters,
        uint32_t errorThreshold);

    VulkanVideoParser(VkVideoCodecOperationFlagBitsKHR codecType,
        uint32_t maxNumDecodeSurfaces, uint32_t maxNumDpbSurfaces,
        uint64_t clockRate);

    virtual ~VulkanVideoParser() { Deinitialize(); }

    bool UpdatePictureParameters(
        VkPictureParameters* pPictureParameters,
        VkSharedBaseObj<VkParserVideoRefCountBase>& pictureParametersObject,
        uint64_t updateSequenceCount);

    bool DecodePicture(VkParserPictureData* pParserPictureData,
        vkPicBuffBase* pVkPicBuff,
        VkParserDecodePictureInfo* pDecodePictureInfo);

    int8_t GetPicIdx(vkPicBuffBase*);
    int8_t GetPicIdx(VkPicIf* pPicBuf);
    int8_t GetPicDpbSlot(vkPicBuffBase*);
    int8_t GetPicDpbSlot(int8_t picIndex);
    int8_t SetPicDpbSlot(vkPicBuffBase*, int8_t dpbSlot);
    int8_t SetPicDpbSlot(int8_t picIndex, int8_t dpbSlot);
    uint32_t ResetPicDpbSlots(uint32_t picIndexSlotValidMask);
    bool GetFieldPicFlag(int8_t picIndex);
    bool SetFieldPicFlag(int8_t picIndex, bool fieldPicFlag);

    uint32_t FillDpbH264State(const VkParserPictureData* pd,
        const VkParserH264DpbEntry* dpbIn,
        uint32_t maxDpbInSlotsInUse,
        nvVideoDecodeH264DpbSlotInfo* pDpbRefList,
        uint32_t maxRefPictures,
        VkVideoReferenceSlotKHR* pReferenceSlots,
        int8_t* pGopReferenceImagesIndexes,
        StdVideoDecodeH264PictureInfoFlags currPicFlags,
        int8_t* pCurrAllocatedSlotIndex);
    uint32_t FillDpbH265State(const VkParserPictureData* pd,
        const VkParserHevcPictureData* pin,
        nvVideoDecodeH265DpbSlotInfo* pDpbSlotInfo,
        StdVideoDecodeH265PictureInfo* pStdPictureInfo,
        uint32_t maxRefPictures,
        VkVideoReferenceSlotKHR* pReferenceSlots,
        int8_t* pGopReferenceImagesIndexes);

    int8_t AllocateDpbSlotForCurrentH264(
        vkPicBuffBase* pPic, StdVideoDecodeH264PictureInfoFlags currPicFlags);
    int8_t AllocateDpbSlotForCurrentH265(vkPicBuffBase* pPic, bool isReference);

protected:
    VulkanVideoDecodeParser* m_pParser;
    IVulkanVideoDecoderHandler* m_pDecoderHandler;
    IVulkanVideoFrameBufferParserCb* m_pVideoFrameBuffer;
    std::atomic<int32_t> m_refCount;
    VkVideoCodecOperationFlagBitsKHR m_codecType;
    uint32_t m_maxNumDecodeSurfaces;
    uint32_t m_maxNumDpbSurfaces;
    uint64_t m_clockRate;
    // 90% of the required Dpb mapping code is already in H264Parser.cpp. However
    // we do not want to modify the parser at this point.
    // Essentially, the parser must preallocate a Dpb entry for the current frame,
    // if it is about to become a reference, instead of using the max_dpb for the
    // index.
    VkParserSequenceInfo m_nvsi;
    int32_t m_nCurrentPictureID;
    uint32_t m_dpbSlotsMask;
    uint32_t m_fieldPicFlagMask;
    DpbSlots m_dpb;
    uint32_t m_outOfBandPictureParameters;
    int8_t m_pictureToDpbSlotMap[MAX_FRM_CNT];

public:
    static bool m_dumpParserData;
    static bool m_dumpDpbData;
};

bool VulkanVideoParser::m_dumpParserData = false;
bool VulkanVideoParser::m_dumpDpbData = false;

bool VulkanVideoParser::DecodePicture(VkParserPictureData* pd)
{
    bool result = false;

    if (!pd->pCurrPic) {
        return result;
    }

    vkPicBuffBase* pVkPicBuff = GetPic(pd->pCurrPic);
    const int32_t picIdx = pVkPicBuff ? pVkPicBuff->m_picIdx : -1;
    if (picIdx >= VulkanVideoParser::MAX_FRM_CNT) {
        assert(0);
        return result;
    }

    if (m_dumpParserData) {
        std::cout
            << "\t ==> VulkanVideoParser::DecodePicture " << picIdx << std::endl
            << "\t\t progressive: " << (bool)pd->progressive_frame
            << // Frame is progressive
            "\t\t field: " << (bool)pd->field_pic_flag << std::endl
            << // 0 = frame picture, 1 = field picture
            "\t\t\t bottom_field: " << (bool)pd->bottom_field_flag
            << // 0 = top field, 1 = bottom field (ignored if field_pic_flag=0)
            "\t\t\t second_field: " << (bool)pd->second_field
            << // Second field of a complementary field pair
            "\t\t\t top_field: " << (bool)pd->top_field_first << std::endl
            << // Frame pictures only
            "\t\t repeat_first: " << pd->repeat_first_field
            << // For 3:2 pulldown (number of additional fields, 2 = frame
            // doubling, 4 = frame tripling)
            "\t\t ref_pic: " << (bool)pd->ref_pic_flag
            << std::endl; // Frame is a reference frame
    }

    VkParserDecodePictureInfo decodePictureInfo = VkParserDecodePictureInfo();

    decodePictureInfo.pictureIndex = picIdx;
    decodePictureInfo.flags.progressiveFrame = pd->progressive_frame;
    decodePictureInfo.flags.fieldPic = pd->field_pic_flag; // 0 = frame picture, 1 = field picture
    decodePictureInfo.flags.repeatFirstField = pd->repeat_first_field; // For 3:2 pulldown (number of additional fields,
        // 2 = frame doubling, 4 = frame tripling)
    decodePictureInfo.flags.refPic = pd->ref_pic_flag; // Frame is a reference frame

    // Mark the first field as unpaired Detect unpaired fields
    if (pd->field_pic_flag) {
        decodePictureInfo.flags.bottomField = pd->bottom_field_flag; // 0 = top field, 1 = bottom field (ignored if
            // field_pic_flag=0)
        decodePictureInfo.flags.secondField = pd->second_field; // Second field of a complementary field pair
        decodePictureInfo.flags.topFieldFirst = pd->top_field_first; // Frame pictures only

        if (!pd->second_field) {
            decodePictureInfo.flags.unpairedField = true; // Incomplete (half) frame.
        } else {
            if (decodePictureInfo.flags.unpairedField) {
                decodePictureInfo.flags.syncToFirstField = true;
                decodePictureInfo.flags.unpairedField = false;
            }
        }
    }

    decodePictureInfo.frameSyncinfo.unpairedField = decodePictureInfo.flags.unpairedField;
    decodePictureInfo.frameSyncinfo.syncToFirstField = decodePictureInfo.flags.syncToFirstField;

    return DecodePicture(pd, pVkPicBuff, &decodePictureInfo);
}

bool VulkanVideoParser::DisplayPicture(VkPicIf* pPicBuff, int64_t timestamp)
{
    bool result = false;

    vkPicBuffBase* pVkPicBuff = GetPic(pPicBuff);
    assert(pVkPicBuff);

    int32_t picIdx = pVkPicBuff ? pVkPicBuff->m_picIdx : -1;

    if (m_dumpParserData) {
        std::cout << "\t ======================< " << picIdx
                  << " >============================" << std::endl;
        std::cout << "\t ==> VulkanVideoParser::DisplayPicture " << picIdx
                  << std::endl;
    }
    assert(picIdx != -1);

    assert(m_pVideoFrameBuffer);
    if (m_pVideoFrameBuffer && (picIdx != -1)) {
        VulkanVideoDisplayPictureInfo dispInfo = VulkanVideoDisplayPictureInfo();
        dispInfo.timestamp = (VkVideotimestamp)timestamp;

        int32_t retVal = m_pVideoFrameBuffer->QueueDecodedPictureForDisplay(
            (int8_t)picIdx, &dispInfo);
        if (picIdx == retVal) {
            result = true;
        } else {
            assert(!"QueueDecodedPictureForDisplay failed");
        }
    }

    if (m_dumpParserData) {
        std::cout << "\t <== VulkanVideoParser::DisplayPicture " << picIdx
                  << std::endl;
        std::cout << "\t ======================< " << picIdx
                  << " >============================" << std::endl;
    }
    return result;
}

bool VulkanVideoParser::AllocPictureBuffer(VkPicIf** ppPicBuff)
{
    bool result = false;

    assert(m_pVideoFrameBuffer);
    if (m_pVideoFrameBuffer) {
        *ppPicBuff = m_pVideoFrameBuffer->ReservePictureBuffer();
        if (*ppPicBuff) {
            result = true;
        }
    }

    if (!result) {
        *ppPicBuff = (VkPicIf*)NULL;
    }

    return result;
}

// End of  Parser callback implementation
IVulkanVideoParser* IVulkanVideoParser::CreateInstance(
    IVulkanVideoDecoderHandler* pDecoderHandler,
    IVulkanVideoFrameBufferParserCb* pVideoFrameBuffer,
    VkVideoCodecOperationFlagBitsKHR codecType, uint32_t maxNumDecodeSurfaces,
    uint32_t maxNumDpbSurfaces, uint64_t clockRate,
    uint32_t errorThreshold)
{
    if (!pDecoderHandler || !pVideoFrameBuffer) {
        return NULL;
    }
    VulkanVideoParser* pParser = new VulkanVideoParser(
        codecType, maxNumDecodeSurfaces, maxNumDpbSurfaces, clockRate);
    if (!pParser) {
        return NULL;
    }
    const bool outOfBandPictureParameters = true;
    VkResult err = pParser->Initialize(pDecoderHandler, pVideoFrameBuffer,
                                       outOfBandPictureParameters, errorThreshold);
    if (err != VK_SUCCESS) {
        pParser->Release();
        pParser = NULL;
    }

    return pParser;
}

int32_t VulkanVideoParser::AddRef() { return ++m_refCount; }

int32_t VulkanVideoParser::Release()
{
    uint32_t ret;
    ret = --m_refCount;
    // Destroy the device if refcount reaches zero
    if (ret == 0) {
        Deinitialize();
        delete this;
    }
    return ret;
}

VulkanVideoParser::VulkanVideoParser(VkVideoCodecOperationFlagBitsKHR codecType,
    uint32_t maxNumDecodeSurfaces,
    uint32_t maxNumDpbSurfaces,
    uint64_t clockRate)
    : m_pParser(NULL)
    , m_pDecoderHandler(NULL)
    , m_pVideoFrameBuffer(NULL)
    , m_refCount(1)
    , m_codecType(codecType)
    , m_maxNumDecodeSurfaces(maxNumDecodeSurfaces)
    , m_maxNumDpbSurfaces(maxNumDpbSurfaces)
    , m_clockRate(clockRate)
    , m_nCurrentPictureID(0)
    , m_dpbSlotsMask(0)
    , m_fieldPicFlagMask(0)
    , m_dpb(3)
    , m_outOfBandPictureParameters(false)
{
    memset(&m_nvsi, 0, sizeof(m_nvsi));
    for (uint32_t picId = 0; picId < MAX_FRM_CNT; picId++) {
        m_pictureToDpbSlotMap[picId] = -1;
    }
}

VkResult VulkanVideoParser::Initialize(
    IVulkanVideoDecoderHandler* pDecoderHandler,
    IVulkanVideoFrameBufferParserCb* pVideoFrameBuffer,
    bool outOfBandPictureParameters,
    uint32_t errorThreshold)
{
    Deinitialize();

    m_outOfBandPictureParameters = outOfBandPictureParameters;
    m_pDecoderHandler = pDecoderHandler;
    m_pDecoderHandler->AddRef();
    m_pVideoFrameBuffer = pVideoFrameBuffer;
    m_pVideoFrameBuffer->AddRef();

    memset(&m_nvsi, 0, sizeof(m_nvsi));

    VkParserInitDecodeParameters nvdp;

    memset(&nvdp, 0, sizeof(nvdp));
    nvdp.interfaceVersion = NV_VULKAN_VIDEO_PARSER_API_VERSION;
    nvdp.pClient = reinterpret_cast<VkParserVideoDecodeClient*>(this);
    nvdp.lReferenceClockRate = m_clockRate;
    nvdp.lErrorThreshold = errorThreshold;
    nvdp.bOutOfBandPictureParameters = outOfBandPictureParameters;

    m_pParser = NULL;
    if (!CreateVulkanVideoDecodeParser(&m_pParser, m_codecType)) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return m_pParser->Initialize(&nvdp);
}

void VulkanVideoParser::Deinitialize()
{
    if (m_pParser) {
        m_pParser->Deinitialize();
        m_pParser->Release();
        m_pParser = NULL;
    }

    if (m_pDecoderHandler) {
        m_pDecoderHandler->Release();
        m_pDecoderHandler = NULL;
    }

    if (m_pVideoFrameBuffer) {
        m_pVideoFrameBuffer->Release();
        m_pVideoFrameBuffer = NULL;
    }
}

VkResult VulkanVideoParser::ParseVideoData(VkParserSourceDataPacket* pPacket)
{
    VkParserBitstreamPacket pkt;
    VkResult result;

    memset(&pkt, 0, sizeof(pkt));
    if (pPacket->flags & VK_PARSER_PKT_DISCONTINUITY) {
        // Handle discontinuity separately, in order to flush before before any new
        // content
        pkt.bDiscontinuity = true;
        m_pParser->ParseByteStream(&pkt); // Send a NULL discontinuity packet
    }
    pkt.pByteStream = pPacket->payload;
    pkt.nDataLength = pPacket->payload_size;
    pkt.bEOS = !!(pPacket->flags & VK_PARSER_PKT_ENDOFSTREAM);
    pkt.bEOP = !!(pPacket->flags & VK_PARSER_PKT_ENDOFPICTURE);
    pkt.bPTSValid = !!(pPacket->flags & VK_PARSER_PKT_TIMESTAMP);
    pkt.llPTS = pPacket->timestamp;
    if (m_pParser->ParseByteStream(&pkt))
        result = VK_SUCCESS;
    else
        result = VK_ERROR_INITIALIZATION_FAILED;
    if (pkt.bEOS) {
        // Flush any pending frames after EOS
    }
    return result;
}

int8_t VulkanVideoParser::GetPicIdx(vkPicBuffBase* pPicBuf)
{
    if (pPicBuf) {
        int32_t picIndex = pPicBuf->m_picIdx;
        if ((picIndex >= 0) && ((uint32_t)picIndex < m_maxNumDecodeSurfaces)) {
            return (int8_t)picIndex;
        }
    }
    return -1;
}

int8_t VulkanVideoParser::GetPicIdx(VkPicIf* pPicBuf)
{
    return GetPicIdx(GetPic(pPicBuf));
}

int8_t VulkanVideoParser::GetPicDpbSlot(int8_t picIndex)
{
    return m_pictureToDpbSlotMap[picIndex];
}

int8_t VulkanVideoParser::GetPicDpbSlot(vkPicBuffBase* pPicBuf)
{
    int8_t picIndex = GetPicIdx(pPicBuf);
    assert((picIndex >= 0) && ((uint32_t)picIndex < m_maxNumDecodeSurfaces));
    return GetPicDpbSlot(picIndex);
}

bool VulkanVideoParser::GetFieldPicFlag(int8_t picIndex)
{
    assert((picIndex >= 0) && ((uint32_t)picIndex < m_maxNumDecodeSurfaces));
    return (m_fieldPicFlagMask & (1 << (uint32_t)picIndex));
}

bool VulkanVideoParser::SetFieldPicFlag(int8_t picIndex, bool fieldPicFlag)
{
    assert((picIndex >= 0) && ((uint32_t)picIndex < m_maxNumDecodeSurfaces));
    bool oldFieldPicFlag = GetFieldPicFlag(picIndex);

    if (fieldPicFlag) {
        m_fieldPicFlagMask |= (1 << (uint32_t)picIndex);
    } else {
        m_fieldPicFlagMask &= ~(1 << (uint32_t)picIndex);
    }

    return oldFieldPicFlag;
}

int8_t VulkanVideoParser::SetPicDpbSlot(int8_t picIndex, int8_t dpbSlot)
{
    int8_t oldDpbSlot = m_pictureToDpbSlotMap[picIndex];
    m_pictureToDpbSlotMap[picIndex] = dpbSlot;
    if (dpbSlot >= 0) {
        m_dpbSlotsMask |= (1 << picIndex);
    } else {
        m_dpbSlotsMask &= ~(1 << picIndex);
        if (oldDpbSlot >= 0) {
            m_dpb.FreeSlot(oldDpbSlot);
        }
    }
    return oldDpbSlot;
}

int8_t VulkanVideoParser::SetPicDpbSlot(vkPicBuffBase* pPicBuf,
    int8_t dpbSlot)
{
    int8_t picIndex = GetPicIdx(pPicBuf);
    assert((picIndex >= 0) && ((uint32_t)picIndex < m_maxNumDecodeSurfaces));
    return SetPicDpbSlot(picIndex, dpbSlot);
}

uint32_t VulkanVideoParser::ResetPicDpbSlots(uint32_t picIndexSlotValidMask)
{
    uint32_t resetSlotsMask = ~(picIndexSlotValidMask | ~m_dpbSlotsMask);
    if (resetSlotsMask != 0) {
        for (uint32_t picIdx = 0;
             ((picIdx < m_maxNumDecodeSurfaces) && resetSlotsMask); picIdx++) {
            if (resetSlotsMask & (1 << picIdx)) {
                resetSlotsMask &= ~(1 << picIdx);
                SetPicDpbSlot(picIdx, -1);
            }
        }
    }
    return m_dpbSlotsMask;
}

int32_t VulkanVideoParser::BeginSequence(const VkParserSequenceInfo* pnvsi)
{
    // FIXME: reevaluate the below calculations on Tegra.
    const int32_t maxDbpSlots = MAX_DPB_SLOTS_PLUS_1 - ((pnvsi->eCodec == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) ? 0 : EXTRA_DPB_SLOTS);
    int32_t configDpbSlots = maxDbpSlots;
    int32_t configDpbSlotsPlus1 = std::min((uint32_t)(configDpbSlots + 1), MAX_DPB_SLOTS_PLUS_1);

    if ((pnvsi->eCodec != m_nvsi.eCodec) || (pnvsi->nCodedWidth != m_nvsi.nCodedWidth) || (pnvsi->nCodedHeight != m_nvsi.nCodedHeight) || (pnvsi->nChromaFormat != m_nvsi.nChromaFormat) || (pnvsi->bProgSeq != m_nvsi.bProgSeq)) {
    }
    m_nvsi = *pnvsi;
    if (m_pDecoderHandler) {
        VkParserDetectedVideoFormat detectedFormat;
        uint8_t raw_seqhdr_data[1024]; /* Output the sequence header data, currently
                                      not used */

        memset(&detectedFormat, 0, sizeof(detectedFormat));
        detectedFormat.codec = pnvsi->eCodec;
        detectedFormat.frame_rate.numerator = NV_FRAME_RATE_NUM(pnvsi->frameRate);
        detectedFormat.frame_rate.denominator = NV_FRAME_RATE_DEN(pnvsi->frameRate);
        detectedFormat.progressive_sequence = pnvsi->bProgSeq;
        detectedFormat.coded_width = pnvsi->nCodedWidth;
        detectedFormat.coded_height = pnvsi->nCodedHeight;
        detectedFormat.display_area.right = pnvsi->nDisplayWidth;
        detectedFormat.display_area.bottom = pnvsi->nDisplayHeight;

        if ((StdChromaFormatIdc)pnvsi->nChromaFormat == chroma_format_idc_420) {
            detectedFormat.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
        } else if ((StdChromaFormatIdc)pnvsi->nChromaFormat == chroma_format_idc_422) {
            detectedFormat.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR;
        } else if ((StdChromaFormatIdc)pnvsi->nChromaFormat == chroma_format_idc_444) {
            detectedFormat.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR;
        } else {
            assert(!"Invalid chroma sub-sampling format");
        }

        switch (pnvsi->uBitDepthLumaMinus8) {
        case 0:
            detectedFormat.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
            break;
        case 2:
            detectedFormat.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
            break;
        case 4:
            detectedFormat.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;
            break;
        default:
            assert(false);
        }

        switch (pnvsi->uBitDepthChromaMinus8) {
        case 0:
            detectedFormat.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
            break;
        case 2:
            detectedFormat.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
            break;
        case 4:
            detectedFormat.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;
            break;
        default:
            assert(false);
        }

        detectedFormat.bit_depth_luma_minus8 = pnvsi->uBitDepthLumaMinus8;
        detectedFormat.bit_depth_chroma_minus8 = pnvsi->uBitDepthChromaMinus8;
        detectedFormat.bitrate = pnvsi->lBitrate;
        detectedFormat.display_aspect_ratio.x = pnvsi->lDARWidth;
        detectedFormat.display_aspect_ratio.y = pnvsi->lDARHeight;
        detectedFormat.video_signal_description.video_format = pnvsi->lVideoFormat;
        detectedFormat.video_signal_description.video_full_range_flag = pnvsi->uVideoFullRange;
        detectedFormat.video_signal_description.color_primaries = pnvsi->lColorPrimaries;
        detectedFormat.video_signal_description.transfer_characteristics = pnvsi->lTransferCharacteristics;
        detectedFormat.video_signal_description.matrix_coefficients = pnvsi->lMatrixCoefficients;
        detectedFormat.seqhdr_data_length = (uint32_t)std::min((size_t)pnvsi->cbSequenceHeader, sizeof(raw_seqhdr_data));
        detectedFormat.minNumDecodeSurfaces = pnvsi->nMinNumDecodeSurfaces;

        configDpbSlots = (pnvsi->nMinNumDecodeSurfaces > 0)
            ? (pnvsi->nMinNumDecodeSurfaces - (pnvsi->isSVC ? 3 : 1))
            : 0;
        if (configDpbSlots > maxDbpSlots) {
            configDpbSlots = maxDbpSlots;
        }

        configDpbSlotsPlus1 = std::min((uint32_t)(configDpbSlots + 1), MAX_DPB_SLOTS_PLUS_1);
        detectedFormat.maxNumDpbSlots = configDpbSlotsPlus1;

        if (detectedFormat.seqhdr_data_length > 0) {
            memcpy(raw_seqhdr_data, pnvsi->SequenceHeaderData,
                detectedFormat.seqhdr_data_length);
        }
        int maxDecodeRTs = m_pDecoderHandler->StartVideoSequence(&detectedFormat);
        // nDecodeRTs = 0 means SequenceCallback failed
        // nDecodeRTs = 1 means SequenceCallback succeeded
        // nDecodeRTs > 1 means we need to overwrite the MaxNumDecodeSurfaces
        if (!maxDecodeRTs) {
            return 0;
        }
        // MaxNumDecodeSurface may not be correctly calculated by the client while
        // parser creation so overwrite it with NumDecodeSurface. (only if nDecodeRT
        // > 1)
        if (maxDecodeRTs > 1) {
            m_maxNumDecodeSurfaces = maxDecodeRTs;
        }
    } else {
        assert(!"m_pDecoderHandler is NULL");
    }

    // The number of minNumDecodeSurfaces can be overwritten.
    // Add one for the current Dpb setup slot.
    m_maxNumDpbSurfaces = configDpbSlotsPlus1;
    m_dpb.Init(m_maxNumDpbSurfaces);

    // NOTE: Important Tegra parser requires the maxDpbSlotsPlus1 and not
    // dpbSlots.
    return configDpbSlotsPlus1;
}

// FillDpbState
uint32_t VulkanVideoParser::FillDpbH264State(
    const VkParserPictureData* pd, const VkParserH264DpbEntry* dpbIn,
    uint32_t maxDpbInSlotsInUse, nvVideoDecodeH264DpbSlotInfo* pDpbRefList,
    uint32_t maxRefPictures, VkVideoReferenceSlotKHR* pReferenceSlots,
    int8_t* pGopReferenceImagesIndexes,
    StdVideoDecodeH264PictureInfoFlags currPicFlags,
    int8_t* pCurrAllocatedSlotIndex)
{
    // #### Update m_dpb based on dpb parameters ####
    // Create unordered DPB and generate a bitmask of all render targets present
    // in DPB
    uint32_t num_ref_frames = pd->CodecSpecific.h264.pStdSps->max_num_ref_frames;
    assert(num_ref_frames <= m_maxNumDpbSurfaces);
    dpbH264Entry refOnlyDpbIn[VulkanVideoParser::AVC_MAX_DPB_SLOTS]; // max number of Dpb
        // surfaces
    memset(&refOnlyDpbIn, 0, m_maxNumDpbSurfaces * sizeof(refOnlyDpbIn[0]));
    uint32_t refDpbUsedAndValidMask = 0;
    uint32_t numUsedRef = 0;
    for (int32_t inIdx = 0; (uint32_t)inIdx < maxDpbInSlotsInUse; inIdx++) {
        // used_for_reference: 0 = unused, 1 = top_field, 2 = bottom_field, 3 =
        // both_fields
        const uint32_t used_for_reference = dpbIn[inIdx].used_for_reference & fieldIsReferenceMask;
        if (used_for_reference) {
            int8_t picIdx = (!dpbIn[inIdx].not_existing && dpbIn[inIdx].pPicBuf)
                ? GetPicIdx(dpbIn[inIdx].pPicBuf)
                : -1;
            const bool isFieldRef = (picIdx >= 0) ? GetFieldPicFlag(picIdx)
                                                  : (used_for_reference && (used_for_reference != fieldIsReferenceMask));
            const int16_t fieldOrderCntList[2] = {
                (int16_t)dpbIn[inIdx].FieldOrderCnt[0],
                (int16_t)dpbIn[inIdx].FieldOrderCnt[1]
            };
            refOnlyDpbIn[numUsedRef].setReferenceAndTopBoottomField(
                !!used_for_reference,
                (picIdx < 0), /* not_existing is frame inferred by the decoding
                           process for gaps in frame_num */
                !!dpbIn[inIdx].is_long_term, isFieldRef,
                !!(used_for_reference & topFieldMask),
                !!(used_for_reference & bottomFieldMask), dpbIn[inIdx].FrameIdx,
                fieldOrderCntList, GetPic(dpbIn[inIdx].pPicBuf));
            if (picIdx >= 0) {
                refDpbUsedAndValidMask |= (1 << picIdx);
            }
            numUsedRef++;
        }
        // Invalidate all slots.
        pReferenceSlots[inIdx].slotIndex = -1;
        pGopReferenceImagesIndexes[inIdx] = -1;
    }

    if (m_dumpDpbData) {
        std::cout << " =>>> ********************* picIdx: "
                  << (int32_t)GetPicIdx(pd->pCurrPic)
                  << " *************************" << std::endl;
        std::cout << "\tRef frames data in for picIdx: "
                  << (int32_t)GetPicIdx(pd->pCurrPic) << std::endl
                  << "\tSlot Index:\t\t";
        for (uint32_t slot = 0; slot < numUsedRef; slot++) {
            if (!refOnlyDpbIn[slot].is_non_existing) {
                std::cout << slot << ",\t";
            } else {
                std::cout << 'X' << ",\t";
            }
        }
        std::cout << std::endl
                  << "\tPict Index:\t\t";
        for (uint32_t slot = 0; slot < numUsedRef; slot++) {
            if (!refOnlyDpbIn[slot].is_non_existing) {
                std::cout << refOnlyDpbIn[slot].m_picBuff->m_picIdx << ",\t";
            } else {
                std::cout << 'X' << ",\t";
            }
        }
        std::cout << "\n\tTotal Ref frames for picIdx: "
                  << (int32_t)GetPicIdx(pd->pCurrPic) << " : " << numUsedRef
                  << " out of " << num_ref_frames << " MAX(" << m_maxNumDpbSurfaces
                  << ")" << std::endl
                  << std::endl;

        std::cout << std::flush;
    }

    assert(numUsedRef <= 16);

    assert(numUsedRef <= m_maxNumDpbSurfaces);
    assert(numUsedRef <= num_ref_frames);

    // Map all frames not present in DPB as non-reference, and generate a mask of
    // all used DPB entries
    /* uint32_t destUsedDpbMask = */ ResetPicDpbSlots(refDpbUsedAndValidMask);

    // Now, map DPB render target indices to internal frame buffer index,
    // assign each reference a unique DPB entry, and create the ordered DPB
    // This is an undocumented MV restriction: the position in the DPB is stored
    // along with the co-located data, so once a reference frame is assigned a DPB
    // entry, it can no longer change.

    // Find or allocate slots for existing dpb items.
    // Take into account the reference picture now.
    int8_t currPicIdx = GetPicIdx(pd->pCurrPic);
    assert(currPicIdx >= 0);
    int8_t bestNonExistingPicIdx = currPicIdx;
    if (refDpbUsedAndValidMask) {
        int32_t minFrameNumDiff = 0x10000;
        for (int32_t dpbIdx = 0; (uint32_t)dpbIdx < numUsedRef; dpbIdx++) {
            if (!refOnlyDpbIn[dpbIdx].is_non_existing) {
                vkPicBuffBase* picBuff = refOnlyDpbIn[dpbIdx].m_picBuff;
                int8_t picIdx = GetPicIdx(picBuff); // should always be valid at this point
                assert(picIdx >= 0);
                // We have up to 17 internal frame buffers, but only MAX_DPB_SIZE dpb
                // entries, so we need to re-map the index from the [0..MAX_DPB_SIZE]
                // range to [0..15]
                int8_t dpbSlot = GetPicDpbSlot(picIdx);
                if (dpbSlot < 0) {
                    dpbSlot = m_dpb.AllocateSlot();
                    assert((dpbSlot >= 0) && ((uint32_t)dpbSlot < m_maxNumDpbSurfaces));
                    SetPicDpbSlot(picIdx, dpbSlot);
                    m_dpb[dpbSlot].setPictureResource(picBuff, m_nCurrentPictureID);
                }
                m_dpb[dpbSlot].MarkInUse(m_nCurrentPictureID);
                assert(dpbSlot >= 0);

                if (dpbSlot >= 0) {
                    refOnlyDpbIn[dpbIdx].dpbSlot = dpbSlot;
                } else {
                    // This should never happen
                    printf("DPB mapping logic broken!\n");
                    assert(0);
                }

                int32_t frameNumDiff = ((int32_t)pd->CodecSpecific.h264.frame_num - refOnlyDpbIn[dpbIdx].FrameIdx);
                if (frameNumDiff <= 0) {
                    frameNumDiff = 0xffff;
                }
                if (frameNumDiff < minFrameNumDiff) {
                    bestNonExistingPicIdx = picIdx;
                    minFrameNumDiff = frameNumDiff;
                } else if (bestNonExistingPicIdx == currPicIdx) {
                    bestNonExistingPicIdx = picIdx;
                }
            }
        }
    }
    // In Vulkan, we always allocate a Dbp slot for the current picture,
    // regardless if it is going to become a reference or not. Non-reference slots
    // get freed right after usage. if (pd->ref_pic_flag) {
    int8_t currPicDpbSlot = AllocateDpbSlotForCurrentH264(GetPic(pd->pCurrPic), currPicFlags);
    assert(currPicDpbSlot >= 0);
    *pCurrAllocatedSlotIndex = currPicDpbSlot;

    if (refDpbUsedAndValidMask) {
        // Find or allocate slots for non existing dpb items and populate the slots.
        uint32_t dpbInUseMask = m_dpb.getSlotInUseMask();
        int8_t firstNonExistingDpbSlot = 0;
        for (uint32_t dpbIdx = 0; dpbIdx < numUsedRef; dpbIdx++) {
            int8_t dpbSlot = -1;
            int8_t picIdx = -1;
            if (refOnlyDpbIn[dpbIdx].is_non_existing) {
                assert(refOnlyDpbIn[dpbIdx].m_picBuff == NULL);
                while (((uint32_t)firstNonExistingDpbSlot < m_maxNumDpbSurfaces) && (dpbSlot == -1)) {
                    if (!(dpbInUseMask & (1 << firstNonExistingDpbSlot))) {
                        dpbSlot = firstNonExistingDpbSlot;
                    }
                    firstNonExistingDpbSlot++;
                }
                assert((dpbSlot >= 0) && ((uint32_t)dpbSlot < m_maxNumDpbSurfaces));
                picIdx = bestNonExistingPicIdx;
                // Find the closest valid refpic already in the DPB
                uint32_t minDiffPOC = 0x7fff;
                for (uint32_t j = 0; j < numUsedRef; j++) {
                    if (!refOnlyDpbIn[j].is_non_existing && (refOnlyDpbIn[j].used_for_reference & refOnlyDpbIn[dpbIdx].used_for_reference) == refOnlyDpbIn[dpbIdx].used_for_reference) {
                        uint32_t diffPOC = abs((int32_t)(refOnlyDpbIn[j].FieldOrderCnt[0] - refOnlyDpbIn[dpbIdx].FieldOrderCnt[0]));
                        if (diffPOC <= minDiffPOC) {
                            minDiffPOC = diffPOC;
                            picIdx = GetPicIdx(refOnlyDpbIn[j].m_picBuff);
                        }
                    }
                }
            } else {
                assert(refOnlyDpbIn[dpbIdx].m_picBuff != NULL);
                dpbSlot = refOnlyDpbIn[dpbIdx].dpbSlot;
                picIdx = GetPicIdx(refOnlyDpbIn[dpbIdx].m_picBuff);
            }
            assert((dpbSlot >= 0) && ((uint32_t)dpbSlot < m_maxNumDpbSurfaces));
            refOnlyDpbIn[dpbIdx].setH264PictureData(pDpbRefList, pReferenceSlots,
                dpbIdx, dpbSlot);
            pGopReferenceImagesIndexes[dpbIdx] = picIdx;
        }
    }

    if (m_dumpDpbData) {
        uint32_t slotInUseMask = m_dpb.getSlotInUseMask();
        uint32_t slotsInUseCount = 0;
        std::cout << "\tAllocated Ref slot " << (int32_t)currPicDpbSlot << " for "
                  << (pd->ref_pic_flag ? "REFERENCE" : "NON-REFERENCE")
                  << " picIdx: " << (int32_t)currPicIdx << std::endl;
        std::cout << "\tRef frames map for picIdx: " << (int32_t)currPicIdx
                  << std::endl
                  << "\tSlot Index:\t\t";
        for (uint32_t slot = 0; slot < m_dpb.getMaxSize(); slot++) {
            if (slotInUseMask & (1 << slot)) {
                std::cout << slot << ",\t";
                slotsInUseCount++;
            } else {
                std::cout << 'X' << ",\t";
            }
        }
        std::cout << std::endl
                  << "\tPict Index:\t\t";
        for (uint32_t slot = 0; slot < m_dpb.getMaxSize(); slot++) {
            if (slotInUseMask & (1 << slot)) {
                if (m_dpb[slot].getPictureResource()) {
                    std::cout << m_dpb[slot].getPictureResource()->m_picIdx << ",\t";
                } else {
                    std::cout << "non existent"
                              << ",\t";
                }
            } else {
                std::cout << 'X' << ",\t";
            }
        }
        std::cout << "\n\tTotal slots in use for picIdx: " << (int32_t)currPicIdx
                  << " : " << slotsInUseCount << " out of " << m_dpb.getMaxSize()
                  << std::endl;
        std::cout << " <<<= ********************* picIdx: "
                  << (int32_t)GetPicIdx(pd->pCurrPic)
                  << " *************************" << std::endl
                  << std::endl;
        std::cout << std::flush;
    }
    return refDpbUsedAndValidMask ? numUsedRef : 0;
}

uint32_t VulkanVideoParser::FillDpbH265State(
    const VkParserPictureData* pd, const VkParserHevcPictureData* pin,
    nvVideoDecodeH265DpbSlotInfo* pDpbSlotInfo,
    StdVideoDecodeH265PictureInfo* pStdPictureInfo, uint32_t maxRefPictures,
    VkVideoReferenceSlotKHR* pReferenceSlots,
    int8_t* pGopReferenceImagesIndexes)
{
    // #### Update m_dpb based on dpb parameters ####
    // Create unordered DPB and generate a bitmask of all render targets present
    // in DPB
    dpbH264Entry refOnlyDpbIn[VulkanVideoParser::AVC_MAX_DPB_SLOTS];
    memset(&refOnlyDpbIn, 0, m_maxNumDpbSurfaces * sizeof(refOnlyDpbIn[0]));
    uint32_t refDpbUsedAndValidMask = 0;
    uint32_t numUsedRef = 0;
    if (m_dumpParserData)
        std::cout << "Ref frames data: " << std::endl;
    for (int32_t inIdx = 0; inIdx < HEVC_MAX_DPB_SLOTS; inIdx++) {
        // used_for_reference: 0 = unused, 1 = top_field, 2 = bottom_field, 3 =
        // both_fields
        int8_t picIdx = GetPicIdx(pin->RefPics[inIdx]);
        if (picIdx >= 0) {
            refOnlyDpbIn[numUsedRef].setReference((pin->IsLongTerm[inIdx] == 1),
                pin->PicOrderCntVal[inIdx],
                GetPic(pin->RefPics[inIdx]));
            if (picIdx >= 0) {
                refDpbUsedAndValidMask |= (1 << picIdx);
            }
            refOnlyDpbIn[numUsedRef].originalDpbIndex = inIdx;
            numUsedRef++;
        }
        // Invalidate all slots.
        pReferenceSlots[inIdx].slotIndex = -1;
        pGopReferenceImagesIndexes[inIdx] = -1;
    }

    if (m_dumpParserData)
        std::cout << "Total Ref frames: " << numUsedRef << std::endl;

    assert(numUsedRef <= m_maxNumDpbSurfaces);

    // Take into account the reference picture now.
    int8_t currPicIdx = GetPicIdx(pd->pCurrPic);
    int8_t currPicDpbSlot = -1;
    assert(currPicIdx >= 0);
    if (currPicIdx >= 0) {
        currPicDpbSlot = GetPicDpbSlot(currPicIdx);
        refDpbUsedAndValidMask |= (1 << currPicIdx);
    }
    assert(currPicDpbSlot >= 0);

    // Map all frames not present in DPB as non-reference, and generate a mask of
    // all used DPB entries
    /* uint32_t destUsedDpbMask = */ ResetPicDpbSlots(refDpbUsedAndValidMask);

    // Now, map DPB render target indices to internal frame buffer index,
    // assign each reference a unique DPB entry, and create the ordered DPB
    // This is an undocumented MV restriction: the position in the DPB is stored
    // along with the co-located data, so once a reference frame is assigned a DPB
    // entry, it can no longer change.

    int8_t frmListToDpb[HEVC_MAX_DPB_SLOTS];
    // TODO change to -1 for invalid indexes.
    memset(&frmListToDpb, 0, sizeof(frmListToDpb));
    // Find or allocate slots for existing dpb items.
    for (int32_t dpbIdx = 0; (uint32_t)dpbIdx < numUsedRef; dpbIdx++) {
        if (!refOnlyDpbIn[dpbIdx].is_non_existing) {
            vkPicBuffBase* picBuff = refOnlyDpbIn[dpbIdx].m_picBuff;
            int32_t picIdx = GetPicIdx(picBuff); // should always be valid at this point
            assert(picIdx >= 0);
            // We have up to 17 internal frame buffers, but only HEVC_MAX_DPB_SLOTS
            // dpb entries, so we need to re-map the index from the
            // [0..HEVC_MAX_DPB_SLOTS] range to [0..15]
            int8_t dpbSlot = GetPicDpbSlot(picIdx);
            if (dpbSlot < 0) {
                dpbSlot = m_dpb.AllocateSlot();
                assert(dpbSlot >= 0);
                SetPicDpbSlot(picIdx, dpbSlot);
                m_dpb[dpbSlot].setPictureResource(picBuff, m_nCurrentPictureID);
            }
            m_dpb[dpbSlot].MarkInUse(m_nCurrentPictureID);
            assert(dpbSlot >= 0);

            if (dpbSlot >= 0) {
                refOnlyDpbIn[dpbIdx].dpbSlot = dpbSlot;
                uint32_t originalDpbIndex = refOnlyDpbIn[dpbIdx].originalDpbIndex;
                assert(originalDpbIndex < HEVC_MAX_DPB_SLOTS);
                frmListToDpb[originalDpbIndex] = dpbSlot;
            } else {
                // This should never happen
                printf("DPB mapping logic broken!\n");
                assert(0);
            }
        }
    }

    // Find or allocate slots for non existing dpb items and populate the slots.
    uint32_t dpbInUseMask = m_dpb.getSlotInUseMask();
    int8_t firstNonExistingDpbSlot = 0;
    for (uint32_t dpbIdx = 0; dpbIdx < numUsedRef; dpbIdx++) {
        int8_t dpbSlot = -1;
        if (refOnlyDpbIn[dpbIdx].is_non_existing) {
            // There shouldn't be  not_existing in h.265
            assert(0);
            assert(refOnlyDpbIn[dpbIdx].m_picBuff == NULL);
            while (((uint32_t)firstNonExistingDpbSlot < m_maxNumDpbSurfaces) && (dpbSlot == -1)) {
                if (!(dpbInUseMask & (1 << firstNonExistingDpbSlot))) {
                    dpbSlot = firstNonExistingDpbSlot;
                }
                firstNonExistingDpbSlot++;
            }
            assert((dpbSlot >= 0) && ((uint32_t)dpbSlot < m_maxNumDpbSurfaces));
        } else {
            assert(refOnlyDpbIn[dpbIdx].m_picBuff != NULL);
            dpbSlot = refOnlyDpbIn[dpbIdx].dpbSlot;
        }
        assert((dpbSlot >= 0) && (dpbSlot < HEVC_MAX_DPB_SLOTS));
        refOnlyDpbIn[dpbIdx].setH265PictureData(pDpbSlotInfo, pReferenceSlots,
            dpbIdx, dpbSlot);
        pGopReferenceImagesIndexes[dpbIdx] = GetPicIdx(refOnlyDpbIn[dpbIdx].m_picBuff);
    }

    if (m_dumpParserData) {
        std::cout << "frmListToDpb:" << std::endl;
        for (int8_t dpbResIdx = 0; dpbResIdx < HEVC_MAX_DPB_SLOTS; dpbResIdx++) {
            std::cout << "\tfrmListToDpb[" << (int32_t)dpbResIdx << "] is "
                      << (int32_t)frmListToDpb[dpbResIdx] << std::endl;
        }
    }

    int32_t numPocTotalCurr = 0;
    int32_t numPocStCurrBefore = 0;
    const size_t maxNumPocStCurrBefore = sizeof(pStdPictureInfo->RefPicSetStCurrBefore) / sizeof(pStdPictureInfo->RefPicSetStCurrBefore[0]);
    assert((size_t)pin->NumPocStCurrBefore < maxNumPocStCurrBefore);
    for (int32_t i = 0; i < pin->NumPocStCurrBefore; i++) {
        uint8_t idx = (uint8_t)pin->RefPicSetStCurrBefore[i];
        if (idx < HEVC_MAX_DPB_SLOTS) {
            if (m_dumpParserData)
                std::cout << "\trefPicSetStCurrBefore[" << i << "] is " << (int32_t)idx
                          << " -> " << (int32_t)frmListToDpb[idx] << std::endl;
            pStdPictureInfo->RefPicSetStCurrBefore[numPocStCurrBefore++] = frmListToDpb[idx] & 0xf;
            numPocTotalCurr++;
        }
    }
    while (numPocStCurrBefore < 8) {
        pStdPictureInfo->RefPicSetStCurrBefore[numPocStCurrBefore++] = 0xff;
    }

    int32_t numPocStCurrAfter = 0;
    const size_t maxNumPocStCurrAfter = sizeof(pStdPictureInfo->RefPicSetStCurrAfter) / sizeof(pStdPictureInfo->RefPicSetStCurrAfter[0]);
    assert((size_t)pin->NumPocStCurrAfter < maxNumPocStCurrAfter);
    for (int32_t i = 0; i < pin->NumPocStCurrAfter; i++) {
        uint8_t idx = (uint8_t)pin->RefPicSetStCurrAfter[i];
        if (idx < HEVC_MAX_DPB_SLOTS) {
            if (m_dumpParserData)
                std::cout << "\trefPicSetStCurrAfter[" << i << "] is " << (int32_t)idx
                          << " -> " << (int32_t)frmListToDpb[idx] << std::endl;
            pStdPictureInfo->RefPicSetStCurrAfter[numPocStCurrAfter++] = frmListToDpb[idx] & 0xf;
            numPocTotalCurr++;
        }
    }
    while (numPocStCurrAfter < 8) {
        pStdPictureInfo->RefPicSetStCurrAfter[numPocStCurrAfter++] = 0xff;
    }

    int32_t numPocLtCurr = 0;
    const size_t maxNumPocLtCurr = sizeof(pStdPictureInfo->RefPicSetLtCurr) / sizeof(pStdPictureInfo->RefPicSetLtCurr[0]);
    assert((size_t)pin->NumPocLtCurr < maxNumPocLtCurr);
    for (int32_t i = 0; i < pin->NumPocLtCurr; i++) {
        uint8_t idx = (uint8_t)pin->RefPicSetLtCurr[i];
        if (idx < HEVC_MAX_DPB_SLOTS) {
            if (m_dumpParserData)
                std::cout << "\trefPicSetLtCurr[" << i << "] is " << (int32_t)idx
                          << " -> " << (int32_t)frmListToDpb[idx] << std::endl;
            pStdPictureInfo->RefPicSetLtCurr[numPocLtCurr++] = frmListToDpb[idx] & 0xf;
            numPocTotalCurr++;
        }
    }
    while (numPocLtCurr < 8) {
        pStdPictureInfo->RefPicSetLtCurr[numPocLtCurr++] = 0xff;
    }

    for (int32_t i = 0; i < 8; i++) {
        if (m_dumpParserData)
            std::cout << "\tlist indx " << i << ": "
                      << " refPicSetStCurrBefore: "
                      << (int32_t)pStdPictureInfo->RefPicSetStCurrBefore[i]
                      << " refPicSetStCurrAfter: "
                      << (int32_t)pStdPictureInfo->RefPicSetStCurrAfter[i]
                      << " refPicSetLtCurr: "
                      << (int32_t)pStdPictureInfo->RefPicSetLtCurr[i] << std::endl;
    }

    return numUsedRef;
}

int8_t VulkanVideoParser::AllocateDpbSlotForCurrentH264(
    vkPicBuffBase* pPic, StdVideoDecodeH264PictureInfoFlags currPicFlags)
{
    // Now, map the current render target
    int8_t dpbSlot = -1;
    int8_t currPicIdx = GetPicIdx(pPic);
    assert(currPicIdx >= 0);
    SetFieldPicFlag(currPicIdx, currPicFlags.field_pic_flag);
    // In Vulkan we always allocate reference slot for the current picture.
    if (true /* currPicFlags.is_reference */) {
        dpbSlot = GetPicDpbSlot(currPicIdx);
        if (dpbSlot < 0) {
            dpbSlot = m_dpb.AllocateSlot();
            assert(dpbSlot >= 0);
            SetPicDpbSlot(currPicIdx, dpbSlot);
            m_dpb[dpbSlot].setPictureResource(pPic, m_nCurrentPictureID);
        }
        assert(dpbSlot >= 0);
    }
    return dpbSlot;
}

int8_t VulkanVideoParser::AllocateDpbSlotForCurrentH265(vkPicBuffBase* pPic,
    bool isReference)
{
    // Now, map the current render target
    int8_t dpbSlot = -1;
    int8_t currPicIdx = GetPicIdx(pPic);
    assert(currPicIdx >= 0);
    assert(isReference);
    if (isReference) {
        dpbSlot = GetPicDpbSlot(currPicIdx);
        if (dpbSlot < 0) {
            dpbSlot = m_dpb.AllocateSlot();
            assert(dpbSlot >= 0);
            SetPicDpbSlot(currPicIdx, dpbSlot);
            m_dpb[dpbSlot].setPictureResource(pPic, m_nCurrentPictureID);
        }
        assert(dpbSlot >= 0);
    }
    return dpbSlot;
}

static const char* PictureParametersTypeToName(VkParserPictureParametersUpdateType updateType)
{
    switch (updateType)
    {
    case VK_PICTURE_PARAMETERS_UPDATE_H264_SPS:
        return "H264_SPS";
    case VK_PICTURE_PARAMETERS_UPDATE_H264_PPS:
        return "H264_PPS";
    case VK_PICTURE_PARAMETERS_UPDATE_H265_VPS:
        return "H265_VPS";
    case VK_PICTURE_PARAMETERS_UPDATE_H265_SPS:
        return "H265_SPS";
    case VK_PICTURE_PARAMETERS_UPDATE_H265_PPS:
        return "H265_PPS";
    }
    return "unknown";
}

bool VulkanVideoParser::UpdatePictureParameters(
    VkPictureParameters* pPictureParameters,
    VkSharedBaseObj<VkParserVideoRefCountBase>& pictureParametersObject,
    uint64_t updateSequenceCount)
{
    if (false) {
        std::cout << "################################################# " << std::endl;
        std::cout << "Update Picture parameters "
                << PictureParametersTypeToName(pPictureParameters->updateType) << ": "
                << pPictureParameters << ", pUpdateParameters: "
                << pictureParametersObject.Get()
                << ", count: " << (uint32_t)updateSequenceCount
                << std::endl;
        std::cout << "################################################# " << std::endl;
    }

    if (m_pDecoderHandler == NULL) {
        assert(!"m_pDecoderHandler is NULL");
        return NULL;
    }

    if (pPictureParameters != NULL) {
        return m_pDecoderHandler->UpdatePictureParameters(pPictureParameters, pictureParametersObject, updateSequenceCount);
    }

    return NULL;
}

bool VulkanVideoParser::DecodePicture(
    VkParserPictureData* pd, vkPicBuffBase* pVkPicBuff,
    VkParserDecodePictureInfo* pDecodePictureInfo)
{
    bool bRet = false;

    // union {
    nvVideoH264PicParameters h264;
    nvVideoH265PicParameters hevc;
    // };

    if (m_pDecoderHandler == NULL) {
        assert(!"m_pDecoderHandler is NULL");
        return false;
    }

    if (!pd->pCurrPic) {
        return false;
    }

    const uint32_t PicIdx = GetPicIdx(pd->pCurrPic);
    if (PicIdx >= MAX_FRM_CNT) {
        assert(0);
        return false;
    }

    const uint32_t maxRefPictures = VkParserPerFrameDecodeParameters::MAX_DPB_REF_SLOTS; // max 16 reference pictures plus 1 for the current picture.
    VkParserPerFrameDecodeParameters pictureParams;
    memset(&pictureParams, 0, sizeof(pictureParams));

    VkParserPerFrameDecodeParameters* pPerFrameDecodeParameters = &pictureParams;
    pPerFrameDecodeParameters->currPicIdx = PicIdx;
    pPerFrameDecodeParameters->bitstreamDataLen = pd->nBitstreamDataLen;
    pPerFrameDecodeParameters->pBitstreamData = pd->pBitstreamData;

    VkVideoReferenceSlotKHR
        referenceSlots[VkParserPerFrameDecodeParameters::MAX_DPB_REF_SLOTS];
    VkVideoReferenceSlotKHR setupReferenceSlot = {
        VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_KHR, NULL,
        -1, // slotIndex
        NULL // pPictureResource
    };
    // TODO: not exactly true.
    pPerFrameDecodeParameters->decodeFrameInfo.codedExtent.width = pd->PicWidthInMbs << 4;
    pPerFrameDecodeParameters->decodeFrameInfo.codedExtent.height = pd->FrameHeightInMbs << 4;

    pPerFrameDecodeParameters->decodeFrameInfo.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
    pPerFrameDecodeParameters->decodeFrameInfo.dstPictureResource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_KHR;

    if (m_codecType == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) {
        const VkParserH264PictureData* const pin = &pd->CodecSpecific.h264;

        h264 = nvVideoH264PicParameters();

        nvVideoH264PicParameters* const pout = &h264;
        VkVideoDecodeH264PictureInfoEXT* pPictureInfo = &h264.pictureInfo;
        nvVideoDecodeH264DpbSlotInfo* pDpbRefList = h264.dpbRefList;
        StdVideoDecodeH264PictureInfo* pStdPictureInfo = &h264.stdPictureInfo;

        pPerFrameDecodeParameters->pCurrentPictureParameters = StdVideoPictureParametersSet::StdVideoPictureParametersSetFromBase(pin->pPpsClientObject);
        if (false) {
            std::cout << "\n\tCurrent h.264 Picture SPS update : "
                    << StdVideoPictureParametersSet::StdVideoPictureParametersSetFromBase(pin->pSpsClientObject)->m_updateSequenceCount << std::endl;
            std::cout << "\tCurrent h.264 Picture PPS update : "
                    << StdVideoPictureParametersSet::StdVideoPictureParametersSetFromBase(pin->pPpsClientObject)->m_updateSequenceCount << std::endl;
        }

        pDecodePictureInfo->videoFrameType = 0; // pd->CodecSpecific.h264.slice_type;
        // FIXME: If mvcext is enabled.
        pDecodePictureInfo->viewId = pd->CodecSpecific.h264.mvcext.view_id;

        pPictureInfo->pStdPictureInfo = &h264.stdPictureInfo;

        pPictureInfo->sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_EXT;

        if (!m_outOfBandPictureParameters) {
            // In-band h264 Picture Parameters for testing
            h264.pictureParameters.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_EXT;
            h264.pictureParameters.pSpsStd = pin->pStdSps;
            h264.pictureParameters.pPpsStd = pin->pStdPps;
            pPictureInfo->pNext = &h264.pictureParameters;
        } else {
            pPictureInfo->pNext = nullptr;
        }

        pPerFrameDecodeParameters->decodeFrameInfo.pNext = &h264.pictureInfo;

        pStdPictureInfo->pic_parameter_set_id = pin->pic_parameter_set_id; // PPS ID
        pStdPictureInfo->seq_parameter_set_id = pin->seq_parameter_set_id; // SPS ID;

        pStdPictureInfo->frame_num = (uint16_t)pin->frame_num;
        pPictureInfo->slicesCount = pd->nNumSlices;
        pPictureInfo->pSlicesDataOffsets = pd->pSliceDataOffsets;

        h264.mvcInfo.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_MVC_EXT;
        h264.mvcInfo.pNext = NULL; // No more extension structures.

        StdVideoDecodeH264PictureInfoFlags currPicFlags = StdVideoDecodeH264PictureInfoFlags();
        // 0 = frame picture, 1 = field picture
        if (pd->field_pic_flag) {
            // 0 = top field, 1 = bottom field (ignored if field_pic_flag = 0)
            currPicFlags.field_pic_flag = true;
            if (pd->bottom_field_flag) {
                currPicFlags.bottom_field_flag = true;
            }
        }
        // Second field of a complementary field pair
        if (pd->second_field) {
            currPicFlags.complementary_field_pair = true;
        }
        // Frame is a reference frame
        if (pd->ref_pic_flag) {
            currPicFlags.is_reference = true;
        }
        pStdPictureInfo->flags = currPicFlags;
        if (!pd->field_pic_flag) {
            pStdPictureInfo->PicOrderCnt[0] = pin->CurrFieldOrderCnt[0];
            pStdPictureInfo->PicOrderCnt[1] = pin->CurrFieldOrderCnt[1];
        } else {
            pStdPictureInfo->PicOrderCnt[pd->bottom_field_flag] = pin->CurrFieldOrderCnt[pd->bottom_field_flag];
        }

        const uint32_t maxDpbInputSlots = sizeof(pin->dpb) / sizeof(pin->dpb[0]);
        pPerFrameDecodeParameters->numGopReferenceSlots = FillDpbH264State(
            pd, pin->dpb, maxDpbInputSlots, pDpbRefList, maxRefPictures,
            referenceSlots, pPerFrameDecodeParameters->pGopReferenceImagesIndexes,
            h264.stdPictureInfo.flags, &setupReferenceSlot.slotIndex);
        // TODO: Remove it is for debugging only.
        pout->stdPictureInfo.reserved = pPerFrameDecodeParameters->numGopReferenceSlots;
        // slotLayer requires NVIDIA specific extension VK_KHR_video_layers, not
        // enabled, just yet. setupReferenceSlot.slotLayerIndex = 0;
        assert(!pd->ref_pic_flag || (setupReferenceSlot.slotIndex >= 0));
        if (setupReferenceSlot.slotIndex >= 0) {
            setupReferenceSlot.pPictureResource = &pPerFrameDecodeParameters->decodeFrameInfo.dstPictureResource;
            pPerFrameDecodeParameters->decodeFrameInfo.pSetupReferenceSlot = &setupReferenceSlot;
        }
        if (pPerFrameDecodeParameters->numGopReferenceSlots) {
            for (uint32_t dpbEntryIdx = 0; dpbEntryIdx < (uint32_t)pPerFrameDecodeParameters->numGopReferenceSlots;
                 dpbEntryIdx++) {
                pPerFrameDecodeParameters->pictureResources[dpbEntryIdx].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_KHR;
                referenceSlots[dpbEntryIdx].pPictureResource = &pPerFrameDecodeParameters->pictureResources[dpbEntryIdx];
                assert(pDpbRefList[dpbEntryIdx].IsReference());
                // referenceSlots[dpbEntryIdx].pNext =
                // &pDpbRefList[dpbEntryIdx].dpbSlotInfo;
            }

            pPerFrameDecodeParameters->decodeFrameInfo.pReferenceSlots = referenceSlots;
            pPerFrameDecodeParameters->decodeFrameInfo.referenceSlotCount = pPerFrameDecodeParameters->numGopReferenceSlots;
        } else {
            pPerFrameDecodeParameters->decodeFrameInfo.pReferenceSlots = NULL;
            pPerFrameDecodeParameters->decodeFrameInfo.referenceSlotCount = 0;
        }

    } else if (m_codecType == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT) {
        const VkParserHevcPictureData* const pin = &pd->CodecSpecific.hevc;
        hevc = nvVideoH265PicParameters();
        VkVideoDecodeH265PictureInfoEXT* pPictureInfo = &hevc.pictureInfo;
        StdVideoDecodeH265PictureInfo* pStdPictureInfo = &hevc.stdPictureInfo;
        nvVideoDecodeH265DpbSlotInfo* pDpbRefList = hevc.dpbRefList;

        pPerFrameDecodeParameters->pCurrentPictureParameters = StdVideoPictureParametersSet::StdVideoPictureParametersSetFromBase(pin->pPpsClientObject);
        if (false) {
            std::cout << "\n\tCurrent h.265 Picture SPS update : "
                    << StdVideoPictureParametersSet::StdVideoPictureParametersSetFromBase(pin->pSpsClientObject)->m_updateSequenceCount << std::endl;
            std::cout << "\tCurrent h.265 Picture PPS update : "
                    << StdVideoPictureParametersSet::StdVideoPictureParametersSetFromBase(pin->pPpsClientObject)->m_updateSequenceCount << std::endl;
        }


        pPictureInfo->sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_EXT;

        if (!m_outOfBandPictureParameters) {
            // In-band h264 Picture Parameters for testing
            hevc.pictureParameters.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_EXT;
            hevc.pictureParameters.pSpsStd = pin->pStdSps;
            hevc.pictureParameters.pPpsStd = pin->pStdPps;
            pPictureInfo->pNext = &hevc.pictureParameters;
        } else {
            pPictureInfo->pNext = nullptr;
        }

        pPictureInfo->pStdPictureInfo = &hevc.stdPictureInfo;
        pPerFrameDecodeParameters->decodeFrameInfo.pNext = &hevc.pictureInfo;

        pDecodePictureInfo->videoFrameType = 0; // pd->CodecSpecific.hevc.SliceType;
        if (pd->CodecSpecific.hevc.mv_hevc_enable) {
            pDecodePictureInfo->viewId = pd->CodecSpecific.hevc.nuh_layer_id;
        } else {
            pDecodePictureInfo->viewId = 0;
        }

        pPictureInfo->slicesCount = pd->nNumSlices;
        pPictureInfo->pSlicesDataOffsets = pd->pSliceDataOffsets;

        pStdPictureInfo->pps_pic_parameter_set_id   = pin->pic_parameter_set_id;       // PPS ID
        pStdPictureInfo->sps_seq_parameter_set_id   = pin->seq_parameter_set_id;       // SPS ID
        pStdPictureInfo->vps_video_parameter_set_id = pin->vps_video_parameter_set_id; // VPS ID

        // hevc->irapPicFlag = m_slh.nal_unit_type >= NUT_BLA_W_LP &&
        // m_slh.nal_unit_type <= NUT_CRA_NUT;
        pStdPictureInfo->flags.IrapPicFlag = pin->IrapPicFlag; // Intra Random Access Point for current picture.
        // hevc->idrPicFlag = m_slh.nal_unit_type == NUT_IDR_W_RADL ||
        // m_slh.nal_unit_type == NUT_IDR_N_LP;
        pStdPictureInfo->flags.IdrPicFlag = pin->IdrPicFlag; // Instantaneous Decoding Refresh for current picture.

        // NumBitsForShortTermRPSInSlice = s->sh.short_term_rps ?
        // s->sh.short_term_ref_pic_set_size : 0
        pStdPictureInfo->NumBitsForSTRefPicSetInSlice = pin->NumBitsForShortTermRPSInSlice;

        // NumDeltaPocsOfRefRpsIdx = s->sh.short_term_rps ?
        // s->sh.short_term_rps->rps_idx_num_delta_pocs : 0
        pStdPictureInfo->NumDeltaPocsOfRefRpsIdx = pin->NumDeltaPocsOfRefRpsIdx;
        pStdPictureInfo->PicOrderCntVal = pin->CurrPicOrderCntVal;

        int8_t dpbSlot = AllocateDpbSlotForCurrentH265(GetPic(pd->pCurrPic),
            true /* isReference */);
        setupReferenceSlot.slotIndex = dpbSlot;
        // slotLayer requires NVIDIA specific extension VK_KHR_video_layers, not
        // enabled, just yet. setupReferenceSlot.slotLayerIndex = 0;
        assert(!(dpbSlot < 0));
        if (dpbSlot >= 0) {
            assert(pd->ref_pic_flag);
        }

        if (m_dumpParserData)
            std::cout << "\tnumPocStCurrBefore: " << (int32_t)pin->NumPocStCurrBefore
                      << " numPocStCurrAfter: " << (int32_t)pin->NumPocStCurrAfter
                      << " numPocLtCurr: " << (int32_t)pin->NumPocLtCurr << std::endl;

        pPerFrameDecodeParameters->numGopReferenceSlots = FillDpbH265State(pd, pin, pDpbRefList, pStdPictureInfo, maxRefPictures,
            referenceSlots, pPerFrameDecodeParameters->pGopReferenceImagesIndexes);

        assert(!pd->ref_pic_flag || (setupReferenceSlot.slotIndex >= 0));
        if (setupReferenceSlot.slotIndex >= 0) {
            setupReferenceSlot.pPictureResource = &pPerFrameDecodeParameters->decodeFrameInfo.dstPictureResource;
            pPerFrameDecodeParameters->decodeFrameInfo.pSetupReferenceSlot = &setupReferenceSlot;
        }

        if (pPerFrameDecodeParameters->numGopReferenceSlots) {
            for (uint32_t dpbEntryIdx = 0; dpbEntryIdx < (uint32_t)pPerFrameDecodeParameters->numGopReferenceSlots;
                 dpbEntryIdx++) {
                pPerFrameDecodeParameters->pictureResources[dpbEntryIdx].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_KHR;
                referenceSlots[dpbEntryIdx].pPictureResource = &pPerFrameDecodeParameters->pictureResources[dpbEntryIdx];
                assert(pDpbRefList[dpbEntryIdx].IsReference());
            }

            pPerFrameDecodeParameters->decodeFrameInfo.pReferenceSlots = referenceSlots;
            pPerFrameDecodeParameters->decodeFrameInfo.referenceSlotCount = pPerFrameDecodeParameters->numGopReferenceSlots;
        } else {
            pPerFrameDecodeParameters->decodeFrameInfo.pReferenceSlots = NULL;
            pPerFrameDecodeParameters->decodeFrameInfo.referenceSlotCount = 0;
        }

        if (m_dumpParserData) {
            for (int32_t i = 0; i < HEVC_MAX_DPB_SLOTS; i++) {
                std::cout << "\tdpbIndex: " << i;
                if (pDpbRefList[i]) {
                    std::cout << " REFERENCE FRAME";

                    std::cout << " picOrderCntValList: "
                              << (int32_t)pDpbRefList[i]
                                     .dpbSlotInfo.pStdReferenceInfo->PicOrderCntVal;

                    std::cout << "\t\t Flags: ";
                    if (pDpbRefList[i]
                            .dpbSlotInfo.pStdReferenceInfo->flags.is_long_term) {
                        std::cout << "IS LONG TERM ";
                    }

                } else {
                    std::cout << " NOT A REFERENCE ";
                }
                std::cout << std::endl;
            }
        }

    }

    bRet = (m_pDecoderHandler->DecodePictureWithParameters(pPerFrameDecodeParameters, pDecodePictureInfo) >= 0);

    if (m_dumpParserData) {
        std::cout << "\t <== VulkanVideoParser::DecodePicture " << PicIdx << std::endl;
    }
    m_nCurrentPictureID++;
    return bRet;
}

IVulkanVideoParser* VKAPI_CALL
vulkanCreateVideoParser(IVulkanVideoDecoderHandler* pDecoderHandler,
    IVulkanVideoFrameBufferParserCb* pVideoFrameBuffer,
    VkVideoCodecOperationFlagBitsKHR codecType,
    const VkExtensionProperties* pStdExtensionVersion,
    uint32_t maxNumDecodeSurfaces,
    uint32_t maxNumDpbSurfaces, uint64_t clockRate)
{
    if (codecType == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT) {
        if (!pStdExtensionVersion || strcmp(pStdExtensionVersion->extensionName, VK_STD_VULKAN_VIDEO_CODEC_H264_EXTENSION_NAME) || (pStdExtensionVersion->specVersion != VK_STD_VULKAN_VIDEO_CODEC_H264_SPEC_VERSION)) {
            assert(!"Decoder h264 Codec version is NOT supported");
            return NULL;
        }
    } else if (codecType == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT) {
        if (!pStdExtensionVersion || strcmp(pStdExtensionVersion->extensionName, VK_STD_VULKAN_VIDEO_CODEC_H265_EXTENSION_NAME) || (pStdExtensionVersion->specVersion != VK_STD_VULKAN_VIDEO_CODEC_H265_SPEC_VERSION)) {
            assert(!"Decoder h265 Codec version is NOT supported");
            return NULL;
        }
    } else {
        assert(!"Decoder Codec is NOT supported");
        return NULL;
    }

    return IVulkanVideoParser::CreateInstance(pDecoderHandler, pVideoFrameBuffer,
        codecType, maxNumDecodeSurfaces,
        maxNumDpbSurfaces, clockRate);
}
