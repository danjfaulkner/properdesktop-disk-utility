// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "base/dbus_types.h"

#include <chrono>
#include <functional>
#include <string>

namespace udisks {

// A method call on one of UDisks2's objects.
struct Call
{
    std::string path;
    std::string interface;
    std::string method;
    properdesktop::base::dbus::Variant::Items arguments;
    // Formatting and checking run as long as the disk takes, and a
    // password prompt as long as the user does.
    std::chrono::seconds timeout{24 * 60 * 60};

    bool operator==(const Call&) const = default;
};

// Its reply: the return values, or an error.
struct Result
{
    // A D-Bus error name, such as org.freedesktop.UDisks2.Error.NotAuthorized
    // or org.freedesktop.UDisks2.Error.Cancelled; empty on success.
    std::string errorName;
    std::string errorMessage;
    properdesktop::base::dbus::Variant::Items values;

    bool ok() const { return errorName.empty(); }
    // The user dismissed the password dialog or cancelled the job.
    bool cancelled() const;

    // The first return value as a string (Mount's mount point) or an object
    // path (CreatePartition's partition, Unlock's cleartext block, LoopSetup's
    // loop device); empty when there is none.
    std::string string() const;
    std::string objectPath() const;
    // Check and Repair: the filesystem is consistent or was repaired.
    bool boolean() const;
};

using Done = std::function<void(const Result&)>;

// Where calls go: the system bus, or a test's record of them. Calls never
// block; done runs later from the event loop, never inside call(), and not
// at all once the backend is gone.
class Backend
{
public:
    virtual ~Backend() = default;
    virtual void call(const Call& call, Done done) = 0;
};

} // namespace udisks
