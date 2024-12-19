/*
* Copyright 2020 NVIDIA Corporation.
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

#include <stdint.h>
#include "vulkan_interfaces.h"
#include "nvidia_utils/vulkan/ycbcrvkinfo.h"
#include "nvidia_utils/vulkan/ycbcrinfotbl.h"

const VkFormatDesc vkFormatInfo[] = {
    { VK_FORMAT_R8_UNORM,                       1,   1,        "r8",                },
    { VK_FORMAT_R8G8_UNORM,                     2,   2,        "rg8",               },
    { VK_FORMAT_R8G8B8_UNORM,                   3,   3,        "rgb8",              },
    { VK_FORMAT_R8G8B8A8_UNORM,                 4,   4,        "rgba8",             },
    { VK_FORMAT_R32G32B32A32_SFLOAT,            4,  16,       "rgba32f",            },
    { VK_FORMAT_R16G16B16A16_SFLOAT,            4,   8,        "rgba16f",           },
    { VK_FORMAT_R32G32_SFLOAT,                  2,   8,        "rg32f",             },
    { VK_FORMAT_R16G16_SFLOAT,                  2,   4,        "rg16f",             },
    { VK_FORMAT_B10G11R11_UFLOAT_PACK32,        3,   4,        "r11f_g11f_b10f",    },
    { VK_FORMAT_R32_SFLOAT,                     1,   4,        "r32f",              },
    { VK_FORMAT_R16_SFLOAT,                     1,   2,        "r16f",              },
    { VK_FORMAT_R16G16B16A16_UNORM,             4,   8,        "rgba16",            },
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32,       4,   4,        "rgb10_a2",          },
    { VK_FORMAT_R16G16_UNORM,                   2,   4,        "rg16",              },
    { VK_FORMAT_R16_UNORM,                      1,   2,        "r16",               },
    { VK_FORMAT_R16G16B16A16_SNORM,             4,   8,        "rgba16_snorm",      },
    { VK_FORMAT_R8G8B8A8_SNORM,                 4,   4,        "rgba8_snorm",       },
    { VK_FORMAT_R16G16_SNORM,                   2,   4,        "rg16_snorm",        },
    { VK_FORMAT_R8G8_SNORM,                     2,   2,        "rg8_snorm",         },
    { VK_FORMAT_R16_SNORM,                      1,   2,        "r16_snorm",         },
    { VK_FORMAT_R8_SNORM,                       1,   1,        "r8_snorm",          },
    { VK_FORMAT_R32G32B32A32_SINT,              4,  16,       "rgba32i",            },
    { VK_FORMAT_R16G16B16A16_SINT,              4,   8,        "rgba16i",           },
    { VK_FORMAT_R8G8B8A8_SINT,                  4,   4,        "rgba8i",            },
    { VK_FORMAT_R32G32_SINT,                    2,   8,        "rg32i",             },
    { VK_FORMAT_R16G16_SINT,                    2,   4,        "rg16i",             },
    { VK_FORMAT_R8G8_SINT,                      2,   2,        "rg8i",              },
    { VK_FORMAT_R32_SINT,                       1,   4,        "r32i",              },
    { VK_FORMAT_R16_SINT,                       1,   2,        "r16i",              },
    { VK_FORMAT_R8_SINT,                        1,   1,        "r8i",               },
    { VK_FORMAT_R32G32B32A32_UINT,              4,  16,       "rgba32ui",           },
    { VK_FORMAT_R16G16B16A16_UINT,              4,   8,        "rgba16ui",          },
    { VK_FORMAT_R8G8B8A8_UINT,                  4,   4,        "rgba8ui",           },
    { VK_FORMAT_R32G32_UINT,                    2,   8,        "rg32ui",            },
    { VK_FORMAT_R16G16_UINT,                    2,   4,        "rg16ui",            },
    { VK_FORMAT_R8G8_UINT,                      2,   2,        "rg8ui",             },
    { VK_FORMAT_R32_UINT,                       1,   4,        "r32ui",             },
    { VK_FORMAT_R16_UINT,                       1,   2,        "r16ui",             },
    { VK_FORMAT_R8_UINT,                        1,   1,        "r8ui",              },
    { VK_FORMAT_A2B10G10R10_UINT_PACK32,        4,   4,        "rgb10_a2ui",        },
};

const VkFormatDesc* vkFormatLookUp(VkFormat format)
{
    const VkFormatDesc* pVkFormatDesc = NULL;
    for (unsigned int i = 0; i < sizeof(vkFormatInfo)/sizeof(vkFormatInfo[0]); i++) {
        if (vkFormatInfo[i].format == format) {
            pVkFormatDesc = &vkFormatInfo[i];
            break;
        }
    }

    return pVkFormatDesc;
}

const VkMpFormatInfo* YcbcrVkFormatInfo(const VkFormat format)
{
    return __ycbcrVkFormatInfo(format);
}
