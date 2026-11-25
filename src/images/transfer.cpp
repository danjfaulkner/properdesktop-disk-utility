// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "images/transfer.h"
#include "images/imagefile.h"

#include "udisks/describe.h"
#include "udisks/model.h"
#include "udisks/operations.h"

#include "base/dbus_types.h"
#include "base/i18n.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using properdesktop::base::dbus::UnixFd;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;
using udisks::Model;
using udisks::Object;

namespace images {

// The block and, for a disk, every partition on it.
static std::vector<const Object*> blocksOn(const Model& model, const std::string& block)
{
    std::vector<const Object*> blocks;
    const Object* object = model.object(block);
    if (!object || !object->block)
        return blocks;
    blocks.push_back(object);
    if (object->partitionTable) {
        for (const Object* partition : model.partitionsOf(block))
            blocks.push_back(partition);
    }
    return blocks;
}

std::vector<std::string> mountedOn(const Model& model, const std::string& block)
{
    std::vector<std::string> mounted;
    for (const Object* object : blocksOn(model, block)) {
        const Object* content = model.contentOf(object->path);
        for (const Object* candidate : {object, content}) {
            if (candidate && candidate->filesystem && !candidate->filesystem->mountPoints.empty()
                && std::find(mounted.begin(), mounted.end(), candidate->path) == mounted.end())
                mounted.push_back(candidate->path);
        }
    }
    return mounted;
}

std::vector<std::string> unlockedOn(const Model& model, const std::string& block)
{
    std::vector<std::string> unlocked;
    for (const Object* object : blocksOn(model, block)) {
        if (object->encrypted && model.cleartextOf(object->path))
            unlocked.push_back(object->path);
    }
    return unlocked;
}

std::optional<uint64_t> sizeOf(const Model& model, const Endpoint& endpoint)
{
    if (endpoint.isBlock()) {
        const Object* object = model.object(endpoint.block);
        if (!object || !object->block)
            return std::nullopt;
        return object->block->size;
    }
    struct stat info {};
    if (::stat(endpoint.file.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return std::nullopt;
    return uint64_t(info.st_size);
}

// The block and what it lies inside: its disk for a partition, the
// encrypted block for a cleartext one, and theirs in turn.
static std::vector<std::string> enclosing(const Model& model, std::string block)
{
    std::vector<std::string> chain;
    while (!block.empty() && std::find(chain.begin(), chain.end(), block) == chain.end()) {
        chain.push_back(block);
        const Object* object = model.object(block);
        if (!object || !object->block)
            break;
        if (!object->block->cryptoBackingDevice.empty())
            block = object->block->cryptoBackingDevice;
        else if (object->partition)
            block = object->partition->table;
        else
            break;
    }
    return chain;
}

bool overlaps(const Model& model, const std::string& block, const std::string& other)
{
    const std::vector<std::string> blockChain = enclosing(model, block);
    const std::vector<std::string> otherChain = enclosing(model, other);
    return std::find(otherChain.begin(), otherChain.end(), block) != otherChain.end()
        || std::find(blockChain.begin(), blockChain.end(), other) != blockChain.end();
}

std::string restoreProblem(const Model& model, const Endpoint& source, const std::string& target)
{
    const Object* destination = model.object(target);
    if (!destination || !destination->block)
        return tr("The destination is no longer there.");
    if (source.isBlock()) {
        if (source.block == target)
            return tr("A volume cannot be restored from itself.");
        if (overlaps(model, source.block, target))
            return tr("The source and the destination overlap: one lies inside the other.");
    } else if (const std::string compression = compressionOf(source.file); !compression.empty()) {
        // Written as it is, it would be the compressed bytes on the disk.
        return format(tr("The file is compressed ({format}). Decompress it first."), {{"format", compression}});
    } else if (const std::string loop = loopOfFile(model, source.file); !loop.empty()) {
        // An attached image changes under its mounted volumes, and restoring
        // it onto its own loop device copies it onto itself.
        if (overlaps(model, loop, target))
            return tr("A disk image cannot be restored onto itself.");
        if (!mountedOn(model, loop).empty())
            return tr("The disk image is attached and mounted. Eject it first.");
    }
    const std::optional<uint64_t> size = sizeOf(model, source);
    if (!size)
        return tr("The source could not be read.");
    if (*size > destination->block->size)
        return format(tr("The source ({source}) is larger than the destination ({destination})."),
                      {{"source", udisks::formatSize(*size)},
                       {"destination", udisks::formatSize(destination->block->size)}});
    if (*size == 0)
        return tr("The source is empty.");
    return {};
}

bool isOnFilesystemOf(const Model& model, const std::string& block, const std::string& path)
{
    std::error_code error;
    const std::filesystem::path folder = std::filesystem::weakly_canonical(
            std::filesystem::path(path).parent_path(), error);
    if (error)
        return false;
    for (const std::string& mounted : mountedOn(model, block)) {
        const Object* object = model.object(mounted);
        for (const std::string& point : object->filesystem->mountPoints) {
            const std::filesystem::path relative = folder.lexically_relative(point);
            if (!relative.empty() && *relative.begin() != "..")
                return true;
        }
    }
    return false;
}

Transfer::Transfer(Model& model, udisks::Operations& operations)
    : m_model(model)
    , m_operations(operations)
{
}

Transfer::~Transfer()
{
    m_job.reset();
    closeFiles();
    if (m_running && m_createdTarget)
        ::unlink(m_target.file.c_str());
}

template<typename F>
auto Transfer::guard(F f)
{
    return [this, alive = std::weak_ptr<bool>(m_alive), f = std::move(f)](const udisks::Result& result) {
        if (alive.expired())
            return;
        if (m_cancelled || result.cancelled()) {
            closeFiles();
            Outcome outcome;
            outcome.cancelled = true;
            finish(outcome);
            return;
        }
        f(result);
    };
}

Endpoint Endpoint::ofBlock(const udisks::Model& model, std::string block)
{
    Endpoint endpoint{std::move(block), {}, {}};
    endpoint.identity = udisks::identityOf(model, endpoint.block);
    return endpoint;
}

bool Transfer::stillTheSame()
{
    for (const Endpoint* endpoint : {&m_source, &m_target}) {
        if (endpoint->isBlock() && !m_model.matches(endpoint->identity)) {
            fail(tr("The disk was removed or replaced by another."));
            return false;
        }
    }
    return true;
}

void Transfer::start(Endpoint source, Endpoint target)
{
    if (m_running)
        return;
    m_running = true;
    m_source = std::move(source);
    m_target = std::move(target);
    for (Endpoint* endpoint : {&m_source, &m_target}) {
        if (endpoint->isBlock() && !endpoint->identity.isValid())
            endpoint->identity = udisks::identityOf(m_model, endpoint->block);
    }
    if (!stillTheSame())
        return;

    const std::optional<uint64_t> length = sizeOf(m_model, m_source);
    if (!length || *length == 0) {
        fail(tr("The source could not be read."));
        return;
    }
    m_length = *length;
    if (m_target.isBlock()) {
        const std::string problem = restoreProblem(m_model, m_source, m_target.block);
        if (!problem.empty()) {
            fail(problem);
            return;
        }
    }

    for (const Endpoint* endpoint : {&m_source, &m_target}) {
        if (!endpoint->isBlock())
            continue;
        for (const std::string& mounted : mountedOn(m_model, endpoint->block))
            m_releases.emplace_back(false, mounted);
        for (const std::string& unlocked : unlockedOn(m_model, endpoint->block))
            m_releases.emplace_back(true, unlocked);
    }
    std::stable_partition(m_releases.begin(), m_releases.end(), [](const auto& release) { return !release.first; });
    if (!m_releases.empty() && onStep)
        onStep(Step::Unmounting);
    release(0);
}

void Transfer::release(size_t index)
{
    if (index >= m_releases.size()) {
        openSource();
        return;
    }
    auto next = guard([this, index](const udisks::Result& result) {
        if (!result.ok()) {
            fail(result);
            return;
        }
        release(index + 1);
    });
    const auto& [lock, block] = m_releases[index];
    if (lock)
        m_operations.lock(block, std::move(next));
    else
        m_operations.unmount(block, std::move(next));
}

static int descriptorOf(const udisks::Result& result)
{
    if (result.values.empty())
        return -1;
    const std::optional<UnixFd> fd = result.values.front().get<UnixFd>();
    return fd && fd->isValid() ? fd->release() : -1;
}

void Transfer::openSource()
{
    if (!stillTheSame())
        return;
    if (onStep)
        onStep(Step::Opening);
    if (!m_source.isBlock()) {
        m_sourceFd = ::open(m_source.file.c_str(), O_RDONLY | O_CLOEXEC);
        if (m_sourceFd < 0) {
            fail(format(tr("The source could not be opened: {reason}"), {{"reason", std::strerror(errno)}}));
            return;
        }
        openTarget();
        return;
    }
    m_operations.openForBackup(m_source.block, guard([this](const udisks::Result& result) {
        if (!result.ok()) {
            fail(result);
            return;
        }
        m_sourceFd = descriptorOf(result);
        if (m_sourceFd < 0) {
            fail(tr("The source could not be opened."));
            return;
        }
        openTarget();
    }));
}

void Transfer::openTarget()
{
    if (!stillTheSame())
        return;
    if (!m_target.isBlock()) {
        m_targetFd = ::open(m_target.file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (m_targetFd < 0) {
            fail(format(tr("The destination could not be opened: {reason}"), {{"reason", std::strerror(errno)}}));
            return;
        }
        m_createdTarget = true;
        copy();
        return;
    }
    m_operations.openForRestore(m_target.block, guard([this](const udisks::Result& result) {
        if (!result.ok()) {
            fail(result);
            return;
        }
        m_targetFd = descriptorOf(result);
        if (m_targetFd < 0) {
            fail(tr("The destination could not be opened."));
            return;
        }
        copy();
    }));
}

void Transfer::copy()
{
    for (const Endpoint* endpoint : {&m_source, &m_target}) {
        if (endpoint->isBlock())
            m_holds.push_back(m_operations.hold(endpoint->block));
    }
    if (onStep)
        onStep(Step::Copying);
    m_job = std::make_unique<CopyJob>();
    m_job->onProgress = [this](uint64_t copied, uint64_t total) {
        if (onProgress)
            onProgress(copied, total);
    };
    m_job->onFinished = [this](const CopyResult& result) {
        Outcome outcome;
        outcome.ok = result.ok();
        outcome.cancelled = result.status == CopyResult::Status::Cancelled;
        outcome.error = result.message();
        outcome.copied = result.copied;
        if (!outcome.ok && m_createdTarget)
            ::unlink(m_target.file.c_str());
        m_createdTarget = false;
        finish(outcome);
    };
    CopyOptions options;
    options.sparse = !m_target.isBlock();
    m_job->start(m_sourceFd, m_targetFd, m_length, options);
    m_sourceFd = -1;
    m_targetFd = -1;
    if (m_cancelled)
        m_job->cancel();
}

void Transfer::cancel()
{
    m_cancelled = true;
    if (m_job)
        m_job->cancel();
}

void Transfer::fail(const udisks::Result& result)
{
    fail(result.errorMessage.empty() ? result.errorName : result.errorMessage);
}

void Transfer::fail(std::string error)
{
    closeFiles();
    if (m_createdTarget) {
        ::unlink(m_target.file.c_str());
        m_createdTarget = false;
    }
    Outcome outcome;
    outcome.error = std::move(error);
    finish(outcome);
}

void Transfer::finish(Outcome outcome)
{
    closeFiles();
    m_holds.clear();
    m_running = false;
    if (onFinished)
        onFinished(outcome);
}

void Transfer::closeFiles()
{
    for (int* fd : {&m_sourceFd, &m_targetFd}) {
        if (*fd >= 0)
            ::close(*fd);
        *fd = -1;
    }
}

} // namespace images
