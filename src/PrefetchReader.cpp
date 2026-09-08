// SPDX-License-Identifier: MIT
// This file is part of DirettaRendererUPnP.
// See LICENSE for copyright holders and terms.

#include "PrefetchReader.h"
#include "LogLevel.h"

#include <algorithm>
#include <chrono>
#include <cstring>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/error.h>
}

PrefetchReader::PrefetchReader() = default;

PrefetchReader::~PrefetchReader() {
    stop();
}

AVIOContext* PrefetchReader::open(const std::string& url, AVDictionary** options,
                                  AVIOInterruptCB* interruptCb,
                                  std::function<void()> onThreadStart,
                                  size_t initialLimit) {
    stop();

    // By value: on an avformat_open_input() failure FFmpeg frees the context
    // this pointer lives in before the reader thread is stopped.
    m_interruptCb = interruptCb ? *interruptCb : AVIOInterruptCB{nullptr, nullptr};
    m_onThreadStart = std::move(onThreadStart);
    m_stopRequested.store(false, std::memory_order_release);

    int ret = avio_open2(&m_http, url.c_str(), AVIO_FLAG_READ, &m_ownInterrupt, options);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("[Prefetch] Failed to open " << url.substr(0, 80) << ": " << errbuf);
        m_http = nullptr;
        return nullptr;
    }
    m_size = avio_size(m_http);
    if (m_size < 0) m_size = -1;

    unsigned char* ioBuf = static_cast<unsigned char*>(av_malloc(IO_BUF));
    if (!ioBuf) {
        avio_closep(&m_http);
        return nullptr;
    }
    m_pb = avio_alloc_context(ioBuf, IO_BUF, 0, this, &PrefetchReader::readCb, nullptr,
                              &PrefetchReader::seekCb);
    if (!m_pb) {
        av_free(ioBuf);
        avio_closep(&m_http);
        return nullptr;
    }
    // Mirror the inner context, as FFmpeg's own path would see it: http marks
    // a response without Accept-Ranges/Content-Range as streamed, and a seek
    // the demuxer attempted on that would only fail later.
    m_pb->seekable = m_http->seekable;

    m_ring.resize(RING_SIZE);
    m_head = m_tail = m_count = 0;
    m_demuxPos = 0;
    m_error = 0;
    m_limit = std::min(std::max(initialLimit, static_cast<size_t>(IO_BUF)), RING_SIZE);

    startThread();

    LOG_INFO("[Prefetch] Reader started (ring " << (RING_SIZE >> 20) << " MB, fill limit "
             << (m_limit >> 10) << " KB, size "
             << (m_size > 0 ? std::to_string(m_size) : std::string("unknown")) << ")");
    return m_pb;
}

void PrefetchReader::setLimit(size_t limit) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_limit = std::min(std::max(limit, static_cast<size_t>(IO_BUF)), RING_SIZE);
    }
    m_spaceAvailable.notify_one();
}

void PrefetchReader::stop() {
    stopThread();
    if (m_pb) {
        // The outer context is owned by the caller's AVFormatContext until it
        // is closed; the caller frees buffer + struct (AVFMT_FLAG_CUSTOM_IO
        // semantics) exactly like the Audirvana wrapper. Just forget it here.
        m_pb = nullptr;
    }
    if (m_http) {
        avio_closep(&m_http);
    }
    std::vector<uint8_t>().swap(m_ring);
}

int PrefetchReader::interruptCb(void* opaque) {
    auto* self = static_cast<PrefetchReader*>(opaque);
    if (self->m_stopRequested.load(std::memory_order_acquire)) return 1;
    if (self->m_interruptCb.callback) {
        return self->m_interruptCb.callback(self->m_interruptCb.opaque);
    }
    return 0;
}

//=============================================================================
// Reader thread
//=============================================================================

void PrefetchReader::startThread() {
    m_stopping = false;
    m_stopRequested.store(false, std::memory_order_release);
    m_thread = std::thread(&PrefetchReader::threadFunc, this);
}

void PrefetchReader::stopThread() {
    m_stopRequested.store(true, std::memory_order_release);  // unblocks a network read
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_spaceAvailable.notify_all();
    m_dataAvailable.notify_all();
    if (m_thread.joinable()) m_thread.join();
    // The thread is gone: a following avio_seek()/avio_closep() on the inner
    // context must not see the abort flag.
    m_stopRequested.store(false, std::memory_order_release);
}

// Single writer of [m_tail, m_tail + n): the demuxer only ever reads
// [m_head, m_head + m_count), so the network read and the copy into the ring
// happen without the mutex; the mutex only publishes m_count.
void PrefetchReader::threadFunc() {
    if (m_onThreadStart) m_onThreadStart();

    std::vector<uint8_t> chunk(READ_CHUNK);

    while (true) {
        size_t toRead, tail;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // Low-water refill: wake only when a worthwhile amount fits, so a
            // demuxer consuming a few KB at a time does not wake us per read.
            m_spaceAvailable.wait(lock, [this] {
                return m_stopping || m_error != 0 || m_count + READ_MIN <= m_limit;
            });
            if (m_stopping || m_error != 0) break;
            toRead = std::min(READ_CHUNK, m_limit - m_count);
            tail = m_tail;
        }

        // avio_read_partial() returns after one underlying read (whatever the
        // socket had), so a slow source is forwarded byte by byte instead of
        // after a full chunk — a 128 kbit/s radio would otherwise need 16 s
        // to deliver the first 256 KB and trip the decoder's stall deadline.
        int n = avio_read_partial(m_http, chunk.data(), static_cast<int>(toRead));

        if (n > 0) {
            size_t first = std::min(static_cast<size_t>(n), RING_SIZE - tail);
            std::memcpy(m_ring.data() + tail, chunk.data(), first);
            if (first < static_cast<size_t>(n)) {
                std::memcpy(m_ring.data(), chunk.data() + first, n - first);
            }
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) break;
        if (n > 0) {
            m_tail = (tail + n) % RING_SIZE;
            m_count += n;
        } else if (n == 0) {
            m_error = (m_http->error < 0) ? m_http->error : AVERROR_EOF;
        } else {
            m_error = n;  // AVERROR code, AVERROR_EXIT when interrupted
        }
        m_dataAvailable.notify_all();
        if (m_error != 0) break;
    }

    m_dataAvailable.notify_all();
}

//=============================================================================
// Demuxer side
//=============================================================================

int PrefetchReader::readCb(void* opaque, uint8_t* buf, int bufSize) {
    return static_cast<PrefetchReader*>(opaque)->readFromRing(buf, bufSize);
}

int64_t PrefetchReader::seekCb(void* opaque, int64_t offset, int whence) {
    return static_cast<PrefetchReader*>(opaque)->seekTo(offset, whence);
}

int PrefetchReader::readFromRing(uint8_t* buf, int bufSize) {
    if (bufSize <= 0) return 0;
    std::unique_lock<std::mutex> lock(m_mutex);

    // Wait for data; poll the interrupt callback every 50 ms so the
    // decoder's stall deadline keeps working exactly as before.
    while (m_count == 0 && m_error == 0 && !m_stopping) {
        m_dataAvailable.wait_for(lock, std::chrono::milliseconds(50));
        if (m_interruptCb.callback && m_interruptCb.callback(m_interruptCb.opaque)) {
            return AVERROR_EXIT;
        }
    }
    if (m_stopping) return AVERROR_EXIT;
    if (m_count == 0) {
        return m_error;  // AVERROR_EOF or the read error
    }

    size_t n = std::min(static_cast<size_t>(bufSize), m_count);
    size_t first = std::min(n, RING_SIZE - m_head);
    std::memcpy(buf, m_ring.data() + m_head, first);
    if (first < n) {
        std::memcpy(buf + first, m_ring.data(), n - first);
    }
    m_head = (m_head + n) % RING_SIZE;
    m_count -= n;
    m_demuxPos += n;
    bool wakeReader = (m_count + READ_MIN <= m_limit);
    lock.unlock();
    if (wakeReader) m_spaceAvailable.notify_one();
    return static_cast<int>(n);
}

int64_t PrefetchReader::seekTo(int64_t offset, int whence) {
    if (whence == AVSEEK_SIZE) {
        return m_size > 0 ? m_size : AVERROR(ENOSYS);
    }
    whence &= ~AVSEEK_FORCE;

    int64_t target;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        switch (whence) {
            case SEEK_SET: target = offset; break;
            case SEEK_CUR: target = m_demuxPos + offset; break;
            case SEEK_END:
                if (m_size < 0) return AVERROR(ENOSYS);
                target = m_size + offset;
                break;
            default: return AVERROR(EINVAL);
        }
        if (target < 0) return AVERROR(EINVAL);

        // Forward seek inside what is already buffered: just skip.
        if (target >= m_demuxPos && target - m_demuxPos <= static_cast<int64_t>(m_count)) {
            size_t skip = static_cast<size_t>(target - m_demuxPos);
            m_head = (m_head + skip) % RING_SIZE;
            m_count -= skip;
            m_demuxPos = target;
            m_spaceAvailable.notify_one();
            return target;
        }
    }

    // Otherwise restart the reader at the new position (HTTP Range).
    stopThread();
    // stopThread() may have aborted the reader mid-read through the interrupt
    // callback; that leaves the inner context flagged eof/error (sticky), and
    // the seek below would fail with the stale AVERROR_EOF.
    m_http->eof_reached = 0;
    m_http->error = 0;
    int64_t ret = avio_seek(m_http, target, SEEK_SET);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_head = m_tail = m_count = 0;
        m_error = 0;
        m_stopping = false;   // not "stopping" any more, whatever happens next
        if (ret < 0) {
            m_error = static_cast<int>(ret);   // reads now report the seek error
        } else {
            m_demuxPos = ret;
        }
    }
    if (ret >= 0) startThread();
    return ret;
}
