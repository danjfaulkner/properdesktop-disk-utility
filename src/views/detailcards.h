// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "views/detailcontent.h"

#include "ui/view.h"

#include <vector>

namespace properdesktop::ui {
class Label;
class ProgressBar;
class ThemeIcon;
}

namespace views {

// The space as one rounded bar of segments, a hairline of the card between
// neighbours. Named "detail:usage".
class UsageBar : public properdesktop::ui::View
{
public:
    explicit UsageBar(std::vector<UsageSegment> segments);

    properdesktop::ui::SizeF sizeHint() const override;

protected:
    void paint(properdesktop::ui::Painter& painter) override;

private:
    std::vector<UsageSegment> m_segments;
};

// The colour of a segment, on the bar and in the legend.
properdesktop::ui::Color segmentColor(const UsageSegment& segment);

// The card at the top of the detail pane: the large icon, the name and the
// subtitle, the usage bar with its legend, and the job working on the
// selection with its progress.
class SummaryCard : public properdesktop::ui::View
{
public:
    explicit SummaryCard(const DetailContent& content);

    double heightForWidth(double width) const override;
    properdesktop::ui::SizeF sizeHint() const override { return {0, heightForWidth(0)}; }

protected:
    void layout() override;
    void paint(properdesktop::ui::Painter& painter) override;

private:
    class LegendItem;

    // The legend's rows, laid out for a width: where each item goes.
    std::vector<properdesktop::ui::RectF> legendPlaces(double width) const;
    double headTop() const;

    properdesktop::ui::ThemeIcon* m_icon;
    properdesktop::ui::Label* m_name;
    properdesktop::ui::Label* m_subtitle;
    UsageBar* m_bar = nullptr;
    std::vector<LegendItem*> m_legend;
    properdesktop::ui::Label* m_job = nullptr;
    properdesktop::ui::Label* m_percent = nullptr;
    properdesktop::ui::ProgressBar* m_progress = nullptr;
};

// The information grid in Settings' card rows, two columns side by side
// when there is room, a hairline between rows and between the columns.
class InfoGrid : public properdesktop::ui::View
{
public:
    InfoGrid(const std::vector<InfoItem>& left, const std::vector<InfoItem>& right);

    double heightForWidth(double width) const override;
    properdesktop::ui::SizeF sizeHint() const override { return {0, heightForWidth(0)}; }

protected:
    void layout() override;
    void paint(properdesktop::ui::Painter& painter) override;

private:
    struct Cell
    {
        properdesktop::ui::Label* title;
        properdesktop::ui::Label* value;
    };

    bool twoColumns(double width) const;
    size_t rowCount(double width) const;

    std::vector<Cell> m_left;
    std::vector<Cell> m_right;
};

} // namespace views
