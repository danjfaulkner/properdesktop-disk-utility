// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "views/toolbar.h"
#include "views/context.h"

#include "base/i18n.h"
#include "ui/color_mixer.h"
#include "ui/color_provider.h"
#include "ui/header_button.h"
#include "ui/header_button_group.h"
#include "ui/label.h"
#include "ui/theme.h"
#include "ui/window.h"
#include "ui/window_controls.h"
#include "ui/window_header.h"

#include <algorithm>
#include <cmath>

using namespace properdesktop::ui;
using properdesktop::i18n::tr;

namespace views {

static constexpr double kPlateGap = 8;
// Settings' header title: 10 points.
static constexpr double kTitleSize = 10 * 4.0 / 3;

static const char* glyphFor(Action action)
{
    switch (action) {
    case Action::View:
        return "view-sidebar";
    case Action::AddVolume:
        return "add";
    case Action::RemoveVolume:
        return "remove";
    case Action::FirstAid:
        return "first-aid";
    case Action::Partition:
        return "partition";
    case Action::Erase:
        return "erase";
    case Action::Restore:
        return "restore";
    case Action::Mount:
        return "mount";
    case Action::Unmount:
        return "eject";
    case Action::Info:
        return "get-info";
    default:
        return "";
    }
}

Toolbar::Toolbar(Context& context, const double& sidebarWidth)
    : m_context(context)
    , m_sidebarWidth(sidebarWidth)
{
    m_title = addChild<Label>(tr("Disk Utility"));
    m_title->setName("window:title");
    m_title->setFontSize(kTitleSize);
    m_title->setWeight(600);
    m_title->setElide(Label::Elide::Right);
    HeaderButtonGroup* volumes = nullptr;
    for (Action action : kToolbarActions) {
        if (action == Action::Unmount)
            continue;
        HeaderButtonGroup* plate = nullptr;
        if (action == Action::AddVolume || action == Action::RemoveVolume) {
            if (!volumes) {
                volumes = addChild<HeaderButtonGroup>();
                m_plates.push_back(volumes);
            }
            plate = volumes;
        } else {
            plate = addChild<HeaderButtonGroup>();
            m_plates.push_back(plate);
        }
        HeaderButton* button = plate->addButton(glyphFor(action));
        button->setName("toolbar:" + actionName(action));
        button->setToolTip(actionTitle(action));
        button->onClicked = [this, action] { triggered(action); };
        m_tools.push_back({action, plate, button});
    }
    refresh();
}

Toolbar::~Toolbar() = default;

HeaderButton* Toolbar::button(Action action) const
{
    for (const Tool& tool : m_tools) {
        if (tool.action == action)
            return tool.button;
    }
    return nullptr;
}

void Toolbar::triggered(Action action)
{
    if (action == Action::Mount && m_context.actions.isEnabled(Action::Unmount))
        action = Action::Unmount;
    m_context.actions.trigger(action);
}

void Toolbar::refresh()
{
    for (const Tool& tool : m_tools) {
        Action shown = tool.action;
        if (shown == Action::Mount && m_context.actions.isEnabled(Action::Unmount))
            shown = Action::Unmount;
        const ActionState state = m_context.actions.state(shown);
        tool.button->setEnabled(state.enabled);
        if (shown == Action::Mount || shown == Action::Unmount)
            tool.button->setGlyph(glyphFor(shown));
        tool.button->setToolTip(state.reason.empty() ? actionTitle(shown) : actionTitle(shown) + "\n" + state.reason);
    }
}

void Toolbar::layout()
{
    const auto* header = static_cast<const WindowHeader*>(parent());
    const double scale = window() ? window()->scale() : 1;
    const auto snap = [scale](double value) { return std::round(value * scale) / scale; };
    if (m_plates.empty())
        return;
    const double plateHeight = m_plates.front()->sizeHint().height;
    // Centred on the window buttons, and as far past the sidebar's edge as
    // from the window's top.
    const double top = header->buttonsTop() - (plateHeight - window_controls::kButton) / 2;
    const SizeF first = m_plates.front()->sizeHint();
    const double left = snap(m_sidebarWidth + top);
    m_plates.front()->setGeometry({left, top, first.width, first.height});

    double right = geometry().width - top;
    for (size_t i = m_plates.size(); i-- > 1;) {
        const SizeF size = m_plates[i]->sizeHint();
        right -= size.width;
        m_plates[i]->setGeometry({std::round(right), top, size.width, size.height});
        right -= kPlateGap;
    }

    // The title's capitals are centred on the plates, as Settings' are.
    const Theme& theme = Theme::current();
    const bool active = !window() || window()->isActive();
    m_title->setColor(active ? withAlpha(ColorId::Text, 0.85) : ColorRecipe(theme.disabledText));
    const double titleLeft = left + first.width + theme.smallSpacing;
    const double room = right - titleLeft;
    const SizeF title = m_title->sizeHint();
    m_title->setVisible(room >= std::min(title.width, 80.0));
    const double baseline = snap(top + plateHeight / 2 + m_title->capHeight() / 2);
    m_title->setGeometry({titleLeft, baseline - m_title->baselineOffset(), std::max(0.0, std::min(title.width, room)),
                          title.height});
}

} // namespace views
