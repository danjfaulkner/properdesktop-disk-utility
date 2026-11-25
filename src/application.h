// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "base/signal.h"

#include <memory>
#include <string>
#include <vector>

namespace properdesktop::base::dbus {
class Connection;
class Object;
}

namespace udisks {
class Backend;
class Model;
class Operations;
}

// UDisks2's objects and the operations on them, shared by the window.
class Application
{
public:
    static constexpr const char* kService = "org.properdesktop.DiskUtility";
    static constexpr const char* kPath = "/DiskUtility";

    // UDisks2 on the system bus; the devices of the running system are found
    // again whenever the disks change.
    Application();
    // Another backend, such as a test's, which fills the model and sets the
    // system's devices itself.
    explicit Application(std::unique_ptr<udisks::Backend> backend);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    udisks::Model& model() { return *m_model; }
    udisks::Operations& operations() { return *m_operations; }
    udisks::Backend& backend() { return *m_backend; }
    // The preferences file; empty with a test's backend, which keeps none.
    const std::string& configPath() const { return m_configPath; }

    // The single instance's object, before its name is asked for.
    void publish(properdesktop::base::dbus::Connection* bus);
    // The running-application object the dock follows, once the name is ours.
    void publishInstance(properdesktop::base::dbus::Connection* bus);
    // Disk image files from the command line or another launch: paths or
    // file: URLs. A relative path is taken from the directory when one is
    // given, and dropped otherwise.
    void open(const std::vector<std::string>& paths, const std::string& directory = {});

    // Another launch asked for the window.
    properdesktop::base::Signal<const std::string&> activateRequested;
    properdesktop::base::Signal<> quitRequested;
    // Disk images to attach.
    properdesktop::base::Signal<const std::vector<std::string>&> openRequested;

private:
    void scheduleProbe();

    std::unique_ptr<udisks::Model> m_model;
    std::unique_ptr<udisks::Backend> m_backend;
    std::unique_ptr<udisks::Operations> m_operations;
    std::string m_configPath;
    bool m_probes = false;
    bool m_probeScheduled = false;
    std::unique_ptr<properdesktop::base::dbus::Object> m_object;
    std::unique_ptr<properdesktop::base::dbus::Object> m_instance;
    std::vector<properdesktop::base::Subscription> m_subscriptions;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
