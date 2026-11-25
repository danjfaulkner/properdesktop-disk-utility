// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "actions.h"

#include "ui/view.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace properdesktop::ui {
class Menu;
class ScrollView;
}

namespace views {

struct Context;
class SidebarList;
class SidebarRow;

// The window's left column: Internal, External and Disk Images, each with
// its disks and volumes from Context::tree. A row is named "sidebar:" and
// its node's id, its disclosure triangle "sidebar:disclosure:" and the id,
// its eject button "sidebar:eject:" and the id; a heading
// "sidebar:section:<n>". A click selects through Context::select; Up and
// Down move the selection, Left and Right fold and unfold a disk; a right
// click opens the actions' menu over the row.
class Sidebar : public properdesktop::ui::View
{
public:
    explicit Sidebar(Context& context);
    ~Sidebar() override;

    // Context::tree changed.
    void reload();
    // The selected node's id; empty for none.
    void setCurrent(const std::string& id);

    // Folds or unfolds a node with children, in Show All Devices.
    void setExpanded(const std::string& id, bool expanded);
    bool isExpanded(const std::string& id) const;

    // The rows of the context menu last opened, in order, and whether each
    // was enabled when it opened.
    struct MenuEntry
    {
        Action action;
        bool enabled;
    };
    const std::vector<MenuEntry>& menuEntries() const { return m_menuEntries; }

    bool acceptsFocus() const override { return true; }

protected:
    void layout() override;
    void paint(properdesktop::ui::Painter& painter) override;
    bool keyPress(const properdesktop::ui::KeyEvent& event) override;

private:
    friend class SidebarRow;

    void rowPressed(SidebarRow* row);
    void ejectPressed(SidebarRow* row);
    void openMenu(SidebarRow* row, properdesktop::ui::PointF position);
    // The rows shown, in order.
    std::vector<SidebarRow*> shownRows() const;
    void moveTo(SidebarRow* row);
    void updateFolding();

    Context& m_context;
    properdesktop::ui::ScrollView* m_scroll;
    SidebarList* m_list;
    std::vector<SidebarRow*> m_rows;
    std::string m_current;
    std::set<std::string> m_collapsed;
    std::unique_ptr<properdesktop::ui::Menu> m_menu;
    std::vector<MenuEntry> m_menuEntries;
};

} // namespace views
