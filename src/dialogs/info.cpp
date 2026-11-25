// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/info.h"

#include "dialogs/cards.h"
#include "dialogs/smart.h"
#include "views/context.h"

#include "udisks/describe.h"
#include "udisks/model.h"
#include "udisks/properties.h"

#include "base/event_loop.h"
#include "base/i18n.h"
#include "ui/color_provider.h"
#include "ui/dialog_window.h"
#include "ui/keys.h"
#include "ui/window.h"

#include <algorithm>
#include <set>

using namespace properdesktop::ui;
using properdesktop::i18n::tr;

namespace dialogs {

static constexpr double kWidth = 480;
static constexpr double kHeight = 600;

InfoWindow::InfoWindow(views::Context& context, udisks::Node node)
    : m_context(context)
    , m_node(std::move(node))
{
    m_dialog = std::make_unique<DialogWindow>(&context.window, udisks::displayName(context.model, m_node), kWidth);
    m_dialog->setHeight(kHeight);
    m_dialog->setEdgeToEdge(true);
    m_dialog->window()->setBackgroundColor(ColorId::Background);
    m_dialog->window()->addShortcut(NoModifier, key::Escape, [this] { m_dialog->close(); });
    m_dialog->onClosed = [this] { m_subscriptions.clear(); };
    m_subscriptions.push_back(context.model.changed.connect([this] { schedule(); }));
    build();
}

InfoWindow::~InfoWindow() = default;

void InfoWindow::show()
{
    m_dialog->show();
}

bool InfoWindow::isOpen() const
{
    return m_dialog->window() != nullptr;
}

void InfoWindow::schedule()
{
    if (m_scheduled)
        return;
    m_scheduled = true;
    properdesktop::base::EventLoop::current()->post([this, alive = std::weak_ptr<bool>(m_alive)] {
        if (alive.expired())
            return;
        m_scheduled = false;
        if (!isOpen())
            return;
        // Gone with its disk.
        if (!m_context.model.object(m_node.block)) {
            m_dialog->close();
            return;
        }
        build();
    });
}

static std::string lastPart(const std::string& interface)
{
    const size_t dot = interface.rfind('.');
    return dot == std::string::npos ? interface : interface.substr(dot + 1);
}

void InfoWindow::build()
{
    std::vector<udisks::PropertyGroup> groups = udisks::describeProperties(m_context.model, m_node);
    // Most changes on the bus are to other objects, or to a job's progress.
    if (m_scroll && groups == m_groups)
        return;
    m_groups = std::move(groups);
    auto* content = m_dialog->setContent<ScrollingForm>("info:scroll");
    m_scroll = content->scroll;
    Form* form = content->form;

    std::set<std::string> seen;
    for (const udisks::PropertyGroup& group : m_groups) {
        Card* card = form->addSection(group.title);
        // An unlocked volume's cleartext block follows the encrypted one.
        const std::string prefix = (seen.insert(group.interface).second ? "info:" : "info:Unlocked")
                + lastPart(group.interface);
        for (const udisks::Property& property : group.properties) {
            auto* row = card->addChild<InfoRow>(property.name, property.value);
            row->setName(prefix + "." + property.key);
        }
    }

    const udisks::Object* drive = m_context.model.object(m_node.drive);
    if (m_node.kind == udisks::NodeKind::Disk && drive && (drive->ata || drive->nvme)) {
        auto* smart = content->footer->addEnd<DialogButton>(tr("S.M.A.R.T. Details…"));
        smart->setName("info:smart");
        smart->onClicked = [this] { openSmart(); };
    }
}

void InfoWindow::openSmart()
{
    if (m_smart && m_smart->isOpen())
        return;
    m_smart = std::make_unique<SmartDialog>(m_context, m_dialog->window(), m_node.drive);
    m_smart->show();
}

} // namespace dialogs
