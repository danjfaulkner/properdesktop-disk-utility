// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/choice_button.h"

#include "base/i18n.h"
#include "ui/event.h"
#include "ui/icon.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/menu.h"
#include "ui/painter.h"
#include "ui/theme.h"

#include <cmath>

using namespace properdesktop::ui;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace dialogs {

// Above 12 px, so the glyph keeps its thin drawing.
static constexpr double kChevron = 14;
// A plain row's height less its padding, so a choice keeps the row's height.
static constexpr double kHeight = 32;

ChoiceButton::ChoiceButton()
{
    m_label = addChild<Label>();
    m_label->setVerticallyCentered(true);
    m_label->setElide(Label::Elide::Right);
    m_chevron = addChild<Icon>("chevron-down", kChevron);
}

ChoiceButton::~ChoiceButton() = default;

void ChoiceButton::setItems(std::vector<Item> items)
{
    const std::string value = currentValue();
    m_items = std::move(items);
    m_current = -1;
    setCurrentValue(value);
    if (m_current < 0 && !m_items.empty())
        m_current = 0;
    update();
}

void ChoiceButton::setCurrentIndex(int index)
{
    if (index < -1 || index >= int(m_items.size()) || index == m_current)
        return;
    m_current = index;
    update();
}

void ChoiceButton::setCurrentValue(const std::string& value)
{
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_items[i].value == value) {
            setCurrentIndex(int(i));
            return;
        }
    }
}

std::string ChoiceButton::currentValue() const
{
    return m_current >= 0 ? m_items[size_t(m_current)].value : std::string();
}

std::string ChoiceButton::currentText() const
{
    return m_current >= 0 ? m_items[size_t(m_current)].text : std::string();
}

bool ChoiceButton::isMenuOpen() const
{
    return m_menu && m_menu->isOpen();
}

void ChoiceButton::update()
{
    m_label->setText(currentText());
    requestLayout();
    requestPaint();
}

SizeF ChoiceButton::sizeHint() const
{
    const Theme& theme = Theme::current();
    const double width = theme.largeSpacing + std::ceil(m_label->sizeHint().width) + theme.smallSpacing + kChevron
            + theme.smallSpacing;
    const double height = std::max(kHeight, m_label->sizeHint().height + 2 * theme.smallSpacing);
    return {width, height};
}

void ChoiceButton::layout()
{
    const Theme& theme = Theme::current();
    const double width = geometry().width;
    const double height = geometry().height;
    const double x = theme.largeSpacing;
    const double right = width - theme.smallSpacing - kChevron - theme.smallSpacing;
    m_label->setGeometry({x, 0, std::max(0.0, right - x), height});
    m_chevron->setGeometry({width - kChevron - theme.smallSpacing, std::round((height - kChevron) / 2), kChevron,
                            kChevron});
    const bool enabled = isEnabled();
    m_chevron->setColor(enabled ? theme.text : theme.disabledText);
    m_label->setColor(enabled ? theme.text : theme.disabledText);
    m_chevron->setOpacity(enabled ? 1 : 0.6);
}

void ChoiceButton::paint(Painter& painter)
{
    const Theme& theme = Theme::current();
    const bool open = isMenuOpen();
    if (!isEnabled() || !(m_hovered || open))
        return;
    const double alpha = theme.darkMode ? (open ? 0.12 : 0.08) : (open ? 0.1 : 0.06);
    painter.drawRoundedRect(localRect(), {theme.smallRadius, 0, themeCorners()}, theme.text.withAlpha(alpha));
}

void ChoiceButton::enabledChanged()
{
    requestLayout();
    requestPaint();
}

void ChoiceButton::pointerEnter()
{
    m_hovered = true;
    requestPaint();
}

void ChoiceButton::pointerLeave()
{
    m_hovered = false;
    requestPaint();
}

void ChoiceButton::pointerPress(const PointerEvent& event)
{
    if (event.button != MouseButton::Left)
        return;
    setFocus(FocusReason::Mouse);
    toggleMenu(false);
}

bool ChoiceButton::keyPress(const KeyEvent& event)
{
    switch (event.key) {
    case key::Down:
    case key::Up:
    case key::Space:
    case key::Return:
    case key::KeypadEnter:
        toggleMenu(true);
        return true;
    default:
        return false;
    }
}

void ChoiceButton::toggleMenu(bool fromKeyboard)
{
    if (isMenuOpen()) {
        m_menu->close();
        return;
    }
    if (m_items.empty())
        return;
    m_menu = std::make_unique<Menu>();
    m_menu->setDropDown(true);
    for (size_t i = 0; i < m_items.size(); ++i) {
        const Item& item = m_items[i];
        // TRANSLATORS: a choice that cannot be made, and why: "XFS (Needs mkfs.xfs, which is not installed.)".
        const std::string text = item.enabled || item.reason.empty()
                ? item.text
                : format(tr("{choice} ({reason})"), {{"choice", item.text}, {"reason", item.reason}});
        const size_t index = m_menu->addItem(text, [this, i] {
            m_current = int(i);
            update();
            if (onActivated)
                onActivated(int(i));
        });
        m_menu->setItemChecked(index, int(i) == m_current);
        m_menu->setItemEnabled(index, item.enabled);
    }
    m_menu->setMinimumWidth(geometry().width);
    m_menu->onClosed = [this] { requestPaint(); };
    m_menu->popupFor(this, 3, std::nullopt, fromKeyboard);
    requestPaint();
}

} // namespace dialogs
