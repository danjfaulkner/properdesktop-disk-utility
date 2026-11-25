// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/partition_plan.h"
#include "dialogs/volume_format.h"

#include "udisks/model.h"

#include "base/i18n.h"

#include <algorithm>

using properdesktop::i18n::format;
using properdesktop::i18n::tr;
using udisks::Model;
using udisks::Object;

namespace dialogs {

static constexpr uint64_t kTwoTiB = uint64_t(1) << 41;

std::vector<FreeRegion> freeRegions(const Model& model, const std::string& disk)
{
    std::vector<FreeRegion> regions;
    const Object* whole = model.object(disk);
    if (!whole || !whole->block || !whole->partitionTable)
        return regions;
    uint64_t end = whole->block->size > kMiB ? whole->block->size - kMiB : 0;
    if (whole->partitionTable->type == "dos")
        end = std::min(end, kTwoTiB);
    uint64_t cursor = kMiB;
    const auto gap = [&](uint64_t until) {
        const uint64_t start = alignUp(cursor);
        const uint64_t stop = alignDown(std::min(until, end));
        if (stop > start)
            regions.push_back({start, stop - start});
    };
    for (const Object* partition : model.partitionsOf(disk)) {
        if (partition->partition->isContained)
            continue;
        gap(partition->partition->offset);
        cursor = std::max(cursor, partition->partition->offset + partition->partition->size);
    }
    gap(end);
    return regions;
}

uint64_t usableStart()
{
    return kMiB;
}

uint64_t usableEnd(uint64_t diskSize, const std::string& tableType)
{
    uint64_t end = diskSize > 2 * kMiB ? alignDown(diskSize - kMiB) : 0;
    // 32-bit sector numbers of 512 bytes.
    if (tableType == "dos")
        end = std::min(end, kTwoTiB);
    return std::max(end, usableStart());
}

size_t maximumPartitions(const std::string& tableType)
{
    return tableType == "dos" ? 4 : 128;
}

static udisks::FormatOptions formatOptionsFor(const std::string& filesystem, const std::string& name)
{
    udisks::FormatOptions options;
    options.label = normalizedLabel(filesystem, name);
    options.takeOwnership = hasOwners(filesystem);
    return options;
}

std::vector<PlanStep> teardownSteps(const Model& model, const Object& block, const std::string& subject)
{
    std::vector<PlanStep> steps;
    const auto mounted = [](const Object* object) {
        return object && object->filesystem && !object->filesystem->mountPoints.empty();
    };
    if (block.encrypted) {
        const Object* clear = model.cleartextOf(block.path);
        if (!clear)
            return steps;
        if (mounted(clear))
            steps.push_back({.kind = PlanStep::Kind::Unmount, .path = clear->path, .subject = subject,
                             .expected = udisks::identityOf(model, clear->path)});
        steps.push_back({.kind = PlanStep::Kind::Lock, .path = block.path, .subject = subject,
                         .expected = udisks::identityOf(model, block.path)});
    } else if (mounted(&block)) {
        steps.push_back({.kind = PlanStep::Kind::Unmount, .path = block.path, .subject = subject,
                         .expected = udisks::identityOf(model, block.path)});
    }
    return steps;
}

static Slice partitionSlice(const Model& model, const Object& partition, const std::string& disk,
                            const udisks::StatFunction& stat)
{
    Slice slice;
    slice.kind = partition.partition->isContainer ? Slice::Kind::Container : Slice::Kind::Partition;
    slice.block = partition.path;
    slice.offset = partition.partition->offset;
    slice.size = partition.partition->size;
    slice.originalSize = slice.size;
    slice.encrypted = partition.encrypted.has_value();

    const Object* content = model.contentOf(partition.path);
    const Object* holder = content && content->block ? content : &partition;
    slice.contentBlock = holder->path;
    slice.hasFilesystem = holder->filesystem.has_value();
    slice.identity = udisks::identityOf(model, partition.path);
    slice.contentIdentity = udisks::identityOf(model, holder->path);
    slice.filesystem = holder->block->idType;
    slice.name = holder->block->idLabel;
    slice.originalFilesystem = slice.filesystem;
    slice.originalName = slice.name;
    if (holder->filesystem && !holder->filesystem->mountPoints.empty()) {
        slice.mounted = true;
        if (stat) {
            if (const auto stats = stat(holder->filesystem->mountPoints.front()))
                slice.used = (stats->blocks - std::min(stats->blocks, stats->free)) * stats->fragmentSize;
        }
    }

    udisks::Node node;
    node.block = partition.path;
    node.disk = disk;
    if (slice.kind == Slice::Kind::Container)
        node.kind = udisks::NodeKind::Container;
    else if (holder->filesystem || partition.encrypted)
        node.kind = udisks::NodeKind::Volume;
    else
        node.kind = udisks::NodeKind::Partition;
    slice.displayName = udisks::displayName(model, node);
    return slice;
}

PartitionLayout PartitionLayout::fromDisk(const Model& model, const std::string& disk,
                                          const udisks::StatFunction& stat)
{
    PartitionLayout layout;
    layout.m_disk = disk;
    const Object* whole = model.object(disk);
    if (!whole || !whole->block)
        return layout;

    udisks::Node node;
    node.kind = udisks::NodeKind::Disk;
    node.block = disk;
    node.disk = disk;
    node.drive = whole->block->drive;
    layout.m_diskName = udisks::displayName(model, node);
    layout.m_createsTable = !whole->partitionTable;
    layout.m_tableType = whole->partitionTable ? whole->partitionTable->type : "gpt";
    layout.m_start = usableStart();
    layout.m_end = usableEnd(whole->block->size, layout.m_tableType);

    if (layout.m_createsTable) {
        std::vector<PlanStep>& teardown = layout.m_teardown[disk];
        teardown = teardownSteps(model, *whole, layout.m_diskName);
        if (layout.m_end > layout.m_start)
            layout.insert(0, {.kind = Slice::Kind::Free, .offset = layout.m_start,
                              .size = layout.m_end - layout.m_start});
        return layout;
    }

    uint64_t cursor = layout.m_start;
    const auto gap = [&](uint64_t until) {
        const uint64_t start = alignUp(cursor);
        const uint64_t stop = alignDown(std::min(until, layout.m_end));
        if (stop > start)
            layout.insert(layout.m_slices.size(), {.kind = Slice::Kind::Free, .offset = start, .size = stop - start});
    };
    for (const Object* partition : model.partitionsOf(disk)) {
        if (partition->partition->isContained)
            continue;
        gap(partition->partition->offset);
        Slice slice = partitionSlice(model, *partition, disk, stat);
        layout.m_teardown[slice.block] = teardownSteps(model, *partition, slice.displayName);
        layout.insert(layout.m_slices.size(), std::move(slice));
        cursor = std::max(cursor, partition->partition->offset + partition->partition->size);
    }
    gap(layout.m_end);
    return layout;
}

uint32_t PartitionLayout::insert(size_t index, Slice slice)
{
    slice.id = m_nextId++;
    const uint32_t id = slice.id;
    m_slices.insert(m_slices.begin() + ptrdiff_t(std::min(index, m_slices.size())), std::move(slice));
    return id;
}

const Slice* PartitionLayout::slice(uint32_t id) const
{
    const std::optional<size_t> index = indexOf(id);
    return index ? &m_slices[*index] : nullptr;
}

std::optional<size_t> PartitionLayout::indexOf(uint32_t id) const
{
    for (size_t i = 0; i < m_slices.size(); ++i) {
        if (m_slices[i].id == id)
            return i;
    }
    return std::nullopt;
}

void PartitionLayout::setResizeSupport(const std::string& filesystem, bool available, uint64_t modes,
                                       const std::string& tool)
{
    m_resize[filesystem] = {available, modes, tool};
}

std::vector<std::string> PartitionLayout::resizableFilesystems() const
{
    std::vector<std::string> types;
    for (const Slice& slice : m_slices) {
        if (slice.kind != Slice::Kind::Partition || slice.encrypted || slice.originalFilesystem.empty())
            continue;
        if (std::find(types.begin(), types.end(), slice.originalFilesystem) == types.end())
            types.push_back(slice.originalFilesystem);
    }
    return types;
}

static ActionState allowed()
{
    return {true, {}};
}

static ActionState refused(std::string reason)
{
    return {false, std::move(reason)};
}

// shrink: with the offline and online flags for shrinking, else growing.
static ActionState resizeState(const Slice& slice, const PartitionLayout::ResizeSupport* support, bool shrink)
{
    if (slice.kind == Slice::Kind::New || slice.reformats())
        return allowed();
    if (slice.kind != Slice::Kind::Partition)
        return refused({});
    if (slice.encrypted)
        return refused(tr("Encrypted volumes can't be resized here."));
    const std::string name = formatName(slice.originalFilesystem);
    if (slice.originalFilesystem.empty() || slice.originalFilesystem == "swap")
        return refused(tr("The partition has no filesystem that can be resized."));
    if (!support)
        return refused(tr("Checking whether the filesystem can be resized…"));
    if (!support->available) {
        if (!support->tool.empty())
            return refused(format(tr("Resizing {format} needs {tool}, which is not installed."),
                                  {{"format", name}, {"tool", support->tool}}));
        return refused(format(tr("{format} can't be resized."), {{"format", name}}));
    }
    // libblockdev's BDFSResizeFlags, which UDisks2 passes on.
    const uint64_t offline = shrink ? 2 : 4;
    const uint64_t online = shrink ? 8 : 16;
    const uint64_t modes = support->modes;
    const bool possible = slice.mounted ? (modes & online) : (modes & offline);
    if (possible)
        return allowed();
    if (slice.mounted && (modes & offline))
        return refused(format(shrink ? tr("Unmount “{name}” to shrink it.") : tr("Unmount “{name}” to grow it."),
                              {{"name", slice.displayName}}));
    if (!slice.mounted && (modes & online))
        return refused(format(shrink ? tr("Mount “{name}” to shrink it.") : tr("Mount “{name}” to grow it."),
                              {{"name", slice.displayName}}));
    return refused(format(shrink ? tr("{format} can't shrink.") : tr("{format} can't grow."), {{"format", name}}));
}

ActionState PartitionLayout::shrinkState(uint32_t id) const
{
    const Slice* found = slice(id);
    if (!found)
        return refused({});
    const auto it = m_resize.find(found->originalFilesystem);
    return resizeState(*found, it == m_resize.end() ? nullptr : &it->second, true);
}

ActionState PartitionLayout::growState(uint32_t id) const
{
    const Slice* found = slice(id);
    if (!found)
        return refused({});
    const auto it = m_resize.find(found->originalFilesystem);
    return resizeState(*found, it == m_resize.end() ? nullptr : &it->second, false);
}

uint64_t PartitionLayout::minimumSizeOf(const Slice& slice) const
{
    switch (slice.kind) {
    case Slice::Kind::Free:
        return 0;
    case Slice::Kind::Container:
        return slice.size;
    case Slice::Kind::New:
        return std::max(minimumSize(slice.filesystem), kMiB);
    case Slice::Kind::Partition:
        break;
    }
    if (slice.reformats())
        return std::max(minimumSize(slice.filesystem), kMiB);
    return std::max(alignUp(slice.used), minimumSize(slice.filesystem));
}

size_t PartitionLayout::partitionCount() const
{
    return size_t(std::count_if(m_slices.begin(), m_slices.end(),
                                [](const Slice& slice) { return slice.kind != Slice::Kind::Free; }));
}

std::pair<uint64_t, uint64_t> PartitionLayout::endRange(size_t index) const
{
    const Slice& slice = m_slices[index];
    const Slice* next = index + 1 < m_slices.size() ? &m_slices[index + 1] : nullptr;
    const bool nextMovable = next && (next->kind == Slice::Kind::Free || next->kind == Slice::Kind::New);

    uint64_t low = slice.end();
    switch (slice.kind) {
    case Slice::Kind::Free:
        if (nextMovable)
            low = slice.offset;
        break;
    case Slice::Kind::New:
        low = slice.offset + minimumSizeOf(slice);
        break;
    case Slice::Kind::Partition:
        if (shrinkState(slice.id).enabled)
            low = slice.offset + minimumSizeOf(slice);
        break;
    case Slice::Kind::Container:
        break;
    }
    low = std::min(low, slice.end());

    uint64_t high = slice.end();
    if (!next) {
        high = std::max(slice.end(), m_end);
    } else if (next->kind == Slice::Kind::Free) {
        high = next->end();
    } else if (next->kind == Slice::Kind::New) {
        high = std::max(next->offset, next->end() - std::min(next->size, minimumSizeOf(*next)));
    }
    if (slice.kind == Slice::Kind::Container
        || (slice.kind == Slice::Kind::Partition && !growState(slice.id).enabled)
        || (slice.kind == Slice::Kind::Free && !nextMovable))
        high = slice.end();
    high = std::max(high, slice.end());
    return {low, high};
}

bool PartitionLayout::canMoveBoundary(size_t index) const
{
    if (index >= m_slices.size())
        return false;
    const auto [low, high] = endRange(index);
    return high >= low + kMiB;
}

std::pair<uint64_t, uint64_t> PartitionLayout::boundaryRange(size_t index) const
{
    if (index >= m_slices.size())
        return {0, 0};
    return endRange(index);
}

void PartitionLayout::moveBoundary(size_t index, uint64_t position)
{
    if (index >= m_slices.size())
        return;
    const auto [low, high] = endRange(index);
    uint64_t target = (position + kMiB / 2) / kMiB * kMiB;
    target = std::clamp(target, low, high);
    Slice& slice = m_slices[index];
    const uint64_t oldEnd = slice.end();
    if (target == oldEnd)
        return;
    slice.size = target - slice.offset;
    Slice* next = index + 1 < m_slices.size() ? &m_slices[index + 1] : nullptr;
    if (next && (next->kind == Slice::Kind::Free || next->kind == Slice::Kind::New)) {
        const uint64_t nextEnd = next->end();
        next->offset = target;
        next->size = nextEnd - target;
    } else if (target < oldEnd) {
        insert(index + 1, {.kind = Slice::Kind::Free, .offset = target, .size = oldEnd - target});
    }
}

std::pair<uint64_t, uint64_t> PartitionLayout::sizeRange(uint32_t id) const
{
    const std::optional<size_t> index = indexOf(id);
    if (!index)
        return {0, 0};
    const auto [low, high] = endRange(*index);
    const uint64_t offset = m_slices[*index].offset;
    return {low - offset, high - offset};
}

void PartitionLayout::setSize(uint32_t id, uint64_t size)
{
    const std::optional<size_t> index = indexOf(id);
    if (!index || m_slices[*index].kind == Slice::Kind::Free)
        return;
    moveBoundary(*index, m_slices[*index].offset + size);
    normalize();
}

void PartitionLayout::setName(uint32_t id, const std::string& name)
{
    const std::optional<size_t> index = indexOf(id);
    if (index && m_slices[*index].isEditable())
        m_slices[*index].name = name;
}

void PartitionLayout::setFilesystem(uint32_t id, const std::string& filesystem)
{
    const std::optional<size_t> index = indexOf(id);
    if (!index || !m_slices[*index].isEditable())
        return;
    Slice& slice = m_slices[*index];
    slice.filesystem = filesystem;
    // A size only reformatting allowed goes with it.
    if (slice.kind == Slice::Kind::Partition && !slice.reformats() && slice.size != slice.originalSize) {
        const ActionState state = slice.size < slice.originalSize ? shrinkState(id) : growState(id);
        if (!state.enabled || slice.size < minimumSizeOf(slice))
            restoreSize(*index);
    }
}

void PartitionLayout::restoreSize(size_t index)
{
    Slice& slice = m_slices[index];
    const uint64_t oldEnd = slice.end();
    const uint64_t target = slice.offset + slice.originalSize;
    slice.size = slice.originalSize;
    if (target < oldEnd) {
        Slice* next = index + 1 < m_slices.size() ? &m_slices[index + 1] : nullptr;
        if (next && (next->kind == Slice::Kind::Free || next->kind == Slice::Kind::New) && next->offset == oldEnd) {
            next->size += oldEnd - target;
            next->offset = target;
        } else {
            insert(index + 1, {.kind = Slice::Kind::Free, .offset = target, .size = oldEnd - target});
        }
    } else {
        // Only free space and new partitions took the space it gave up.
        for (size_t i = index + 1; i < m_slices.size() && m_slices[i].offset < target;) {
            Slice& next = m_slices[i];
            if (next.kind != Slice::Kind::Free && next.kind != Slice::Kind::New)
                break;
            const uint64_t cut = std::min(next.end(), target) - next.offset;
            next.offset += cut;
            next.size -= cut;
            if (next.size == 0)
                m_slices.erase(m_slices.begin() + ptrdiff_t(i));
            else
                ++i;
        }
    }
    normalize();
}

ActionState PartitionLayout::addState(std::optional<uint32_t> selected) const
{
    if (partitionCount() >= maximumPartitions(m_tableType)) {
        if (m_tableType == "dos")
            return refused(tr("A Master Boot Record holds at most four partitions."));
        return refused(tr("The partition map holds no more partitions."));
    }
    for (const Slice& slice : m_slices) {
        if (slice.kind == Slice::Kind::Free && slice.size >= kMiB)
            return allowed();
    }
    if (selected) {
        if (const std::optional<size_t> index = indexOf(*selected)) {
            const Slice& slice = m_slices[*index];
            const uint64_t half = alignDown(slice.size / 2);
            if (slice.isEditable() && half >= kMiB && slice.offset + half >= endRange(*index).first)
                return allowed();
        }
    }
    return refused(tr("There is no free space for another partition."));
}

std::optional<uint32_t> PartitionLayout::add(std::optional<uint32_t> selected, const std::string& filesystem,
                                             const std::string& name)
{
    if (!addState(selected).enabled)
        return std::nullopt;
    Slice created{.kind = Slice::Kind::New, .name = name, .filesystem = filesystem, .displayName = name};

    std::optional<size_t> target;
    if (selected) {
        const std::optional<size_t> index = indexOf(*selected);
        if (index && m_slices[*index].kind == Slice::Kind::Free && m_slices[*index].size >= kMiB)
            target = index;
    }
    if (!target) {
        for (size_t i = 0; i < m_slices.size(); ++i) {
            const Slice& slice = m_slices[i];
            if (slice.kind == Slice::Kind::Free && slice.size >= kMiB && (!target || slice.size > m_slices[*target].size))
                target = i;
        }
    }
    if (target) {
        const Slice free = m_slices[*target];
        const uint64_t start = alignUp(free.offset);
        const uint64_t stop = std::max(start, alignDown(free.end()));
        created.offset = start;
        created.size = stop - start;
        m_slices.erase(m_slices.begin() + ptrdiff_t(*target));
        return insert(*target, std::move(created));
    }

    const std::optional<size_t> index = indexOf(*selected);
    Slice& slice = m_slices[*index];
    const uint64_t half = alignDown(slice.size / 2);
    created.offset = slice.offset + half;
    created.size = slice.end() - created.offset;
    slice.size = half;
    return insert(*index + 1, std::move(created));
}

ActionState PartitionLayout::removeState(uint32_t id) const
{
    const Slice* found = slice(id);
    if (!found || found->kind == Slice::Kind::Free)
        return refused({});
    if (found->kind == Slice::Kind::Container)
        return refused(tr("Extended partitions and the partitions inside them can't be changed here."));
    return allowed();
}

bool PartitionLayout::remove(uint32_t id)
{
    if (!removeState(id).enabled)
        return false;
    const size_t index = *indexOf(id);
    Slice& slice = m_slices[index];
    if (slice.kind == Slice::Kind::Partition)
        m_deleted.push_back(slice);
    m_slices[index] = {.id = slice.id, .kind = Slice::Kind::Free, .offset = slice.offset, .size = slice.size};
    normalize();
    return true;
}

void PartitionLayout::normalize()
{
    std::vector<Slice> merged;
    for (Slice& slice : m_slices) {
        if (slice.kind == Slice::Kind::Free && !merged.empty() && merged.back().kind == Slice::Kind::Free) {
            merged.back().size = std::max(merged.back().end(), slice.end()) - merged.back().offset;
            continue;
        }
        merged.push_back(std::move(slice));
    }
    std::erase_if(merged, [](const Slice& slice) { return slice.kind == Slice::Kind::Free && slice.size < kMiB; });
    m_slices = std::move(merged);
}

bool PartitionLayout::isChanged() const
{
    if (!m_deleted.empty())
        return true;
    for (const Slice& slice : m_slices) {
        if (slice.kind == Slice::Kind::New)
            return true;
        if (slice.kind == Slice::Kind::Partition
            && (slice.size != slice.originalSize || slice.reformats() || slice.name != slice.originalName))
            return true;
    }
    return false;
}

std::string PartitionLayout::problem() const
{
    for (const Slice& slice : m_slices) {
        if (!slice.isEditable())
            continue;
        const bool formats = slice.kind == Slice::Kind::New || slice.reformats();
        if (formats && slice.size < minimumSize(slice.filesystem)) {
            return format(tr("“{name}” is too small for {format}, which needs at least {size}."),
                          {{"name", slice.name.empty() ? slice.displayName : slice.name},
                           {"format", formatName(slice.filesystem)},
                           {"size", udisks::formatSize(minimumSize(slice.filesystem))}});
        }
        if (slice.kind == Slice::Kind::Partition && !formats && slice.size != slice.originalSize) {
            const bool shrinks = slice.size < slice.originalSize;
            const ActionState state = shrinks ? shrinkState(slice.id) : growState(slice.id);
            if (!state.enabled)
                return state.reason.empty() ? format(tr("“{name}” can't be resized."), {{"name", slice.displayName}})
                                            : state.reason;
            if (shrinks && slice.size < minimumSizeOf(slice))
                return format(tr("“{name}” can't be smaller than {size}."),
                              {{"name", slice.displayName}, {"size", udisks::formatSize(minimumSizeOf(slice))}});
        }
        if (formats || slice.name != slice.originalName) {
            const std::string label = labelProblem(slice.filesystem, slice.name);
            if (!label.empty())
                return label;
        }
    }
    return {};
}

void PartitionLayout::updateTeardown(const Model& model)
{
    const auto update = [&](const std::string& block, const std::string& subject) {
        if (const Object* object = model.object(block))
            m_teardown[block] = teardownSteps(model, *object, subject);
        else
            m_teardown.erase(block);
    };
    if (m_createsTable)
        update(m_disk, m_diskName);
    for (const Slice& slice : m_slices) {
        if (slice.kind == Slice::Kind::Partition)
            update(slice.block, slice.displayName);
    }
}

std::vector<PlanStep> PartitionLayout::plan() const
{
    std::vector<PlanStep> steps;
    if (!problem().empty())
        return steps;
    std::vector<std::string> tornDown;
    const auto teardown = [&](const std::string& block) {
        if (std::find(tornDown.begin(), tornDown.end(), block) != tornDown.end())
            return;
        tornDown.push_back(block);
        const auto it = m_teardown.find(block);
        if (it != m_teardown.end())
            steps.insert(steps.end(), it->second.begin(), it->second.end());
    };
    const auto partitions = [&](auto&& condition, auto&& emit) {
        for (const Slice& slice : m_slices) {
            if (slice.kind == Slice::Kind::Partition && condition(slice))
                emit(slice);
        }
    };

    if (m_createsTable) {
        const bool creates = std::any_of(m_slices.begin(), m_slices.end(),
                                         [](const Slice& slice) { return slice.kind == Slice::Kind::New; });
        if (!creates)
            return steps;
        teardown(m_disk);
        udisks::FormatOptions options;
        options.takeOwnership = false;
        steps.push_back({.kind = PlanStep::Kind::CreateTable, .path = m_disk, .type = m_tableType,
                         .options = options, .subject = m_diskName});
    }

    partitions([](const Slice& slice) { return slice.size < slice.originalSize; },
               [&](const Slice& slice) {
                   if (slice.reformats()) {
                       teardown(slice.block);
                   } else {
                       steps.push_back({.kind = PlanStep::Kind::ShrinkFilesystem, .path = slice.contentBlock,
                                        .size = slice.size, .subject = slice.displayName});
                   }
                   steps.push_back({.kind = PlanStep::Kind::ResizePartition, .path = slice.block, .size = slice.size,
                                    .subject = slice.displayName});
               });

    std::vector<Slice> deleted = m_deleted;
    std::sort(deleted.begin(), deleted.end(), [](const Slice& a, const Slice& b) { return a.offset < b.offset; });
    for (const Slice& slice : deleted)
        steps.push_back({.kind = PlanStep::Kind::Delete, .path = slice.block, .subject = slice.displayName});

    partitions([](const Slice& slice) { return slice.size > slice.originalSize; },
               [&](const Slice& slice) {
                   if (slice.reformats())
                       teardown(slice.block);
                   steps.push_back({.kind = PlanStep::Kind::ResizePartition, .path = slice.block, .size = slice.size,
                                    .subject = slice.displayName});
                   if (!slice.reformats()) {
                       steps.push_back({.kind = PlanStep::Kind::GrowFilesystem, .path = slice.contentBlock,
                                        .size = slice.size, .subject = slice.displayName});
                   }
               });
    partitions([](const Slice& slice) { return slice.reformats(); },
               [&](const Slice& slice) {
                   teardown(slice.block);
                   steps.push_back({.kind = PlanStep::Kind::Format, .path = slice.block, .size = slice.size,
                                    .type = slice.filesystem, .options = formatOptionsFor(slice.filesystem, slice.name),
                                    .subject = slice.displayName});
               });
    for (const Slice& slice : m_slices) {
        if (slice.kind != Slice::Kind::New)
            continue;
        const std::string label = normalizedLabel(slice.filesystem, slice.name);
        steps.push_back({.kind = PlanStep::Kind::Create, .path = m_disk, .offset = slice.offset, .size = slice.size,
                         .type = slice.filesystem,
                         .partitionName = m_tableType == "gpt" ? partitionNameFor(label) : std::string(),
                         .options = formatOptionsFor(slice.filesystem, slice.name),
                         .subject = label.empty() ? formatName(slice.filesystem) : label});
    }
    partitions([](const Slice& slice) {
                   return !slice.reformats() && slice.hasFilesystem && slice.name != slice.originalName;
               },
               [&](const Slice& slice) {
                   udisks::FormatOptions options;
                   options.label = normalizedLabel(slice.filesystem, slice.name);
                   steps.push_back({.kind = PlanStep::Kind::SetLabel, .path = slice.contentBlock, .options = options,
                                    .subject = slice.displayName});
               });
    // Each step expects its block as the editor read it, with the size the
    // plan's own resize gave it.
    std::map<std::string, std::pair<udisks::Identity, uint64_t>> expected;
    const auto known = [&](const Slice& slice) {
        expected[slice.block] = {slice.identity, 0};
        if (slice.contentBlock != slice.block)
            expected[slice.contentBlock] = {slice.contentIdentity, 0};
    };
    for (const Slice& slice : m_slices) {
        if (slice.kind == Slice::Kind::Partition)
            known(slice);
    }
    for (const Slice& slice : m_deleted)
        known(slice);
    for (PlanStep& step : steps) {
        const auto it = expected.find(step.path);
        if (it == expected.end())
            continue;
        step.expected = it->second.first;
        step.resizedFrom = it->second.second;
        if (step.kind == PlanStep::Kind::ResizePartition) {
            it->second.second = it->second.first.size;
            it->second.first.size = step.size;
        }
    }
    return steps;
}

} // namespace dialogs
