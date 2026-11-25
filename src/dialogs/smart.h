// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "base/signal.h"
#include "base/dbus_types.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace properdesktop::ui {
class DialogButton;
class DialogWindow;
class Window;
}

namespace views {
struct Context;
}

namespace dialogs {

class Footnote;
struct SmartPage;

// A drive's S.M.A.R.T. data: its status, temperature, hours powered on and
// self-test, then an ATA drive's attributes or an NVMe drive's health log,
// read once it opens. A short self-test starts from its footer and its
// progress shows as UDisks2 reports it.
class SmartDialog
{
public:
    SmartDialog(views::Context& context, properdesktop::ui::Window* parent, std::string drive);
    ~SmartDialog();

    SmartDialog(const SmartDialog&) = delete;
    SmartDialog& operator=(const SmartDialog&) = delete;

    void show();
    bool isOpen() const;

private:
    void readAttributes();
    void startSelftest();
    void schedule();
    void build();

    views::Context& m_context;
    std::string m_drive;
    std::unique_ptr<properdesktop::ui::DialogWindow> m_dialog;
    // The last SmartGetAttributes answer; none before it came.
    std::optional<properdesktop::base::dbus::Variant::Items> m_attributes;
    std::string m_error;
    // What shows now.
    std::shared_ptr<SmartPage> m_built;
    bool m_starting = false;
    bool m_scheduled = false;
    std::vector<properdesktop::base::Subscription> m_subscriptions;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace dialogs
