// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "views/diskwindow.h"
#include "views/context.h"
#include "views/detail.h"
#include "views/sidebar.h"
#include "views/style.h"
#include "views/toolbar.h"

#include "application.h"
#include "dialogs/diskimages.h"
#include "dialogs/volume_actions.h"
#include "dialogs/volumetools.h"
#include "udisks/describe.h"
#include "udisks/model.h"
#include "udisks/operations.h"

#include "base/event_loop.h"
#include "base/i18n.h"
#include "base/settings.h"
#include "ui/about_dialog.h"
#include "ui/application.h"
#include "ui/color_provider.h"
#include "ui/dialog_window.h"
#include "ui/header_button.h"
#include "ui/keys.h"
#include "ui/menu.h"
#include "ui/sidebar_divider.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/window.h"
#include "ui/window_header.h"

#include <algorithm>
#include <cmath>

using namespace properdesktop::ui;
using properdesktop::i18n::tr;

namespace views {

static constexpr const char* kSidebarWidthKey = "Sidebar/Width";
static constexpr const char* kAllDevicesKey = "View/AllDevices";

static void relayoutTree(View* view)
{
    view->requestLayout();
    view->requestPaint();
    for (const auto& child : view->children())
        relayoutTree(child.get());
}

// The sidebar's width on whole device pixels, so its edge and the page's
// clip share one.
static double snappedWidth(double width, double scale)
{
    return std::round(width * scale) / scale;
}

class DiskRoot : public View
{
public:
    DiskRoot(Context& context, const std::string& configPath)
        : m_configPath(configPath)
    {
        sidebar = addChild<Sidebar>(context);
        sidebar->setName("sidebar");
        detail = addChild<DetailPane>(context);
        detail->setName("detail");
        header = addChild<WindowHeader>(metrics::headerHeight);
        toolbar = header->setContent<Toolbar>(context, sidebarWidth);
        header->setContentUnderButtons(true);
        divider = addChild<SidebarDivider>(metrics::sidebarMinimumWidth, metrics::sidebarMaximumWidth,
                                           metrics::sidebarWidth);
        divider->setName("sidebarDivider");
        if (!m_configPath.empty()) {
            const properdesktop::base::Settings settings(m_configPath);
            sidebarWidth = std::clamp(settings.doubleValue(kSidebarWidthKey, metrics::sidebarWidth),
                                      metrics::sidebarMinimumWidth, metrics::sidebarMaximumWidth);
        }
        divider->onResized = [this](double width) {
            sidebarWidth = width;
            requestLayout();
            if (onSidebarResized)
                onSidebarResized();
        };
        divider->onFinished = [this](double width) {
            sidebarWidth = width;
            requestLayout();
            if (!m_configPath.empty()) {
                properdesktop::base::Settings settings(m_configPath);
                settings.setValue(kSidebarWidthKey, width);
            }
            if (onSidebarResized)
                onSidebarResized();
        };
    }

    std::function<void()> onSidebarResized;
    std::function<void()> onLayout;
    double sidebarWidth = metrics::sidebarWidth;

    Sidebar* sidebar;
    DetailPane* detail;
    WindowHeader* header;
    Toolbar* toolbar;
    SidebarDivider* divider;

protected:
    void layout() override
    {
        const double width = geometry().width;
        const double height = geometry().height;
        const double left = snappedWidth(sidebarWidth, window() ? window()->scale() : 1);
        sidebar->setGeometry({0, 0, left, height});
        divider->setSidebarWidth(left);
        divider->setGeometry({left - SidebarDivider::kWidth / 2, 0, SidebarDivider::kWidth, height});
        detail->setGeometry({left, 0, std::max(0.0, width - left), height});
        toolbar->requestLayout();
        header->setGeometry({0, 0, width, metrics::headerHeight});
        if (onLayout)
            onLayout();
    }

private:
    std::string m_configPath;
};

DiskWindow::DiskWindow(::Application& application)
    : m_application(application)
    , m_window(std::make_unique<Window>())
    , m_actions(std::make_unique<ActionDispatcher>(application.model(), application.operations()))
    , m_context(std::make_unique<Context>(application, *m_window, *m_actions))
{
    m_window->setTitle(tr("Disk Utility"));
    m_window->resize({metrics::windowWidth, metrics::windowHeight});
    m_window->setMinimumSize({metrics::windowMinimumWidth, metrics::windowMinimumHeight});
    m_window->setRestoresSize("main");
    m_window->onCloseRequested = [this] { requestQuit(); };
    m_toast = std::make_unique<Toast>(m_window.get());

    if (!application.configPath().empty()) {
        const properdesktop::base::Settings settings(application.configPath());
        m_context->viewMode = settings.boolValue(kAllDevicesKey) ? udisks::ViewMode::AllDevices
                                                                 : udisks::ViewMode::Volumes;
    }
    m_context->select = [this](const std::string& id) { select(id); };
    m_context->setViewMode = [this](udisks::ViewMode mode) { setViewMode(mode); };
    m_context->toast = [this](const std::string& message) { m_toast->show(message); };
    m_context->showError = [this](const std::string& title, const std::string& message) { showError(title, message); };

    auto root = std::make_unique<DiskRoot>(*m_context, application.configPath());
    m_root = root.get();
    m_window->setContentView(std::move(root));

    const auto updateBackground = [this] {
        m_window->setBackgroundColor(color(m_window->isActive() ? ColorId::TranslucentWindowBackground
                                                                : ColorId::TranslucentWindowBackgroundInactive));
        const SizeF size = m_window->size();
        m_window->setBackgroundBlur({0, 0, size.width, size.height}, Theme::current().windowRadius);
    };
    m_root->onSidebarResized = updateBackground;
    m_root->onLayout = updateBackground;
    m_subscriptions.push_back(Theme::changed().connect([this, updateBackground] {
        updateBackground();
        relayoutTree(m_root);
        m_root->sidebar->reload();
        updateSelection();
    }));
    m_window->onActiveChanged = [this, updateBackground] {
        updateBackground();
        m_window->requestLayout();
        m_root->toolbar->requestLayout();
        m_root->requestPaint();
    };
    m_window->onStatesChanged = [this, updateBackground] {
        updateBackground();
        m_root->requestLayout();
    };
    updateBackground();

    m_actions->setHandler(Action::View, [this](const Selection&) { showViewMenu(); });
    m_diskImages = std::make_unique<dialogs::DiskImages>(*m_context);
    m_volumeTools = std::make_unique<dialogs::VolumeTools>(*m_context);
    dialogs::installVolumeActions(*m_context);
    m_subscriptions.push_back(m_actions->statesChanged.connect([this] { m_root->toolbar->refresh(); }));
    m_subscriptions.push_back(application.model().changed.connect([this] { scheduleRebuild(); }));
    // Only an action whose state was out of date can reach a refusal.
    m_subscriptions.push_back(application.operations().finished.connect(
        [this](const udisks::Call&, const udisks::Result& result) {
            if (result.errorName == udisks::Operations::kSystemError)
                showError(tr("The operation was refused."), result.errorMessage);
        }));
    m_subscriptions.push_back(application.activateRequested.connect([this](const std::string& token) { activate(token); }));
    m_subscriptions.push_back(properdesktop::ui::Application::instance()->languageChanged.connect([this] {
        buildMainMenu();
        m_root->sidebar->reload();
        updateSelection();
    }));

    bindShortcuts();
    buildMainMenu();
    rebuildTree();
    m_root->sidebar->setFocus(FocusReason::Other);
}

DiskWindow::~DiskWindow() = default;

void DiskWindow::show()
{
    m_window->show();
}

void DiskWindow::activate(const std::string& token)
{
    m_window->activate(token);
}

void DiskWindow::scheduleRebuild()
{
    if (m_rebuildScheduled)
        return;
    m_rebuildScheduled = true;
    // Several signals arrive together for one change on the disk.
    properdesktop::base::EventLoop::current()->post([this, alive = std::weak_ptr<bool>(m_alive)] {
        if (alive.expired())
            return;
        m_rebuildScheduled = false;
        rebuildTree();
    });
}

static const udisks::Node* firstNode(const std::vector<udisks::SectionNodes>& tree)
{
    for (const udisks::SectionNodes& section : tree) {
        if (!section.nodes.empty())
            return &section.nodes.front();
    }
    return nullptr;
}

static const udisks::Node* nodeIn(const std::vector<udisks::Node>& nodes, const std::string& disk)
{
    for (const udisks::Node& node : nodes) {
        if (node.disk == disk)
            return &node;
        if (const udisks::Node* found = nodeIn(node.children, disk))
            return found;
    }
    return nullptr;
}

static const udisks::Node* nodeOfDisk(const std::vector<udisks::SectionNodes>& tree, const std::string& disk)
{
    for (const udisks::SectionNodes& section : tree) {
        if (const udisks::Node* found = nodeIn(section.nodes, disk))
            return found;
    }
    return nullptr;
}

void DiskWindow::rebuildTree()
{
    m_rebuildScheduled = false;
    m_context->tree = udisks::buildTree(m_application.model(), m_context->viewMode);
    if (!udisks::findNode(m_context->tree, m_selected)) {
        // The same block in the other view, as its disk or a volume of it.
        std::string block = m_selected.size() > 2 ? m_selected.substr(2) : std::string();
        const udisks::Node* same = nullptr;
        for (const char* prefix : {"v:", "d:", "p:", "c:"}) {
            if (!same && !block.empty())
                same = udisks::findNode(m_context->tree, prefix + block);
        }
        // Then its disk, or another volume of it.
        if (!same && !m_selectedDisk.empty())
            same = udisks::findNode(m_context->tree, "d:" + m_selectedDisk);
        if (!same && !m_selectedDisk.empty())
            same = nodeOfDisk(m_context->tree, m_selectedDisk);
        if (!same)
            same = firstNode(m_context->tree);
        m_selected = same ? same->id : std::string();
    }
    m_root->sidebar->reload();
    updateSelection();
    m_context->treeChanged.emit();
}

void DiskWindow::updateSelection()
{
    const udisks::Node* node = udisks::findNode(m_context->tree, m_selected);
    Selection selection;
    if (node) {
        selection = *node;
        selection->children.clear();
        m_selectedDisk = node->disk;
    }
    m_root->sidebar->setCurrent(m_selected);
    m_root->detail->setSelection(selection);
    m_actions->setSelection(selection);
    m_root->toolbar->refresh();
    m_window->setTitle(node ? udisks::displayName(m_application.model(), *node) : tr("Disk Utility"));
}

void DiskWindow::select(const std::string& id)
{
    if (!udisks::findNode(m_context->tree, id) || id == m_selected)
        return;
    m_selected = id;
    updateSelection();
}

void DiskWindow::setViewMode(udisks::ViewMode mode)
{
    if (mode == m_context->viewMode)
        return;
    m_context->viewMode = mode;
    if (!m_application.configPath().empty()) {
        properdesktop::base::Settings settings(m_application.configPath());
        settings.setValue(kAllDevicesKey, mode == udisks::ViewMode::AllDevices);
    }
    rebuildTree();
}

void DiskWindow::showViewMenu()
{
    m_viewMenu = std::make_unique<Menu>();
    const size_t volumes = m_viewMenu->addItem(tr("Show Only Volumes"),
                                               [this] { setViewMode(udisks::ViewMode::Volumes); });
    const size_t all = m_viewMenu->addItem(tr("Show All Devices"),
                                           [this] { setViewMode(udisks::ViewMode::AllDevices); });
    m_viewMenu->setItemChecked(volumes, m_context->viewMode == udisks::ViewMode::Volumes);
    m_viewMenu->setItemChecked(all, m_context->viewMode == udisks::ViewMode::AllDevices);
    if (HeaderButton* button = m_root->toolbar->button(Action::View))
        m_viewMenu->popupFor(button);
}

void DiskWindow::requestQuit()
{
    if (!m_application.operations().isAnyBusy()) {
        properdesktop::ui::Application::instance()->quit();
        return;
    }
    if (m_quitConfirm && m_quitConfirm->window()) {
        m_quitConfirm->window()->activate();
        return;
    }
    m_quitConfirm = std::make_unique<ConfirmDialog>(
        m_window.get(), tr("An operation is in progress."),
        tr("Quitting now stops it partway, which can leave the disk unusable. Wait until it is done."));
    m_quitConfirm->setCancelText(tr("Keep Running"));
    m_quitConfirm->setConfirmText(tr("Quit Anyway"));
    m_quitConfirm->setDestructive(true);
    m_quitConfirm->onAccepted = [] { properdesktop::ui::Application::instance()->quit(); };
    m_quitConfirm->show();
}

void DiskWindow::showError(const std::string& title, const std::string& message)
{
    m_error = std::make_unique<ConfirmDialog>(m_window.get(), title, message);
    m_error->setConfirmText(tr("OK"));
    m_error->setCancelVisible(false);
    m_error->show();
}

void DiskWindow::bindShortcuts()
{
    Window& window = *m_window;
    window.addShortcut(ControlModifier, '1', [this] { setViewMode(udisks::ViewMode::Volumes); });
    window.addShortcut(ControlModifier, '2', [this] { setViewMode(udisks::ViewMode::AllDevices); });
    window.addShortcut(ControlModifier, key::letter('i'), [this] { m_actions->trigger(Action::Info); });
    window.addShortcut(ControlModifier, key::letter('e'), [this] { m_actions->trigger(Action::Eject); });
    window.addShortcut(ControlModifier, key::letter('o'), [this] { m_actions->trigger(Action::AttachImage); });
    window.addShortcut(ControlModifier, key::letter('w'), [this] { requestQuit(); });
    window.addShortcut(ControlModifier, key::letter('q'), [this] { requestQuit(); });
}

void DiskWindow::openAbout()
{
    if (!m_about) {
        m_about = std::make_unique<AboutDialog>(m_window.get());
        m_about->setName(tr("Disk Utility"));
        m_about->setDescription(tr("Erases, partitions, checks and images the disks of your computer."));
        m_about->setIconSource("gnome-disks");
    }
    m_about->show();
}

void DiskWindow::buildMainMenu()
{
    m_window->setMainMenu(nullptr);
    m_mainMenu = std::make_unique<Menu>();
    const uint32_t ctrl = ControlModifier;

    // An item per action: its text and, when it has one, its key; enabled
    // as the dispatcher says each time its menu opens.
    struct Item
    {
        Menu* menu;
        size_t index;
        Action action;
    };
    auto items = std::make_shared<std::vector<Item>>();
    const auto addAction = [this, items](Menu* menu, Action action, const char* key = nullptr,
                                         uint32_t modifiers = ControlModifier) {
        const size_t index = menu->addItem(actionTitle(action), [this, action] { m_actions->trigger(action); });
        if (key)
            menu->setItemKeyEquivalent(index, key, modifiers);
        items->push_back({menu, index, action});
    };
    const auto refresh = [this, items](Menu* menu) {
        for (const Item& item : *items) {
            if (item.menu == menu)
                menu->setItemEnabled(item.index, m_actions->isEnabled(item.action));
        }
    };

    Menu* application = m_mainMenu->addSubmenu(tr("Disk Utility"));
    application->setRole(MenuRole::Application);
    application->addItem(tr("About Disk Utility"), [this] { openAbout(); });
    application->addSeparator();
    application->addItem(tr("Hide Disk Utility"), [this] { m_window->hideApplication(); });
    application->addSeparator();
    const size_t quit = application->addItem(tr("Quit Disk Utility"),
                                             [this] { requestQuit(); });
    application->setItemKeyEquivalent(quit, "q", ctrl);

    Menu* file = m_mainMenu->addSubmenu(tr("File"));
    Menu* newImage = file->addSubmenu(tr("New Image"));
    addAction(newImage, Action::NewBlankImage);
    addAction(newImage, Action::NewImageFromSelection);
    newImage->onAboutToShow = [newImage, refresh] { refresh(newImage); };
    addAction(file, Action::AttachImage, "o");
    file->addSeparator();
    addAction(file, Action::Info, "i");
    addAction(file, Action::Eject, "e");
    file->addSeparator();
    const size_t close = file->addItem(tr("Close Window"), [this] { requestQuit(); });
    file->setItemKeyEquivalent(close, "w", ctrl);
    file->onAboutToShow = [file, refresh] { refresh(file); };

    Menu* edit = m_mainMenu->addSubmenu(tr("Edit"));
    addAction(edit, Action::AddVolume);
    addAction(edit, Action::RemoveVolume);
    edit->addSeparator();
    addAction(edit, Action::FirstAid);
    addAction(edit, Action::Partition);
    addAction(edit, Action::Erase);
    addAction(edit, Action::Restore);
    addAction(edit, Action::Rename);
    edit->addSeparator();
    addAction(edit, Action::Mount);
    addAction(edit, Action::Unmount);
    addAction(edit, Action::Lock);
    edit->onAboutToShow = [edit, refresh] { refresh(edit); };

    Menu* view = m_mainMenu->addSubmenu(tr("View"));
    const size_t volumes = view->addItem(tr("Show Only Volumes"), [this] { setViewMode(udisks::ViewMode::Volumes); });
    view->setItemKeyEquivalent(volumes, "1", ctrl);
    const size_t all = view->addItem(tr("Show All Devices"), [this] { setViewMode(udisks::ViewMode::AllDevices); });
    view->setItemKeyEquivalent(all, "2", ctrl);
    view->onAboutToShow = [this, view, volumes, all] {
        view->setItemChecked(volumes, m_context->viewMode == udisks::ViewMode::Volumes);
        view->setItemChecked(all, m_context->viewMode == udisks::ViewMode::AllDevices);
    };

    Menu* windowMenu = m_mainMenu->addSubmenu(tr("Window"));
    windowMenu->setRole(MenuRole::Windows);
    windowMenu->addItem(tr("Minimize"), [this] { m_window->minimize(); });
    windowMenu->addItem(tr("Zoom"), [this] { m_window->setMaximized(!m_window->states().maximized); });

    m_window->setMainMenu(m_mainMenu.get());
}

} // namespace views
