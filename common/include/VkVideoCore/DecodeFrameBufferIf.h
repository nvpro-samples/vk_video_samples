/*
 * Copyright 2023-2024 NVIDIA Corporation.
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

#ifndef _VKVIDEOCORE_DECODEFRAMEBUFFERIF_H_
#define _VKVIDEOCORE_DECODEFRAMEBUFFERIF_H_

class DecodeFrameBufferIf
{
public:
    enum { MAX_PER_FRAME_IMAGE_TYPES = 4};

    enum ImageTypeIdx : uint8_t  { IMAGE_TYPE_IDX_DPB             = 0,
                                   IMAGE_TYPE_IDX_OUT             = 1,
                                   IMAGE_TYPE_IDX_FILTER          = 2,
                                   IMAGE_TYPE_IDX_INVALID         = (uint8_t)-1,
                                 };

    enum ImageType : uint8_t     { IMAGE_TYPE_MASK_DPB             = (1 << IMAGE_TYPE_IDX_DPB),
                                   IMAGE_TYPE_MASK_OUTPUT          = (1 << IMAGE_TYPE_IDX_OUT),
                                   IMAGE_TYPE_MASK_FILTER          = (1 << IMAGE_TYPE_IDX_FILTER),
                                   IMAGE_TYPE_MASK_ALL             = ( IMAGE_TYPE_MASK_DPB |
                                                                       IMAGE_TYPE_MASK_OUTPUT |
                                                                       IMAGE_TYPE_MASK_FILTER),
                                   IMAGE_TYPE_MASK_NONE            = 0,
                                 };

    struct ImageViews {
        ImageViews()
        : view()
        , singleLevelView()
        , inUse(false) {}

        VkSharedBaseObj<VkImageResourceView>  view;
        VkSharedBaseObj<VkImageResourceView>  singleLevelView;
        uint32_t inUse : 1;

        bool GetImageResourceView(VkSharedBaseObj<VkImageResourceView>& imageResourceView)
        {
            if (!inUse) {
                return false;
            }

            if (singleLevelView != nullptr) {
                imageResourceView = singleLevelView;
                return true;
            }

            if (view != nullptr) {
                imageResourceView = view;
                return true;
            }

            return false;
        }
    };

};

#endif /* _VKVIDEOCORE_DECODEFRAMEBUFFERIF_H_ */
