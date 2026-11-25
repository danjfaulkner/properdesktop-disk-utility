// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "ui/view.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace properdesktop::ui {
class Icon;
class Label;
class Menu;
}

namespace dialogs {

// A card row's choice, as Settings draws one: the current item's text and a
// chevron, no fill until the pointer is over it or its menu is open. Items
// can be disabled, with why after their text in the menu.
class ChoiceButton : public properdesktop::ui::View
{
public:
    struct Item
    {
        std::string text;
        // What the choice stands for, such as "ext4".
        std::string value;
        bool enabled = true;
        // Shown after the text of a disabled item.
        std::string reason;
    };

    ChoiceButton();
    ~ChoiceButton() override;

    void setItems(std::vector<Item> items);
    const std::vector<Item>& items() const { return m_items; }
    int currentIndex() const { return m_current; }
    // Does not call onActivated.
    void setCurrentIndex(int index);
    void setCurrentValue(const std::string& value);
    std::string currentValue() const;
    std::string currentText() const;
    bool isMenuOpen() const;

    properdesktop::ui::SizeF sizeHint() const override;
    bool acceptsPointer() const override { return true; }
    bool acceptsFocus() const override { return true; }

    // After the user picked an item.
    std::function<void(int index)> onActivated;

protected:
    void layout() override;
    void paint(properdesktop::ui::Painter& painter) override;
    void enabledChanged() override;
    void pointerEnter() override;
    void pointerLeave() override;
    void pointerPress(const properdesktop::ui::PointerEvent& event) override;
    bool keyPress(const properdesktop::ui::KeyEvent& event) override;

private:
    void update();
    void toggleMenu(bool fromKeyboard);

    std::vector<Item> m_items;
    int m_current = -1;
    properdesktop::ui::Label* m_label;
    properdesktop::ui::Icon* m_chevron;
    std::unique_ptr<properdesktop::ui::Menu> m_menu;
    bool m_hovered = false;
};

} // namespace dialogs
