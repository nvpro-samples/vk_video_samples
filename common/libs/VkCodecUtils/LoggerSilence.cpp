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

#include "VkCodecUtils/LoggerSilence.h"

#include <cassert>
#include <mutex>

#include "Logger.h"

namespace {

std::mutex g_silenceMutex;
int g_silenceCount = 0;
LogLevel g_levelBeforeSilence = LogLevel::LOG_INFO;

void AcquireSilence() {
    std::lock_guard<std::mutex> lock(g_silenceMutex);
    if (g_silenceCount++ == 0) {
        g_levelBeforeSilence = Logger::instance().getLogLevel();
        Logger::instance().setLogLevel(static_cast<int>(LogLevel::LOG_NONE));
    }
}

void ReleaseSilence() {
    std::lock_guard<std::mutex> lock(g_silenceMutex);
    assert(g_silenceCount > 0);
    if (--g_silenceCount == 0) {
        Logger::instance().setLogLevel(static_cast<int>(g_levelBeforeSilence));
    }
}

}  // namespace

LoggerSilenceScope::LoggerSilenceScope(bool requestSilence) : m_held(requestSilence) {
    if (m_held) {
        AcquireSilence();
    }
}

LoggerSilenceScope::LoggerSilenceScope(LoggerSilenceScope&& other) noexcept : m_held(other.m_held) { other.m_held = false; }

LoggerSilenceScope& LoggerSilenceScope::operator=(LoggerSilenceScope&& other) noexcept {
    if (this != &other) {
        Release();
        m_held = other.m_held;
        other.m_held = false;
    }
    return *this;
}

void LoggerSilenceScope::Release() {
    if (!m_held) {
        return;
    }
    m_held = false;
    ReleaseSilence();
}
