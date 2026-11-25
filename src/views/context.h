// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "actions.h"
#include "udisks/tree.h"

#include "base/signal.h"

#include <functional>
#include <string>
#include <vector>

class Application;

namespace properdesktop::ui {
class Window;
}

namespace udisks {
class Model;
class Operations;
}

namespace views {

// What the window's views and dialogs share: the disks, the operations, the
// actions, and the sidebar's tree as shown now.
struct Context
{
    Context(Application& application, properdesktop::ui::Window& window, ActionDispatcher& actions);

    Application& application;
    udisks::Model& model;
    udisks::Operations& operations;
    ActionDispatcher& actions;
    // The main window, which dialogs belong to.
    properdesktop::ui::Window& window;

    udisks::ViewMode viewMode = udisks::ViewMode::Volumes;
    // Built again whenever the model changes, before treeChanged.
    std::vector<udisks::SectionNodes> tree;

    // Selects the node by its id, as a click in the sidebar does.
    std::function<void(const std::string& id)> select;
    std::function<void(udisks::ViewMode mode)> setViewMode;
    // A short message over the window's foot.
    std::function<void(const std::string& message)> toast;
    // An alert for a failed operation: what was being done, and UDisks2's
    // message.
    std::function<void(const std::string& title, const std::string& message)> showError;

    properdesktop::base::Signal<> treeChanged;
};

} // namespace views
