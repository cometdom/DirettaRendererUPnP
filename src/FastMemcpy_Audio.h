#ifndef __FAST_MEMCPY_AUDIO_H__
#define __FAST_MEMCPY_AUDIO_H__

#include <stddef.h>
#include <stdint.h>
#include <immintrin.h>
#include "FastMemcpy_Avx.h"

#ifdef __GNUC__
#define AUDIO_INLINE __inline__ __attribute__((always_inline))
#else
#define AUDIO_INLINE __forceinline
#endif

//---------------------------------------------------------------------
// 512-byte aligned copy (16 x 32-byte AVX registers)
//---------------------------------------------------------------------
static AUDIO_INLINE void memcpy_audio_512_aligned(void *dst, const void *src) {
    __m256i r0  = _mm256_load_si256(((const __m256i*)src) + 0);
    __m256i r1  = _mm256_load_si256(((const __m256i*)src) + 1);
    __m256i r2  = _mm256_load_si256(((const __m256i*)src) + 2);
    __m256i r3  = _mm256_load_si256(((const __m256i*)src) + 3);
    __m256i r4  = _mm256_load_si256(((const __m256i*)src) + 4);
    __m256i r5  = _mm256_load_si256(((const __m256i*)src) + 5);
    __m256i r6  = _mm256_load_si256(((const __m256i*)src) + 6);
    __m256i r7  = _mm256_load_si256(((const __m256i*)src) + 7);
    __m256i r8  = _mm256_load_si256(((const __m256i*)src) + 8);
    __m256i r9  = _mm256_load_si256(((const __m256i*)src) + 9);
    __m256i r10 = _mm256_load_si256(((const __m256i*)src) + 10);
    __m256i r11 = _mm256_load_si256(((const __m256i*)src) + 11);
    __m256i r12 = _mm256_load_si256(((const __m256i*)src) + 12);
    __m256i r13 = _mm256_load_si256(((const __m256i*)src) + 13);
    __m256i r14 = _mm256_load_si256(((const __m256i*)src) + 14);
    __m256i r15 = _mm256_load_si256(((const __m256i*)src) + 15);

    _mm256_store_si256(((__m256i*)dst) + 0,  r0);
    _mm256_store_si256(((__m256i*)dst) + 1,  r1);
    _mm256_store_si256(((__m256i*)dst) + 2,  r2);
    _mm256_store_si256(((__m256i*)dst) + 3,  r3);
    _mm256_store_si256(((__m256i*)dst) + 4,  r4);
    _mm256_store_si256(((__m256i*)dst) + 5,  r5);
    _mm256_store_si256(((__m256i*)dst) + 6,  r6);
    _mm256_store_si256(((__m256i*)dst) + 7,  r7);
    _mm256_store_si256(((__m256i*)dst) + 8,  r8);
    _mm256_store_si256(((__m256i*)dst) + 9,  r9);
    _mm256_store_si256(((__m256i*)dst) + 10, r10);
    _mm256_store_si256(((__m256i*)dst) + 11, r11);
    _mm256_store_si256(((__m256i*)dst) + 12, r12);
    _mm256_store_si256(((__m256i*)dst) + 13, r13);
    _mm256_store_si256(((__m256i*)dst) + 14, r14);
    _mm256_store_si256(((__m256i*)dst) + 15, r15);
}

//---------------------------------------------------------------------
// 512-byte unaligned copy
//---------------------------------------------------------------------
static AUDIO_INLINE void memcpy_audio_512_unaligned(void *dst, const void *src) {
    __m256i r0  = _mm256_loadu_si256(((const __m256i*)src) + 0);
    __m256i r1  = _mm256_loadu_si256(((const __m256i*)src) + 1);
    __m256i r2  = _mm256_loadu_si256(((const __m256i*)src) + 2);
    __m256i r3  = _mm256_loadu_si256(((const __m256i*)src) + 3);
    __m256i r4  = _mm256_loadu_si256(((const __m256i*)src) + 4);
    __m256i r5  = _mm256_loadu_si256(((const __m256i*)src) + 5);
    __m256i r6  = _mm256_loadu_si256(((const __m256i*)src) + 6);
    __m256i r7  = _mm256_loadu_si256(((const __m256i*)src) + 7);
    __m256i r8  = _mm256_loadu_si256(((const __m256i*)src) + 8);
    __m256i r9  = _mm256_loadu_si256(((const __m256i*)src) + 9);
    __m256i r10 = _mm256_loadu_si256(((const __m256i*)src) + 10);
    __m256i r11 = _mm256_loadu_si256(((const __m256i*)src) + 11);
    __m256i r12 = _mm256_loadu_si256(((const __m256i*)src) + 12);
    __m256i r13 = _mm256_loadu_si256(((const __m256i*)src) + 13);
    __m256i r14 = _mm256_loadu_si256(((const __m256i*)src) + 14);
    __m256i r15 = _mm256_loadu_si256(((const __m256i*)src) + 15);

    _mm256_storeu_si256(((__m256i*)dst) + 0,  r0);
    _mm256_storeu_si256(((__m256i*)dst) + 1,  r1);
    _mm256_storeu_si256(((__m256i*)dst) + 2,  r2);
    _mm256_storeu_si256(((__m256i*)dst) + 3,  r3);
    _mm256_storeu_si256(((__m256i*)dst) + 4,  r4);
    _mm256_storeu_si256(((__m256i*)dst) + 5,  r5);
    _mm256_storeu_si256(((__m256i*)dst) + 6,  r6);
    _mm256_storeu_si256(((__m256i*)dst) + 7,  r7);
    _mm256_storeu_si256(((__m256i*)dst) + 8,  r8);
    _mm256_storeu_si256(((__m256i*)dst) + 9,  r9);
    _mm256_storeu_si256(((__m256i*)dst) + 10, r10);
    _mm256_storeu_si256(((__m256i*)dst) + 11, r11);
    _mm256_storeu_si256(((__m256i*)dst) + 12, r12);
    _mm256_storeu_si256(((__m256i*)dst) + 13, r13);
    _mm256_storeu_si256(((__m256i*)dst) + 14, r14);
    _mm256_storeu_si256(((__m256i*)dst) + 15, r15);
}

//---------------------------------------------------------------------
// Main audio memcpy - optimized for 8KB-64KB buffers
//---------------------------------------------------------------------
static inline void* memcpy_audio_fast(void *destination, const void *source, size_t size)
{
    unsigned char *dst = (unsigned char*)destination;
    const unsigned char *src = (const unsigned char*)source;

    if (size <= 256) {
        memcpy_tiny(dst, src, size);
        _mm256_zeroupper();
        return destination;
    }

    int is_aligned = (((uintptr_t)src | (uintptr_t)dst) & 31) == 0;

    _mm_prefetch((const char*)src, _MM_HINT_T0);
    _mm_prefetch((const char*)src + 64, _MM_HINT_T0);
    _mm_prefetch((const char*)src + 128, _MM_HINT_T0);
    _mm_prefetch((const char*)src + 192, _MM_HINT_T0);

    if (is_aligned) {
        for (; size >= 512; size -= 512) {
            _mm_prefetch((const char*)(src + 1024), _MM_HINT_T0);
            _mm_prefetch((const char*)(src + 1088), _MM_HINT_T0);

            memcpy_audio_512_aligned(dst, src);

            src += 512;
            dst += 512;
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

    } else {
        for (; size >= 512; size -= 512) {
            _mm_prefetch((const char*)(src + 1024), _MM_HINT_T0);
            _mm_prefetch((const char*)(src + 1088), _MM_HINT_T0);

            memcpy_audio_512_unaligned(dst, src);

            src += 512;
            dst += 512;
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
    }

    if (size > 0) {
        memcpy_tiny(dst, src, size);
    }

    _mm256_zeroupper();
    return destination;
}

#endif // __FAST_MEMCPY_AUDIO_H__
