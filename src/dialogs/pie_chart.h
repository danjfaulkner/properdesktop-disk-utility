// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "ui/view.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace properdesktop::ui {
class Label;
}

namespace dialogs {

// The partition editor's disk as a pie: a wedge per partition and per
// stretch of free space, clockwise from twelve o'clock in disk order, the
// selected one lifted out a little. Handles on the rim mark the boundaries
// that can be dragged.
class PieChart : public properdesktop::ui::View
{
public:
    struct Wedge
    {
        uint32_t id = 0;
        uint64_t offset = 0;
        uint64_t size = 0;
        // Shown inside a wedge wide enough: the name over the size.
        std::string name;
        std::string sizeText;
        bool free = false;

        uint64_t end() const { return offset + size; }
    };

    PieChart();

    // The stretch of the disk the whole circle stands for.
    void setRange(uint64_t start, uint64_t end);
    void setWedges(std::vector<Wedge> wedges);
    void setSelected(std::optional<uint32_t> id);
    // The boundary after wedge index can be dragged.
    void setMovableBoundaries(std::vector<size_t> boundaries);

    // The middle of the wedge, and the boundary's handle, in the view's
    // coordinates.
    properdesktop::ui::PointF wedgeCentre(uint32_t id) const;
    std::optional<properdesktop::ui::PointF> handlePoint(size_t boundary) const;

    properdesktop::ui::SizeF sizeHint() const override { return {kSize, kSize}; }
    bool acceptsPointer() const override { return true; }

    // A press, before the pie finds what is under it: whatever changes the
    // wedges first goes here, so a handle is taken from the wedges shown.
    std::function<void()> onPressed;
    std::function<void(uint32_t id)> onSelected;
    // A drag of the boundary after wedge index to the disk position.
    std::function<void(size_t boundary, uint64_t position)> onBoundaryMoved;
    std::function<void()> onDragFinished;

    static constexpr double kSize = 232;

protected:
    void layout() override;
    void paint(properdesktop::ui::Painter& painter) override;
    void pointerPress(const properdesktop::ui::PointerEvent& event) override;
    void pointerMove(const properdesktop::ui::PointerEvent& event) override;
    void pointerRelease(const properdesktop::ui::PointerEvent& event) override;

private:
    // Clockwise from twelve o'clock, in radians.
    double angleOf(uint64_t position) const;
    uint64_t positionOf(double angle) const;
    double angleAt(properdesktop::ui::PointF point) const;
    properdesktop::ui::PointF centre() const;
    double radius() const;
    properdesktop::ui::Color wedgeColor(size_t index) const;

    uint64_t m_start = 0;
    uint64_t m_end = 1;
    std::vector<Wedge> m_wedges;
    std::vector<properdesktop::ui::Label*> m_names;
    std::vector<properdesktop::ui::Label*> m_sizes;
    std::optional<uint32_t> m_selected;
    std::vector<size_t> m_movable;
    std::optional<size_t> m_dragging;
};

} // namespace dialogs
