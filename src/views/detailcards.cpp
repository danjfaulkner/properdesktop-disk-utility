// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "views/detailcards.h"
#include "views/style.h"

#include "udisks/describe.h"

#include "base/i18n.h"
#include "ui/color_provider.h"
#include "ui/icon.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/progress_bar.h"
#include "ui/theme.h"

#include <algorithm>
#include <cmath>

using namespace properdesktop::ui;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace views {

static constexpr double kIconGap = 14;
static constexpr double kNameGap = 2;
static constexpr double kBarGap = 16;
static constexpr double kLegendGap = 12;
static constexpr double kLegendSpacing = 24;
static constexpr double kLegendRowSpacing = 10;
static constexpr double kLegendTextGap = 6;
static constexpr double kJobGap = 16;
static constexpr double kProgressGap = 6;
static constexpr double kProgressHeight = 6;
static constexpr double kInfoTitleGap = 12;

static void paintCard(Painter& painter, const RectF& rect)
{
    painter.drawRoundedRect(rect, {Theme::current().bigRadius, 0, themeCorners()}, palette::card());
}

Color segmentColor(const UsageSegment& segment)
{
    switch (segment.kind) {
    case UsageSegment::Kind::Used:
        return palette::accent();
    case UsageSegment::Kind::Other:
        return palette::otherSpace();
    case UsageSegment::Kind::Free:
        return palette::freeSpace();
    case UsageSegment::Kind::Partition:
        return palette::partition(segment.index);
    }
    return palette::freeSpace();
}

UsageBar::UsageBar(std::vector<UsageSegment> segments)
    : m_segments(std::move(segments))
{
    setName("detail:usage");
}

SizeF UsageBar::sizeHint() const
{
    return {0, metrics::usageBarHeight};
}

void UsageBar::paint(Painter& painter)
{
    const RectF rect = localRect();
    uint64_t total = 0;
    for (const UsageSegment& segment : m_segments)
        total += segment.bytes;
    const double radius = metrics::usageBarRadius;
    painter.drawRoundedRect(rect, {radius, 0, themeCorners()}, palette::freeSpace());
    if (total == 0)
        return;

    const double scale = painter.scale();
    const auto snap = [scale](double value) { return std::round(value * scale) / scale; };
    const double pixel = 1 / scale;
    painter.save();
    painter.clipRoundedRect(rect, radius, themeCorners());
    uint64_t before = 0;
    double previousEnd = -1;
    for (const UsageSegment& segment : m_segments) {
        if (segment.bytes == 0)
            continue;
        double start = snap(rect.width * double(before) / double(total));
        before += segment.bytes;
        const double end = snap(rect.width * double(before) / double(total));
        // A hairline of the card between neighbours.
        if (previousEnd >= 0)
            start += pixel;
        if (end > start && segment.kind != UsageSegment::Kind::Free)
            painter.fillRect({start, 0, end - start, rect.height}, segmentColor(segment));
        if (previousEnd >= 0)
            painter.fillRect({start - pixel, 0, pixel, rect.height}, palette::card());
        previousEnd = end;
    }
    painter.restore();
}

// A dot in the segment's colour, the segment's name and its size below.
class SummaryCard::LegendItem : public View
{
public:
    explicit LegendItem(const UsageSegment& segment)
        : m_color(segmentColor(segment))
        , m_free(segment.kind == UsageSegment::Kind::Free)
    {
        m_title = addChild<Label>(segment.title);
        m_title->setFontSize(12);
        m_title->setWeight(500);
        m_size = addChild<Label>(udisks::formatSize(segment.bytes));
        m_size->setFontSize(12);
    }

    SizeF sizeHint() const override
    {
        const SizeF title = m_title->sizeHint();
        const SizeF size = m_size->sizeHint();
        return {std::ceil(metrics::legendDot + kLegendTextGap + std::max(title.width, size.width)),
                std::ceil(title.height + size.height)};
    }

protected:
    void layout() override
    {
        const double x = metrics::legendDot + kLegendTextGap;
        const double width = std::max(0.0, geometry().width - x);
        const double titleHeight = m_title->sizeHint().height;
        m_title->setColor(palette::text());
        m_title->setGeometry({x, 0, width, titleHeight});
        m_size->setColor(palette::muted());
        m_size->setGeometry({x, titleHeight, width, m_size->sizeHint().height});
    }

    void paint(Painter& painter) override
    {
        const double dot = metrics::legendDot;
        const double top = std::round(m_title->geometry().y + m_title->capCenter() - dot / 2);
        const RectF rect{0, top, dot, dot};
        painter.fillEllipse(rect, m_color);
        // Free space is the bar's track; its dot gets an edge to show.
        if (m_free)
            painter.drawRoundedRect(rect, {dot / 2, 0, Corners::Circular}, {0, 0, 0, 0}, 1,
                                    palette::muted().withAlpha(0.5));
    }

private:
    Color m_color;
    bool m_free;
    Label* m_title;
    Label* m_size;
};

SummaryCard::SummaryCard(const DetailContent& content)
{
    setName("detail:summary");
    setLayoutHints({.fillWidth = true});
    m_icon = addChild<ThemeIcon>(content.icon, metrics::detailIcon);
    m_icon->setName("detail:icon");
    m_name = addChild<Label>(content.name);
    m_name->setName("detail:name");
    m_name->setFontSize(metrics::nameSize);
    m_name->setWeight(600);
    m_name->setElide(Label::Elide::Right);
    m_subtitle = addChild<Label>(content.subtitle);
    m_subtitle->setName("detail:subtitle");
    m_subtitle->setFontSize(metrics::subtitleSize);
    m_subtitle->setElide(Label::Elide::Right);

    if (!content.segments.empty()) {
        m_bar = addChild<UsageBar>(content.segments);
        for (size_t i = 0; i < content.segments.size(); ++i) {
            auto* item = addChild<LegendItem>(content.segments[i]);
            item->setName("detail:legend:" + std::to_string(i));
            m_legend.push_back(item);
        }
    }

    if (content.job) {
        m_job = addChild<Label>(content.job->text);
        m_job->setName("detail:job");
        m_job->setFontSize(12);
        m_job->setElide(Label::Elide::Right);
        m_progress = addChild<ProgressBar>();
        m_progress->setName("detail:jobProgress");
        if (content.job->progress) {
            m_progress->setValue(*content.job->progress);
            m_percent = addChild<Label>(format(tr("{percent}%"),
                                               {{"percent", std::to_string(int(std::lround(*content.job->progress * 100)))}}));
            m_percent->setFontSize(12);
            m_percent->setTextAlignment(Label::TextAlignment::Right);
        } else {
            m_progress->setIndeterminate(true);
        }
    }
}

std::vector<RectF> SummaryCard::legendPlaces(double width) const
{
    std::vector<RectF> places;
    const double inner = std::max(0.0, width - 2 * metrics::cardPadding);
    double x = 0;
    double y = 0;
    double rowHeight = 0;
    for (const LegendItem* item : m_legend) {
        const SizeF size = item->sizeHint();
        const double itemWidth = std::min(size.width, inner);
        if (x > 0 && x + itemWidth > inner) {
            x = 0;
            y += rowHeight + kLegendRowSpacing;
            rowHeight = 0;
        }
        places.push_back({x, y, itemWidth, size.height});
        x += itemWidth + kLegendSpacing;
        rowHeight = std::max(rowHeight, size.height);
    }
    return places;
}

double SummaryCard::heightForWidth(double width) const
{
    double height = metrics::cardPadding + metrics::detailIcon;
    if (m_bar) {
        height += kBarGap + metrics::usageBarHeight + kLegendGap;
        double bottom = 0;
        for (const RectF& place : legendPlaces(width))
            bottom = std::max(bottom, place.y + place.height);
        height += bottom;
    }
    if (m_job)
        height += kJobGap + m_job->sizeHint().height + kProgressGap + kProgressHeight;
    return std::ceil(height + metrics::cardPadding);
}

void SummaryCard::layout()
{
    const double width = geometry().width;
    const double padding = metrics::cardPadding;
    const double inner = std::max(0.0, width - 2 * padding);
    m_icon->setGeometry({padding, padding, metrics::detailIcon, metrics::detailIcon});

    // The name and the subtitle centred together on the icon.
    const double textLeft = padding + metrics::detailIcon + kIconGap;
    const double textWidth = std::max(0.0, width - textLeft - padding);
    const double nameHeight = m_name->sizeHint().height;
    const double subtitleHeight = m_subtitle->sizeHint().height;
    const double textTop = std::round(padding + (metrics::detailIcon - nameHeight - kNameGap - subtitleHeight) / 2);
    m_name->setColor(palette::text());
    m_name->setGeometry({textLeft, textTop, textWidth, nameHeight});
    m_subtitle->setColor(palette::muted());
    m_subtitle->setGeometry({textLeft, textTop + nameHeight + kNameGap, textWidth, subtitleHeight});

    double y = padding + metrics::detailIcon;
    if (m_bar) {
        y += kBarGap;
        m_bar->setGeometry({padding, y, inner, metrics::usageBarHeight});
        y += metrics::usageBarHeight + kLegendGap;
        const std::vector<RectF> places = legendPlaces(width);
        double bottom = 0;
        for (size_t i = 0; i < m_legend.size(); ++i) {
            const RectF& place = places[i];
            m_legend[i]->setGeometry({padding + place.x, y + place.y, place.width, place.height});
            bottom = std::max(bottom, place.y + place.height);
        }
        y += bottom;
    }
    if (m_job) {
        y += kJobGap;
        const double lineHeight = m_job->sizeHint().height;
        double jobWidth = inner;
        if (m_percent) {
            const double percentWidth = std::ceil(m_percent->sizeHint().width);
            m_percent->setColor(palette::muted());
            m_percent->setGeometry({width - padding - percentWidth, y, percentWidth, lineHeight});
            jobWidth -= percentWidth + 12;
        }
        m_job->setColor(palette::text());
        m_job->setGeometry({padding, y, std::max(0.0, jobWidth), lineHeight});
        y += lineHeight + kProgressGap;
        m_progress->setGeometry({padding, y, inner, kProgressHeight});
    }
}

void SummaryCard::paint(Painter& painter)
{
    paintCard(painter, localRect());
}

InfoGrid::InfoGrid(const std::vector<InfoItem>& left, const std::vector<InfoItem>& right)
{
    setName("detail:info");
    setLayoutHints({.fillWidth = true});
    const auto addCells = [this](const std::vector<InfoItem>& items, std::vector<Cell>& cells) {
        for (const InfoItem& item : items) {
            Cell cell;
            cell.title = addChild<Label>(item.title);
            cell.title->setName("detail:title:" + item.key);
            cell.title->setElide(Label::Elide::Right);
            cell.value = addChild<Label>(item.value);
            cell.value->setName("detail:" + item.key);
            cell.value->setTextAlignment(Label::TextAlignment::Right);
            cell.value->setElide(Label::Elide::Middle);
            cells.push_back(cell);
        }
    };
    addCells(left, m_left);
    addCells(right, m_right);
}

bool InfoGrid::twoColumns(double width) const
{
    return width >= metrics::infoTwoColumnWidth && !m_right.empty() && !m_left.empty();
}

size_t InfoGrid::rowCount(double width) const
{
    return twoColumns(width) ? std::max(m_left.size(), m_right.size()) : m_left.size() + m_right.size();
}

double InfoGrid::heightForWidth(double width) const
{
    return rowCount(width) * metrics::infoRowHeight;
}

void InfoGrid::layout()
{
    const double width = geometry().width;
    const double padding = metrics::cardPadding;
    const bool two = twoColumns(width);
    const double columnWidth = two ? std::floor(width / 2) : width;
    const auto place = [&](const Cell& cell, double x, double y, double cellWidth) {
        const double height = metrics::infoRowHeight;
        const SizeF title = cell.title->sizeHint();
        const SizeF value = cell.value->sizeHint();
        const double inner = std::max(0.0, cellWidth - 2 * padding);
        const double titleWidth = std::min(std::ceil(title.width), std::round(inner * 0.5));
        cell.title->setColor(palette::text());
        cell.title->setGeometry({x + padding, std::round(y + (height - title.height) / 2), titleWidth, title.height});
        const double valueWidth = std::max(0.0, inner - titleWidth - kInfoTitleGap);
        cell.value->setColor(Theme::current().disabledText);
        cell.value->setGeometry({x + cellWidth - padding - valueWidth, std::round(y + (height - value.height) / 2),
                                 valueWidth, value.height});
    };
    if (two) {
        for (size_t i = 0; i < m_left.size(); ++i)
            place(m_left[i], 0, i * metrics::infoRowHeight, columnWidth);
        for (size_t i = 0; i < m_right.size(); ++i)
            place(m_right[i], columnWidth, i * metrics::infoRowHeight, width - columnWidth);
        return;
    }
    size_t row = 0;
    for (const Cell& cell : m_left)
        place(cell, 0, row++ * metrics::infoRowHeight, width);
    for (const Cell& cell : m_right)
        place(cell, 0, row++ * metrics::infoRowHeight, width);
}

void InfoGrid::paint(Painter& painter)
{
    paintCard(painter, localRect());
    const double width = geometry().width;
    const double padding = metrics::cardPadding;
    const double pixel = 1 / painter.scale();
    const Color separator = color(ColorId::Separator);
    const size_t rows = rowCount(width);
    const bool two = twoColumns(width);
    const double columnWidth = two ? std::floor(width / 2) : width;
    for (size_t i = 0; i + 1 < rows; ++i) {
        const double y = (i + 1) * metrics::infoRowHeight - pixel;
        if (two) {
            painter.drawRoundedRect({padding, y, columnWidth - 2 * padding, pixel}, {}, separator);
            painter.drawRoundedRect({columnWidth + padding, y, width - columnWidth - 2 * padding, pixel}, {},
                                    separator);
        } else {
            painter.drawRoundedRect({padding, y, width - 2 * padding, pixel}, {}, separator);
        }
    }
    if (two) {
        const double inset = 12;
        painter.drawRoundedRect({columnWidth, inset, pixel, std::max(0.0, geometry().height - 2 * inset)}, {},
                                separator);
    }
}

} // namespace views
