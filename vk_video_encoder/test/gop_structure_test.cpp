/*
 * Test for GOP structure calculation - verifying GOP sequnces with corner cases
 * 
 * GOP frame count: 11, IDR period: 25, Consecutive B frames: 3, Open GOP
 */

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <vector>
#include "../libs/VkVideoEncoder/VkVideoGopStructure.h"

void printGopTable(uint32_t gopFrameCount, uint32_t idrPeriod, uint32_t consecutiveBFrames, uint32_t numFrames) {
    VkVideoGopStructure gop(gopFrameCount, idrPeriod, consecutiveBFrames, 1, 
                            VkVideoGopStructure::FRAME_TYPE_P, 
                            VkVideoGopStructure::FRAME_TYPE_P, 
                            false /* open GOP */, 0);

    std::cout << "\nGOP frame count: " << gopFrameCount 
              << ", IDR period: " << idrPeriod 
              << ", Consecutive B frames: " << consecutiveBFrames 
              << ", Open GOP\n";

    // Print header
    std::cout << "Frame Index:  ";
    for (uint32_t i = 0; i < numFrames; i++) {
        std::cout << std::setw(4) << i;
    }
    std::cout << "\n";

    // Collect all positions
    VkVideoGopStructure::GopState gopState;
    std::vector<VkVideoGopStructure::GopPosition> positions;
    
    for (uint32_t i = 0; i < numFrames; i++) {
        positions.emplace_back(gopState.positionInInputOrder);
        uint32_t framesLeft = numFrames - i;
        gop.GetPositionInGOP(gopState, positions[i], (i == 0), framesLeft);
    }

    // Print Frame Type
    std::cout << "Frame Type:   ";
    for (uint32_t i = 0; i < numFrames; i++) {
        std::cout << std::setw(4) << VkVideoGopStructure::GetFrameTypeName(positions[i].pictureType);
    }
    std::cout << "\n";

    // Print Input order
    std::cout << "Input  order: ";
    for (uint32_t i = 0; i < numFrames; i++) {
        std::cout << std::setw(4) << positions[i].inputOrder;
    }
    std::cout << "\n";

    // Print Encode order
    std::cout << "Encode order: ";
    for (uint32_t i = 0; i < numFrames; i++) {
        std::cout << std::setw(4) << positions[i].encodeOrder;
    }
    std::cout << "\n";

    // Print InGop order
    std::cout << "InGop  order: ";
    for (uint32_t i = 0; i < numFrames; i++) {
        std::cout << std::setw(4) << positions[i].inGop;
    }
    std::cout << "\n";

    // Print numBFrames
    std::cout << "numBFrames:   ";
    for (uint32_t i = 0; i < numFrames; i++) {
        if (positions[i].numBFrames < 0) {
            std::cout << "   X";
        } else {
            std::cout << std::setw(4) << (int)positions[i].numBFrames;
        }
    }
    std::cout << "\n";

    // Print bFramePos
    std::cout << "bFramePos:    ";
    for (uint32_t i = 0; i < numFrames; i++) {
        if (positions[i].bFramePos < 0) {
            std::cout << "   X";
        } else {
            std::cout << std::setw(4) << (int)positions[i].bFramePos;
        }
    }
    std::cout << "\n";

    // Print isRef
    std::cout << "isRef:        ";
    for (uint32_t i = 0; i < numFrames; i++) {
        bool isRef = (positions[i].flags & VkVideoGopStructure::FLAGS_IS_REF) != 0;
        std::cout << std::setw(4) << (isRef ? "R" : "N");
    }
    std::cout << "\n";

    // Print closeGOP flag
    std::cout << "closeGOP:     ";
    for (uint32_t i = 0; i < numFrames; i++) {
        bool closeGop = (positions[i].flags & VkVideoGopStructure::FLAGS_CLOSE_GOP) != 0;
        std::cout << std::setw(4) << (closeGop ? "C" : "-");
    }
    std::cout << "\n";
}

void verifyExpectedValues() {
    std::cout << "\n=== Verifying Expected Values ===\n";
    
    VkVideoGopStructure gop(11, 25, 3, 1, 
                            VkVideoGopStructure::FRAME_TYPE_P, 
                            VkVideoGopStructure::FRAME_TYPE_P, 
                            false /* open GOP */, 0);

    VkVideoGopStructure::GopState gopState;
    std::vector<VkVideoGopStructure::GopPosition> positions;
    
    for (uint32_t i = 0; i < 30; i++) {
        positions.emplace_back(gopState.positionInInputOrder);
        gop.GetPositionInGOP(gopState, positions[i], (i == 0), 30 - i);
    }

    // Expected encode orders from the PR description (after fix)
    int expectedEncodeOrder[] = {0, 2, 3, 4, 1, 6, 7, 8, 5, 10, 11, 9, 13, 14, 15, 12, 17, 18, 19, 16, 21, 22, 20, 24, 23, 0, 2, 3, 4, 1};
    
    // Expected numBFrames (X = -1)
    int expectedNumBFrames[] = {-1, 3, 3, 3, -1, 3, 3, 3, -1, 2, 2, -1, 3, 3, 3, -1, 3, 3, 3, -1, 2, 2, -1, 1, -1, -1, 3, 3, 3, -1};
    
    // Expected bFramePos (X = -1)
    int expectedBFramePos[] = {-1, 0, 1, 2, -1, 0, 1, 2, -1, 0, 1, -1, 0, 1, 2, -1, 0, 1, 2, -1, 0, 1, -1, 0, -1, -1, 0, 1, 2, -1};

    bool allPassed = true;

    std::cout << "\nEncode Order checks:\n";
    for (int i = 0; i < 30; i++) {
        if ((int)positions[i].encodeOrder != expectedEncodeOrder[i]) {
            std::cout << "  Frame " << i << ": got " << positions[i].encodeOrder 
                      << ", expected " << expectedEncodeOrder[i] << " FAIL\n";
            allPassed = false;
        }
    }

    std::cout << "\nnumBFrames checks:\n";
    for (int i = 0; i < 30; i++) {
        if (positions[i].numBFrames != expectedNumBFrames[i]) {
            std::cout << "  Frame " << i << ": got " << (int)positions[i].numBFrames 
                      << ", expected " << expectedNumBFrames[i] << " FAIL\n";
            allPassed = false;
        }
    }

    std::cout << "\nbFramePos checks:\n";
    for (int i = 0; i < 30; i++) {
        if (positions[i].bFramePos != expectedBFramePos[i]) {
            std::cout << "  Frame " << i << ": got " << (int)positions[i].bFramePos 
                      << ", expected " << expectedBFramePos[i] << " FAIL\n";
            allPassed = false;
        }
    }

    // Check FLAG_CLOSE_GOP - should NOT be on IDR (frame 0, 25)
    std::cout << "\nFLAG_CLOSE_GOP checks:\n";
    for (int i = 0; i < 30; i++) {
        bool isIDR = positions[i].pictureType == VkVideoGopStructure::FRAME_TYPE_IDR;
        bool hasCloseGOP = (positions[i].flags & VkVideoGopStructure::FLAGS_CLOSE_GOP) != 0;
        
        if (isIDR && hasCloseGOP) {
            std::cout << "  Frame " << i << " (IDR): has FLAG_CLOSE_GOP set incorrectly! FAIL\n";
            allPassed = false;
        }
    }

    if (allPassed) {
        std::cout << "\n=== ALL TESTS PASSED ===\n";
    } else {
        std::cout << "\n=== SOME TESTS FAILED ===\n";
    }
}

void testEdgeCases() {
    std::cout << "\n=== Edge Case Tests ===\n";
    
    // Test 1: No B-frames
    std::cout << "\n--- Test: No B-frames (GOP=8, IDR=16, B=0) ---\n";
    printGopTable(8, 16, 0, 20);
    
    // Test 2: Single B-frame
    std::cout << "\n--- Test: Single B-frame (GOP=8, IDR=16, B=1) ---\n";
    printGopTable(8, 16, 1, 20);
    
    // Test 3: GOP = IDR period
    std::cout << "\n--- Test: GOP = IDR (GOP=10, IDR=10, B=2) ---\n";
    printGopTable(10, 10, 2, 25);
    
    // Test 4: Small GOP
    std::cout << "\n--- Test: Small GOP (GOP=4, IDR=12, B=2) ---\n";
    printGopTable(4, 12, 2, 20);
}

int main() {
    std::cout << "=== GOP Structure Test ===\n";
    std::cout << "Testing: GOP=11, IDR=25, B=3, Open GOP\n";
    
    printGopTable(11, 25, 3, 30);
    
    verifyExpectedValues();
    
    testEdgeCases();
    
    return 0;
}
