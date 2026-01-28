#ifndef __MEMCPYFAST_AUDIO_H__
#define __MEMCPYFAST_AUDIO_H__

#include <stdio.h>
#include <stdlib.h>
#include <cstdint>
#include <cstddef>
#include <cstring>

//---------------------------------------------------------------------
// Architecture detection
// Use compiler-defined macros to detect actual SIMD capability
//---------------------------------------------------------------------
#if defined(__AVX2__)
    #define MEMCPY_AUDIO_AVX2 1
    #define MEMCPY_AUDIO_X86 1
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define MEMCPY_AUDIO_X86 1
    #define MEMCPY_AUDIO_X86_SCALAR 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define MEMCPY_AUDIO_ARM64 1
#endif

#ifdef MEMCPY_AUDIO_AVX2
#include "FastMemcpy_Audio.h"
#include <immintrin.h>

#ifdef __AVX512F__
#include "FastMemcpy_Audio_AVX512.h"
#endif
#endif // MEMCPY_AUDIO_AVX2

#ifdef MEMCPY_AUDIO_AVX2
//---------------------------------------------------------------------
// Runtime CPU feature detection (AVX2 x86 only)
//---------------------------------------------------------------------
static int g_avx512_checked = 0;
static int g_has_avx512 = 0;

static inline int detect_avx512(void) {
    if (!g_avx512_checked) {
        g_avx512_checked = 1;
#if defined(__GNUC__) && defined(__AVX512F__)
        __builtin_cpu_init();
        g_has_avx512 = __builtin_cpu_supports("avx512f") &&
                       __builtin_cpu_supports("avx512bw");
        if (g_has_avx512) {
            fprintf(stderr, "[memcpy_audio] AVX-512 detected and enabled\n");
        }
#else
        g_has_avx512 = 0;
#endif
    }
    return g_has_avx512;
}
#endif // MEMCPY_AUDIO_AVX2

#ifdef MEMCPY_AUDIO_AVX2
/**
 * Consistent-timing memcpy for audio buffers (128-4096 bytes)
 * Uses overlapping stores for tail handling to eliminate timing variance
 * x86 AVX2 implementation
 */
static inline void memcpy_audio_fixed(void* dst, const void* src, size_t size) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    while (size >= 128) {
        __m256i r0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 0));
        __m256i r1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 32));
        __m256i r2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 64));
        __m256i r3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 96));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 0), r0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 32), r1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 64), r2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 96), r3);
        s += 128;
        d += 128;
        size -= 128;
    }

    if (size >= 64) {
        __m256i a0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
        __m256i a1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 32));
        __m256i b0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + size - 64));
        __m256i b1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + size - 32));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d), a0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 32), a1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + size - 64), b0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + size - 32), b1);
    } else if (size >= 32) {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + size - 32));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d), a);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + size - 32), b);
    } else if (size >= 16) {
        __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
        __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + size - 16));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d), a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + size - 16), b);
    } else if (size >= 8) {
        uint64_t a;
        uint64_t b;
        std::memcpy(&a, s, 8);
        std::memcpy(&b, s + size - 8, 8);
        std::memcpy(d, &a, 8);
        std::memcpy(d + size - 8, &b, 8);
    } else if (size >= 4) {
        uint32_t a;
        uint32_t b;
        std::memcpy(&a, s, 4);
        std::memcpy(&b, s + size - 4, 4);
        std::memcpy(d, &a, 4);
        std::memcpy(d + size - 4, &b, 4);
    } else if (size > 0) {
        d[0] = s[0];
        if (size > 1) d[size - 1] = s[size - 1];
        if (size > 2) d[1] = s[1];
    }

    _mm256_zeroupper();
}

/**
 * Prefetch audio buffer for upcoming memcpy
 * Tuned for 180-1500 byte buffers on Zen 4
 */
static inline void prefetch_audio_buffer(const void* src, size_t size) {
    const char* p = static_cast<const char*>(src);

    _mm_prefetch(p, _MM_HINT_T0);

    if (size > 256) {
        _mm_prefetch(p + 64, _MM_HINT_T0);
    }
    if (size > 512) {
        _mm_prefetch(p + size - 64, _MM_HINT_T0);
    }
}

//---------------------------------------------------------------------
// Threshold for AVX-512 usage (32KB)
//---------------------------------------------------------------------
#define AVX512_THRESHOLD (32 * 1024)

//---------------------------------------------------------------------
// Main dispatcher - selects optimal path based on size and CPU (AVX2)
//---------------------------------------------------------------------
static inline void* memcpy_audio(void *dst, const void *src, size_t len) {
#ifndef NDEBUG
    const char *s = (const char *)src;
    const char *d = (const char *)dst;
    if (len > 0 && ((s < d && s + len > d) || (d < s && d + len > s))) {
        fprintf(stderr, "FATAL: memcpy_audio called with overlapping buffers!\n");
        fprintf(stderr, "  src=%p, dst=%p, len=%zu\n", src, dst, len);
        abort();
    }
#endif

#ifdef __AVX512F__
    if (len >= AVX512_THRESHOLD && detect_avx512()) {
        return memcpy_audio_avx512(dst, src, len);
    }
#endif

    return memcpy_audio_fast(dst, src, len);
}

#elif defined(MEMCPY_AUDIO_X86_SCALAR)
//---------------------------------------------------------------------
// x86 without AVX2 (Sandy Bridge, Ivy Bridge) - scalar fallback
//---------------------------------------------------------------------

/**
 * Prefetch audio buffer (no-op for scalar x86)
 */
static inline void prefetch_audio_buffer(const void* src, size_t size) {
    (void)src;
    (void)size;
}

/**
 * Audio memcpy - uses standard memcpy for x86 without AVX2
 */
static inline void* memcpy_audio(void *dst, const void *src, size_t len) {
#ifndef NDEBUG
    const char *s = (const char *)src;
    const char *d = (const char *)dst;
    if (len > 0 && ((s < d && s + len > d) || (d < s && d + len > s))) {
        fprintf(stderr, "FATAL: memcpy_audio called with overlapping buffers!\n");
        fprintf(stderr, "  src=%p, dst=%p, len=%zu\n", src, dst, len);
        abort();
    }
#endif
    return std::memcpy(dst, src, len);
}

/**
 * Fixed-timing memcpy - uses standard memcpy for x86 without AVX2
 */
static inline void memcpy_audio_fixed(void* dst, const void* src, size_t size) {
    std::memcpy(dst, src, size);
}

#else // !MEMCPY_AUDIO_AVX2 && !MEMCPY_AUDIO_X86_SCALAR (ARM64 and other platforms)

//---------------------------------------------------------------------
// ARM64 / Fallback implementation
// Uses standard memcpy - still fast on ARM64 due to NEON auto-vectorization
//---------------------------------------------------------------------

/**
 * Prefetch audio buffer (no-op on non-x86, compiler may auto-prefetch)
 */
static inline void prefetch_audio_buffer(const void* src, size_t size) {
    (void)src;
    (void)size;
    // ARM64: __builtin_prefetch could be added here if needed
}

/**
 * Audio memcpy - uses standard memcpy on ARM64
 * GCC/Clang will auto-vectorize with NEON on ARM64
 */
static inline void* memcpy_audio(void *dst, const void *src, size_t len) {
#ifndef NDEBUG
    const char *s = (const char *)src;
    const char *d = (const char *)dst;
    if (len > 0 && ((s < d && s + len > d) || (d < s && d + len > s))) {
        fprintf(stderr, "FATAL: memcpy_audio called with overlapping buffers!\n");
        fprintf(stderr, "  src=%p, dst=%p, len=%zu\n", src, dst, len);
        abort();
    }
#endif
    return std::memcpy(dst, src, len);
}

/**
 * Fixed-timing memcpy - uses standard memcpy on ARM64
 */
static inline void memcpy_audio_fixed(void* dst, const void* src, size_t size) {
    std::memcpy(dst, src, size);
}

#endif // MEMCPY_AUDIO_AVX2 / MEMCPY_AUDIO_X86_SCALAR / fallback

#endif // __MEMCPYFAST_AUDIO_H__
