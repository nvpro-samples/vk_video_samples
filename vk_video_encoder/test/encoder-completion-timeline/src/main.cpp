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

// The session's completion timeline, reached through ICompletionTimeline.
//
// THE PROPERTY. With frame ids that increase in submission order, the
// timeline reaches CompletionValue(N) = N + 1 once frame N has encoded, its
// value never decreases -- B-frame reordering included -- and frame N becomes
// Ready shortly after that value is reached. Exported as an OPAQUE_FD, the
// timeline imports into a semaphore that reads the same payload.
//
// THE GOP HAS THREE B-FRAMES between references, so frames are encoded out of
// submission order and the encoder signals once per released batch rather
// than once per frame. Every frame is waited for in submission order, each
// wait bounded at kWaitNs: a wait that times out is a frame the timeline
// never reported, and fails the test.
//
// Skips (exit 77) only when no device encodes H.264; a device that cannot
// export the semaphore reports that one check as SKIP.

#include "encoder_test_support.h"

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace vk::video::enc;

namespace {

const char* const kName = "encoder-completion-timeline";

const uint32_t kWidth = 320;
const uint32_t kHeight = 240;
// I, then three runs of B B B P; the last frame ends the stream.
const uint32_t kFrames = 13;
const uint32_t kImages = 4;
const uint64_t kWaitNs = 5ull * 1000 * 1000 * 1000;
const auto kReadyTimeout = std::chrono::seconds(2);

struct TimelineFns {
    PFN_vkWaitSemaphores WaitSemaphores = nullptr;
    PFN_vkGetSemaphoreCounterValue GetSemaphoreCounterValue = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkImportSemaphoreFdKHR ImportSemaphoreFdKHR = nullptr;

    bool Load(const enctest::DeviceFns& fns, VkDevice device) {
        WaitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(fns.GetDeviceProcAddr(device, "vkWaitSemaphores"));
        GetSemaphoreCounterValue =
            reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(fns.GetDeviceProcAddr(device, "vkGetSemaphoreCounterValue"));
        CreateSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(fns.GetDeviceProcAddr(device, "vkCreateSemaphore"));
        DestroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(fns.GetDeviceProcAddr(device, "vkDestroySemaphore"));
        ImportSemaphoreFdKHR =
            reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(fns.GetDeviceProcAddr(device, "vkImportSemaphoreFdKHR"));
        return WaitSemaphores && GetSemaphoreCounterValue && CreateSemaphore && DestroySemaphore;
    }
};

class Run {
   public:
    Run(enctest::Report& report, enctest::Session& s, const TimelineFns& fns, VkDevice device, VkSemaphore timeline)
        : m_report(report), m_s(s), m_fns(fns), m_device(device), m_timeline(timeline) {}

    // Wait for frame |frameId|'s value, then for the frame to become Ready,
    // and retire it.
    void Verify(uint64_t frameId) {
        const uint64_t value = ICompletionTimeline::CompletionValue(frameId);
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait.semaphoreCount = 1;
        wait.pSemaphores = &m_timeline;
        wait.pValues = &value;
        const VkResult waited = m_fns.WaitSemaphores(m_device, &wait, kWaitNs);
        if (waited != VK_SUCCESS) {
            ++m_waitsFailed;
            fprintf(stderr, "        frame %llu: vkWaitSemaphores for %llu returned %d\n", static_cast<unsigned long long>(frameId),
                    static_cast<unsigned long long>(value), static_cast<int>(waited));
            return;
        }
        uint64_t counter = 0;
        m_fns.GetSemaphoreCounterValue(m_device, m_timeline, &counter);
        if ((counter < value) || (counter < m_lastCounter)) {
            ++m_notMonotonic;
            fprintf(stderr, "        frame %llu: counter %llu after waiting for %llu, previous %llu\n",
                    static_cast<unsigned long long>(frameId), static_cast<unsigned long long>(counter),
                    static_cast<unsigned long long>(value), static_cast<unsigned long long>(m_lastCounter));
        }
        m_lastCounter = counter;
        m_counters.push_back(counter);

        const auto deadline = std::chrono::steady_clock::now() + kReadyTimeout;
        FrameState state = m_s.submitter->GetFrameState(frameId);
        while ((state == FrameState::Pending) && (std::chrono::steady_clock::now() < deadline)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            state = m_s.submitter->GetFrameState(frameId);
        }
        if (state != FrameState::Ready) {
            ++m_notReady;
            fprintf(stderr, "        frame %llu: state %u after its value was reached\n", static_cast<unsigned long long>(frameId),
                    static_cast<unsigned>(state));
            return;
        }
        Expected<EncodedFrame> got = m_s.bitstream->Acquire(frameId);
        if (got && (got->outcome == ResultCode::Ok) && !got->bitstream.empty()) {
            ++m_encoded;
            if (got->pictureType == PictureType::Bidirectional) {
                ++m_bidirectional;
            }
        } else {
            fprintf(stderr, "        frame %llu: Ready but not acquired with a bitstream\n",
                    static_cast<unsigned long long>(frameId));
        }
        if (got) {
            m_s.bitstream->Release(frameId);
        }
    }

    void Report(uint32_t frames, bool bFrames) const {
        std::string counters;
        for (uint64_t c : m_counters) {
            counters += " " + std::to_string(c);
        }
        fprintf(stderr, "        timeline after each wait:%s\n", counters.c_str());
        fprintf(stderr, "        %u of %u frames encoded, %u bidirectional\n", m_encoded, frames, m_bidirectional);
        m_report.Check(m_waitsFailed == 0, "every frame's completion value is reached within 5 s");
        m_report.Check(m_notMonotonic == 0, "the timeline never falls and never reads below a value just waited for");
        m_report.Check(m_notReady == 0, "every frame is Ready within 2 s of its completion value");
        m_report.Check(m_encoded == frames, "every frame is acquired with a bitstream");
        if (bFrames) {
            m_report.Check(m_bidirectional > 0, "the stream carries B-frames, so the timeline was read under reordering");
        }
    }

    uint64_t LastCounter() const { return m_lastCounter; }

   private:
    enctest::Report& m_report;
    enctest::Session& m_s;
    const TimelineFns& m_fns;
    VkDevice m_device;
    VkSemaphore m_timeline;
    uint64_t m_lastCounter = 0;
    std::vector<uint64_t> m_counters;
    uint32_t m_waitsFailed = 0;
    uint32_t m_notMonotonic = 0;
    uint32_t m_notReady = 0;
    uint32_t m_encoded = 0;
    uint32_t m_bidirectional = 0;
};

// The exported fd, imported into a timeline semaphore on the session's device,
// reads the payload the session signals.
void CheckExport(enctest::Report& report, ICompletionTimeline& timeline, const TimelineFns& fns, VkDevice device,
                 uint64_t reached) {
    Expected<int> exported = timeline.ExportOpaqueFd();
    if (exported.status().code() == ResultCode::UnsupportedFeature) {
        report.Skip("the OPAQUE_FD export", exported.status().detail());
        return;
    }
    report.Check(exported.ok() && (*exported >= 0), "the timeline exports as an OPAQUE_FD");
    if (!exported.ok() || (*exported < 0)) {
        return;
    }
    if (fns.ImportSemaphoreFdKHR == nullptr) {
        close(*exported);
        report.Check(false, "vkImportSemaphoreFdKHR loads on the session's device");
        return;
    }
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    ci.pNext = &type;
    VkSemaphore imported = VK_NULL_HANDLE;
    if (fns.CreateSemaphore(device, &ci, nullptr, &imported) != VK_SUCCESS) {
        close(*exported);
        report.Check(false, "a timeline semaphore is created to import into");
        return;
    }
    VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    import.semaphore = imported;
    import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    import.fd = *exported;
    const VkResult result = fns.ImportSemaphoreFdKHR(device, &import);
    if (result != VK_SUCCESS) {
        close(*exported);
    }
    report.Check(result == VK_SUCCESS, "the exported fd imports into a timeline semaphore");
    uint64_t value = 0;
    if (result == VK_SUCCESS) {
        fns.GetSemaphoreCounterValue(device, imported, &value);
    }
    fprintf(stderr, "        imported semaphore reads %llu, the session's timeline had reached %llu\n",
            static_cast<unsigned long long>(value), static_cast<unsigned long long>(reached));
    report.Check((result == VK_SUCCESS) && (value >= reached), "the imported semaphore reads the session's payload");
    fns.DestroySemaphore(device, imported, nullptr);
}

}  // namespace

int main(int argc, const char** argv) {
    (void)argc;
    (void)argv;

    enctest::Report report;
    enctest::Session s;
    GopStructure gop;
    gop.gopLength = 16;
    gop.consecutiveBFrames = 3;
    const char* why = "";
    const enctest::Session::Opened opened = s.Open(kWidth, kHeight, &why, PlatformCreateInfo(), &gop);
    if (opened == enctest::Session::NoDevice) {
        report.Skip("every check", why);
        return report.Summarise(kName);
    }
    if (opened != enctest::Session::Ready) {
        report.Check(false, why);
        return report.Summarise(kName);
    }

    Ref<ICompletionTimeline> timeline = Query<ICompletionTimeline>(s.session);
    report.Check(static_cast<bool>(timeline), "the session offers ICompletionTimeline");
    const VkSemaphore semaphore = timeline ? timeline->Semaphore() : VK_NULL_HANDLE;
    report.Check(semaphore != VK_NULL_HANDLE, "the timeline semaphore exists before the first frame");

    enctest::DeviceFns fns;
    TimelineFns timelineFns;
    if (!fns.Load(s.binding) || !timelineFns.Load(fns, s.binding->Device())) {
        report.Check(false, "the device entry points load");
        return report.Summarise(kName);
    }
    if (semaphore == VK_NULL_HANDLE) {
        return report.Summarise(kName);
    }
    uint64_t initial = 0;
    timelineFns.GetSemaphoreCounterValue(s.binding->Device(), semaphore, &initial);
    report.Check(initial == 0, "the timeline starts at 0");

    // Each image is written once and submitted every kImages frames: a staged
    // copy only reads it, so no frame waits for an earlier one to let go.
    std::vector<std::unique_ptr<enctest::HostImage>> images;
    for (uint32_t i = 0; i < kImages; i++) {
        images.push_back(
            std::make_unique<enctest::HostImage>(fns, s.binding->PhysicalDevice(), s.binding->Device(), kWidth, kHeight));
        if (!images.back()->Create(&why) || !images.back()->WritePattern(i, &why)) {
            report.Check(false, why);
            return report.Summarise(kName);
        }
    }

    const bool bFrames = s.platform->Caps()->Supports(Feature::BFrames);
    Run run(report, s, timelineFns, s.binding->Device(), semaphore);
    uint64_t verified = 0;
    uint32_t accepted = 0;
    for (uint32_t f = 0; f < kFrames; f++) {
        FrameSubmit frame;
        frame.frameId = f;
        frame.pts = static_cast<uint64_t>(f) * 3000;
        frame.isLastFrame = (f + 1 == kFrames);
        frame.image = images[f % kImages]->Describe();
        Result result = s.submitter->SubmitFrame(frame);
        // Flow control: retire the oldest frame, which the timeline reports,
        // and submit again.
        while ((result.code() == ResultCode::NotReady) && (verified < f)) {
            run.Verify(verified++);
            result = s.submitter->SubmitFrame(frame);
        }
        if (!result) {
            fprintf(stderr, "        frame %u refused: %s\n", f, result.detail());
            break;
        }
        images[f % kImages]->MarkSubmitted();
        ++accepted;
    }
    report.Check(accepted == kFrames, "every frame is accepted");
    while (verified < accepted) {
        run.Verify(verified++);
    }
    run.Report(accepted, bFrames);
    report.Check(run.LastCounter() >= ICompletionTimeline::CompletionValue(kFrames - 1),
                 "the timeline ends at the last frame's completion value or above");

    CheckExport(report, *timeline, timelineFns, s.binding->Device(), run.LastCounter());

    report.Check(static_cast<bool>(s.session->Finish()), "the stream ends");
    report.Check(timeline->Semaphore() == VK_NULL_HANDLE, "after Finish the session has no timeline");
    report.Check(timeline->ExportOpaqueFd().status().code() == ResultCode::NotConfigured,
                 "after Finish the export is refused as NotConfigured");
    return report.Summarise(kName);
}
