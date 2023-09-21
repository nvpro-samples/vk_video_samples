/*
 * Copyright 2022 NVIDIA Corporation.
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

#include "VkVideoEncoder/VkVideoEncoder.h"

#define INPUT_FRAME_BUFFER_SIZE 16

int8_t parseArguments(EncodeConfig *encodeConfig, int argc, char *argv[])
{
    bool providedInputFileName = false;
    bool providedOutputFileName = false;
    bool providedQP = false;

    encodeConfig->name = argv[0];

    for (int32_t i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--width") == 0)) {
            if (++i >= argc || sscanf(argv[i], "%u", &encodeConfig->width) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
        }
        else if ((strcmp(argv[i], "--height") == 0)) {
            if (++i >= argc || sscanf(argv[i], "%u", &encodeConfig->height) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--startFrame") == 0) {
            if (++i >= argc || sscanf(argv[i], "%u", &encodeConfig->startFrame) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--numFrames") == 0) {
            if (++i >= argc || sscanf(argv[i], "%u", &encodeConfig->numFrames) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "-i") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
            strcpy(encodeConfig->inFileName, argv[i]);
            providedInputFileName = true;
        }
        else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
            strcpy(encodeConfig->outFileName, argv[i]);
            providedOutputFileName = true;
        }
        else if (strcmp(argv[i], "-qp") == 0) {
            if (++i >= argc || sscanf(argv[i], "%u", &encodeConfig->qp) != 1) {
                fprintf(stderr, "invalid parameter for %s\n", argv[i - 1]);
                return -1;
            }
            providedQP = true;
        }
        else if (strcmp(argv[i], "--logBatchEncoding") == 0) {
            encodeConfig->logBatchEncoding = true;
        }
        else {
            fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
            return -1;
        }
    }

    if (!providedInputFileName) {
        fprintf(stderr, "The input file was not specified\n");
        return -1;
    }

    if (!encodeConfig->width) {
        fprintf(stderr, "The width was not specified\n");
        return -1;
    }

    if (!encodeConfig->height) {
        fprintf(stderr, "The height was not specified\n");
        return -1;
    }

    if (!providedOutputFileName) {
        fprintf(stdout, "No output file name provided. Using out.264.\n");
        strcpy(encodeConfig->outFileName, "out.264");
    }

    if (!providedQP) {
        fprintf(stdout, "No QP was provided. Using default value: 20.\n");
        encodeConfig->qp = 20;
    }

    encodeConfig->codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT; //H264
    encodeConfig->chromaFormatIDC = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420; // YUV 420
    encodeConfig->inputVkFormat = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM; // YUV420 8bpp VkFormat
    encodeConfig->codecBlockAlignment = H264MbSizeAlignment; // H264
    encodeConfig->alignedWidth = (encodeConfig->width + encodeConfig->codecBlockAlignment - 1) & ~(encodeConfig->codecBlockAlignment - 1);
    encodeConfig->alignedHeight = (encodeConfig->height + encodeConfig->codecBlockAlignment - 1) & ~(encodeConfig->codecBlockAlignment - 1);
    encodeConfig->lumaPlaneSize = encodeConfig->alignedWidth * encodeConfig->alignedHeight;
    encodeConfig->chromaPlaneSize = ((encodeConfig->alignedWidth + 1) / 2) * ((encodeConfig->alignedHeight + 1) / 2);
    encodeConfig->fullImageSize = encodeConfig->lumaPlaneSize + encodeConfig->chromaPlaneSize*2;
    encodeConfig->bpp = 8;

    return 0;
}

void printHelp()
{
    fprintf(stderr,
            "Usage : EncodeApp \n\
    -i                              .yuv Input YUV File Name (YUV420p 8bpp only) \n\
    -o                              .264 Output H264 File Name \n\
    --startFrame                    <integer> : Start Frame Number to be Encoded \n\
    --numFrames                     <integer> : End Frame Number to be Encoded \n\
    --width                         <integer> : Encode Width \n\
    --height                        <integer> : Encode Height \n\
    -qp                             <integer> : QP value in the range [0, 51] \n\
    --logBatchEncoding              Enable verbose logging of batch recording and submission of commands \n"
    );
}

int32_t handle_error(const std::error_code& error)
{
    const auto& errmsg = error.message();
    std::printf("error mapping file: %s, exiting...\n", errmsg.c_str());
    return error.value();
}

int32_t openFiles(EncodeConfig *encodeConfig)
{
    encodeConfig->inputVid = fopen(encodeConfig->inFileName, "rb");
    if (!encodeConfig->inputVid) {
        fprintf(stderr, "Failed to open input file %s",encodeConfig->inFileName);
        return -1;
    }

    std::error_code error;
    encodeConfig->inputVideoMmap.map(encodeConfig->inFileName,
                                     0, mio::map_entire_file, error);
    if (error) {
        return handle_error(error);
        return -1;
    }

    printf("Input file size is: %zd\n", encodeConfig->inputVideoMmap.length());

    encodeConfig->outputVid = fopen(encodeConfig->outFileName, "wb");
    if (!encodeConfig->outputVid) {
        fprintf(stderr, "Failed to open output file %s",encodeConfig->outFileName);
        return -1;
    }

    return 0;
}

int8_t closeFiles(EncodeConfig *encodeConfig)
{
    if(fclose(encodeConfig->inputVid)) {
        fprintf(stderr, "Failed to close input file %s",encodeConfig->inFileName);
        return -1;
    }

    encodeConfig->inputVideoMmap.unmap();

    if(fclose(encodeConfig->outputVid)) {
        fprintf(stderr, "Failed to close output file %s",encodeConfig->outFileName);
        return -1;
    }

    fflush(stdout);
    return 0;
}

//--------------------------------------------------------------------------------------------------
// Entry of the example
//
int main(int argc, char** argv)
{
    EncodeConfig programConfig{};

    if (argc == 1) {
        printHelp();
        return -1;
    }

    if(parseArguments(&programConfig, argc, argv))
        return -1;

    if(openFiles(&programConfig))
        return -1;


    static const char* const requiredInstanceLayerExtensions[] = {
        "VK_LAYER_LUNARG_standard_validation",
        VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
        nullptr
    };

    static const char* const requiredDeviceExtension[] = {
#if defined(__linux) || defined(__linux__) || defined(linux)
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
#endif
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        nullptr
    };

    static const char* const optinalDeviceExtension[] = {
        VK_EXT_YCBCR_2PLANE_444_FORMATS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        nullptr
    };

    VulkanDeviceContext vkDevCtxt(programConfig.deviceId,
            programConfig.validate ? requiredInstanceLayerExtensions : nullptr,
            nullptr, // no instance extensions are required
            requiredDeviceExtension,
            optinalDeviceExtension);


    VkResult result = vkDevCtxt.InitVulkanDevice(programConfig.name.c_str(),
                                                 programConfig.verbose);
    if (result != VK_SUCCESS) {
        printf("Could not initialize the Vulkan device!\n");
        return -1;
    }

    result = vkDevCtxt.InitDebugReport(programConfig.validate,
                                       programConfig.validateVerbose);
    if (result != VK_SUCCESS) {
        return -1;
    }

    result = vkDevCtxt.InitPhysicalDevice((VK_QUEUE_TRANSFER_BIT |
                                           VK_QUEUE_VIDEO_ENCODE_BIT_KHR),
                                           nullptr);
    if (result != VK_SUCCESS) {

        assert(!"Can't initialize the Vulkan physical device!");
        return -1;
    }

    result = vkDevCtxt.CreateVulkanDevice(0,     // num decode queues
                                          1,     // num encode queues
                                          false, // createGraphicsQueue
                                          false, // createDisplayQueue
                                          false  // createComputeQueue
                                          );
    if (result != VK_SUCCESS) {

        assert(!"Failed to create Vulkan device!");
        return -1;
    }

    EncodeApp encodeApp(&vkDevCtxt);
    encodeApp.InitEncoder(&programConfig);

    // Encoding loop
    const bool logBatchEnc = programConfig.logBatchEncoding;
    const uint32_t batchSize = 8;
    const uint32_t numBatches = 2;
    assert((batchSize > 0) && !(batchSize & (batchSize - 1)));
    const uint32_t maxFramesInFlight = INPUT_FRAME_BUFFER_SIZE;
    assert(batchSize * numBatches <= maxFramesInFlight);
    uint32_t batchId = 0;
    uint32_t framesToProcess = programConfig.numFrames;
    if (logBatchEnc) fprintf(stdout, "programConfig.startFrame %d, totalFrames  %d, programConfig.endFrame  %d\n", programConfig.startFrame, framesToProcess, programConfig.numFrames);
    uint32_t firstAsmBufferIdx = 0;
    uint32_t numAsmBuffers = 0;
    uint32_t curFrameIndex = 0;
    uint32_t asmFrameIndex = 0;

    // Encoding loop
    while (framesToProcess || numAsmBuffers) {

        if (logBatchEnc) fprintf(stdout, "####################################################################################\n");
        if (logBatchEnc) fprintf(stdout, "Process framesToProcess %d, numAsmBuffers %d\n", framesToProcess, numAsmBuffers);

        // 1. Process the first/next batch of encode frames
        // #################################################################################################################
        uint32_t numFramesLoadRecordCmd = std::min((uint32_t)batchSize, framesToProcess);
        assert(numFramesLoadRecordCmd <= batchSize);
        uint32_t firstLoadRecordCmdIndx = batchId * batchSize;

        if (logBatchEnc) fprintf(stdout, "### Load and record command buffer for encoder batchId %d, numFramesLoadRecordCmd %d ###\n", batchId, numFramesLoadRecordCmd);
        for(uint32_t cpuBatchIdx = 0; cpuBatchIdx < numFramesLoadRecordCmd; cpuBatchIdx++) {
            const uint32_t cpuFrameBufferIdx = firstLoadRecordCmdIndx + cpuBatchIdx;
            if (logBatchEnc) fprintf(stdout, "\tloadFrame curFrameIndex %d, cpuBatchIdx %d, cpuFrameBufferIdx %d\n", curFrameIndex, cpuBatchIdx, cpuFrameBufferIdx);
            // load frame data for the current frame index
            encodeApp.LoadFrame(&programConfig, curFrameIndex, cpuFrameBufferIdx);
            if (logBatchEnc) fprintf(stdout, "\tRecord frame curFrameIndex %d, cpuBatchIdx %d, cpuFrameBufferIdx %d\n", curFrameIndex, cpuBatchIdx, cpuFrameBufferIdx);
            // encode frame for the current frame index
            encodeApp.EncodeFrame(&programConfig, curFrameIndex, (curFrameIndex == 0), cpuFrameBufferIdx);
            curFrameIndex++;
        }
        // #################################################################################################################

        // 2. Submit the current batch to the encoder's queue
        // #################################################################################################################
        if (logBatchEnc) fprintf(stdout, "### Submit to the HW encoder batchId %d, numFramesLoadRecordCmd %d, firstLoadRecordCmdIndx %d ###\n", batchId, numFramesLoadRecordCmd, firstLoadRecordCmdIndx);
        // submit the current batch
        encodeApp.BatchSubmit(firstLoadRecordCmdIndx, numFramesLoadRecordCmd);
        // #################################################################################################################

        // 3. Assemble the frame data from the previous batch processing (if any) of the submitted to the HW encoded frames.
        // #################################################################################################################
        if (logBatchEnc) fprintf(stdout, "### Assemble firstAsmBufferIdx %d, numAsmBuffers %d ###\n", firstAsmBufferIdx, numAsmBuffers);
        for(uint32_t asmBufferIdx = firstAsmBufferIdx; asmBufferIdx < firstAsmBufferIdx + numAsmBuffers; asmBufferIdx++) {
            if (logBatchEnc) fprintf(stdout, "\tAssemble asmFrameIndex %d, asmBatchIdx %d\n", asmFrameIndex, asmBufferIdx);
            encodeApp.AssembleBitstreamData(&programConfig, (asmFrameIndex == 0), asmBufferIdx);
            asmFrameIndex++;
        }
        // #################################################################################################################

        // Assemble frames with submitted firstSubmitFrameId and batchSize.
        firstAsmBufferIdx = firstLoadRecordCmdIndx;
        numAsmBuffers     = numFramesLoadRecordCmd;

        framesToProcess -= numFramesLoadRecordCmd;

        assert(framesToProcess < programConfig.numFrames);

        // increment the current batchID
        batchId++;
        batchId %= numBatches;
        if (logBatchEnc) fprintf(stdout, "####################################################################################\n\n");
    }

    if(closeFiles(&programConfig))
        return -1;

    return 0;
}
