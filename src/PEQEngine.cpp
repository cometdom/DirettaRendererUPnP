// SPDX-License-Identifier: MIT
// This file is part of DirettaRendererUPnP.
// See LICENSE for copyright holders and terms.

/**
 * @file PEQEngine.cpp
 * @brief Parametric EQ filter engine — implementation
 *
 * Biquad coefficient formulas from:
 *   "Cookbook formulae for audio EQ biquad filter coefficients"
 *   by Robert Bristow-Johnson (public domain)
 *   https://www.w3.org/TR/audio-eq-cookbook/
 *
 * Thread-safety design:
 *   - m_activeBands is ONLY written from the audio thread (after initial load or
 *     during the swap inside process()). No lock needed for the DSP loop.
 *   - m_pendingBands is written by the loader thread under m_swapMutex and
 *     moved into m_activeBands by the audio thread when m_swapPending is true.
 *   - In steady state (no config change), process() takes ZERO locks.
 */

#include "PEQEngine.h"
#include <cmath>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <sys/stat.h>
#include <cstring>
#include <climits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================================
// Biquad coefficient computation (RBJ Audio EQ Cookbook)
// ============================================================================

void PEQBand::computeCoeffs(uint32_t sampleRate) {
    if (sampleRate == 0 || freq <= 0.0 || q <= 0.0) {
        // Identity filter (pass-through)
        coeffs = {1.0, 0.0, 0.0, 0.0, 0.0};
        return;
    }

    const double Fs    = static_cast<double>(sampleRate);
    const double f0    = std::clamp(freq, 1.0, Fs / 2.0 - 1.0);
    const double w0    = 2.0 * M_PI * f0 / Fs;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);

    double b0, b1, b2, a0, a1, a2;

    switch (type) {
        case PEQFilterType::Peaking: {
            // Bell/peaking EQ — A = 10^(dBgain/40)
            const double A = std::pow(10.0, gainDb / 40.0);
            b0 =  1.0 + alpha * A;
            b1 = -2.0 * cosw0;
            b2 =  1.0 - alpha * A;
            a0 =  1.0 + alpha / A;
            a1 = -2.0 * cosw0;
            a2 =  1.0 - alpha / A;
            break;
        }

        case PEQFilterType::LowShelf: {
            // Low shelving — A = 10^(dBgain/40), S = 1 (max flat)
            const double A      = std::pow(10.0, gainDb / 40.0);
            const double sqrtA  = std::sqrt(A);
            // alpha_S = sin(w0)/2 * sqrt(2) when shelf slope S = 1
            const double alphaS = sinw0 * std::sqrt(2.0) / 2.0;

            b0 =  A * ((A + 1.0) - (A - 1.0)*cosw0 + 2.0*sqrtA*alphaS);
            b1 =  2.0 * A * ((A - 1.0) - (A + 1.0)*cosw0);
            b2 =  A * ((A + 1.0) - (A - 1.0)*cosw0 - 2.0*sqrtA*alphaS);
            a0 =        (A + 1.0) + (A - 1.0)*cosw0 + 2.0*sqrtA*alphaS;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0)*cosw0);
            a2 =        (A + 1.0) + (A - 1.0)*cosw0 - 2.0*sqrtA*alphaS;
            break;
        }

        case PEQFilterType::HighShelf: {
            const double A      = std::pow(10.0, gainDb / 40.0);
            const double sqrtA  = std::sqrt(A);
            const double alphaS = sinw0 * std::sqrt(2.0) / 2.0;

            b0 =  A * ((A + 1.0) + (A - 1.0)*cosw0 + 2.0*sqrtA*alphaS);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0)*cosw0);
            b2 =  A * ((A + 1.0) + (A - 1.0)*cosw0 - 2.0*sqrtA*alphaS);
            a0 =        (A + 1.0) - (A - 1.0)*cosw0 + 2.0*sqrtA*alphaS;
            a1 =  2.0 * ((A - 1.0) - (A + 1.0)*cosw0);
            a2 =        (A + 1.0) - (A - 1.0)*cosw0 - 2.0*sqrtA*alphaS;
            break;
        }

        case PEQFilterType::LowPass: {
            // 2nd-order Butterworth (Q selects slope; Q=1/√2 is flat Butterworth)
            b0 = (1.0 - cosw0) / 2.0;
            b1 =  1.0 - cosw0;
            b2 = (1.0 - cosw0) / 2.0;
            a0 =  1.0 + alpha;
            a1 = -2.0 * cosw0;
            a2 =  1.0 - alpha;
            break;
        }

        case PEQFilterType::HighPass: {
            b0 =  (1.0 + cosw0) / 2.0;
            b1 = -(1.0 + cosw0);
            b2 =  (1.0 + cosw0) / 2.0;
            a0 =   1.0 + alpha;
            a1 =  -2.0 * cosw0;
            a2 =   1.0 - alpha;
            break;
        }

        case PEQFilterType::Notch: {
            // Sharp null at center frequency
            b0 =  1.0;
            b1 = -2.0 * cosw0;
            b2 =  1.0;
            a0 =  1.0 + alpha;
            a1 = -2.0 * cosw0;
            a2 =  1.0 - alpha;
            break;
        }

        default:
            coeffs = {1.0, 0.0, 0.0, 0.0, 0.0};
            return;
    }

    // Normalise by a0 (RBJ convention)
    coeffs.b0 = b0 / a0;
    coeffs.b1 = b1 / a0;
    coeffs.b2 = b2 / a0;
    coeffs.a1 = a1 / a0;
    coeffs.a2 = a2 / a0;
}

// ============================================================================
// Config file parsing
// ============================================================================

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool PEQEngine::parseBand(const std::string& line, uint32_t sampleRate, PEQBand& out) {
    // Strip inline comments
    std::string stripped = line;
    auto commentPos = stripped.find('#');
    if (commentPos != std::string::npos) {
        stripped = stripped.substr(0, commentPos);
    }
    stripped = trim(stripped);
    if (stripped.empty()) return false;

    std::istringstream iss(stripped);
    std::string typeStr;
    double freq = 0.0, gainDb = 0.0, q = 1.0;

    if (!(iss >> typeStr)) return false;
    if (!(iss >> freq))    return false;
    if (!(iss >> gainDb))  gainDb = 0.0;   // optional for lp/hp/notch
    if (!(iss >> q))       q = 0.707;      // Butterworth default

    typeStr = toLower(typeStr);

    PEQBand band;
    if      (typeStr == "peaking"   || typeStr == "peak")         band.type = PEQFilterType::Peaking;
    else if (typeStr == "lowshelf"  || typeStr == "low_shelf"  ||
             typeStr == "ls"        || typeStr == "loshelf")       band.type = PEQFilterType::LowShelf;
    else if (typeStr == "highshelf" || typeStr == "high_shelf" ||
             typeStr == "hs"        || typeStr == "hishelf")       band.type = PEQFilterType::HighShelf;
    else if (typeStr == "lowpass"   || typeStr == "lp"         ||
             typeStr == "low_pass")                                band.type = PEQFilterType::LowPass;
    else if (typeStr == "highpass"  || typeStr == "hp"         ||
             typeStr == "high_pass")                               band.type = PEQFilterType::HighPass;
    else if (typeStr == "notch")                                   band.type = PEQFilterType::Notch;
    else {
        std::cerr << "[PEQEngine] Unknown filter type: '" << typeStr << "'" << std::endl;
        return false;
    }

    if (freq <= 0.0 || freq >= static_cast<double>(sampleRate) / 2.0) {
        std::cerr << "[PEQEngine] Frequency " << freq
                  << " Hz out of range for sample rate " << sampleRate << " Hz" << std::endl;
        return false;
    }

    if (q <= 0.0) {
        std::cerr << "[PEQEngine] Q must be > 0 (got " << q << ")" << std::endl;
        return false;
    }

    band.freq   = freq;
    band.gainDb = gainDb;
    band.q      = q;
    band.state.reset();
    band.computeCoeffs(sampleRate);

    out = band;
    return true;
}

bool PEQEngine::loadFromFile(const std::string& path, uint32_t sampleRate,
                             std::vector<PEQBand>& bands, bool& bypassed) {
    bypassed = false;
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[PEQEngine] Cannot open config: " << path << std::endl;
        return false;
    }

    bands.clear();
    std::string line;
    int lineNo = 0;

    while (std::getline(file, line)) {
        ++lineNo;

        // Strip inline comment, trim whitespace
        std::string t = line;
        auto cp = t.find('#');
        if (cp != std::string::npos) t = t.substr(0, cp);
        t = trim(t);
        if (t.empty()) continue;

        // ── "bypass" keyword ──
        // Sets the bypass flag; ALL filter lines below are still parsed
        // so their values survive in memory. Removing "bypass" re-enables
        // them on the next hot-reload without losing any settings.
        if (toLower(t) == "bypass") {
            bypassed = true;
            std::cout << "[PEQEngine] Bypass keyword found (line " << lineNo
                      << ") — PEQ disabled, filter values preserved" << std::endl;
            continue;
        }

        PEQBand band;
        if (parseBand(line, sampleRate, band)) {
            if (bands.size() < static_cast<size_t>(PEQ_MAX_BANDS)) {
                bands.push_back(band);
            } else {
                std::cerr << "[PEQEngine] Max bands (" << PEQ_MAX_BANDS
                          << ") reached, ignoring line " << lineNo << std::endl;
                break;
            }
        } else {
            std::cerr << "[PEQEngine] Skipping invalid line " << lineNo
                      << ": " << trim(line) << std::endl;
        }
    }

    return !bands.empty();
}

// ============================================================================
// PEQEngine public interface
// ============================================================================

static void logBands(const std::vector<PEQBand>& bands, bool bypassed) {
    if (bypassed) {
        std::cout << "[PEQEngine]   (bypass active — filters loaded but inactive)" << std::endl;
    }
    for (size_t i = 0; i < bands.size(); ++i) {
        const auto& b = bands[i];
        const char* typeStr = "?";
        switch (b.type) {
            case PEQFilterType::Peaking:   typeStr = "peaking";   break;
            case PEQFilterType::LowShelf:  typeStr = "lowshelf";  break;
            case PEQFilterType::HighShelf: typeStr = "highshelf"; break;
            case PEQFilterType::LowPass:   typeStr = "lowpass";   break;
            case PEQFilterType::HighPass:  typeStr = "highpass";  break;
            case PEQFilterType::Notch:     typeStr = "notch";     break;
        }
        std::cout << "[PEQEngine]   Band " << (i + 1)
                  << ": " << typeStr
                  << "  f=" << b.freq << " Hz"
                  << "  gain=" << b.gainDb << " dB"
                  << "  Q=" << b.q
                  << (bypassed ? "  [bypassed]" : "") << std::endl;
    }
}

bool PEQEngine::load(const std::string& path, uint32_t sampleRate) {
    m_configPath     = path;
    m_lastSampleRate = (sampleRate > 0) ? sampleRate : 44100;

    std::vector<PEQBand> newBands;
    bool bypassed = false;
    bool hasBands = loadFromFile(path, m_lastSampleRate, newBands, bypassed);

    // Stage bands for audio thread pickup.
    // Before the audio thread starts, we can write directly to m_activeBands.
    // m_swapPending stays false — audio thread will pick up m_activeBands directly.
    m_activeBands = newBands;   // audio thread not yet running, direct write is safe
    m_pendingBands.clear();
    m_swapPending.store(false, std::memory_order_relaxed);

    // Update status atomics
    m_bypassed.store(bypassed, std::memory_order_release);
    m_bandCount.store(static_cast<int>(newBands.size()), std::memory_order_release);
    m_enabled.store(hasBands && !bypassed, std::memory_order_release);

    // Record mtime
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        m_lastMtime = st.st_mtime;
    }

    std::cout << "[PEQEngine] Loaded " << newBands.size()
              << " filter band(s) from " << path
              << " (fs=" << m_lastSampleRate << " Hz)"
              << (bypassed ? " [BYPASSED]" : "") << std::endl;
    logBands(newBands, bypassed);

    if (!hasBands && !bypassed) {
        std::cerr << "[PEQEngine] No valid filters in " << path << std::endl;
    }

    return hasBands && !bypassed;
}

void PEQEngine::reload() {
    if (m_configPath.empty()) return;

    struct stat st{};
    if (stat(m_configPath.c_str(), &st) != 0) return;
    if (st.st_mtime == m_lastMtime) return;   // File unchanged — zero work

    std::cout << "[PEQEngine] Config changed, staging reload: " << m_configPath << std::endl;

    std::vector<PEQBand> newBands;
    bool bypassed = false;
    bool hasBands = loadFromFile(m_configPath, m_lastSampleRate, newBands, bypassed);

    // Write new bands into pending slot (loader thread, under m_swapMutex)
    {
        std::lock_guard<std::mutex> lock(m_swapMutex);
        m_pendingBands = std::move(newBands);
    }

    // Update status atomics BEFORE setting m_swapPending so the audio thread
    // sees the correct m_enabled value when it picks up the swap.
    m_bypassed.store(bypassed, std::memory_order_release);
    m_bandCount.store(static_cast<int>(m_pendingBands.size()), std::memory_order_release);
    m_enabled.store(hasBands && !bypassed, std::memory_order_release);

    // Signal audio thread: "new bands ready, please swap on next process()"
    m_swapPending.store(true, std::memory_order_release);
    m_lastMtime = st.st_mtime;

    std::cout << "[PEQEngine] Reload staged: " << m_pendingBands.size()
              << " band(s)" << (bypassed ? " [BYPASSED]" : "") << std::endl;
    (void)hasBands;
}

void PEQEngine::recomputeCoeffs(uint32_t newRate) {
    // Called from audio thread only, on m_activeBands (no lock needed)
    for (auto& band : m_activeBands) {
        band.computeCoeffs(newRate);
        band.state.reset();
    }
    m_lastSampleRate = newRate;
}

// ============================================================================
// Audio processing  (RT-safe: no lock in DSP loop)
// ============================================================================

void PEQEngine::process(uint8_t* buffer, size_t numSamples,
                        uint32_t sampleRate, uint32_t bitDepth, uint32_t channels) {
    if (!buffer || numSamples == 0 || channels == 0) return;
    if (channels > static_cast<uint32_t>(PEQ_MAX_CHANNELS)) channels = PEQ_MAX_CHANNELS;

    // ── Double-buffer swap (triggered by hot-reload) ──────────────────────────
    // This check is a single atomic load — essentially free in steady state.
    // The lock is taken ONLY when the loader thread has staged a reload.
    // Even when bypassed we must swap so that m_activeBands is cleared
    // (otherwise stale state would re-activate if bypass is later removed).
    if (m_swapPending.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(m_swapMutex);
            m_activeBands = std::move(m_pendingBands);
            m_pendingBands.clear();
        }
        m_swapPending.store(false, std::memory_order_release);

        // Recompute coefficients if sample rate changed since last load
        if (sampleRate != m_lastSampleRate && sampleRate > 0 && !m_activeBands.empty()) {
            std::cout << "[PEQEngine] Sample rate changed after reload "
                      << m_lastSampleRate << " -> " << sampleRate
                      << " Hz, recomputing" << std::endl;
            recomputeCoeffs(sampleRate);
        }
    }

    // ── Enabled check (lock-free) ─────────────────────────────────────────────
    // Fast exit if: (a) no bands loaded, or (b) "bypass" keyword active.
    // In both cases we do absolutely no DSP work.
    if (!m_enabled.load(std::memory_order_acquire)) return;
    if (m_activeBands.empty()) return;

    // ── Sample rate change (gapless transition) ───────────────────────────────
    if (sampleRate != m_lastSampleRate && sampleRate > 0) {
        std::cout << "[PEQEngine] Sample rate changed " << m_lastSampleRate
                  << " -> " << sampleRate << " Hz, recomputing coefficients" << std::endl;
        recomputeCoeffs(sampleRate);
    }

    // ══════════════════════════════════════════════════════════════════════════
    // DSP LOOP — entirely lock-free from this point.
    // m_activeBands is written only by THIS thread (after the swap above).
    // ══════════════════════════════════════════════════════════════════════════

    // ── 16-bit path ───────────────────────────────────────────────────────────
    if (bitDepth == 16) {
        auto* samples = reinterpret_cast<int16_t*>(buffer);

        for (auto& band : m_activeBands) {
            const double b0 = band.coeffs.b0;
            const double b1 = band.coeffs.b1;
            const double b2 = band.coeffs.b2;
            const double a1 = band.coeffs.a1;
            const double a2 = band.coeffs.a2;

            for (uint32_t ch = 0; ch < channels; ++ch) {
                double& x1 = band.state.x1[ch];
                double& x2 = band.state.x2[ch];
                double& y1 = band.state.y1[ch];
                double& y2 = band.state.y2[ch];

                for (size_t i = 0; i < numSamples; ++i) {
                    const double x = static_cast<double>(samples[i * channels + ch]);
                    // Direct Form I: y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2
                    const double y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2;
                    x2 = x1; x1 = x;
                    y2 = y1; y1 = y;
                    samples[i * channels + ch] =
                        static_cast<int16_t>(std::clamp(y, -32768.0, 32767.0));
                }
            }
        }
        return;
    }

    // ── 24-bit / 32-bit path (S32 container) ─────────────────────────────────
    if (bitDepth == 24 || bitDepth == 32) {
        auto* samples = reinterpret_cast<int32_t*>(buffer);

        // Normalise to [-1, +1] for numerically stable biquad at 32-bit precision.
        // Works correctly for both true 32-bit and 24-bit-in-32-bit MSB-aligned.
        static constexpr double kScale    = 1.0 / 2147483648.0;
        static constexpr double kScaleInv = 2147483648.0;

        for (auto& band : m_activeBands) {
            const double b0 = band.coeffs.b0;
            const double b1 = band.coeffs.b1;
            const double b2 = band.coeffs.b2;
            const double a1 = band.coeffs.a1;
            const double a2 = band.coeffs.a2;

            for (uint32_t ch = 0; ch < channels; ++ch) {
                double& x1 = band.state.x1[ch];
                double& x2 = band.state.x2[ch];
                double& y1 = band.state.y1[ch];
                double& y2 = band.state.y2[ch];

                for (size_t i = 0; i < numSamples; ++i) {
                    const double x = static_cast<double>(samples[i * channels + ch]) * kScale;
                    const double y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2;
                    x2 = x1; x1 = x;
                    y2 = y1; y1 = y;
                    const double clipped = std::clamp(y, -1.0, 1.0 - (1.0 / kScaleInv));
                    samples[i * channels + ch] = static_cast<int32_t>(clipped * kScaleInv);
                }
            }
        }
        return;
    }

    // Unknown bit depth — log once and skip
    static bool warnedUnknownDepth = false;
    if (!warnedUnknownDepth) {
        std::cerr << "[PEQEngine] Unsupported bitDepth=" << bitDepth
                  << ", PEQ bypassed" << std::endl;
        warnedUnknownDepth = true;
    }
}
