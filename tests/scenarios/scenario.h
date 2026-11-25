// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "fake_udisks.h"

#include "application.h"
#include "views/diskwindow.h"

#include "ui/test_driver.h"
#include "ui/window.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

// The window over the sample disks, shown, and worked only through the
// driver; the backend records what it is asked.
class Scenario
{
public:
    Scenario()
        : window(makeWindow())
    {
        window->window().resize({900, 600});
        window->window().show();
        driver.layout();
    }

    udisks::fake::FakeBackend& backend() { return static_cast<udisks::fake::FakeBackend&>(application.backend()); }
    udisks::Model& model() { return application.model(); }

    ::Application application{std::make_unique<udisks::fake::FakeBackend>()};
    std::unique_ptr<views::DiskWindow> window;
    properdesktop::ui::TestDriver driver{window->window(), [](const std::string& message) { ADD_FAILURE() << message; }};

private:
    std::unique_ptr<views::DiskWindow> makeWindow()
    {
        udisks::fake::loadSample(application.model());
        return std::make_unique<views::DiskWindow>(application);
    }
};
