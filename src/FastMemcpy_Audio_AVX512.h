#ifndef __FAST_MEMCPY_AUDIO_AVX512_H__
#define __FAST_MEMCPY_AUDIO_AVX512_H__

#include <stddef.h>
#include <stdint.h>
#include <immintrin.h>
#include "FastMemcpy_Avx.h"

#ifdef __GNUC__
#define AUDIO_INLINE __inline__ __attribute__((always_inline))
#else
#define AUDIO_INLINE __forceinline
#endif

#ifdef __AVX512F__
//---------------------------------------------------------------------
// 1024-byte aligned copy (16 x 64-byte AVX-512 registers)
//---------------------------------------------------------------------
static AUDIO_INLINE void memcpy_audio_1024_aligned_avx512(void *dst, const void *src) {
    __m512i r0  = _mm512_load_si512(((const __m512i*)src) + 0);
    __m512i r1  = _mm512_load_si512(((const __m512i*)src) + 1);
    __m512i r2  = _mm512_load_si512(((const __m512i*)src) + 2);
    __m512i r3  = _mm512_load_si512(((const __m512i*)src) + 3);
    __m512i r4  = _mm512_load_si512(((const __m512i*)src) + 4);
    __m512i r5  = _mm512_load_si512(((const __m512i*)src) + 5);
    __m512i r6  = _mm512_load_si512(((const __m512i*)src) + 6);
    __m512i r7  = _mm512_load_si512(((const __m512i*)src) + 7);
    __m512i r8  = _mm512_load_si512(((const __m512i*)src) + 8);
    __m512i r9  = _mm512_load_si512(((const __m512i*)src) + 9);
    __m512i r10 = _mm512_load_si512(((const __m512i*)src) + 10);
    __m512i r11 = _mm512_load_si512(((const __m512i*)src) + 11);
    __m512i r12 = _mm512_load_si512(((const __m512i*)src) + 12);
    __m512i r13 = _mm512_load_si512(((const __m512i*)src) + 13);
    __m512i r14 = _mm512_load_si512(((const __m512i*)src) + 14);
    __m512i r15 = _mm512_load_si512(((const __m512i*)src) + 15);

    _mm512_store_si512(((__m512i*)dst) + 0,  r0);
    _mm512_store_si512(((__m512i*)dst) + 1,  r1);
    _mm512_store_si512(((__m512i*)dst) + 2,  r2);
    _mm512_store_si512(((__m512i*)dst) + 3,  r3);
    _mm512_store_si512(((__m512i*)dst) + 4,  r4);
    _mm512_store_si512(((__m512i*)dst) + 5,  r5);
    _mm512_store_si512(((__m512i*)dst) + 6,  r6);
    _mm512_store_si512(((__m512i*)dst) + 7,  r7);
    _mm512_store_si512(((__m512i*)dst) + 8,  r8);
    _mm512_store_si512(((__m512i*)dst) + 9,  r9);
    _mm512_store_si512(((__m512i*)dst) + 10, r10);
    _mm512_store_si512(((__m512i*)dst) + 11, r11);
    _mm512_store_si512(((__m512i*)dst) + 12, r12);
    _mm512_store_si512(((__m512i*)dst) + 13, r13);
    _mm512_store_si512(((__m512i*)dst) + 14, r14);
    _mm512_store_si512(((__m512i*)dst) + 15, r15);
}

//---------------------------------------------------------------------
// 1024-byte unaligned copy (AVX-512)
//---------------------------------------------------------------------
static AUDIO_INLINE void memcpy_audio_1024_unaligned_avx512(void *dst, const void *src) {
    __m512i r0  = _mm512_loadu_si512(((const __m512i*)src) + 0);
    __m512i r1  = _mm512_loadu_si512(((const __m512i*)src) + 1);
    __m512i r2  = _mm512_loadu_si512(((const __m512i*)src) + 2);
    __m512i r3  = _mm512_loadu_si512(((const __m512i*)src) + 3);
    __m512i r4  = _mm512_loadu_si512(((const __m512i*)src) + 4);
    __m512i r5  = _mm512_loadu_si512(((const __m512i*)src) + 5);
    __m512i r6  = _mm512_loadu_si512(((const __m512i*)src) + 6);
    __m512i r7  = _mm512_loadu_si512(((const __m512i*)src) + 7);
    __m512i r8  = _mm512_loadu_si512(((const __m512i*)src) + 8);
    __m512i r9  = _mm512_loadu_si512(((const __m512i*)src) + 9);
    __m512i r10 = _mm512_loadu_si512(((const __m512i*)src) + 10);
    __m512i r11 = _mm512_loadu_si512(((const __m512i*)src) + 11);
    __m512i r12 = _mm512_loadu_si512(((const __m512i*)src) + 12);
    __m512i r13 = _mm512_loadu_si512(((const __m512i*)src) + 13);
    __m512i r14 = _mm512_loadu_si512(((const __m512i*)src) + 14);
    __m512i r15 = _mm512_loadu_si512(((const __m512i*)src) + 15);

    _mm512_storeu_si512(((__m512i*)dst) + 0,  r0);
    _mm512_storeu_si512(((__m512i*)dst) + 1,  r1);
    _mm512_storeu_si512(((__m512i*)dst) + 2,  r2);
    _mm512_storeu_si512(((__m512i*)dst) + 3,  r3);
    _mm512_storeu_si512(((__m512i*)dst) + 4,  r4);
    _mm512_storeu_si512(((__m512i*)dst) + 5,  r5);
    _mm512_storeu_si512(((__m512i*)dst) + 6,  r6);
    _mm512_storeu_si512(((__m512i*)dst) + 7,  r7);
    _mm512_storeu_si512(((__m512i*)dst) + 8,  r8);
    _mm512_storeu_si512(((__m512i*)dst) + 9,  r9);
    _mm512_storeu_si512(((__m512i*)dst) + 10, r10);
    _mm512_storeu_si512(((__m512i*)dst) + 11, r11);
    _mm512_storeu_si512(((__m512i*)dst) + 12, r12);
    _mm512_storeu_si512(((__m512i*)dst) + 13, r13);
    _mm512_storeu_si512(((__m512i*)dst) + 14, r14);
    _mm512_storeu_si512(((__m512i*)dst) + 15, r15);
}

//---------------------------------------------------------------------
// AVX-512 main routine - optimized for large buffers (32KB+)
//---------------------------------------------------------------------
static inline void* memcpy_audio_avx512(void *destination, const void *source, size_t size)
{
    unsigned char *dst = (unsigned char*)destination;
    const unsigned char *src = (const unsigned char*)source;

    int is_aligned = (((uintptr_t)src | (uintptr_t)dst) & 63) == 0;

    _mm_prefetch((const char*)src, _MM_HINT_T0);
    _mm_prefetch((const char*)src + 64, _MM_HINT_T0);
    _mm_prefetch((const char*)src + 128, _MM_HINT_T0);
    _mm_prefetch((const char*)src + 192, _MM_HINT_T0);
    _mm_prefetch((const char*)src + 256, _MM_HINT_T0);
    _mm_prefetch((const char*)src + 320, _MM_HINT_T0);

    if (is_aligned) {
        for (; size >= 1024; size -= 1024) {
            _mm_prefetch((const char*)(src + 2048), _MM_HINT_T0);
            _mm_prefetch((const char*)(src + 2112), _MM_HINT_T0);
            _mm_prefetch((const char*)(src + 2176), _MM_HINT_T0);
            _mm_prefetch((const char*)(src + 2240), _MM_HINT_T0);

            memcpy_audio_1024_aligned_avx512(dst, src);

            src += 1024;
            dst += 1024;
        }
    } else {
        for (; size >= 1024; size -= 1024) {
            _mm_prefetch((const char*)(src + 2048), _MM_HINT_T0);
            _mm_prefetch((const char*)(src + 2112), _MM_HINT_T0);
            _mm_prefetch((const char*)(src + 2176), _MM_HINT_T0);
            _mm_prefetch((const char*)(src + 2240), _MM_HINT_T0);

            memcpy_audio_1024_unaligned_avx512(dst, src);

            src += 1024;
            dst += 1024;
        }
    }

    if (size >= 512) {
        __m512i r0 = _mm512_loadu_si512(((const __m512i*)src) + 0);
        __m512i r1 = _mm512_loadu_si512(((const __m512i*)src) + 1);
        __m512i r2 = _mm512_loadu_si512(((const __m512i*)src) + 2);
        __m512i r3 = _mm512_loadu_si512(((const __m512i*)src) + 3);
        __m512i r4 = _mm512_loadu_si512(((const __m512i*)src) + 4);
        __m512i r5 = _mm512_loadu_si512(((const __m512i*)src) + 5);
        __m512i r6 = _mm512_loadu_si512(((const __m512i*)src) + 6);
        __m512i r7 = _mm512_loadu_si512(((const __m512i*)src) + 7);
        _mm512_storeu_si512(((__m512i*)dst) + 0, r0);
        _mm512_storeu_si512(((__m512i*)dst) + 1, r1);
        _mm512_storeu_si512(((__m512i*)dst) + 2, r2);
        _mm512_storeu_si512(((__m512i*)dst) + 3, r3);
        _mm512_storeu_si512(((__m512i*)dst) + 4, r4);
        _mm512_storeu_si512(((__m512i*)dst) + 5, r5);
        _mm512_storeu_si512(((__m512i*)dst) + 6, r6);
        _mm512_storeu_si512(((__m512i*)dst) + 7, r7);
        src += 512;
        dst += 512;
        size -= 512;
    }

    if (size >= 256) {
        memcpy_avx_256(dst, src);
        src += 256;
        dst += 256;
        size -= 256;
    }

    if (size >= 128) {
        memcpy_avx_128(dst, src);
        src += 128;
        dst += 128;
        size -= 128;
    }

    if (size > 0) {
        memcpy_tiny(dst, src, size);
    }

    _mm256_zeroupper();
    return destination;
}
#endif // __AVX512F__

#endif // __FAST_MEMCPY_AUDIO_AVX512_H__
