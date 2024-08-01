#ifndef CPUDETECT_H
#define CPUDETECT_H

enum SIMD_ISA
{
    NOSIMD = 0,
    SSSE3,
    AVX2,
    AVX512,
    NEON,
    SVE
};

SIMD_ISA check_simd_support();

#endif