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
 * VkVideoEncoderPsnrSum, without a device.
 *
 * The encoder's average PSNR is the sum of the frames' PSNR over the frame count. The assembly
 * threads add the frames in the order their fences complete, so the sum must not depend on the
 * order of its terms: the same frames give the same average in every run.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <vector>

#include "VkVideoEncoder/VkVideoEncoderPsnrSum.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

uint64_t Bits(double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// 300 frames of a plausible encode: PSNR between 25 and 55 dB, every bit of the double used.
std::vector<double> FramePsnr() {
    std::mt19937_64 generator(20261029);
    std::uniform_real_distribution<double> psnr(25.0, 55.0);
    std::vector<double> values(300);
    for (double& value : values) {
        value = psnr(generator);
    }
    return values;
}

double ExactSum(const std::vector<double>& values) {
    VkVideoEncoderPsnrSum sum;
    for (double value : values) {
        sum.Add(value);
    }
    return sum.Value();
}

// The same frames in 50 orders give one sum, bit for bit. A double sum of them takes more than one
// value over the same orders, which is printed, not checked: it is the reason for the exact sum.
void SumDoesNotDependOnOrder() {
    std::vector<double> values = FramePsnr();
    const double first = ExactSum(values);
    std::set<uint64_t> exactSums;
    std::set<uint64_t> doubleSums;
    std::mt19937_64 shuffler(7);
    for (int order = 0; order < 50; order++) {
        exactSums.insert(Bits(ExactSum(values)));
        double doubleSum = 0.0;
        for (double value : values) {
            doubleSum += value;
        }
        doubleSums.insert(Bits(doubleSum));
        std::shuffle(values.begin(), values.end(), shuffler);
    }
    std::printf("  50 orders of 300 frames: %zu exact sum(s), %zu double sum(s)\n", exactSums.size(), doubleSums.size());
    CHECK(exactSums.size() == 1);
    CHECK(Bits(ExactSum(values)) == Bits(first));
}

// Each value is rounded to 2^-32 dB, so the sum is within half a unit per frame of the true sum.
void SumIsCloseToTheTrueSum() {
    const std::vector<double> values = FramePsnr();
    long double reference = 0.0L;
    for (double value : values) {
        reference += value;
    }
    const double error = std::fabs(static_cast<double>(reference) - ExactSum(values));
    std::printf("  error against a long double sum: %.3g dB\n", error);
    CHECK(error <= 300.0 * std::ldexp(1.0, -33) + 1e-9);
}

// Values that are multiples of 2^-32 dB are summed without any rounding.
void RepresentableValuesAreExact() {
    VkVideoEncoderPsnrSum sum;
    sum.Add(100.0);
    sum.Add(0.5);
    sum.Add(std::ldexp(1.0, -32));
    CHECK(sum.Value() == 100.5 + std::ldexp(1.0, -32));
    sum.Reset();
    CHECK(sum.Value() == 0.0);
}

// A frame whose PSNR could not be measured (negative) or is NaN adds nothing.
void InvalidValuesAreNotAdded() {
    VkVideoEncoderPsnrSum sum;
    sum.Add(40.0);
    sum.Add(-1.0);
    sum.Add(std::nan(""));
    CHECK(sum.Value() == 40.0);
}

// The low word carries into the high one. Values far above any PSNR reach it in five additions:
// 5 * 10^9 dB is more than 2^64 units of 2^-32 dB, and every term and the sum are exact doubles.
void CarryReachesTheHighWord() {
    VkVideoEncoderPsnrSum sum;
    for (int i = 0; i < 5; i++) {
        sum.Add(1.0e9);
    }
    CHECK(sum.Value() == 5.0e9);
}

}  // namespace

int main() {
    struct {
        const char* name;
        void (*run)();
    } const tests[] = {
        {"SumDoesNotDependOnOrder", SumDoesNotDependOnOrder},         {"SumIsCloseToTheTrueSum", SumIsCloseToTheTrueSum},
        {"RepresentableValuesAreExact", RepresentableValuesAreExact}, {"InvalidValuesAreNotAdded", InvalidValuesAreNotAdded},
        {"CarryReachesTheHighWord", CarryReachesTheHighWord},
    };

    for (const auto& test : tests) {
        const int before = g_failures;
        std::printf("%s\n", test.name);
        test.run();
        std::printf("  %s\n", (g_failures == before) ? "PASS" : "FAIL");
    }
    std::printf("%s: %d failed expectation(s)\n", (g_failures == 0) ? "PASS" : "FAIL", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
