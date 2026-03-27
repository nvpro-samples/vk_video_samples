#ifndef VKVS_COMMON_H
#define VKVS_COMMON_H

// Common definitions for Vulkan Video Samples

#ifdef __cplusplus
extern "C" {
#endif

// Version information
#define VKVS_VERSION_MAJOR 0
#define VKVS_VERSION_MINOR 4
#define VKVS_VERSION_PATCH 2

// Helper macros for version string construction (prefixed to avoid reserved identifier issues)
#define VKVS_STRINGIFY(x) #x
#define VKVS_VERSION_STRING_IMPL(major,minor,patch) VKVS_STRINGIFY(major) "." VKVS_STRINGIFY(minor) "." VKVS_STRINGIFY(patch)

#define VKVS_VERSION_STRING \
    VKVS_VERSION_STRING_IMPL(VKVS_VERSION_MAJOR,VKVS_VERSION_MINOR,VKVS_VERSION_PATCH)

// Include stdlib.h for standard exit codes
#include <stdlib.h>

// Include sysexits.h for extended exit codes (POSIX systems)
// On Windows, define the exit codes manually as sysexits.h is not available
#ifndef _WIN32
#include <sysexits.h>
#else
// Define EX_UNAVAILABLE for Windows (service unavailable)
#define EX_UNAVAILABLE 69
#endif

// Standard exit codes for Vulkan Video applications
// Use EXIT_SUCCESS and EXIT_FAILURE from stdlib.h
// EX_UNAVAILABLE (69) indicates a required service is unavailable
// This is used when video codec features are not supported by hardware/driver
#define VVS_EXIT_UNSUPPORTED  EX_UNAVAILABLE

#ifdef __cplusplus
} // extern "C"

// Macro to check Vulkan features and return error if not supported
// Note: This macro contains a return statement - use with care
#define CHECK_VULKAN_FEATURE(feature, name, optional) \
    do { \
        if (!(feature)) { \
            std::cerr << ((optional) ? "WARNING: " : "ERROR: ") << (name) << " feature not supported" << std::endl; \
            if (!(optional)) { \
                return VK_ERROR_FEATURE_NOT_PRESENT; \
            } \
        } \
    } while(0)

// Stringification macros (prefixed to avoid collisions with common macro names)
#ifndef VKVS_CASE_STR
#define VKVS_CASE_STR(x) case x: return VKVS_STRINGIFY(x)
#endif

// Helper function to check if a VkResult indicates video profile/feature not supported
// Returns true for video-specific KHR errors (profile, format, codec, std version)
// and general Vulkan capability errors (format, feature, driver, extension)
inline bool IsVideoUnsupportedResult(VkResult result) {
    return result == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR ||
           result == VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR ||
           result == VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR ||
           result == VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR ||
           result == VK_ERROR_FORMAT_NOT_SUPPORTED ||
           result == VK_ERROR_FEATURE_NOT_PRESENT ||
           result == VK_ERROR_INCOMPATIBLE_DRIVER ||
           result == VK_ERROR_EXTENSION_NOT_PRESENT;
}

inline int ExitCodeFromVkResult(VkResult result) {
    if (IsVideoUnsupportedResult(result)) {
        return VVS_EXIT_UNSUPPORTED;
    }
    return (result == VK_SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif // __cplusplus

#endif // VKVS_COMMON_H
