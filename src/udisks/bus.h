// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "udisks/backend.h"

#include "base/dbus.h"
#include "base/weak_ptr.h"

#include <cstdint>
#include <map>
#include <vector>

namespace udisks {

class Model;

// UDisks2 on the system bus: mirrors its objects into the model from
// GetManagedObjects and the ObjectManager and property signals, starts over
// when the service restarts, and makes the calls. Every call may ask the
// user for authorization through polkit.
class BusClient : public Backend
{
public:
    BusClient(Model& model, properdesktop::base::dbus::Connection* bus);
    ~BusClient() override;

    BusClient(const BusClient&) = delete;
    BusClient& operator=(const BusClient&) = delete;

    void call(const Call& call, Done done) override;

private:
    void load();
    void loadManager();
    // Drops a finished call's slot once its callback has returned.
    void release(uint64_t call);

    Model& m_model;
    properdesktop::base::dbus::Connection* m_bus;
    uint64_t m_generation = 0;
    uint64_t m_nextCall = 1;
    bool m_ownerKnown = false;
    std::map<uint64_t, properdesktop::base::dbus::PendingCall> m_calls;
    std::vector<properdesktop::base::dbus::Match> m_matches;
    properdesktop::base::dbus::NameWatch m_ownerWatch;
    properdesktop::base::WeakPtrFactory<BusClient> m_weakFactory{this};
};

} // namespace udisks
