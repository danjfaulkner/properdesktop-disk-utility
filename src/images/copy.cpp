// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "images/copy.h"

#include "base/event_loop.h"
#include "base/i18n.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace images {

std::string CopyResult::message() const
{
    const std::string reason = error ? std::strerror(error) : std::string();
    switch (status) {
    case Status::Done:
    case Status::Cancelled:
        return {};
    case Status::ReadFailed:
        return format(tr("The source could not be read: {reason}"), {{"reason", reason}});
    case Status::WriteFailed:
        if (error == ENOSPC)
            return tr("The destination is too small.");
        return format(tr("The destination could not be written: {reason}"), {{"reason", reason}});
    case Status::SourceEnded:
        return tr("The source ended before all of it was read.");
    case Status::SyncFailed:
        return format(tr("The data could not be saved on the destination: {reason}"), {{"reason", reason}});
    }
    return {};
}

static bool allZero(const char* data, size_t size)
{
    // A buffer is zero when its first byte is and it equals itself shifted by one.
    return size == 0 || (data[0] == 0 && std::memcmp(data, data + 1, size - 1) == 0);
}

// Fills the buffer from the offset; fewer bytes only at the end of the source.
static ssize_t readFully(int fd, char* data, size_t size, uint64_t offset)
{
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::pread(fd, data + done, size - done, off_t(offset + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        done += size_t(n);
    }
    return ssize_t(done);
}

static bool writeFully(int fd, const char* data, size_t size, uint64_t offset)
{
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::pwrite(fd, data + done, size - done, off_t(offset + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0) {
            errno = ENOSPC;
            return false;
        }
        done += size_t(n);
    }
    return true;
}

CopyResult copyData(int source, int target, uint64_t length, const CopyOptions& options,
                    const std::atomic<bool>& cancel, const std::function<void(uint64_t copied)>& progress)
{
    CopyResult result;
    struct stat info {};
    const bool sparse = options.sparse && ::fstat(target, &info) == 0 && S_ISREG(info.st_mode);
    ::posix_fadvise(source, 0, off_t(length), POSIX_FADV_SEQUENTIAL);

    std::vector<char> buffer(std::max<size_t>(options.bufferSize, 4096));
    while (result.copied < length) {
        if (cancel) {
            result.status = CopyResult::Status::Cancelled;
            return result;
        }
        const size_t want = size_t(std::min<uint64_t>(buffer.size(), length - result.copied));
        const ssize_t got = readFully(source, buffer.data(), want, result.copied);
        if (got < 0) {
            result.status = CopyResult::Status::ReadFailed;
            result.error = errno;
            return result;
        }
        if (got == 0) {
            result.status = CopyResult::Status::SourceEnded;
            return result;
        }
        if (!(sparse && allZero(buffer.data(), size_t(got)))
            && !writeFully(target, buffer.data(), size_t(got), result.copied)) {
            result.status = CopyResult::Status::WriteFailed;
            result.error = errno;
            return result;
        }
        result.copied += uint64_t(got);
        if (progress)
            progress(result.copied);
    }

    if (sparse && ::ftruncate(target, off_t(length)) != 0) {
        result.status = CopyResult::Status::WriteFailed;
        result.error = errno;
        return result;
    }
    if (::fsync(target) != 0) {
        result.status = CopyResult::Status::SyncFailed;
        result.error = errno;
    }
    return result;
}

CopyJob::CopyJob() = default;

CopyJob::~CopyJob()
{
    m_alive.reset();
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
}

void CopyJob::start(int source, int target, uint64_t length, CopyOptions options)
{
    if (m_running || m_thread.joinable()) {
        ::close(source);
        ::close(target);
        return;
    }
    m_running = true;
    m_cancel = false;
    const properdesktop::base::LoopHandle loop = properdesktop::base::EventLoop::current()->handle();
    const std::weak_ptr<bool> alive = m_alive;
    m_thread = std::thread([this, source, target, length, options, loop, alive] {
        using Clock = std::chrono::steady_clock;
        Clock::time_point last = Clock::now();
        const CopyResult result = copyData(source, target, length, options, m_cancel, [&](uint64_t copied) {
            const Clock::time_point now = Clock::now();
            if (now - last < std::chrono::milliseconds(100))
                return;
            last = now;
            loop.post([this, alive, copied, length] {
                if (alive.expired() || !m_running)
                    return;
                if (onProgress)
                    onProgress(copied, length);
            });
        });
        ::close(source);
        ::close(target);
        loop.post([this, alive, result] {
            if (alive.expired())
                return;
            m_thread.join();
            m_running = false;
            if (onFinished)
                onFinished(result);
        });
    });
}

void CopyJob::cancel()
{
    m_cancel = true;
}

} // namespace images
