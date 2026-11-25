// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/pie_chart.h"

#include "ui/color_provider.h"
#include "ui/event.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/path.h"
#include "ui/theme.h"

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace properdesktop::ui;

namespace dialogs {

static constexpr double kTau = 2 * std::numbers::pi;
// Room around the circle for the lifted wedge and the handles.
static constexpr double kMargin = 10;
static constexpr double kLift = 6;
static constexpr double kHandleRadius = 5;
// How near a press must come to a handle to take it.
static constexpr double kHandleReach = 12;
// Narrower wedges show no text.
static constexpr double kLabelSweep = 0.5;

PieChart::PieChart()
{
    setLayoutHints({.preferredWidth = kSize, .preferredHeight = kSize});
}

void PieChart::setRange(uint64_t start, uint64_t end)
{
    m_start = start;
    m_end = std::max(end, start + 1);
    requestLayout();
    requestPaint();
}

void PieChart::setWedges(std::vector<Wedge> wedges)
{
    m_wedges = std::move(wedges);
    while (m_names.size() < m_wedges.size()) {
        auto* name = addChild<Label>();
        name->setTextAlignment(Label::TextAlignment::Center);
        name->setElide(Label::Elide::Right);
        name->setWeight(600);
        m_names.push_back(name);
        auto* size = addChild<Label>();
        size->setTextAlignment(Label::TextAlignment::Center);
        size->setFontSize(Theme::current().smallFontSize());
        m_sizes.push_back(size);
    }
    for (size_t i = 0; i < m_names.size(); ++i) {
        const bool used = i < m_wedges.size();
        m_names[i]->setText(used ? m_wedges[i].name : std::string());
        m_sizes[i]->setText(used ? m_wedges[i].sizeText : std::string());
    }
    requestLayout();
    requestPaint();
}

void PieChart::setSelected(std::optional<uint32_t> id)
{
    m_selected = id;
    requestLayout();
    requestPaint();
}

void PieChart::setMovableBoundaries(std::vector<size_t> boundaries)
{
    m_movable = std::move(boundaries);
    requestPaint();
}

double PieChart::angleOf(uint64_t position) const
{
    const uint64_t clamped = std::clamp(position, m_start, m_end);
    return double(clamped - m_start) / double(m_end - m_start) * kTau;
}

uint64_t PieChart::positionOf(double angle) const
{
    const double fraction = std::clamp(angle / kTau, 0.0, 1.0);
    return m_start + uint64_t(std::llround(fraction * double(m_end - m_start)));
}

PointF PieChart::centre() const
{
    return {geometry().width / 2, geometry().height / 2};
}

double PieChart::radius() const
{
    return std::max(0.0, std::min(geometry().width, geometry().height) / 2 - kMargin);
}

static PointF onCircle(PointF centre, double radius, double angle)
{
    return {centre.x + radius * std::sin(angle), centre.y - radius * std::cos(angle)};
}

double PieChart::angleAt(PointF point) const
{
    const PointF c = centre();
    double angle = std::atan2(point.x - c.x, c.y - point.y);
    if (angle < 0)
        angle += kTau;
    return angle;
}

Color PieChart::wedgeColor(size_t index) const
{
    static constexpr Color kColors[] = {accent::blue, accent::purple, accent::green, accent::orange, accent::pink,
                                        accent::grey};
    size_t used = 0;
    for (size_t i = 0; i < index; ++i) {
        if (!m_wedges[i].free)
            ++used;
    }
    return kColors[used % std::size(kColors)];
}

PointF PieChart::wedgeCentre(uint32_t id) const
{
    for (const Wedge& wedge : m_wedges) {
        if (wedge.id != id)
            continue;
        const double middle = (angleOf(wedge.offset) + angleOf(wedge.offset + wedge.size)) / 2;
        return onCircle(centre(), radius() * 0.6, middle);
    }
    return centre();
}

std::optional<PointF> PieChart::handlePoint(size_t boundary) const
{
    if (boundary >= m_wedges.size() || std::find(m_movable.begin(), m_movable.end(), boundary) == m_movable.end())
        return std::nullopt;
    const Wedge& wedge = m_wedges[boundary];
    return onCircle(centre(), radius(), angleOf(wedge.offset + wedge.size));
}

void PieChart::layout()
{
    const Theme& theme = Theme::current();
    for (size_t i = 0; i < m_names.size(); ++i) {
        Label* name = m_names[i];
        Label* size = m_sizes[i];
        const bool used = i < m_wedges.size();
        const double sweep = used ? angleOf(m_wedges[i].offset + m_wedges[i].size) - angleOf(m_wedges[i].offset) : 0;
        const bool shown = used && sweep >= kLabelSweep && !m_wedges[i].name.empty();
        name->setVisible(shown);
        size->setVisible(shown);
        if (!shown)
            continue;
        const Color text = m_wedges[i].free ? theme.disabledText : Color{255, 255, 255, 255};
        name->setColor(text);
        size->setColor(m_wedges[i].free ? theme.disabledText : Color{255, 255, 255, 220});
        PointF point = wedgeCentre(m_wedges[i].id);
        if (m_selected == m_wedges[i].id && m_wedges.size() > 1) {
            const double middle = (angleOf(m_wedges[i].offset) + angleOf(m_wedges[i].end())) / 2;
            point = {point.x + kLift * std::sin(middle), point.y - kLift * std::cos(middle)};
        }
        // As wide as the wedge's chord through its middle, at most.
        const double width = std::min(radius() * 1.1, 2 * radius() * 0.6 * std::sin(std::min(sweep, 3.0) / 2));
        const double nameHeight = name->sizeHint().height;
        const double sizeHeight = size->sizeHint().height;
        const double top = std::round(point.y - (nameHeight + sizeHeight) / 2);
        name->setGeometry({std::round(point.x - width / 2), top, std::round(width), nameHeight});
        size->setGeometry({std::round(point.x - width / 2), top + nameHeight, std::round(width), sizeHeight});
    }
}

void PieChart::paint(Painter& painter)
{
    const Theme& theme = Theme::current();
    const PointF c = centre();
    const double r = radius();
    const Color background = color(ColorId::Background);
    if (m_wedges.empty()) {
        painter.fillEllipse({c.x - r, c.y - r, 2 * r, 2 * r}, theme.secondBackground);
        return;
    }
    for (size_t i = 0; i < m_wedges.size(); ++i) {
        const Wedge& wedge = m_wedges[i];
        const double from = angleOf(wedge.offset);
        const double to = angleOf(wedge.end());
        if (to - from <= 0)
            continue;
        PointF origin = c;
        if (m_selected == wedge.id && m_wedges.size() > 1) {
            const double middle = (from + to) / 2;
            origin = {c.x + kLift * std::sin(middle), c.y - kLift * std::cos(middle)};
        }
        const Color fill = wedge.free ? theme.secondBackground : wedgeColor(i);
        if (to - from >= kTau - 1e-9) {
            painter.fillEllipse({origin.x - r, origin.y - r, 2 * r, 2 * r}, fill);
            continue;
        }
        Path path;
        path.moveTo(origin);
        // Clockwise from twelve o'clock, in the path's degrees.
        path.arcTo({origin.x - r, origin.y - r, 2 * r, 2 * r}, 90 - from * 360 / kTau, -(to - from) * 360 / kTau);
        path.close();
        painter.fillPath(path, fill);
        // A gap of the background between wedges.
        painter.strokePath(path, background, 1.5, LineCap::Butt, LineJoin::Miter);
    }
    for (size_t boundary : m_movable) {
        const std::optional<PointF> point = handlePoint(boundary);
        if (!point)
            continue;
        const RectF dot{point->x - kHandleRadius, point->y - kHandleRadius, 2 * kHandleRadius, 2 * kHandleRadius};
        painter.drawShadow(dot, kHandleRadius, 1.5, Color{0, 0, 0, 70}, {0, 0.5});
        painter.fillEllipse(dot, Color{255, 255, 255, 255});
    }
}

void PieChart::pointerPress(const PointerEvent& event)
{
    if (event.button != MouseButton::Left)
        return;
    if (onPressed)
        onPressed();
    for (size_t boundary : m_movable) {
        const std::optional<PointF> point = handlePoint(boundary);
        if (point && std::hypot(point->x - event.position.x, point->y - event.position.y) <= kHandleReach) {
            m_dragging = boundary;
            return;
        }
    }
    const PointF c = centre();
    if (std::hypot(event.position.x - c.x, event.position.y - c.y) > radius() + kLift)
        return;
    const uint64_t position = positionOf(angleAt(event.position));
    for (const Wedge& wedge : m_wedges) {
        if (position >= wedge.offset && position < wedge.end()) {
            if (onSelected)
                onSelected(wedge.id);
            return;
        }
    }
}

void PieChart::pointerMove(const PointerEvent& event)
{
    if (!m_dragging || !onBoundaryMoved)
        return;
    onBoundaryMoved(*m_dragging, positionOf(angleAt(event.position)));
}

void PieChart::pointerRelease(const PointerEvent&)
{
    if (!m_dragging)
        return;
    m_dragging.reset();
    if (onDragFinished)
        onDragFinished();
}

} // namespace dialogs
