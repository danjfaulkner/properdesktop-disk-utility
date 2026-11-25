// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "images/copy.h"

#include "base/event_loop.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace images;

class CopyTest : public testing::Test
{
protected:
    void SetUp() override
    {
        folder = std::filesystem::temp_directory_path() / ("disk-utility-copy-" + std::to_string(::getpid()));
        std::filesystem::create_directories(folder);
    }
    void TearDown() override { std::filesystem::remove_all(folder); }

    std::string file(const std::string& name) { return (folder / name).string(); }

    std::string write(const std::string& name, const std::string& data)
    {
        std::ofstream(file(name), std::ios::binary) << data;
        return file(name);
    }

    static std::string read(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }

    static std::string noise(size_t size)
    {
        std::mt19937 random(7);
        std::string data(size, '\0');
        for (char& c : data)
            c = char(random());
        return data;
    }

    // Runs the loop until the condition holds, for the job's thread.
    static bool waitFor(const std::function<bool()>& condition)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!condition()) {
            if (std::chrono::steady_clock::now() > end)
                return false;
            properdesktop::base::EventLoop::current()->dispatch();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    std::filesystem::path folder;
    std::atomic<bool> cancel = false;
};

TEST_F(CopyTest, CopiesTheLengthAcrossBuffers)
{
    const std::string data = noise(10'000 + 123);
    const int source = ::open(write("source", data).c_str(), O_RDONLY);
    const int target = ::open(file("target").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    std::vector<uint64_t> reported;
    const CopyResult result = copyData(source, target, 9000, {.bufferSize = 4096}, cancel,
                                       [&](uint64_t copied) { reported.push_back(copied); });
    ::close(source);
    ::close(target);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.copied, 9000u);
    EXPECT_EQ(result.message(), "");
    EXPECT_EQ(reported, (std::vector<uint64_t>{4096, 8192, 9000}));
    EXPECT_EQ(read(file("target")), data.substr(0, 9000));
}

TEST_F(CopyTest, WritesOverTheStartOfTheTarget)
{
    const int source = ::open(write("source", "abc").c_str(), O_RDONLY);
    const int target = ::open(write("target", "123456").c_str(), O_WRONLY);
    EXPECT_TRUE(copyData(source, target, 3, {}, cancel).ok());
    ::close(source);
    ::close(target);
    EXPECT_EQ(read(file("target")), "abc456");
}

TEST_F(CopyTest, SparseTargetsKeepHoles)
{
    std::string data(8 << 20, '\0');
    data.replace(0, 5, "start");
    data.replace(data.size() - 3, 3, "end");
    const int source = ::open(write("source", data).c_str(), O_RDONLY);
    const int target = ::open(file("target").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const CopyResult result = copyData(source, target, data.size(), {.sparse = true, .bufferSize = 1 << 20}, cancel);
    ::close(source);
    ::close(target);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(read(file("target")), data);
    struct stat info {};
    ASSERT_EQ(::stat(file("target").c_str(), &info), 0);
    EXPECT_LT(uint64_t(info.st_blocks) * 512, uint64_t(4 << 20));

    // A zero end is still the length.
    const int again = ::open(file("zeros").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int zero = ::open("/dev/zero", O_RDONLY);
    EXPECT_TRUE(copyData(zero, again, 3 << 20, {.sparse = true}, cancel).ok());
    ::close(zero);
    ::close(again);
    EXPECT_EQ(std::filesystem::file_size(file("zeros")), uint64_t(3 << 20));
}

TEST_F(CopyTest, ShortSource)
{
    const int source = ::open(write("source", "abc").c_str(), O_RDONLY);
    const int target = ::open(file("target").c_str(), O_WRONLY | O_CREAT, 0644);
    const CopyResult result = copyData(source, target, 10, {}, cancel);
    ::close(source);
    ::close(target);
    EXPECT_EQ(result.status, CopyResult::Status::SourceEnded);
    EXPECT_EQ(result.copied, 3u);
    EXPECT_NE(result.message(), "");
}

TEST_F(CopyTest, ReadAndWriteErrors)
{
    const int unreadable = ::open(write("source", "abc").c_str(), O_WRONLY);
    const int target = ::open(file("target").c_str(), O_WRONLY | O_CREAT, 0644);
    CopyResult result = copyData(unreadable, target, 3, {}, cancel);
    EXPECT_EQ(result.status, CopyResult::Status::ReadFailed);
    EXPECT_EQ(result.error, EBADF);
    EXPECT_NE(result.message().find(std::strerror(EBADF)), std::string::npos);
    ::close(unreadable);
    ::close(target);

    const int source = ::open(file("source").c_str(), O_RDONLY);
    const int unwritable = ::open(file("target").c_str(), O_RDONLY);
    result = copyData(source, unwritable, 3, {}, cancel);
    EXPECT_EQ(result.status, CopyResult::Status::WriteFailed);
    EXPECT_EQ(result.error, EBADF);
    ::close(unwritable);

    // A full disk says so.
    const int full = ::open("/dev/full", O_WRONLY);
    ASSERT_GE(full, 0);
    result = copyData(source, full, 3, {}, cancel);
    EXPECT_EQ(result.status, CopyResult::Status::WriteFailed);
    EXPECT_EQ(result.error, ENOSPC);
    EXPECT_EQ(result.message(), "The destination is too small.");
    ::close(full);
    ::close(source);
}

TEST_F(CopyTest, Cancels)
{
    const std::string data = noise(20'000);
    const int source = ::open(write("source", data).c_str(), O_RDONLY);
    const int target = ::open(file("target").c_str(), O_WRONLY | O_CREAT, 0644);
    const CopyResult result = copyData(source, target, data.size(), {.bufferSize = 4096}, cancel,
                                       [&](uint64_t copied) {
                                           if (copied >= 8192)
                                               cancel = true;
                                       });
    EXPECT_EQ(result.status, CopyResult::Status::Cancelled);
    EXPECT_EQ(result.copied, 8192u);
    EXPECT_EQ(result.message(), "");

    cancel = true;
    EXPECT_EQ(copyData(source, target, data.size(), {}, cancel).copied, 0u);
    ::close(source);
    ::close(target);
}

TEST_F(CopyTest, JobCopiesOnItsThread)
{
    properdesktop::base::EventLoop loop;
    const std::string data = noise(3 << 20);
    CopyJob job;
    std::optional<CopyResult> result;
    job.onFinished = [&](const CopyResult& r) { result = r; };
    job.start(::open(write("source", data).c_str(), O_RDONLY),
              ::open(file("target").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644), data.size());
    EXPECT_TRUE(job.isRunning());
    ASSERT_TRUE(waitFor([&] { return result.has_value(); }));
    EXPECT_FALSE(job.isRunning());
    EXPECT_TRUE(result->ok());
    EXPECT_EQ(read(file("target")), data);
}

TEST_F(CopyTest, JobCancelsAndStopsWithItsOwner)
{
    properdesktop::base::EventLoop loop;
    // Endless: zeros into nothing.
    constexpr uint64_t kLength = 1ull << 50;
    CopyJob job;
    std::optional<CopyResult> result;
    uint64_t progress = 0;
    job.onProgress = [&](uint64_t copied, uint64_t total) {
        progress = copied;
        EXPECT_EQ(total, kLength);
    };
    job.onFinished = [&](const CopyResult& r) { result = r; };
    job.start(::open("/dev/zero", O_RDONLY), ::open("/dev/null", O_WRONLY), kLength);
    ASSERT_TRUE(waitFor([&] { return progress > 0; }));
    job.cancel();
    ASSERT_TRUE(waitFor([&] { return result.has_value(); }));
    EXPECT_EQ(result->status, CopyResult::Status::Cancelled);

    bool finished = false;
    {
        CopyJob running;
        running.onFinished = [&](const CopyResult&) { finished = true; };
        running.start(::open("/dev/zero", O_RDONLY), ::open("/dev/null", O_WRONLY), kLength);
    }
    loop.dispatch();
    EXPECT_FALSE(finished);
}
