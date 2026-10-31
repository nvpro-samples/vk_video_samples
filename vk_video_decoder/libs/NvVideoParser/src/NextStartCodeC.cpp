#include "NextStartCode.h"

size_t NextStartCodeC(const uint8_t *pdatain, size_t datasize, uint32_t& bitBfr, bool& found_start_code)
{
    uint32_t bfr = bitBfr;
    size_t i = 0;
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