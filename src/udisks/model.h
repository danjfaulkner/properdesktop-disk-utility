// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "udisks/system.h"
#include "udisks/types.h"

#include "base/signal.h"

#include <map>
#include <string>
#include <vector>

namespace udisks {

struct Identity;

// UDisks2's object tree as of its last signal: what GetManagedObjects gave,
// kept up to date by InterfacesAdded, InterfacesRemoved and
// PropertiesChanged. Fed by BusClient from the system bus, or by a test
// directly. Every update emits changed() once it has been applied.
class Model
{
public:
    // Interface name to its properties.
    using Interfaces = std::map<std::string, VariantMap>;
    // Object path to its interfaces, as GetManagedObjects returns them.
    using Objects = std::map<std::string, Interfaces>;

    Model() = default;
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    // Replaces every object, as after GetManagedObjects; an empty map while
    // UDisks2 is not running.
    void reset(const Objects& objects);
    void addInterfaces(const std::string& path, const Interfaces& interfaces);
    void removeInterfaces(const std::string& path, const std::vector<std::string>& interfaces);
    void changeProperties(const std::string& path, const std::string& interface, const VariantMap& changed,
                          const std::vector<std::string>& invalidated = {});
    void setManager(const VariantMap& properties);
    // The devices the running system lives on, as probeSystemDevices() finds.
    void setSystemDevices(SystemDevices devices);

    bool isReady() const { return m_ready; }
    const std::map<std::string, Object>& objects() const { return m_objects; }
    // Null for an unknown path.
    const Object* object(const std::string& path) const;
    const Manager& manager() const { return m_manager; }
    const SystemDevices& systemDevices() const { return m_system; }

    // The drive's whole-disk block: the one without a partition or a
    // cleartext role. Null when the drive has no medium.
    const Object* wholeBlock(const std::string& drive) const;
    // Every block of the drive: the whole disk, its partitions, and the
    // cleartext blocks of its encrypted ones.
    std::vector<const Object*> blocksOfDrive(const std::string& drive) const;
    // The partitions of a partition table block, by offset; logical
    // partitions included.
    std::vector<const Object*> partitionsOf(const std::string& table) const;
    // The logical partitions inside an extended partition, by offset.
    std::vector<const Object*> logicalPartitionsOf(const std::string& container) const;
    // The unlocked block of an encrypted one; null while it is locked.
    const Object* cleartextOf(const std::string& encrypted) const;
    // Where a block's data lives: its cleartext block when it is unlocked,
    // otherwise itself.
    const Object* contentOf(const std::string& block) const;
    // The jobs working on the object, or on a block of the drive for a drive.
    std::vector<const Object*> jobsFor(const std::string& path) const;

    // The block holds the running system: a system device, mounted at one
    // of the system's mount points, or swap in use; for an encrypted block,
    // its cleartext block does; for a partition table or an extended
    // partition, any partition in it does. The EFI system, BIOS boot and
    // extended boot loader partitions of a disk that holds the system hold
    // it too, mounted or not. Every block does while the system's devices
    // are not known; none that is not in the model does.
    bool holdsSystem(const std::string& block) const;
    // Any block of the drive holds the system.
    bool driveHoldsSystem(const std::string& drive) const;
    // Mounted anywhere, through its cleartext block or as active swap.
    bool isInUse(const std::string& block) const;
    // The block at the identity's path is still the one it was taken of
    // (identity.h). A filesystem made or renamed on it, or its mounts,
    // change nothing; another drive or image, or a partition moved or
    // resized, does.
    bool matches(const Identity& identity) const;

    // Any update.
    properdesktop::base::Signal<> changed;
    properdesktop::base::Signal<const std::string&> objectAdded;
    properdesktop::base::Signal<const std::string&> objectRemoved;
    // Properties changed, or an interface came or went on an object that
    // stays.
    properdesktop::base::Signal<const std::string&> objectChanged;

private:
    bool holdsSystemItself(const std::string& block) const;
    void reparse(Object& object, const std::string& interface);

    std::map<std::string, Object> m_objects;
    Manager m_manager;
    SystemDevices m_system;
    bool m_ready = false;
};

} // namespace udisks
