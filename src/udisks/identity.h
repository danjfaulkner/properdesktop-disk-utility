// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "base/signal.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace udisks {

class Model;

// What a block was when a dialog was opened on it, beyond its object path:
// UDisks2 names blocks after the kernel's devices, and a stick pulled out
// and another put in, or a disk image detached and another attached, takes
// the same path at once. A dialog checks it again before it changes
// anything.
struct Identity
{
    std::string block;
    std::string drive;
    // The drive's Id, Serial and WWN, and when it and its medium appeared:
    // two sticks without a serial, or two cards of a size swapped in one
    // reader, differ there.
    std::string driveId;
    std::string serial;
    std::string wwn;
    uint64_t timeDetected = 0;
    uint64_t timeMediaDetected = 0;
    // A disk image's backing file.
    std::string backingFile;
    uint64_t size = 0;
    // A partition's place and UUID in its table; ignored for a whole disk.
    bool partition = false;
    uint64_t offset = 0;
    std::string partitionUuid;

    bool isValid() const { return !block.empty(); }
    bool operator==(const Identity&) const = default;
};

// The block as it is now; invalid when it is not in the model.
Identity identityOf(const Model& model, const std::string& block);
// The disk the block is on, as it is now: its drive and whole size, which
// stay while partitions on it are created, resized and deleted.
Identity diskIdentityOf(const Model& model, const std::string& block);
// Follows identities on every change of the model, before anything is laid
// out again, and calls onLost once when one of them stops matching.
class IdentityWatch
{
public:
    IdentityWatch(Model& model, std::vector<Identity> identities, std::function<void()> onLost);

    IdentityWatch(const IdentityWatch&) = delete;
    IdentityWatch& operator=(const IdentityWatch&) = delete;

    bool holds() const;
    const std::vector<Identity>& identities() const { return m_identities; }

private:
    void check();

    Model& m_model;
    std::vector<Identity> m_identities;
    std::function<void()> m_onLost;
    bool m_lost = false;
    properdesktop::base::Subscription m_subscription;
};

} // namespace udisks
