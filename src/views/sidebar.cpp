// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "views/sidebar.h"
#include "views/context.h"
#include "views/style.h"
#include "dialogs/identity_guard.h"

#include "udisks/describe.h"
#include "udisks/identity.h"
#include "udisks/model.h"

#include "base/event_loop.h"
#include "base/i18n.h"
#include "ui/color_provider.h"
#include "ui/icon.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/menu.h"
#include "ui/painter.h"
#include "ui/scroll_view.h"
#include "ui/theme.h"
#include "ui/window.h"

#include <algorithm>
#include <cmath>

using namespace properdesktop::ui;
using properdesktop::i18n::tr;

namespace views {

static constexpr double kHeadingHeight = 26;
static constexpr double kHeadingSpace = 14;
static constexpr double kDisclosureGlyph = 12;
static constexpr double kGlyphGap = 10;
static constexpr double kEjectSize = metrics::rowHeight - 2 * metrics::ejectGap;

static std::string sectionTitle(udisks::Section section)
{
    switch (section) {
    case udisks::Section::Internal:
        return tr("Internal");
    case udisks::Section::External:
        return tr("External");
    case udisks::Section::Images:
        return tr("Disk Images");
    }
    return {};
}

static double rowRadius()
{
    return Theme::current().mediumRadius;
}

// "Internal", "External", "Disk Images".
class SidebarHeading : public View
{
public:
    SidebarHeading(const std::string& text, bool first)
        : first(first)
    {
        m_label = addChild<Label>(text);
        m_label->setFontSize(metrics::sidebarFontSize);
    }

    double height() const { return kHeadingHeight + (first ? 0 : kHeadingSpace); }

    const bool first;

protected:
    void layout() override
    {
        m_label->setColor(Theme::current().disabledText);
        // On a whole pixel, as Settings' headings.
        const double height = m_label->sizeHint().height;
        const double top = geometry().height - kHeadingHeight + std::round((kHeadingHeight - height) / 2);
        m_label->setGeometry({8, top, std::max(0.0, geometry().width - 16), height});
    }

private:
    Label* m_label;
};

// The triangle that folds a disk's partitions and volumes.
class Disclosure : public View
{
public:
    Disclosure()
    {
        m_icon = addChild<Icon>("chevron-right-small", kDisclosureGlyph);
    }

    void setExpanded(bool expanded)
    {
        m_icon->setIconName(expanded ? "chevron-down-small" : "chevron-right-small");
    }

    // On the selected row's accent fill.
    void setOnAccent(bool onAccent)
    {
        m_onAccent = onAccent;
        requestLayout();
    }

    bool acceptsPointer() const override { return true; }

    std::function<void()> onClicked;

protected:
    void layout() override
    {
        m_icon->setColor(m_onAccent ? palette::onAccent() : palette::muted());
        m_icon->setGeometry({std::round((geometry().width - kDisclosureGlyph) / 2),
                             std::round((geometry().height - kDisclosureGlyph) / 2), kDisclosureGlyph,
                             kDisclosureGlyph});
    }

    void pointerPress(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Left && onClicked)
            onClicked();
    }

private:
    Icon* m_icon;
    bool m_onAccent = false;
};

// The eject glyph at a row's end, on a plate under the pointer that keeps
// one gap to the row's edges.
class EjectButton : public View
{
public:
    EjectButton()
    {
        setTracksHover(true);
        m_icon = addChild<Icon>("eject", metrics::ejectGlyph);
        setToolTip(actionTitle(Action::Eject));
    }

    // On the selected row's accent fill.
    void setOnAccent(bool onAccent)
    {
        m_onAccent = onAccent;
        requestLayout();
        requestPaint();
    }

    bool acceptsPointer() const override { return true; }

    std::function<void()> onClicked;

protected:
    void layout() override
    {
        const double size = metrics::ejectGlyph;
        m_icon->setColor(m_onAccent ? palette::onAccent() : palette::muted());
        m_icon->setGeometry({std::round((geometry().width - size) / 2), std::round((geometry().height - size) / 2),
                             size, size});
    }

    void paint(Painter& painter) override
    {
        if (!isHovered() && !m_pressed)
            return;
        const Color fill = m_onAccent ? palette::onAccent().withAlpha(m_pressed ? 0.3 : 0.18)
                                      : (m_pressed ? palette::pressFill() : palette::hoverFill());
        painter.drawRoundedRect(localRect(), {rowRadius() - metrics::ejectGap, 0, themeCorners()}, fill);
    }

    void hoverChanged() override { requestPaint(); }

    void pointerPress(const PointerEvent& event) override
    {
        if (event.button != MouseButton::Left)
            return;
        m_pressed = true;
        requestPaint();
    }

    void pointerRelease(const PointerEvent& event) override
    {
        if (!m_pressed)
            return;
        m_pressed = false;
        requestPaint();
        if (localRect().contains(event.position) && onClicked)
            onClicked();
    }

private:
    Icon* m_icon;
    bool m_pressed = false;
    bool m_onAccent = false;
};

// One disk, partition or volume: the disclosure column in Show All Devices,
// the glyph, the name, and the eject button for what can be ejected.
class SidebarRow : public View
{
public:
    SidebarRow(Sidebar& sidebar, const udisks::Node& node, std::string parent, int depth, bool disclosureColumn,
               bool ejectable)
        : id(node.id)
        , parent(std::move(parent))
        , depth(depth)
        , hasChildren(!node.children.empty())
        , m_sidebar(sidebar)
        , m_disclosureColumn(disclosureColumn)
    {
        setName("sidebar:" + id);
        setTracksHover(true);
        const Context& context = sidebar.m_context;
        if (hasChildren) {
            m_disclosure = addChild<Disclosure>();
            m_disclosure->setName("sidebar:disclosure:" + id);
            m_disclosure->onClicked = [this] { m_sidebar.setExpanded(this->id, !m_sidebar.isExpanded(this->id)); };
        }
        m_glyph = addChild<Icon>(deviceGlyph(context.model, node, context.viewMode), metrics::rowGlyph);
        m_glyph->setName("sidebar:glyph:" + id);
        m_label = addChild<Label>(udisks::displayName(context.model, node));
        m_label->setName("sidebar:label:" + id);
        m_label->setFontSize(metrics::sidebarFontSize);
        m_label->setElide(Label::Elide::Right);
        if (ejectable) {
            m_eject = addChild<EjectButton>();
            m_eject->setName("sidebar:eject:" + id);
            m_eject->onClicked = [this] { m_sidebar.ejectPressed(this); };
        }
    }

    const std::string id;
    const std::string parent;
    const int depth;
    const bool hasChildren;

    void setCurrent(bool current)
    {
        if (current == m_current)
            return;
        m_current = current;
        if (m_disclosure)
            m_disclosure->setOnAccent(current);
        if (m_eject)
            m_eject->setOnAccent(current);
        requestLayout();
        requestPaint();
    }

    void setExpanded(bool expanded)
    {
        if (m_disclosure)
            m_disclosure->setExpanded(expanded);
    }

    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        const double width = geometry().width;
        const double height = geometry().height;
        double x = 8 + depth * metrics::rowIndent;
        if (m_disclosureColumn) {
            if (m_disclosure)
                m_disclosure->setGeometry({x - 2, 0, metrics::disclosureWidth + 2, height});
            x += metrics::disclosureWidth;
        }
        // In device pixels from the window's top: the glyph is drawn whole
        // on them, so it goes where it is drawn, centred in the row, and the
        // capitals' middle meets its middle with the baseline on a pixel.
        const double scale = window() ? window()->scale() : 1;
        const double origin = mapToWindow({}).y * scale;
        const double side = std::max(1.0, double(std::lround(metrics::rowGlyph * scale)));
        const double glyphTop = std::round(origin + (height * scale - side) / 2);
        const double glyphMiddle = glyphTop + side / 2;
        m_glyph->setColor(m_current ? palette::onAccent() : palette::text().withAlpha(0.8));
        m_glyph->setGeometry({x, (glyphTop - origin) / scale, side / scale, side / scale});
        x += metrics::rowGlyph + kGlyphGap;
        double right = width - 8;
        if (m_eject) {
            const double gap = metrics::ejectGap;
            m_eject->setGeometry({width - gap - kEjectSize, gap, kEjectSize, kEjectSize});
            right = width - gap - kEjectSize - 2;
        }
        m_label->setColor(m_current ? palette::onAccent() : palette::text());
        const double baseline = std::round(glyphMiddle + m_label->capHeight() * scale / 2);
        m_label->setGeometry({x, (baseline - origin) / scale - m_label->baselineOffset(), std::max(0.0, right - x),
                              m_label->sizeHint().height});
    }

    void paint(Painter& painter) override
    {
        Color fill{0, 0, 0, 0};
        if (m_current)
            fill = palette::accent();
        else if (isHovered())
            fill = palette::hoverFill();
        if (fill.a)
            painter.drawRoundedRect(localRect(), {rowRadius(), 0, themeCorners()}, fill);
    }

    void hoverChanged() override { requestPaint(); }

    void pointerPress(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Right) {
            m_sidebar.openMenu(this, event.position);
            return;
        }
        if (event.button != MouseButton::Left)
            return;
        m_sidebar.rowPressed(this);
        if (event.clickCount == 2 && hasChildren)
            m_sidebar.setExpanded(id, !m_sidebar.isExpanded(id));
    }

private:
    Sidebar& m_sidebar;
    bool m_disclosureColumn;
    bool m_current = false;
    Disclosure* m_disclosure = nullptr;
    Icon* m_glyph;
    Label* m_label;
    EjectButton* m_eject = nullptr;
};

// The headings and rows, one under another; folded rows are hidden.
class SidebarList : public View
{
public:
    double heightForWidth(double) const override
    {
        double height = 0;
        for (const auto& child : children()) {
            if (child->isVisible())
                height += heightOf(child.get());
        }
        return height + metrics::sidebarPadding;
    }

protected:
    void layout() override
    {
        // The scroll view spans the column so its bar sits at the trailing
        // edge; the rows keep the column's padding.
        const double side = metrics::sidebarPadding;
        double y = 0;
        for (const auto& child : children()) {
            if (!child->isVisible())
                continue;
            if (auto* heading = dynamic_cast<SidebarHeading*>(child.get())) {
                heading->setGeometry({side, y, std::max(0.0, geometry().width - 2 * side), heading->height()});
            } else {
                const double before = child->geometry().y;
                child->setGeometry({side, y, std::max(0.0, geometry().width - 2 * side), metrics::rowHeight});
                // A row places its glyph and text on its device pixels,
                // which a move changes.
                if (before != y)
                    child->requestLayout();
            }
            y += heightOf(child.get());
        }
    }

private:
    static double heightOf(const View* child)
    {
        if (auto* heading = dynamic_cast<const SidebarHeading*>(child))
            return heading->height();
        return metrics::rowHeight + metrics::rowGap;
    }
};

Sidebar::Sidebar(Context& context)
    : m_context(context)
{
    m_scroll = addChild<ScrollView>();
    m_scroll->setName("sidebar:scroll");
    m_list = m_scroll->setContent<SidebarList>();
    reload();
}

Sidebar::~Sidebar() = default;

static bool canEject(const Context& context, const udisks::Node& node, udisks::Section section, int depth)
{
    if (depth > 0 || section == udisks::Section::Internal)
        return false;
    return actionState(Action::Eject, context.model, &context.operations, node).enabled;
}

void Sidebar::reload()
{
    // A row's own press may be what changed the tree; the old rows go once
    // it is done.
    std::vector<std::shared_ptr<View>> old;
    while (!m_list->children().empty())
        old.push_back(m_list->takeChild(m_list->children().front().get()));
    if (!old.empty())
        properdesktop::base::EventLoop::current()->post([old = std::move(old)] {});
    m_rows.clear();

    const bool disclosureColumn = m_context.viewMode == udisks::ViewMode::AllDevices;
    std::set<std::string> ids;
    const auto addRows = [&](const auto& self, const std::vector<udisks::Node>& nodes, const std::string& parent,
                             int depth, udisks::Section section) -> void {
        for (const udisks::Node& node : nodes) {
            ids.insert(node.id);
            auto* row = m_list->addChild<SidebarRow>(*this, node, parent, depth, disclosureColumn,
                                                     canEject(m_context, node, section, depth));
            row->setCurrent(node.id == m_current);
            m_rows.push_back(row);
            self(self, node.children, node.id, depth + 1, section);
        }
    };
    for (size_t i = 0; i < m_context.tree.size(); ++i) {
        const udisks::SectionNodes& section = m_context.tree[i];
        auto* heading = m_list->addChild<SidebarHeading>(sectionTitle(section.section), i == 0);
        heading->setName("sidebar:section:" + std::to_string(i));
        addRows(addRows, section.nodes, {}, 0, section.section);
    }
    // Disks gone forget their folding.
    std::erase_if(m_collapsed, [&ids](const std::string& id) { return !ids.contains(id); });
    updateFolding();
}

void Sidebar::updateFolding()
{
    std::set<std::string> hidden;
    for (SidebarRow* row : m_rows) {
        const bool folded = !row->parent.empty() && (hidden.contains(row->parent) || m_collapsed.contains(row->parent));
        row->setVisible(!folded);
        if (folded)
            hidden.insert(row->id);
        row->setExpanded(!m_collapsed.contains(row->id));
    }
    m_list->requestLayout();
    m_scroll->requestLayout();
    requestLayout();
}

void Sidebar::setCurrent(const std::string& id)
{
    m_current = id;
    // In place: the click that selects comes from one of the rows.
    for (SidebarRow* row : m_rows)
        row->setCurrent(row->id == id);
}

void Sidebar::setExpanded(const std::string& id, bool expanded)
{
    if (expanded == isExpanded(id))
        return;
    if (expanded)
        m_collapsed.erase(id);
    else
        m_collapsed.insert(id);
    updateFolding();
    // A folded selection goes to the disk it is folded into.
    const auto current = std::find_if(m_rows.begin(), m_rows.end(),
                                      [this](const SidebarRow* row) { return row->id == m_current; });
    if (!expanded && current != m_rows.end() && !(*current)->isVisible() && m_context.select)
        m_context.select(id);
}

bool Sidebar::isExpanded(const std::string& id) const
{
    return !m_collapsed.contains(id);
}

std::vector<SidebarRow*> Sidebar::shownRows() const
{
    std::vector<SidebarRow*> rows;
    for (SidebarRow* row : m_rows) {
        if (row->isVisible())
            rows.push_back(row);
    }
    return rows;
}

void Sidebar::moveTo(SidebarRow* row)
{
    if (!row)
        return;
    if (m_context.select)
        m_context.select(row->id);
    m_scroll->ensureVisible(row->geometry());
}

void Sidebar::rowPressed(SidebarRow* row)
{
    setFocus(FocusReason::Mouse);
    if (m_context.select)
        m_context.select(row->id);
}

void Sidebar::ejectPressed(SidebarRow* row)
{
    // The eject button works on its own row, which it selects first, as
    // the dispatcher acts on the selection.
    if (m_context.select)
        m_context.select(row->id);
    m_context.actions.trigger(Action::Eject);
}

void Sidebar::openMenu(SidebarRow* row, PointF position)
{
    rowPressed(row);
    ActionDispatcher& actions = m_context.actions;
    m_menu = std::make_unique<Menu>();
    m_menuEntries.clear();
    // The row's node and what its block is now: a choice made later acts on
    // them, not on whatever is selected then, and not on another disk that
    // took the path meanwhile.
    const Selection node = actions.selection();
    const udisks::Identity identity = node ? udisks::identityOf(m_context.model, node->block) : udisks::Identity();
    const auto add = [this, &actions, node, identity](Action action) {
        const bool enabled = actions.isEnabled(action);
        const size_t index = m_menu->addItem(actionTitle(action), [this, action, node, identity] {
            if (node && !m_context.model.matches(identity)) {
                dialogs::showGone(m_context, udisks::displayName(m_context.model, *node));
                return;
            }
            m_context.actions.triggerOn(action, node);
        });
        m_menu->setItemEnabled(index, enabled);
        m_menuEntries.push_back({action, enabled});
    };
    add(Action::Info);
    add(Action::Rename);
    m_menu->addSeparator();
    add(actions.isEnabled(Action::Unmount) ? Action::Unmount : Action::Mount);
    if (actions.isEnabled(Action::Lock))
        add(Action::Lock);
    add(Action::Eject);
    m_menu->addSeparator();
    add(Action::FirstAid);
    add(Action::Partition);
    add(Action::Erase);
    add(Action::Restore);
    m_menu->addSeparator();
    add(Action::NewImageFromSelection);
    m_menu->popup(&m_context.window, row->mapToWindow(position));
}

bool Sidebar::keyPress(const KeyEvent& event)
{
    if (event.modifiers & (ControlModifier | AltModifier | SuperModifier))
        return false;
    const std::vector<SidebarRow*> rows = shownRows();
    if (rows.empty())
        return false;
    const auto current = std::find_if(rows.begin(), rows.end(),
                                      [this](const SidebarRow* row) { return row->id == m_current; });
    const bool none = current == rows.end();
    SidebarRow* row = none ? nullptr : *current;
    switch (event.key) {
    case key::Up:
        moveTo(none ? rows.front() : *(current == rows.begin() ? current : current - 1));
        return true;
    case key::Down:
        moveTo(none ? rows.front() : *(current + 1 == rows.end() ? current : current + 1));
        return true;
    case key::Home:
        moveTo(rows.front());
        return true;
    case key::End:
        moveTo(rows.back());
        return true;
    case key::Left:
        if (!row)
            return true;
        if (row->hasChildren && isExpanded(row->id)) {
            setExpanded(row->id, false);
        } else if (!row->parent.empty()) {
            const auto parent = std::find_if(rows.begin(), rows.end(),
                                             [row](const SidebarRow* other) { return other->id == row->parent; });
            if (parent != rows.end())
                moveTo(*parent);
        }
        return true;
    case key::Right:
        if (!row || !row->hasChildren)
            return true;
        if (!isExpanded(row->id))
            setExpanded(row->id, true);
        else if (current + 1 != rows.end())
            moveTo(*(current + 1));
        return true;
    default:
        return false;
    }
}

void Sidebar::layout()
{
    const double top = metrics::headerHeight;
    m_scroll->setGeometry({0, top, geometry().width, std::max(0.0, geometry().height - top)});
}

void Sidebar::paint(Painter& painter)
{
    // The header backdrop's hairline, down the edge: 0.5 px, below 2x a
    // device pixel with its alpha scaled to the same weight.
    const double scale = painter.scale();
    const double devicePixels = std::max(1.0, std::round(0.5 * scale));
    const double lineWidth = devicePixels / scale;
    const Color separator = color(ColorId::HeaderSeparator);
    painter.drawRoundedRect({geometry().width - lineWidth, 0, lineWidth, geometry().height}, {0},
                            separator.withAlpha(separator.a / 255.0 * 0.5 * scale / devicePixels));
}

} // namespace views
