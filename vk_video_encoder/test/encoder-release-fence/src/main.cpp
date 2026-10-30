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

// The release fence: when may the caller write the buffer again.
//
// A producer that hands the encoder a buffer needs to know when the encoder
// has finished READING it, which is earlier than when the bitstream appears
// and is the only thing that lets the buffer be recycled. FrameSubmit asks for
// that answer by naming storage; the library writes a descriptor into it.
//
// FOUR PROPERTIES, AND THE THIRD IS THE ONE THAT MATTERS.
//
//   * The storage is written on every submission, before anything can refuse.
//     A caller that re-submits must not inherit the previous attempt's fd.
//   * What arrives is usable, and belongs to the caller, who closes it.
//   * THEY ARE UNSIGNALLED WHEN HANDED OVER. A fence that is already
//     signalled is indistinguishable from no fence at all: the test would
//     pass against an encoder that returns a pre-signalled descriptor and
//     never orders anything. Each frame waits on a semaphore the test signals
//     only after it has looked at that frame's fence, so the encoder cannot
//     have read the image yet, and a real fence is pending at handover on
//     any device, however fast it reads.
//   * Every one of them eventually signals. A fence that never does is worse
//     than none: the producer waits forever on a buffer it owns.
//
// And across all of it the process must not accumulate descriptors, which is
// checked by counting them rather than by trusting the arithmetic.

#include "encoder_test_support.h"

#include <cstdio>
#include <dirent.h>
#include <poll.h>
#include <unistd.h>
#include <vector>

using namespace vk::video::enc;

namespace {

// FULL HD, AND ENOUGH FRAMES TO CYCLE THE ENCODER'S POOLS. The handover check
// does not depend on the frame size: each frame's read is held back until
// its fence has been looked at.
const uint32_t kWidth  = 1920;
const uint32_t kHeight = 1080;
const uint32_t kFrames = 64;

// A value no fence descriptor can be, so "was it written" is answerable
// without trusting the library to have written something plausible.
const int kSentinel = 0x5EED;

// How many descriptors this process holds. Counted, not inferred: an
// arithmetic argument about closes is exactly the thing a leak defeats.
int OpenDescriptorCount()
{
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (readdir(dir) != nullptr) {
        ++count;
    }
    closedir(dir);
    return count;
}

// Whether a sync descriptor has signalled, without waiting for it.
bool HasSignalled(int fd)
{
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, 0) > 0;
}

// Wait for one, bounded. A fence that never signals is a defect, not a reason
// to hang the suite.
bool WaitSignalled(int fd, int timeoutMs)
{
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, timeoutMs) > 0;
}

// The gate each frame waits on: a timeline semaphore on the encoder's device,
// signalled from the host to release one frame at a time.
struct Gate {
    PFN_vkCreateSemaphore  CreateSemaphore  = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkSignalSemaphore  SignalSemaphore  = nullptr;
    VkDevice    device    = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;

    bool Create(const enctest::DeviceFns& fns, VkDevice dev)
    {
        device = dev;
        CreateSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(
            fns.GetDeviceProcAddr(device, "vkCreateSemaphore"));
        DestroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(
            fns.GetDeviceProcAddr(device, "vkDestroySemaphore"));
        SignalSemaphore = reinterpret_cast<PFN_vkSignalSemaphore>(
            fns.GetDeviceProcAddr(device, "vkSignalSemaphore"));
        if (SignalSemaphore == nullptr) {
            SignalSemaphore = reinterpret_cast<PFN_vkSignalSemaphore>(
                fns.GetDeviceProcAddr(device, "vkSignalSemaphoreKHR"));
        }
        if ((CreateSemaphore == nullptr) || (DestroySemaphore == nullptr) ||
            (SignalSemaphore == nullptr)) {
            return false;
        }
        VkSemaphoreTypeCreateInfo typeInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        typeInfo.initialValue = 0;
        VkSemaphoreCreateInfo createInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &typeInfo };
        return CreateSemaphore(device, &createInfo, nullptr, &semaphore) == VK_SUCCESS;
    }

    // Lets the frame that waits on |value| read its image.
    void Release(uint64_t value)
    {
        VkSemaphoreSignalInfo signalInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
        signalInfo.semaphore = semaphore;
        signalInfo.value = value;
        SignalSemaphore(device, &signalInfo);
    }

    void Destroy()
    {
        if (semaphore != VK_NULL_HANDLE) {
            DestroySemaphore(device, semaphore, nullptr);
            semaphore = VK_NULL_HANDLE;
        }
    }
};

}  // namespace

int main(int argc, const char** argv)
{
    (void)argc;
    (void)argv;

    enctest::Report report;

    // Only a machine with no device that encodes skips. Once the session is
    // open, every failure is the library's or the test's, and fails.
    enctest::Session s;
    const char* why = "";
    switch (s.Open(kWidth, kHeight, &why)) {
    case enctest::Session::Ready:
        break;
    case enctest::Session::NoDevice:
        report.Skip("every check", why);
        return report.Summarise("encoder-release-fence");
    case enctest::Session::Failed:
        report.Check(false, why);
        return report.Summarise("encoder-release-fence");
    }

    enctest::DeviceFns fns;
    if (!fns.Load(s.binding)) {
        report.Check(false, "the device entry points load");
        return report.Summarise("encoder-release-fence");
    }

    enctest::HostImage image(fns, s.binding->PhysicalDevice(), s.binding->Device(),
                            kWidth, kHeight);
    if (!image.Create(&why)) {
        report.Check(false, why);
        return report.Summarise("encoder-release-fence");
    }

    Gate gate;
    if (!gate.Create(fns, s.binding->Device())) {
        report.Check(false, "a timeline semaphore is created on the encoder's device");
        return report.Summarise("encoder-release-fence");
    }

    Expected<ResourceId> registered = s.registry->RegisterImage(image.Describe());
    if (!registered) {
        report.Check(false, registered.status().detail());
        return report.Summarise("encoder-release-fence");
    }

    const int fdBefore = OpenDescriptorCount();

    uint32_t submitted = 0;
    uint32_t retiredDuringSubmit = 0;
    uint32_t written = 0;
    uint32_t usable = 0;
    uint32_t pendingAtHandover = 0;
    std::vector<int> fences;

    for (uint32_t i = 0; i < kFrames; ++i) {
        if (!image.WritePattern(i, &why)) {
            break;
        }

        // Armed with the sentinel, so "written" is a fact about this call and
        // not a leftover from the last one.
        int releaseFenceFd = kSentinel;

        // Held back until this frame's fence has been looked at below.
        const SemaphoreWait gateWait = { gate.semaphore, i + 1 };

        FrameSubmit frame;
        frame.frameId         = 1000 + i;
        frame.pts             = i * 3000;
        frame.registeredImage = *registered;
        frame.image.layout    = VK_IMAGE_LAYOUT_PREINITIALIZED;
        frame.waitSemaphores  = ArrayView<SemaphoreWait>(&gateWait, 1);
        frame.releaseFenceFd  = &releaseFenceFd;

        // BACKPRESSURE IS NOT A FAILURE. A full pipeline refuses with NotReady,
        // which means park the frame and re-submit once something has been
        // taken out -- so that is what a caller does, and what this does.
        // Treating it as a refusal would make this test a measurement of the
        // queue depth instead of the fence.
        Result sent = s.submitter->SubmitFrame(frame);
        for (int attempt = 0; !sent && sent.code() == ResultCode::NotReady &&
                              attempt < 64; ++attempt) {
            retiredDuringSubmit += s.DrainRetrievable();
            releaseFenceFd = kSentinel;
            sent = s.submitter->SubmitFrame(frame);
        }
        if (!sent) {
            fprintf(stderr, "        submission refused: %s\n", sent.detail());
            gate.Release(i + 1);
            break;
        }
        ++submitted;

        if (releaseFenceFd != kSentinel) {
            ++written;
        }
        if (releaseFenceFd >= 0) {
            ++usable;
            if (!HasSignalled(releaseFenceFd)) {
                ++pendingAtHandover;
            }
            fences.push_back(releaseFenceFd);
        }
        gate.Release(i + 1);
    }

    report.Check(submitted == kFrames, "every frame is accepted");
    report.Check(written == submitted,
                 "the release-fence slot is written on every submission");

    if (usable == 0) {
        // A library that answers "no fence" for every frame is a legal
        // implementation of the interface, and nothing below can be measured
        // against it. That is a property of this encoder, not a failure.
        report.Skip("release-fence behaviour",
                    "this encoder returns no release fence for any frame");
    } else {
        report.Check(usable == submitted,
                     "every accepted frame yields a usable release fence");

        fprintf(stderr, "        %u of %u fences were still pending at handover\n",
                pendingAtHandover, usable);
        // EVERY ONE. The frame's read waits on the gate, so a fence that has
        // signalled at handover was not made from that read: it was exported
        // after the fact, or pre-signalled.
        report.Check(pendingAtHandover == usable,
                     "release fences are handed over before they have signalled");

        s.session->Drain();

        uint32_t signalled = 0;
        for (int fd : fences) {
            if (WaitSignalled(fd, 5000)) {
                ++signalled;
            }
        }
        report.Check(signalled == usable, "every release fence signals");
    }

    // The bitstream is checked too: a run that ordered its fences perfectly and
    // encoded nothing has measured the wrong thing.
    uint32_t frames = 0;
    size_t bytes = 0;
    for (;;) {
        Expected<EncodedFrame> got = s.bitstream->AcquireNext();
        if (!got) {
            break;
        }
        ++frames;
        bytes += got->bitstream.size();
        s.bitstream->Release(got->frameId);
    }
    frames += retiredDuringSubmit;
    fprintf(stderr, "        %u frames (%u retired under backpressure), "
                    "%zu bitstream bytes\n", frames, retiredDuringSubmit, bytes);
    report.Check(frames > 0, "the encode produced frames");
    report.Check(bytes > 0, "the encode produced a bitstream");

    for (int fd : fences) {
        close(fd);
    }
    s.registry->UnregisterImage(*registered);
    fns.DeviceWaitIdle(s.binding->Device());
    gate.Destroy();
    image.Destroy();

    const int fdAfter = OpenDescriptorCount();
    fprintf(stderr, "        descriptors: %d before, %d after\n", fdBefore, fdAfter);
    report.Check((fdBefore >= 0) && (fdAfter >= 0) && (fdAfter <= fdBefore),
                 "the run holds no more descriptors than it started with");

    return report.Summarise("encoder-release-fence");
}
