#!/bin/bash
# Build the Vulkan Video Samples project on a remote VM via SSH

VM_USER="tzlatinski"
VM_HOST="192.168.122.216"
VM_SSH="${VM_USER}@${VM_HOST}"

echo "BUILDING Vulkan Video Samples project ..."

ssh ${VM_SSH} << 'REMOTE_COMMANDS'
cd /data/nvidia/android-extra/video-apps/vulkan-video-samples

# Create build directory if it doesn't exist
mkdir -p build
cd build

# Configure with CMake
echo "Configuring with CMake..."
# Compile for Ada (8.9) and Blackwell (9.0) GPUs
# Also include 8.6 (Ampere GA102/GA104) for broader compatibility
# Note: NV_AQ_GPU_LIB path will automatically resolve the required include directories:
#   - aq_common/interface (for EncodeAqAnalyzes.h)
#   - aq_common/inc (for AqProcessor.h and other headers)
#   - aq-vulkan/inc (for VulkanGpuTemporalAQ.h and Vulkan-specific headers)
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CUDA_ARCHITECTURES="86;89;90" -DNV_AQ_GPU_LIB="/data/nvidia/vulkan/video/algo/aq-vulkan"

# Build
echo "Building..."
make -j$(nproc)

# Check if build succeeded
if [ $? -eq 0 ]; then
    echo "Build completed successfully!"
    echo "Executables in: $(pwd)"
    ls -la vulkan_test_real_content 2>/dev/null || echo "Executable: vulkan_test_real_content"
else
    echo "Build failed!"
    exit 1
fi
REMOTE_COMMANDS
