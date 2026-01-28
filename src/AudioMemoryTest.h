#ifndef AUDIO_MEMORY_TEST_H
#define AUDIO_MEMORY_TEST_H

#include <iostream>
#include <cstring>
#include <chrono>
#include <vector>
#include <cmath>
#include <cstdint>

#define TEST_ASSERT(condition, msg) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAIL: " << msg << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while (0)

#define TEST_ASSERT_EQ(a, b, msg) \
    TEST_ASSERT((a) == (b), msg << " (expected " << (b) << ", got " << (a) << ")")

#define RUN_TEST(test_func) \
    do { \
        std::cout << "Running " << #test_func << "... "; \
        if (test_func()) { \
            std::cout << "PASS" << std::endl; \
            passed++; \
        } else { \
            std::cout << "FAIL" << std::endl; \
            failed++; \
        } \
    } while (0)

struct TimingStats {
    double min_us = 1e9;
    double max_us = 0;
    double sum_us = 0;
    double sum_sq = 0;
    int count = 0;

    void record(double us) {
        if (us < min_us) min_us = us;
        if (us > max_us) max_us = us;
        sum_us += us;
        sum_sq += us * us;
        count++;
    }

    double mean() const { return count > 0 ? sum_us / count : 0; }
    double variance() const {
        if (count < 2) return 0;
        double m = mean();
        return (sum_sq / count) - (m * m);
    }
    double stddev() const { return std::sqrt(variance()); }
    double cv() const { return mean() > 0 ? stddev() / mean() : 0; }
};

#endif // AUDIO_MEMORY_TEST_H
