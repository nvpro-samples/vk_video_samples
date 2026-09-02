/*
 * Optional OS-handle completion currency.
 *
 * The ext layer's default path carries no OS-specific code -- no
 * <sys/eventfd.h>, no <windows.h>, no poll/epoll. The currency is declared
 * here in platform-neutral terms and the ext layer talks to it through these
 * three functions and nothing else.
 *
 * These declarations are portable; the implementations are not. Exactly one
 * implementation translation unit is compiled per platform and the build
 * selects it; the Linux one is vulkan_video_encoder_os_event_linux.cpp. A
 * platform with no implementation stops the build at configure time.
 *
 * Create can also fail on a platform that has an implementation -- the
 * process can be out of handles -- and returns kOsCompletionEventNone when it
 * does, which the caller reports as ERROR_HANDLE_TYPE_UNSUPPORTED.
 */
#ifndef VULKAN_VIDEO_ENCODER_OS_EVENT_LINUX_H_
#define VULKAN_VIDEO_ENCODER_OS_EVENT_LINUX_H_

#include <stdint.h>

namespace vkenc {

// The "no handle" value. All-ones rather than 0, because 0 is a legal
// handle: a process that closed stdin can be handed exactly that. A
// sentinel that collided with a valid handle would make a live completion
// event indistinguishable from a refusal.
constexpr uint64_t kOsCompletionEventNone = ~0ull;

// A waitable OS handle signalled once per completion, or
// kOsCompletionEventNone if one could not be created. Non-blocking and
// close-on-exec where the platform expresses those.
uint64_t OsCompletionEventCreate();

// Raise the edge. Called from the capture path, so it must not allocate, must
// not lock, and must be safe to call after Destroy on another thread has been
// ordered against it by the caller.
void OsCompletionEventSignal(uint64_t handle);

// A NEW handle onto the same completion object, carrying its own close
// obligation. The library keeps its original; a caller that exports one owns
// the duplicate and closes it. Returns kOsCompletionEventNone if the platform
// refuses, and MUST leave the original open when it does -- a failed export is
// not a reason to take the library's own event away from it.
//
// Duplicates share the underlying object. They are independent close
// obligations, not independent broadcast queues: a completion drained through
// one is not redelivered to the other.
uint64_t OsCompletionEventDuplicate(uint64_t handle);

void OsCompletionEventDestroy(uint64_t handle);

}  // namespace vkenc

#endif  // VULKAN_VIDEO_ENCODER_OS_EVENT_LINUX_H_
