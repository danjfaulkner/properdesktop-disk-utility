// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "images/copy.h"
#include "udisks/backend.h"
#include "udisks/identity.h"
#include "udisks/operations.h"

#include "base/signal.h"
#include "base/weak_ptr.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace udisks {
class Model;
struct Object;
}

namespace dialogs {

// One call of an erase or a partition plan.
struct PlanStep
{
    enum class Kind {
        Unmount,
        Lock,
        Delete,
        // To size, before its partition shrinks.
        ShrinkFilesystem,
        ResizePartition,
        // To fill its partition, after it grew.
        GrowFilesystem,
        // A new partition map of type ("gpt", "dos") over the whole disk.
        CreateTable,
        // The block erased to the filesystem type.
        Format,
        // A partition of size at offset in the table at path, formatted to type.
        Create,
        SetLabel,
    };

    Kind kind = Kind::Format;
    // The block, or the partition table for Create.
    std::string path;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::string type;
    // Create on a GUID partition map: the partition's own name.
    std::string partitionName;
    // Format, Create, CreateTable: the label, the erase and the encryption;
    // SetLabel: the label.
    udisks::FormatOptions options;
    // What the summary calls the volume or disk, such as "Data".
    std::string subject;
    // What the block at path must still be when the step starts; not
    // checked while invalid. After the plan's own resize of the block it
    // has the new size, and the size it had (resizedFrom) passes too, in
    // case UDisks2's change has not arrived yet.
    udisks::Identity expected;
    uint64_t resizedFrom = 0;
};

// The block at the step's path is still the one the step was made for.
bool stepHolds(const udisks::Model& model, const PlanStep& step);

// A line of the summary and of the progress: "Delete “Data”",
// "Shrink “Data” to 200 GB"...
std::string describe(const PlanStep& step);
// "Deleting “Data”…", while the step runs.
std::string describeRunning(const PlanStep& step);

// A step whose failure part way can leave the data changed: a format runs
// wipefs before its mkfs, a new partition can stay without its filesystem.
bool erasesData(const PlanStep& step);
// The failed call reached UDisks2 and may have run part way: it was not
// turned down at the password, nor refused before it was sent.
bool ranPartWay(const udisks::Result& result);
// The password was turned down: nothing was sent.
bool dismissed(const udisks::Result& result);

// UDisks2's job on the first of the objects that has one: the step's
// block, then its disk; null when none is running.
const udisks::Object* jobOf(const udisks::Model& model, const std::vector<std::string>& paths);

// Runs the steps one after another through the operations, stopping at the
// first that fails, and before any step once the block the plan was made
// for (a disk, or a volume) or the step's own block is not the one it was.
// Nothing runs unless every step's block is still the expected one.
//
// A new partition map that fails because the kernel still reads partitions
// no tool recognizes, left over from an earlier startup disk, is tried once
// more after the start of the disk is overwritten with zeros, which asks
// for authorization.
class PlanRunner
{
public:
    static constexpr const char* kChangedError = "org.properdesktop.DiskUtility.Error.Changed";
    static constexpr const char* kStaleTableError = "org.properdesktop.DiskUtility.Error.StaleTable";

    PlanRunner(const udisks::Model& model, udisks::Operations& operations, udisks::Identity subject,
               std::vector<PlanStep> steps);

    PlanRunner(const PlanRunner&) = delete;
    PlanRunner& operator=(const PlanRunner&) = delete;

    void start();

    const std::vector<PlanStep>& steps() const { return m_steps; }
    // The step running, or the one that failed; the step count once all ran.
    // Also how many steps ran.
    size_t current() const { return m_current; }
    // The step that failed, or whose block was found changed: a later one
    // than current() when the check before anything ran found it.
    size_t failedStep() const { return m_failed; }
    bool isRunning() const { return m_running; }
    // The last result: the failed step's, or the last step's.
    const udisks::Result& result() const { return m_result; }
    bool succeeded() const { return !m_running && m_started && m_result.ok() && m_current == m_steps.size(); }

    // The index of the step that started.
    properdesktop::base::Signal<size_t> stepStarted;
    // Every step ran, or one failed: result() says which.
    properdesktop::base::Signal<> finished;

private:
    void runStep();
    void fail(udisks::Result result);
    // Starts clearing the step's disk when its failure calls for it.
    bool clearStaleTable(const udisks::Result& result);

    const udisks::Model& m_model;
    udisks::Operations& m_operations;
    udisks::Identity m_subject;
    std::vector<PlanStep> m_steps;
    size_t m_current = 0;
    size_t m_failed = 0;
    bool m_running = false;
    bool m_started = false;
    udisks::Result m_result;
    bool m_clearedTable = false;
    std::unique_ptr<images::CopyJob> m_clearing;
    udisks::Operations::Hold m_clearingHold;
    properdesktop::base::WeakPtrFactory<PlanRunner> m_weakFactory{this};
};

} // namespace dialogs
