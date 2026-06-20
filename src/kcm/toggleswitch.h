/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QCheckBox>
#include <QVariantAnimation>

/**
 * An iOS/macOS-style toggle switch. Behaves exactly like a QCheckBox (and is
 * therefore picked up by KConfigDialogManager through the "checked" user
 * property), but paints a sliding switch instead of a check indicator.
 */
class ToggleSwitch : public QCheckBox
{
    Q_OBJECT

public:
    explicit ToggleSwitch(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    bool hitButton(const QPoint &pos) const override;
    void showEvent(QShowEvent *event) override;

private:
    /// Knob position, 0.0 (off) .. 1.0 (on), animated on toggle.
    qreal m_position{0.0};
    QVariantAnimation m_animation;
};
