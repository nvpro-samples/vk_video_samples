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

#ifndef _VKCODECUTILS_LOGGERSILENCE_H_
#define _VKCODECUTILS_LOGGERSILENCE_H_

// A request to silence the Logger (common/include/Logger.h) of this library. While any scope that requests
// silence lives, the Logger's level is LOG_NONE; when the last one goes, the Logger gets back the level it had
// when the first came. An embedder that wants no output from the encoder library sets
// VkVideoEncoderConfig::silenceStdio, and each platform, context, session or query made with it holds a scope.
//
// The requests are a count, not a flag: one owner's silence holds while any other's lives, and an owner that
// wants output holds no scope, so it cannot turn another owner's silence off. The count and the Logger are
// the library's own.
class LoggerSilenceScope {
   public:
    LoggerSilenceScope() = default;
    explicit LoggerSilenceScope(bool requestSilence);
    ~LoggerSilenceScope() { Release(); }

    LoggerSilenceScope(const LoggerSilenceScope&) = delete;
    LoggerSilenceScope& operator=(const LoggerSilenceScope&) = delete;
    LoggerSilenceScope(LoggerSilenceScope&& other) noexcept;
    LoggerSilenceScope& operator=(LoggerSilenceScope&& other) noexcept;

    // Gives up the request, if the scope holds one.
    void Release();
    bool RequestsSilence() const { return m_held; }

   private:
    bool m_held = false;
};

#endif  // _VKCODECUTILS_LOGGERSILENCE_H_
