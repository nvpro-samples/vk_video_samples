#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#include <cpudetect.h>
#include "NextStartCode.h"

size_t NextStartCodeSSSE3(const uint8_t *pdatain, size_t datasize, uint32_t& bitBfr, bool& found_start_code)
{
    size_t i = 0;
    size_t datasize32 = (datasize >> 5) << 5;
    if (datasize32 > 32)
    {
        const __m128i v1 = _mm_set1_epi8(1);
        __m128i vdata = _mm_loadu_si128((const __m128i*)pdatain);
        __m128i vBfr = _mm_set1_epi16(((bitBfr << 8) & 0xFF00) | ((bitBfr >> 8) & 0xFF));
        __m128i vdata_prev1 = _mm_alignr_epi8(vdata, vBfr, 15);
        __m128i vdata_prev2 = _mm_alignr_epi8(vdata, vBfr, 14);
        for ( ; i < datasize32 - 32; i += 32)
        {
            for (int c = 0; c < 32; c += 16)
            {
                // hotspot begin
                __m128i vdata_prev1or2 = _mm_or_si128(vdata_prev2, vdata_prev1);
                __m128i vmask = _mm_cmpeq_epi8(_mm_and_si128(vdata, _mm_cmpeq_epi8(vdata_prev1or2, _mm_setzero_si128())), v1);
                const int resmask = _mm_movemask_epi8(vmask);
                // hotspot end
                if (resmask)
                {
                    const int offset = count_trailing_zeros((uint64_t) (resmask & 0xFFFFFFFF));
                    found_start_code = true;
                    bitBfr =  1;
                    return offset + i + c + 1;
                }
                // hotspot begin
                __m128i vdata_next = _mm_loadu_si128((const __m128i*)&pdatain[i + c + 16]);
                vdata_prev1 = _mm_alignr_epi8(vdata_next, vdata, 15);
                vdata_prev2 = _mm_alignr_epi8(vdata_next, vdata, 14);
                vdata = vdata_next;
                // hotspot end
            }
        } // main processing loop end
        bitBfr = (pdatain[i-2] << 8) | pdatain[i-1];
    }
    // process a tail (rest):
    uint32_t bfr = bitBfr;
    do
    {
        bfr = (bfr << 8) | pdatain[i++];
        if ((bfr & 0x00ffffff) == 1) {
            break;
        }
    } while (i < datasize);
    bitBfr = bfr;
    found_start_code = ((bfr & 0x00ffffff) == 1);
    return i;
}
#endif