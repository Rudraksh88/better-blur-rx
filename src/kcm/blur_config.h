/*
    SPDX-FileCopyrightText: 2010 Fredrik Höglund <fredrik@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ui_blur_config.h"
#include <KCModule>
#include <QWidget>
#include <KContextualHelpButton>

#include <QList>
#include <QString>
#include <QVariantMap>

#include <optional>

namespace BBDX
{

class BlurEffectConfig : public KCModule {
    Q_OBJECT

private:
    ::Ui::BlurEffectConfig ui;

    void setContextualHelp(
        KContextualHelpButton *const contextualHelpButton,
        const QString &text,
        QWidget *const heightHintWidget = nullptr
    );
    void setupContextualHelp();
    void setupSpinboxSliderSync();

    /**
     * Fixed compile time constraints
     */
    void setupConstraints();

public:
    explicit BlurEffectConfig(QObject *parent, const KPluginMetaData &data);
    ~BlurEffectConfig() override;

    void save() override;
    bool eventFilter(QObject *watched, QEvent *event) override;

public Q_SLOTS:
    void slotRefractionModeChanged(int index);

private:
    // Window-specific overrides.
    // The rule list + editor form are a visible front-end for the hidden,
    // KConfigSkeleton-managed kcfg_WindowOverrides text edit. The two are kept
    // in sync; m_syncingOverrides guards against recursion between them and
    // m_loadingEditor suppresses editor change signals while a rule is being
    // loaded into the form.
    struct OverrideRule {
        QString windowClass;
        std::optional<double> cornerRadius;
        std::optional<int> brightness;
        std::optional<int> saturation;
        std::optional<int> contrast;
        bool squircle{false};
    };

    void setupWindowOverrides();
    void rebuildRulesFromText();
    void writeRulesToText();
    void loadEditorFromRule(int row);
    void applyEditorToRule();
    void updateOverrideItemLabel(int row);
    void updateOverrideButtonsState();
    void moveOverrideRule(int delta);
    void slotDetectWindow();
    void showDetectedProperties(const QVariantMap &info);

    QList<OverrideRule> m_overrideRules;
    bool m_syncingOverrides{false};
    bool m_loadingEditor{false};
};

} // namespace BBDX
