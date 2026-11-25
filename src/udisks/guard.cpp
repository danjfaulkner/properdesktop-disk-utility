// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "udisks/guard.h"
#include "udisks/model.h"

#include "base/i18n.h"

using properdesktop::i18n::tr;

namespace udisks {

// The whole disk the block is on: through its encrypted block and its
// partition table; itself for a whole disk.
static std::string diskOf(const Model& model, const std::string& path, int depth = 0)
{
    const Object* object = model.object(path);
    if (!object || !object->block || depth > 4)
        return path;
    if (!object->block->cryptoBackingDevice.empty())
        return diskOf(model, object->block->cryptoBackingDevice, depth + 1);
    if (object->partition)
        return object->partition->table;
    return path;
}

Change formatChange(const Model& model, const std::string& block)
{
    const Object* object = model.object(block);
    return object && object->partition ? Change::Volume : Change::Table;
}

std::string systemRefusal(const Model& model, const std::string& block, Change change)
{
    const Object* object = model.object(block);
    if (!object || !object->block)
        return tr("The device is no longer available.");
    if (!model.systemDevices().known)
        return tr("The disks of the running system could not be found.");
    if (model.holdsSystem(block))
        return change == Change::Volume ? tr("The volume holds the running system.")
                                        : tr("The disk holds the running system.");
    if (change == Change::Volume)
        return {};
    const std::string disk = diskOf(model, block);
    const Object* whole = model.object(disk);
    const std::string drive = whole && whole->block ? whole->block->drive : object->block->drive;
    if (model.holdsSystem(disk) || (!drive.empty() && model.driveHoldsSystem(drive)))
        return tr("The disk holds the running system.");
    return {};
}

} // namespace udisks
