/*
 * Linux implementation of the completion-handle seam declared in
 * vulkan_video_encoder_os_event_linux.h.
 *
 * This translation unit is platform-specific by construction. Another
 * platform is served by another translation unit implementing the same
 * declarations, selected by the build; it is never served by an #else arm in
 * this one.
 */
#include "vulkan_video_encoder_os_event_linux.h"

#if !defined(__linux__)
#error "vulkan_video_encoder_os_event_linux.cpp implements the completion-handle seam for Linux only. Port the seam in a sibling translation unit and select it in the build."
#endif

#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace vkenc {

uint64_t OsCompletionEventCreate()
{
    // NONBLOCK so a caller polling on a sequence that forbids blocking cannot
    // block there; CLOEXEC so the handle does not leak into a child process,
    // which for a browser is a sandbox concern rather than hygiene.
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0) {
        return kOsCompletionEventNone;
    }
    // Any legal fd -- INCLUDING 0 -- stays distinct from the sentinel:
    // eventfd() never returns a negative fd on success.
    return (uint64_t)(int64_t)fd;
}

void OsCompletionEventSignal(uint64_t handle)
{
    if (handle == kOsCompletionEventNone) {
        return;
    }
    const uint64_t one = 1;
    // eventfd ACCUMULATES rather than latching, so an edge raised while the
    // reader is busy is added rather than lost -- which is what makes
    // coalescing safe here instead of lossy. EAGAIN is only reachable at the
    // 64-bit ceiling, where the reader is so far behind that the completion
    // counter reconciliation is the thing that will recover it.
    const ssize_t written = ::write((int)handle, &one, sizeof(one));
    (void)written;
}

uint64_t OsCompletionEventDuplicate(uint64_t handle)
{
    if (handle == kOsCompletionEventNone) {
        return kOsCompletionEventNone;
    }
    // F_DUPFD_CLOEXEC rather than dup(), so the duplicate carries close-on-exec
    // from the moment it exists. dup() followed by an fcntl() to set the flag
    // leaves a window in which a concurrent fork/exec inherits the descriptor,
    // which for a browser is the sandbox concern Create() already guards.
    //
    // The lowest free descriptor is fine: 0 is a legal result and stays
    // distinct from the sentinel, which is all-ones.
    const int duplicate = ::fcntl((int)handle, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        // The original is untouched. A caller that cannot export still has a
        // working encoder.
        return kOsCompletionEventNone;
    }
    return (uint64_t)(int64_t)duplicate;
}

void OsCompletionEventDestroy(uint64_t handle)
{
    if (handle != kOsCompletionEventNone) {
        ::close((int)handle);
    }
}

}  // namespace vkenc
