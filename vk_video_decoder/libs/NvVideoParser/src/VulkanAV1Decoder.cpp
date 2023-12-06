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

/////////////////////////////////////////////////////////////////////////////////////////////////////
//
// AV1 elementary stream parser (picture & sequence layer)
//
/////////////////////////////////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cstdint>
#include <climits>
#include "VulkanVideoParserIf.h"

#ifdef ENABLE_AV1_DECODER

#define DEBUG_PARSER 0
#if DEBUG_PARSER
#include <stdio.h>
#endif


#include "VulkanAV1Decoder.h"

// constructor
VulkanAV1Decoder::VulkanAV1Decoder(VkVideoCodecOperationFlagBitsKHR std, bool annexB)
    : VulkanVideoDecoder(std)
{
    memset(&m_PicData, 0, sizeof(m_PicData));
    m_pCurrPic = nullptr;
    for (int i = 0; i < 8; i++) {
        memset(&m_pBuffers[i], 0, sizeof(m_pBuffers[0]));
    }
    for (int i = 0; i < NUM_REF_FRAMES; i++) {
        ref_frame_id[i] = -1;
    }
    temporal_id = 0;
    spatial_id = 0;
    m_bSPSReceived = false;
    m_bSPSChanged = false;
    m_obuAnnexB = annexB;
    timing_info_present = 0;
    memset(&timing_info, 0, sizeof(timing_info));
    memset(&buffer_model, 0, sizeof(buffer_model));
    memset(&op_params, 0, sizeof(op_params));
    memset(&op_frame_timing, 0, sizeof(op_frame_timing));
    last_frame_type = 0;
    last_intra_only = 0;
    all_lossless = 0;
    frame_width = 0;
    frame_height = 0;
    render_width = 0;
    render_height = 0;
    intra_only = 0;
    showable_frame = 0;
    last_show_frame = 0;
    show_existing_frame = 0;
    tu_presentation_delay = 0;
    primary_ref_frame = PRIMARY_REF_NONE;
    current_frame_id = 0;
    frame_offset = 0;
    refresh_frame_flags = (1 << NUM_REF_FRAMES) - 1;
    log2_tile_cols = 0;
    log2_tile_rows = 0;
    tile_size_bytes_minus_1 = 3;
    m_numOutFrames = 0;
    m_bOutputAllLayers = false;
    m_OperatingPointIDCActive = 0;
    memset(&m_pOutFrame[0], 0, sizeof(m_pOutFrame));
    memset(&m_showableFrame[0], 0, sizeof(m_showableFrame));
    for (int i = 0; i < GM_GLOBAL_MODELS_PER_FRAME; ++i)
    {
        global_motions[i] = default_warp_params;
    }
}

// destructor
VulkanAV1Decoder::~VulkanAV1Decoder()
{

}

// initialization
void VulkanAV1Decoder::InitParser()
{
    m_bNoStartCodes = true;
    m_bEmulBytesPresent = false;
    m_bSPSReceived = false;
    EndOfStream();
}

// EOS
void VulkanAV1Decoder::EndOfStream()
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
    for (int i = 0; i < MAX_NUM_SPATIAL_LAYERS; i++) {
        if (m_pOutFrame[i]) {
            m_pOutFrame[i]->Release();
            m_pOutFrame[i] = nullptr;
        }
    }
}

bool VulkanAV1Decoder::AddBuffertoOutputQueue(VkPicIf* pDispPic, bool bShowableFrame)
{
    if (m_bOutputAllLayers) {
/*
        if (m_numOutFrames >= MAX_NUM_SPATIAL_LAYERS) 
        {
            // We can't store the new frame anywhere, so drop it and return an error
            return false;
        }
        else {
            m_pOutFrame[m_numOutFrames] = pDispPic;
            m_showableFrame[m_numOutFrames] = bShowableFrame;
            m_numOutFrames++;
        }
*/
        // adding buffer to output queue will cause display latency so display immediately to avoid latency
        AddBuffertoDispQueue(pDispPic);
        lEndPicture(pDispPic, !bShowableFrame);
        if (pDispPic) {
            pDispPic->Release();
        }
    } else {
        assert(m_numOutFrames == 0 || m_numOutFrames == 1);

        if (m_numOutFrames > 0) {
            m_pOutFrame[0]->Release();
        }

        m_pOutFrame[0] = pDispPic;
        m_showableFrame[0] = bShowableFrame;
        m_numOutFrames++;
    }
    return true;
}

void VulkanAV1Decoder::AddBuffertoDispQueue(VkPicIf* pDispPic)
{
    int32_t lDisp = 0;

    // Find an entry in m_DispInfo
    for (int32_t i = 0; i < MAX_DELAY; i++) {
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
    m_DispInfo[lDisp].bDiscontinuity = false;
    //m_DispInfo[lDisp].lPOC = m_pNVDPictureData->picture_order_count;
    m_DispInfo[lDisp].lNumFields = 2;

    // Find a PTS in the list
    unsigned int ndx = m_lPTSPos;
    m_DispInfo[lDisp].bPTSValid = false;
    m_DispInfo[lDisp].llPTS = m_llExpectedPTS; // Will be updated later on
    for (int k = 0; k < MAX_QUEUED_PTS; k++) {
        if ((m_PTSQueue[ndx].bPTSValid) && (m_PTSQueue[ndx].llPTSPos - m_llFrameStartLocation <= (m_bNoStartCodes ? 0 : 3))) {
            m_DispInfo[lDisp].bPTSValid = true;
            m_DispInfo[lDisp].llPTS = m_PTSQueue[ndx].llPTS;
            m_DispInfo[lDisp].bDiscontinuity = m_PTSQueue[ndx].bDiscontinuity;
            m_PTSQueue[ndx].bPTSValid = false;
        }
        ndx = (ndx + 1) % MAX_QUEUED_PTS;
    }
}

// kick-off decoding
bool VulkanAV1Decoder::end_of_picture(uint32_t frameSize)
{
    *m_pVkPictureData = VkParserPictureData();
    m_pVkPictureData->numSlices = m_PicData.num_tile_cols * m_PicData.num_tile_rows;  // set number of tiles as AV1 doesn't have slice concept
    
    m_pVkPictureData->bitstreamDataLen = frameSize;
    m_pVkPictureData->bitstreamData = m_bitstreamData.GetBitstreamBuffer();
    m_pVkPictureData->bitstreamDataOffset = 0; // TODO: The extra storage in this library and necessarily the app is silly.

    std::copy(std::begin(m_tileOffsets), std::end(m_tileOffsets), m_PicData.tileOffsets);
    std::copy(std::begin(m_tileSizes), std::end(m_tileSizes), m_PicData.tileSizes);

    m_PicData.needsSessionReset = m_bSPSChanged;
    m_bSPSChanged = false;

    m_pVkPictureData->firstSliceIndex = 0;
    m_pVkPictureData->CodecSpecific.av1 = m_PicData;
    m_pVkPictureData->intra_pic_flag = m_PicData.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY;

    if (!BeginPicture(m_pVkPictureData)) {
        // Error: BeginPicture failed
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
        // "WARNING: no valid render target for current picture
    }

    // decode_frame_wrapup
    UpdateFramePointers(m_pCurrPic);
    if (m_PicData.show_frame && !bSkipped) {
        AddBuffertoOutputQueue(m_pCurrPic, !!showable_frame);
        m_pCurrPic = nullptr;
    } else {
        if (m_pCurrPic) {
            m_pCurrPic->Release();
            m_pCurrPic = nullptr;
        }
    }

    return true;
}

// BeginPicture
bool VulkanAV1Decoder::BeginPicture(VkParserPictureData* pnvpd)
{
    VkParserAv1PictureData* const av1 = &pnvpd->CodecSpecific.av1;
    av1_seq_param_s *const sps = m_sps.Get();
    DE_ASSERT(sps);

    // TODO: drop all the SDK fluff
    av1->upscaled_width = upscaled_width;
    av1->frame_width = frame_width;
    av1->frame_height = frame_height;

    av1->frame_offset = frame_offset;
    // sps
    av1->profile                              = sps->profile;
    av1->use_128x128_superblock               = sps->flags.use_128x128_superblock;  // 0:64x64, 1: 128x128

    // color_config
    memcpy(&av1->color_config, &sps->color_config, sizeof(StdVideoAV1ColorConfig));

    av1->enable_fgs                     = sps->flags.film_grain_params_present;
    av1->primary_ref_frame              = primary_ref_frame;
    av1->temporal_layer_id              = temporal_id;
    av1->spatial_layer_id               = spatial_id;
    av1->enable_order_hint              = sps->flags.enable_order_hint;

    VkParserSequenceInfo nvsi = m_ExtSeqInfo;
    nvsi.eCodec         = (VkVideoCodecOperationFlagBitsKHR)VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
    nvsi.nChromaFormat  = av1->color_config.flags.mono_chrome ? 0 : (av1->color_config.subsampling_x && av1->color_config.subsampling_y) ? 1 : (!av1->color_config.subsampling_x && !av1->color_config.subsampling_y) ? 3 : 2;
    nvsi.nMaxWidth      = (sps->max_frame_width_minus_1 + 2) & ~1;
    nvsi.nMaxHeight     = (sps->max_frame_height_minus_1 + 2) & ~1;
    nvsi.nCodedWidth    = av1->upscaled_width;
    nvsi.nCodedHeight   = frame_height;
    nvsi.nDisplayWidth  = nvsi.nCodedWidth; // (nvsi.nCodedWidth + 1) & (~1);
    nvsi.nDisplayHeight = nvsi.nCodedHeight; //(nvsi.nCodedHeight + 1) & (~1);
    nvsi.bProgSeq = true; // AV1 doesnt have explicit interlaced coding.

    nvsi.uBitDepthLumaMinus8 = av1->color_config.BitDepth - 8;
    nvsi.uBitDepthChromaMinus8 = nvsi.uBitDepthLumaMinus8;

    nvsi.lDARWidth = nvsi.nDisplayWidth;
    nvsi.lDARHeight = nvsi.nDisplayHeight;
    // nMinNumDecodeSurfaces = dpbsize (8 for av1)  + 1
    // double the decode RT count to account film grained output if film grain present
    nvsi.nMinNumDecodeSurfaces = 9;

    nvsi.lVideoFormat = VideoFormatUnspecified;
    nvsi.lColorPrimaries = sps->color_config.color_primaries;
    nvsi.lTransferCharacteristics = sps->color_config.transfer_characteristics;
    nvsi.lMatrixCoefficients = sps->color_config.matrix_coefficients;

    nvsi.pbSideData = pnvpd->pSideData;
    nvsi.cbSideData = pnvpd->sideDataLen;

    nvsi.filmGrainEnabled = sps->flags.film_grain_params_present;

    if (av1->needsSessionReset && !init_sequence(&nvsi))
        return false;

    // Allocate a buffer for the current picture
    if (m_pCurrPic == nullptr) {
        m_pClient->AllocPictureBuffer(&m_pCurrPic);
        if (m_pCurrPic) {
            m_pCurrPic->frameWidth = frame_width;
            m_pCurrPic->frameHeight = frame_height;
            m_pCurrPic->renderWidth = render_width;
            m_pCurrPic->renderHeight = render_height;
            m_pCurrPic->upscaledWidth = upscaled_width;
        }
    }

    pnvpd->PicWidthInMbs    = nvsi.nCodedWidth >> 4;
    pnvpd->FrameHeightInMbs = nvsi.nCodedHeight >> 4;
    pnvpd->pCurrPic         = m_pCurrPic;
    pnvpd->progressive_frame = 1;
    // TODO: always needs a setup slot index.
    pnvpd->ref_pic_flag     = true;
    pnvpd->chroma_format    = nvsi.nChromaFormat; // 1 : 420

    for (int i = 0; i < 7; i++) {
        av1->ref_frame_picture[i] = m_pBuffers[i].buffer;
        av1->ref_frame_idx[i] = ref_frame_idx[i];
        av1->ref_global_motion[i].wmtype = global_motions[i].wmtype;
        for (int j = 0; j <= 5; j++) {
            av1->ref_global_motion[i].wmmat[j] = global_motions[i].wmmat[j];
        }
        av1->ref_global_motion[i].invalid = global_motions[i].invalid;
        // Add a mask of which buffers are being updated.
        av1->refFrameParams[i].primary_ref_frame = m_pBuffers[i].primary_ref_frame; // if not 0 -- may not alloc a slot. Re-resolve this per frame per dpb index.
        av1->refFrameParams[i].base_q_index = m_pBuffers[i].base_q_index;
        av1->refFrameParams[i].disable_frame_end_update_cdf = m_pBuffers[i].disable_frame_end_update_cdf;
        av1->refFrameParams[i].segmentation_enabled = m_pBuffers[i].segmentation_enabled;
        av1->refFrameParams[i].frame_type = m_pBuffers[i].frame_type;
        av1->refFrameParams[i].order_hint = m_pBuffers[i].order_hint;
        for (size_t av1name = 0; av1name < (sizeof(av1->refFrameParams[i].ref_order_hint)); av1name += 1 ) {
            av1->refFrameParams[i].ref_order_hint[av1name] = m_pBuffers[i].ref_order_hint[av1name];
            av1->refFrameParams[i].RefFrameSignBias[av1name] = m_pBuffers[i].RefFrameSignBias[av1name];
        }

    }

    //av1->refFrameParams = m_pBuffers; // Issues with includes and 

    // if not 0 -- may not alloc a slot. Re-resolve this per frame per dpb index.
    // What to do with index [7] ?
    // NOTE(charlie): This is the thing I was talking about, and calling a "scratch space" in the Khr issues
    // Some bitstreams store something here for future reference I understand. Checkout 48delayed.ivf for one example.
    av1->ref_frame_picture[7] = m_pBuffers[7].buffer;
    av1->refFrameParams[7].primary_ref_frame = m_pBuffers[7].primary_ref_frame; // if not 0 -- may not alloc a slot. Re-resolve this per frame per dpb index.
    av1->refFrameParams[7].base_q_index = m_pBuffers[7].base_q_index;
    av1->refFrameParams[7].disable_frame_end_update_cdf = m_pBuffers[7].disable_frame_end_update_cdf;
    av1->refFrameParams[7].segmentation_enabled = m_pBuffers[7].segmentation_enabled;
    av1->refFrameParams[7].frame_type = m_pBuffers[7].frame_type;
    av1->refFrameParams[7].order_hint = m_pBuffers[7].order_hint;
    for (int av1name = 0; av1name < (sizeof(av1->refFrameParams[7].ref_order_hint)); av1name += 1 ) {
        av1->refFrameParams[7].ref_order_hint[av1name] = m_pBuffers[7].ref_order_hint[av1name];
        av1->refFrameParams[7].RefFrameSignBias[av1name] = m_pBuffers[7].RefFrameSignBias[av1name];
    }

    return true;
}

int VulkanAV1Decoder::GetRelativeDist(int a, int b)
{
    auto* sps = m_sps.Get();
    if (sps->flags.enable_order_hint == false) {
        return 0;
    }

    const int bits = sps->order_hint_bits_minus_1 + 1;

    assert(bits >= 1);
    assert(a >= 0 && a < (1 << bits));
    assert(b >= 0 && b < (1 << bits));

    int diff = a - b;
    const int m = 1 << (bits - 1);
    diff = (diff & (m - 1)) - (diff & m);
    return diff;
}

void VulkanAV1Decoder::UpdateFramePointers(VkPicIf* currentPicture)
{
    VkParserAv1PictureData* pic_info = &m_PicData;

    // uint32_t i;
    uint32_t mask, ref_index = 0;

    for (mask = refresh_frame_flags; mask; mask >>= 1) {
        if (mask & 1) {
            if (m_pBuffers[ref_index].buffer) {
                m_pBuffers[ref_index].buffer->Release();
            }

            m_pBuffers[ref_index].buffer = currentPicture;
            m_pBuffers[ref_index].showable_frame = showable_frame;

            m_pBuffers[ref_index].frame_type = (StdVideoAV1FrameType)pic_info->frame_type;
            m_pBuffers[ref_index].order_hint = frame_offset;
            for (uint8_t av1name = 0; av1name < sizeof(pic_info->ref_order_hint); av1name ++) {
                uint8_t ref_order_hint = 0;
                if ((ref_frame_idx[av1name] < 8) && (ref_frame_idx[av1name] >= 0)) {
                    ref_order_hint = pic_info->ref_order_hint[ref_frame_idx[av1name]];
                } else {
                    ref_order_hint = frame_offset;
                }

                m_pBuffers[ref_index].ref_order_hint[av1name] = ref_order_hint;
                m_pBuffers[ref_index].RefFrameSignBias[av1name] = GetRelativeDist(frame_offset, ref_order_hint);
            }

            // film grain
            memcpy(&m_pBuffers[ref_index].film_grain_params, &pic_info->fgs, sizeof(av1_film_grain_s));
            // global motion
            memcpy(&m_pBuffers[ref_index].global_models, &global_motions, sizeof(AV1WarpedMotionParams) * GM_GLOBAL_MODELS_PER_FRAME);
            // loop filter 
            memcpy(&m_pBuffers[ref_index].lf_ref_delta, pic_info->loop_filter_ref_deltas, sizeof(pic_info->loop_filter_ref_deltas));
            memcpy(&m_pBuffers[ref_index].lf_mode_delta, pic_info->loop_filter_mode_deltas, sizeof(pic_info->loop_filter_mode_deltas));
            // segmentation
            memcpy(&m_pBuffers[ref_index].seg.feature_enable, pic_info->segmentation_feature_enable, sizeof(pic_info->segmentation_feature_enable));
            memcpy(&m_pBuffers[ref_index].seg.feature_data, pic_info->segmentation_feature_data, sizeof(pic_info->segmentation_feature_data));
            m_pBuffers[ref_index].seg.last_active_id = pic_info->segid_preskip;
            m_pBuffers[ref_index].seg.preskip_id = pic_info->last_active_segid;

            m_pBuffers[ref_index].primary_ref_frame = pic_info->primary_ref_frame;
            m_pBuffers[ref_index].base_q_index = pic_info->base_qindex;
            m_pBuffers[ref_index].disable_frame_end_update_cdf = pic_info->disable_frame_end_update_cdf;
            m_pBuffers[ref_index].segmentation_enabled = pic_info->segmentation_enabled;

            RefOrderHint[ref_index] = frame_offset;

            if (m_pBuffers[ref_index].buffer) {
                m_pBuffers[ref_index].buffer->AddRef();
            }
        }
        ++ref_index;
    }

    // Invalidate these references until the next frame starts.
    // for (i = 0; i < ALLOWED_REFS_PER_FRAME; i++)
    //     pic_info->activeRefIdx[i] = 0xffff;
}

// EndPicture
void VulkanAV1Decoder::lEndPicture(VkPicIf* pDispPic, bool bEvict)
{
    if (pDispPic) {
        display_picture(pDispPic, bEvict);
    }
}

// 
uint32_t VulkanAV1Decoder::ReadUvlc()
{
    int lz = 0;
    while (!u(1)) lz++;

    if (lz >= 32) {
        return BIT32_MAX;
    }
    uint32_t v = u(lz);
    v += (1 << lz) - 1;
    return v;
}

// Read OBU size (size does not include obu_header or the obu_size syntax element
bool VulkanAV1Decoder::ReadObuSize(const uint8_t* data, uint32_t datasize, uint32_t* obu_size, uint32_t* length_feild_size)
{
    for (uint32_t i = 0; i < 8 && (i < datasize); ++i) {
        const uint8_t decoded_byte = data[i] & 0x7f;
        *obu_size |= ((uint64_t)decoded_byte) << (i * 7);
        if ((data[i] >> 7) == 0) {
            *length_feild_size = i + 1;
            if (*obu_size > BIT32_MAX) {
                return false;
            } else {
                return true;
            }
        }
    }

    return false;
}

// Parses OBU header
bool VulkanAV1Decoder::ReadObuHeader(const uint8_t* pData, uint32_t datasize, AV1ObuHeader* hdr)
{
    const uint8_t* local = pData;
    hdr->header_size = 1;

    if (((local[0] >> 7) & 1) != 0) {
        // Forbidden bit
        // Corrupt frame
        return false;
    }

    hdr->type = (AV1_OBU_TYPE)((local[0] >> 3) & 0xf);

    if (!((hdr->type >= AV1_OBU_SEQUENCE_HEADER && hdr->type <= AV1_OBU_PADDING))) {
        // Invalid OBU type
        return false;
    }

    hdr->has_extension = (local[0] >> 2) & 1;
    hdr->has_size_field = (local[0] >> 1) & 1;

    if (!hdr->has_size_field && !m_obuAnnexB) {
        // obu streams must have obu_size field set.
        // Unsupported bitstream
        return false;
    }

    if (((local[0] >> 0) & 1) != 0) {
        // must be set to 0.
        // Corrupt frame
        return false;
    }

    if (hdr->has_extension) {
        if (datasize < 2)
            return false;
        hdr->header_size += 1;
        hdr->temporal_id = (local[1] >> 5) & 0x7;
        hdr->spatial_id = (local[1] >> 3) & 0x3;
        if (((local[1] >> 0) & 0x7) != 0) {
            // must be set to 0.
            // Corrupt frame
            return false;
        }
    }

    return true;
}

// 
bool VulkanAV1Decoder::ParseOBUHeaderAndSize(const uint8_t* data, uint32_t datasize, AV1ObuHeader* hdr)
{
    uint32_t annexb_obu_length = 0, annexb_uleb_length = 0;

    if (datasize == 0) {
        return false;
    }

    if (m_obuAnnexB) {
        if (!ReadObuSize(data, datasize, &annexb_obu_length, &annexb_uleb_length)) {
            return false;
        }
    }

    if (!ReadObuHeader(data + annexb_uleb_length, datasize - annexb_uleb_length, hdr)) {
        // read_obu_header() failed
        return false;;
    }

    if (m_obuAnnexB) {
        // Derive the payload size from the data we've already read
        if (annexb_obu_length < hdr->header_size) return false;

        // The Annex B OBU length includes the OBU header.
        hdr->payload_size = annexb_obu_length - hdr->header_size;
        hdr->header_size += annexb_uleb_length;
        uint32_t obu_size = 0;
        uint32_t size_field_uleb_length = 0;
        if (hdr->has_size_field) {
            if (!ReadObuSize(data + hdr->header_size, datasize - hdr->header_size, &obu_size, &size_field_uleb_length)) {
                return false;
            }
            hdr->header_size += size_field_uleb_length;
            hdr->payload_size = obu_size;
        }
    } else {
        assert(hdr->has_size_field);
        // Size field comes after the OBU header, and is just the payload size
        uint32_t obu_size = 0;
        uint32_t size_field_uleb_length = 0;

        if (!ReadObuSize(data + hdr->header_size, datasize - hdr->header_size, &obu_size, &size_field_uleb_length)) {
            return false;
        }
        hdr->payload_size = obu_size;
        hdr->header_size += size_field_uleb_length;
    }

    return true;
}

bool VulkanAV1Decoder::ParseObuTemporalDelimiter()
{
	m_tileOffsets.clear();
	m_tileSizes.clear();
    return true;
}

void VulkanAV1Decoder::ReadTimingInfoHeader()
{
    timing_info.num_units_in_display_tick = u(32);  // Number of units in a display tick
    timing_info.time_scale = u(32);  // Time scale
    if (timing_info.num_units_in_display_tick == 0 || timing_info.time_scale == 0){
        // num_units_in_display_tick and time_scale must be greater than 0.
    }
    timing_info.equal_picture_interval = u(1);  // Equal picture interval bit
    if (timing_info.equal_picture_interval) {
        timing_info.num_ticks_per_picture = ReadUvlc() + 1;  // ticks per picture
        if (timing_info.num_ticks_per_picture == 0)
        {
            // num_ticks_per_picture_minus_1 cannot be (1 << 32) ? 1.
        }
    }
}

void VulkanAV1Decoder::ReadDecoderModelInfo()
{
    buffer_model.encoder_decoder_buffer_delay_length = u(5) + 1;
    buffer_model.num_units_in_decoding_tick = u(32);  // Number of units in a decoding tick
    buffer_model.buffer_removal_time_length = u(5) + 1;
    buffer_model.frame_presentation_time_length = u(5) + 1;
}

int VulkanAV1Decoder::ChooseOperatingPoint()
{
    int operating_point = 0;
    if (m_pClient != nullptr) {
        VkParserOperatingPointInfo OPInfo;
        memset(&OPInfo, 0, sizeof(OPInfo));

        OPInfo.eCodec = (VkVideoCodecOperationFlagBitsKHR)VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
        OPInfo.av1.operating_points_cnt = m_sps->operating_points_cnt_minus_1 + 1;
        for (int i = 0; i < OPInfo.av1.operating_points_cnt; i++) {
            OPInfo.av1.operating_points_idc[i] = m_sps->operating_point_idc[i];
        }

        operating_point = m_pClient->GetOperatingPoint(&OPInfo);

        if (operating_point < 0) {
            assert(!"GetOperatingPoint callback failed");
            // ignoring error and continue with operating point 0
            operating_point = 0;
        }
        m_bOutputAllLayers = !!(operating_point & 0x400);
        operating_point = operating_point & ~0x400;
        if (operating_point < 0 || operating_point > m_sps->operating_points_cnt_minus_1) {
            operating_point = 0;
        }
    }
    return operating_point;
}

static int spsSequenceCounter = 0;

bool VulkanAV1Decoder::ParseObuSequenceHeader()
{
    auto prevSps = m_sps;
    VkResult result = av1_seq_param_s::Create(spsSequenceCounter++, m_sps);

    assert((result == VK_SUCCESS) && m_sps);
    if (result != VK_SUCCESS)
        return false;

    auto* sps = m_sps.Get();

    sps->pColorConfig = &sps->color_config;
    sps->pTimingInfo = &sps->timing_info;
    sps->profile = (AV1_PROFILE)u(3);
    if (sps->profile > AV1_PROFILE_2) {
        // Unsupported profile
        return false;
    }

    sps->flags.still_picture = u(1);
    sps->flags.reduced_still_picture_header = u(1);

    if (!sps->flags.still_picture && sps->flags.reduced_still_picture_header) {
        // Error: Video must have reduced_still_picture_hdr == 0
        return false;
    }

    if (sps->flags.reduced_still_picture_header) {
        timing_info_present = 0;
        sps->decoder_model_info_present = 0;
        sps->display_model_info_present = 0;
        sps->operating_points_cnt_minus_1 = 0;
        sps->operating_point_idc[0] = 0;

        //const uint8_t seq_level_idx = u(5);
        //if (seq_level_idx > 7) {
        //    // Error: Unsupported sequence level
        //    return false;
        //}
        //sps->level[0].major = (seq_level_idx >> LEVEL_MINOR_BITS) + LEVEL_MAJOR_MIN;
        //sps->level[0].minor = seq_level_idx & ((1 << LEVEL_MINOR_BITS) - 1);
        sps->level[0] = (AV1_LEVEL)u(5);
        if (sps->level[0] > LEVEL_7) {
            // Error: unsupported sequence level
            return false;
        }

        sps->tier[0] = 0;
        op_params[0].decoder_model_param_present = 0;
        op_params[0].display_model_param_present = 0;
    } else {
        timing_info_present = u(1);
        if (timing_info_present) {
            ReadTimingInfoHeader();

            sps->decoder_model_info_present = u(1);
            if (sps->decoder_model_info_present) {
                ReadDecoderModelInfo();
            }
        } else {
            sps->decoder_model_info_present = 0;
        }
        sps->display_model_info_present = u(1);
        sps->operating_points_cnt_minus_1 = u(5);
        for (int i = 0; i <= sps->operating_points_cnt_minus_1; i++) {
            sps->operating_point_idc[i] = u(12);
            //const uint8_t seq_level_idx = u(5);
            //if (!(seq_level_idx < 24 || seq_level_idx == 31)) {
            //    // Error: Unsupported sequence level
            //    return false;
            //}
            //sps->level[i].major = (seq_level_idx >> LEVEL_MINOR_BITS) + LEVEL_MAJOR_MIN;
            //sps->level[i].minor = seq_level_idx & ((1 << LEVEL_MINOR_BITS) - 1);
            sps->level[i] = (AV1_LEVEL)u(5);
            if (!(sps->level[i] <= LEVEL_23 || sps->level[i] == LEVEL_MAX)) {
                // Error: Unsupported sequence level
                return false;
            }

            //if (sps->level[i].major > 3) {
            if (sps->level[i] > LEVEL_3_3) {
                sps->tier[i] = u(1);
            } else {
                sps->tier[i] = 0;
            }
            if (sps->decoder_model_info_present) {
                op_params[i].decoder_model_param_present = u(1);
                if (op_params[i].decoder_model_param_present) {
                    int n = buffer_model.encoder_decoder_buffer_delay_length;
                    op_params[i].decoder_buffer_delay = u(n);
                    op_params[i].encoder_buffer_delay = u(n);
                    op_params[i].low_delay_mode_flag = u(1);
                }
            } else {
                op_params[i].decoder_model_param_present = 0;
            }
            if (sps->display_model_info_present) {
                op_params[i].display_model_param_present = u(1);
                if (op_params[i].display_model_param_present) {
                    op_params[i].initial_display_delay = u(4) + 1;
#if 0
                    if (op_params[i].initial_display_delay > 10)
                    {
                        // doesn't support delay of 10 decode frames
                        return false;
                    }
#endif
                } else {
                    op_params[i].initial_display_delay = 10;
                }
            } else {
                op_params[i].display_model_param_present = 0;
                op_params[i].initial_display_delay = 10;
            }
        }
    }

    sps->frame_width_bits_minus_1 = u(4);
    sps->frame_height_bits_minus_1 = u(4);
    sps->max_frame_width_minus_1 = u(sps->frame_width_bits_minus_1 + 1);
    sps->max_frame_height_minus_1 = u(sps->frame_height_bits_minus_1 + 1);

    if (sps->flags.reduced_still_picture_header) {
        sps->flags.frame_id_numbers_present_flag = 0;
    } else {
        sps->flags.frame_id_numbers_present_flag = u(1);
    }

    if (sps->flags.frame_id_numbers_present_flag) {
        sps->delta_frame_id_length = u(4) + 2;
        sps->frame_id_length = u(3) + sps->delta_frame_id_length + 1;
        if (sps->frame_id_length > 16) {
            // Invalid frame_id_length
            return false;
        }
    }

    sps->flags.use_128x128_superblock = u(1);
    sps->flags.enable_filter_intra = u(1);
    sps->flags.enable_intra_edge_filter = u(1);

    if (sps->flags.reduced_still_picture_header) {
        sps->flags.enable_interintra_compound = 0;
        sps->flags.enable_masked_compound = 0;
        sps->flags.enable_warped_motion = 0;
        sps->flags.enable_dual_filter = 0;
        sps->flags.enable_order_hint = 0;
        sps->flags.enable_jnt_comp = 0;
        sps->flags.enable_ref_frame_mvs = 0;
        sps->force_screen_content_tools = SELECT_SCREEN_CONTENT_TOOLS;
        sps->force_integer_mv = SELECT_INTEGER_MV;
        sps->order_hint_bits_minus_1 = 0;
    } else {
        sps->flags.enable_interintra_compound = u(1);
        sps->flags.enable_masked_compound = u(1);
        sps->flags.enable_warped_motion = u(1);
        sps->flags.enable_dual_filter = u(1);
        sps->flags.enable_order_hint = u(1);
        if (sps->flags.enable_order_hint) {
            sps->flags.enable_jnt_comp = u(1);
            sps->flags.enable_ref_frame_mvs = u(1);
        } else {
            sps->flags.enable_jnt_comp = 0;
            sps->flags.enable_ref_frame_mvs = 0;
        }

        if (u(1)) {
            sps->force_screen_content_tools = SELECT_SCREEN_CONTENT_TOOLS;
        }
        else
            sps->force_screen_content_tools = u(1);

        if (sps->force_screen_content_tools > 0) {
            if (u(1)) {
                sps->force_integer_mv = SELECT_INTEGER_MV;
            } else {
                sps->force_integer_mv = u(1);
            }
        } else {
            sps->force_integer_mv = SELECT_INTEGER_MV;
        }
        sps->order_hint_bits_minus_1 = sps->flags.enable_order_hint ? u(3) : 0;
    }

    sps->flags.enable_superres = u(1);
    sps->flags.enable_cdef = u(1);
    sps->flags.enable_restoration = u(1);
    // color config
    bool high_bitdepth = u(1);
    if (sps->profile == AV1_PROFILE_2 && high_bitdepth) {
        const bool twelve_bit = u(1);
        sps->color_config.BitDepth = twelve_bit ? 12 : 10;
    } else if (sps->profile <= AV1_PROFILE_2) {
        sps->color_config.BitDepth = high_bitdepth ? 10 : 8;
    } else {
        // Unsupported profile/bit-depth combination
    }
    sps->color_config.BitDepth = sps->color_config.BitDepth;
    sps->color_config.flags.color_range = sps->color_config.flags.color_range;
    sps->color_config.flags.separate_uv_delta_q = sps->color_config.flags.separate_uv_delta_q;
    sps->color_config.subsampling_x = sps->color_config.subsampling_x;
    sps->color_config.subsampling_y = sps->color_config.subsampling_y;

    sps->color_config.flags.mono_chrome = sps->profile != AV1_PROFILE_1 ? u(1) : 0;
    sps->color_config.flags.color_description_present_flag = u(1);
    if (sps->color_config.flags.color_description_present_flag) {
        sps->color_config.color_primaries = (StdVideoAV1ColorPrimaries)u(8);
        sps->color_config.transfer_characteristics = (StdVideoAV1TransferCharacteristics)u(8);
        sps->color_config.matrix_coefficients = (StdVideoAV1MatrixCoefficients)u(8);
    } else {
        sps->color_config.color_primaries = STD_VIDEO_AV1_COLOR_PRIMARIES_BT_UNSPECIFIED;
        sps->color_config.transfer_characteristics = STD_VIDEO_AV1_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
        sps->color_config.matrix_coefficients = STD_VIDEO_AV1_MATRIX_COEFFICIENTS_UNSPECIFIED;
    }

    if (sps->color_config.flags.mono_chrome) {
        sps->color_config.flags.color_range = u(1);
        sps->color_config.subsampling_x = sps->color_config.subsampling_y = 1;
        sps->color_config.flags.separate_uv_delta_q = 0;
    } else {
        if (sps->color_config.color_primaries == STD_VIDEO_AV1_COLOR_PRIMARIES_BT_709 &&
            sps->color_config.transfer_characteristics == STD_VIDEO_AV1_TRANSFER_CHARACTERISTICS_SRGB &&
            sps->color_config.matrix_coefficients == STD_VIDEO_AV1_MATRIX_COEFFICIENTS_IDENTITY) {
            sps->color_config.subsampling_y = sps->color_config.subsampling_x = 0;
            sps->color_config.flags.color_range = 1;  // assume full color-range
        } else {
            sps->color_config.flags.color_range = u(1);
            if (sps->profile == 0) {
                sps->color_config.subsampling_x = sps->color_config.subsampling_y = 1;// 420
            } else if (sps->profile == 1) {
                sps->color_config.subsampling_x = sps->color_config.subsampling_y = 0;// 444
            } else {
                if (sps->color_config.BitDepth == 12) {
                    sps->color_config.subsampling_x = u(1);
                    if (sps->color_config.subsampling_x) {
                        sps->color_config.subsampling_y = u(1);
                    } else {
                        sps->color_config.subsampling_y = 0;
                    }
                } else {
                    sps->color_config.subsampling_x = 1; //422
                    sps->color_config.subsampling_y = 0;
                }
            }
            if (sps->color_config.subsampling_x&&sps->color_config.subsampling_y) { // subsampling equals 1 1
	        sps->color_config.chroma_sample_position = (StdVideoAV1ChromaSamplePosition)u(2);
            }
        }
        sps->color_config.flags.separate_uv_delta_q = u(1);
    }
    sps->flags.film_grain_params_present = u(1);

    // check_trailing_bits()
    int bits_before_byte_alignment = 8 - (m_nalu.get_bfroffs % 8);
    int trailing = u(bits_before_byte_alignment);
    if (trailing != (1 << (bits_before_byte_alignment - 1))) {
        // trailing bits of SPS corrupted
        return false;
    }

    if (m_bSPSReceived) {
        // @review: this is not correct
	if (m_sps->isDifferentFrom(prevSps.Get()))
            m_bSPSChanged = true;
    } else {
        m_bSPSChanged = true;
    }

    m_bSPSReceived = true;

    VkSharedBaseObj<StdVideoPictureParametersSet> picParamObj(m_sps);
    m_PicData.pStdSps = picParamObj.Get();
    if (m_pClient) { // @review need to make sure this has really changed!
        bool success = m_pClient->UpdatePictureParameters(picParamObj, m_sps->client);
        assert(success);
        if (!success) {
            nvParserErrorLog("s", "\nError updating the AV1 sequence parameters\n");
        }
    }

    int operating_point = 0;
    if (m_sps->operating_points_cnt_minus_1 > 0) {
        operating_point = ChooseOperatingPoint();
    }

    m_OperatingPointIDCActive = sps->operating_point_idc[operating_point];

    return true;
}


void VulkanAV1Decoder::SetupFrameSize(int frame_size_override_flag)
{
    av1_seq_param_s *const sps = m_sps.Get();
    VkParserAv1PictureData* pic_info = &m_PicData;

    if (frame_size_override_flag) {
        frame_width = u(sps->frame_width_bits_minus_1 + 1) + 1;
        frame_height = u(sps->frame_height_bits_minus_1 + 1) + 1;
        if (frame_width > (sps->max_frame_width_minus_1 + 1) || frame_height > (sps->max_frame_height_minus_1 + 1)) {
            assert(false);
        }
    } else {
        frame_width = sps->max_frame_width_minus_1 + 1;
        frame_height = sps->max_frame_height_minus_1 + 1;
    }

    //superres_params 
    upscaled_width = frame_width;
    pic_info->coded_denom = 0;
    uint8_t superres_scale_denominator = 8;
    pic_info->use_superres = 0;
    if (sps->flags.enable_superres){
        if (u(1)) {
            pic_info->use_superres = 1;
            superres_scale_denominator = u(3);
            pic_info->coded_denom = superres_scale_denominator;
            superres_scale_denominator += SUPERRES_DENOM_MIN;
            frame_width = (upscaled_width*SUPERRES_NUM + superres_scale_denominator / 2) / superres_scale_denominator;
        }
    }

    //render size 
    if (u(1)) {
        render_width = u(16) + 1;
        render_height = u(16) + 1;

    } else {
        render_width = upscaled_width;
        render_height = frame_height;
    }
}

int VulkanAV1Decoder::SetupFrameSizeWithRefs()
{
    av1_seq_param_s *const sps = m_sps.Get();
    VkParserAv1PictureData* pic_info = &m_PicData;

    uint32_t tmp;// index;
    int32_t found, i;

    found = 0;

    for (i = 0; i < REFS_PER_FRAME; ++i) {
        tmp = u(1);
        if (tmp) {
            found = 1;
            VkPicIf *m_pPic = m_pBuffers[ref_frame_idx[i]].buffer;
            if (m_pPic) {
                upscaled_width = m_pPic->upscaledWidth;
		frame_width = m_pPic->frameWidth;
                frame_height = m_pPic->frameHeight;
                render_width = m_pPic->renderWidth;
                render_height = m_pPic->renderHeight;
            }
            break;
        }
    }

    if (!found) {
        SetupFrameSize(1);
    } else {
        //superres_params 
        uint8_t superres_scale_denominator = SUPERRES_NUM;
        pic_info->coded_denom = 0;
        pic_info->use_superres = 0;
        if (sps->flags.enable_superres) {
            if (u(1)) {
                pic_info->use_superres = 1;
                superres_scale_denominator = u(SUPERRES_DENOM_BITS);
                pic_info->coded_denom = superres_scale_denominator;
                superres_scale_denominator += SUPERRES_DENOM_MIN;
            }
        }

        frame_width = (upscaled_width*SUPERRES_NUM + superres_scale_denominator / 2) / superres_scale_denominator;
    }

    return 1;
}

bool VulkanAV1Decoder::ReadFilmGrainParams()
{
    av1_seq_param_s *const sps = m_sps.Get();
    VkParserAv1PictureData* pic_info = &m_PicData;

    if (sps->flags.film_grain_params_present && (pic_info->show_frame || showable_frame)) {
        VkParserAv1FilmGrain* fgp = &pic_info->fgs;
        fgp->apply_grain = u(1);
        if (!fgp->apply_grain) {
            memset(fgp, 0, sizeof(*fgp));
            return 1;
        }

        fgp->grain_seed = u(16);
        if (pic_info->frame_type == STD_VIDEO_AV1_FRAME_TYPE_INTER) {
            fgp->update_grain = u(1);
        } else {
            fgp->update_grain = 1;
        }

        if (!fgp->update_grain) {
            // Use previous reference frame film grain params
            int buf_idx = u(3);
            uint16_t random_seed = fgp->grain_seed;
            if (m_pBuffers[buf_idx].buffer) {
                memcpy(fgp, &(m_pBuffers[buf_idx].film_grain_params), sizeof(av1_film_grain_s));
            }
            fgp->grain_seed = random_seed;
            fgp->film_grain_params_ref_idx = buf_idx;
            return 1;
        }

        // Scaling functions parameters
        fgp->num_y_points = u(4);  // max 14
        if (fgp->num_y_points > 14) {
            assert("num_y_points exceeds the maximum value\n");
        }
        for (uint32_t i = 0; i < fgp->num_y_points; i++) {
            fgp->scaling_points_y[i][0] = u(8);
            if (i && fgp->scaling_points_y[i - 1][0] >= fgp->scaling_points_y[i][0]) {
                assert(!"Y cordinates should be increasing\n");
            }

            fgp->scaling_points_y[i][1] = u(8);
        }

        if (!sps->color_config.flags.mono_chrome) {
            fgp->chroma_scaling_from_luma = u(1);
        } else {
            fgp->chroma_scaling_from_luma = 0;
        }

        if (sps->color_config.flags.mono_chrome || fgp->chroma_scaling_from_luma ||
            ((sps->color_config.subsampling_x == 1) && (sps->color_config.subsampling_y == 1) && (fgp->num_y_points == 0))) {
            fgp->num_cb_points = 0;
            fgp->num_cr_points = 0;
        } else {
            fgp->num_cb_points = u(4);  // max 10
            if (fgp->num_cb_points > 10) {
                assert(!"num_cb_points exceeds the maximum value\n");
            }

            for (uint32_t i = 0; i < fgp->num_cb_points; i++) {
                fgp->scaling_points_cb[i][0] = u(8);
                if (i && fgp->scaling_points_cb[i - 1][0] >= fgp->scaling_points_cb[i][0]) {
                    assert(!"cb cordinates should be increasing\n");
                }
                fgp->scaling_points_cb[i][1] = u(8);
            }

            fgp->num_cr_points = u(4);  // max 10
            if (fgp->num_cr_points > 10) {
                assert(!"num_cr_points exceeds the maximum value\n");
            }

            for (uint32_t i = 0; i < fgp->num_cr_points; i++) {
                fgp->scaling_points_cr[i][0] = u(8);
                if (i && fgp->scaling_points_cr[i - 1][0] >= fgp->scaling_points_cr[i][0]) {
                    assert(!"cr cordinates should be increasing\n");
                }
                fgp->scaling_points_cr[i][1] = u(8);
            }
        }

        fgp->scaling_shift_minus8 = u(2);
        fgp->ar_coeff_lag = u(2);

        int numPosLuma = 2 * fgp->ar_coeff_lag * (fgp->ar_coeff_lag + 1);
        int numPosChroma = numPosLuma;
        if (fgp->num_y_points > 0) {
            ++numPosChroma;
        }

        if (fgp->num_y_points) {
            for (int i = 0; i < numPosLuma; i++) {
                fgp->ar_coeffs_y[i] = u(8) - 128;
            }
        }

        if (fgp->num_cb_points || fgp->chroma_scaling_from_luma) {
            for (int i = 0; i < numPosChroma; i++) {
                fgp->ar_coeffs_cb[i] = u(8) - 128;
            }
        }

        if (fgp->num_cr_points || fgp->chroma_scaling_from_luma) {
            for (int i = 0; i < numPosChroma; i++) {
                fgp->ar_coeffs_cr[i] = u(8) - 128;
            }
        }

        fgp->ar_coeff_shift_minus6 = u(2);

        fgp->grain_scale_shift = u(2);

        if (fgp->num_cb_points) {
            fgp->cb_mult = u(8);
            fgp->cb_luma_mult = u(8);
            fgp->cb_offset = u(9);
        }

        if (fgp->num_cr_points) {
            fgp->cr_mult = u(8);
            fgp->cr_luma_mult = u(8);
            fgp->cr_offset = u(9);
        }

        fgp->overlap_flag = u(1);

        fgp->clip_to_restricted_range = u(1);
    } else {
        memset(&pic_info->fgs, 0, sizeof(av1_film_grain_s));
    }

    return true;
}

static uint32_t tile_log2(int blk_size, int target)
{
    uint32_t k;
    
    for (k = 0; (blk_size << k) < target; k++) {
    }
    
    return k;
}

uint32_t FloorLog2(uint32_t x)
{
    int s = 0;
    
    while (x != 0) {
        x = x >> 1;
        s++;
    }
    
    return s - 1;
}
uint32_t VulkanAV1Decoder::SwGetUniform(uint32_t max_value)
{
    uint32_t w = FloorLog2(max_value) + 1;
    uint32_t m = (1 << w) - max_value;
    uint32_t v = u(w - 1);
    if (v < m) {
        return v;
    }
    uint32_t extral_bit = u(1);
    return (v << 1) - m + extral_bit;
}

bool VulkanAV1Decoder::DecodeTileInfo()
{
    av1_seq_param_s *const sps = m_sps.Get();
    const auto& seq_hdr_flags = sps->flags;

    VkParserAv1PictureData* pic_info = &m_PicData;

    int mi_cols = 2 * ((frame_width + 7) >> 3);
    int mi_rows = 2 * ((frame_height + 7) >> 3);

    int min_log2_tile_rows;
    uint32_t max_tile_height_sb;

    // Macroblock dimensions to superblock dimenseions
    uint32_t sb_cols = seq_hdr_flags.use_128x128_superblock ? ((mi_cols + 31) >> 5) : ((mi_cols + 15) >> 4);
    uint32_t sb_rows = seq_hdr_flags.use_128x128_superblock ? ((mi_rows + 31) >> 5) : ((mi_rows + 15) >> 4);
    int numSuperblocks = sb_cols * sb_rows;
    int sb_shift = seq_hdr_flags.use_128x128_superblock ? 5 : 4;
    int sb_size = sb_shift + 2;

    uint32_t max_tile_width_sb = MAX_TILE_WIDTH >> sb_size;
    uint32_t max_tile_area_sb = MAX_TILE_AREA >> (2 * sb_size);
    uint32_t min_log2_tile_cols = tile_log2(max_tile_width_sb, sb_cols);
    uint32_t max_log2_tile_cols = tile_log2(1, std::min(sb_cols, MAX_TILE_COLS));
    uint32_t max_log2_tile_rows = tile_log2(1, std::min(sb_rows, MAX_TILE_ROWS));
    uint32_t min_log2_tiles = std::max(min_log2_tile_cols, tile_log2(max_tile_area_sb, sb_rows * sb_cols));

    uint8_t uniform_tile_spacing_flag = u(1);
    pic_info->uniform_tile_spacing_flag = uniform_tile_spacing_flag;
    memset(&pic_info->tile_col_start_sb[0], 0, sizeof(pic_info->tile_col_start_sb));
    memset(&pic_info->tile_row_start_sb[0], 0, sizeof(pic_info->tile_row_start_sb));
    memset(&pic_info->tile_width_in_sbs_minus_1[0], 0, sizeof(pic_info->tile_width_in_sbs_minus_1));
    memset(&pic_info->tile_height_in_sbs_minus_1[0], 0, sizeof(pic_info->tile_height_in_sbs_minus_1));

    if (uniform_tile_spacing_flag) {
        int tile_width_sb, tile_height_sb;
        log2_tile_cols = min_log2_tile_cols;
        while (log2_tile_cols < max_log2_tile_cols) {
            if (!u(1))
                break;
            log2_tile_cols++;
        }

        tile_width_sb = (sb_cols + (1 << log2_tile_cols) - 1) >> log2_tile_cols;
        for (uint32_t off = 0, i = 0; off < sb_cols; off += tile_width_sb)
            pic_info->tile_col_start_sb[i++] = off;

        pic_info->num_tile_cols = (sb_cols + tile_width_sb - 1) / tile_width_sb;

        min_log2_tile_rows = std::max(int(min_log2_tiles - log2_tile_cols), 0);
        log2_tile_rows = min_log2_tile_rows;
        while (log2_tile_rows < max_log2_tile_rows) {
            if (!u(1))
                break;
            log2_tile_rows++;
        }

        tile_height_sb = (sb_rows + (1 << log2_tile_rows) - 1) >> log2_tile_rows;
        for (uint32_t off = 0, i = 0; off < sb_rows; off += tile_height_sb)
            pic_info->tile_row_start_sb[i++] = off;

        pic_info->num_tile_rows = (sb_rows + tile_height_sb - 1) / tile_height_sb;

        // Derive tile_width_in_sbs_minus_1 and tile_height_in_sbs_minus_1
        uint32_t tile_col = 0;
         for ( ; tile_col < pic_info->num_tile_cols - 1u; tile_col++)
            pic_info->tile_width_in_sbs_minus_1[tile_col] = tile_width_sb - 1;
        pic_info->tile_width_in_sbs_minus_1[tile_col] = sb_cols - (pic_info->num_tile_cols - 1) * tile_width_sb - 1;

        uint32_t tile_row = 0;
         for ( ; tile_row < pic_info->num_tile_rows - 1u; tile_row++)
            pic_info->tile_height_in_sbs_minus_1[tile_row] = tile_height_sb - 1;
        pic_info->tile_height_in_sbs_minus_1[tile_row] = sb_rows - (pic_info->num_tile_rows - 1) * tile_height_sb - 1;

        // Derivce superblock column / row start positions
        uint32_t i, start_sb;
        for (i = 0, start_sb = 0; start_sb < sb_cols; i++) {
            pic_info->tile_col_start_sb[i] = start_sb;
            start_sb += tile_width_sb;
        }
        pic_info->tile_col_start_sb[i] = sb_cols;

        for (i = 0, start_sb = 0; start_sb < sb_rows; i++) {
            pic_info->tile_row_start_sb[i] = start_sb;
            start_sb += tile_height_sb;
        }
        pic_info->tile_row_start_sb[i] = sb_rows;
    } else {
        uint32_t i, widest_tile_sb, start_sb, size_sb, max_width, max_height;
        widest_tile_sb = 0;
        start_sb = 0;

        start_sb = 0;
        for (i = 0; start_sb < sb_cols && i < MAX_TILE_COLS; i++) {
            pic_info->tile_col_start_sb[i] = start_sb;
            max_width = std::min(sb_cols - start_sb, max_tile_width_sb);
            pic_info->tile_width_in_sbs_minus_1[i] = (max_width > 1) ? 1 + SwGetUniform(max_width) : 1;
            size_sb = pic_info->tile_width_in_sbs_minus_1[i] + 1;
            widest_tile_sb = std::max(size_sb, widest_tile_sb);
            start_sb += size_sb;
        }
        log2_tile_cols = tile_log2(1, i);
        pic_info->num_tile_cols = i;

        if (min_log2_tiles > 0)
            max_tile_area_sb = (numSuperblocks) >> (min_log2_tiles + 1);
        else
            max_tile_area_sb = numSuperblocks;
        max_tile_height_sb = std::max(max_tile_area_sb / widest_tile_sb, 1u);

        start_sb = 0;
        for (i = 0; start_sb < sb_rows && i < MAX_TILE_ROWS; i++) {
            pic_info->tile_row_start_sb[i] = start_sb;
            max_height = std::min(sb_rows - start_sb, max_tile_height_sb);
            pic_info->tile_height_in_sbs_minus_1[i] = (max_height > 1) ? 1 + SwGetUniform(max_height) : 1;
            size_sb = pic_info->tile_height_in_sbs_minus_1[i] + 1;
            start_sb += size_sb;
        }
        log2_tile_rows = tile_log2(1, i);
        pic_info->num_tile_rows = i;
    }

    pic_info->context_update_tile_id = 0;
    tile_size_bytes_minus_1 = 3;
    if (pic_info->num_tile_rows * pic_info->num_tile_cols > 1) {
        // tile to use for cdf update
        pic_info->context_update_tile_id = u(log2_tile_rows + log2_tile_cols);
        // tile size magnitude
        tile_size_bytes_minus_1 = u(2);
        pic_info->tile_size_bytes_minus_1 = tile_size_bytes_minus_1;
    }

    return true;
}

inline int VulkanAV1Decoder::ReadSignedBits(uint32_t bits)
{
    const int nbits = sizeof(uint32_t) * 8 - bits - 1;
    uint32_t v = (uint32_t)u(bits + 1) << nbits;
    return ((int)v) >> nbits;
}

inline int VulkanAV1Decoder::ReadDeltaQ(uint32_t bits)
{
    return u(1) ? ReadSignedBits(bits) : 0;
}

void VulkanAV1Decoder::DecodeQuantizationData()
{
    av1_seq_param_s *const sps = m_sps.Get();
    VkParserAv1PictureData* pic_info = &m_PicData;

    pic_info->base_qindex = u(8);
    pic_info->qp_y_dc_delta_q = ReadDeltaQ(6);
    if (!sps->color_config.flags.mono_chrome) {
        int diff_uv_delta = 0;
        if (sps->color_config.flags.separate_uv_delta_q)
            diff_uv_delta = u(1);
        pic_info->qp_u_dc_delta_q = ReadDeltaQ(6);
        pic_info->qp_u_ac_delta_q = ReadDeltaQ(6);
        if (diff_uv_delta) {
            pic_info->qp_v_dc_delta_q = ReadDeltaQ(6);
            pic_info->qp_v_ac_delta_q = ReadDeltaQ(6);
        } else {
            pic_info->qp_v_dc_delta_q = pic_info->qp_u_dc_delta_q;
            pic_info->qp_v_ac_delta_q = pic_info->qp_u_ac_delta_q;
        }
    } else {
        pic_info->qp_u_dc_delta_q = 0;
        pic_info->qp_u_ac_delta_q = 0;
        pic_info->qp_v_dc_delta_q = 0;
        pic_info->qp_v_ac_delta_q = 0;
    }

    pic_info->using_qmatrix = u(1);
    if (pic_info->using_qmatrix) {
        pic_info->qm_y = u(4);
        pic_info->qm_u = u(4);
        if (!sps->color_config.flags.separate_uv_delta_q) {
            pic_info->qm_v = pic_info->qm_u;
        } else {
            pic_info->qm_v = u(4);
        }
    } else {
        pic_info->qm_y = 0;
        pic_info->qm_u = 0;
        pic_info->qm_v = 0;
    }
}

static const int av1_seg_feature_data_signed[MAX_SEG_LVL] = { 1, 1, 1, 1, 1, 0, 0, 0 };
static const int av1_seg_feature_Bits[MAX_SEG_LVL] = { 8, 6, 6, 6, 6, 3, 0, 0 };
static const int av1_seg_feature_data_max[MAX_SEGMENTS] = { 255, 63, 63, 63, 63, 7, 0, 0 };

void VulkanAV1Decoder::DecodeSegmentationData()
{
    VkParserAv1PictureData* pic_info = &m_PicData;

    pic_info->segmentation_update_map = 0;
    pic_info->segmentation_update_data = 0;
    pic_info->segmentation_temporal_update = 0;

    pic_info->segmentation_enabled = u(1);

    if (!pic_info->segmentation_enabled) {
        memset(pic_info->segmentation_feature_enable, 0, sizeof(pic_info->segmentation_feature_enable));
        memset(pic_info->segmentation_feature_data, 0, sizeof(pic_info->segmentation_feature_data));
        pic_info->last_active_segid = pic_info->segid_preskip = 0;
        return;
    }

    if (primary_ref_frame == PRIMARY_REF_NONE) {
        pic_info->segmentation_update_map = 1;
        pic_info->segmentation_update_data = 1;
        pic_info->segmentation_temporal_update = 0;
    } else {
        pic_info->segmentation_update_map = u(1);

        if (pic_info->segmentation_update_map) {
            pic_info->segmentation_temporal_update = u(1);
        } else {
            pic_info->segmentation_temporal_update = 0;
        }

        pic_info->segmentation_update_data = u(1);
    }

    if (pic_info->segmentation_update_data) {
        for (int i = 0; i < MAX_SEGMENTS; i++) {
            for (int j = 0; j < MAX_SEG_LVL; j++) {
                int feature_value = 0;
                pic_info->segmentation_feature_enable[i][j] = u(1);
                if (pic_info->segmentation_feature_enable[i][j]) {
                    pic_info->segid_preskip |= (j >= AV1_SEG_LVL_REF_FRAME);
                    pic_info->last_active_segid = i;
                    const int data_max = av1_seg_feature_data_max[j];
                    if (av1_seg_feature_data_signed[j]) {
                        feature_value = ReadSignedBits(av1_seg_feature_Bits[j]);
                        feature_value = CLAMP(feature_value, -data_max, data_max);
                    } else {
                        feature_value = u(av1_seg_feature_Bits[j]);
                        feature_value = CLAMP(feature_value, 0, data_max);
                    }
                }
                pic_info->segmentation_feature_data[i][j] = feature_value;
            }
        }
    } else {
        if (primary_ref_frame != PRIMARY_REF_NONE) {
            // overwrite default values with prev frame data
            int prim_buf_idx = ref_frame_idx[primary_ref_frame];
            if (m_pBuffers[prim_buf_idx].buffer) {
                memcpy(pic_info->segmentation_feature_enable, m_pBuffers[prim_buf_idx].seg.feature_enable, sizeof(pic_info->segmentation_feature_enable));
                memcpy(pic_info->segmentation_feature_data, m_pBuffers[prim_buf_idx].seg.feature_data, sizeof(pic_info->segmentation_feature_data));
                pic_info->segid_preskip      = m_pBuffers[prim_buf_idx].seg.preskip_id;
                pic_info->last_active_segid  = m_pBuffers[prim_buf_idx].seg.last_active_id;
            }
        }
    }
}

static const char lf_ref_delta_default[] = { 1, 0, 0, 0, (char)-1, 0, (char)-1, (char)-1 };

void VulkanAV1Decoder::DecodeLoopFilterdata()
{
    av1_seq_param_s *const sps = m_sps.Get();
    VkParserAv1PictureData* pic_info = &m_PicData;

    //set default values
    pic_info->loop_filter_delta_enabled = 0;
    memcpy(pic_info->loop_filter_ref_deltas, lf_ref_delta_default, sizeof(lf_ref_delta_default));
    memset(pic_info->loop_filter_mode_deltas, 0, sizeof(pic_info->loop_filter_mode_deltas));
    pic_info->loop_filter_level_u = pic_info->loop_filter_level_v = 0;

    if (pic_info->allow_intrabc || pic_info->coded_lossless) {
        pic_info->loop_filter_level[0] = pic_info->loop_filter_level[1] = 0;
        return;
    }

    if (primary_ref_frame != PRIMARY_REF_NONE) {
        // overwrite default values with prev frame data
        int prim_buf_idx = ref_frame_idx[primary_ref_frame];
        if (m_pBuffers[prim_buf_idx].buffer) {
            memcpy(pic_info->loop_filter_ref_deltas, m_pBuffers[prim_buf_idx].lf_ref_delta, sizeof(lf_ref_delta_default));
            memcpy(pic_info->loop_filter_mode_deltas, m_pBuffers[prim_buf_idx].lf_mode_delta, sizeof(pic_info->loop_filter_mode_deltas));
        }
    }

    pic_info->loop_filter_level[0] = u(6);
    pic_info->loop_filter_level[1] = u(6);
    if (!sps->color_config.flags.mono_chrome && (pic_info->loop_filter_level[0] || pic_info->loop_filter_level[1])) {
        pic_info->loop_filter_level_u = u(6);
        pic_info->loop_filter_level_v = u(6);
    }
    pic_info->loop_filter_sharpness = u(3);

    uint8_t lf_mode_ref_delta_update = 0;
    pic_info->loop_filter_delta_enabled = u(1);
    if (pic_info->loop_filter_delta_enabled) {
        lf_mode_ref_delta_update = u(1);
        pic_info->loop_filter_delta_update = lf_mode_ref_delta_update;
        if (lf_mode_ref_delta_update) {
            for (int i = 0; i < NUM_REF_FRAMES; i++) {
                if (u(1)) {
                    pic_info->loop_filter_ref_deltas[i] = ReadSignedBits(6);
                }
            }

            for (int i = 0; i < 2; i++) {
                if (u(1)) {
                    pic_info->loop_filter_mode_deltas[i] = ReadSignedBits(6);
                }
            }
        }
    }
}

void VulkanAV1Decoder::DecodeCDEFdata()
{
    av1_seq_param_s *const sps = m_sps.Get();
    VkParserAv1PictureData* pic_info = &m_PicData;

    if (pic_info->allow_intrabc)
        return;

    pic_info->cdef_damping_minus_3 = u(2);
    pic_info->cdef_bits = u(2);

    for (int i = 0; i < 8; i++) {
        if (i == (1 << pic_info->cdef_bits)) {
            break;
        }
        pic_info->cdef_y_pri_strength[i] = u(4);
        pic_info->cdef_y_sec_strength[i] = u(2);
        if (!sps->color_config.flags.mono_chrome) {
            pic_info->cdef_uv_pri_strength[i] = u(4);
            pic_info->cdef_uv_sec_strength[i] = u(2);
        }
    }
}

void VulkanAV1Decoder::DecodeLoopRestorationData()
{
    av1_seq_param_s *const sps = m_sps.Get();
    VkParserAv1PictureData* pic_info = &m_PicData;

    if (pic_info->allow_intrabc) {
        return;
    }

    int n_planes = sps->color_config.flags.mono_chrome ? 1 : 3;
    bool use_lr = false, use_chroma_lr = false;

    uint8_t remap_lr_type[4] = { RESTORE_NONE, RESTORE_SWITCHABLE, RESTORE_WIENER, RESTORE_SGRPROJ };
    for (int pl = 0; pl < n_planes; pl++) {
        uint8_t lr_type = u(2);
        pic_info->FrameRestorationType[pl] = remap_lr_type[lr_type];

        if (pic_info->FrameRestorationType[pl] != RESTORE_NONE) {
            use_lr = true;
            if (pl > 0) {
                use_chroma_lr = true;
            }
        }
    }
    pic_info->UsesLr = use_lr;
    if (use_lr)  {
        int lr_unit_shift = 0;
        int sb_size = sps->flags.use_128x128_superblock == 1 /*BLOCK_128X128*/ ? 2 : 1; //128 : 64;

        for (int pl = 0; pl < n_planes; pl++) {
            pic_info->LoopRestorationSize[pl] = sb_size;  // 64 or 128
        }
        if (sps->flags.use_128x128_superblock == 1) {
            lr_unit_shift = 1 + u(1);
        } else {
            lr_unit_shift = u(1);
            if (lr_unit_shift) {
                lr_unit_shift += u(1);
            }
        }
        pic_info->LoopRestorationSize[0] = 1 + lr_unit_shift;
    } else {
        for (int pl = 0; pl < n_planes; pl++)
            pic_info->LoopRestorationSize[pl] = 3;
    }
    uint8_t lr_uv_shift = 0;

    if (!sps->color_config.flags.mono_chrome) {
        if (use_chroma_lr && (sps->color_config.subsampling_x && sps->color_config.subsampling_y)) {
            lr_uv_shift = u(1);
            pic_info->LoopRestorationSize[1] = pic_info->LoopRestorationSize[0] - lr_uv_shift; // *std::min(sps->subsampling_x, sps->subsampling_y));
            pic_info->LoopRestorationSize[2] = pic_info->LoopRestorationSize[1];
        } else {
            pic_info->LoopRestorationSize[1] = pic_info->LoopRestorationSize[0];
            pic_info->LoopRestorationSize[2] = pic_info->LoopRestorationSize[0];
        }
    }
    pic_info->LoopRestorationSize[1] = pic_info->LoopRestorationSize[0] >> lr_uv_shift;
    pic_info->LoopRestorationSize[1] = pic_info->LoopRestorationSize[1] >> lr_uv_shift;
}

int VulkanAV1Decoder::GetRelativeDist1(int a, int b)
{
    av1_seq_param_s *const sps = m_sps.Get();
    if (!sps->flags.enable_order_hint) {
        return 0;
    }

    const int bits = sps->order_hint_bits_minus_1 + 1;

    assert(bits >= 1);
    assert(a >= 0 && a < (1 << bits));
    assert(b >= 0 && b < (1 << bits));

    int diff = a - b;
    const int m = 1 << (bits - 1);
    diff = (diff & (m - 1)) - (diff & m);
    return diff;
}

//follow spec 7.8
void VulkanAV1Decoder::SetFrameRefs(int last_frame_idx, int gold_frame_idx)
{
    av1_seq_param_s *const sps = m_sps.Get();

    assert(sps->flags.enable_order_hint);
    assert(sps->order_hint_bits_minus_1 >= 0);

    const int OrderHint = (int)frame_offset;
    const int curFrameHint = 1 << sps->order_hint_bits_minus_1;

    int shiftedOrderHints[NUM_REF_FRAMES];
    int Ref_OrderHint;
    int usedFrame[NUM_REF_FRAMES];
    int hint;
    for (int i = 0; i < REFS_PER_FRAME; i++) {
        ref_frame_idx[i] = -1;
    }

    for (int i = 0; i < NUM_REF_FRAMES; i++) {
        usedFrame[i] = 0;
    }

    ref_frame_idx[LAST_FRAME - LAST_FRAME] = last_frame_idx;
    ref_frame_idx[GOLDEN_FRAME - LAST_FRAME] = gold_frame_idx;
    usedFrame[last_frame_idx] = 1;
    usedFrame[gold_frame_idx] = 1;

    for (int i = 0; i < NUM_REF_FRAMES; i++) {
        Ref_OrderHint = RefOrderHint[i];
        shiftedOrderHints[i] = curFrameHint + GetRelativeDist1(Ref_OrderHint, OrderHint);
    }

    {//ALTREF_FRAME
        int ref = -1;
        int latestOrderHint = -1;
        for (int i = 0; i < NUM_REF_FRAMES; i++) {
            hint = shiftedOrderHints[i];
            if (!usedFrame[i] &&
                hint >= curFrameHint &&
                (ref < 0 || hint >= latestOrderHint)) {
                ref = i;
                latestOrderHint = hint;
            }
        }
        if (ref >= 0) {
            ref_frame_idx[ALTREF_FRAME - LAST_FRAME] = ref;
            usedFrame[ref] = 1;
        }
    }

    {//BWDREF_FRAME
        int ref = -1;
        int earliestOrderHint = -1;
        for (int i = 0; i < NUM_REF_FRAMES; i++) {
            hint = shiftedOrderHints[i];
            if (!usedFrame[i] &&
                hint >= curFrameHint &&
                (ref < 0 || hint < earliestOrderHint)) {
                ref = i;
                earliestOrderHint = hint;
            }
        }
        if (ref >= 0) {
            ref_frame_idx[BWDREF_FRAME - LAST_FRAME] = ref;
            usedFrame[ref] = 1;
        }
    }

    {//ALTREF2_FRAME
        int ref = -1;
        int earliestOrderHint = -1;
        for (int i = 0; i < NUM_REF_FRAMES; i++) {
            hint = shiftedOrderHints[i];
            if (!usedFrame[i] &&
                hint >= curFrameHint &&
                (ref < 0 || hint < earliestOrderHint)) {
                ref = i;
                earliestOrderHint = hint;
            }
        }
        if (ref >= 0) {
            ref_frame_idx[ALTREF2_FRAME - LAST_FRAME] = ref;
            usedFrame[ref] = 1;
        }
    }

    uint32_t Ref_Frame_List[REFS_PER_FRAME - 2] = { LAST2_FRAME, LAST3_FRAME, BWDREF_FRAME, ALTREF2_FRAME, ALTREF_FRAME };

    for (int j = 0; j < REFS_PER_FRAME - 2; j++) {
        int refFrame = Ref_Frame_List[j];
        if (ref_frame_idx[refFrame - LAST_FRAME] < 0) {
            int ref = -1;
            int latestOrderHint = -1;
            for (int i = 0; i < NUM_REF_FRAMES; i++) {
                hint = shiftedOrderHints[i];
                if (!usedFrame[i] &&
                    hint < curFrameHint &&
                    (ref < 0 || hint >= latestOrderHint)) {
                    ref = i;
                    latestOrderHint = hint;
                }
            }
            if (ref >= 0) {
                ref_frame_idx[refFrame - LAST_FRAME] = ref;
                usedFrame[ref] = 1;
            }
        }
    }

    {
        int ref = -1;
        int earliestOrderHint = -1;
        for (int i = 0; i < NUM_REF_FRAMES; i++) {
            hint = shiftedOrderHints[i];
            if (ref < 0 || hint < earliestOrderHint) {
                ref = i;
                earliestOrderHint = hint;
            }
        }
        for (int i = 0; i < REFS_PER_FRAME; i++) {
            if (ref_frame_idx[i] < 0) {
                ref_frame_idx[i] = ref;
            }
        }
    }

}


int VulkanAV1Decoder::IsSkipModeAllowed()
{
    av1_seq_param_s *const sps = m_sps.Get();
    VkParserAv1PictureData* pic_info = &m_PicData;

    if (!sps->flags.enable_order_hint || IsFrameIntra() ||
        pic_info->reference_mode == AV1_SINGLE_PREDICTION_ONLY) {
        return 0;
    }

    // Identify the nearest forward and backward references.
    int ref0 = -1, ref1 = -1;
    int ref0_off = -1, ref1_off = -1;
    for (int i = 0; i < REFS_PER_FRAME; i++) {
        int frame_idx = ref_frame_idx[i];
        if (frame_idx != -1) {
            int ref_frame_offset = RefOrderHint[frame_idx];

            int rel_off = GetRelativeDist1(ref_frame_offset, frame_offset);
            // Forward reference
            if (rel_off < 0
                && (ref0_off == -1 || GetRelativeDist1(ref_frame_offset, ref0_off) > 0)) {
                ref0 = i + LAST_FRAME;
                ref0_off = ref_frame_offset;
            }
            // Backward reference
            if (rel_off > 0
                && (ref1_off == -1 || GetRelativeDist1(ref_frame_offset, ref1_off) < 0)) {
                ref1 = i + LAST_FRAME;
                ref1_off = ref_frame_offset;
            }
        }
    }

    if (ref0 != -1 && ref1 != -1) {
        // == Bi-directional prediction ==
        pic_info->SkipModeFrame0 = std::min(ref0, ref1);
        pic_info->SkipModeFrame1 = std::max(ref0, ref1);
        return 1;
    } else if (ref0 != -1) {
        // == Forward prediction only ==
        // Identify the second nearest forward reference.
        for (int i = 0; i < REFS_PER_FRAME; i++) {
            int frame_idx = ref_frame_idx[i];
            if (frame_idx != -1) {
                int ref_frame_offset = RefOrderHint[frame_idx];
                // Forward reference
                if (GetRelativeDist1(ref_frame_offset, ref0_off) < 0
                    && (ref1_off == -1 || GetRelativeDist1(ref_frame_offset, ref1_off) > 0)) {
                    ref1 = i + LAST_FRAME;
                    ref1_off = ref_frame_offset;
                }
            }
        }
        if (ref1 != -1) {
            pic_info->SkipModeFrame0 = std::min(ref0, ref1);
            pic_info->SkipModeFrame1 = std::max(ref0, ref1);
            return 1;
        }
    }

    return 0;
}

bool VulkanAV1Decoder::ParseObuFrameHeader()
{
    av1_seq_param_s *const sps = m_sps.Get();
    VkParserAv1PictureData* pic_info = &m_PicData;

    int frame_size_override_flag = 0;

    last_frame_type = pic_info->frame_type;
    last_intra_only = intra_only;

    if (sps->flags.reduced_still_picture_header) {
        show_existing_frame = 0;
        showable_frame = 0;
        pic_info->show_frame = 1;
        pic_info->frame_type = STD_VIDEO_AV1_FRAME_TYPE_KEY;
        pic_info->error_resilient_mode = 1;
    } else {
        uint8_t reset_decoder_state = 0;
        show_existing_frame = u(1);

        if (show_existing_frame) {
            int32_t frame_to_show_map_idx = u(3);
            int32_t show_existing_frame_index = frame_to_show_map_idx;

            if (sps->decoder_model_info_present && timing_info.equal_picture_interval == 0) {
                tu_presentation_delay = u(buffer_model.frame_presentation_time_length);
            }
            if (sps->flags.frame_id_numbers_present_flag) {
                int frame_id_length = sps->frame_id_length;
                int display_frame_id = u(frame_id_length);

                if (display_frame_id != ref_frame_id[frame_to_show_map_idx] ||
                    RefValid[frame_to_show_map_idx] == 0) {
                    assert(!"ref frame ID mismatch");
                }
            }
            if (!m_pBuffers[show_existing_frame_index].buffer) {
                assert("Error: Frame not decoded yet\n");
                return false;
            }

            reset_decoder_state = m_pBuffers[show_existing_frame_index].frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY;
            pic_info->loop_filter_level[0] = pic_info->loop_filter_level[1] = 0;
            pic_info->show_frame = 1;
            showable_frame = m_pBuffers[show_existing_frame_index].showable_frame;

            if (sps->flags.film_grain_params_present) {
                memcpy(&pic_info->fgs, &m_pBuffers[show_existing_frame_index].film_grain_params, sizeof(av1_film_grain_s));
            }

            if (reset_decoder_state) {
                showable_frame = 0;
                pic_info->frame_type = STD_VIDEO_AV1_FRAME_TYPE_KEY;
                refresh_frame_flags = (1 << NUM_REF_FRAMES) - 1;
                // load loop filter params
                memcpy(pic_info->loop_filter_ref_deltas, m_pBuffers[show_existing_frame_index].lf_ref_delta, sizeof(lf_ref_delta_default));
                memcpy(pic_info->loop_filter_mode_deltas, m_pBuffers[show_existing_frame_index].lf_mode_delta, sizeof(pic_info->loop_filter_mode_deltas));
                // load global motions
                memcpy(&global_motions, &m_pBuffers[show_existing_frame_index].global_models, sizeof(AV1WarpedMotionParams) * GM_GLOBAL_MODELS_PER_FRAME);
                // load segmentation
                memcpy(pic_info->segmentation_feature_enable, &m_pBuffers[show_existing_frame_index].seg.feature_enable, sizeof(pic_info->segmentation_feature_enable));
                memcpy(pic_info->segmentation_feature_data, &m_pBuffers[show_existing_frame_index].seg.feature_data, sizeof(pic_info->segmentation_feature_data));
                pic_info->segid_preskip = m_pBuffers[show_existing_frame_index].seg.last_active_id;
                pic_info->last_active_segid = m_pBuffers[show_existing_frame_index].seg.preskip_id;
                frame_offset = RefOrderHint[show_existing_frame_index];
                UpdateFramePointers(m_pBuffers[show_existing_frame_index].buffer);
            } else {
                refresh_frame_flags = 0;
            }

            VkPicIf* pDispPic = m_pBuffers[show_existing_frame_index].buffer;
            if (pDispPic)
                pDispPic->AddRef();
            AddBuffertoOutputQueue(pDispPic, !!showable_frame);

            return true;
        }
        pic_info->frame_type = (StdVideoAV1FrameType)u(2);
        intra_only = pic_info->frame_type == STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY;
        pic_info->show_frame = u(1);
        if (pic_info->show_frame) {
            if (sps->decoder_model_info_present && timing_info.equal_picture_interval == 0) {
                tu_presentation_delay = u(buffer_model.frame_presentation_time_length);
            }
            showable_frame = pic_info->frame_type != STD_VIDEO_AV1_FRAME_TYPE_KEY;
        } else {
            showable_frame = u(1);
        }

        pic_info->error_resilient_mode = (pic_info->frame_type == STD_VIDEO_AV1_FRAME_TYPE_SWITCH || (pic_info->frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY && pic_info->show_frame)) ? 1 : u(1);
    }


    if (pic_info->frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY && pic_info->show_frame) {
        for (int i = 0; i < NUM_REF_FRAMES; i++) {
            RefValid[i] = 0;
            RefOrderHint[i] = 0;
        }
    }

    pic_info->disable_cdf_update = u(1);
    if (sps->force_screen_content_tools == SELECT_SCREEN_CONTENT_TOOLS) {
        pic_info->allow_screen_content_tools = u(1);
    } else {
        pic_info->allow_screen_content_tools = sps->force_screen_content_tools;
    }

    if (pic_info->allow_screen_content_tools) {
        if (sps->force_integer_mv == SELECT_INTEGER_MV) {
            pic_info->force_integer_mv = u(1);
        }
        else {
            pic_info->force_integer_mv = sps->force_integer_mv;
        }
    } else {
        pic_info->force_integer_mv = 0;
    }

    if (IsFrameIntra()) {
        pic_info->force_integer_mv = 1;
    }

    int32_t frame_refs_short_signaling = 0;
    pic_info->allow_intrabc = 0;
    primary_ref_frame = PRIMARY_REF_NONE;
    frame_size_override_flag = 0;

    if (!sps->flags.reduced_still_picture_header) {
        if (sps->flags.frame_id_numbers_present_flag) {
            int frame_id_length = sps->frame_id_length;
            int diff_len = sps->delta_frame_id_length;
            int prev_frame_id = 0;
            if (!(pic_info->frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY && pic_info->show_frame)) {
                prev_frame_id = current_frame_id;
            }
            current_frame_id = u(frame_id_length);

            if (!(pic_info->frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY && pic_info->show_frame)) {
                int diff_frame_id = 0;
                if (current_frame_id > prev_frame_id) {
                    diff_frame_id = current_frame_id - prev_frame_id;
                } else {
                    diff_frame_id = (1 << frame_id_length) + current_frame_id - prev_frame_id;
                }
                // check for conformance
                if (prev_frame_id == current_frame_id || diff_frame_id >= (1 << (frame_id_length - 1))) {
                    // Invalid current_frame_id
                    // return 0;
                }
            }
            // Mark ref frames not valid for referencing 
            for (int i = 0; i < NUM_REF_FRAMES; i++) {
                if (pic_info->frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY && pic_info->show_frame) {
                    RefValid[i] = 0;
                } else if (current_frame_id > (1 << diff_len)) {
                    if (ref_frame_id[i] > current_frame_id ||
                        ref_frame_id[i] < current_frame_id - (1 << diff_len))
                        RefValid[i] = 0;
                } else {
                    if (ref_frame_id[i] > current_frame_id &&
                        ref_frame_id[i] < (1 << frame_id_length) + current_frame_id - (1 << diff_len))
                        RefValid[i] = 0;
                }
            }
        } else {
            current_frame_id = 0;
        }

        frame_size_override_flag = pic_info->frame_type == STD_VIDEO_AV1_FRAME_TYPE_SWITCH ? 1 : u(1);
        //order_hint
        frame_offset = sps->flags.enable_order_hint ? u(sps->order_hint_bits_minus_1 + 1) : 0;

        if (!pic_info->error_resilient_mode && !(IsFrameIntra())) {
            primary_ref_frame = u(3);
        }
    }

    if (sps->decoder_model_info_present) {
        uint8_t buffer_removal_time_present = u(1);
        if (buffer_removal_time_present) {
            for (int opNum = 0; opNum <= sps->operating_points_cnt_minus_1; opNum++) {
                if (op_params[opNum].decoder_model_param_present) {
                    int opPtIdc = sps->operating_point_idc[opNum];
                    int inTemporalLayer = (opPtIdc >> temporal_id) & 1;
                    int inSpatialLyaer = (opPtIdc >> (spatial_id + 8)) & 1;
                    if (opPtIdc == 0 || (inTemporalLayer&&inSpatialLyaer)) {
                        op_frame_timing[opNum] = u(buffer_model.buffer_removal_time_length);
                    } else {
                        op_frame_timing[opNum] = 0;
                    }
                } else {
                    op_frame_timing[opNum] = 0;
                }
            }
        }
    }
    if (pic_info->frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY) {
        if (!pic_info->show_frame) {
            refresh_frame_flags = u(8);
        } else {
            refresh_frame_flags = (1 << NUM_REF_FRAMES) - 1;
        }

        for (int i = 0; i < REFS_PER_FRAME; i++) {
            ref_frame_idx[i] = 0;
        }

        // memset(&ref_frame_names, -1, sizeof(ref_frame_names));
    } else {
        if (intra_only || pic_info->frame_type != 3) {
            refresh_frame_flags = u(NUM_REF_FRAMES);
            if (refresh_frame_flags == 0xFF && intra_only) {
                assert(!"Intra_only frames cannot have refresh flags 0xFF");
            }

            //  memset(&ref_frame_names, -1, sizeof(ref_frame_names));
        } else {
            refresh_frame_flags = (1 << NUM_REF_FRAMES) - 1;
        }
    }

    if (((!IsFrameIntra()) || refresh_frame_flags != 0xFF) &&
        pic_info->error_resilient_mode && sps->flags.enable_order_hint) {
        for (int buf_idx = 0; buf_idx < NUM_REF_FRAMES; buf_idx++) {
            // ref_order_hint[i]
            int offset = u(sps->order_hint_bits_minus_1 + 1);
            // assert(buf_idx < FRAME_BUFFERS);
            if (buf_idx == -1 || offset != RefOrderHint[buf_idx]) {
                //RefValid[buf_idx] = 0;
                //RefOrderHint[buf_idx] = frame_offset;
                assert(0);
            }
        }
    }

    if (IsFrameIntra()) {
        SetupFrameSize(frame_size_override_flag);

        if (pic_info->allow_screen_content_tools && frame_width == upscaled_width) {
            pic_info->allow_intrabc = u(1);
        }
        pic_info->use_ref_frame_mvs = 0;
    } else {
        pic_info->use_ref_frame_mvs = 0;
        // if (pbi->need_resync != 1)
        {
            if (sps->flags.enable_order_hint) {
                frame_refs_short_signaling = u(1);
            } else {
                frame_refs_short_signaling = 0;
            }

            if (frame_refs_short_signaling) {
                const int lst_ref = u(REF_FRAMES_BITS);
                const int lst_idx = lst_ref;

                const int gld_ref = u(REF_FRAMES_BITS);
                const int gld_idx = gld_ref;

                if (lst_idx == -1 || gld_idx == -1) {
                    assert(!"invalid reference");
                }

                SetFrameRefs(lst_ref, gld_ref);
            }

            for (int i = 0; i < REFS_PER_FRAME; i++) {
                if (!frame_refs_short_signaling) {
                    int ref_frame_index = u(REF_FRAMES_BITS);
                    ref_frame_idx[i] = ref_frame_index;

                    if (ref_frame_index == -1) {
                        assert(!"invalid reference");
                    }
                    ref_frame_idx[i] = ref_frame_index;
                }

                if (sps->flags.frame_id_numbers_present_flag) {
                    int frame_id_length = sps->frame_id_length;
                    int diff_len = sps->delta_frame_id_length;
                    int delta_frame_id_minus_1 = u(diff_len);
                    int ref_id = ((current_frame_id - (delta_frame_id_minus_1 + 1) +
                        (1 << frame_id_length)) % (1 << frame_id_length));

                    if (ref_id != ref_frame_id[ref_frame_idx[i]] || RefValid[ref_frame_idx[i]] == 0) {
                        //assert(!"Ref frame ID mismatch");
                    }
                }
            }

            if (!pic_info->error_resilient_mode && frame_size_override_flag) {
                SetupFrameSizeWithRefs();
            } else {
                SetupFrameSize(frame_size_override_flag);
            }

            if (pic_info->force_integer_mv) {
                pic_info->allow_high_precision_mv = 0;
            } else {
                pic_info->allow_high_precision_mv = u(1);
            }

            //read_interpolation_filter
            int tmp = u(1);
            pic_info->is_filter_switchable = tmp;
            if (tmp) {
                pic_info->interp_filter = AV1_SWITCHABLE;
            } else {
                pic_info->interp_filter = u(2);
            }
            pic_info->switchable_motion_mode = u(1);
        }

        if (!pic_info->error_resilient_mode && sps->flags.enable_ref_frame_mvs &&
            sps->flags.enable_order_hint && !IsFrameIntra()) {
            pic_info->use_ref_frame_mvs = u(1);
        } else {
            pic_info->use_ref_frame_mvs = 0;
        }

        for (int i = 0; i < 8; i++)
        {
            pic_info->ref_order_hint[i] = RefOrderHint[i];
        }

        /*      for (int i = 0; i < REFS_PER_FRAME; ++i)
                {
                    RefBuffer *const ref_buf = &cm->frame_refs[i];
                    av1_setup_scale_factors_for_frame(
                        &ref_buf->sf, ref_buf->buf->y_crop_width,
                        ref_buf->buf->y_crop_height, cm->width, cm->height);
                    if ((!av1_is_valid_scale(&ref_buf->sf)))
                        aom_internal_error(&cm->error, AOM_CODEC_UNSUP_BITSTREAM,
                            "Reference frame has invalid dimensions");
                }
        */
    }

    if (sps->flags.frame_id_numbers_present_flag) {
        // Update reference frame id's
        int tmp_flags = refresh_frame_flags;
        for (int i = 0; i < NUM_REF_FRAMES; i++) {
            if ((tmp_flags >> i) & 1) {
                ref_frame_id[i] = current_frame_id;
                RefValid[i] = 1;
            }
        }
    }

    if (!(sps->flags.reduced_still_picture_header) && !(pic_info->disable_cdf_update)) {
        pic_info->disable_frame_end_update_cdf = u(1);
    } else {
        pic_info->disable_frame_end_update_cdf = 1;
    }

    // tile_info
    DecodeTileInfo();
    DecodeQuantizationData();
    DecodeSegmentationData();

    pic_info->delta_q_res = 0;
    pic_info->delta_lf_res = 0;
    pic_info->delta_lf_present = 0;
    pic_info->delta_lf_multi = 0;
    pic_info->delta_q_present = pic_info->base_qindex > 0 ? u(1) : 0;
    if (pic_info->delta_q_present) {
        pic_info->delta_q_res = u(2); // 1 << u(2); use log2(). Shift is done at HW
        if (!pic_info->allow_intrabc) {
            pic_info->delta_lf_present = u(1);
        }
        if (pic_info->delta_lf_present) {
            pic_info->delta_lf_res = u(2); //1 << u(2);
            pic_info->delta_lf_multi = u(1);
            // av1_reset_loop_filter_delta(xd, av1_num_planes(cm)); // FIXME
        }
    }

    for (int i = 0; i < MAX_SEGMENTS; ++i) {
        int qindex = pic_info->segmentation_enabled && pic_info->segmentation_feature_enable[i][0]
            ? pic_info->segmentation_feature_data[i][0] + pic_info->base_qindex : pic_info->base_qindex;
        qindex = CLAMP(qindex, 0, 255);
        lossless[i] = qindex == 0 && pic_info->qp_y_dc_delta_q == 0 &&
            pic_info->qp_u_dc_delta_q == 0 && pic_info->qp_u_ac_delta_q == 0 &&
            pic_info->qp_v_dc_delta_q == 0 && pic_info->qp_v_ac_delta_q == 0;
    }

    pic_info->coded_lossless = lossless[0];
    if (pic_info->segmentation_enabled) {
        for (int i = 1; i < MAX_SEGMENTS; i++) {
            pic_info->coded_lossless &= lossless[i];
        }
    }

    all_lossless = pic_info->coded_lossless && (frame_width == upscaled_width);
    // setup_segmentation_dequant();  //FIXME
    if (pic_info->coded_lossless) {
        pic_info->loop_filter_level[0] = 0;
        pic_info->loop_filter_level[1] = 0;
    }
    if (pic_info->coded_lossless || !sps->flags.enable_cdef) {
        pic_info->cdef_bits = 0;
        //cm->cdef_strengths[0] = 0;
        //cm->cdef_uv_strengths[0] = 0;
    }
    if (all_lossless || !sps->flags.enable_restoration) {
        pic_info->FrameRestorationType[0] = RESTORE_NONE;
        pic_info->FrameRestorationType[1] = RESTORE_NONE;
        pic_info->FrameRestorationType[2] = RESTORE_NONE;
    }
    DecodeLoopFilterdata();

    if (!pic_info->coded_lossless && sps->flags.enable_cdef && !pic_info->allow_intrabc) {
        DecodeCDEFdata();
    }
    if (!all_lossless && sps->flags.enable_restoration && !pic_info->allow_intrabc) {
        DecodeLoopRestorationData();
    }

    pic_info->tx_mode = pic_info->coded_lossless ? AV1_ONLY_4X4 : (u(1) ? AV1_TX_MODE_SELECT : AV1_TX_MODE_LARGEST);
    if (!IsFrameIntra()) {
        pic_info->reference_mode = u(1);
    } else {
        pic_info->reference_mode = AV1_SINGLE_PREDICTION_ONLY;
    }

    pic_info->skip_mode = IsSkipModeAllowed() ? u(1) : 0;

    if (!IsFrameIntra() && !pic_info->error_resilient_mode && sps->flags.enable_warped_motion) {
        pic_info->allow_warped_motion = u(1);
    } else {
        pic_info->allow_warped_motion = 0;
    }

    pic_info->reduced_tx_set = u(1);

    // reset global motions
    for (int i = 0; i < GM_GLOBAL_MODELS_PER_FRAME; ++i) {
        global_motions[i] = default_warp_params;
    }

    if (!IsFrameIntra()) {
        DecodeGlobalMotionParams();
    }

    ReadFilmGrainParams();

    pic_info->refresh_frame_flags = refresh_frame_flags;

    return true;
}


bool VulkanAV1Decoder::ParseObuTileGroup(const AV1ObuHeader& hdr, int num_tiles)
{
    // printf("parse_tile_group: ");
    // for(int i = 0; i < 8; i++)
    //     printf("%02x ", (m_bitstreamData.GetBitstreamPtr() + m_nalu.start_offset)[i]);
    // printf("\n");

	// Tile group header
    int log2_num_tiles = log2_tile_cols + log2_tile_rows;
    bool tile_start_and_end_present_flag = 0;
    if (num_tiles > 1) {
        tile_start_and_end_present_flag = !!(u(1));
    }
    // "For OBU_FRAME type obu tile_start_and_end_present_flag must be 0"
    if (hdr.type == AV1_OBU_FRAME && tile_start_and_end_present_flag) {
		return false;
	}

	int tg_start = 0;
	int tg_end = 0;
    if (num_tiles == 1 || !tile_start_and_end_present_flag) {
        tg_start = 0;
        tg_end = num_tiles - 1;
    } else {
        tg_start = u(log2_num_tiles);
        tg_end = u(log2_num_tiles);
    }

	byte_alignment();
	// Tile payload
    int consumedBytes = (consumed_bits() + 7) / 8;
	//                   offset of obu         number of bytes read getting the tile data
	m_tileOffsets.push_back(m_nalu.start_offset + consumedBytes);

	// Compute the tile group size
	uint32_t totalTileSize = 0;
    for (int TileNum = tg_start; TileNum <= tg_end; TileNum++)
    {
        int lastTile = TileNum == tg_end;
        size_t tileSize = 0;
        if (lastTile)
        {
            tileSize = hdr.payload_size - consumedBytes;
        }
        else
        {
            uint64_t tile_size_minus_1 = le(tile_size_bytes_minus_1 + 1);
            tileSize = tile_size_minus_1 + 1;
        }

        totalTileSize += tileSize;
    }

	m_tileSizes.push_back(totalTileSize);
    return (tg_end == num_tiles - 1);
}

bool IsObuInCurrentOperatingPoint(int  current_operating_point, AV1ObuHeader *hdr) {
    if (current_operating_point == 0) return true;
    if (((current_operating_point >> hdr->temporal_id) & 0x1) &&
        ((current_operating_point >> (hdr->spatial_id + 8)) & 0x1)) {
        return true;
    }

    return false;
}

bool VulkanAV1Decoder::ParseOneFrame(const uint8_t*const pFrameStart, const int32_t frameSizeBytes, const VkParserBitstreamPacket* pck, int* pParsedBytes)
{
    m_bSPSChanged = false;
    uint32_t consumedBytes = 0;
    AV1ObuHeader hdr;

	const uint8_t* pCurrOBU = pFrameStart;
	int32_t remainingFrameBytes = frameSizeBytes;

    while (remainingFrameBytes > 0) {
        memset(&hdr, 0, sizeof(hdr));
		// NOTE: This does not modify any bitstream reader stare.
        if (!ParseOBUHeaderAndSize(pCurrOBU, remainingFrameBytes, &hdr)) {
            // OBU header parsing failed
            return false;
        }

        if (remainingFrameBytes < int(hdr.payload_size + hdr.header_size)) {
            // Error: Truncated frame data
            return false;
        }

        m_nalu.start_offset += hdr.header_size;

        temporal_id = hdr.temporal_id;
        spatial_id = hdr.spatial_id;
        if (hdr.type != AV1_OBU_TEMPORAL_DELIMITER && hdr.type != AV1_OBU_SEQUENCE_HEADER && hdr.type != AV1_OBU_PADDING) {
            if (!IsObuInCurrentOperatingPoint(m_OperatingPointIDCActive, &hdr)) { // TODO: || !DecodeAllLayers
                m_nalu.start_offset += hdr.payload_size;
                pCurrOBU  += (hdr.payload_size + hdr.header_size);
                remainingFrameBytes -= (hdr.payload_size + hdr.header_size);
                continue;
            }
        }

		// Prime the bit buffer with the 4 bytes
        init_dbits();
        switch (hdr.type) {
        case AV1_OBU_TEMPORAL_DELIMITER:
            ParseObuTemporalDelimiter();
            break;

        case AV1_OBU_SEQUENCE_HEADER:
            ParseObuSequenceHeader();
            break;

        case AV1_OBU_FRAME_HEADER:
        case AV1_OBU_FRAME:
        {
            m_tileOffsets.clear();
            m_tileSizes.clear();
            ParseObuFrameHeader();
            if (show_existing_frame) break;
            if (hdr.type != AV1_OBU_FRAME) {
                rbsp_trailing_bits();
            }

            if (hdr.type != AV1_OBU_FRAME) break;

			byte_alignment();
        }   // fall through

        case AV1_OBU_TILE_GROUP:
        {
            int numTiles = m_PicData.num_tile_cols * m_PicData.num_tile_rows;
            if (ParseObuTileGroup(hdr, numTiles)) {
				// Last tile group for this frame
		        consumedBytes = (consumed_bits() + 7) / 8;
		        assert(consumedBytes < hdr.payload_size);
		        uint32_t tileGroupSizeBytes = hdr.payload_size - consumedBytes;
		        assert(m_tileOffsets.size() == m_tileSizes.size());
		        assert(m_tileOffsets.size() == numTiles);
		        //assert((m_nalu.start_offset + consumedBytes + tileGroupSizeBytes) == frameSizeBytes);
                if (!end_of_picture(frameSizeBytes))
                    return false;
            }
            break;
        }
        case AV1_OBU_REDUNDANT_FRAME_HEADER:
        case AV1_OBU_PADDING:
        case AV1_OBU_METADATA:
        default:
            break;
        }

		// The header was skipped over to parse the payload.
        m_nalu.start_offset += hdr.payload_size;

        pCurrOBU += (hdr.payload_size + hdr.header_size);
        remainingFrameBytes -= (hdr.payload_size + hdr.header_size);

        assert(remainingFrameBytes >= 0);
    }

    if (pParsedBytes) { // TODO: How is this useful with a boolean return value?
        *pParsedBytes += (int)pck->nDataLength;
    }

    return true;
}

bool VulkanAV1Decoder::ParseByteStream(const VkParserBitstreamPacket* pck, size_t* pParsedBytes)
{
    const uint8_t* pdataStart = (pck->nDataLength > 0) ? pck->pByteStream : nullptr;
    const uint8_t* pdataEnd = (pck->nDataLength > 0) ? pck->pByteStream + pck->nDataLength : nullptr;
    int datasize = (int)pck->nDataLength;

    if (pParsedBytes) {
        *pParsedBytes = 0;
    }

    if (m_bitstreamData.GetBitstreamPtr() == nullptr) {
        // make sure we're initialized
        return false;
    }

    m_nCallbackEventCount = 0;

    // Handle discontinuity
    if (pck->bDiscontinuity) {
        memset(&m_nalu, 0, sizeof(m_nalu));
        memset(&m_PTSQueue, 0, sizeof(m_PTSQueue));
        m_bDiscontinuityReported = true;
    }

    if (pck->bPTSValid) {
        m_PTSQueue[m_lPTSPos].bPTSValid = true;
        m_PTSQueue[m_lPTSPos].llPTS = pck->llPTS;
        m_PTSQueue[m_lPTSPos].llPTSPos = m_llParsedBytes;
        m_PTSQueue[m_lPTSPos].bDiscontinuity = m_bDiscontinuityReported;
        m_bDiscontinuityReported = false;
        m_lPTSPos = (m_lPTSPos + 1) % MAX_QUEUED_PTS;
    }

    // Decode in serial mode.
    while (pdataStart < pdataEnd) {
        uint32_t frame_size = 0;
        frame_size = datasize;

        if (frame_size > (uint32_t)m_bitstreamDataLen) {
            if (!resizeBitstreamBuffer(frame_size - (m_bitstreamDataLen))) {
                // Error: Failed to resize bitstream buffer
                return false;
            }
        }

        if (datasize > 0) {
            m_nalu.start_offset = 0;
            m_nalu.end_offset = frame_size;
            memcpy(m_bitstreamData.GetBitstreamPtr(), pdataStart, frame_size);
            m_llNaluStartLocation = m_llFrameStartLocation = m_llParsedBytes; // TODO: NaluStart and FrameStart are always the same here
            m_llParsedBytes += frame_size;

        }
        int parsedBytes = 0;
        if (!ParseOneFrame(pdataStart, frame_size, pck, &parsedBytes)) {
            return false;
        }

		if (pParsedBytes)
			*pParsedBytes = parsedBytes;

        pdataStart += frame_size;
        // Allow extra zero bytes after the frame end
        while (pdataStart < pdataEnd) {
            const uint8_t marker = pdataStart[0];
            if (marker) break;
            ++pdataStart;
        }
    }

    // display frames from output queue
    int index = 0;
    while (index < m_numOutFrames) {
        AddBuffertoDispQueue(m_pOutFrame[index]);
        lEndPicture(m_pOutFrame[index], !m_showableFrame[index]);
        if (m_pOutFrame[index]) {
            m_pOutFrame[index]->Release();
            m_pOutFrame[index] = nullptr;
        }
        index++;
    }
    m_numOutFrames = 0;

    // flush if EOS set
    if (pck->bEOS) {
        end_of_stream();
    }

    return true;
}

const char* av1_seq_param_s::m_refClassId = "av1SpsVideoPictureParametersSet";

#endif // ENABLE_AV1_DECODER
