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

/**
 * @file main.cpp
 * @brief VulkanFilterYuvCompute Test Application
 * 
 * This test application validates the VulkanFilterYuvCompute filter
 * with various input/output format combinations.
 * 
 * Usage:
 *   vk_filter_test [options]
 * 
 * Options:
 *   --help, -h       Show this help message
 *   --verbose, -v    Enable verbose output
 *   --smoke          Run only smoke tests (quick validation)
 *   --all            Run all standard tests
 *   --test <name>    Run specific test by name
 *   --list           List all available tests
 */

#include <iostream>
#include <string>
#include <vector>
#include <cstring>

#include "FilterTestApp.h"
#include "TestCases.h"

using namespace vkfilter_test;

void printUsage(const char* programName) {
    std::cout << "VulkanFilterYuvCompute Test Application\n" << std::endl;
    std::cout << "Usage: " << programName << " [options]\n" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --help, -h       Show this help message" << std::endl;
    std::cout << "  --verbose, -v    Enable verbose output" << std::endl;
    std::cout << "  --smoke          Run only smoke tests (quick validation)" << std::endl;
    std::cout << "  --all            Run all standard tests" << std::endl;
    std::cout << "  --production     Run production validation tests" << std::endl;
    std::cout << "  --regression     Run regression tests (verify bug fixes)" << std::endl;
    std::cout << "  --primaries      Run color primaries tests (BT.601/709/2020)" << std::endl;
    std::cout << "  --test <name>    Run specific test by name" << std::endl;
    std::cout << "  --list           List all available tests" << std::endl;
    std::cout << std::endl;
}

void listTests() {
    std::cout << "Available Tests:\n" << std::endl;
    
    std::cout << "=== SMOKE TESTS ===" << std::endl;
    auto smokeTests = TestCases::getSmokeTests();
    for (const auto& test : smokeTests) {
        std::cout << "  " << test.name << std::endl;
    }
    
    std::cout << "\n=== REGRESSION TESTS ===" << std::endl;
    auto regressionTests = TestCases::getRegressionTests();
    for (const auto& test : regressionTests) {
        std::cout << "  " << test.name << std::endl;
    }
    
    std::cout << "\n=== PRODUCTION TESTS ===" << std::endl;
    auto productionTests = TestCases::getProductionTests();
    for (const auto& test : productionTests) {
        std::cout << "  " << test.name << std::endl;
    }
    
    std::cout << "\n=== COLOR PRIMARIES TESTS ===" << std::endl;
    auto primariesTests = TestCases::getColorPrimariesTests();
    for (const auto& test : primariesTests) {
        std::cout << "  " << test.name << std::endl;
    }
    
    std::cout << "\n=== ALL STANDARD TESTS ===" << std::endl;
    auto allTests = TestCases::getAllStandardTests();
    for (const auto& test : allTests) {
        std::cout << "  " << test.name << std::endl;
    }
    
    std::cout << std::endl;
}

int main(int argc, char* argv[]) {
    bool verbose = false;
    bool runAll = false;
    bool runProduction = false;
    bool runRegression = false;
    bool runPrimaries = false;
    std::string specificTest;
    
    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--smoke") == 0) {
            runAll = false;
        } else if (strcmp(argv[i], "--all") == 0) {
            runAll = true;
        } else if (strcmp(argv[i], "--production") == 0) {
            runProduction = true;
        } else if (strcmp(argv[i], "--regression") == 0) {
            runRegression = true;
        } else if (strcmp(argv[i], "--primaries") == 0) {
            runPrimaries = true;
        } else if (strcmp(argv[i], "--test") == 0 && i + 1 < argc) {
            specificTest = argv[++i];
            runAll = false;
        } else if (strcmp(argv[i], "--list") == 0) {
            listTests();
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    
    std::cout << "========================================" << std::endl;
    std::cout << "VulkanFilterYuvCompute Test Application" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    // Create test application
    FilterTestApp app;
    
    // Initialize Vulkan
    VkResult result = app.init(verbose);
    if (result != VK_SUCCESS) {
        std::cerr << "Failed to initialize test application: " << result << std::endl;
        return 1;
    }
    
    // Register test cases
    std::vector<TestCaseConfig> testsToRun;
    
    if (!specificTest.empty()) {
        // Find specific test - search all test categories
        auto allTests = TestCases::getAllStandardTests();
        auto regressionTests = TestCases::getRegressionTests();
        auto productionTests = TestCases::getProductionTests();
        
        // Combine all tests for search
        std::vector<TestCaseConfig> allPossibleTests;
        allPossibleTests.insert(allPossibleTests.end(), allTests.begin(), allTests.end());
        allPossibleTests.insert(allPossibleTests.end(), regressionTests.begin(), regressionTests.end());
        allPossibleTests.insert(allPossibleTests.end(), productionTests.begin(), productionTests.end());
        
        bool found = false;
        for (const auto& test : allPossibleTests) {
            if (test.name == specificTest) {
                testsToRun.push_back(test);
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "Test not found: " << specificTest << std::endl;
            std::cerr << "Use --list to see available tests." << std::endl;
            return 1;
        }
    } else if (runAll) {
        testsToRun = TestCases::getAllStandardTests();
        // Also add production and regression tests
        auto regressionTests = TestCases::getRegressionTests();
        auto productionTests = TestCases::getProductionTests();
        testsToRun.insert(testsToRun.end(), regressionTests.begin(), regressionTests.end());
        testsToRun.insert(testsToRun.end(), productionTests.begin(), productionTests.end());
    } else if (runProduction) {
        testsToRun = TestCases::getProductionTests();
    } else if (runRegression) {
        testsToRun = TestCases::getRegressionTests();
    } else if (runPrimaries) {
        testsToRun = TestCases::getColorPrimariesTests();
    } else {
        testsToRun = TestCases::getSmokeTests();
    }
    
    // Register tests
    for (const auto& test : testsToRun) {
        app.registerTest(test);
    }
    
    // Run tests
    auto results = app.runAllTests();
    
    // Count failures
    int failures = 0;
    for (const auto& result : results) {
        if (!result.passed) {
            failures++;
        }
    }
    
    return failures > 0 ? 1 : 0;
}
