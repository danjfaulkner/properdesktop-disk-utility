// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "views/detail.h"
#include "views/context.h"
#include "views/detailcards.h"
#include "views/style.h"

#include "udisks/model.h"

#include "base/i18n.h"
#include "base/timer.h"
#include "ui/cell_layout.h"
#include "ui/header_backdrop.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/scroll_bar.h"
#include "ui/scroll_view.h"
#include "ui/theme.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace properdesktop::ui;
using properdesktop::i18n::tr;
using namespace std::chrono_literals;

namespace views {

// "No Disks", in the middle of the pane.
class EmptyState : public View
{
public:
    EmptyState()
    {
        setName("detail:empty");
        m_title = addChild<Label>(tr("No Disks"));
        m_title->setFontSize(metrics::nameSize);
        m_title->setWeight(600);
        m_title->setTextAlignment(Label::TextAlignment::Center);
        m_note = addChild<Label>(tr("Disks, volumes and disk images show here when they are attached."));
        m_note->setFontSize(metrics::subtitleSize);
        m_note->setTextAlignment(Label::TextAlignment::Center);
        m_note->setWrap(true);
    }

protected:
    void layout() override
    {
        const double width = geometry().width;
        const double inner = std::max(0.0, std::min(width - 2 * metrics::pagePadding, 320.0));
        const double titleHeight = m_title->sizeHint().height;
        const double noteHeight = m_note->heightForWidth(inner);
        // Centred under the header, on the window's middle.
        double y = std::round((geometry().height - titleHeight - 4 - noteHeight) / 2);
        const double left = std::round((width - inner) / 2);
        m_title->setColor(palette::muted());
        m_title->setGeometry({left, y, inner, titleHeight});
        y += titleHeight + 4;
        m_note->setColor(palette::faint());
        m_note->setGeometry({left, y, inner, noteHeight});
    }

private:
    Label* m_title;
    Label* m_note;
};

DetailPane::DetailPane(Context& context)
    : m_context(context)
    , m_timer(std::make_unique<properdesktop::base::Timer>())
{
    m_scroll = addChild<ScrollView>();
    m_scroll->setName("detail:scroll");
    m_scroll->setVerticalBarPadding(metrics::headerHeight + ScrollBar::inset, ScrollBar::inset);
    m_scroll->setBottomCornerRadius(Theme::current().windowRadius);
    m_backdrop = addChild<HeaderBackdrop>(m_scroll);
    m_backdrop->setSeparatorVisible(false);
    m_empty = addChild<EmptyState>();
    // What a filesystem holds changes without UDisks2 saying so.
    m_timer->onTimeout = [this] { refresh(); };
    m_timer->start(5s);
    refresh();
}

DetailPane::~DetailPane() = default;

void DetailPane::setSelection(const Selection& selection)
{
    m_selection = selection;
    refresh();
}

void DetailPane::refresh()
{
    std::optional<DetailContent> content;
    if (m_selection)
        content = describeDetail(m_context.model, *m_selection);
    if (content && content->name.empty())
        content.reset();
    if (m_shown == content && m_scroll->contentView())
        return;
    m_shown = content;

    m_empty->setVisible(!content);
    m_empty->requestLayout();
    if (!content) {
        m_scroll->setContent<View>();
        requestLayout();
        return;
    }
    auto* column = m_scroll->setContent<ColumnLayout>();
    column->setSpacing(metrics::cardSpacing);
    column->setPadding({metrics::pagePadding, metrics::headerHeight + metrics::pageTopMargin, metrics::pagePadding,
                        metrics::pagePadding});
    column->addChild<SummaryCard>(*content);
    if (!content->left.empty() || !content->right.empty())
        column->addChild<InfoGrid>(content->left, content->right);
    requestLayout();
}

void DetailPane::layout()
{
    const Theme& theme = Theme::current();
    m_backdrop->setBaseColor(theme.background);
    m_scroll->setGeometry(localRect());
    m_empty->setGeometry(localRect());
    m_backdrop->setGeometry({0, 0, geometry().width, metrics::headerHeight});
}

void DetailPane::paint(Painter& painter)
{
    painter.fillRect(localRect(), palette::page());
}

} // namespace views
