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

// Drain() is not the end of the session, and Finish() is.
//
// THE INVARIANT. After a drain, everything submitted has been encoded and the
// session is exactly as usable as it was before: further frames are accepted
// and each becomes retrievable once. A drain may be taken as often as a caller
// likes.
//
// WHY IT IS WORTH A TEST OF ITS OWN. The failure this guards is silent. A
// drain that quietly ends retrieval leaves a session that still ACCEPTS
// frames and never retires any: the submissions succeed, the encoder
// reports no error, and the caller waits forever for output that will not
// come. Nothing crashes and nothing returns a failure -- so only a second
// batch, submitted after a drain and required to retire, catches it.
//
// The second batch is what this test is. The first batch exists to prove the
// pipeline retires anything at all, because a second batch that retires
// nothing proves nothing if the first did not either.
//
// Frames are submitted inline, as an image on the session's own device
// described with each submission -- the shape an embedding renderer uses.

#include "encoder_test_support.h"

#include <cstdio>

using namespace vk::video::enc;

namespace {

const uint32_t kWidth  = 320;
const uint32_t kHeight = 240;
const uint32_t kBatch  = 6;

// Submit |count| frames from |id|, returning how many the session accepted.
uint32_t SubmitBatch(enctest::Session& s,
                     enctest::HostImage& image,
                     uint64_t firstId,
                     uint32_t count)
{
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const char* why = "";
        if (!image.WritePattern(i, &why)) {
            break;
        }
        FrameSubmit frame;
        frame.frameId = firstId + i;
        frame.pts     = (firstId + i) * 3000;
        frame.image   = image.Describe();
        if (!s.submitter->SubmitFrame(frame)) {
            break;
        }
        image.MarkSubmitted();
        ++accepted;
    }
    return accepted;
}

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
        return report.Summarise("encoder-drain");
    case enctest::Session::Failed:
        report.Check(false, why);
        return report.Summarise("encoder-drain");
    }

    enctest::DeviceFns fns;
    if (!fns.Load(s.binding)) {
        report.Check(false, "the device entry points load");
        return report.Summarise("encoder-drain");
    }

    enctest::HostImage image(fns, s.binding->PhysicalDevice(), s.binding->Device(),
                            kWidth, kHeight);
    if (!image.Create(&why)) {
        report.Check(false, why);
        return report.Summarise("encoder-drain");
    }

    // ---- batch one: prove the pipeline retires at all ----
    const uint32_t submitted1 = SubmitBatch(s, image, 1000, kBatch);
    report.Check(submitted1 == kBatch, "every frame in the first batch is accepted");

    report.Check(static_cast<bool>(s.session->Drain()), "the first drain completes");
    const uint32_t retired1 = s.DrainRetrievable();
    fprintf(stderr, "        batch 1: %u submitted, %u retired\n", submitted1, retired1);

    // Asserted before the second batch, because a second batch that retires
    // nothing proves nothing if the first retired nothing either.
    report.Check(retired1 > 0, "the first batch retires frames");

    // ---- batch two: the session survived the drain ----
    const uint32_t submitted2 = SubmitBatch(s, image, 2000, kBatch);
    report.Check(submitted2 == kBatch,
                 "a frame is still accepted after a drain");

    report.Check(static_cast<bool>(s.session->Drain()), "a second drain completes");
    const uint32_t retired2 = s.DrainRetrievable();
    fprintf(stderr, "        batch 2: %u submitted, %u retired\n", submitted2, retired2);

    report.Check(retired2 > 0,
                 "a frame submitted after a drain still becomes retrievable");

    // ---- and every frame of both batches was retrievable ----
    report.Check(retired1 + retired2 >= submitted1 + submitted2,
                 "every frame submitted around a drain becomes retrievable");

    // ---- Finish, by contrast, does end it ----
    report.Check(static_cast<bool>(s.session->Finish()), "Finish ends the stream");
    s.DrainRetrievable();

    FrameSubmit after;
    after.frameId = 3000;
    after.image   = image.Describe();
    report.Check(!s.submitter->SubmitFrame(after),
                 "a frame submitted after Finish is refused");
    report.Check(s.submitter->GetFrameState(after.frameId) == FrameState::Unknown,
                 "a refused frame is not tracked");

    fns.DeviceWaitIdle(s.binding->Device());
    image.Destroy();

    return report.Summarise("encoder-drain");
}
