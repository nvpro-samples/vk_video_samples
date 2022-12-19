/*
 * Copyright 2021 NVIDIA Corporation.  All rights reserved.
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

class ElementaryStream {
   private:
    int nWidth, nHeight, nBitDepth;
    int m_videoCodecType;
    char *m_Input;
    size_t m_InputSize;
    size_t m_BytesRead;
   public:

   private:

   public:
    ElementaryStream(const char *szFilePath, int forceParserType) {
        nWidth = 176;
        nHeight = 144;
        nBitDepth = 8;
        m_Input = nullptr;
        m_InputSize = 0;
        m_BytesRead = 0;

        m_videoCodecType = forceParserType;
        FILE *InputFile = fopen(szFilePath, "rb");
        fseek(InputFile, 0, SEEK_END);
        size_t FileSize = ftell(InputFile);
        fseek(InputFile, 0, SEEK_SET);
        m_Input = new char[FileSize];
        if (m_Input != nullptr) {
            fread(m_Input, 1, FileSize, InputFile);
            m_InputSize = FileSize;
        }

        fclose(InputFile);
    }

    ElementaryStream(const char *Input, const size_t Length, int forceParserType) {
        nWidth = 176;
        nHeight = 144;
        nBitDepth = 8;
        m_Input = nullptr;
        m_InputSize = 0;
        m_BytesRead = 0;

        m_videoCodecType = forceParserType;
        if (Input != nullptr) {
            m_Input = new char[Length + 1];
            strcpy(m_Input, Input);
            m_InputSize = Length;
        }
    }

    ~ElementaryStream() {
        if (m_Input) {
            delete[] m_Input;
        }
    }

    void Rewind() { m_BytesRead = 0; }
    int GetVideoCodec() { return m_videoCodecType; }
    int GetWidth() { return nWidth; }
    int GetHeight() { return nHeight; }
    int GetBitDepth() { return nBitDepth; }
    int GetFrameSize() { return nBitDepth == 8 ? nWidth * nHeight * 3 / 2 : nWidth * nHeight * 3; }
    bool Demux(uint8_t **ppVideo, int *pnVideoBytes) {
        if ((m_Input == nullptr) || (ppVideo == nullptr)) {
            return false;
        }

        if (*ppVideo == nullptr) {
            *ppVideo = (uint8_t*)m_Input;
            *pnVideoBytes = (int)m_InputSize;
            return true;
        }

        if (m_BytesRead >= m_InputSize) {
            return false;
        }

        // Consume bytes and remove them from the input stream.
        m_BytesRead += *pnVideoBytes;
        // Compute and return amount of bytes left.
        *pnVideoBytes = (int)(m_InputSize - m_BytesRead);
        // Compute and return the pointer to data at new offset.
        *ppVideo = (uint8_t *)(m_Input + m_BytesRead);
        return true;
    }

    static int ReadPacket(void *opaque, uint8_t *pBuf, int nBuf) {
        return 0;
    }

    void DumpStreamParameters() {
    }
};

class FFmpegDemuxer : public ElementaryStream {
   public:
    FFmpegDemuxer(const char *file, int forceParserType) : ElementaryStream(file, forceParserType) {}
    FFmpegDemuxer(const char *Input, const size_t Length, int forceParserType) : ElementaryStream(Input, Length, forceParserType) {}
};

inline VkVideoCodecOperationFlagBitsKHR FFmpeg2NvCodecId(int id) { return (VkVideoCodecOperationFlagBitsKHR)id; }
