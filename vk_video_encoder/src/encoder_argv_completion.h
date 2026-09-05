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

#ifndef _VK_VIDEO_ENCODER_ARGV_COMPLETION_H_
#define _VK_VIDEO_ENCODER_ARGV_COMPLETION_H_

#include <cstdio>
#include <functional>

#include "vulkan/vulkan.h"

//=============================================================================
// The argv wrapper's completion verdict.
//
// PRIVATE: no public ABI, no queried role, not installed. It exists so the
// wrapper can tell the truth about a completion it previously reported as
// VK_SUCCESS whatever happened -- it discarded the thread join's bool and
// never asked whether the buffered bitstream had reached the file.
//
// Both steps are supplied as callables so the decision can be tested without
// an encoder or a device, and so the test exercises this code rather than a
// copy of its condition.
//=============================================================================
class ArgvCompletionState {
public:
    // ORDER IS LOAD-BEARING: wait, then check the output, then let the caller
    // release the owner. A flush that runs before the workers have joined can
    // report success for a file those workers are still writing.
    //
    // |flushOutput| is only consulted when file output is enabled; a capture
    // session has no file to flush and must not be failed for the absence of
    // one.
    //
    // Cached. A repeated call neither joins nor flushes again, and never turns
    // a recorded failure into a success.
    VkResult Complete(bool fileOutputEnabled,
                      const std::function<bool()>& waitForThreads,
                      const std::function<bool()>& flushOutput)
    {
        if (m_completed) {
            return m_result;
        }
        m_completed = true;

        const bool waited = waitForThreads ? waitForThreads() : false;

        bool flushed = true;
        if (fileOutputEnabled) {
            flushed = flushOutput ? flushOutput() : false;
        }

        // The lower bool supplies no precise cause, so this does not invent a
        // Vulkan reason it cannot support.
        m_result = (waited && flushed) ? VK_SUCCESS : VK_ERROR_UNKNOWN;
        return m_result;
    }

    bool     Completed() const { return m_completed; }
    VkResult Result()    const { return m_result; }

    // Flush and check one stdio stream. Separate so the wrapper binds it and a
    // test can substitute a failing stream.
    static bool FlushFileOutput(FILE* file)
    {
        if (file == nullptr) {
            // File output was enabled and there is no handle: the output the
            // caller asked for does not exist.
            return false;
        }
        if (fflush(file) != 0) {
            return false;
        }
        return ferror(file) == 0;
    }

private:
    bool     m_completed = false;
    VkResult m_result    = VK_SUCCESS;
};

#endif /* _VK_VIDEO_ENCODER_ARGV_COMPLETION_H_ */
