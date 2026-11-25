// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "actions.h"
#include "views/detailcontent.h"

#include "ui/view.h"

#include <memory>
#include <optional>

namespace properdesktop::base {
class Timer;
}

namespace properdesktop::ui {
class HeaderBackdrop;
class ScrollView;
}

namespace views {

struct Context;
class EmptyState;

// The selected disk or volume, as a Settings page: a card with its icon,
// name and summary, the usage bar and the job working on it, and the
// information grid below. Its views are named "detail:<thing>"
// (detailcontent.h and detailcards.h); "detail:empty" while there is no
// disk. The usage is read again every few seconds; nothing is built again
// unless something shown changed.
class DetailPane : public properdesktop::ui::View
{
public:
    explicit DetailPane(Context& context);
    ~DetailPane() override;

    // The selection changed, or the disks did.
    void setSelection(const Selection& selection);

protected:
    void layout() override;
    void paint(properdesktop::ui::Painter& painter) override;

private:
    void refresh();

    Context& m_context;
    Selection m_selection;
    std::optional<DetailContent> m_shown;
    properdesktop::ui::ScrollView* m_scroll;
    properdesktop::ui::HeaderBackdrop* m_backdrop;
    EmptyState* m_empty;
    std::unique_ptr<properdesktop::base::Timer> m_timer;
};

} // namespace views
