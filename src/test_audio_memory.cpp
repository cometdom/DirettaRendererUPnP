#include "AudioMemoryTest.h"
#include "memcpyfast_audio.h"
#include "DirettaRingBuffer.h"

bool test_memcpy_audio_fixed_correctness();
bool test_memcpy_audio_fixed_timing_variance();
bool test_staging_buffer_alignment();
bool test_24bit_packing_correctness();
bool test_24bit_packing_timing();
bool test_16to32_correctness();
bool test_dsd_stereo_correctness();
bool test_ring_buffer_wraparound();
bool test_full_integration();

int main() {
    std::cout << "=== Audio Memory Optimization Tests ===" << std::endl;
    std::cout << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_memcpy_audio_fixed_correctness);
    RUN_TEST(test_memcpy_audio_fixed_timing_variance);
    RUN_TEST(test_staging_buffer_alignment);
    RUN_TEST(test_24bit_packing_correctness);
    RUN_TEST(test_24bit_packing_timing);
    RUN_TEST(test_16to32_correctness);
    RUN_TEST(test_dsd_stereo_correctness);
    RUN_TEST(test_ring_buffer_wraparound);
    RUN_TEST(test_full_integration);

    std::cout << std::endl;
    std::cout << "=== Results: " << passed << " passed, " << failed << " failed ===" << std::endl;

    return failed > 0 ? 1 : 0;
}

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
        double elapsed = 0.0;
        while (innerLoops < MAX_INNER_LOOPS) {
            elapsed = measure(innerLoops);
            if (elapsed >= TARGET_US) break;
            innerLoops <<= 1;
        }

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
                  << "us cv=" << cv << " loops=" << innerLoops << "] ";
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

bool test_24bit_packing_correctness() {
    constexpr size_t NUM_SAMPLES = 64;
    alignas(64) uint8_t input[NUM_SAMPLES * 4];
    alignas(64) uint8_t output[NUM_SAMPLES * 3];
    alignas(64) uint8_t expected[NUM_SAMPLES * 3];

    for (size_t i = 0; i < NUM_SAMPLES; i++) {
        uint32_t sample = 0x112233 + (i * 0x010101);
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
        "24-bit packing produced incorrect output");

    return true;
}

bool test_24bit_packing_timing() {
    constexpr int ITERATIONS = 5000;
    constexpr double TARGET_US = 50.0;
    constexpr int MAX_INNER_LOOPS = 1 << 20;
    constexpr size_t NUM_SAMPLES = 192;

    alignas(64) uint8_t input[NUM_SAMPLES * 4];
    alignas(64) uint8_t output[NUM_SAMPLES * 3];

    for (size_t i = 0; i < NUM_SAMPLES * 4; i++) {
        input[i] = static_cast<uint8_t>(i);
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    auto measure = [&](int loops) {
        auto start = std::chrono::steady_clock::now();
        for (int j = 0; j < loops; j++) {
            ring.convert24BitPacked_AVX2(output, input, NUM_SAMPLES);
        }
        auto end = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::micro>(end - start).count();
    };

    int innerLoops = 1;
    double elapsed = 0.0;
    while (innerLoops < MAX_INNER_LOOPS) {
        elapsed = measure(innerLoops);
        if (elapsed >= TARGET_US) break;
        innerLoops <<= 1;
    }

    for (int i = 0; i < 20; i++) {
        measure(innerLoops);
    }

    TimingStats stats;
    for (int i = 0; i < ITERATIONS; i++) {
        double us = measure(innerLoops);
        stats.record(us / innerLoops);
    }

    std::cout << "[24bit mean=" << stats.mean() << "us cv=" << stats.cv()
              << " loops=" << innerLoops << "] ";
    TEST_ASSERT(stats.cv() < 0.5, "24-bit packing timing variance too high");

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

bool test_dsd_stereo_correctness() {
    constexpr size_t BYTES_PER_CHANNEL = 64;
    constexpr size_t TOTAL_INPUT = BYTES_PER_CHANNEL * 2;
    constexpr size_t TOTAL_OUTPUT = BYTES_PER_CHANNEL * 2;

    alignas(64) uint8_t input[TOTAL_INPUT];
    alignas(64) uint8_t output[TOTAL_OUTPUT];
    alignas(64) uint8_t expected[TOTAL_OUTPUT];

    for (size_t i = 0; i < BYTES_PER_CHANNEL; i++) {
        input[i] = 0xAA;
        input[BYTES_PER_CHANNEL + i] = 0x55;
    }

    for (size_t i = 0; i < BYTES_PER_CHANNEL / 4; i++) {
        expected[i * 8 + 0] = 0xAA;
        expected[i * 8 + 1] = 0xAA;
        expected[i * 8 + 2] = 0xAA;
        expected[i * 8 + 3] = 0xAA;
        expected[i * 8 + 4] = 0x55;
        expected[i * 8 + 5] = 0x55;
        expected[i * 8 + 6] = 0x55;
        expected[i * 8 + 7] = 0x55;
    }

    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x69);

    size_t converted = ring.convertDSDPlanar_AVX2(
        output, input, TOTAL_INPUT, 2,
        nullptr,
        false
    );

    TEST_ASSERT_EQ(converted, TOTAL_OUTPUT, "Wrong DSD output size");
    TEST_ASSERT(std::memcmp(output, expected, TOTAL_OUTPUT) == 0,
        "DSD stereo interleaving produced incorrect output");

    return true;
}

bool test_ring_buffer_wraparound() {
    DirettaRingBuffer ring;
    ring.resize(1024, 0x00);

    std::vector<uint8_t> data(900, 0xAA);
    ring.push(data.data(), data.size());

    std::vector<uint8_t> tmp(800);
    ring.pop(tmp.data(), tmp.size());

    std::vector<uint8_t> leftover(100);
    ring.pop(leftover.data(), leftover.size());

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

bool test_full_integration() {
    DirettaRingBuffer ring;
    ring.resize(1024 * 1024, 0x00);

    {
        alignas(64) uint8_t input[768];
        for (size_t i = 0; i < 768; i++) input[i] = static_cast<uint8_t>(i);

        size_t written = ring.push24BitPacked(input, 768);
        TEST_ASSERT(written > 0, "24-bit push failed");
        TEST_ASSERT(written == 192 * 4, "24-bit push wrong size");
    }

    ring.clear();
    {
        alignas(64) uint8_t input[384];
        for (size_t i = 0; i < 384; i++) input[i] = static_cast<uint8_t>(i);

        size_t written = ring.push16To32(input, 384);
        TEST_ASSERT(written > 0, "16->32 push failed");
        TEST_ASSERT(written == 192 * 2, "16->32 push wrong size");
    }

    ring.clear();
    {
        alignas(64) uint8_t input[128];
        for (size_t i = 0; i < 128; i++) input[i] = static_cast<uint8_t>(i);

        size_t written = ring.pushDSDPlanar(input, 128, 2, nullptr, false);
        TEST_ASSERT(written > 0, "DSD push failed");
        TEST_ASSERT(written == 128, "DSD push wrong size");
    }

    return true;
}
