// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/plan_runner.h"
#include "dialogs/volume_format.h"

#include "udisks/describe.h"
#include "udisks/errors.h"
#include "udisks/model.h"

#include "base/dbus_types.h"
#include "base/i18n.h"

#include <algorithm>

#include <fcntl.h>
#include <unistd.h>

using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace dialogs {

static std::string tableName(const std::string& type)
{
    return udisks::schemeName(type);
}

std::string describe(const PlanStep& step)
{
    const std::string size = udisks::formatSize(step.size);
    const std::string type = formatName(step.type);
    switch (step.kind) {
    case PlanStep::Kind::Unmount:
        return format(tr("Unmount “{name}”"), {{"name", step.subject}});
    case PlanStep::Kind::Lock:
        return format(tr("Lock “{name}”"), {{"name", step.subject}});
    case PlanStep::Kind::Delete:
        return format(tr("Delete “{name}”"), {{"name", step.subject}});
    case PlanStep::Kind::ShrinkFilesystem:
        return format(tr("Shrink the filesystem of “{name}” to {size}"), {{"name", step.subject}, {"size", size}});
    case PlanStep::Kind::ResizePartition:
        return format(tr("Resize “{name}” to {size}"), {{"name", step.subject}, {"size", size}});
    case PlanStep::Kind::GrowFilesystem:
        return format(tr("Grow the filesystem of “{name}” to fill its partition"), {{"name", step.subject}});
    case PlanStep::Kind::CreateTable:
        return format(tr("Create a {scheme} on “{name}”"), {{"scheme", tableName(step.type)}, {"name", step.subject}});
    case PlanStep::Kind::Format:
        if (!step.options.passphrase.empty())
            return format(tr("Erase “{name}” as encrypted {format}"), {{"name", step.subject}, {"format", type}});
        return format(tr("Erase “{name}” as {format}"), {{"name", step.subject}, {"format", type}});
    case PlanStep::Kind::Create:
        if (!step.options.passphrase.empty())
            return format(tr("Create “{name}”, encrypted {format}, {size}"),
                          {{"name", step.subject}, {"format", type}, {"size", size}});
        return format(tr("Create “{name}”, {format}, {size}"),
                      {{"name", step.subject}, {"format", type}, {"size", size}});
    case PlanStep::Kind::SetLabel:
        return format(tr("Rename “{name}” to “{label}”"), {{"name", step.subject}, {"label", step.options.label}});
    }
    return {};
}

std::string describeRunning(const PlanStep& step)
{
    switch (step.kind) {
    case PlanStep::Kind::Unmount:
        return format(tr("Unmounting “{name}”…"), {{"name", step.subject}});
    case PlanStep::Kind::Lock:
        return format(tr("Locking “{name}”…"), {{"name", step.subject}});
    case PlanStep::Kind::Delete:
        return format(tr("Deleting “{name}”…"), {{"name", step.subject}});
    case PlanStep::Kind::ShrinkFilesystem:
    case PlanStep::Kind::GrowFilesystem:
        return format(tr("Resizing the filesystem of “{name}”…"), {{"name", step.subject}});
    case PlanStep::Kind::ResizePartition:
        return format(tr("Resizing “{name}”…"), {{"name", step.subject}});
    case PlanStep::Kind::CreateTable:
        return format(tr("Creating the partition map on “{name}”…"), {{"name", step.subject}});
    case PlanStep::Kind::Format:
        return format(tr("Erasing “{name}”…"), {{"name", step.subject}});
    case PlanStep::Kind::Create:
        return format(tr("Creating “{name}”…"), {{"name", step.subject}});
    case PlanStep::Kind::SetLabel:
        return format(tr("Renaming “{name}”…"), {{"name", step.subject}});
    }
    return {};
}

bool stepHolds(const udisks::Model& model, const PlanStep& step)
{
    if (!step.expected.isValid())
        return true;
    const udisks::Identity now = udisks::identityOf(model, step.expected.block);
    if (now == step.expected)
        return true;
    if (step.resizedFrom == 0)
        return false;
    // The plan resized the block itself: the same partition, at the size it
    // had or at about the one asked for, which the partition tools may align.
    udisks::Identity same = now;
    same.size = step.expected.size;
    if (same != step.expected)
        return false;
    const uint64_t asked = step.expected.size;
    const uint64_t slack = asked > now.size ? asked - now.size : now.size - asked;
    return now.size == step.resizedFrom || slack <= kMiB;
}

bool erasesData(const PlanStep& step)
{
    return step.kind == PlanStep::Kind::CreateTable || step.kind == PlanStep::Kind::Format
            || step.kind == PlanStep::Kind::Create;
}

bool dismissed(const udisks::Result& result)
{
    return result.errorName.starts_with("org.freedesktop.UDisks2.Error.NotAuthorized");
}

bool ranPartWay(const udisks::Result& result)
{
    return !result.ok() && !dismissed(result) && result.errorName != udisks::Operations::kSystemError
            && result.errorName != udisks::Operations::kBusyError && result.errorName != PlanRunner::kChangedError;
}

const udisks::Object* jobOf(const udisks::Model& model, const std::vector<std::string>& paths)
{
    for (const std::string& path : paths) {
        for (const udisks::Object* job : model.jobsFor(path)) {
            if (job->job)
                return job;
        }
    }
    return nullptr;
}

PlanRunner::PlanRunner(const udisks::Model& model, udisks::Operations& operations, udisks::Identity subject,
                       std::vector<PlanStep> steps)
    : m_model(model)
    , m_operations(operations)
    , m_subject(std::move(subject))
    , m_steps(std::move(steps))
{
}

void PlanRunner::start()
{
    if (m_running || m_started)
        return;
    m_started = true;
    m_running = true;
    m_current = 0;
    for (size_t i = 0; i < m_steps.size(); ++i) {
        const PlanStep& step = m_steps[i];
        if (!stepHolds(m_model, step)) {
            m_failed = i;
            m_result = {kChangedError,
                        format(tr("“{name}” is no longer the one the changes were made for."), {{"name", step.subject}}),
                        {}};
            m_running = false;
            finished.emit();
            return;
        }
    }
    runStep();
}

void PlanRunner::runStep()
{
    if (m_current >= m_steps.size()) {
        m_running = false;
        finished.emit();
        return;
    }
    if (!m_model.matches(m_subject)) {
        m_failed = m_current;
        m_result = {kChangedError, tr("The disk was removed or replaced by another."), {}};
        m_running = false;
        finished.emit();
        return;
    }
    const PlanStep& step = m_steps[m_current];
    if (!stepHolds(m_model, step)) {
        m_failed = m_current;
        m_result = {kChangedError,
                    format(tr("“{name}” is no longer the one the changes were made for."), {{"name", step.subject}}),
                    {}};
        m_running = false;
        finished.emit();
        return;
    }
    stepStarted.emit(m_current);
    udisks::Done done = [self = m_weakFactory.getWeakPtr()](const udisks::Result& result) {
        PlanRunner* runner = self.get();
        if (!runner)
            return;
        if (!result.ok()) {
            if (!runner->clearStaleTable(result))
                runner->fail(result);
            return;
        }
        runner->m_result = result;
        ++runner->m_current;
        runner->runStep();
    };
    switch (step.kind) {
    case PlanStep::Kind::Unmount:
        m_operations.unmount(step.path, std::move(done));
        break;
    case PlanStep::Kind::Lock:
        m_operations.lock(step.path, std::move(done));
        break;
    case PlanStep::Kind::Delete:
        m_operations.deletePartition(step.path, std::move(done));
        break;
    case PlanStep::Kind::ShrinkFilesystem:
        m_operations.resizeFilesystem(step.path, step.size, std::move(done));
        break;
    case PlanStep::Kind::ResizePartition:
        m_operations.resizePartition(step.path, step.size, std::move(done));
        break;
    case PlanStep::Kind::GrowFilesystem:
        // 0 fills the block.
        m_operations.resizeFilesystem(step.path, 0, std::move(done));
        break;
    case PlanStep::Kind::CreateTable:
        m_operations.format(step.path, step.type, step.options, std::move(done));
        break;
    case PlanStep::Kind::Format:
        m_operations.format(step.path, step.type, step.options, std::move(done));
        break;
    case PlanStep::Kind::Create:
        m_operations.createPartitionAndFormat(step.path, step.offset, step.size, {}, step.partitionName, step.type,
                                              step.options, std::move(done));
        break;
    case PlanStep::Kind::SetLabel:
        m_operations.setLabel(step.path, step.options.label, std::move(done));
        break;
    }
}

void PlanRunner::fail(udisks::Result result)
{
    m_result = std::move(result);
    m_failed = m_current;
    m_running = false;
    finished.emit();
}

// The kernel lists partitions on the disk in a table that no tool reads.
static bool holdsUnknownPartitions(const udisks::Model& model, const std::string& block)
{
    const udisks::Object* object = model.object(block);
    return object && object->partitionTable && object->partitionTable->type.empty()
            && !object->partitionTable->partitions.empty();
}

static int descriptorOf(const udisks::Result& result)
{
    if (result.values.empty())
        return -1;
    const std::optional<properdesktop::base::dbus::UnixFd> fd = result.values.front().get<properdesktop::base::dbus::UnixFd>();
    return fd && fd->isValid() ? fd->release() : -1;
}

// Every partition table the kernel reads starts within it.
static constexpr uint64_t kClearedLength = kMiB;

bool PlanRunner::clearStaleTable(const udisks::Result& result)
{
    const PlanStep& step = m_steps[m_current];
    if (m_clearedTable || step.kind != PlanStep::Kind::CreateTable
        || !udisks::isStaleTableError(result.errorName, result.errorMessage)
        || !holdsUnknownPartitions(m_model, step.path))
        return false;
    m_clearedTable = true;
    const udisks::Object* object = m_model.object(step.path);
    const uint64_t length = std::min(kClearedLength, object->block ? object->block->size : 0);
    m_operations.openForRestore(step.path, [self = m_weakFactory.getWeakPtr(), length](const udisks::Result& opened) {
        PlanRunner* runner = self.get();
        if (!runner)
            return;
        const std::string name = runner->m_steps[runner->m_current].subject;
        if (!opened.ok()) {
            if (dismissed(opened) || opened.cancelled()) {
                runner->fail({kStaleTableError,
                              format(tr("“{name}” still holds data from an earlier startup disk that the system reads "
                                        "as partitions. Clearing it needs an administrator’s password."),
                                     {{"name", name}}),
                              {}});
                return;
            }
            runner->fail({kStaleTableError,
                          format(tr("The leftover data on “{name}” could not be cleared: {reason}"),
                                 {{"name", name}, {"reason", opened.errorMessage}}),
                          {}});
            return;
        }
        const int target = descriptorOf(opened);
        const int zeros = ::open("/dev/zero", O_RDONLY | O_CLOEXEC);
        if (target < 0 || zeros < 0) {
            if (target >= 0)
                ::close(target);
            if (zeros >= 0)
                ::close(zeros);
            runner->fail({kStaleTableError,
                          format(tr("The leftover data on “{name}” could not be cleared."), {{"name", name}}), {}});
            return;
        }
        runner->m_clearingHold = runner->m_operations.hold(runner->m_steps[runner->m_current].path);
        runner->m_clearing = std::make_unique<images::CopyJob>();
        runner->m_clearing->onFinished = [self, name](const images::CopyResult& copied) {
            PlanRunner* runner = self.get();
            if (!runner)
                return;
            runner->m_clearingHold.reset();
            if (!copied.ok()) {
                runner->fail({kStaleTableError,
                              format(tr("The leftover data on “{name}” could not be cleared: {reason}"),
                                     {{"name", name}, {"reason", copied.message()}}),
                              {}});
                return;
            }
            runner->runStep();
        };
        runner->m_clearing->start(zeros, target, length);
    });
    return true;
}

} // namespace dialogs
