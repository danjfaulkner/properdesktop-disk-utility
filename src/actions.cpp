// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "actions.h"

#include "udisks/guard.h"
#include "udisks/model.h"
#include "udisks/operations.h"

#include "base/i18n.h"

#include <algorithm>

using properdesktop::i18n::tr;
using udisks::Change;
using udisks::Model;
using udisks::Node;
using udisks::NodeKind;
using udisks::Object;

static constexpr uint64_t kMiB = 1024 * 1024;

std::string actionTitle(Action action)
{
    switch (action) {
    case Action::View:
        return tr("View");
    case Action::AddVolume:
        return tr("Add Partition…");
    case Action::RemoveVolume:
        return tr("Delete Partition…");
    case Action::FirstAid:
        return tr("First Aid…");
    case Action::Partition:
        return tr("Partition…");
    case Action::Erase:
        return tr("Erase…");
    case Action::Restore:
        return tr("Restore…");
    case Action::Mount:
        return tr("Mount");
    case Action::Unmount:
        return tr("Unmount");
    case Action::Info:
        return tr("Get Info");
    case Action::Eject:
        return tr("Eject");
    case Action::Rename:
        return tr("Rename…");
    case Action::Lock:
        return tr("Lock");
    case Action::AttachImage:
        return tr("Open Disk Image…");
    case Action::NewBlankImage:
        return tr("Blank Image…");
    case Action::NewImageFromSelection:
        return tr("Image from Selection…");
    }
    return {};
}

uint64_t largestFreeSpace(const Model& model, const std::string& disk)
{
    const Object* whole = model.object(disk);
    if (!whole || !whole->block || !whole->partitionTable)
        return 0;
    uint64_t end = whole->block->size > kMiB ? whole->block->size - kMiB : 0;
    // A Master Boot Record numbers 512-byte sectors in 32 bits.
    if (whole->partitionTable->type == "dos")
        end = std::min(end, uint64_t(1) << 41);
    uint64_t cursor = kMiB;
    uint64_t largest = 0;
    const auto gap = [&](uint64_t until) {
        const uint64_t start = (cursor + kMiB - 1) / kMiB * kMiB;
        const uint64_t stop = std::min(until, end) / kMiB * kMiB;
        if (stop > start)
            largest = std::max(largest, stop - start);
    };
    for (const Object* partition : model.partitionsOf(disk)) {
        if (partition->partition->isContained)
            continue;
        gap(partition->partition->offset);
        cursor = std::max(cursor, partition->partition->offset + partition->partition->size);
    }
    gap(end);
    return largest;
}

static void addVolume(const Model& model, const Object* block, std::vector<const Object*>& volumes)
{
    if (!block || !block->block)
        return;
    if (block->encrypted) {
        const Object* clear = model.cleartextOf(block->path);
        volumes.push_back(clear && clear->filesystem ? clear : block);
    } else if (block->filesystem) {
        volumes.push_back(block);
    }
}

std::vector<const Object*> volumesOnDisk(const Model& model, const std::string& disk)
{
    std::vector<const Object*> volumes;
    const Object* whole = model.object(disk);
    if (!whole)
        return volumes;
    addVolume(model, whole, volumes);
    for (const Object* partition : model.partitionsOf(disk))
        addVolume(model, partition, volumes);
    return volumes;
}

std::string actionName(Action action)
{
    switch (action) {
    case Action::View:
        return "view";
    case Action::AddVolume:
        return "addVolume";
    case Action::RemoveVolume:
        return "removeVolume";
    case Action::FirstAid:
        return "firstAid";
    case Action::Partition:
        return "partition";
    case Action::Erase:
        return "erase";
    case Action::Restore:
        return "restore";
    case Action::Mount:
        return "mount";
    case Action::Unmount:
        return "unmount";
    case Action::Info:
        return "info";
    case Action::Eject:
        return "eject";
    case Action::Rename:
        return "rename";
    case Action::Lock:
        return "lock";
    case Action::AttachImage:
        return "attachImage";
    case Action::NewBlankImage:
        return "newBlankImage";
    case Action::NewImageFromSelection:
        return "newImageFromSelection";
    }
    return {};
}

static bool isMounted(const Object* block)
{
    return block && block->filesystem && !block->filesystem->mountPoints.empty();
}

static bool isLocked(const Model& model, const Object* block)
{
    return block && block->encrypted && !model.cleartextOf(block->path);
}

static bool isBusy(const Model& model, const udisks::Operations* operations, const Node& node)
{
    if (operations && (operations->isBusy(node.block) || operations->isBusy(node.disk)))
        return true;
    return !model.jobsFor(node.block).empty() || !model.jobsFor(node.disk).empty()
            || (!node.drive.empty() && !model.jobsFor(node.drive).empty());
}

static ActionState enabled()
{
    return {true, {}};
}

static ActionState disabled(std::string reason = {})
{
    return {false, std::move(reason)};
}

static ActionState allowedUnless(std::string refusal)
{
    return refusal.empty() ? enabled() : disabled(std::move(refusal));
}

ActionState actionState(Action action, const Model& model, const udisks::Operations* operations,
                        const Selection& selection)
{
    switch (action) {
    case Action::View:
    case Action::AttachImage:
    case Action::NewBlankImage:
        return enabled();
    default:
        break;
    }

    if (!selection)
        return disabled();
    const Node& node = *selection;
    const Object* object = model.object(node.block);
    const Object* disk = model.object(node.disk);
    if (!object || !object->block || !disk || !disk->block)
        return disabled();
    if (action == Action::Info)
        return enabled();

    const bool busy = isBusy(model, operations, node);
    const bool readOnly = object->block->readOnly || disk->block->readOnly;
    const Object* content = model.contentOf(node.block);
    const bool isDisk = node.kind == NodeKind::Disk;

    if (busy)
        return disabled(tr("An operation on this disk is in progress."));

    switch (action) {
    case Action::Mount: {
        if (isDisk) {
            for (const Object* volume : volumesOnDisk(model, node.disk)) {
                if (isLocked(model, volume) || (volume->filesystem && !isMounted(volume)))
                    return enabled();
            }
            return disabled();
        }
        if (node.kind != NodeKind::Volume)
            return disabled();
        if (isLocked(model, object) || (content && content->filesystem && !isMounted(content)))
            return enabled();
        return disabled();
    }
    case Action::Unmount: {
        if (isDisk) {
            bool any = false;
            for (const Object* volume : volumesOnDisk(model, node.disk)) {
                if (isMounted(volume)) {
                    if (const std::string why = systemRefusal(model, volume->path, Change::Volume); !why.empty())
                        return disabled(why);
                    any = true;
                }
            }
            return any ? enabled() : disabled();
        }
        if (node.kind != NodeKind::Volume)
            return disabled();
        const bool unlocked = object->encrypted && content != object;
        if (!isMounted(content) && !unlocked)
            return disabled();
        return allowedUnless(systemRefusal(model, node.block, Change::Volume));
    }
    case Action::Eject: {
        if (const std::string why = systemRefusal(model, node.disk, Change::Drive); !why.empty())
            return disabled(why);
        if (disk->loop)
            return enabled();
        const Object* drive = model.object(node.drive);
        if (drive && drive->drive && (drive->drive->ejectable || drive->drive->canPowerOff))
            return enabled();
        return disabled();
    }
    case Action::Erase:
    case Action::Restore: {
        if (node.kind == NodeKind::Container)
            return disabled();
        // Erasing a partition formats it in place; erasing a disk, or
        // restoring onto anything, changes what the disk holds.
        const Change change = action == Action::Erase ? udisks::formatChange(model, node.block) : Change::Table;
        if (const std::string why = systemRefusal(model, node.block, change); !why.empty()) {
            // A restored copy keeps its source's filesystem UUID: a second
            // one beside the system's can start the wrong one at boot.
            if (action == Action::Restore && model.systemDevices().known && !model.holdsSystem(node.block))
                return disabled(tr("Nothing is restored onto the system's disk: the copy would bring another "
                                   "volume's identity, and the computer could start from the wrong one."));
            return disabled(why);
        }
        if (readOnly || object->block->size == 0)
            return disabled();
        return enabled();
    }
    case Action::Partition:
        if (const std::string why = systemRefusal(model, node.disk, Change::Table); !why.empty())
            return disabled(why);
        if (readOnly || disk->block->size == 0)
            return disabled();
        return enabled();
    case Action::AddVolume:
        if (const std::string why = systemRefusal(model, node.disk, Change::Table); !why.empty())
            return disabled(why);
        if (readOnly)
            return disabled();
        if (disk->partitionTable && disk->partitionTable->type == "dos") {
            size_t primaries = 0;
            for (const Object* partition : model.partitionsOf(node.disk)) {
                if (!partition->partition->isContained)
                    ++primaries;
            }
            if (primaries >= 4)
                return disabled(tr("A Master Boot Record holds at most four partitions."));
        }
        if (largestFreeSpace(model, node.disk) == 0)
            return disabled(disk->partitionTable ? tr("There is no free space for another partition.") : std::string());
        return enabled();
    case Action::RemoveVolume:
        if (!object->partition)
            return disabled();
        if (const std::string why = systemRefusal(model, node.block, Change::Table); !why.empty())
            return disabled(why);
        if (object->partition->isContainer) {
            for (const Object* logical : model.logicalPartitionsOf(node.block)) {
                if (model.isInUse(logical->path) || model.cleartextOf(logical->path))
                    return disabled(tr("A partition in it is mounted or unlocked."));
            }
        }
        return readOnly ? disabled() : enabled();
    case Action::FirstAid:
        // On a disk, every volume of it, the system's too.
        if (isDisk)
            return allowedUnless(systemRefusal(model, node.disk, Change::Table));
        if (node.kind != NodeKind::Volume || !content || !content->filesystem)
            return disabled();
        return allowedUnless(systemRefusal(model, node.block, Change::Volume));
    case Action::Rename:
        if (node.kind != NodeKind::Volume || !content || !content->filesystem)
            return disabled();
        if (const std::string why = systemRefusal(model, node.block, Change::Volume); !why.empty())
            return disabled(why);
        return readOnly ? disabled() : enabled();
    case Action::NewImageFromSelection:
        return node.kind == NodeKind::Container ? disabled() : enabled();
    case Action::Lock:
        if (node.kind != NodeKind::Volume || !object->encrypted || !content || content == object)
            return disabled();
        return allowedUnless(systemRefusal(model, node.block, Change::Volume));
    default:
        return disabled();
    }
}

ActionDispatcher::ActionDispatcher(Model& model, udisks::Operations& operations)
    : m_model(model)
    , m_operations(operations)
{
    m_subscriptions.push_back(model.changed.connect([this] { statesChanged.emit(); }));
    m_subscriptions.push_back(operations.busyChanged.connect([this] { statesChanged.emit(); }));
}

void ActionDispatcher::setHandler(Action action, Handler handler)
{
    std::erase_if(m_handlers, [action](const auto& entry) { return entry.first == action; });
    if (handler)
        m_handlers.emplace_back(action, std::move(handler));
    statesChanged.emit();
}

bool ActionDispatcher::hasHandler(Action action) const
{
    return std::any_of(m_handlers.begin(), m_handlers.end(),
                       [action](const auto& entry) { return entry.first == action; });
}

void ActionDispatcher::setSelection(Selection selection)
{
    if (selection == m_selection)
        return;
    m_selection = std::move(selection);
    statesChanged.emit();
}

void ActionDispatcher::setRefinement(Action action, Refinement refinement)
{
    std::erase_if(m_refinements, [action](const auto& entry) { return entry.first == action; });
    if (refinement)
        m_refinements.emplace_back(action, std::move(refinement));
    statesChanged.emit();
}

ActionState ActionDispatcher::stateFor(Action action, const Selection& selection) const
{
    if (!hasHandler(action))
        return {};
    const ActionState state = actionState(action, m_model, &m_operations, selection);
    if (!state.enabled)
        return state;
    for (const auto& [candidate, refinement] : m_refinements) {
        if (candidate == action)
            return refinement(selection);
    }
    return state;
}

bool ActionDispatcher::triggerOn(Action action, const Selection& selection)
{
    if (!stateFor(action, selection).enabled)
        return false;
    for (const auto& [candidate, handler] : m_handlers) {
        if (candidate == action) {
            // The handler may replace itself, or the selection; it runs from
            // copies.
            const Handler run = handler;
            const Selection node = selection;
            run(node);
            return true;
        }
    }
    return false;
}
