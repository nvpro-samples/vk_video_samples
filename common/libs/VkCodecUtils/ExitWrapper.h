#pragma once

#include <cstdio>
#include <cstdlib>

inline void safe_exit(int status) {
#if defined(DECODER_APP_BUILD_AS_LIB) || defined(ENCODER_APP_BUILD_AS_LIB)
    printf("Application would exit with status %d\n", status);
#else
    exit(status);
#endif
} 