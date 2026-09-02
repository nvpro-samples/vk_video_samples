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

#ifndef _VKCODECUTILS_VKENCODERSTDIOLATCH_H_
#define _VKCODECUTILS_VKENCODERSTDIOLATCH_H_

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <cassert>
#include <iostream>
#include <ostream>
#include <streambuf>

//=============================================================================
// Process-wide stdio-silence gate.
//
// Chromium runs the encoder inside the sandboxed GPU process, where writes to
// stdout/stderr are at best lost and at worst trip sandbox diagnostics. The
// public VkVideoEncoderConfig::silenceStdio flag lets the caller suppress the
// library's info prints and error messages -- specifically, everything routed
// through the VkEncOut() / VkEncErr() wrappers below, which is where the
// library's std::cout / std::cerr output goes.
//
// SCOPE, precisely: the latch covers everything routed through the wrappers
// in this header -- the two gated streams via VkEncOut() / VkEncErr(), and
// gated formatted output via VkEncPrintfOut() / VkEncPrintfErr() /
// VkEncVPrintf(). A printf or fprintf written directly, bypassing those, is
// unaffected unless its own call site tests IsVkEncoderStdioSilenced(), which
// exactly one does: the argv-parsing failure print in
// vulkan_video_encoder_argv.cpp.
//
// EncoderConfig::ParseArguments() IS silenced. VkEncoderConfig.cpp routes its
// usage text and its per-option diagnostics through the gated wrappers and
// contains no direct printf or fprintf.
//
// The policy is process-wide rather than per-encoder state because the gated
// streams are used from translation units that have no handle to the
// VkVideoEncoderConfig. What is process-wide is a COUNT OF ACTIVE SILENCE
// REQUESTS, not a bool anyone may assign:
//
//   * silence holds while ANY live platform, context, session or scoped query
//     is requesting it;
//   * a caller that wants audible output holds no token -- it does not, and
//     cannot, turn another owner's silence off;
//   * silence ends when the last such owner is gone.
//
// A bool could not express that. The previous form was assigned by whoever
// initialized last, so a second session created while a first was running
// overwrote the first's decision, and the format enumeration's
// save/set/restore could restore a value another thread had changed in
// between. Simultaneous callers genuinely cannot each choose: this is a
// process-wide effect and one owner requiring silence wins.
//
// ODR constraint: the accessor must NOT be `static inline` with a
// function-local static. `static` gives the function INTERNAL linkage, so
// every translation unit that includes this header would get its OWN copy
// of the function -- and therefore its OWN counter -- and a request made in
// one TU would be invisible to every other TU. The storage has exactly ONE
// definition, in VkEncoderStdioLatch.cpp, and this header only declares it.
// The thin wrappers are plain `inline` (external linkage, ODR-merged) so
// call sites are unchanged.
//
// WHY ITS OWN FILE. The definition used to sit in VulkanDeviceContext.cpp,
// on the reasoning that VkCodecUtils is the bottom-most target every encoder
// consumer links. That reasoning was wrong: the standalone decoder, demo and
// test targets pick individual VkCodecUtils sources and do not compile the
// device context, so they failed to link the moment those sources started
// routing output through this gate. A target that needs the gate adds
// VkEncoderStdioLatch.cpp, which pulls in no Vulkan and no device.
//=============================================================================
// Single definition: VkEncoderStdioLatch.cpp.
std::atomic<int>& VkEncoderStdioSilenceCountRef();

// Reading the policy is a relaxed load: the counter arbitrates emission and
// publishes no other state, so no call site needs to see anything else that
// an owner did before requesting silence.
inline bool IsVkEncoderStdioSilenced()
{
    return VkEncoderStdioSilenceCountRef().load(std::memory_order_relaxed) > 0;
}

// One request. Construction takes it, destruction gives it back, and a move
// transfers it -- so an owner's silence lasts exactly as long as the owner,
// which is the property a bool assignment could not express.
//
// An aliasing role that keeps an encoder alive must keep its logging policy
// alive too, which means holding a token of its own. Two tokens held by one
// object are fine: correctness here is balanced lifetime, not a single
// designated owner.
class VkEncoderStdioSilenceScope {
public:
    // Requests nothing. This is what an audible owner holds.
    VkEncoderStdioSilenceScope() : m_held(false) { }

    explicit VkEncoderStdioSilenceScope(bool requestSilence)
        : m_held(requestSilence)
    {
        if (m_held) {
            VkEncoderStdioSilenceCountRef().fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    ~VkEncoderStdioSilenceScope() { Release(); }

    VkEncoderStdioSilenceScope(const VkEncoderStdioSilenceScope&) = delete;
    VkEncoderStdioSilenceScope& operator=(
        const VkEncoderStdioSilenceScope&) = delete;

    VkEncoderStdioSilenceScope(VkEncoderStdioSilenceScope&& other) noexcept
        : m_held(other.m_held)
    {
        other.m_held = false;
    }

    VkEncoderStdioSilenceScope& operator=(
        VkEncoderStdioSilenceScope&& other) noexcept
    {
        if (this != &other) {
            Release();
            m_held = other.m_held;
            other.m_held = false;
        }
        return *this;
    }

    // Idempotent: a token releases its one request and never a second.
    void Release()
    {
        if (!m_held) {
            return;
        }
        m_held = false;
        const int previous = VkEncoderStdioSilenceCountRef().fetch_sub(
            1, std::memory_order_relaxed);
        // Underflow means a request was released twice or never taken, which
        // would leave the process audible while an owner still needs silence.
        assert(previous > 0);
        (void)previous;
    }

    bool RequestsSilence() const { return m_held; }

private:
    bool m_held;
};

// A std::ostream backed by a no-op streambuf. VkEncOut()/VkEncErr() return a
// reference to either the real stream or this sink depending on the gate, so a
// call site only needs "std::cout" -> "VkEncOut()" (the trailing "<< ... <<
// std::endl" chain is unchanged and simply discarded when silenced).
class VkEncoderNullStreambuf : public std::streambuf {
protected:
    int overflow(int c) override { return c; } // swallow every character
};

// Plain `inline` (NOT `static inline`) so this is one ODR-merged function
// rather than one per translation unit.
//
// The objects it returns are THREAD-LOCAL. Discarding every character does
// not make an ostream safe to format into from two threads at once: width,
// fill, precision and the sentry all live in the stream object, and two
// silenced workers writing through one shared instance are a data race on
// them. One sink per thread costs nothing and removes it.
inline std::ostream& VkEncoderNullStream()
{
    static thread_local VkEncoderNullStreambuf nullBuf;
    static thread_local std::ostream           nullStream(&nullBuf);
    return nullStream;
}

inline std::ostream& VkEncOut()
{
    return IsVkEncoderStdioSilenced() ? VkEncoderNullStream() : std::cout;
}

inline std::ostream& VkEncErr()
{
    return IsVkEncoderStdioSilenced() ? VkEncoderNullStream() : std::cerr;
}


// Printf-style diagnostics, routed through the same gate.
//
// The library's diagnostics are overwhelmingly printf-style. Rewriting every
// one of them into a stream expression would be a much larger and more
// error-prone change than giving the gate a formatting entry point, so this is
// the seam those call sites move to.
//
// SCOPE, and it is the same distinction the streams draw: these are for
// DIAGNOSTICS. Output written to a file the caller asked for is the library's
// product and never comes through here -- fprintf/fwrite to an output FILE*
// stays exactly as it is.
//
// Formatting happens only when the output will actually be emitted, so a
// silenced process pays nothing for a message it discards.
inline void VkEncVPrintf(std::ostream& out, const char* format, va_list args)
{
    char stack[1024];
    va_list retry;
    va_copy(retry, args);
    const int needed = std::vsnprintf(stack, sizeof(stack), format, args);
    if (needed < 0) {
        va_end(retry);
        return;  // encoding error in the format; nothing useful to say
    }
    if ((size_t)needed < sizeof(stack)) {
        out << stack;
        va_end(retry);
        return;
    }
    // Rare: a message longer than the stack buffer. Heap only for that case.
    std::string heap((size_t)needed + 1, '\0');
    std::vsnprintf(&heap[0], heap.size(), format, retry);
    va_end(retry);
    heap.resize((size_t)needed);
    out << heap;
}

#if defined(__GNUC__)
#define VK_ENC_PRINTF_LIKE(fmtIndex, firstArg) \
    __attribute__((format(printf, fmtIndex, firstArg)))
#else
#define VK_ENC_PRINTF_LIKE(fmtIndex, firstArg)
#endif

VK_ENC_PRINTF_LIKE(1, 2)
inline void VkEncPrintfOut(const char* format, ...)
{
    if (IsVkEncoderStdioSilenced()) {
        return;
    }
    va_list args;
    va_start(args, format);
    VkEncVPrintf(std::cout, format, args);
    va_end(args);
}

VK_ENC_PRINTF_LIKE(1, 2)
inline void VkEncPrintfErr(const char* format, ...)
{
    if (IsVkEncoderStdioSilenced()) {
        return;
    }
    va_list args;
    va_start(args, format);
    VkEncVPrintf(std::cerr, format, args);
    va_end(args);
}

#endif /* _VKCODECUTILS_VKENCODERSTDIOLATCH_H_ */
