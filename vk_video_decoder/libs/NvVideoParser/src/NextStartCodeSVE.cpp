#if defined(__aarch64__) // || defined(_M_ARM64)
#include "arm_sve.h"
#include "NextStartCode.h"

#define SVE_REGISTER_MAX_BYTES 256 // 2048 bits
size_t NextStartCodeSVE(const uint8_t *pdatain, size_t datasize, uint32_t& bitBfr, bool& found_start_code)
{
    size_t i = 0;
    {
        static const int lanes = (int)svcntb();

        svbool_t pred = svwhilelt_b8_u64(i, datasize);
        svbool_t pred_next = svpfalse_b();

        svuint8_t vdata = svld1_u8(pred, pdatain);
        svuint8_t vBfr = svreinterpret_u8_u16(svdup_n_u16(((bitBfr << 8) & 0xFF00) | ((bitBfr >> 8) & 0xFF)));

        static uint8_t data0n[SVE_REGISTER_MAX_BYTES];
        static uint8_t isArrayFilled = 0;
        if (!isArrayFilled)
        {
            for (int idx = 0; idx < lanes; idx++)
            {
                data0n[idx] = idx;
            }
            isArrayFilled = 1;
        }
        svuint8_t v0n = svld1_u8(svptrue_b8(), data0n);

        const svbool_t vext15_mask = svcmpge_n_u8(svptrue_b8(), v0n, lanes-1);
        const svbool_t vext14_mask = svcmpge_n_u8(svptrue_b8(), v0n, lanes-2);
        svuint8_t vdata_prev1 = svsplice_u8(vext15_mask, vBfr, vdata); //svext_u8(vdata, vdata_next, lanes-1);
        svuint8_t vdata_prev2 = svsplice_u8(vext14_mask, vBfr, vdata); //svext_u8(vdata, vdata_next, lanes-2);

        for ( ; i < datasize; i += lanes)
        {
            // hotspot begin
            svuint8_t vdata_prev1or2 = svorr_u8_z(pred, vdata_prev2, vdata_prev1);
            svbool_t vmask = svcmpeq_n_u8(svcmpeq_n_u8(pred, vdata_prev1or2, 0), vdata, 1);
            const size_t resmask = svmaxv_u8(vmask, vdata);

            if (resmask)
            {
              const uint8_t offset = svminv_u8(vmask, v0n);
              found_start_code = true;
              bitBfr =  1;
              return (size_t)offset + i + 1;
            }
            // hotspot begin
            pred_next = svwhilelt_b8_u64(i + lanes, datasize);
            svuint8_t vdata_next = svld1_u8(pred_next, &pdatain[i + lanes]);
            vdata_prev1 = svsplice_u8(vext15_mask, vdata, vdata_next); //svext_u8(vdata, vdata_next, lanes-1);
            vdata_prev2 = svsplice_u8(vext14_mask, vdata, vdata_next); //svext_u8(vdata, vdata_next, lanes-2);
            pred = pred_next;
            vdata = vdata_next;
            // hotspot end
        }
    }
    // a very rare case:
    if (datasize >= 2) {
        bitBfr = pdatain[datasize-2];
    }
    bitBfr = (bitBfr << 8) | pdatain[datasize >= 1 ? datasize - 1 : 0];
    found_start_code = false;
    return datasize;
}
#undef SVE_REGISTER_MAX_BYTES
#endif