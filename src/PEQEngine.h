// SPDX-License-Identifier: MIT
// This file is part of DirettaRendererUPnP.
// See LICENSE for copyright holders and terms.

/**
 * @file PEQEngine.h
 * @brief Parametric EQ filter engine (PCM-only, biquad IIR)
 *
 * Self-contained, zero-dependency implementation using the RBJ Audio EQ Cookbook
 * (Robert Bristow-Johnson, public domain).
 *
 * Supported filter types:
 *   peaking   - Bell/peaking EQ (boost or cut at center frequency)
 *   lowshelf  - Low-frequency shelving
 *   highshelf - High-frequency shelving
 *   lowpass   - 2nd-order Butterworth low-pass
 *   highpass  - 2nd-order Butterworth high-pass
 *   notch     - Narrow null (band-reject)
 *
 * Config file format (one filter per line):
 *   # comment
 *   TYPE  FREQ_HZ  GAIN_DB  Q
 *
 * Example:
 *   peaking   80     -4.0   3.0    # tame 80 Hz room mode
 *   highshelf 10000   1.5   0.7    # gentle treble lift
 *
 * GAIN_DB is ignored for lowpass, highpass, notch.
 * Q (quality factor) must be > 0. Typical values: 0.5–10.
 *
 * Usage:
 *   PEQEngine peq;
 *   peq.load("/etc/direttarenderer/peq.conf");
 *   // In audio thread:
 *   peq.reload();   // hot-reload if file changed (call periodically, e.g. every 5s)
 *   peq.process(buffer, numSamples, sampleRate, bitDepth, channels);
 */

#ifndef PEQ_ENGINE_H
#define PEQ_ENGINE_H

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <ctime>

// Maximum number of supported channels (mono/stereo/multi)
static constexpr int PEQ_MAX_CHANNELS = 8;

// Maximum number of filter bands
static constexpr int PEQ_MAX_BANDS = 32;

/**
 * @brief Filter type for a PEQ band
 */
enum class PEQFilterType {
    Peaking,
    LowShelf,
    HighShelf,
    LowPass,
    HighPass,
    Notch
};

/**
 * @brief Normalised biquad coefficients (Direct Form I)
 *
 * Transfer function:
 *   H(z) = (b0 + b1*z^-1 + b2*z^-2) / (1 + a1*z^-1 + a2*z^-2)
 *
 * Note: a0 has been divided out, so a0 = 1 always here.
 */
struct BiquadCoeffs {
    double b0, b1, b2;   ///< Numerator coefficients
    double a1, a2;        ///< Denominator (feedback) coefficients (a0 = 1)
};

/**
 * @brief Per-channel biquad state (Direct Form I delay lines)
 *
 * Aligned to a cache line to avoid false sharing between channel states.
 */
struct alignas(64) BiquadState {
    double x1[PEQ_MAX_CHANNELS]{};  ///< x[n-1] per channel
    double x2[PEQ_MAX_CHANNELS]{};  ///< x[n-2] per channel
    double y1[PEQ_MAX_CHANNELS]{};  ///< y[n-1] per channel
    double y2[PEQ_MAX_CHANNELS]{};  ///< y[n-2] per channel

    void reset() {
        for (int ch = 0; ch < PEQ_MAX_CHANNELS; ++ch) {
            x1[ch] = x2[ch] = y1[ch] = y2[ch] = 0.0;
        }
    }
};

/**
 * @brief A single PEQ band (one biquad filter)
 */
struct PEQBand {
    PEQFilterType type = PEQFilterType::Peaking;
    double        freq   = 1000.0;   ///< Center/corner frequency in Hz
    double        gainDb =    0.0;   ///< Gain in dB (peaking/shelf only)
    double        q      =    1.0;   ///< Quality factor

    BiquadCoeffs  coeffs{};          ///< Pre-computed at load time
    BiquadState   state{};           ///< Runtime state (per-channel delay lines)

    /**
     * @brief Compute biquad coefficients from parameters and sample rate.
     *        Must be called whenever freq/gainDb/q or sampleRate changes.
     * @param sampleRate  Output sample rate in Hz
     */
    void computeCoeffs(uint32_t sampleRate);
};
/**
 * @brief Parametric EQ engine — zero-dependency, file-driven, hot-reloadable
 *
 * Thread-safety model (RT-safe double buffer):
 *
 *   Loader thread (position thread, non-RT):
 *     - Calls reload() every 5 s. It only does work when mtime changes.
 *     - Parses new bands into m_pendingBands under m_swapMutex.
 *     - Sets m_swapPending = true and updates m_enabled atomically.
 *
 *   Audio thread (RT):
 *     - process() checks m_swapPending atomically (a single 1-byte load).
 *       If false (steady state), NO lock is ever taken — DSP runs freely.
 *       If true (reload occurred), takes m_swapMutex for the swap only
 *       (~microseconds), then releases and runs DSP lock-free on m_activeBands.
 *     - m_activeBands is ONLY accessed by the audio thread after initial setup.
 *
 *   Bypass:
 *     - Adding "bypass" anywhere in the config file sets m_enabled = false.
 *     - Filter values are still parsed and stored — removing "bypass" re-enables
 *       them immediately on the next hot-reload without losing any settings.
 */
class PEQEngine {
public:
    PEQEngine() = default;
    ~PEQEngine() = default;

    // Non-copyable
    PEQEngine(const PEQEngine&) = delete;
    PEQEngine& operator=(const PEQEngine&) = delete;

    /**
     * @brief Load filter config from file.
     *        May be called before the audio thread starts.
     * @param path        Absolute path to the PEQ config file
     * @param sampleRate  Initial sample rate (for coefficient computation)
     * @return true if at least one filter was loaded and not bypassed
     */
    bool load(const std::string& path, uint32_t sampleRate = 44100);

    /**
     * @brief Check mtime and reload if the file has changed.
     *        Call periodically (every 5 s) from the non-RT position thread.
     *        Triggers a double-buffer swap: audio thread picks it up on next
     *        process() call with a brief (~µs) lock, then runs DSP lock-free.
     */
    void reload();

    /**
     * @brief Apply all PEQ bands in-place to a packed-integer PCM buffer.
     *
     * RT-safe: NO lock is held during the DSP loop in steady state.
     * A brief lock is taken ONLY when a reload has just occurred (m_swapPending).
     * DSD must NEVER be passed here — guard with !trackInfo.isDSD upstream.
     *
     * @param buffer       Pointer to packed integer PCM samples (modified in place)
     * @param numSamples   Frame count (NOT byte count, NOT sample×channels)
     * @param sampleRate   Current sample rate (coefficients recomputed if changed)
     * @param bitDepth     16 or 32 (24-bit content is passed as 32)
     * @param channels     Number of interleaved channels (typically 2)
     */
    void process(uint8_t* buffer, size_t numSamples,
                 uint32_t sampleRate, uint32_t bitDepth, uint32_t channels);

    /**
     * @brief Returns true if PEQ is active (bands loaded AND not bypassed).
     *        Lock-free, safe to call from any thread.
     */
    bool isEnabled() const { return m_enabled.load(std::memory_order_acquire); }

    /**
     * @brief Returns true if the file has a "bypass" line (PEQ deliberately off).
     *        Lock-free, safe to call from any thread.
     */
    bool isBypassed() const { return m_bypassed.load(std::memory_order_acquire); }

    /**
     * @brief Number of active filter bands (for status logging, non-RT).
     */
    int bandCount() const { return m_bandCount.load(std::memory_order_acquire); }

    /**
     * @brief Config file path this engine was loaded from.
     */
    const std::string& configPath() const { return m_configPath; }

private:
    /**
     * @brief Parse a single non-comment, non-bypass line from the config file.
     * @param line       Raw line text (may have inline comments)
     * @param sampleRate Sample rate for coefficient computation
     * @param out        Populated PEQBand on success
     * @return true on success
     */
    static bool parseBand(const std::string& line, uint32_t sampleRate, PEQBand& out);

    /**
     * @brief Load bands from file into a staging vector.
     *        Sets bypassed=true if a "bypass" line is found anywhere in the file.
     *        Bands below "bypass" are still parsed and stored (preserved for when
     *        bypass is removed).
     *
     * @param path        Config file path
     * @param sampleRate  For coefficient computation
     * @param bands       Output: parsed bands (populated even when bypassed)
     * @param bypassed    Output: true if "bypass" keyword found in file
     * @return true if at least one band was parsed (regardless of bypass state)
     */
    static bool loadFromFile(const std::string& path, uint32_t sampleRate,
                             std::vector<PEQBand>& bands, bool& bypassed);

    /**
     * @brief Recompute all biquad coefficients and reset state for new sample rate.
     *        Called on m_activeBands from the audio thread only.
     */
    void recomputeCoeffs(uint32_t newRate);

    // ── Double-buffer for RT-safe hot-reload ────────────────────────────────
    //
    // m_activeBands  — owned exclusively by the audio thread after initial load.
    //                  Never written by the loader thread.
    // m_pendingBands — written by the loader thread under m_swapMutex.
    //                  Swapped into m_activeBands by the audio thread on next
    //                  process() when m_swapPending is true.
    // m_swapPending  — atomic flag: set by loader, cleared by audio thread.
    //                  In steady state (no reload), the audio thread only reads
    //                  this single byte — the DSP path is otherwise lock-free.
    std::mutex            m_swapMutex;
    std::vector<PEQBand>  m_activeBands;    ///< Audio thread only — no lock needed
    std::vector<PEQBand>  m_pendingBands;   ///< Loader thread → audio thread
    std::atomic<bool>     m_swapPending{false};

    // ── Hot-reload state ─────────────────────────────────────────────────────
    std::string           m_configPath;
    std::time_t           m_lastMtime = 0;
    uint32_t              m_lastSampleRate = 0;

    // ── Lock-free status flags ───────────────────────────────────────────────
    std::atomic<bool>     m_enabled{false};  ///< true = bands loaded AND not bypassed
    std::atomic<bool>     m_bypassed{false}; ///< true = "bypass" keyword found in file
    std::atomic<int>      m_bandCount{0};    ///< Band count for non-RT status queries
};

#endif // PEQ_ENGINE_H
