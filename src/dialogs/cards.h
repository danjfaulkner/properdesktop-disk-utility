// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "ui/cell_layout.h"
#include "ui/label.h"
#include "ui/view.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace properdesktop::ui {
class DialogButton;
class DialogWindow;
class RowLayout;
class ScrollView;
class TextArea;
class TextField;
class Window;
}

// The dialogs' building blocks, as Settings draws them: rounded cards of
// rows with a hairline between them, a section label over a card, a
// footnote under it, and a footer of buttons with the primary one at the
// end.
namespace dialogs {

namespace metrics {
inline constexpr double rowHeight = 44;
inline constexpr double cardPadding = 16;
inline constexpr double rowSpacing = 12;
inline constexpr double dialogSpacing = 16;
// ui::DialogWindow's padding around its content.
inline constexpr double dialogPadding = 16;
inline constexpr double sectionLabelSpacing = 8;
inline constexpr double buttonSpacing = 8;
} // namespace metrics

// A rounded panel of rows, edge to edge, in the second background colour.
class Card : public properdesktop::ui::ColumnLayout
{
public:
    Card();

protected:
    void paint(properdesktop::ui::Painter& painter) override;
};

// A row of a card: its label, then whatever is added after it, against the
// row's end. The hairline under it stops short of the card's padding.
class CardRow : public properdesktop::ui::View
{
public:
    explicit CardRow(std::string text = {});

    properdesktop::ui::Label* label() const { return m_label; }
    properdesktop::ui::RowLayout* content() const { return m_row; }
    template<typename T, typename... Args>
    T* addContent(Args&&... args)
    {
        return m_row->addChild<T>(std::forward<Args>(args)...);
    }

    properdesktop::ui::SizeF sizeHint() const override;
    double heightForWidth(double width) const override;

protected:
    void layout() override;
    void paint(properdesktop::ui::Painter& painter) override;

private:
    bool isLastRow() const;

    properdesktop::ui::RowLayout* m_row;
    properdesktop::ui::Label* m_label;
};

// A label and its value in secondary text, which can be selected and
// copied; long values wrap.
class InfoRow : public CardRow
{
public:
    InfoRow(std::string text, std::string value);

    void setValue(std::string value);
    properdesktop::ui::TextArea* value() const { return m_value; }

protected:
    void layout() override;

private:
    properdesktop::ui::TextArea* m_value;
};

// A label and a frameless text field, its text against the row's end.
class TextFieldRow : public CardRow
{
public:
    TextFieldRow(std::string text, double fieldWidth);

    properdesktop::ui::TextField* field() const { return m_field; }

    bool acceptsPointer() const override { return true; }

protected:
    void pointerPress(const properdesktop::ui::PointerEvent& event) override;

private:
    properdesktop::ui::TextField* m_field;
};

// A section's name over its card, lined up with the rows' text.
class SectionLabel : public properdesktop::ui::Label
{
public:
    explicit SectionLabel(std::string text);
};

// Small secondary text under a card; hidden while empty.
class Footnote : public properdesktop::ui::Label
{
public:
    explicit Footnote(std::string text = {});

    void setText(std::string text);
    // In red, for why an action cannot go ahead.
    void setError(bool error);
};

// A row of dialog buttons: the primary one and its companions at the end,
// anything destructive or extra at the start.
class Footer : public properdesktop::ui::View
{
public:
    template<typename T, typename... Args>
    T* addEnd(Args&&... args)
    {
        T* view = addChild<T>(std::forward<Args>(args)...);
        m_end.push_back(view);
        return view;
    }
    template<typename T, typename... Args>
    T* addStart(Args&&... args)
    {
        T* view = addChild<T>(std::forward<Args>(args)...);
        m_start.push_back(view);
        return view;
    }

    properdesktop::ui::SizeF sizeHint() const override;
    double heightForWidth(double) const override { return sizeHint().height; }

protected:
    void layout() override;

private:
    std::vector<properdesktop::ui::View*> m_start;
    std::vector<properdesktop::ui::View*> m_end;
};

// The dialogs' column: cards, section labels, notes and the footer, one
// under another with the dialog's spacing; a label sits close over its
// card and a note close under it. Labels and notes line up with the rows'
// text.
class Form : public properdesktop::ui::View
{
public:
    template<typename T, typename... Args>
    T* add(Args&&... args)
    {
        return addChild<T>(std::forward<Args>(args)...);
    }
    Card* addCard() { return add<Card>(); }
    // A section label and its card.
    Card* addSection(const std::string& title);
    // Added last.
    Footer* addFooter() { return add<Footer>(); }

    properdesktop::ui::SizeF sizeHint() const override;
    double heightForWidth(double width) const override;

protected:
    void layout() override;

private:
    double spacingAfter(size_t index) const;
    static double inset(const properdesktop::ui::View* child);
};

// The content of an edge-to-edge dialog: cards that scroll, above a footer
// of buttons. The scroll view spans the window from edge to edge, so its bar
// runs along the window's edge, and reaches the window's bottom while the
// footer is empty; the cards and the footer keep the dialog's padding.
class ScrollingForm : public properdesktop::ui::View
{
public:
    explicit ScrollingForm(std::string scrollName);

    properdesktop::ui::ScrollView* scroll;
    Form* form;
    // Shown once it has buttons.
    Footer* footer;

protected:
    void layout() override;
};

} // namespace dialogs
