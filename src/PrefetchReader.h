// SPDX-License-Identifier: MIT
// This file is part of DirettaRendererUPnP.
// See LICENSE for copyright holders and terms.

/**
 * @file PrefetchReader.h
 * @brief HTTP source reader running on its own thread, feeding the demuxer
 *        through a custom AVIOContext.
 *
 * Without it, every network read of the current track happens on the decode
 * thread: FFmpeg refills its avio buffer synchronously from inside
 * av_read_frame(), i.e. recv(2), TCP processing and the occasional stall all
 * land on the SCHED_FIFO decode core, right next to the Diretta worker.
 *
 * With it, a plain SCHED_OTHER thread on the --cpu-other cores keeps a
 * bounded byte ring (a few seconds of audio) ahead of the demuxer; the
 * decode thread only ever copies from that ring. Seeks are forwarded to the
 * inner HTTP context (Range requests) after draining the ring.
 *
 * The object owns the inner AVIOContext (HTTP) and the outer custom
 * AVIOContext handed to avformat_open_input(); the caller must stop() the
 * reader before closing the format context and destroy the reader after.
 * stop() returns promptly even if the reader is blocked in a network read:
 * the inner context is opened with our own interrupt callback, which
 * reports "abort" as soon as a stop is requested and otherwise chains to
 * the decoder's stall-deadline callback.
 */

#ifndef PREFETCH_READER_H
#define PREFETCH_READER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

class PrefetchReader {
public:
    static constexpr size_t RING_SIZE  = 4u << 20;   // 4 MB ahead of the demuxer
    static constexpr size_t READ_CHUNK = 256u << 10; // max bytes per avio_read_partial()
    static constexpr size_t READ_MIN   = 64u << 10;  // refill only when this much fits
    static constexpr int    IO_BUF     = 64 << 10;   // outer AVIOContext buffer (demuxer side)

    PrefetchReader();
    ~PrefetchReader();

    PrefetchReader(const PrefetchReader&) = delete;
    PrefetchReader& operator=(const PrefetchReader&) = delete;

    /**
     * @brief Open the HTTP source and start the reader thread.
     * @param url            Source URL
     * @param options        FFmpeg protocol options (consumed like avio_open2)
     * @param interruptCb    The format context's interrupt callback, checked
     *                       while a read waits for data and by the inner HTTP
     *                       context (stall detection stays with the decoder)
     * @param onThreadStart  Called first thing on the reader thread (affinity,
     *                       scheduling policy)
     * @param initialLimit   Bytes the reader may buffer before the first
     *                       setLimit(): a preloaded next track uses a small
     *                       limit so it does not pull megabytes from the
     *                       server while the current track is playing
     *                       (see e4c4428, Audirvana stream corruption)
     * @return the AVIOContext to install as AVFormatContext::pb (with
     *         AVFMT_FLAG_CUSTOM_IO), or nullptr on failure
     */
    AVIOContext* open(const std::string& url, AVDictionary** options,
                      AVIOInterruptCB* interruptCb,
                      std::function<void()> onThreadStart,
                      size_t initialLimit = RING_SIZE);

    /** @brief Raise (or lower) how far ahead the reader may buffer. */
    void setLimit(size_t limit);

    /** @brief Stop the reader thread and close the HTTP context (idempotent). */
    void stop();

private:
    static int  readCb(void* opaque, uint8_t* buf, int bufSize);
    static int64_t seekCb(void* opaque, int64_t offset, int whence);
    static int  interruptCb(void* opaque);

    int  readFromRing(uint8_t* buf, int bufSize);
    int64_t seekTo(int64_t offset, int whence);
    void threadFunc();
    void startThread();
    void stopThread();

    // Inner (HTTP) side — only the reader thread touches m_http while running
    AVIOContext* m_http = nullptr;
    AVIOContext* m_pb = nullptr;
    AVIOInterruptCB m_interruptCb{nullptr, nullptr};   // copy of the decoder's (stall deadline)
    AVIOInterruptCB m_ownInterrupt{&PrefetchReader::interruptCb, this};  // ours: stop + chain
    std::atomic<bool> m_stopRequested{false};   // makes a blocked avio_read() return at stop
    std::function<void()> m_onThreadStart;
    int64_t m_size = -1;          // total size if known (AVSEEK_SIZE)

    // Byte ring shared by the two threads
    std::vector<uint8_t> m_ring;
    size_t m_head = 0;            // next byte to hand to the demuxer
    size_t m_tail = 0;            // next byte the reader writes
    size_t m_count = 0;           // bytes in the ring
    size_t m_limit = RING_SIZE;   // reader stops filling beyond this
    int64_t m_demuxPos = 0;       // logical stream position of m_head
    int    m_error = 0;           // sticky: AVERROR_EOF or a read error
    bool   m_stopping = false;

    std::mutex m_mutex;
    std::condition_variable m_dataAvailable;   // reader → demuxer
    std::condition_variable m_spaceAvailable;  // demuxer → reader
    std::thread m_thread;
};

#endif // PREFETCH_READER_H
