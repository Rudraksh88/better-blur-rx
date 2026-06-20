/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "toggleswitch.h"

#include <QPainter>

namespace {
constexpr int TrackWidth = 40;
constexpr int TrackHeight = 22;
constexpr int KnobMargin = 2;
constexpr int TextSpacing = 8;
} // namespace

ToggleSwitch::ToggleSwitch(QWidget *parent)
    : QCheckBox(parent)
{
    setCursor(Qt::PointingHandCursor);
    // Keyboard-only focus: clicking toggles without leaving the switch
    // permanently focused (and ringed).
    setFocusPolicy(Qt::TabFocus);

    m_animation.setDuration(120);
    m_animation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_position = value.toReal();
        update();
    });
    connect(this, &QCheckBox::toggled, this, [this](const bool checked) {
        // Snap instead of animating while not on screen (e.g. config load).
        if (!isVisible()) {
            m_position = checked ? 1.0 : 0.0;
            update();
            return;
        }
        m_animation.stop();
        m_animation.setStartValue(m_position);
        m_animation.setEndValue(checked ? 1.0 : 0.0);
        m_animation.start();
    });

    m_position = isChecked() ? 1.0 : 0.0;
}

QSize ToggleSwitch::sizeHint() const
{
    int width = TrackWidth + 2;
    if (!text().isEmpty()) {
        width += TextSpacing + fontMetrics().horizontalAdvance(text());
    }
    return {width, qMax(TrackHeight + 2, fontMetrics().height())};
}

QSize ToggleSwitch::minimumSizeHint() const
{
    return sizeHint();
}

bool ToggleSwitch::hitButton(const QPoint &pos) const
{
    return rect().contains(pos);
}

void ToggleSwitch::showEvent(QShowEvent *event)
{
    // The managed config value may have been applied before the first show.
    m_position = isChecked() ? 1.0 : 0.0;
    QCheckBox::showEvent(event);
}

void ToggleSwitch::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const auto &pal = palette();
    const QColor foreground = pal.color(QPalette::WindowText);

    // Track: black at 25% opacity when off, filled with the foreground color when on.
    const QColor offColor(0, 0, 0, 120);
    QColor trackColor = QColor::fromRgbF(
        offColor.redF() + (foreground.redF() - offColor.redF()) * m_position,
        offColor.greenF() + (foreground.greenF() - offColor.greenF()) * m_position,
        offColor.blueF() + (foreground.blueF() - offColor.blueF()) * m_position,
        offColor.alphaF() + (foreground.alphaF() - offColor.alphaF()) * m_position);

    // Subtle outline keeps the transparent off-state visible; it fades out as
    // the switch turns on.
    QColor borderColor = foreground;
    borderColor.setAlphaF(0.0 * (1.0 - m_position));

    QColor knobColor(0x22, 0x22, 0x22);
    if (!isEnabled()) {
        trackColor.setAlphaF(trackColor.alphaF() * 0.4);
        borderColor.setAlphaF(borderColor.alphaF() * 0.4);
        knobColor.setAlphaF(knobColor.alphaF() * 0.6);
    }

    const QRectF track(1.0, (height() - TrackHeight) / 2.0, TrackWidth, TrackHeight);
    const qreal radius = TrackHeight / 2.0;

    if (hasFocus()) {
        QColor focusColor = pal.color(QPalette::Highlight);
        focusColor.setAlphaF(0.35);
        painter.setPen(QPen(focusColor, 2));
    } else if (borderColor.alphaF() > 0.01) {
        painter.setPen(QPen(borderColor, 1));
    } else {
        painter.setPen(Qt::NoPen);
    }
    painter.setBrush(trackColor);
    painter.drawRoundedRect(track, radius, radius);

    const qreal knobDiameter = TrackHeight - 2.0 * KnobMargin;
    const qreal knobTravel = TrackWidth - 2.0 * KnobMargin - knobDiameter;
    const QRectF knob(track.x() + KnobMargin + knobTravel * m_position,
                      track.y() + KnobMargin, knobDiameter, knobDiameter);
    QColor knobOutline = foreground;
    knobOutline.setAlphaF(0.0);
    painter.setPen(QPen(knobOutline, 1));
    painter.setBrush(knobColor);
    painter.drawEllipse(knob);

    if (!text().isEmpty()) {
        painter.setPen(pal.color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::WindowText));
        const QRectF textRect(track.right() + TextSpacing, 0,
                              width() - track.right() - TextSpacing, height());
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text());
    }
}

#include "moc_toggleswitch.cpp"
