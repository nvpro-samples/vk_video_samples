/*
 * vv_assert.h - Custom assert implementation using printf
 * 
 * This header replaces the standard assert.h functionality
 * with a printf-based implementation for debugging.
 */

#ifndef VV_ASSERT_H
#define VV_ASSERT_H

#include <stdio.h>
#include <stdlib.h>

#ifdef NDEBUG
    #define vv_assert(expr) ((void)0)
#else
    #ifdef VV_NO_ABORT
    #define vv_assert(expr) \
        ((expr) ? (void)0 : \
         (printf("Assertion failed: %s, file %s, line %d\n", \
                 #expr, __FILE__, __LINE__), (void)0))
    #else
    #define vv_assert(expr) \
        ((expr) ? (void)0 : \
         (printf("Assertion failed: %s, file %s, line %d\n", \
                #expr, __FILE__, __LINE__), abort()))
    #endif
#endif

#endif /* VV_ASSERT_H */
