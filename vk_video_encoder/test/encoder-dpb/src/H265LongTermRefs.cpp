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

// vk-video-encoder-dpb-h265-long-term drives VkEncDpbH265 from the encoder library with H.265
// long-term reference pictures, which the encoder itself does not set, with no Vulkan device. Each
// picture names its reference picture set; the test checks the long-term sets the class derives
// (RefPicSetLtCurr, RefPicSetLtFoll) and the marking of 8.3.2: every picture in either long-term
// set is "used for long-term reference", and every reference picture in no set becomes "unused for
// reference". The marking is read back through FillStdReferenceInfo, as the encoder sends it.
//
// The reference pictures, 8 bits of POC LSB, every reference used by the current picture unless
// it is in a Foll set:
//   POC 0  IDR
//   POC 1  P, short-term {0}
//   POC 2  P, short-term {1}, long-term Foll {0}: more Foll than Curr entries
//   POC 3  P, short-term {2}, long-term Curr {0} and Foll {1}
//   POC 4  P, short-term {3}, long-term Curr {0}: 1 and 2 are in no set
// Then two pictures the class must refuse (DpbPictureStart returns -1) before it reads their
// long-term pictures:
//   POC 5  P, short-term {4} and 16 long-term pictures: more than the DPB less one (7.4.8), and
//          more than the 15 its arrays hold
//   POC 5  P, short-term {4} and lt_idx_sps 32: past the SPS's 32 long-term candidates
//
// The class is allocated on its own, so that a sanitizer sees an access outside it.
//
// Exit status: 0 every check held, 1 a check failed.

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "VkVideoEncoder/VkEncoderDpbH265.h"

namespace {

constexpr int32_t kMaxPicOrderCntLsb = 256;
constexpr int32_t kDpbSize = 6;

struct LongTermRef {
    int32_t poc;
    bool usedByCurrPic;
};

class LongTermRefTest {
   public:
    LongTermRefTest() : m_dpb(new VkEncDpbH265()) { m_dpb->DpbSequenceStart(kDpbSize, true); }

    // Encodes one reference picture: the IDR at POC 0, otherwise a P picture whose short-term
    // references precede it, nearest first, and whose long-term references are coded by POC LSB.
    VkEncDpbH265::RefPicSet Picture(int32_t poc, const std::vector<int32_t>& shortTerm, const std::vector<LongTermRef>& longTerm) {
        const bool idr = (poc == 0);
        StdVideoEncodeH265PictureInfo info{};
        info.pic_type = idr ? STD_VIDEO_H265_PICTURE_TYPE_IDR : STD_VIDEO_H265_PICTURE_TYPE_P;
        info.flags.IrapPicFlag = idr ? 1 : 0;
        info.flags.is_reference = 1;
        info.flags.pic_output_flag = 1;
        info.PicOrderCntVal = poc;

        StdVideoH265ShortTermRefPicSet rps{};
        int32_t previous = poc;
        for (size_t i = 0; i < shortTerm.size(); i++) {
            rps.delta_poc_s0_minus1[i] = static_cast<uint16_t>(previous - shortTerm[i] - 1);
            rps.used_by_curr_pic_s0_flag |= static_cast<uint16_t>(1u << i);
            previous = shortTerm[i];
        }
        rps.num_negative_pics = static_cast<uint8_t>(shortTerm.size());

        StdVideoEncodeH265LongTermRefPics longTermPics{};
        longTermPics.num_long_term_pics = static_cast<uint8_t>(longTerm.size());
        for (size_t i = 0; i < longTerm.size(); i++) {
            longTermPics.poc_lsb_lt[i] = static_cast<uint16_t>(longTerm[i].poc % kMaxPicOrderCntLsb);
            if (longTerm[i].usedByCurrPic) {
                longTermPics.used_by_curr_pic_lt_flag |= static_cast<uint16_t>(1u << i);
            }
        }
        info.pLongTermRefPics = &longTermPics;
        // The class counts the long-term pictures only with the SPS list present; it holds none.
        const StdVideoH265LongTermRefPicsSps longTermSps{};

        VkEncDpbH265::RefPicSet refPicSet{};
        const int8_t slot = m_dpb->DpbPictureStart(&info, &rps, &longTermSps, kMaxPicOrderCntLsb,
                                                   static_cast<uint64_t>(poc), &refPicSet);
        VkSharedBaseObj<VulkanVideoImagePoolNode> image(new VulkanVideoImagePoolNode());
        m_dpb->DpbPictureEnd(image, 1, true);
        m_slots[poc] = slot;
        return refPicSet;
    }

    // Starts a picture the class must refuse and returns DpbPictureStart's slot: |longTerm| POC LSBs
    // coded in the slice, |ltIdxSps| indexes into the SPS's long-term candidates.
    int8_t Start(int32_t poc, const std::vector<int32_t>& shortTerm, const std::vector<int32_t>& longTerm,
                 const std::vector<uint8_t>& ltIdxSps) {
        StdVideoEncodeH265PictureInfo info{};
        info.pic_type = STD_VIDEO_H265_PICTURE_TYPE_P;
        info.flags.is_reference = 1;
        info.flags.pic_output_flag = 1;
        info.PicOrderCntVal = poc;

        StdVideoH265ShortTermRefPicSet rps{};
        int32_t previous = poc;
        for (size_t i = 0; i < shortTerm.size(); i++) {
            rps.delta_poc_s0_minus1[i] = static_cast<uint16_t>(previous - shortTerm[i] - 1);
            rps.used_by_curr_pic_s0_flag |= static_cast<uint16_t>(1u << i);
            previous = shortTerm[i];
        }
        rps.num_negative_pics = static_cast<uint8_t>(shortTerm.size());

        StdVideoEncodeH265LongTermRefPics longTermPics{};
        longTermPics.num_long_term_sps = static_cast<uint8_t>(ltIdxSps.size());
        for (size_t i = 0; i < ltIdxSps.size(); i++) {
            longTermPics.lt_idx_sps[i] = ltIdxSps[i];
        }
        longTermPics.num_long_term_pics = static_cast<uint8_t>(longTerm.size());
        for (size_t i = 0; i < longTerm.size(); i++) {
            longTermPics.poc_lsb_lt[i] = static_cast<uint16_t>(longTerm[i] % kMaxPicOrderCntLsb);
        }
        info.pLongTermRefPics = &longTermPics;
        const StdVideoH265LongTermRefPicsSps longTermSps{};

        VkEncDpbH265::RefPicSet refPicSet{};
        return m_dpb->DpbPictureStart(&info, &rps, &longTermSps, kMaxPicOrderCntLsb,
                                      static_cast<uint64_t>(poc), &refPicSet);
    }

    int8_t Slot(int32_t poc) const { return m_slots.at(poc); }

    StdVideoEncodeH265ReferenceInfo Info(int32_t poc) const {
        StdVideoEncodeH265ReferenceInfo info{};
        m_dpb->FillStdReferenceInfo(static_cast<uint8_t>(Slot(poc)), &info);
        return info;
    }

    // The marking of the picture of |poc|: "long-term", "short-term" or "unused", or "replaced"
    // when its slot holds another picture.
    const char* Marking(int32_t poc) const {
        const StdVideoEncodeH265ReferenceInfo info = Info(poc);
        if (info.PicOrderCntVal != poc) {
            return "replaced";
        }
        if (info.flags.unused_for_reference) {
            return "unused";
        }
        return info.flags.used_for_long_term_reference ? "long-term" : "short-term";
    }

   private:
    std::unique_ptr<VkEncDpbH265> m_dpb;
    std::map<int32_t, int8_t> m_slots;
};

int g_failures = 0;

void Check(int32_t poc, const char* what, bool held) {
    printf("%s  POC %d: %s\n", held ? "PASS" : "FAIL", poc, what);
    if (!held) {
        g_failures++;
    }
}

void CheckMarking(const LongTermRefTest& test, int32_t poc, int32_t of, const char* expected) {
    const char* marking = test.Marking(of);
    char what[128];
    snprintf(what, sizeof(what), "POC %d is %s (%s)", of, expected, marking);
    Check(poc, what, std::string(marking) == expected);
}

}  // namespace

int main() {
    LongTermRefTest test;

    test.Picture(0, {}, {});
    test.Picture(1, {0}, {});
    CheckMarking(test, 1, 0, "short-term");

    VkEncDpbH265::RefPicSet set = test.Picture(2, {1}, {{0, false}});
    Check(2, "RefPicSetLtFoll holds POC 0", set.ltFoll[0] == test.Slot(0));
    Check(2, "RefPicSetLtCurr is empty", set.ltCurr[0] == -1);
    CheckMarking(test, 2, 0, "long-term");
    CheckMarking(test, 2, 1, "short-term");

    set = test.Picture(3, {2}, {{0, true}, {1, false}});
    Check(3, "RefPicSetLtCurr holds POC 0", set.ltCurr[0] == test.Slot(0));
    Check(3, "RefPicSetLtFoll holds POC 1", set.ltFoll[0] == test.Slot(1));
    Check(3, "RefPicSetStCurrBefore holds POC 2", set.stCurrBefore[0] == test.Slot(2));
    CheckMarking(test, 3, 0, "long-term");
    CheckMarking(test, 3, 1, "long-term");
    CheckMarking(test, 3, 2, "short-term");

    set = test.Picture(4, {3}, {{0, true}});
    Check(4, "RefPicSetLtCurr holds POC 0", set.ltCurr[0] == test.Slot(0));
    CheckMarking(test, 4, 0, "long-term");
    CheckMarking(test, 4, 1, "unused");
    CheckMarking(test, 4, 2, "unused");
    CheckMarking(test, 4, 3, "short-term");

    std::vector<int32_t> sixteen;
    for (int32_t i = 0; i < 16; i++) {
        sixteen.push_back(i);
    }
    Check(5, "a set of 16 long-term pictures is refused", test.Start(5, {4}, sixteen, {}) == -1);
    Check(5, "lt_idx_sps 32 is refused", test.Start(5, {4}, {}, {32}) == -1);

    printf("%d checks failed\n", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
