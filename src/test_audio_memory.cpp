#include "AudioMemoryTest.h"
#include "memcpyfast_audio.h"
#include "DirettaRingBuffer.h"

// Forward declarations
bool test_memcpy_audio_fixed_correctness();
bool test_memcpy_audio_fixed_timing_variance();
bool test_staging_buffer_alignment();
bool test_24bit_packing_correctness();
bool test_24bit_packing_shifted_correctness();
bool test_24bit_packing_single_sample();
bool test_16to32_correctness();
bool test_16to32_single_sample();
bool test_16to24_correctness();
bool test_dsd_passthrough_correctness();
bool test_dsd_bit_reverse_correctness();
bool test_dsd_byte_swap_correctness();
bool test_dsd_bit_reverse_swap_correctness();
bool test_dsd_small_input();
bool test_ring_buffer_wraparound();
bool test_ring_buffer_power_of_2();
bool test_ring_buffer_full();
bool test_ring_buffer_empty_pop();
bool test_push24bit_pop_integration();
bool test_pushDSD_optimized_integration();

int main() {
    std::cout << "=== DirettaRingBuffer Unit Tests ===" << std::endl;
    std::cout << std::endl;

    int passed = 0;
    int failed = 0;

    // Group 1: Memory infrastructure
    std::cout << "--- Memory Infrastructure ---" << std::endl;
    RUN_TEST(test_memcpy_audio_fixed_correctness);
    RUN_TEST(test_memcpy_audio_fixed_timing_variance);
    RUN_TEST(test_staging_buffer_alignment);

    // Group 2: PCM format conversions
    std::cout << std::endl << "--- PCM Format Conversions ---" << std::endl;
    RUN_TEST(test_24bit_packing_correctness);
    RUN_TEST(test_24bit_packing_shifted_correctness);
    RUN_TEST(test_24bit_packing_single_sample);
    RUN_TEST(test_16to32_correctness);
    RUN_TEST(test_16to32_single_sample);
    RUN_TEST(test_16to24_correctness);

    // Group 3: DSD conversions (4 modes)
    std::cout << std::endl << "--- DSD Conversions ---" << std::endl;
    RUN_TEST(test_dsd_passthrough_correctness);
    RUN_TEST(test_dsd_bit_reverse_correctness);
    RUN_TEST(test_dsd_byte_swap_correctness);
    RUN_TEST(test_dsd_bit_reverse_swap_correctness);
    RUN_TEST(test_dsd_small_input);

    // Group 4: Ring buffer mechanics
    std::cout << std::endl << "--- Ring Buffer ---" << std::endl;
    RUN_TEST(test_ring_buffer_wraparound);
    RUN_TEST(test_ring_buffer_power_of_2);
    RUN_TEST(test_ring_buffer_full);
    RUN_TEST(test_ring_buffer_empty_pop);

    // Group 5: Integration (push → pop)
    std::cout << std::endl << "--- Integration ---" << std::endl;
    RUN_TEST(test_push24bit_pop_integration);
    RUN_TEST(test_pushDSD_optimized_integration);

    std::cout << std::endl;
    std::cout << "=== Results: " << passed << " passed, " << failed << " failed ===" << std::endl;

    return failed > 0 ? 1 : 0;
}

//=============================================================================
// Group 1: Memory Infrastructure
//=============================================================================

bool test_memcpy_audio_fixed_correctness() {
    std::vector<size_t> test_sizes = {128, 180, 256, 512, 768, 1024, 1500, 2048, 4096};

    for (size_t size : test_sizes) {
        alignas(64) uint8_t src[8192];
        alignas(64) uint8_t dst[8192];
        alignas(64) uint8_t expected[8192];

        for (size_t i = 0; i < size; i++) {
            src[i] = static_cast<uint8_t>(i & 0xFF);
        }
        std::memset(dst, 0xAA, size);
        std::memcpy(expected, src, size);

        memcpy_audio_fixed(dst, src, size);

        TEST_ASSERT(std::memcmp(dst, expected, size) == 0,
            "memcpy_audio_fixed failed at size " << size);
    }

    return true;
}

bool test_memcpy_audio_fixed_timing_variance() {
    constexpr int ITERATIONS = 5000;
    constexpr double TARGET_US = 50.0;
    constexpr int MAX_INNER_LOOPS = 1 << 20;
    std::vector<size_t> test_sizes = {180, 768, 1536};

    for (size_t size : test_sizes) {
        alignas(64) uint8_t src[4096];
        alignas(64) uint8_t dst[4096];

        std::memset(src, 0x5A, sizeof(src));
        std::memset(dst, 0x00, sizeof(dst));

        auto measure = [&](int loops) {
            auto start = std::chrono::steady_clock::now();
            for (int j = 0; j < loops; j++) {
                memcpy_audio_fixed(dst, src, size);
            }
            auto end = std::chrono::steady_clock::now();
            return std::chrono::duration<double, std::micro>(end - start).count();
        };

        int innerLoops = 1;
        while (innerLoops < MAX_INNER_LOOPS) {
            double elapsed = measure(innerLoops);
            if (elapsed >= TARGET_US) break;
            innerLoops <<= 1;
        }

        // Warmup
        for (int i = 0; i < 20; i++) {
            measure(innerLoops);
        }

        TimingStats stats;
        for (int i = 0; i < ITERATIONS; i++) {
            double us = measure(innerLoops);
            stats.record(us / innerLoops);
        }

        double cv = stats.cv();
        TEST_ASSERT(cv < 0.5,
            "Timing variance too high for size " << size <<
            " (CV=" << cv << ", mean=" << stats.mean() << "us)");

        std::cout << "[size=" << size << " mean=" << stats.mean()
                  << "us cv=" << cv << "] ";
    }

    return true;
}

bool test_staging_buffer_alignment() {
    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    const uint8_t* staging24 = ring.getStaging24BitPack();
    const uint8_t* staging16to32 = ring.getStaging16To32();
    const uint8_t* stagingDSD = ring.getStagingDSD();

    TEST_ASSERT((reinterpret_cast<uintptr_t>(staging24) % 64) == 0,
        "staging24BitPack not 64-byte aligned");
    TEST_ASSERT((reinterpret_cast<uintptr_t>(staging16to32) % 64) == 0,
        "staging16To32 not 64-byte aligned");
    TEST_ASSERT((reinterpret_cast<uintptr_t>(stagingDSD) % 64) == 0,
        "stagingDSD not 64-byte aligned");

    TEST_ASSERT(staging16to32 >= staging24 + 65536 || staging24 >= staging16to32 + 65536,
        "staging buffers overlap");
    TEST_ASSERT(stagingDSD >= staging24 + 65536 || staging24 >= stagingDSD + 65536,
        "staging buffers overlap");

    return true;
}

//=============================================================================
// Group 2: PCM Format Conversions
//=============================================================================

bool test_24bit_packing_correctness() {
    constexpr size_t NUM_SAMPLES = 64;
    alignas(64) uint8_t input[NUM_SAMPLES * 4];
    alignas(64) uint8_t output[NUM_SAMPLES * 3];
    alignas(64) uint8_t expected[NUM_SAMPLES * 3];

    // S24_P32 LSB-aligned: [data0, data1, data2, 0x00]
    for (size_t i = 0; i < NUM_SAMPLES; i++) {
        uint32_t sample = 0x112233 + static_cast<uint32_t>(i * 0x010101);
        input[i * 4 + 0] = sample & 0xFF;
        input[i * 4 + 1] = (sample >> 8) & 0xFF;
        input[i * 4 + 2] = (sample >> 16) & 0xFF;
        input[i * 4 + 3] = 0x00;

        expected[i * 3 + 0] = sample & 0xFF;
        expected[i * 3 + 1] = (sample >> 8) & 0xFF;
        expected[i * 3 + 2] = (sample >> 16) & 0xFF;
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    size_t converted = ring.convert24BitPacked_AVX2(output, input, NUM_SAMPLES);

    TEST_ASSERT_EQ(converted, NUM_SAMPLES * 3, "Wrong output size");
    TEST_ASSERT(std::memcmp(output, expected, NUM_SAMPLES * 3) == 0,
        "24-bit packing (LSB) produced incorrect output");

    return true;
}

bool test_24bit_packing_shifted_correctness() {
    constexpr size_t NUM_SAMPLES = 64;
    alignas(64) uint8_t input[NUM_SAMPLES * 4];
    alignas(64) uint8_t output[NUM_SAMPLES * 3];
    alignas(64) uint8_t expected[NUM_SAMPLES * 3];

    // S24_P32 MSB-aligned: [0x00, data0, data1, data2]
    for (size_t i = 0; i < NUM_SAMPLES; i++) {
        uint32_t sample = 0x112233 + static_cast<uint32_t>(i * 0x010101);
        input[i * 4 + 0] = 0x00;  // padding in LSB
        input[i * 4 + 1] = sample & 0xFF;
        input[i * 4 + 2] = (sample >> 8) & 0xFF;
        input[i * 4 + 3] = (sample >> 16) & 0xFF;

        expected[i * 3 + 0] = sample & 0xFF;
        expected[i * 3 + 1] = (sample >> 8) & 0xFF;
        expected[i * 3 + 2] = (sample >> 16) & 0xFF;
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    size_t converted = ring.convert24BitPackedShifted_AVX2(output, input, NUM_SAMPLES);

    TEST_ASSERT_EQ(converted, NUM_SAMPLES * 3, "Wrong output size");
    TEST_ASSERT(std::memcmp(output, expected, NUM_SAMPLES * 3) == 0,
        "24-bit packing (MSB/shifted) produced incorrect output");

    return true;
}

bool test_24bit_packing_single_sample() {
    alignas(64) uint8_t input[4] = {0xAB, 0xCD, 0xEF, 0x00};
    alignas(64) uint8_t output[3] = {};

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    size_t converted = ring.convert24BitPacked_AVX2(output, input, 1);

    TEST_ASSERT_EQ(converted, static_cast<size_t>(3), "Wrong output size for single sample");
    TEST_ASSERT(output[0] == 0xAB && output[1] == 0xCD && output[2] == 0xEF,
        "Single sample 24-bit pack incorrect");

    return true;
}

bool test_16to32_correctness() {
    constexpr size_t NUM_SAMPLES = 64;
    alignas(64) uint8_t input[NUM_SAMPLES * 2];
    alignas(64) uint8_t output[NUM_SAMPLES * 4];
    alignas(64) uint8_t expected[NUM_SAMPLES * 4];

    for (size_t i = 0; i < NUM_SAMPLES; i++) {
        int16_t sample = static_cast<int16_t>(i * 256 - 32768);
        input[i * 2 + 0] = sample & 0xFF;
        input[i * 2 + 1] = (sample >> 8) & 0xFF;

        // 16-bit placed in upper 16 bits of 32-bit word
        expected[i * 4 + 0] = 0x00;
        expected[i * 4 + 1] = 0x00;
        expected[i * 4 + 2] = input[i * 2 + 0];
        expected[i * 4 + 3] = input[i * 2 + 1];
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    size_t converted = ring.convert16To32_AVX2(output, input, NUM_SAMPLES);

    TEST_ASSERT_EQ(converted, NUM_SAMPLES * 4, "Wrong output size");
    TEST_ASSERT(std::memcmp(output, expected, NUM_SAMPLES * 4) == 0,
        "16->32 conversion produced incorrect output");

    return true;
}

bool test_16to32_single_sample() {
    alignas(64) uint8_t input[2] = {0xAB, 0xCD};
    alignas(64) uint8_t output[4] = {};

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    size_t converted = ring.convert16To32_AVX2(output, input, 1);

    TEST_ASSERT_EQ(converted, static_cast<size_t>(4), "Wrong output size for single sample");
    TEST_ASSERT(output[0] == 0x00 && output[1] == 0x00 &&
                output[2] == 0xAB && output[3] == 0xCD,
        "Single sample 16->32 incorrect");

    return true;
}

bool test_16to24_correctness() {
    constexpr size_t NUM_SAMPLES = 64;
    alignas(64) uint8_t input[NUM_SAMPLES * 2];
    alignas(64) uint8_t output[NUM_SAMPLES * 3];
    alignas(64) uint8_t expected[NUM_SAMPLES * 3];

    for (size_t i = 0; i < NUM_SAMPLES; i++) {
        input[i * 2 + 0] = static_cast<uint8_t>(i);       // 16-bit LSB
        input[i * 2 + 1] = static_cast<uint8_t>(i + 0x80); // 16-bit MSB

        // Packed 24-bit: [0x00, 16-bit LSB, 16-bit MSB]
        expected[i * 3 + 0] = 0x00;
        expected[i * 3 + 1] = input[i * 2 + 0];
        expected[i * 3 + 2] = input[i * 2 + 1];
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    size_t converted = ring.convert16To24(output, input, NUM_SAMPLES);

    TEST_ASSERT_EQ(converted, NUM_SAMPLES * 3, "Wrong output size");
    TEST_ASSERT(std::memcmp(output, expected, NUM_SAMPLES * 3) == 0,
        "16->24 conversion produced incorrect output");

    return true;
}

//=============================================================================
// Group 3: DSD Conversions (4 modes)
//=============================================================================

// Helper: bit reverse a single byte using the same LUT as DirettaRingBuffer
static uint8_t bitReverse(uint8_t b) {
    static constexpr uint8_t lut[256] = {
        0x00,0x80,0x40,0xC0,0x20,0xA0,0x60,0xE0,0x10,0x90,0x50,0xD0,0x30,0xB0,0x70,0xF0,
        0x08,0x88,0x48,0xC8,0x28,0xA8,0x68,0xE8,0x18,0x98,0x58,0xD8,0x38,0xB8,0x78,0xF8,
        0x04,0x84,0x44,0xC4,0x24,0xA4,0x64,0xE4,0x14,0x94,0x54,0xD4,0x34,0xB4,0x74,0xF4,
        0x0C,0x8C,0x4C,0xCC,0x2C,0xAC,0x6C,0xEC,0x1C,0x9C,0x5C,0xDC,0x3C,0xBC,0x7C,0xFC,
        0x02,0x82,0x42,0xC2,0x22,0xA2,0x62,0xE2,0x12,0x92,0x52,0xD2,0x32,0xB2,0x72,0xF2,
        0x0A,0x8A,0x4A,0xCA,0x2A,0xAA,0x6A,0xEA,0x1A,0x9A,0x5A,0xDA,0x3A,0xBA,0x7A,0xFA,
        0x06,0x86,0x46,0xC6,0x26,0xA6,0x66,0xE6,0x16,0x96,0x56,0xD6,0x36,0xB6,0x76,0xF6,
        0x0E,0x8E,0x4E,0xCE,0x2E,0xAE,0x6E,0xEE,0x1E,0x9E,0x5E,0xDE,0x3E,0xBE,0x7E,0xFE,
        0x01,0x81,0x41,0xC1,0x21,0xA1,0x61,0xE1,0x11,0x91,0x51,0xD1,0x31,0xB1,0x71,0xF1,
        0x09,0x89,0x49,0xC9,0x29,0xA9,0x69,0xE9,0x19,0x99,0x59,0xD9,0x39,0xB9,0x79,0xF9,
        0x05,0x85,0x45,0xC5,0x25,0xA5,0x65,0xE5,0x15,0x95,0x55,0xD5,0x35,0xB5,0x75,0xF5,
        0x0D,0x8D,0x4D,0xCD,0x2D,0xAD,0x6D,0xED,0x1D,0x9D,0x5D,0xDD,0x3D,0xBD,0x7D,0xFD,
        0x03,0x83,0x43,0xC3,0x23,0xA3,0x63,0xE3,0x13,0x93,0x53,0xD3,0x33,0xB3,0x73,0xF3,
        0x0B,0x8B,0x4B,0xCB,0x2B,0xAB,0x6B,0xEB,0x1B,0x9B,0x5B,0xDB,0x3B,0xBB,0x7B,0xFB,
        0x07,0x87,0x47,0xC7,0x27,0xA7,0x67,0xE7,0x17,0x97,0x57,0xD7,0x37,0xB7,0x77,0xF7,
        0x0F,0x8F,0x4F,0xCF,0x2F,0xAF,0x6F,0xEF,0x1F,0x9F,0x5F,0xDF,0x3F,0xBF,0x7F,0xFF
    };
    return lut[b];
}

bool test_dsd_passthrough_correctness() {
    // Stereo DSD: L and R channels, interleaved as 4-byte groups
    constexpr size_t BYTES_PER_CHANNEL = 64;
    constexpr size_t TOTAL_INPUT = BYTES_PER_CHANNEL * 2;

    alignas(64) uint8_t input[TOTAL_INPUT];
    alignas(64) uint8_t output[TOTAL_INPUT];
    alignas(64) uint8_t expected[TOTAL_INPUT];

    // Fill L channel with incrementing pattern, R with decrementing
    for (size_t i = 0; i < BYTES_PER_CHANNEL; i++) {
        input[i] = static_cast<uint8_t>(i);                              // L
        input[BYTES_PER_CHANNEL + i] = static_cast<uint8_t>(0xFF - i);   // R
    }

    // Expected: interleaved by 4-byte groups [L0-3, R0-3, L4-7, R4-7, ...]
    for (size_t i = 0; i < BYTES_PER_CHANNEL / 4; i++) {
        for (int b = 0; b < 4; b++) {
            expected[i * 8 + b]     = input[i * 4 + b];                      // L
            expected[i * 8 + 4 + b] = input[BYTES_PER_CHANNEL + i * 4 + b];  // R
        }
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x69);

    size_t converted = ring.convertDSD_Passthrough(output, input, TOTAL_INPUT, 2);

    TEST_ASSERT_EQ(converted, TOTAL_INPUT, "Wrong DSD passthrough output size");
    TEST_ASSERT(std::memcmp(output, expected, TOTAL_INPUT) == 0,
        "DSD passthrough interleaving incorrect");

    return true;
}

bool test_dsd_bit_reverse_correctness() {
    constexpr size_t BYTES_PER_CHANNEL = 64;
    constexpr size_t TOTAL_INPUT = BYTES_PER_CHANNEL * 2;

    alignas(64) uint8_t input[TOTAL_INPUT];
    alignas(64) uint8_t output[TOTAL_INPUT];
    alignas(64) uint8_t expected[TOTAL_INPUT];

    // Known bit-reverse pairs: 0x01→0x80, 0x80→0x01, 0xFF→0xFF, 0x00→0x00
    for (size_t i = 0; i < BYTES_PER_CHANNEL; i++) {
        input[i] = static_cast<uint8_t>(i);                              // L
        input[BYTES_PER_CHANNEL + i] = static_cast<uint8_t>(0xFF - i);   // R
    }

    // Expected: bit-reverse each byte, then interleave by 4-byte groups
    for (size_t i = 0; i < BYTES_PER_CHANNEL / 4; i++) {
        for (int b = 0; b < 4; b++) {
            expected[i * 8 + b]     = bitReverse(input[i * 4 + b]);
            expected[i * 8 + 4 + b] = bitReverse(input[BYTES_PER_CHANNEL + i * 4 + b]);
        }
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    size_t converted = ring.convertDSD_BitReverse(output, input, TOTAL_INPUT, 2);

    TEST_ASSERT_EQ(converted, TOTAL_INPUT, "Wrong DSD bit-reverse output size");
    TEST_ASSERT(std::memcmp(output, expected, TOTAL_INPUT) == 0,
        "DSD bit-reverse conversion incorrect");

    return true;
}

bool test_dsd_byte_swap_correctness() {
    constexpr size_t BYTES_PER_CHANNEL = 64;
    constexpr size_t TOTAL_INPUT = BYTES_PER_CHANNEL * 2;

    alignas(64) uint8_t input[TOTAL_INPUT];
    alignas(64) uint8_t output[TOTAL_INPUT];
    alignas(64) uint8_t expected[TOTAL_INPUT];

    for (size_t i = 0; i < BYTES_PER_CHANNEL; i++) {
        input[i] = static_cast<uint8_t>(i);
        input[BYTES_PER_CHANNEL + i] = static_cast<uint8_t>(0xFF - i);
    }

    // Expected: interleave by 4-byte groups, then byte-swap each 32-bit word
    // Input L group: [A,B,C,D], R group: [E,F,G,H]
    // After interleave: [A,B,C,D, E,F,G,H]
    // After byte swap:  [D,C,B,A, H,G,F,E]
    for (size_t i = 0; i < BYTES_PER_CHANNEL / 4; i++) {
        // L group byte-swapped
        expected[i * 8 + 0] = input[i * 4 + 3];
        expected[i * 8 + 1] = input[i * 4 + 2];
        expected[i * 8 + 2] = input[i * 4 + 1];
        expected[i * 8 + 3] = input[i * 4 + 0];
        // R group byte-swapped
        expected[i * 8 + 4] = input[BYTES_PER_CHANNEL + i * 4 + 3];
        expected[i * 8 + 5] = input[BYTES_PER_CHANNEL + i * 4 + 2];
        expected[i * 8 + 6] = input[BYTES_PER_CHANNEL + i * 4 + 1];
        expected[i * 8 + 7] = input[BYTES_PER_CHANNEL + i * 4 + 0];
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    size_t converted = ring.convertDSD_ByteSwap(output, input, TOTAL_INPUT, 2);

    TEST_ASSERT_EQ(converted, TOTAL_INPUT, "Wrong DSD byte-swap output size");
    TEST_ASSERT(std::memcmp(output, expected, TOTAL_INPUT) == 0,
        "DSD byte-swap conversion incorrect");

    return true;
}

bool test_dsd_bit_reverse_swap_correctness() {
    constexpr size_t BYTES_PER_CHANNEL = 64;
    constexpr size_t TOTAL_INPUT = BYTES_PER_CHANNEL * 2;

    alignas(64) uint8_t input[TOTAL_INPUT];
    alignas(64) uint8_t output[TOTAL_INPUT];
    alignas(64) uint8_t expected[TOTAL_INPUT];

    for (size_t i = 0; i < BYTES_PER_CHANNEL; i++) {
        input[i] = static_cast<uint8_t>(i);
        input[BYTES_PER_CHANNEL + i] = static_cast<uint8_t>(0xFF - i);
    }

    // Expected: bit-reverse each byte, interleave, then byte-swap each 32-bit word
    for (size_t i = 0; i < BYTES_PER_CHANNEL / 4; i++) {
        // L group: bit-reverse then byte-swap
        expected[i * 8 + 0] = bitReverse(input[i * 4 + 3]);
        expected[i * 8 + 1] = bitReverse(input[i * 4 + 2]);
        expected[i * 8 + 2] = bitReverse(input[i * 4 + 1]);
        expected[i * 8 + 3] = bitReverse(input[i * 4 + 0]);
        // R group: bit-reverse then byte-swap
        expected[i * 8 + 4] = bitReverse(input[BYTES_PER_CHANNEL + i * 4 + 3]);
        expected[i * 8 + 5] = bitReverse(input[BYTES_PER_CHANNEL + i * 4 + 2]);
        expected[i * 8 + 6] = bitReverse(input[BYTES_PER_CHANNEL + i * 4 + 1]);
        expected[i * 8 + 7] = bitReverse(input[BYTES_PER_CHANNEL + i * 4 + 0]);
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    size_t converted = ring.convertDSD_BitReverseSwap(output, input, TOTAL_INPUT, 2);

    TEST_ASSERT_EQ(converted, TOTAL_INPUT, "Wrong DSD bit-reverse+swap output size");
    TEST_ASSERT(std::memcmp(output, expected, TOTAL_INPUT) == 0,
        "DSD bit-reverse+swap conversion incorrect");

    return true;
}

bool test_dsd_small_input() {
    // 8 bytes per channel — exercises scalar tail only (below SIMD threshold)
    constexpr size_t BYTES_PER_CHANNEL = 8;
    constexpr size_t TOTAL_INPUT = BYTES_PER_CHANNEL * 2;

    alignas(64) uint8_t input[TOTAL_INPUT];
    alignas(64) uint8_t output[TOTAL_INPUT];
    alignas(64) uint8_t expected[TOTAL_INPUT];

    for (size_t i = 0; i < BYTES_PER_CHANNEL; i++) {
        input[i] = static_cast<uint8_t>(0x10 + i);
        input[BYTES_PER_CHANNEL + i] = static_cast<uint8_t>(0xA0 + i);
    }

    // Passthrough: interleave by 4-byte groups
    for (size_t i = 0; i < BYTES_PER_CHANNEL / 4; i++) {
        for (int b = 0; b < 4; b++) {
            expected[i * 8 + b]     = input[i * 4 + b];
            expected[i * 8 + 4 + b] = input[BYTES_PER_CHANNEL + i * 4 + b];
        }
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    size_t converted = ring.convertDSD_Passthrough(output, input, TOTAL_INPUT, 2);

    TEST_ASSERT_EQ(converted, TOTAL_INPUT, "Wrong small DSD output size");
    TEST_ASSERT(std::memcmp(output, expected, TOTAL_INPUT) == 0,
        "Small DSD passthrough incorrect (scalar path)");

    return true;
}

//=============================================================================
// Group 4: Ring Buffer Mechanics
//=============================================================================

bool test_ring_buffer_wraparound() {
    DirettaRingBuffer ring;
    ring.resize(1024, 0x00);

    // Fill most of the buffer
    std::vector<uint8_t> data(900, 0xAA);
    ring.push(data.data(), data.size());

    // Pop most of it (advance read pointer near end)
    std::vector<uint8_t> tmp(800);
    ring.pop(tmp.data(), tmp.size());

    // Pop remaining
    std::vector<uint8_t> leftover(100);
    ring.pop(leftover.data(), leftover.size());

    // Now write data that wraps around the end
    std::vector<uint8_t> wrapData(200);
    for (size_t i = 0; i < 200; i++) {
        wrapData[i] = static_cast<uint8_t>(i);
    }

    size_t written = ring.push(wrapData.data(), wrapData.size());
    TEST_ASSERT(written == 200, "Failed to write wraparound data");

    std::vector<uint8_t> readBack(200);
    size_t read = ring.pop(readBack.data(), readBack.size());
    TEST_ASSERT(read == 200, "Failed to read wraparound data");

    TEST_ASSERT(std::memcmp(wrapData.data(), readBack.data(), 200) == 0,
        "Wraparound data corrupted");

    return true;
}

bool test_ring_buffer_power_of_2() {
    DirettaRingBuffer ring;

    // 1000 should round up to 1024
    ring.resize(1000, 0x00);
    TEST_ASSERT_EQ(ring.size(), static_cast<size_t>(1024),
        "1000 should round to 1024");

    // 1024 stays 1024
    ring.resize(1024, 0x00);
    TEST_ASSERT_EQ(ring.size(), static_cast<size_t>(1024),
        "1024 should stay 1024");

    // 1025 should round up to 2048
    ring.resize(1025, 0x00);
    TEST_ASSERT_EQ(ring.size(), static_cast<size_t>(2048),
        "1025 should round to 2048");

    // Small value
    ring.resize(3, 0x00);
    TEST_ASSERT(ring.size() >= 4, "Minimum size should be at least 4");
    // Must be power of 2
    TEST_ASSERT((ring.size() & (ring.size() - 1)) == 0,
        "Size must be power of 2");

    return true;
}

bool test_ring_buffer_full() {
    DirettaRingBuffer ring;
    ring.resize(64, 0x00);  // Small buffer: 64 bytes

    // Try to write more than capacity (64 - 1 usable = 63)
    std::vector<uint8_t> data(100, 0xBB);
    size_t written = ring.push(data.data(), data.size());

    // Should write at most 63 bytes (capacity - 1 for SPSC sentinel)
    TEST_ASSERT(written <= 63, "Wrote more than buffer capacity");
    TEST_ASSERT(written > 0, "Should write at least some data");

    // Free space should now be very small
    TEST_ASSERT(ring.getFreeSpace() < 5, "Free space should be near zero");

    return true;
}

bool test_ring_buffer_empty_pop() {
    DirettaRingBuffer ring;
    ring.resize(1024, 0x00);

    // Pop from empty buffer
    uint8_t buf[64];
    size_t read = ring.pop(buf, sizeof(buf));
    TEST_ASSERT_EQ(read, static_cast<size_t>(0), "Pop from empty buffer should return 0");

    // Available should be 0
    TEST_ASSERT_EQ(ring.getAvailable(), static_cast<size_t>(0),
        "Empty buffer should have 0 available");

    return true;
}

//=============================================================================
// Group 5: Integration (push → pop)
//=============================================================================

bool test_push24bit_pop_integration() {
    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    // Push 192 samples of S24_P32 (768 bytes) → should produce 576 bytes packed
    constexpr size_t NUM_SAMPLES = 192;
    alignas(64) uint8_t input[NUM_SAMPLES * 4];
    for (size_t i = 0; i < NUM_SAMPLES * 4; i++) {
        input[i] = static_cast<uint8_t>(i & 0xFF);
    }

    size_t written = ring.push24BitPacked(input, NUM_SAMPLES * 4);
    TEST_ASSERT(written > 0, "24-bit push failed");
    TEST_ASSERT_EQ(written, NUM_SAMPLES * 4, "24-bit push should consume all input");

    // Pop the packed data and verify
    size_t available = ring.getAvailable();
    TEST_ASSERT_EQ(available, NUM_SAMPLES * 3, "Expected 576 bytes in ring");

    std::vector<uint8_t> popped(available);
    size_t read = ring.pop(popped.data(), available);
    TEST_ASSERT_EQ(read, available, "Should read all available data");

    // Verify first few samples manually
    // Input sample 0: [0x00, 0x01, 0x02, 0x03] → packed: [0x00, 0x01, 0x02]
    TEST_ASSERT(popped[0] == 0x00 && popped[1] == 0x01 && popped[2] == 0x02,
        "First packed sample incorrect");

    return true;
}

bool test_pushDSD_optimized_integration() {
    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x69);

    // Push stereo DSD data using the optimized API with Passthrough mode
    constexpr size_t BYTES_PER_CHANNEL = 128;
    constexpr size_t TOTAL_INPUT = BYTES_PER_CHANNEL * 2;

    alignas(64) uint8_t input[TOTAL_INPUT];
    for (size_t i = 0; i < BYTES_PER_CHANNEL; i++) {
        input[i] = static_cast<uint8_t>(i & 0xFF);
        input[BYTES_PER_CHANNEL + i] = static_cast<uint8_t>((i + 0x80) & 0xFF);
    }

    size_t written = ring.pushDSDPlanarOptimized(
        input, TOTAL_INPUT, 2,
        DirettaRingBuffer::DSDConversionMode::Passthrough);

    TEST_ASSERT(written > 0, "DSD optimized push failed");
    TEST_ASSERT_EQ(written, TOTAL_INPUT, "DSD push should consume all input");

    // Pop and verify interleaving
    size_t available = ring.getAvailable();
    TEST_ASSERT_EQ(available, TOTAL_INPUT, "DSD output size should equal input");

    std::vector<uint8_t> popped(available);
    ring.pop(popped.data(), available);

    // Verify first 8 bytes: [L0,L1,L2,L3, R0,R1,R2,R3]
    TEST_ASSERT(popped[0] == 0x00 && popped[1] == 0x01 &&
                popped[2] == 0x02 && popped[3] == 0x03,
        "DSD L channel interleave incorrect");
    TEST_ASSERT(popped[4] == 0x80 && popped[5] == 0x81 &&
                popped[6] == 0x82 && popped[7] == 0x83,
        "DSD R channel interleave incorrect");

    return true;
}
