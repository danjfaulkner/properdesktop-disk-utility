// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "ui/geometry.h"

#include "udisks/tree.h"

#include <string>

namespace udisks {
class Model;
}

namespace views {

// The window's proportions, in logical pixels.
namespace metrics {

inline constexpr double windowWidth = 900;
inline constexpr double windowHeight = 600;
inline constexpr double windowMinimumWidth = 760;
inline constexpr double windowMinimumHeight = 480;

inline constexpr double sidebarWidth = 192;
inline constexpr double sidebarMaximumWidth = 232;
inline constexpr double sidebarMinimumWidth = 180;
inline constexpr double sidebarPadding = 12;
inline constexpr double headerHeight = 46;

// The sidebar's text, rows and headings alike, as Settings' sidebar: 8
// points.
inline constexpr double sidebarFontSize = 8 * 4.0 / 3;
// The sidebar's rows, as Music's.
inline constexpr double rowHeight = 30;
inline constexpr double rowGap = 2;
inline constexpr double rowGlyph = 17;
inline constexpr double rowIndent = 16;
// The disclosure triangle's column, before the glyph.
inline constexpr double disclosureWidth = 14;
// The eject button's plate keeps this gap to the row's edges.
inline constexpr double ejectGap = 4;
inline constexpr double ejectGlyph = 13;

// The detail pane, as a Settings page: cards on the page's background.
inline constexpr double pagePadding = 24;
inline constexpr double pageTopMargin = 12;
inline constexpr double cardPadding = 16;
inline constexpr double cardSpacing = 16;
inline constexpr double detailIcon = 56;
inline constexpr double nameSize = 19;
inline constexpr double subtitleSize = 12;
inline constexpr double usageBarHeight = 18;
inline constexpr double usageBarRadius = 5;
inline constexpr double legendDot = 8;
inline constexpr double infoRowHeight = 44;
// Narrower than this, the information grid keeps one column.
inline constexpr double infoTwoColumnWidth = 520;

} // namespace metrics

// Colours that follow the theme.
namespace palette {

properdesktop::ui::Color text();
properdesktop::ui::Color muted();
properdesktop::ui::Color faint();
properdesktop::ui::Color hairline();
properdesktop::ui::Color hoverFill();
properdesktop::ui::Color pressFill();
properdesktop::ui::Color accent();
// Text and glyphs on an accent fill.
properdesktop::ui::Color onAccent();
// The detail pane's background, under its cards.
properdesktop::ui::Color page();
properdesktop::ui::Color card();
// The usage bar: what is free, and what the filesystem keeps for itself.
properdesktop::ui::Color freeSpace();
properdesktop::ui::Color otherSpace();
// The partitions of a disk on its usage bar, in order, round again past
// the last.
properdesktop::ui::Color partition(size_t index);

} // namespace palette

// The sidebar's glyph for the node: its disk's kind (internal, external, SD
// card, disk image) for a disk, and for a volume in the Volumes view; a
// volume's or a partition's own in All Devices.
std::string deviceGlyph(const udisks::Model& model, const udisks::Node& node, udisks::ViewMode mode);

} // namespace views
