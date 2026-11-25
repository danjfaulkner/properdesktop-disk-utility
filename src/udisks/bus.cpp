// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "udisks/bus.h"
#include "udisks/model.h"

#include "base/event_loop.h"
#include "base/logging.h"

namespace udisks {

using namespace properdesktop::base;
using namespace properdesktop::base::dbus;

static constexpr const char* kObjectManager = "org.freedesktop.DBus.ObjectManager";
static constexpr const char* kProperties = "org.freedesktop.DBus.Properties";
static const LogCategory cfUdisks("properdesktop.diskutility.udisks");

BusClient::BusClient(Model& model, Connection* bus)
    : m_model(model)
    , m_bus(bus)
{
    if (!m_bus)
        return;

    // Subscribed before the first load: a signal sent before the snapshot
    // arrives ahead of its reply and is overwritten by it, one sent after
    // follows it.
    m_matches.push_back(m_bus->subscribe({kService, kRoot, {}, kObjectManager, "InterfacesAdded", {}},
                                         [this](const Message& message) {
                                             ObjectPath path;
                                             Model::Interfaces interfaces;
                                             if (message.read(path, interfaces))
                                                 m_model.addInterfaces(path.path, interfaces);
                                         }));
    m_matches.push_back(m_bus->subscribe({kService, kRoot, {}, kObjectManager, "InterfacesRemoved", {}},
                                         [this](const Message& message) {
                                             ObjectPath path;
                                             std::vector<std::string> interfaces;
                                             if (message.read(path, interfaces))
                                                 m_model.removeInterfaces(path.path, interfaces);
                                         }));
    m_matches.push_back(m_bus->subscribe({kService, {}, kRoot, kProperties, "PropertiesChanged", {}},
                                         [this](const Message& message) {
                                             std::string interface;
                                             VariantMap changed;
                                             std::vector<std::string> invalidated;
                                             if (!message.read(interface, changed, invalidated))
                                                 return;
                                             if (message.path() == kManagerPath)
                                                 loadManager();
                                             else
                                                 m_model.changeProperties(message.path(), interface, changed,
                                                                          invalidated);
                                         }));

    m_ownerWatch = m_bus->watchName(kService, [this](const std::string& owner) {
        // The first answer is the owner as of now, which the first load reads.
        if (!m_ownerKnown) {
            m_ownerKnown = true;
            return;
        }
        ++m_generation;
        m_model.reset({});
        if (!owner.empty())
            load();
    });

    load();
}

BusClient::~BusClient() = default;

void BusClient::release(uint64_t call)
{
    EventLoop::current()->post([self = m_weakFactory.getWeakPtr(), call] {
        if (BusClient* client = self.get())
            client->m_calls.erase(call);
    });
}

void BusClient::load()
{
    const uint64_t generation = m_generation;
    const uint64_t id = m_nextCall++;
    const Proxy manager(m_bus, kService, kRoot, kObjectManager);
    m_calls[id] = manager.callAsync("GetManagedObjects", [this, generation, id](const Reply& reply) {
        release(id);
        if (generation != m_generation)
            return;
        if (reply.isError()) {
            // No UDisks2: the window shows no disks, and its start fills them in.
            CF_WARNING(cfUdisks) << reply.error().message;
            m_model.reset({});
            return;
        }
        Model::Objects objects;
        for (auto& [path, interfaces] : reply.value<std::map<ObjectPath, Model::Interfaces>>().value_or(
                 std::map<ObjectPath, Model::Interfaces>()))
            objects.emplace(path.path, std::move(interfaces));
        m_model.reset(objects);
    });
    loadManager();
}

void BusClient::loadManager()
{
    const uint64_t generation = m_generation;
    const uint64_t id = m_nextCall++;
    const Proxy manager(m_bus, kService, kManagerPath, interface::kManager);
    m_calls[id] = manager.getAllAsync([this, generation, id](const Reply& reply) {
        release(id);
        if (generation == m_generation && !reply.isError())
            m_model.setManager(reply.value<VariantMap>().value_or(VariantMap()));
    });
}

void BusClient::call(const Call& call, Done done)
{
    // Done never runs inside call(), so a caller can start a call while it
    // updates its own state.
    const auto finish = [self = m_weakFactory.getWeakPtr(), done](Result result) {
        EventLoop::current()->post([self, done, result = std::move(result)] {
            if (self.get() && done)
                done(result);
        });
    };

    if (!m_bus) {
        finish({"org.freedesktop.DBus.Error.Disconnected", "The system bus cannot be reached.", {}});
        return;
    }
    Message message = m_bus->newMethodCall(kService, call.path, call.interface, call.method);
    for (const Variant& argument : call.arguments)
        message.appendAs(argument);
    message.setAllowInteractiveAuthorization(true);

    const uint64_t id = m_nextCall++;
    PendingCall pending = m_bus->callAsync(
        message,
        [this, id, finish](const Reply& reply) {
            release(id);
            Result result;
            if (reply.isError()) {
                result.errorName = reply.error().name;
                result.errorMessage = reply.error().message;
            } else {
                result.values = reply.arguments();
            }
            finish(std::move(result));
        },
        std::chrono::duration_cast<std::chrono::microseconds>(call.timeout));
    if (!pending.isNull())
        m_calls[id] = std::move(pending);
}

} // namespace udisks
