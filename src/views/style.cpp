// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "views/style.h"

#include "udisks/describe.h"
#include "udisks/model.h"

#include "ui/theme.h"

#include <array>

using properdesktop::ui::Color;
using properdesktop::ui::Theme;

namespace views {

namespace palette {

static bool dark()
{
    return Theme::current().darkMode;
}

Color text()
{
    return Theme::current().text;
}

Color muted()
{
    return dark() ? Color{235, 235, 245, 143} : Color{60, 60, 67, 158};
}

Color faint()
{
    return dark() ? Color{235, 235, 245, 77} : Color{60, 60, 67, 82};
}

Color hairline()
{
    return dark() ? Color{255, 255, 255, 18} : Color{0, 0, 0, 18};
}

Color hoverFill()
{
    return dark() ? Color{255, 255, 255, 15} : Color{0, 0, 0, 10};
}

Color pressFill()
{
    return dark() ? Color{255, 255, 255, 26} : Color{0, 0, 0, 20};
}

Color accent()
{
    return Theme::current().highlight;
}

Color onAccent()
{
    return Theme::current().highlightedText;
}

Color page()
{
    return Theme::current().background;
}

Color card()
{
    return Theme::current().secondBackground;
}

Color freeSpace()
{
    return dark() ? Color{255, 255, 255, 26} : Color{0, 0, 0, 15};
}

Color otherSpace()
{
    return dark() ? Color{152, 152, 160, 255} : Color{174, 174, 180, 255};
}

Color partition(size_t index)
{
    namespace accent = properdesktop::ui::accent;
    static constexpr std::array<Color, 6> colors = {accent::blue, accent::purple, accent::orange,
                                                    accent::green, accent::pink, accent::red};
    return colors[index % colors.size()];
}

} // namespace palette

static std::string diskGlyph(const udisks::Model& model, const udisks::Node& node)
{
    if (node.drive.empty())
        return "device-image";
    const udisks::Object* drive = model.object(node.drive);
    if (drive && drive->drive && drive->drive->optical)
        return "device-optical";
    if (udisks::connectionOf(model, node.drive) == udisks::Connection::SdCard
        || (drive && drive->drive && drive->drive->media.starts_with("flash_sd")))
        return "device-sd";
    if (udisks::sectionOfDrive(model, node.drive) == udisks::Section::External)
        return "device-external";
    return "device-internal";
}

std::string deviceGlyph(const udisks::Model& model, const udisks::Node& node, udisks::ViewMode mode)
{
    if (node.kind == udisks::NodeKind::Disk || mode == udisks::ViewMode::Volumes)
        return diskGlyph(model, node);
    if (node.kind == udisks::NodeKind::Volume)
        return "device-volume";
    return "device-partition";
}

} // namespace views
