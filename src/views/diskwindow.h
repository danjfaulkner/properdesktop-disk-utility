// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "actions.h"
#include "udisks/tree.h"

#include "base/signal.h"

#include <memory>
#include <string>
#include <vector>

class Application;

namespace properdesktop::ui {
class AboutDialog;
class ConfirmDialog;
class Menu;
class Toast;
class Window;
}

namespace dialogs {
class DiskImages;
class VolumeTools;
}

namespace views {

class DiskRoot;
struct Context;

// The window: the sidebar of disks and volumes, the selection's detail pane
// and the header's tools. The window's own actions (View, Info's fallback)
// are set here; Erase, Partition, First Aid, Restore, the disk images and
// the rest set their handlers on actions() when they are built.
class DiskWindow
{
public:
    explicit DiskWindow(Application& application);
    ~DiskWindow();

    DiskWindow(const DiskWindow&) = delete;
    DiskWindow& operator=(const DiskWindow&) = delete;

    void show();
    void activate(const std::string& token);
    properdesktop::ui::Window& window() { return *m_window; }
    Context& context() { return *m_context; }
    ActionDispatcher& actions() { return *m_actions; }
    dialogs::DiskImages& diskImages() { return *m_diskImages; }

    void select(const std::string& id);
    // Quits, after asking while an operation is running: the window's close
    // control, Quit, Close Window and the dock's Quit all come here.
    void requestQuit();
    void setViewMode(udisks::ViewMode mode);

private:
    void rebuildTree();
    void scheduleRebuild();
    void updateSelection();
    void bindShortcuts();
    void buildMainMenu();
    void openAbout();
    void showViewMenu();
    void showError(const std::string& title, const std::string& message);

    Application& m_application;
    std::unique_ptr<properdesktop::ui::Window> m_window;
    std::unique_ptr<ActionDispatcher> m_actions;
    std::unique_ptr<Context> m_context;
    DiskRoot* m_root;
    std::string m_selected;
    // The selected node's disk, which the selection falls back to when the
    // node goes.
    std::string m_selectedDisk;
    bool m_rebuildScheduled = false;
    std::unique_ptr<properdesktop::ui::Toast> m_toast;
    std::unique_ptr<properdesktop::ui::AboutDialog> m_about;
    std::unique_ptr<properdesktop::ui::ConfirmDialog> m_error;
    std::unique_ptr<properdesktop::ui::ConfirmDialog> m_quitConfirm;
    std::unique_ptr<properdesktop::ui::Menu> m_mainMenu;
    std::unique_ptr<properdesktop::ui::Menu> m_viewMenu;
    std::unique_ptr<dialogs::DiskImages> m_diskImages;
    std::unique_ptr<dialogs::VolumeTools> m_volumeTools;
    std::vector<properdesktop::base::Subscription> m_subscriptions;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace views
