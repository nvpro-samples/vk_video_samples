/*
* Copyright 2024 NVIDIA Corporation.
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

#ifndef _VKCODECUTILS_VKVIDEOQUEUE_H_
#define _VKCODECUTILS_VKVIDEOQUEUE_H_

#include <stdint.h>
#include "VkCodecUtils/VkVideoRefCountBase.h"

/**
 * @class VkVideoQueue
 * @brief Interface for retrieving frames from a Vulkan-based video queue.
 *
 * This class provides a mechanism to access decoded frames (`FrameDataType`) and to manage
 * those frames once the application is done processing them. The methods to query frame
 * properties (e.g., width, height, bit depth, and format) may return valid results
 * immediately after the decoder instance is created if the underlying
 * `videoStreamDemuxer` supports video stream probing. Otherwise, these methods will return
 * the correct format information only after sufficient data has been parsed by
 * ParserProcessNextDataChunk().
 *
 * @tparam FrameDataType The type representing a decoded frame data type.
 */

template<class FrameDataType>
class VkVideoQueue : public VkVideoRefCountBase {
public:

    /**
     * @brief Retrieves the current stream coded picture width of the video frames.
     *
     * Depending on the demuxer's implementation, this value may be
     * available immediately after the decoder is instantiated (if probing is
     * supported). Otherwise, it will be valid after parsing enough data
     * with ParserProcessNextDataChunk() to determine the stream parameters.
     *
     * @return The width of each decoded frame, in pixels.
     *         If the coded width is undetermined yet, GetWidth() will return "-1".
     */
    virtual int32_t  GetWidth()            const = 0;

    /**
     * @brief Retrieves the current stream coded picture height of the video frames.
     *
     * Depending on the demuxer's implementation, this value may be
     * available immediately after the decoder is instantiated (if probing is
     * supported). Otherwise, it will be valid after parsing enough data
     * with ParserProcessNextDataChunk() to determine the stream parameters.
     *
     * @return The height of each decoded frame, in pixels.
     *         If the coded height is undetermined yet, GetHeight() will return "-1".
     */
    virtual int32_t  GetHeight()           const = 0;

    /**
     * @brief Retrieves the coded bit depth of the video frames.
     *
     * Depending on the demuxer's implementation, this value may be
     * available immediately after the decoder is instantiated (if probing is
     * supported). Otherwise, it will be valid after parsing enough data to
     * determine the stream parameters.
     *
     * @return The bit depth (e.g., 8, 10, 12) of each decoded frame.
     *         If the coded bit depth is undetermined yet, GetBitDepth() will return "-1".
     */
    virtual int32_t  GetBitDepth()         const = 0;

    /**
     * @brief Retrieves the Vulkan image format of the video frames.
     *
     * Depending on the demuxer's implementation, this value may be
     * available immediately after the decoder is instantiated (if probing is
     * supported). Otherwise, it will be valid after parsing enough data to
     * determine the stream parameters.
     *
     * @return A VkFormat enumeration value indicating the image format.
     *         If the frame Vulkan image format is undetermined yet, GetFrameImageFormat() will return VK_FORMAT_UNDEFINED.
     */
    virtual VkFormat GetFrameImageFormat() const = 0;

    /**
     * @brief Retrieve the Vulkan video profile information.
     *
     * Similar to `GetWidth()`, `GetHeight()`, etc., this function may return valid data immediately
     * if the underlying demuxer supports probing of the stream format. Otherwise, it may only
     * become valid once sufficient data has been parsed (e.g., by calling `ParserProcessNextDataChunk()`).
     *
     * @return A `VkVideoProfileInfoKHR` structure describing the video profile in use.
     */
    virtual VkVideoProfileInfoKHR GetVkProfile() const  = 0;

    /**
     * @brief Retrieve the profile IDC value indicating the specific profile of the coded video.
     *
     * Like the other video property queries (`GetWidth()`, `GetHeight()`, etc.), this value may be
     * valid upon decoder creation if probing is available, or only valid after the stream has been
     * partially parsed.
     *
     * @return A `uint32_t` representing the profile IDC (implementation-specific meaning or mapping
     *         to a standard profile identifier).
     */
    virtual uint32_t GetProfileIdc() const = 0;

    /**
     * @brief Retrieve the video extent, typically containing width, height, and depth dimensions.
     *
     * This function may return valid values immediately if the demuxer can probe the stream
     * format; otherwise, valid results may only be obtained after partial parsing of the stream.
     *
     * @return A `VkExtent3D` structure specifying the width, height, and depth of the video frames.
     *         Typically, the `depth` field is set to 1 for most 2D video streams.
     */
    virtual VkExtent3D GetVideoExtent() const = 0;

    /**
     * @brief Retrieve the next decoded frame from the queue in display order.
     *
     * This method returns a decoded frame if one is available. Decoded frames may be delayed due to
     * B-frames (bi-directional predicted frames) reordering, or simply because not enough stream data
     * has been processed (e.g., insufficient data in `ParserProcessNextDataChunk()` calls). The frames
     * returned are in display order, which may differ from the bitstream order when B-frames are used.
     *
     * @param[out] pNewFrame   Pointer to a `FrameDataType` that will be populated with
     *                         information about the newly decoded frame, if available.
     *                         If no frame is currently available or it was the end of the stream,
     *                         there will be no data filled in pNewFrame.
     * @param[out] endOfStream Set to `true` if the end of the stream has been reached (i.e., no more
     *                         frames will ever be available), or `false` otherwise.
     *
     * @return
     * - `1` if a decoded frame is successfully retrieved.
     * - `0` if no frame is currently available (but not an error, you may need to parse more data or
     *        wait for pending frames to be reordered because of the B-frames are present).
     * - `-1` if an error occurs or if the end of the stream is reached. In either case, decoding should
     *         be terminated or reset accordingly.
     */
    virtual int32_t  GetNextFrame(FrameDataType* pNewFrame, bool* endOfStream) = 0;

    /**
     * @brief Release a previously retrieved decoded frame.
     *
     * This method must be called for every frame obtained via `GetNextFrame()` once
     * the frame is no longer needed by the client (e.g., after rendering or further
     * processing). The decoder can then reuse or free any associated resources.
     *
     * The pointer passed in `pFrameDone` must be the same pointer received from
     * `GetNextFrame()`. If frames are used with a signaling mechanism (e.g., fence
     * or semaphore), the client can set additional fields such as `hasConsummerSignalFence`
     * or `hasConsummerSignalSemaphore` in the `FrameDataType` to indicate that the
     * frame data will be consumed upon signaling of the corresponding fence or
     * semaphore by the consumer. The decoder or any related pipeline might use this
     * information to coordinate resource reuse or disposal.
     *
     * @param[in] pFrameDone Pointer to the `FrameDataType` that was previously returned
     *                       by `GetNextFrame()` and is now ready to be released.
     *
     * @return
     * - 0 on successful release of the frame.
     * - A non-zero value indicates an error or special condition (implementation-defined).
     */
    virtual int32_t  ReleaseFrame(FrameDataType* pFrameDone) = 0;

    virtual ~VkVideoQueue() {};
};

#endif /* _VKCODECUTILS_VKVIDEOQUEUE_H_ */
