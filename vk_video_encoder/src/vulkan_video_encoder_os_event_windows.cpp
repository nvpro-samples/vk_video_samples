/*
 * Copyright 2025 NVIDIA Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "vulkan_video_encoder_os_event_linux.h"

#if !defined(_WIN32)
#error "vulkan_video_encoder_os_event_windows.cpp implements the completion-handle seam for Windows only."
#endif

#include <windows.h>

namespace vkenc {

uint64_t OsCompletionEventCreate()
{
    // AUTO-RESET, matching the eventfd the Linux seam creates with an initial
    // count of zero: a wait that arrives first blocks, and a manual-reset event
    // would leave the handle signalled after the first completion and report
    // every later wait as already-complete.
    //
    // The two platforms do NOT agree on what an unread signal accumulates to.
    // An auto-reset event is a latch: signalling one that is already signalled
    // coalesces, so N completions raised before any wait release ONE waiter.
    // A Linux eventfd accumulates a counter and a single read drains all of it.
    // Neither is one-signal-per-one-wait. The supported use on both is the
    // same: wake, drain the completions that are available, reconcile against
    // the monotonic completion count, then wait again.
    const HANDLE handle = ::CreateEventW(nullptr, /*bManualReset=*/FALSE,
                                         /*bInitialState=*/FALSE, nullptr);
    if (handle == nullptr) {
        return kOsCompletionEventNone;
    }
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
}

void OsCompletionEventSignal(uint64_t handle)
{
    if (handle == kOsCompletionEventNone) {
        return;
    }
    ::SetEvent(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)));
}

uint64_t OsCompletionEventDuplicate(uint64_t handle)
{
    if (handle == kOsCompletionEventNone) {
        return kOsCompletionEventNone;
    }
    HANDLE duplicate = nullptr;
    // Non-inheritable, for the reason CreateEventW's unnamed handle is: the
    // duplicate must not cross into a child process. DUPLICATE_SAME_ACCESS
    // keeps the wait/signal rights the original has.
    if (!::DuplicateHandle(::GetCurrentProcess(),
                           reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)),
                           ::GetCurrentProcess(), &duplicate, 0,
                           /*bInheritHandle=*/FALSE, DUPLICATE_SAME_ACCESS)) {
        // The original is untouched.
        return kOsCompletionEventNone;
    }
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(duplicate));
}

void OsCompletionEventDestroy(uint64_t handle)
{
    if (handle != kOsCompletionEventNone) {
        ::CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)));
    }
}

}  // namespace vkenc
