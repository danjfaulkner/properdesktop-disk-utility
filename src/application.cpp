// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "application.h"

#include "udisks/bus.h"
#include "udisks/model.h"
#include "udisks/operations.h"
#include "udisks/system.h"

#include "base/dbus.h"
#include "base/event_loop.h"
#include "base/paths.h"
#include "base/url.h"

#include <filesystem>
#include <optional>

Application::Application()
    : m_model(std::make_unique<udisks::Model>())
    , m_backend(std::make_unique<udisks::BusClient>(*m_model, properdesktop::base::dbus::Connection::system()))
    , m_operations(std::make_unique<udisks::Operations>(*m_model, *m_backend))
    , m_configPath(properdesktop::base::paths::properdesktopConfig("disk-utility"))
    , m_probes(true)
{
    // A mount or a new swap device changes what holds the system.
    m_subscriptions.push_back(m_model->changed.connect([this] { scheduleProbe(); }));
    m_model->setSystemDevices(udisks::probeSystemDevices());
}

Application::Application(std::unique_ptr<udisks::Backend> backend)
    : m_model(std::make_unique<udisks::Model>())
    , m_backend(std::move(backend))
    , m_operations(std::make_unique<udisks::Operations>(*m_model, *m_backend))
{
}

Application::~Application() = default;

void Application::scheduleProbe()
{
    if (!m_probes || m_probeScheduled)
        return;
    m_probeScheduled = true;
    properdesktop::base::EventLoop::current()->post([this, alive = std::weak_ptr<bool>(m_alive)] {
        if (alive.expired())
            return;
        m_probeScheduled = false;
        m_model->setSystemDevices(udisks::probeSystemDevices());
    });
}

void Application::open(const std::vector<std::string>& paths, const std::string& directory)
{
    std::vector<std::string> files;
    for (const std::string& path : paths) {
        std::filesystem::path file;
        if (path.find("://") != std::string::npos) {
            const std::optional<std::string> local = properdesktop::base::url::toLocalFile(path);
            if (!local)
                continue;
            file = *local;
        } else {
            file = path;
        }
        if (file.is_relative()) {
            if (directory.empty() || std::filesystem::path(directory).is_relative())
                continue;
            file = std::filesystem::path(directory) / file;
        }
        files.push_back(file.lexically_normal().string());
    }
    if (!files.empty())
        openRequested.emit(files);
}

void Application::publish(properdesktop::base::dbus::Connection* bus)
{
    if (!bus)
        return;
    m_object = std::make_unique<properdesktop::base::dbus::Object>(bus, kPath, kService);
    m_object->addMethod(
        "open",
        [this](const std::vector<std::string>& paths, const std::string& activationToken) {
            activateRequested.emit(activationToken);
            open(paths);
        },
        properdesktop::base::dbus::ArgumentNames{{"paths", "activationToken"}, {}});
    m_object->publish();
}

void Application::publishInstance(properdesktop::base::dbus::Connection* bus)
{
    if (!bus)
        return;
    // The dock's view of a running application: the dock's Quit (and
    // Command+Q) ends it here rather than closing its window.
    m_instance = std::make_unique<properdesktop::base::dbus::Object>(bus, "/org/properdesktop/Application",
                                                                "org.properdesktop.Application");
    m_instance->addProperty("AppId", [] { return std::string("properdesktop-disk-utility"); });
    m_instance->addProperty("ShowsInDock", [] { return true; });
    m_instance->addMethod(
        "Launch",
        [this](const std::vector<std::string>& paths, const std::string& activationToken) {
            activateRequested.emit(activationToken);
            open(paths);
        },
        properdesktop::base::dbus::ArgumentNames{{"paths", "activationToken"}, {}});
    m_instance->addMethod("Quit", [this] { quitRequested.emit(); });
    m_instance->publish();
    bus->requestName("org.properdesktop.Application.properdesktop_disk_utility");
}
