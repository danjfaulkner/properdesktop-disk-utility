// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "udisks/properties.h"
#include "udisks/tree.h"

#include "base/signal.h"

#include <memory>
#include <string>
#include <vector>

namespace properdesktop::ui {
class DialogWindow;
class ScrollView;
}

namespace views {
struct Context;
}

namespace dialogs {

class SmartDialog;

// Get Info: every property UDisks2 has for the disk or volume, a card per
// interface, kept up to date while it is open. A drive with S.M.A.R.T. data
// opens its details from here.
class InfoWindow
{
public:
    InfoWindow(views::Context& context, udisks::Node node);
    ~InfoWindow();

    InfoWindow(const InfoWindow&) = delete;
    InfoWindow& operator=(const InfoWindow&) = delete;

    void show();
    bool isOpen() const;
    const udisks::Node& node() const { return m_node; }
    void openSmart();

private:
    void build();
    void schedule();

    views::Context& m_context;
    udisks::Node m_node;
    std::unique_ptr<properdesktop::ui::DialogWindow> m_dialog;
    properdesktop::ui::ScrollView* m_scroll = nullptr;
    std::vector<udisks::PropertyGroup> m_groups;
    std::unique_ptr<SmartDialog> m_smart;
    bool m_scheduled = false;
    std::vector<properdesktop::base::Subscription> m_subscriptions;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace dialogs
