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

// LoggerSilenceScope: a scope that requests silence makes the Logger print nothing while it lives; scopes are
// counted, so an outer one's silence holds after an inner one goes; the last release gives back the level the
// Logger had before the first; a scope that requests nothing changes nothing; a moved scope keeps the request.
// The Logger writes its stream messages to std::cout and std::cerr, which the test reads through their buffers.
//
// Exit codes: 0 pass, 1 fail.

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include "Logger.h"
#include "VkCodecUtils/LoggerSilence.h"

namespace {

int g_failures = 0;

void Expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

// What one info and one error message print, out and err.
std::pair<std::string, std::string> Print() {
    std::ostringstream out;
    std::ostringstream err;
    std::streambuf* coutBuf = std::cout.rdbuf(out.rdbuf());
    std::streambuf* cerrBuf = std::cerr.rdbuf(err.rdbuf());
    LOG_S_INFO << "info" << std::endl;
    LOG_S_ERROR << "error" << std::endl;
    std::cout.rdbuf(coutBuf);
    std::cerr.rdbuf(cerrBuf);
    return {out.str(), err.str()};
}

bool Silent() {
    const auto printed = Print();
    return printed.first.empty() && printed.second.empty();
}

bool Audible() {
    const auto printed = Print();
    return (printed.first == "info\n") && (printed.second == "Error: error\n");
}

}  // namespace

int main() {
    Logger::instance().setLogLevel(static_cast<int>(LogLevel::LOG_DEBUG));
    Expect(Audible(), "the Logger prints with no scope");

    {
        LoggerSilenceScope none;
        LoggerSilenceScope notRequested(false);
        Expect(!notRequested.RequestsSilence(), "a scope made with false requests nothing");
        Expect(Audible(), "scopes that request nothing leave the Logger printing");
    }

    {
        LoggerSilenceScope outer(true);
        Expect(outer.RequestsSilence(), "a scope made with true requests silence");
        Expect(Logger::instance().getLogLevel() == LogLevel::LOG_NONE, "a request sets LOG_NONE");
        Expect(Silent(), "the Logger prints nothing while a scope requests silence");
        {
            LoggerSilenceScope inner(true);
            Expect(Silent(), "the Logger prints nothing under two scopes");
        }
        Expect(Silent(), "the outer scope's silence holds after the inner one goes");

        LoggerSilenceScope moved(std::move(outer));
        Expect(!outer.RequestsSilence() && moved.RequestsSilence(), "a moved scope carries the request");
        Expect(Silent(), "the moved request keeps the Logger silent");
        moved.Release();
        Expect(!moved.RequestsSilence(), "Release gives up the request");
    }
    Expect(Logger::instance().getLogLevel() == LogLevel::LOG_DEBUG, "the last release gives back LOG_DEBUG");
    Expect(Audible(), "the Logger prints again after the last scope");

    Logger::instance().setLogLevel(static_cast<int>(LogLevel::LOG_ERROR));
    {
        LoggerSilenceScope first(true);
        LoggerSilenceScope second(true);
        first = std::move(second);
        Expect(Silent(), "move assignment keeps one request and the Logger silent");
    }
    Expect(Logger::instance().getLogLevel() == LogLevel::LOG_ERROR, "the last release gives back LOG_ERROR");

    if (g_failures != 0) {
        std::printf("FAIL: %d expectation(s) missed\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
