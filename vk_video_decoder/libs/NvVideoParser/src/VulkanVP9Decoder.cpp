/*
* Copyright 2023 NVIDIA Corporation.
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

#include "VulkanVideoParserIf.h"

#include "VulkanVP9Decoder.h"

VulkanVP9Decoder::VulkanVP9Decoder(VkVideoCodecOperationFlagBitsKHR std)
    : VulkanVideoDecoder(std)
    , m_PicData()
    , m_pCurrPic()
    , m_frameIdx(-1)
    , m_dataSize()
    , m_frameSize()
    , m_frameSizeChanged()
    , m_rtOrigWidth()
    , m_rtOrigHeight()
    , m_pictureStarted()
    , m_bitstreamComplete(true)
    , m_lastFrameWidth(0)
    , m_lastFrameHeight(0)
    , m_lastShowFrame(false)
    , m_pBuffers() {
}

VulkanVP9Decoder::~VulkanVP9Decoder()
{
}

void VulkanVP9Decoder::InitParser()
{
    m_bNoStartCodes = true;
    m_bEmulBytesPresent = false;
    m_pCurrPic = nullptr;
    m_bitstreamComplete = true;
    m_pictureStarted = false;
    EndOfStream();
}

void VulkanVP9Decoder::EndOfStream()
{
    if (m_pCurrPic) {
        m_pCurrPic->Release();
        m_pCurrPic = nullptr;
    }
    for (int i = 0; i < 8; i++) {
        if (m_pBuffers[i].buffer) {
            m_pBuffers[i].buffer->Release();
            m_pBuffers[i].buffer = nullptr;
        }
    }
}

bool VulkanVP9Decoder::ParseByteStream(const VkParserBitstreamPacket* pck, size_t* pParsedBytes)
{
    const uint8_t* pDataIn = pck->pByteStream;
    int dataSize = (int)pck->nDataLength;

    if (pParsedBytes) {
        *pParsedBytes = 0;
    }

    // Use different bitstreamBuffer than the previous frames bitstreamBuffer
    // TODO: Make sure that the bitstreamBuffer is not in use.
    VkSharedBaseObj<VulkanBitstreamBuffer> bitstreamBuffer;
    assert(m_pClient);
    m_pClient->GetBitstreamBuffer(m_bitstreamDataLen,
                                  m_bufferOffsetAlignment, m_bufferSizeAlignment,
                                  nullptr, 0, bitstreamBuffer);
    assert(bitstreamBuffer);
    if (!bitstreamBuffer) {
        return false;
    }
    m_bitstreamDataLen = m_bitstreamData.SetBitstreamBuffer(bitstreamBuffer);
    m_bitstreamData.ResetStreamMarkers();

    if (m_bitstreamData.GetBitstreamBuffer() == nullptr) {
        // make sure we're initialized
        return false;
    }

    m_nCallbackEventCount = 0;

    // Handle discontinuity
    if (pck->bDiscontinuity) {
        memset(&m_nalu, 0, sizeof(m_nalu));
        memset(&m_PTSQueue, 0, sizeof(m_PTSQueue));
        m_bDiscontinuityReported = true;
        m_pictureStarted = false;
    }

    if (pck->bPTSValid) {
        m_PTSQueue[m_lPTSPos].bPTSValid = true;
        m_PTSQueue[m_lPTSPos].llPTS = pck->llPTS;
        m_PTSQueue[m_lPTSPos].llPTSPos = m_llParsedBytes;
        m_PTSQueue[m_lPTSPos].bDiscontinuity = m_bDiscontinuityReported;
        m_bDiscontinuityReported = false;
        m_lPTSPos = (m_lPTSPos + 1) % MAX_QUEUED_PTS;
    }

    if (pck->pByteStream && pck->nDataLength && m_frameIdx == -1) {
        memset(&m_PicData, 0, sizeof(VkParserVp9PictureData));
        m_frameIdx++;
    }

    while ((dataSize > 0) || m_pictureStarted) {
        if (!m_pictureStarted) {
            if (m_bitstreamComplete) {
                // fill bitstreambuffer from start
                //  assuming parser will get bitstream per frame from demuxer
                m_frameSize = dataSize;
                m_nalu.start_offset = 0;
                m_nalu.end_offset = 0;
            }
            if (((VkDeviceSize)dataSize > m_bitstreamDataLen) && !resizeBitstreamBuffer(dataSize - m_bitstreamDataLen)) {
                return false;
            }

            if (dataSize >= (m_frameSize - m_nalu.end_offset)) {
                memcpy(m_bitstreamData.GetBitstreamPtr() + m_nalu.end_offset, pDataIn, (size_t)(m_frameSize - m_nalu.end_offset));
                m_pictureStarted = true;
                pDataIn += (m_frameSize - (int)m_nalu.end_offset);
                dataSize -= (m_frameSize - (int)m_nalu.end_offset);
                m_nalu.end_offset = m_frameSize;
                m_bitstreamComplete = true;
            } else {
                memcpy(m_bitstreamData.GetBitstreamPtr() + m_nalu.end_offset, pDataIn, (size_t)(dataSize));
                m_nalu.end_offset += dataSize;
                pDataIn += dataSize;
                dataSize = 0;
                m_bitstreamComplete = false;
            }
        } else {
            uint32_t frames_processed = 0;
            uint32_t sizeparsed = 0, framesdone = 0;

            uint32_t frame_size = m_frameSize;

            const uint8_t* data_start = m_bitstreamData.GetBitstreamPtr();
            const uint8_t* data_end = data_start + m_frameSize;
            uint32_t data_size = m_frameSize;
            uint32_t frames_in_superframe, frame_sizes[8];

            ParseSuperFrameIndex(data_start, data_size, frame_sizes, &frames_in_superframe);

            do {
                // Skip over the superframe index, if present
                if ((data_size > 0) && ((data_start[0] & 0xe0) == 0xc0)) {
                    const uint8_t marker = data_start[0];
                    const uint32_t frames = (marker & 0x7) + 1;
                    const uint32_t mag = ((marker >> 3) & 0x3) + 1;
                    const uint32_t index_sz = 2 + mag * frames;

                    if ((data_size >= index_sz) && (data_start[index_sz - 1] == marker)) {
                        data_start += index_sz;
                        data_size -= index_sz;
                        if (data_start < data_end) {
                            continue;
                        } else {
                            break;
                        }
                    }
                }

                // Use the correct size for this frame, if an index is present
                if (frames_in_superframe > 0) {
                    frame_size = frame_sizes[frames_processed];
                    if (data_size < frame_size) {
                        // Invalid frame size in index
                        return false;
                    }
                    data_size = frame_size;
                    m_nalu.start_offset = sizeparsed;

                }

                ParseFrameHeader(frame_size);

                if (frames_in_superframe > 0) {
                    sizeparsed += frame_sizes[framesdone];
                    framesdone++;
                }
                data_start += data_size;
                while (data_start < data_end && *data_start == 0) {
                    data_start++;
                }

                data_size = (int)(data_end - data_start);
                frames_processed += 1;
            } while (data_start < data_end);

            m_frameIdx++;
            m_pictureStarted = false;
        }

    }

    if (pck->bEOS) {
        end_of_stream();
    }

    if (pParsedBytes) {
        *pParsedBytes = pck->nDataLength;
    }

    return true;
}


bool VulkanVP9Decoder::ParseFrameHeader(uint32_t framesize)
{
    m_llNaluStartLocation = m_llParsedBytes;
    m_llFrameStartLocation = m_llNaluStartLocation;
    m_llParsedBytes += framesize;
    //m_pSliceOffsets[0] = 0;

    init_dbits();
    //parse uncompressed header
    if(!ParseUncompressedHeader())
    {
        assert((!"Error in ParseUncompressedVP9\n"));
        return 0;
    }
    if (m_PicData.show_existing_frame == true)  {
        // display an existing frame
        VkPicIf* pDispPic = m_pBuffers[m_PicData.frame_to_show_map_idx].buffer;
        if (pDispPic) {
            pDispPic->AddRef();
        }

        AddBuffertoOutputQueue(pDispPic);

        return 0;
    }

    // handle bitstream start offset alignment (for super frame)
    assert((m_nalu.start_offset >= 0) && (m_nalu.start_offset <= UINT32_MAX));
    uint32_t addOffset = (uint32_t)(m_nalu.start_offset & (m_bufferOffsetAlignment - 1));
    m_PicData.uncompressedHeaderOffset += addOffset;
    m_PicData.compressedHeaderOffset += addOffset;
    m_PicData.tilesOffset += addOffset;

    *m_pVkPictureData = VkParserPictureData();
    m_pVkPictureData->CodecSpecific.vp9 = m_PicData;
    m_pVkPictureData->numSlices = m_PicData.numTiles;
    m_pVkPictureData->bitstreamDataLen = (framesize + addOffset + m_bufferSizeAlignment - 1) & ~(m_bufferSizeAlignment - 1); // buffer is already aligned so, no issues.
    m_pVkPictureData->bitstreamData = m_bitstreamData.GetBitstreamBuffer();
    m_pVkPictureData->bitstreamDataOffset = (size_t)(m_nalu.start_offset & ~((int64_t)m_bufferOffsetAlignment - 1));

    if (!BeginPicture(m_pVkPictureData)) {
        assert(!"BeginPicture failed");
        return false;
    }

    bool bSkipped = false;
    if (m_pClient != nullptr) {
        // Notify client
        if (!m_pClient->DecodePicture(m_pVkPictureData)) {
            bSkipped = true;
            // WARNING: skipped decoding current picture;
        } else {
            m_nCallbackEventCount++;
        }
    } else {
        // WARNING: no valid render target for current picture
    }

    //m_PicData.prevIsKeyFrame = m_PicData.keyFrame;
    //m_PicData.PrevShowFrame  = m_PicData.showFrame;
    UpdateFramePointers(m_pCurrPic);

    if (m_PicData.stdPictureInfo.flags.show_frame && !bSkipped) {
        // Call back codec for post-decode event (display the decoded frame)
        AddBuffertoOutputQueue(m_pCurrPic);
        m_pCurrPic = nullptr;
    } else {
        m_pCurrPic->Release();
        m_pCurrPic = nullptr;
    }

    return 1;
}

void VulkanVP9Decoder::UpdateFramePointers(VkPicIf* currentPicture)
{
    StdVideoDecodeVP9PictureInfo* const pStdPicInfo = &m_PicData.stdPictureInfo;

    uint32_t mask, ref_index = 0;

    for (mask = pStdPicInfo->refresh_frame_flags; mask; mask >>= 1) {
        if (mask & 1) {
            if (m_pBuffers[ref_index].buffer) {
                m_pBuffers[ref_index].buffer->Release();
            }
            m_pBuffers[ref_index].buffer = currentPicture;

            if (m_pBuffers[ref_index].buffer) {
                m_pBuffers[ref_index].buffer->AddRef();
            }
        }
        ++ref_index;
    }

    // Invalidate these references until the next frame starts.
    //for (int i = 0; i < ALLOWED_REFS_PER_FRAME; i++) {
    //    pFrameInfo->activeRefIdx[i] = 0xffff;
    //}
}

bool VulkanVP9Decoder::AddBuffertoOutputQueue(VkPicIf* pDispPic)
{
    AddBuffertoDispQueue(pDispPic);
    lEndPicture(pDispPic);

    return true;
}

void VulkanVP9Decoder::AddBuffertoDispQueue(VkPicIf* pDispPic)
{
    int lDisp = 0;

    // Find an entry in m_DispInfo
    for (int i = 0; i < MAX_DELAY; i++) {
        if (m_DispInfo[i].pPicBuf == pDispPic) {
            lDisp = i;
            break;
        }
        if ((m_DispInfo[i].pPicBuf == nullptr)
            || ((m_DispInfo[lDisp].pPicBuf != nullptr) && (m_DispInfo[i].llPTS - m_DispInfo[lDisp].llPTS < 0))) {
            lDisp = i;
        }
    }
    m_DispInfo[lDisp].pPicBuf = pDispPic;
    m_DispInfo[lDisp].bSkipped = false;
    m_DispInfo[lDisp].lPOC = 0;
    m_DispInfo[lDisp].lNumFields = 2;

    // Find a PTS in the list
    unsigned int ndx = m_lPTSPos;
    m_DispInfo[lDisp].llPTS = m_llExpectedPTS; // Will be updated later on

    for (int k = 0; k < MAX_QUEUED_PTS; k++) {
        if ((m_PTSQueue[ndx].bPTSValid) && (m_PTSQueue[ndx].llPTSPos - m_llFrameStartLocation <= (m_bNoStartCodes?0:3))) {
            m_DispInfo[lDisp].bPTSValid = true;
            m_DispInfo[lDisp].llPTS = m_PTSQueue[ndx].llPTS;
            m_PTSQueue[ndx].bPTSValid = false;
        }
        ndx = (ndx + 1) % MAX_QUEUED_PTS;
    }
}

void VulkanVP9Decoder::lEndPicture(VkPicIf* pDispPic)
{
    if (pDispPic) {
        display_picture(pDispPic);
        pDispPic->Release();
    }

}


bool VulkanVP9Decoder::ParseUncompressedHeader()
{
    VkParserVp9PictureData *pPicData = &m_PicData;
    StdVideoDecodeVP9PictureInfo* pStdPicInfo = &m_PicData.stdPictureInfo;
    StdVideoVP9ColorConfig* pStdColorConfig = &m_PicData.stdColorConfig;
    StdVideoVP9LoopFilter* pStdLoopFilter = &m_PicData.stdLoopFilter;
    m_frameSizeChanged = false;

    VP9_CHECK_FRAME_MARKER;

    uint32_t profile = u(1);
    profile |= u(1) << 1;
    pStdPicInfo->profile = (StdVideoVP9Profile)profile;
    if (pStdPicInfo->profile == STD_VIDEO_VP9_PROFILE_3) {
        if (u(1) != 0) {
            assert(!"Invalid syntax");
            return false;
        }
    }

    pPicData->show_existing_frame = u(1);
    if (pPicData->show_existing_frame) {
        pPicData->frame_to_show_map_idx = u(3);
        //U32 frame_to_show = vp9parser->m_pBuffers[idx_to_show];
        //Handle direct show:   CHECK
        pPicData->uncompressedHeaderOffset = (consumed_bits() + 7) >> 3;
        pPicData->compressedHeaderSize = 0;
        pStdPicInfo->refresh_frame_flags = 0;
        pStdLoopFilter->loop_filter_level = 0;
        return true;
    }

    pStdPicInfo->frame_type = (StdVideoVP9FrameType)u(1);
    pStdPicInfo->flags.show_frame = u(1);
    pStdPicInfo->flags.error_resilient_mode = u(1);

    if (pStdPicInfo->frame_type == STD_VIDEO_VP9_FRAME_TYPE_KEY) {
        VP9_CHECK_FRAME_SYNC_CODE;
        ParseColorConfig();
        ParseFrameAndRenderSize();
        pStdPicInfo->refresh_frame_flags = (1 << STD_VIDEO_VP9_NUM_REF_FRAMES) - 1;
        pPicData->FrameIsIntra = true;

        for (int i = 0; i < STD_VIDEO_VP9_REFS_PER_FRAME; ++i) {
            pPicData->ref_frame_idx[i] = 0;
        }
    } else { // non key frame
        pStdPicInfo->flags.intra_only = pStdPicInfo->flags.show_frame ? 0 : u(1);
        pPicData->FrameIsIntra = pStdPicInfo->flags.intra_only;
        pStdPicInfo->reset_frame_context = pStdPicInfo->flags.error_resilient_mode ? 0 : u(2);

        if (pStdPicInfo->flags.intra_only == 1) {
            VP9_CHECK_FRAME_SYNC_CODE;
            if (pStdPicInfo->profile > STD_VIDEO_VP9_PROFILE_0) {
                ParseColorConfig();
            } else {
                pStdColorConfig->color_space = STD_VIDEO_VP9_COLOR_SPACE_BT_601;
                pStdColorConfig->subsampling_x = 1;
                pStdColorConfig->subsampling_y = 1;
                pStdColorConfig->BitDepth = 8;
            }

            pStdPicInfo->refresh_frame_flags = u(STD_VIDEO_VP9_NUM_REF_FRAMES); //for non key frame refresh only some

            ParseFrameAndRenderSize();
        } else { // inter frame
            pStdPicInfo->refresh_frame_flags = u(STD_VIDEO_VP9_NUM_REF_FRAMES);

            pStdPicInfo->ref_frame_sign_bias_mask = 0;
            for (int i = 0; i < STD_VIDEO_VP9_REFS_PER_FRAME; i++) {
                pPicData->ref_frame_idx[i] = u(3);
                pStdPicInfo->ref_frame_sign_bias_mask |= (u(1) << (STD_VIDEO_VP9_REFERENCE_NAME_LAST_FRAME + i));
            }

            ParseFrameAndRenderSizeWithRefs();

            pStdPicInfo->flags.allow_high_precision_mv = u(1);

            // interpolation filter
            bool is_filter_switchable = u(1); //mb_switchable_mcomp_filt
            if (is_filter_switchable) {
                pStdPicInfo->interpolation_filter = STD_VIDEO_VP9_INTERPOLATION_FILTER_SWITCHABLE;
            } else {
                const StdVideoVP9InterpolationFilter literal_to_filter[] = {
                                            STD_VIDEO_VP9_INTERPOLATION_FILTER_EIGHTTAP_SMOOTH,
                                            STD_VIDEO_VP9_INTERPOLATION_FILTER_EIGHTTAP,
                                            STD_VIDEO_VP9_INTERPOLATION_FILTER_EIGHTTAP_SHARP,
                                            STD_VIDEO_VP9_INTERPOLATION_FILTER_BILINEAR };
                pStdPicInfo->interpolation_filter = literal_to_filter[u(2)];
            }
        }
    }

    if (pStdPicInfo->flags.error_resilient_mode == 0) {
         /* Refresh entropy probs,
         * 0 == this frame probs are used only for this frame decoding,
         * 1 == this frame probs will be stored for future reference */
        pStdPicInfo->flags.refresh_frame_context = u(1);
        pStdPicInfo->flags.frame_parallel_decoding_mode = u(1);
    } else {
        pStdPicInfo->flags.refresh_frame_context = 0;
        pStdPicInfo->flags.frame_parallel_decoding_mode = 1;
    }

    pStdPicInfo->frame_context_idx = u(2);

    if ((pPicData->FrameIsIntra == 1) || (pStdPicInfo->flags.error_resilient_mode == 1)) {
        StdVideoVP9Segmentation* pStdSegment = &pPicData->stdSegmentation;
        ///* Clear all previous segment data */
        memset(pStdSegment->FeatureEnabled, 0, sizeof(pStdSegment->FeatureEnabled));
        memset(pStdSegment->FeatureData, 0, sizeof(pStdSegment->FeatureData));
        pStdPicInfo->frame_context_idx = 0;
    }

    ParseLoopFilterParams();
    ParseQuantizationParams();
    ParseSegmentationParams();
    ParseTileInfo();

    pPicData->compressedHeaderSize = u(16);

    pPicData->uncompressedHeaderOffset = 0;
    pPicData->compressedHeaderOffset = (consumed_bits() + 7) >> 3;
    pPicData->tilesOffset = pPicData->compressedHeaderOffset + pPicData->compressedHeaderSize;

    pPicData->ChromaFormat = (pStdColorConfig->subsampling_x == 1) && (pStdColorConfig->subsampling_y == 1) ? 1 : 0;
    assert(pPicData->ChromaFormat); // TODO: support only YUV420

    return true;
}

bool VulkanVP9Decoder::ParseColorConfig()
{
    StdVideoDecodeVP9PictureInfo* pStdPicInfo = &m_PicData.stdPictureInfo;
    StdVideoVP9ColorConfig* pStdColorConfig = &m_PicData.stdColorConfig;

    if (pStdPicInfo->profile >= STD_VIDEO_VP9_PROFILE_2) {
        pStdColorConfig->BitDepth = u(1) ? 12 : 10;
    } else {
        pStdColorConfig->BitDepth = 8;
    }

    pStdColorConfig->color_space = (StdVideoVP9ColorSpace)u(3);

    if (pStdColorConfig->color_space != STD_VIDEO_VP9_COLOR_SPACE_RGB) {
        pStdColorConfig->flags.color_range = u(1);
        if ((pStdPicInfo->profile == STD_VIDEO_VP9_PROFILE_1) ||
            (pStdPicInfo->profile == STD_VIDEO_VP9_PROFILE_3)) {
            pStdColorConfig->subsampling_x = u(1);
            pStdColorConfig->subsampling_y = u(1);
            VP9_CHECK_ZERO_BIT
        } else {
            pStdColorConfig->subsampling_x = 1;
            pStdColorConfig->subsampling_y = 1;
        }
    } else {
        pStdColorConfig->flags.color_range = 1;
        if ((pStdPicInfo->profile == STD_VIDEO_VP9_PROFILE_1) ||
            (pStdPicInfo->profile == STD_VIDEO_VP9_PROFILE_3)) {
            pStdColorConfig->subsampling_x = 0;
            pStdColorConfig->subsampling_y = 0;
            VP9_CHECK_ZERO_BIT
        }
    }
    return true;
}

void VulkanVP9Decoder::ParseFrameAndRenderSize()
{
    VkParserVp9PictureData *pPicData = &m_PicData;

    pPicData->FrameWidth = u(16) + 1;
    pPicData->FrameHeight = u(16) + 1;

    ComputeImageSize();

    if (u(1) == 1) { // render_and_frame_size_different
        pPicData->renderWidth = u(16) + 1;
        pPicData->renderHeight = u(16) + 1;
    } else {
        pPicData->renderWidth = pPicData->FrameWidth;
        pPicData->renderHeight = pPicData->FrameHeight;
    }
}

void VulkanVP9Decoder::ParseFrameAndRenderSizeWithRefs()
{
    VkParserVp9PictureData* pPicData = &m_PicData;

    bool found_ref = false;

    for (int i = 0; i < STD_VIDEO_VP9_REFS_PER_FRAME; ++i) {
        found_ref = u(1);
        if (found_ref) {
            VkPicIf* pRefPic = m_pBuffers[pPicData->ref_frame_idx[i]].buffer;
            if (pRefPic != nullptr) {
                pPicData->FrameWidth = pRefPic->decodeWidth;
                pPicData->FrameHeight = pRefPic->decodeHeight;

                ComputeImageSize();
            }

            if (u(1) == 1) { // render_and_frame_size_different
                pPicData->renderWidth = u(16) + 1;
                pPicData->renderHeight = u(16) + 1;
            } else {
                pPicData->renderWidth = pPicData->FrameWidth;
                pPicData->renderHeight = pPicData->FrameHeight;
            }

            break;
        }
    }
    if (!found_ref) {
        ParseFrameAndRenderSize();
    }
}

void VulkanVP9Decoder::ComputeImageSize()
{
    VkParserVp9PictureData* pPicData = &m_PicData;

    // compute_image_size()
    pPicData->MiCols = (pPicData->FrameWidth + 7) >> 3;
    pPicData->MiRows = (pPicData->FrameHeight + 7) >> 3;
    pPicData->Sb64Cols = (pPicData->MiCols + 7) >> 3;
    pPicData->Sb64Rows = (pPicData->MiRows + 7) >> 3;

    // compute_image_size() side effects (7.2.6)
    if (((uint32_t)m_lastFrameHeight != pPicData->FrameHeight) || ((uint32_t)m_lastFrameWidth != pPicData->FrameWidth)) {
        m_frameSizeChanged = true;
        pPicData->stdPictureInfo.flags.UsePrevFrameMvs = false;
    } else { /* 2.a, 2.b */
        bool intraOnly = pPicData->stdPictureInfo.frame_type == STD_VIDEO_VP9_FRAME_TYPE_KEY || pPicData->stdPictureInfo.flags.intra_only;
        pPicData->stdPictureInfo.flags.UsePrevFrameMvs = m_lastShowFrame && /* 2.c */
                                                         pPicData->stdPictureInfo.flags.error_resilient_mode == 0 && /* 2.d */
                                                         !intraOnly /* 2.e */;
    }
    m_lastFrameHeight = pPicData->FrameHeight;
    m_lastFrameWidth = pPicData->FrameWidth;
    m_lastShowFrame = pPicData->stdPictureInfo.flags.show_frame;

}

void VulkanVP9Decoder::ParseLoopFilterParams()
{
    VkParserVp9PictureData *pPicData = &m_PicData;
    StdVideoDecodeVP9PictureInfo *pStdPicInfo = &m_PicData.stdPictureInfo;
    StdVideoVP9LoopFilter* pStdLoopFilter = &m_PicData.stdLoopFilter;

    if (pPicData->FrameIsIntra || (pStdPicInfo->flags.error_resilient_mode == 1)) {
        // setup_past_independence() for loop filter params
        memset(m_loopFilterRefDeltas, 0, sizeof(m_loopFilterRefDeltas));
        memset(m_loopFilterModeDeltas, 0, sizeof(m_loopFilterModeDeltas));
        m_loopFilterRefDeltas[0] = 1;
        m_loopFilterRefDeltas[1] = 0;
        m_loopFilterRefDeltas[2] = -1;
        m_loopFilterRefDeltas[3] = -1;
    }

    pStdLoopFilter->loop_filter_level =  u(6);
    pStdLoopFilter->loop_filter_sharpness = u(3);

    pStdLoopFilter->flags.loop_filter_delta_enabled = u(1);
    if (pStdLoopFilter->flags.loop_filter_delta_enabled) {

        pStdLoopFilter->flags.loop_filter_delta_update = u(1);

        if (pStdLoopFilter->flags.loop_filter_delta_update) {

            for (int i = 0; i < STD_VIDEO_VP9_MAX_REF_FRAMES; i++) {
                uint8_t update_ref_delta = u(1);
                pStdLoopFilter->update_ref_delta |= update_ref_delta << i;
                if (update_ref_delta == 1) {
                    m_loopFilterRefDeltas[i] = u(6);
                    if (u(1)) { // sign
                        m_loopFilterRefDeltas[i] = -m_loopFilterRefDeltas[i];
                    }
                }
            }

            for (int i = 0; i < STD_VIDEO_VP9_LOOP_FILTER_ADJUSTMENTS; i++) {
                uint8_t update_mode_delta = u( 1);
                pStdLoopFilter->update_mode_delta |= update_mode_delta << i;
                if (update_mode_delta) {
                    m_loopFilterModeDeltas[i] = u(6);
                    if(u(1)) { // sign
                        m_loopFilterModeDeltas[i] = -m_loopFilterRefDeltas[i];
                    }
                }
            }
        }
    }

    memcpy(pStdLoopFilter->loop_filter_ref_deltas, m_loopFilterRefDeltas, sizeof(m_loopFilterRefDeltas));
    memcpy(pStdLoopFilter->loop_filter_mode_deltas, m_loopFilterModeDeltas, sizeof(m_loopFilterModeDeltas));
}

void VulkanVP9Decoder::ParseQuantizationParams()
{
   VkParserVp9PictureData *pPicData = &m_PicData;
   StdVideoDecodeVP9PictureInfo* pStdPicInfo = &pPicData->stdPictureInfo;

    pStdPicInfo->base_q_idx = u(8);
    pStdPicInfo->delta_q_y_dc = ReadDeltaQ();
    pStdPicInfo->delta_q_uv_dc = ReadDeltaQ();
    pStdPicInfo->delta_q_uv_ac = ReadDeltaQ();
}

int32_t VulkanVP9Decoder::ReadDeltaQ()
{
    int32_t delta;
    if (u(1)) {
        delta = u(4);
        if (u(1)) {
            delta = -delta;
        }
        return delta;
    } else {
        return 0;
    }
}

void VulkanVP9Decoder::ParseSegmentationParams()
{
    uint8_t segmentation_feature_bits[STD_VIDEO_VP9_SEG_LVL_MAX] = { 8, 6, 2, 0};
    uint8_t segmentation_feature_signed[STD_VIDEO_VP9_SEG_LVL_MAX] = {1, 1, 0, 0};

    StdVideoDecodeVP9PictureInfo* pStdPicInfo = &m_PicData.stdPictureInfo;
    StdVideoVP9Segmentation* pSegment = &m_PicData.stdSegmentation;

    pSegment->flags.segmentation_update_map = 0;
    pSegment->flags.segmentation_temporal_update = 0;

    pStdPicInfo->flags.segmentation_enabled = u(1);
    if (pStdPicInfo->flags.segmentation_enabled == 0) {
        return;
    }

    pSegment->flags.segmentation_update_map = u(1);

    if (pSegment->flags.segmentation_update_map == 1) {

        for (int i = 0; i < STD_VIDEO_VP9_MAX_SEGMENTATION_TREE_PROBS; i++) {
            uint8_t prob_coded = u(1);
            pSegment->segmentation_tree_probs[i] = (prob_coded == 1) ? u(8) : VP9_MAX_PRBABILITY;
        }

        pSegment->flags.segmentation_temporal_update = u(1);
        for (int i = 0; i < STD_VIDEO_VP9_MAX_SEGMENTATION_PRED_PROB; i++) {
            if (pSegment->flags.segmentation_temporal_update) {
                uint8_t prob_coded = u(1);
                pSegment->segmentation_pred_prob[i] = (prob_coded == 1) ? u(8) : VP9_MAX_PRBABILITY;
            } else {
                pSegment->segmentation_pred_prob[i] = VP9_MAX_PRBABILITY;
            }
        }
    }

    pSegment->flags.segmentation_update_data = u(1);
    if (pSegment->flags.segmentation_update_data == 1) {
        pSegment->flags.segmentation_abs_or_delta_update = u(1);

        /* Clear all previous segment data */
        memset(pSegment->FeatureEnabled, 0, sizeof(pSegment->FeatureEnabled));
        memset(pSegment->FeatureData, 0, sizeof(pSegment->FeatureData));

        for (int i = 0; i < STD_VIDEO_VP9_MAX_SEGMENTS; i++) {
            for (int j = 0; j < STD_VIDEO_VP9_SEG_LVL_MAX; j++) {
                uint8_t feature_enabled = u(1);
                pSegment->FeatureEnabled[i] |= (feature_enabled << j);

                if (feature_enabled == 1) {
                    pSegment->FeatureData[i][j] = u(segmentation_feature_bits[j]);

                    if (segmentation_feature_signed[j] == 1) {
                        if (u(1) == 1) {
                            pSegment->FeatureData[i][j] = -pSegment->FeatureData[i][j];
                        }
                    }
                }
            }
        }

    } // segmentation_update_data
}

uint8_t VulkanVP9Decoder::CalcMinLog2TileCols()
{
    VkParserVp9PictureData* pPicData = &m_PicData;
    uint8_t minLog2 = 0;

    while (((uint32_t)VP9_MAX_TILE_WIDTH_B64 << minLog2) < pPicData->Sb64Cols) {
        minLog2++;
    }

    return minLog2;
}

uint8_t VulkanVP9Decoder::CalcMaxLog2TileCols()
{
    VkParserVp9PictureData* pPicData = &m_PicData;
    uint8_t maxLog2 = 1;

    while ((pPicData->Sb64Cols >> maxLog2) >= VP9_MIN_TILE_WIDTH_B64) {
        maxLog2++;
    }

    return maxLog2 - 1;
}

void VulkanVP9Decoder::ParseTileInfo()
{
    VkParserVp9PictureData* pPicData = &m_PicData;
    StdVideoDecodeVP9PictureInfo* pStdPicInfo = &m_PicData.stdPictureInfo;

    uint8_t minLog2TileCols = CalcMinLog2TileCols();
    uint8_t maxLog2TileCols = CalcMaxLog2TileCols();

    pStdPicInfo->tile_cols_log2 = minLog2TileCols;

    while (pStdPicInfo->tile_cols_log2 < maxLog2TileCols) {
        if (u(1) == 1) { // increment_tile_cols_log2
            pStdPicInfo->tile_cols_log2++;
        } else {
            break;
        }
    }

    pStdPicInfo->tile_rows_log2 = u(1);
    if (pStdPicInfo->tile_rows_log2 == 1) {
        pStdPicInfo->tile_rows_log2 += u(1);
    }

    pPicData->numTiles = (1 << pStdPicInfo->tile_rows_log2) * (1 << pStdPicInfo->tile_cols_log2);
}

void VulkanVP9Decoder::ParseSuperFrameIndex(const uint8_t* data, uint32_t data_sz, uint32_t frame_sizes[8], uint32_t* frame_count)
{
    uint8_t final_byte = data[data_sz - 1];
    *frame_count = 0;

    if ((final_byte & 0xe0) == 0xc0) {
        const uint32_t frames = (final_byte & 0x7) + 1;
        const uint32_t mag = ((final_byte >> 3) & 0x3) + 1;
        const uint32_t index_sz = 2 + mag * frames;

        if (data_sz >= index_sz && data[data_sz - index_sz] == final_byte) {
            // found a valid superframe index
            const uint8_t* x = data + data_sz - index_sz + 1;
            for (uint32_t i = 0; i < frames; i++) {
                uint32_t this_sz = 0;
                for (uint32_t j = 0; j < mag; j++) {
                    this_sz |= (*x++) << (j * 8);
                }
                frame_sizes[i] = this_sz;
            }
            *frame_count = frames;
        }
    }
}

bool VulkanVP9Decoder::BeginPicture(VkParserPictureData* pnvpd)
{
    VkParserVp9PictureData* const pPicDataVP9 = &pnvpd->CodecSpecific.vp9;
    StdVideoVP9ColorConfig* pStdColorConfig = &pPicDataVP9->stdColorConfig;
    StdVideoDecodeVP9PictureInfo* pStdPicInfo = &m_PicData.stdPictureInfo;

    uint32_t width = pPicDataVP9->FrameWidth;
    uint32_t height = pPicDataVP9->FrameHeight;

    VkParserSequenceInfo nvsi = m_ExtSeqInfo;
    nvsi.eCodec = VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
    nvsi.nChromaFormat = pPicDataVP9->ChromaFormat;
    nvsi.nMaxWidth = std::max(width, pPicDataVP9->renderWidth);
    nvsi.nMaxHeight = std::max(height, pPicDataVP9->renderHeight);
    nvsi.nCodedWidth = width;
    nvsi.nCodedHeight = height;
    nvsi.nDisplayWidth = pPicDataVP9->renderWidth;
    nvsi.nDisplayHeight = pPicDataVP9->renderHeight;
    nvsi.lDARWidth = pPicDataVP9->renderWidth;
    nvsi.lDARHeight = pPicDataVP9->renderHeight;
    nvsi.bProgSeq = true; // VP9 doesn't have explicit interlaced coding.
    nvsi.nMinNumDecodeSurfaces = 9;
    nvsi.uBitDepthLumaMinus8 = pStdColorConfig->BitDepth - 8;
    nvsi.uBitDepthChromaMinus8 = pStdColorConfig->BitDepth - 8;
    nvsi.codecProfile = pStdPicInfo->profile;

    // Reset decoder only if decode RT orig width is less than required coded width
    if ((nvsi.nMaxWidth > m_rtOrigWidth) || (nvsi.nMaxHeight > m_rtOrigHeight)) {
        m_rtOrigWidth = nvsi.nMaxWidth;
        m_rtOrigHeight = nvsi.nMaxHeight;

        for (int i = 0; i < 8; i++) {
            if (m_pBuffers[i].buffer != nullptr) {
                m_pBuffers[i].buffer->Release();
                m_pBuffers[i].buffer = nullptr;
            }
        }
        if (m_pCurrPic != nullptr) {
            m_pCurrPic->Release();
            m_pCurrPic = nullptr;
        }
    }

    if (!init_sequence(&nvsi)) {
        assert(!"init_sequence failed!");
        return false;
    }

    // Allocate a buffer for the current picture
    if (m_pCurrPic == nullptr) {
        m_pClient->AllocPictureBuffer(&m_pCurrPic);
        assert(m_pCurrPic);

        m_pCurrPic->decodeWidth = width;
        m_pCurrPic->decodeHeight = height;
    }

    pnvpd->PicWidthInMbs = nvsi.nCodedWidth >> 4;
    pnvpd->FrameHeightInMbs = nvsi.nCodedHeight >> 4;
    pnvpd->pCurrPic = m_pCurrPic;
    pnvpd->progressive_frame = 1;
    pnvpd->ref_pic_flag = 1;
    pnvpd->intra_pic_flag = pPicDataVP9->FrameIsIntra;
    pnvpd->chroma_format = pPicDataVP9->ChromaFormat;

    // Reference slots information
    for (int i = 0; i < STD_VIDEO_VP9_NUM_REF_FRAMES; i++) {
        vkPicBuffBase* pb = reinterpret_cast<vkPicBuffBase*>(m_pBuffers[i].buffer);
        pPicDataVP9->pic_idx[i] = pb ? pb->m_picIdx : -1;
    }

    return true;
}
