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
 * A stand-in for a gated test binary: it prints what the loader and the validation layer print for
 * one case, then exits 0, so that cmake/validation_gate.cmake can be tested without a device. The
 * messages are the layer's, in the format both of its reporters use: a "Validation Error: [ <id> ]"
 * header, the message, and for a message with a VUID a "The Vulkan spec states" trailer.
 */

#include <cstdio>
#include <cstring>

namespace {

void PrintLayerInserted() {
    std::printf("[Vulkan Loader] INFO | LAYER:   Inserted instance layer \"VK_LAYER_KHRONOS_validation\"\n");
}

void PrintThreadingError(const char* severity, const char* text) {
    std::printf("Validation %s: [ UNASSIGNED-Threading-MultipleThreads-Write ] | MessageID = 0xa05b236e\n", severity);
    std::printf("%s\n\n", text);
}

void PrintUndefinedValueWarning() {
    std::printf("Validation Warning: [ Undefined-Value-StorageImage-FormatMismatch-ImageView ] | MessageID = 0x13365b2\n");
    std::printf(
        "vkCmdDispatch(): the storage image descriptor [Set 0, Binding 4, Index 0, variable \"outputImageRGB\"] is "
        "accessed by a OpTypeImage that has a Format operand Rgba16 (equivalent to VK_FORMAT_R16G16B16A16_UNORM) which "
        "doesn't match the VkImageView format (VK_FORMAT_R8G8B8A8_UNORM).\n\n");
}

void PrintDispatchError() {
    std::printf("Validation Error: [ VUID-vkCmdDispatch-None-08114 ] | MessageID = 0x30b6e267\n");
    std::printf("vkCmdDispatch(): the descriptor set 0 binding 9 is not updated.\n");
    std::printf(
        "The Vulkan spec states: Descriptors in each bound descriptor set must be valid "
        "(https://docs.vulkan.org/spec/latest/chapters/dispatch.html#VUID-vkCmdDispatch-None-08114)\n\n");
}

}  // namespace

int main(int argc, char** argv) {
    const char* const usage =
        "usage: %s unassigned-error | unassigned-warning | unassigned-allowed | vuid-error | undefined-value-warning\n";
    if (argc != 2) {
        std::printf(usage, argv[0]);
        return 2;
    }
    const char* const which = argv[1];
    PrintLayerInserted();
    if (std::strcmp(which, "unassigned-error") == 0) {
        PrintThreadingError("Error",
                            "vkBeginCommandBuffer(): THREADING ERROR : object of type VkCommandPool is simultaneously "
                            "used in current thread 1 and thread 2");
    } else if (std::strcmp(which, "unassigned-warning") == 0) {
        PrintThreadingError("Warning", "vkBeginCommandBuffer(): a warning without a VUID");
    } else if (std::strcmp(which, "unassigned-allowed") == 0) {
        PrintThreadingError("Error", "vkCreateDevice(): pCreateInfo->pNext chain includes an unknown VkStructureType");
    } else if (std::strcmp(which, "vuid-error") == 0) {
        PrintDispatchError();
    } else if (std::strcmp(which, "undefined-value-warning") == 0) {
        PrintUndefinedValueWarning();
    } else {
        std::printf(usage, argv[0]);
        return 2;
    }
    return 0;
}
