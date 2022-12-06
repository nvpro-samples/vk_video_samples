/*
* Copyright 2017-2018 NVIDIA Corporation.  All rights reserved.
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

#pragma once

extern "C" {
#include "gstdemuxeres.h"
}

#include "NvCodecUtils/Logger.h"

// inline bool check(int e, int iLine, const char *szFile) {
//     if (e < 0) {
//         LOG(ERROR) << "General error " << e << " at line " << iLine << " in file " << szFile;
//         return false;
//     }
//     return true;
// }

//#define ck(call) check(call, __LINE__, __FILE__)


typedef struct {
    gint num;
    gint den;
} GstDemuxerRational;

class GstDemuxer {
private:
    GstDemuxerES *demuxer = NULL;
    //AVIOContext *avioc = NULL;
    GstDemuxerESPacket * pkt, * pktFiltered;
    //AVBSFContext *bsfc = NULL;

    GstDemuxerEStream * iVideoStream;
    bool bMp4;
    GstDemuxerVideoCodec eVideoCodec;
    int nWidth, nHeight, nBitDepth;

    GstVideoFormat format;
    /**
     * Codec-specific bitstream restrictions that the stream conforms to.
     */
    int profile;
    int level;

    /**
     * Video only. The aspect ratio (width / height) which a single pixel
     * should have when displayed.
     *
     * When the aspect ratio is unknown / undefined, the numerator should be
     * set to 0 (the denominator may have any value).
     */
    GstDemuxerRational sample_aspect_ratio;

    /**
     * Video only. The order of the fields in interlaced video.
     */
    GstVideoFieldOrder                  field_order;

    /**
     * Video only. Additional colorspace characteristics.
     */
    // enum AVColorRange                  color_range;
    // enum AVColorPrimaries              color_primaries;
    // enum AVColorTransferCharacteristic color_trc;
    // enum AVColorSpace                  color_space;
    // enum AVChromaLocation              chroma_location;

public:
    class DataProvider {
    public:
        virtual ~DataProvider() {}
        virtual int GetData(uint8_t *pBuf, int nBuf) = 0;
    };

private:
    GstDemuxer(GstDemuxerES *demuxer) : demuxer(demuxer) {
        if (!demuxer) {
            LOG(ERROR) << "No AVFormatContext provided.";
            return;
        }

        //LOG(INFO) << "Media format: " << demuxer->iformat->long_name << " (" << fmtc->iformat->name << ")";

        //ck(avformat_find_stream_info(fmtc, NULL));
        iVideoStream = gst_demuxer_es_find_best_stream(demuxer, DEMUXER_ES_STREAM_TYPE_VIDEO);
        if (iVideoStream == nullptr) {
            LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__ << " " << "Could not find stream in input file";
            return;
        }

        //fmtc->streams[iVideoStream]->need_parsing = AVSTREAM_PARSE_NONE;
        eVideoCodec = iVideoStream->data.video.vcodec;
        nWidth = GST_VIDEO_INFO_WIDTH(&iVideoStream->data.video.info);
        nHeight = GST_VIDEO_INFO_WIDTH(&iVideoStream->data.video.info);
        format = GST_VIDEO_INFO_FORMAT(&iVideoStream->data.video.info);
        nBitDepth = 8;
        if (format == GST_VIDEO_FORMAT_I420_10LE)
            nBitDepth = 10;
        if (format== GST_VIDEO_FORMAT_I420_12LE)
            nBitDepth = 12;

        // bMp4 = (!strcmp(fmtc->iformat->long_name, "QuickTime / MOV") ||
        //         !strcmp(fmtc->iformat->long_name, "FLV (Flash Video)") ||
        //         !strcmp(fmtc->iformat->long_name, "Matroska / WebM"));

        /**
         * Codec-specific bitstream restrictions that the stream conforms to.
         */
        //FIXME
        //profile = iVideoStream->data.video.profile;
        //level = iVideoStream->data.video.level;

        /**
         * Video only. The aspect ratio (width / height) which a single pixel
         * should have when displayed.
         *
         * When the aspect ratio is unknown / undefined, the numerator should be
         * set to 0 (the denominator may have any value).
         */
        sample_aspect_ratio.num = GST_VIDEO_INFO_PAR_N(&iVideoStream->data.video.info);
        sample_aspect_ratio.den = GST_VIDEO_INFO_PAR_D(&iVideoStream->data.video.info);

        /**
         * Video only. The order of the fields in interlaced video.
         */
        field_order = GST_VIDEO_INFO_FIELD_ORDER(&iVideoStream->data.video.info);

        /**
         * Video only. Additional colorspace characteristics.
         */
        // color_range = fmtc->streams[iVideoStream]->codecpar->color_range;
        // color_primaries = fmtc->streams[iVideoStream]->codecpar->color_primaries;
        // color_trc = fmtc->streams[iVideoStream]->codecpar->color_trc;
        // color_space = fmtc->streams[iVideoStream]->codecpar->color_space;
        // chroma_location = fmtc->streams[iVideoStream]->codecpar->chroma_location;

        // av_init_packet(&pkt);
        // pkt.data = NULL;
        // pkt.size = 0;
        // av_init_packet(&pktFiltered);
        // pktFiltered.data = NULL;
        // pktFiltered.size = 0;

        // if (bMp4) {
        //     const AVBitStreamFilter *bsf = NULL;

        //     if (eVideoCodec == AV_CODEC_ID_H264) {
        //         bsf = av_bsf_get_by_name("h264_mp4toannexb");
        //     } else if (eVideoCodec == AV_CODEC_ID_HEVC) {
        //         bsf = av_bsf_get_by_name("hevc_mp4toannexb");
        //     }

        //     if (!bsf) {
        //         LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__ << " " << "av_bsf_get_by_name(): " << eVideoCodec << " failed";
        //         return;
        //     }
        //     ck(av_bsf_alloc(bsf, &bsfc));
        //     bsfc->par_in = fmtc->streams[iVideoStream]->codecpar;
        //     ck(av_bsf_init(bsfc));
        // }
    }

    // AVFormatContext *CreateFormatContext(DataProvider *pDataProvider) {

    //     AVFormatContext *ctx = NULL;
    //     if (!(ctx = avformat_alloc_context())) {
    //         LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__;
    //         return NULL;
    //     }

    //     uint8_t *avioc_buffer = NULL;
    //     int avioc_buffer_size = 8 * 1024 * 1024;
    //     avioc_buffer = (uint8_t *)av_malloc(avioc_buffer_size);
    //     if (!avioc_buffer) {
    //         LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__;
    //         return NULL;
    //     }
    //     avioc = avio_alloc_context(avioc_buffer, avioc_buffer_size,
    //         0, pDataProvider, &ReadPacket, NULL, NULL);
    //     if (!avioc) {
    //         LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__;
    //         return NULL;
    //     }
    //     ctx->pb = avioc;

    //     ck(avformat_open_input(&ctx, NULL, NULL, NULL));
    //     return ctx;
    // }

    GstDemuxerES *CreateFormatContext(const char *szFilePath) {
        //avformat_network_init();

        GstDemuxerES *demuxer = NULL;
        demuxer = gst_demuxer_es_new(szFilePath);
        pkt = NULL;
        return demuxer;
    }

public:
    GstDemuxer(const char *szFilePath) : GstDemuxer(CreateFormatContext(szFilePath)) {}
    //GstDemuxer(DataProvider *pDataProvider) : GstDemuxer(CreateFormatContext(pDataProvider)) {}
    ~GstDemuxer() {
        if (pkt) {
            gst_demuxer_es_clear_packet(pkt);
        }
        // if (pktFiltered.data) {
        //     av_packet_unref(&pktFiltered);
        // }
        gst_demuxer_es_teardown(demuxer);
        // avformat_close_input(&fmtc);
        // if (avioc) {
        //     av_freep(&avioc->buffer);
        //     av_freep(&avioc);
        // }
    }
    GstDemuxerVideoCodec GetVideoCodec() {
        return iVideoStream->data.video.vcodec;
    }
    int GetWidth() {
        return nWidth;
    }
    int GetHeight() {
        return  nHeight;
    }
    int GetBitDepth() {
        return nBitDepth;
    }
    int GetFrameSize() {
        return nBitDepth == 8 ? nWidth * nHeight * 3 / 2: nWidth * nHeight * 3;
    }
    bool Demux(uint8_t **ppVideo, int *pnVideoBytes) {
        if (!demuxer) {
            return false;
        }

        *pnVideoBytes = 0;

        if (pkt) {
            gst_demuxer_es_clear_packet(pkt);
            pkt = NULL;
        }

        int e = 0;
        while ((e = gst_demuxer_es_read_packet(demuxer, &pkt)) < DEMUXER_ES_RESULT_LAST_PACKET && pkt->stream_id != iVideoStream->id) {
            gst_demuxer_es_clear_packet(pkt);
        }
        if (e >= DEMUXER_ES_RESULT_LAST_PACKET) {
            return false;
        }

        // if (bMp4) {
        //     if (pktFiltered.data) {
        //         av_packet_unref(&pktFiltered);
        //     }
        //     ck(av_bsf_send_packet(bsfc, &pkt));
        //     ck(av_bsf_receive_packet(bsfc, &pktFiltered));
        //     *ppVideo = pktFiltered.data;
        //     *pnVideoBytes = pktFiltered.size;
        // } else {
        *ppVideo = pkt->data;
        *pnVideoBytes = pkt->data_size;
        //}

        return true;
    }

    static int ReadPacket(void *opaque, uint8_t *pBuf, int nBuf) {
        return ((DataProvider *)opaque)->GetData(pBuf, nBuf);
    }

    void Rewind(void) { } // TODO: Implement rewind with GStreamer to support loop_count, currently only supported with DECODER_LIB for elementary streams.
    void DumpStreamParameters() {

        std::cout << "Width: "    << nWidth << std::endl;
        std::cout << "Height: "   << nHeight <<  std::endl;
        std::cout << "BitDepth: " << nBitDepth << std::endl;
        std::cout << "Profile: "  << profile << std::endl;
        std::cout << "Level: "    << level << std::endl;
        std::cout << "Aspect Ration: "    << (float)sample_aspect_ratio.num / sample_aspect_ratio.den << std::endl;

        static const char* FieldOrder[] = {
            "UNKNOWN",
            "PROGRESSIVE",
            "TT: Top coded_first, top displayed first",
            "BB: Bottom coded first, bottom displayed first",
            "TB: Top coded first, bottom displayed first",
            "BT: Bottom coded first, top displayed first",
        };
        std::cout << "Field Order: "    << FieldOrder[field_order] << std::endl;

        // static const char* ColorRange[] = {
        //     "UNSPECIFIED",
        //     "MPEG: the normal 219*2^(n-8) MPEG YUV ranges",
        //     "JPEG: the normal     2^n-1   JPEG YUV ranges",
        //     "NB: Not part of ABI",
        // };
        // std::cout << "Color Range: "    << ColorRange[color_range] << std::endl;

        // static const char* ColorPrimaries[] = {
        //     "RESERVED0",
        //     "BT709: also ITU-R BT1361 / IEC 61966-2-4 / SMPTE RP177 Annex B",
        //     "UNSPECIFIED",
        //     "RESERVED",
        //     "BT470M: also FCC Title 47 Code of Federal Regulations 73.682 (a)(20)",

        //     "BT470BG: also ITU-R BT601-6 625 / ITU-R BT1358 625 / ITU-R BT1700 625 PAL & SECAM",
        //     "SMPTE170M: also ITU-R BT601-6 525 / ITU-R BT1358 525 / ITU-R BT1700 NTSC",
        //     "SMPTE240M: also ITU-R BT601-6 525 / ITU-R BT1358 525 / ITU-R BT1700 NTSC",
        //     "FILM: colour filters using Illuminant C",
        //     "BT2020: ITU-R BT2020",
        //     "SMPTE428: SMPTE ST 428-1 (CIE 1931 XYZ)",
        //     "SMPTE431: SMPTE ST 431-2 (2011) / DCI P3",
        //     "SMPTE432: SMPTE ST 432-1 (2010) / P3 D65 / Display P3",
        //     "JEDEC_P22: JEDEC P22 phosphors",
        //     "NB: Not part of ABI",
        // };
        // std::cout << "Color Primaries: "    << ColorPrimaries[color_primaries] << std::endl;

        // static const char* ColorTransferCharacteristic[] = {
        //     "RESERVED0",
        //     "BT709: also ITU-R BT1361",
        //     "UNSPECIFIED",
        //     "RESERVED",
        //     "GAMMA22:  also ITU-R BT470M / ITU-R BT1700 625 PAL & SECAM",
        //     "GAMMA28:  also ITU-R BT470BG",
        //     "SMPTE170M:  also ITU-R BT601-6 525 or 625 / ITU-R BT1358 525 or 625 / ITU-R BT1700 NTSC",
        //     "SMPTE240M",
        //     "LINEAR:  Linear transfer characteristics",
        //     "LOG: Logarithmic transfer characteristic (100:1 range)",
        //     "LOG_SQRT: Logarithmic transfer characteristic (100 * Sqrt(10) : 1 range)",
        //     "IEC61966_2_4: IEC 61966-2-4",
        //     "BT1361_ECG: ITU-R BT1361 Extended Colour Gamut",
        //     "IEC61966_2_1: IEC 61966-2-1 (sRGB or sYCC)",
        //     "BT2020_10: ITU-R BT2020 for 10-bit system",
        //     "BT2020_12: ITU-R BT2020 for 12-bit system",
        //     "SMPTE2084: SMPTE ST 2084 for 10-, 12-, 14- and 16-bit systems",
        //     "SMPTE428:  SMPTE ST 428-1",
        //     "ARIB_STD_B67:  ARIB STD-B67, known as Hybrid log-gamma",
        //     "NB: Not part of ABI",
        // };
        // std::cout << "Color Transfer Characteristic: "    << ColorTransferCharacteristic[color_trc] << std::endl;

        // static const char* ColorSpace[] = {
        //     "RGB:   order of coefficients is actually GBR, also IEC 61966-2-1 (sRGB)",
        //     "BT709:   also ITU-R BT1361 / IEC 61966-2-4 xvYCC709 / SMPTE RP177 Annex B",
        //     "UNSPECIFIED",
        //     "RESERVED",
        //     "FCC:  FCC Title 47 Code of Federal Regulations 73.682 (a)(20)",
        //     "BT470BG:  also ITU-R BT601-6 625 / ITU-R BT1358 625 / ITU-R BT1700 625 PAL & SECAM / IEC 61966-2-4 xvYCC601",
        //     "SMPTE170M:  also ITU-R BT601-6 525 / ITU-R BT1358 525 / ITU-R BT1700 NTSC",
        //     "SMPTE240M:  functionally identical to above",
        //     "YCGCO:  Used by Dirac / VC-2 and H.264 FRext, see ITU-T SG16",
        //     "BT2020_NCL:  ITU-R BT2020 non-constant luminance system",
        //     "BT2020_CL:  ITU-R BT2020 constant luminance system",
        //     "SMPTE2085:  SMPTE 2085, Y'D'zD'x",
        //     "CHROMA_DERIVED_NCL:  Chromaticity-derived non-constant luminance system",
        //     "CHROMA_DERIVED_CL:  Chromaticity-derived constant luminance system",
        //     "ICTCP:  ITU-R BT.2100-0, ICtCp",
        //     "NB:  Not part of ABI",
        // };
        // std::cout << "Color Space: "    << ColorSpace[color_space] << std::endl;

        // static const char* ChromaLocation[] = {
        //     "UNSPECIFIED",
        //     "LEFT: MPEG-2/4 4:2:0, H.264 default for 4:2:0",
        //     "CENTER: MPEG-1 4:2:0, JPEG 4:2:0, H.263 4:2:0",
        //     "TOPLEFT: ITU-R 601, SMPTE 274M 296M S314M(DV 4:1:1), mpeg2 4:2:2",
        //     "TOP",
        //     "BOTTOMLEFT",
        //     "BOTTOM",
        //     "NB:Not part of ABI",
        // };
        // std::cout << "Chroma Location: "    << ChromaLocation[chroma_location] << std::endl;
    }

};

inline VkVideoCodecOperationFlagBitsKHR GStreamer2NvCodecId(GstDemuxerVideoCodec id) {
    switch (id) {
    // case AV_CODEC_ID_MPEG1VIDEO : assert(false); return VkVideoCodecOperationFlagBitsKHR(0);
    // case AV_CODEC_ID_MPEG2VIDEO : assert(false); return VkVideoCodecOperationFlagBitsKHR(0);
    // case AV_CODEC_ID_MPEG4      : assert(false); return VkVideoCodecOperationFlagBitsKHR(0);
    // case AV_CODEC_ID_VC1        : assert(false); return VkVideoCodecOperationFlagBitsKHR(0);
    case DEMUXER_ES_VIDEO_CODEC_H264       : return VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    case DEMUXER_ES_VIDEO_CODEC_H265       : return VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
//     case AV_CODEC_ID_VP8        : assert(false); return VkVideoCodecOperationFlagBitsKHR(0);
// #ifdef VK_EXT_video_decode_vp9
//     case AV_CODEC_ID_VP9        : return VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
// #endif // VK_EXT_video_decode_vp9
//     case AV_CODEC_ID_MJPEG      : assert(false); return VkVideoCodecOperationFlagBitsKHR(0);
    default                     : assert(false); return VkVideoCodecOperationFlagBitsKHR(0);
    }
}
