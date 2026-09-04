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

/*
 * VkVideoEncoder::AssembleBitstreamData -- the SYNCHRONOUS assembly path --
 * must publish NO completion record.
 *
 * THE CONTRACT THIS PINS. This tree has exactly one producer of a
 * CapturedBitstream: PushCapturedBitstream (VkVideoEncoder.h). It is reached
 * from four places, and NONE of them is the synchronous assembly path --
 * WriteBitstreamToFile (called only by the assembly worker), the worker's own
 * readback-failure arm which calls it directly, the AV1 override, and a
 * null-backend test seam in the ext layer. The sibling block in
 * test/encoder-ext-drain-assembly/src/main.cpp enumerates the same four. That
 * is not an incidental arrangement; seven sites in the tree are built on it
 * and say so in as many words:
 *
 *   VkVideoEncoder.cpp  ProcessOrderedFrames hard "return VK_ERROR_UNKNOWN"
 *                       guard, and that guard user-facing error text
 *   VkVideoEncoder.h    the DrainAndRestartThreads contract; and the
 *                       WriteDataToFile contract, which names this exact CLI
 *                       pair -- "under --syncAssembly --disableFileOutput
 *                       they reach neither ... encode-and-discard by
 *                       construction"
 *   vulkan_video_encoder_ext.cpp  the justification for pinning
 *                       cfg->asyncAssembly = 1, and the drain-restart note
 *   test/encoder-ext-drain-assembly/src/main.cpp  its documented premise, and
 *                       its record of why the alternative cannot work:
 *                       publishing from the synchronous path makes records
 *                       arrive before their PendingFrame exists, so
 *                       DrainCapturesLocked discards them and m_lateCaptures
 *                       fills with noise.
 *
 * WHY IT NEEDS ITS OWN TEST. The sync path is unreachable from the ext public
 * surface by three independent closures -- asyncAssembly is pinned on, a
 * completion subscriber is always registered, and ProcessOrderedFrames refuses
 * the sync fallback whenever a subscriber exists -- so no encoder-ext harness
 * can execute the function at all, and the file-based CLI that CAN reach it
 * hands out no VkVideoEncoder to observe. encoder-ext-drain-assembly is green
 * on a tree where this contract is broken, twice over: it runs both its
 * batches with async assembly ON, and its only assertions are LOWER bounds
 * (retired > 0), which are structurally blind to an EXTRA publish.
 *
 * WHY NO DEVICE. Every step of AssembleBitstreamData except the readback is
 * pure bookkeeping over a frame-info struct. So this subclasses the real
 * VkVideoEncoder with a null device context -- the shape the ext layer own
 * capture-funnel seam already uses -- overrides ReadbackBitstreamData to hand
 * back a synthetic payload, and calls the PRODUCTION AssembleBitstreamData.
 * Nothing else is stubbed or reimplemented, which is what makes a failure here
 * attributable to that function rather than to the harness. No GPU, no driver,
 * no display, no worker thread, no input file: it is deterministic on any
 * host, which is why it is labelled device-free and not skippable.
 *
 * ARM 2 IS NOT DECORATION. Arm 1 alone ("publish nothing") is satisfied by
 * simply deleting the write, which would be a far worse bug than the one being
 * pinned. Arm 2 runs the same call with real file output and asserts every
 * coded byte still reaches the file, so that mutation fails. It passes in both
 * states by design -- it is a guard rail, not a detector.
 *
 * The edge counters are separate from the record counters because
 * PushCapturedBitstream raises the completion edge OUTSIDE its own store
 * condition: an arm that stores nothing can still raise the edge, and only a
 * separate counter sees that.
 */

#include "VkVideoEncoder/VkVideoEncoder.h"
#include "VkVideoEncoder/VkEncoderConfigH264.h"

// The encoder headers reach the Xlib platform headers, whose macros collide
// with ordinary identifiers. Scrub them before anything else sees them -- the
// same block, for the same reason, as the sibling library test TUs.
#undef Status
#undef None
#undef Bool
#undef Window

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

std::string U64(uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    return std::string(buf);
}

std::string Hex32(VkResult v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", (unsigned)v);
    return std::string(buf);
}

void Check(bool cond, const char* what, const std::string& detail)
{
    ++g_checks;
    if (cond) {
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL %s -- %s\n", what, detail.c_str());
    }
}

// The synthetic frame. Sizes are arbitrary but fixed, so the bytes-on-disk
// assertion in arm 2 is an exact equality rather than a bound.
const uint32_t kPayloadBytes = 4096;
const uint32_t kHeaderBytes  = 32;

// The real VkVideoEncoder, with a null device context and the codec pure
// virtuals stubbed. None of the stubs is reachable from AssembleBitstreamData;
// they exist only because the class is abstract. The base constructor is pure
// member-init and the destruction path guards the missing device and joins no
// threads (none were started), so the null context is tolerated -- the same
// shape VkEncNullBackendCaptureSource relies on in the ext layer.
class SyncAssemblyProbe : public VkVideoEncoder {
public:
    SyncAssemblyProbe() : VkVideoEncoder(nullptr) {}

    // Configure as "--syncAssembly --disableFileOutput" (captureMode) or as
    // "--syncAssembly -o outputPath". Returns false if the file could not be
    // opened, which is a harness failure, not an assertion failure.
    bool Configure(bool captureMode, const char* outputPath)
    {
        VkSharedBaseObj<EncoderConfigH264> cfg(new EncoderConfigH264());
        cfg->disableFileOutput = captureMode ? 1 : 0;
        if (!captureMode) {
            if (cfg->outputFileHandler.SetFileName(outputPath) == 0) {
                return false;
            }
        }
        m_encoderConfig = cfg;
        return true;
    }

    // Count the completion edge. Deliberately attached in BOTH arms: the edge
    // is raised outside the PushCapturedBitstream store condition, so it is
    // the only observable that reports a publish in file-output mode.
    void CountEdges(uint32_t* counter)
    {
        SetOnBitstreamCaptured([counter](uint64_t) { ++(*counter); });
    }

    // Close the output file so its size can be read back. The config own
    // destructor would do this, but the assertion needs it flushed first.
    void CloseOutput()
    {
        if (m_encoderConfig) {
            m_encoderConfig->outputFileHandler.Destroy();
        }
    }

    // THE ONLY device-touching step of the synchronous path, replaced by a
    // synthetic payload. The bitstreamCopy arm is what lets the funnel and the
    // file-output arm both read a payload with no outputBitstreamBuffer bound;
    // both already honour it, for the assembly worker sake.
    VkResult ReadbackBitstreamData(VkSharedBaseObj<VkVideoEncodeFrameInfo>&,
                                   BitstreamReadback& readback) override
    {
        readback.bitstreamStartOffset = 0;
        readback.bitstreamSize        = kPayloadBytes;
        readback.status               = VK_QUERY_RESULT_STATUS_COMPLETE_KHR;
        readback.bitstreamCopy.assign(kPayloadBytes, 0xA5);
        readback.readbackDone         = true;
        return VK_SUCCESS;
    }

    // Drain the completion FIFO through the PRODUCTION accessor -- the same
    // call the ext layer DrainCapturesLocked makes -- and report how many
    // records the synchronous path left behind.
    uint32_t DrainRecords(size_t* outBytes)
    {
        uint32_t n = 0;
        *outBytes = 0;
        uint64_t frameId = 0;
        std::vector<uint8_t> bytes;
        bool isIdr = false;
        uint32_t pictureType = 0;
        VkResult status = VK_SUCCESS;
        while (TryPopCapturedBitstream(&frameId, &bytes, &isIdr,
                                       &pictureType, &status)) {
            ++n;
            *outBytes += bytes.size();
            bytes.clear();
        }
        return n;
    }

    // --- codec pure virtuals: never reached from AssembleBitstreamData ---
    VkResult CreateFrameInfoBuffersQueue(uint32_t) override {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    bool GetAvailablePoolNode(VkSharedBaseObj<VkVideoEncodeFrameInfo>&) override {
        return false;
    }
    VkResult InitEncoderCodec(VkSharedBaseObj<EncoderConfig>&) override {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkResult EncodeFrame(VkSharedBaseObj<VkVideoEncodeFrameInfo>&) override {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkResult CodecHandleRateControlCmd(
        VkSharedBaseObj<VkVideoEncodeFrameInfo>&) override {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkResult InitRateControl(VkCommandBuffer, uint32_t) override {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkResult ProcessDpb(VkSharedBaseObj<VkVideoEncodeFrameInfo>&,
                        uint32_t, uint32_t) override {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
};

VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> MakeFrame()
{
    VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> frame(
        new VkVideoEncoder::VkVideoEncodeFrameInfo());
    frame->bitstreamHeaderBufferSize = kHeaderBytes;
    frame->bitstreamHeaderOffset     = 0;
    std::memset(frame->bitstreamHeaderBuffer, 0x5A, kHeaderBytes);
    frame->frameEncodeInputOrderNum  = 0;
    return frame;
}

long FileSize(const char* path)
{
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        return -1;
    }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fclose(f);
    return n;
}

// ARM 1 -- capture mode: the "--syncAssembly --disableFileOutput" shape.
// disableFileOutput alone satisfies the PushCapturedBitstream store condition,
// so this arm observes exactly what the CLI would retain, with no
// subscriber-induced distortion.
void CaseCaptureModePublishesNothing()
{
    std::printf("capture mode (--syncAssembly --disableFileOutput):\n");

    SyncAssemblyProbe probe;
    if (!probe.Configure(/*captureMode=*/true, nullptr)) {
        Check(false, "harness: capture-mode config built", "Configure failed");
        return;
    }
    uint32_t edges = 0;
    probe.CountEdges(&edges);

    VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> frame = MakeFrame();
    VkResult result = probe.AssembleBitstreamData(frame, 0, 1);

    size_t retainedBytes = 0;
    const uint32_t records = probe.DrainRecords(&retainedBytes);
    std::printf("    assembled result=0x%x  records published=%u"
                "  retained bytes=%zu  completion edges=%u\n",
                (unsigned)result, records, retainedBytes, edges);

    Check(result == VK_SUCCESS,
          "the synchronous assembly itself still succeeds",
          "result " + Hex32(result));
    Check(records == 0,
          "AssembleBitstreamData leaves NO CapturedBitstream in the FIFO",
          "records=" + U64(records) + " holding " + U64(retainedBytes) +
              " bytes that nothing on this path will ever drain");
    Check(edges == 0,
          "AssembleBitstreamData raises NO completion edge",
          "edges=" + U64(edges));
}

// ARM 2 -- file output: the anti-mutation guard. Passes in both states.
void CaseFileOutputStillWritesEveryByte()
{
    std::printf("file output (--syncAssembly -o <file>):\n");

    const char* path = "encoder_sync_assembly_out.bin";
    std::remove(path);

    uint32_t edges = 0;
    {
        SyncAssemblyProbe probe;
        if (!probe.Configure(/*captureMode=*/false, path)) {
            Check(false, "harness: output file opened", path);
            return;
        }
        probe.CountEdges(&edges);

        VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> frame = MakeFrame();
        VkResult result = probe.AssembleBitstreamData(frame, 0, 1);
        probe.CloseOutput();

        Check(result == VK_SUCCESS,
              "the file-output assembly still succeeds",
              "result " + Hex32(result));
    }

    const long onDisk = FileSize(path);
    std::printf("    bytes on disk=%ld  completion edges=%u\n", onDisk, edges);

    Check(onDisk == (long)(kHeaderBytes + kPayloadBytes),
          "the file-output arm still writes header + every coded byte",
          "wrote " + U64((uint64_t)(int64_t)onDisk) + ", want " +
              U64(kHeaderBytes + kPayloadBytes));
    Check(edges == 0,
          "the file-output arm raises NO completion edge either",
          "edges=" + U64(edges));

    std::remove(path);
}

// GUARD 1 -- ProcessOrderedFrames refuses the synchronous fallback while a
// completion subscriber is registered.
//
// This is the guard that ENFORCES the contract arm 1 pins: arm 1 shows that
// AssembleBitstreamData publishes nothing, and THIS is what keeps a
// subscriber-bearing session off AssembleBitstreamData in the first place. It
// is quoted as a load-bearing site by five comments in the tree and, until
// now, nothing executed it.
//
// The preconditions are free here: m_asyncAssemblyEnabled defaults false, so
// the probe is already in the "async off" shape, and attaching a subscriber is
// the only other one. The guard returns before any device is touched.
void CaseOrderedSubscriberGuard()
{
    std::printf("ProcessOrderedFrames refuses a subscriber with async off:\n");

    SyncAssemblyProbe probe;
    if (!probe.Configure(/*captureMode=*/true, nullptr)) {
        Check(false, "harness: capture-mode config built", "Configure failed");
        return;
    }
    uint32_t edges = 0;
    probe.CountEdges(&edges);
    Check(probe.HasCompletionSubscriber(),
          "harness: a completion subscriber is registered",
          "HasCompletionSubscriber() is false after SetOnBitstreamCaptured");

    VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> frame = MakeFrame();
    const VkResult result = probe.ProcessOrderedFrames(frame, 1);
    std::printf("    ProcessOrderedFrames -> %s  completion edges=%u\n",
                Hex32(result).c_str(), edges);

    Check(result == VK_ERROR_UNKNOWN,
          "ProcessOrderedFrames refuses rather than encoding unreportably",
          "got " + Hex32(result) + ", want VK_ERROR_UNKNOWN " +
              Hex32(VK_ERROR_UNKNOWN) + " -- without the guard the call falls "
              "through into the callback sequence instead of refusing");
    Check(edges == 0,
          "the refused call publishes nothing",
          "edges=" + U64(edges));
}

// GUARD 2 -- the mirror in ProcessOutOfOrderFrames, added by the same commit
// as the fix and, until now, shipped with a comment asserting no test could
// construct the state that fires it.
void CaseOutOfOrderSubscriberGuard()
{
    std::printf("ProcessOutOfOrderFrames refuses a subscriber with async off:\n");

    SyncAssemblyProbe probe;
    if (!probe.Configure(/*captureMode=*/true, nullptr)) {
        Check(false, "harness: capture-mode config built", "Configure failed");
        return;
    }
    uint32_t edges = 0;
    probe.CountEdges(&edges);

    VkSharedBaseObj<VkVideoEncoder::VkVideoEncodeFrameInfo> frame = MakeFrame();
    const VkResult result = probe.ProcessOutOfOrderFrames(frame, 1);
    std::printf("    ProcessOutOfOrderFrames -> %s  completion edges=%u\n",
                Hex32(result).c_str(), edges);

    Check(result == VK_ERROR_UNKNOWN,
          "ProcessOutOfOrderFrames refuses rather than encoding unreportably",
          "got " + Hex32(result) + ", want VK_ERROR_UNKNOWN " +
              Hex32(VK_ERROR_UNKNOWN) + " -- without the guard the call falls "
              "through into the callback sequence instead of refusing");
    Check(edges == 0,
          "the refused call publishes nothing",
          "edges=" + U64(edges));
}

}  // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    std::printf("Synchronous AssembleBitstreamData publishes no completion record\n");
    std::printf("---------------------------------------------------------------\n");

    CaseCaptureModePublishesNothing();
    CaseFileOutputStillWritesEveryByte();
    CaseOrderedSubscriberGuard();
    CaseOutOfOrderSubscriberGuard();

    std::printf("---------------------------------------------------------------\n");
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
