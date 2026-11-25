// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "application.h"
#include "icons.h"
#include "views/diskwindow.h"

#include "base/command_line.h"
#include "base/dbus.h"
#include "base/i18n.h"
#include "ui/application.h"
#include "ui/icon.h"

#include <chrono>
#include <cstdio>
#include <filesystem>

using namespace properdesktop;
using namespace std::chrono_literals;

// Another connection that owns the name, or empty when none does: a failed
// request for a name nobody holds is no second launch.
static std::string ownerOf(base::dbus::Connection* bus, const char* name)
{
    base::dbus::Proxy dbus(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus");
    dbus.setTimeout(3s);
    const std::string owner = dbus.call("GetNameOwner", std::string(name)).value<std::string>().value_or(std::string());
    if (owner.empty())
        std::fprintf(stderr, "properdesktop-disk-utility: could not take the name %s; running without it.\n", name);
    return owner;
}

int main(int argc, char** argv)
{
    base::CommandLineParser parser;
    parser.setApplicationDescription("Proper Desktop Disk Utility");
    parser.setApplicationVersion(DISK_UTILITY_VERSION);
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("images", "Disk images to attach.", "[images...]");
    parser.process(argc, argv);

    ui::Application app(argc, argv);
    app.setApplicationId("properdesktop-disk-utility");
    i18n::setDomain("properdesktop-disk-utility");
    for (const auto& [name, svg] : embeddedIcons())
        ui::Icon::registerGlyph(std::string(name), std::string(svg));

    std::vector<std::string> paths;
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::current_path(error);
    // A running instance has a directory of its own, so relative paths are
    // made whole here; URLs stay as they are.
    for (const std::string& argument : parser.positionalArguments()) {
        std::filesystem::path path(argument);
        if (argument.find("://") == std::string::npos && path.is_relative())
            path = directory / path;
        paths.push_back(argument.find("://") == std::string::npos ? path.lexically_normal().string() : argument);
    }

    // The object goes up before the name, so a second launch that finds the
    // name always finds someone to hand its images to.
    ::Application application;
    base::dbus::Connection* bus = app.sessionBus();
    if (bus) {
        application.publish(bus);
        if (!bus->requestName(::Application::kService) && !ownerOf(bus, ::Application::kService).empty()) {
            base::dbus::Proxy running(bus, ::Application::kService, ::Application::kPath, ::Application::kService);
            running.setTimeout(3s);
            const auto reply = running.call("open", paths, app.takeActivationToken());
            if (!reply.isError())
                return 0;
            // A second copy would work on the disks without knowing what the
            // first one is doing with them.
            std::fprintf(stderr, "properdesktop-disk-utility: Disk Utility is already running but did not answer (%s).\n",
                         reply.error().message.c_str());
            return 1;
        }
        application.publishInstance(bus);
    }

    views::DiskWindow window(application);
    auto quit = application.quitRequested.connect([&window] { window.requestQuit(); });
    window.show();
    if (!paths.empty())
        application.open(paths);
    return app.run();
}
