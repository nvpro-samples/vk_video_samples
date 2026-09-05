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
//   * SOME OF THEM ARE UNSIGNALLED WHEN HANDED OVER. A fence that is already
//     signalled every time is indistinguishable from no fence at all: the
//     test would pass against an encoder that returns a pre-signalled
//     descriptor and never orders anything. Requiring that a fair share are
//     still pending at handover is what makes the rest of this a measurement.
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

// FULL HD, AND ENOUGH FRAMES TO MEASURE. The handover check below asks
// whether a fence is still pending when the caller receives it, and at a small
// enough frame the encode finishes inside the submission call -- so every
// fence comes back signalled and the check reports a property of the frame
// size rather than of the encoder.
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

}  // namespace

int main(int argc, const char** argv)
{
    (void)argc;
    (void)argv;

    enctest::Report report;

    enctest::Session s;
    const char* why = "";
    if (!s.Open(kWidth, kHeight, &why)) {
        report.Skip("every check", why);
        return report.Summarise("encoder-release-fence");
    }

    enctest::DeviceFns fns;
    if (!fns.Load(s.binding)) {
        report.Skip("every check", "device entry points unavailable");
        return report.Summarise("encoder-release-fence");
    }

    enctest::HostImage image(fns, s.binding->PhysicalDevice(), s.binding->Device(),
                            kWidth, kHeight);
    if (!image.Create(&why)) {
        report.Skip("every check", why);
        return report.Summarise("encoder-release-fence");
    }

    Expected<ResourceId> registered = s.registry->RegisterImage(image.Describe());
    if (!registered) {
        report.Skip("every check", registered.status().detail());
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

        FrameSubmit frame;
        frame.frameId         = 1000 + i;
        frame.pts             = i * 3000;
        frame.registeredImage = *registered;
        frame.image.layout    = VK_IMAGE_LAYOUT_PREINITIALIZED;
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
        // A FLOOR, not "> 0". The count exists to rule out a fence exported
        // after the fact, which would come back already signalled on nearly
        // every frame -- and "> 0" passes that as long as a single frame races
        // ahead, which is the exact state the check is written to forbid.
        report.Check(pendingAtHandover >= (usable / 2),
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
    image.Destroy();

    const int fdAfter = OpenDescriptorCount();
    fprintf(stderr, "        descriptors: %d before, %d after\n", fdBefore, fdAfter);
    report.Check((fdBefore >= 0) && (fdAfter >= 0) && (fdAfter <= fdBefore),
                 "the run holds no more descriptors than it started with");

    return report.Summarise("encoder-release-fence");
}
