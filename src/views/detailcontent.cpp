// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "views/detailcontent.h"

#include "udisks/model.h"
#include "udisks/smart.h"

#include "base/i18n.h"

#include <algorithm>

#include <pwd.h>
#include <sys/stat.h>

using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace views {

std::string jobTitle(const std::string& operation)
{
    if (operation.starts_with("format"))
        return tr("Erasing");
    if (operation == "filesystem-check")
        return tr("Checking");
    if (operation == "filesystem-repair")
        return tr("Repairing");
    if (operation == "filesystem-resize" || operation == "partition-resize")
        return tr("Resizing");
    if (operation == "filesystem-mount")
        return tr("Mounting");
    if (operation == "filesystem-unmount")
        return tr("Unmounting");
    if (operation == "partition-create")
        return tr("Adding a partition");
    if (operation == "partition-delete")
        return tr("Deleting a partition");
    if (operation.starts_with("partition-"))
        return tr("Changing the partition");
    if (operation == "encrypted-unlock")
        return tr("Unlocking");
    if (operation == "encrypted-lock")
        return tr("Locking");
    if (operation == "drive-eject")
        return tr("Ejecting");
    if (operation == "drive-power-off")
        return tr("Powering off");
    if (operation.ends_with("selftest"))
        return tr("Running a self-test");
    if (operation == "loop-setup")
        return tr("Attaching");
    return tr("Working");
}

static void add(std::vector<InfoItem>& column, const char* key, const std::string& title, const std::string& value)
{
    if (!value.empty())
        column.push_back({key, title, value});
}

static std::string ownerOf(const std::string& mountPoint)
{
    struct stat info;
    if (mountPoint.empty() || ::stat(mountPoint.c_str(), &info) != 0)
        return {};
    struct passwd entry;
    struct passwd* found = nullptr;
    char buffer[1024];
    if (getpwuid_r(info.st_uid, &entry, buffer, sizeof(buffer), &found) != 0 || !found)
        return std::to_string(info.st_uid);
    return found->pw_name;
}

static std::string locationName(const udisks::Model& model, const udisks::Node& node)
{
    if (node.drive.empty())
        return tr("Disk Image");
    return udisks::sectionOfDrive(model, node.drive) == udisks::Section::External ? tr("External") : tr("Internal");
}

static std::string mediaKind(const udisks::Object* drive)
{
    if (!drive || !drive->drive)
        return {};
    if (drive->drive->optical)
        return tr("Optical Disc");
    if (drive->drive->media.starts_with("flash") || drive->drive->media == "thumb")
        return tr("Flash Memory");
    if (drive->drive->rotationRate == 0)
        return tr("Solid State");
    if (drive->drive->rotationRate > 0)
        return tr("Hard Disk");
    return {};
}

// The content's filesystem, and whether it is encrypted and locked.
static std::string volumeType(const udisks::Model& model, const std::string& block)
{
    const udisks::Object* object = model.object(block);
    if (!object || !object->block)
        return {};
    if (object->encrypted || object->block->idType == "crypto_LUKS") {
        const udisks::Object* cleartext = model.cleartextOf(block);
        if (!cleartext || !cleartext->block)
            // TRANSLATORS: an encrypted volume that is locked, such as "LUKS (Locked)".
            return format(tr("{type} (Locked)"), {{"type", udisks::filesystemName(object->block->idType)}});
        const std::string inner = udisks::filesystemName(cleartext->block->idType, cleartext->block->idVersion);
        // TRANSLATORS: an unlocked encrypted volume, such as "ext4 (Encrypted)".
        return format(tr("{type} (Encrypted)"), {{"type", inner.empty() ? tr("Unformatted") : inner}});
    }
    return udisks::filesystemName(object->block->idType, object->block->idVersion);
}

static std::string joined(const std::vector<std::string>& parts)
{
    std::string text;
    for (const std::string& part : parts) {
        if (part.empty())
            continue;
        if (!text.empty())
            text += " · ";
        text += part;
    }
    return text;
}

static void addJob(const udisks::Model& model, const std::string& path, DetailContent& content)
{
    if (path.empty() || content.job)
        return;
    const std::vector<const udisks::Object*> jobs = model.jobsFor(path);
    if (jobs.empty() || !jobs.front()->job)
        return;
    const udisks::Job& job = *jobs.front()->job;
    JobLine line;
    line.text = jobTitle(job.operation) + "…";
    if (job.progressValid)
        line.progress = std::clamp(job.progress, 0.0, 1.0);
    content.job = line;
}

static void describeDisk(const udisks::Model& model, const udisks::Node& node, const udisks::Object& object,
                         DetailContent& content)
{
    const udisks::Object* drive = model.object(node.drive);
    const udisks::Block& block = *object.block;
    const std::string scheme = object.partitionTable ? udisks::schemeName(object.partitionTable->type) : std::string();
    const std::string connection = udisks::connectionName(udisks::connectionOf(model, node.drive));
    const std::string filesystem = object.partitionTable ? std::string() : volumeType(model, node.block);
    content.subtitle = joined({udisks::formatSize(block.size), scheme.empty() ? filesystem : scheme, connection});

    size_t partitions = 0;
    if (object.partitionTable) {
        uint64_t taken = 0;
        for (const udisks::Object* partition : model.partitionsOf(node.block)) {
            if (partition->partition->isContained || !partition->block)
                continue;
            udisks::Node part{udisks::NodeKind::Partition, {}, node.drive, partition->path, node.disk, {}};
            if (partition->partition->isContainer)
                part.kind = udisks::NodeKind::Container;
            else if (partition->block->idUsage == "filesystem" || partition->encrypted)
                part.kind = udisks::NodeKind::Volume;
            content.segments.push_back({UsageSegment::Kind::Partition, udisks::displayName(model, part),
                                        partition->partition->size, partitions++});
            taken += partition->partition->size;
        }
        if (block.size > taken)
            content.segments.push_back({UsageSegment::Kind::Free, tr("Free"), block.size - taken, 0});
    } else if (block.size > 0) {
        const udisks::Usage usage = udisks::usageOf(model, node);
        if (usage.known)
            content.segments.push_back({UsageSegment::Kind::Partition, udisks::displayName(model, node), block.size, 0});
        else
            content.segments.push_back({UsageSegment::Kind::Free, tr("Free"), block.size, 0});
    }

    add(content.left, "location", tr("Location"), locationName(model, node));
    add(content.left, "capacity", tr("Capacity"), udisks::formatSize(block.size));
    if (object.partitionTable)
        add(content.left, "children", tr("Partitions"), std::to_string(partitions));
    add(content.left, "partitionMap", tr("Partition Map"), scheme);
    if (object.loop)
        add(content.left, "image", tr("Image File"), object.loop->backingFile);
    add(content.right, "connection", tr("Connection"), connection);
    add(content.right, "type", tr("Type"), node.drive.empty() ? tr("Disk Image") : mediaKind(drive));
    if (drive && (drive->ata || drive->nvme))
        add(content.right, "smart", tr("S.M.A.R.T. Status"),
            udisks::smartStatusName(udisks::smartStatus(model, node.drive)));
    add(content.right, "device", tr("Device"), udisks::deviceName(model, node.block));
    if (block.readOnly)
        add(content.right, "readOnly", tr("Access"), tr("Read Only"));
}

static void describeVolume(const udisks::Model& model, const udisks::Node& node, const udisks::Object& object,
                           const udisks::StatFunction& stat, DetailContent& content)
{
    const udisks::Object* inner = model.contentOf(node.block);
    const udisks::Block& block = *object.block;
    const std::string type = volumeType(model, node.block);
    const std::string connection = udisks::connectionName(udisks::connectionOf(model, node.drive));
    content.subtitle = joined({udisks::formatSize(block.size), type, connection});

    const std::string mountPoint = udisks::mountPointOf(model, node);
    const udisks::Usage usage = udisks::usageOf(model, node, stat);
    if (usage.known) {
        content.segments.push_back({UsageSegment::Kind::Used, tr("Used"), usage.used, 0});
        if (usage.other() > 0)
            content.segments.push_back({UsageSegment::Kind::Other, tr("Other"), usage.other(), 0});
        content.segments.push_back({UsageSegment::Kind::Free, tr("Free"), usage.available, 0});
    }

    const bool mountable = inner && inner->filesystem;
    add(content.left, "mountPoint", tr("Mount Point"), mountPoint.empty() && mountable ? tr("Not Mounted") : mountPoint);
    add(content.left, "capacity", tr("Capacity"), udisks::formatSize(block.size));
    if (usage.known) {
        add(content.left, "available", tr("Available"), udisks::formatSize(usage.available));
        add(content.left, "used", tr("Used"), udisks::formatSize(usage.used));
    }
    if (inner && inner->block)
        add(content.left, "uuid", tr("UUID"), inner->block->idUuid);
    add(content.right, "type", tr("Type"), type);
    add(content.right, "owner", tr("Owner"), ownerOf(mountPoint));
    add(content.right, "connection", tr("Connection"), connection);
    add(content.right, "device", tr("Device"), udisks::deviceName(model, node.block));
    if (block.readOnly)
        add(content.right, "readOnly", tr("Access"), tr("Read Only"));
}

static void describePartition(const udisks::Model& model, const udisks::Node& node, const udisks::Object& object,
                              DetailContent& content)
{
    const udisks::Block& block = *object.block;
    std::string type;
    if (node.kind == udisks::NodeKind::Container)
        type = tr("Extended Partition");
    else if (!block.idType.empty())
        type = udisks::filesystemName(block.idType, block.idVersion);
    else
        type = tr("Unformatted");
    const std::string connection = udisks::connectionName(udisks::connectionOf(model, node.drive));
    content.subtitle = joined({udisks::formatSize(block.size), type, connection});

    if (node.kind == udisks::NodeKind::Container) {
        size_t index = 0;
        uint64_t taken = 0;
        for (const udisks::Object* logical : model.partitionsOf(node.disk)) {
            if (!logical->partition->isContained || !logical->block)
                continue;
            udisks::Node part{udisks::NodeKind::Partition, {}, node.drive, logical->path, node.disk, {}};
            if (logical->block->idUsage == "filesystem" || logical->encrypted)
                part.kind = udisks::NodeKind::Volume;
            content.segments.push_back({UsageSegment::Kind::Partition, udisks::displayName(model, part),
                                        logical->partition->size, index++});
            taken += logical->partition->size;
        }
        if (index > 0 && block.size > taken)
            content.segments.push_back({UsageSegment::Kind::Free, tr("Free"), block.size - taken, 0});
    }

    add(content.left, "capacity", tr("Capacity"), udisks::formatSize(block.size));
    if (object.partition && object.partition->number > 0)
        add(content.left, "number", tr("Partition"), std::to_string(object.partition->number));
    if (object.partition)
        add(content.left, "uuid", tr("UUID"), object.partition->uuid);
    add(content.right, "type", tr("Type"), type);
    add(content.right, "connection", tr("Connection"), connection);
    add(content.right, "device", tr("Device"), udisks::deviceName(model, node.block));
}

DetailContent describeDetail(const udisks::Model& model, const udisks::Node& node, const udisks::StatFunction& stat)
{
    DetailContent content;
    const udisks::Object* object = model.object(node.block);
    if (!object || !object->block)
        return content;
    content.icon = udisks::iconName(model, node);
    content.name = udisks::displayName(model, node);
    switch (node.kind) {
    case udisks::NodeKind::Disk:
        describeDisk(model, node, *object, content);
        break;
    case udisks::NodeKind::Volume:
        describeVolume(model, node, *object, stat, content);
        break;
    case udisks::NodeKind::Container:
    case udisks::NodeKind::Partition:
        describePartition(model, node, *object, content);
        break;
    }

    if (node.kind == udisks::NodeKind::Disk && !node.drive.empty())
        addJob(model, node.drive, content);
    addJob(model, node.block, content);
    if (const udisks::Object* inner = model.contentOf(node.block); inner && inner->path != node.block)
        addJob(model, inner->path, content);
    return content;
}

} // namespace views
