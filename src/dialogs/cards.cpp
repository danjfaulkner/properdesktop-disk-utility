// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/cards.h"

#include "ui/color_provider.h"
#include "ui/event.h"
#include "ui/painter.h"
#include "ui/scroll_view.h"
#include "ui/text_area.h"
#include "ui/text_field.h"
#include "ui/theme.h"

#include <algorithm>
#include <cmath>

using namespace properdesktop::ui;

namespace dialogs {

Card::Card()
{
    setSpacing(0);
    setLayoutHints({.fillWidth = true});
}

void Card::paint(Painter& painter)
{
    const Theme& theme = Theme::current();
    painter.drawRoundedRect(localRect(), {theme.bigRadius, 0, themeCorners()}, theme.secondBackground);
}

CardRow::CardRow(std::string text)
{
    m_row = addChild<RowLayout>();
    m_row->setSpacing(metrics::rowSpacing);
    m_label = m_row->addChild<Label>(std::move(text));
    m_label->setWrap(true);
    m_label->setMaximumLineCount(2);
    m_label->setLayoutHints({.fillWidth = true});
    m_label->setVisible(!m_label->text().empty());
    setLayoutHints({.fillWidth = true});
}

SizeF CardRow::sizeHint() const
{
    const SizeF row = m_row->sizeHint();
    return {row.width + 2 * metrics::cardPadding, std::max(metrics::rowHeight, row.height + 12)};
}

double CardRow::heightForWidth(double width) const
{
    const double content = std::max(0.0, width - 2 * metrics::cardPadding);
    return std::max(metrics::rowHeight, std::ceil(m_row->heightForWidth(content)) + 12);
}

void CardRow::layout()
{
    const double width = geometry().width;
    m_row->setGeometry({metrics::cardPadding, 0, std::max(0.0, width - 2 * metrics::cardPadding),
                        geometry().height});
}

bool CardRow::isLastRow() const
{
    if (!parent())
        return true;
    const CardRow* last = nullptr;
    for (const auto& child : parent()->children()) {
        if (const auto* row = dynamic_cast<const CardRow*>(child.get()); row && row->isVisible())
            last = row;
    }
    return last == this;
}

void CardRow::paint(Painter& painter)
{
    if (isLastRow())
        return;
    // One device pixel at any scale.
    const double pixel = 1 / painter.scale();
    painter.drawRoundedRect({metrics::cardPadding, geometry().height - pixel,
                             geometry().width - 2 * metrics::cardPadding, pixel},
                            {}, color(ColorId::Separator));
}

InfoRow::InfoRow(std::string text, std::string value)
    : CardRow(std::move(text))
{
    m_value = addContent<TextArea>();
    m_value->setReadOnly(true);
    m_value->setFrameVisible(false);
    m_value->setPadding({0, 0, 0, 0});
    m_value->setWrapMode(TextArea::WrapMode::Wrap);
    m_value->setHorizontalAlignment(TextArea::HorizontalAlignment::Right);
    setValue(std::move(value));
}

void InfoRow::setValue(std::string value)
{
    m_value->setText(std::move(value));
}

void InfoRow::layout()
{
    m_value->setTextColor(Theme::current().disabledText);
    // A long value such as a path would otherwise squeeze the label out.
    const double content = std::max(0.0, geometry().width - 2 * metrics::cardPadding);
    const double cap = std::round(content * 0.65);
    const double natural = std::ceil(m_value->sizeHint().width);
    const double width = std::min(natural, cap);
    if (m_value->layoutHints().preferredWidth != width)
        m_value->setLayoutHints({.preferredWidth = width});
    CardRow::layout();
}

TextFieldRow::TextFieldRow(std::string text, double fieldWidth)
    : CardRow(std::move(text))
{
    m_field = addContent<TextField>();
    m_field->setFrameVisible(false);
    m_field->setRightAligned(true);
    m_field->setTextMargins(0, 0);
    // The field's own height would make the row higher than its neighbours.
    m_field->setLayoutHints({.preferredWidth = fieldWidth, .preferredHeight = metrics::rowHeight - 12});
}

void TextFieldRow::pointerPress(const PointerEvent& event)
{
    if (event.button == MouseButton::Left)
        m_field->setFocus(FocusReason::Mouse);
}

SectionLabel::SectionLabel(std::string text)
    : Label(std::move(text))
{
    setColor(ColorId::DisabledText);
    setElide(Elide::Right);
}

Footnote::Footnote(std::string text)
    : Label({})
{
    setWrap(true);
    setFontSize(Theme::current().smallFontSize());
    setColor(ColorId::DisabledText);
    setText(std::move(text));
}

void Footnote::setText(std::string text)
{
    setVisible(!text.empty());
    Label::setText(std::move(text));
}

void Footnote::setError(bool error)
{
    if (error)
        setColor(accent::red);
    else
        setColor(ColorId::DisabledText);
}

SizeF Footer::sizeHint() const
{
    double width = 0;
    double height = 0;
    for (const auto& child : children()) {
        if (!child->isVisible())
            continue;
        const SizeF size = child->sizeHint();
        width += std::ceil(size.width) + (width > 0 ? metrics::buttonSpacing : 0);
        height = std::max(height, std::ceil(size.height));
    }
    return {width, height};
}

void Footer::layout()
{
    const double height = geometry().height;
    double x = 0;
    for (View* view : m_start) {
        if (!view->isVisible())
            continue;
        const SizeF size = view->sizeHint();
        view->setGeometry({x, std::round((height - size.height) / 2), std::ceil(size.width), size.height});
        x += std::ceil(size.width) + metrics::buttonSpacing;
    }
    double right = geometry().width;
    for (auto it = m_end.rbegin(); it != m_end.rend(); ++it) {
        if (!(*it)->isVisible())
            continue;
        const SizeF size = (*it)->sizeHint();
        const double width = std::ceil(size.width);
        (*it)->setGeometry({right - width, std::round((height - size.height) / 2), width, size.height});
        right -= width + metrics::buttonSpacing;
    }
}

Card* Form::addSection(const std::string& title)
{
    add<SectionLabel>(title);
    return addCard();
}

double Form::inset(const View* child)
{
    return dynamic_cast<const Label*>(child) ? metrics::cardPadding : 0;
}

double Form::spacingAfter(size_t index) const
{
    const View* child = children()[index].get();
    const View* next = nullptr;
    for (size_t i = index + 1; i < children().size() && !next; ++i) {
        if (children()[i]->isVisible())
            next = children()[i].get();
    }
    if (!next)
        return 0;
    if (dynamic_cast<const SectionLabel*>(child) || dynamic_cast<const Footnote*>(next))
        return metrics::sectionLabelSpacing;
    return metrics::dialogSpacing;
}

double Form::heightForWidth(double width) const
{
    double height = 0;
    for (size_t i = 0; i < children().size(); ++i) {
        const View* child = children()[i].get();
        if (!child->isVisible())
            continue;
        height += std::ceil(child->heightForWidth(width - 2 * inset(child))) + spacingAfter(i);
    }
    return height;
}

SizeF Form::sizeHint() const
{
    return {0, heightForWidth(geometry().width)};
}

void Form::layout()
{
    const double width = geometry().width;
    double y = 0;
    for (size_t i = 0; i < children().size(); ++i) {
        View* child = children()[i].get();
        if (!child->isVisible())
            continue;
        const double x = inset(child);
        const double height = std::ceil(child->heightForWidth(width - 2 * x));
        child->setGeometry({x, y, width - 2 * x, height});
        y += height + spacingAfter(i);
    }
}

// The form, keeping the dialog's padding at its sides and below it.
class PaddedForm : public View
{
public:
    PaddedForm() { form = addChild<Form>(); }

    double heightForWidth(double width) const override
    {
        return form->heightForWidth(std::max(0.0, width - 2 * metrics::dialogPadding)) + metrics::dialogPadding;
    }

    Form* form;

protected:
    void layout() override
    {
        const double width = std::max(0.0, geometry().width - 2 * metrics::dialogPadding);
        form->setGeometry({metrics::dialogPadding, 0, width, form->heightForWidth(width)});
    }
};

ScrollingForm::ScrollingForm(std::string scrollName)
{
    scroll = addChild<ScrollView>();
    scroll->setName(std::move(scrollName));
    form = scroll->setContent<PaddedForm>()->form;
    footer = addChild<Footer>();
}

void ScrollingForm::layout()
{
    const double padding = metrics::dialogPadding;
    const double width = geometry().width;
    const double height = geometry().height;
    const bool hasFooter = !footer->children().empty();
    footer->setVisible(hasFooter);
    const double footerHeight = hasFooter ? footer->sizeHint().height : 0;
    const double footerTop = height - padding - footerHeight;
    const double scrollHeight = hasFooter ? std::max(0.0, footerTop - metrics::dialogSpacing) : height;
    scroll->setBottomCornerRadius(hasFooter ? 0 : Theme::current().windowRadius);
    scroll->setGeometry({0, 0, width, scrollHeight});
    footer->setGeometry({padding, footerTop, std::max(0.0, width - 2 * padding), footerHeight});
}

} // namespace dialogs
