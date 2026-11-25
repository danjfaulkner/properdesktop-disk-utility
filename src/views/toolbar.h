// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "actions.h"

#include "ui/view.h"

#include <vector>

namespace properdesktop::ui {
class HeaderButton;
class HeaderButtonGroup;
class Label;
}

namespace views {

struct Context;

// The header's tools, on the window buttons' line: View after the sidebar,
// then the title ("window:title") as a Settings page's, the rest at the
// end, Add and Delete Volume on one plate. One button per action of
// kToolbarActions, named "toolbar:<action>", enabled as the dispatcher
// says, its tool tip the action and why it is disabled; Mount and Unmount
// share a button, which shows Unmount while that is possible. The title
// gives way when the tools leave it no room.
class Toolbar : public properdesktop::ui::View
{
public:
    Toolbar(Context& context, const double& sidebarWidth);
    ~Toolbar() override;

    // The dispatcher's states changed.
    void refresh();
    // Null for Unmount, which is shown on Mount's button.
    properdesktop::ui::HeaderButton* button(Action action) const;

protected:
    void layout() override;

private:
    struct Tool
    {
        Action action;
        properdesktop::ui::HeaderButtonGroup* plate;
        properdesktop::ui::HeaderButton* button;
    };

    void triggered(Action action);

    Context& m_context;
    const double& m_sidebarWidth;
    properdesktop::ui::Label* m_title;
    std::vector<Tool> m_tools;
    std::vector<properdesktop::ui::HeaderButtonGroup*> m_plates;
};

} // namespace views
