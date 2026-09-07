// SPDX-License-Identifier: MIT
// This file is part of DirettaRendererUPnP.
// See LICENSE for copyright holders and terms.

/**
 * @file test_decode.cpp
 * @brief Decode a URL through AudioDecoder exactly like the renderer does
 *        (prefetch thread, bypass, EOF drain) and print sample count + FNV-1a
 *        hash of the output bytes, for comparison against `ffmpeg -f s32le`.
 *
 * Usage: test_decode <url> <outputBits:16|24|32> [--no-prefetch] [--dump file]
 */

#include "AudioEngine.h"
#include "LogLevel.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

LogLevel g_logLevel = LogLevel::INFO;
bool g_prefetchEnabled = true;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: test_decode <url> <bits> [--no-prefetch] [--dump file] [--quiet]" << std::endl;
        return 2;
    }
    std::string url = argv[1];
    uint32_t bits = static_cast<uint32_t>(std::stoi(argv[2]));
    const char* dumpPath = nullptr;
    double seekTo = -1.0;
    std::vector<double> seeks;   // --seeks 30,60,10,45 : rapid successive seeks
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--no-prefetch")) g_prefetchEnabled = false;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpPath = argv[++i];
        else if (!strcmp(argv[i], "--seek") && i + 1 < argc) seekTo = std::stod(argv[++i]);
        else if (!strcmp(argv[i], "--seeks") && i + 1 < argc) {
            std::string list = argv[++i];
            size_t p = 0;
            while (p <= list.size()) {
                size_t c = list.find(',', p);
                if (c == std::string::npos) c = list.size();
                if (c > p) seeks.push_back(std::stod(list.substr(p, c - p)));
                p = c + 1;
            }
            if (!seeks.empty()) seekTo = seeks.back();
        }
        else if (!strcmp(argv[i], "--quiet")) g_logLevel = LogLevel::WARN;
    }

    AudioDecoder dec;
    if (!dec.open(url)) {
        std::cerr << "open failed" << std::endl;
        return 1;
    }
    const TrackInfo& info = dec.getTrackInfo();
    std::cout << "track: " << info.sampleRate << "Hz/" << info.bitDepth << "bit/"
              << info.channels << "ch codec=" << info.codec
              << " duration=" << info.duration << " samples" << std::endl;

    if (seekTo >= 0.0) {
        // Decode a little first so the seek exercises a running pipeline
        AudioBuffer warm(1 << 20);
        dec.readSamples(warm, 2048, info.sampleRate, bits);
        if (seeks.empty()) seeks.push_back(seekTo);
        for (double s : seeks) {
            if (!dec.seek(s)) {
                std::cerr << "seek failed at " << s << " s" << std::endl;
                return 1;
            }
            // one short read between seeks, like the renderer's process() loop
            dec.readSamples(warm, 2048, info.sampleRate, bits);
        }
        std::cout << "seek: " << seeks.size() << " seek(s), last " << seekTo
                  << " s → expecting about "
                  << (info.duration - static_cast<uint64_t>(seekTo * info.sampleRate) - 2048)
                  << " frames left" << std::endl;
    }

    std::ofstream dump;
    if (dumpPath) dump.open(dumpPath, std::ios::binary);

    AudioBuffer buffer(1 << 20);
    // PCM: one frame = channels × container bytes. DSD: readSamples() counts
    // 1-bit samples per channel and returns [all L][all R] planar bytes, so
    // n samples = n × channels / 8 bytes.
    size_t bytesPerFrame = ((bits == 16) ? 2 : 4) * info.channels;
    auto bytesFor = [&](size_t n) {
        return info.isDSD ? (n * info.channels) / 8 : n * bytesPerFrame;
    };
    if (info.isDSD) {
        std::cout << "dsd: rate=" << info.dsdRate << " source="
                  << (info.dsdSourceFormat == TrackInfo::DSDSourceFormat::DFF ? "DFF" :
                      info.dsdSourceFormat == TrackInfo::DSDSourceFormat::DSF ? "DSF" : "?") << std::endl;
    }
    uint64_t hash = 1469598103934665603ULL;
    uint64_t total = 0;
    int calls = 0;
    while (true) {
        size_t n = dec.readSamples(buffer, 2048, info.sampleRate, bits);
        if (n == 0) break;
        calls++;
        total += n;
        const uint8_t* p = buffer.data();
        size_t len = bytesFor(n);
        for (size_t i = 0; i < len; i++) {
            hash ^= p[i];
            hash *= 1099511628211ULL;
        }
        if (dump.is_open()) dump.write(reinterpret_cast<const char*>(p), len);
    }
    std::cout << "frames=" << total << " expected=" << info.duration
              << " calls=" << calls << " eof=" << dec.isEOF()
              << " decodeError=" << dec.hasDecodeError()
              << " readTimeout=" << dec.hasReadTimeout() << std::endl;
    printf("fnv1a=%016llx\n", static_cast<unsigned long long>(hash));
    dec.close();
    return 0;
}
