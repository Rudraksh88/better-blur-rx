/*
    SPDX-FileCopyrightText: 2010 Fredrik Höglund <fredrik@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "blur_config.h"

//#include <config-kwin.h>

// KConfigSkeleton
#include "blurconfig.h"

#include <KPluginFactory>
#include "kwineffects_interface.h"
#include "settings.hpp"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QSlider>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

namespace {
// Field order of one serialized override rule (tab-separated, one rule per line).
enum OverrideField {
    FieldClass = 0,
    FieldCornerRadius,
    FieldBrightness,
    FieldSaturation,
    FieldContrast,
    FieldCount,
};
}

namespace BBDX
{

K_PLUGIN_CLASS(BlurEffectConfig)

BlurEffectConfig::BlurEffectConfig(QObject *parent, const KPluginMetaData &data)
    : KCModule(parent, data)
{
    ui.setupUi(widget());
    BlurConfig::instance("kwinrc");
    addConfig(BlurConfig::self(), widget());

    // macOS-style settings cards: bold section headers above rounded panels
    // with thin separators between rows.
    widget()->setStyleSheet(QStringLiteral(
        "QFrame[settingsCard=\"true\"] {"
        "  background-color: palette(base);"
        "  border: 1px solid rgba(255, 255, 255, 5%);"
        "  border-radius: 5px;"
        "}"
        "QFrame[cardSeparator=\"true\"] { color: palette(midlight); }"
        "QLabel[sectionHeader=\"true\"] { margin-left: 2px; margin-top: 4px; }"));

    // Qt stylesheets ignore letter-spacing (and weight numbers), so style the
    // section headers through QFont instead.
    for (auto *header : widget()->findChildren<QLabel *>()) {
        if (!header->property("sectionHeader").toBool()) {
            continue;
        }
        QFont font = header->font();
        font.setBold(true);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.2);
        header->setFont(font);
    }

    // Align the right edge within each card: every spinbox/combobox in a card
    // gets the width of the widest one.
    for (auto *card : widget()->findChildren<QFrame *>()) {
        if (!card->property("settingsCard").toBool()) {
            continue;
        }
        QList<QWidget *> inputs;
        for (auto *child : card->findChildren<QWidget *>()) {
            if (qobject_cast<QAbstractSpinBox *>(child) || qobject_cast<QComboBox *>(child)) {
                inputs.append(child);
            }
        }
        int maxWidth = 0;
        for (const auto *input : std::as_const(inputs)) {
            maxWidth = qMax(maxWidth, input->sizeHint().width());
        }
        for (auto *input : std::as_const(inputs)) {
            input->setFixedWidth(maxWidth);
        }
    }

    QFile about(":/effects/better_blur_dx/kcm/about.html");
    if (about.open(QIODevice::ReadOnly)) {
        const auto html = about.readAll()
            .replace("${version}", ABOUT_VERSION_STRING)
            .replace("${repo}", "https://github.com/xarblu/kwin-effects-better-blur-dx");
        ui.aboutText->setHtml(html);
    }

    setupContextualHelp();
    setupSpinboxSliderSync();
    setupConstraints();
    setupWindowOverrides();

    connect(ui.kcfg_RefractionMode, &QComboBox::currentIndexChanged, this, &BlurEffectConfig::slotRefractionModeChanged);
    slotRefractionModeChanged(ui.kcfg_RefractionMode->currentIndex());

    // Fix up the hosting dialog's window title once it shows.
    widget()->installEventFilter(this);
}

BlurEffectConfig::~BlurEffectConfig() {}

void BlurEffectConfig::setContextualHelp(
    KContextualHelpButton *const contextualHelpButton,
    const QString &text,
    QWidget *const heightHintWidget
) {
    contextualHelpButton->setContextualHelpText(text);
    if (heightHintWidget) {
        const auto ownHeightHint = contextualHelpButton->sizeHint().height();
        const auto otherHeightHint = heightHintWidget->sizeHint().height();
        if (ownHeightHint >= otherHeightHint) {
            contextualHelpButton->setHeightHintWidget(heightHintWidget);
        }
    }
}

void BlurEffectConfig::setupContextualHelp() {
    setContextualHelp(
        ui.windowClassesContextualHelp,
        QStringLiteral("<p>Specify one window class pattern per line.</p>") +

        QStringLiteral("<p><strong>Exact match:</strong><br>") +
        QStringLiteral("By default window classes are matched exactly.<br>") +
        QStringLiteral("Example: <code>org.kde.dolphin</code> matches only Dolphin.</p>") +

        QStringLiteral("<p><strong>Regex match:</strong><br>") +
        QStringLiteral("If wrapped with <code>/</code> window classes are matched by Perl compatible regular expression.<br>") +
        QStringLiteral("Example: <code>/^org\\.kde\\..*/</code> matches all KDE applications.</p>") +

        QStringLiteral("<p><strong>Empty match:</strong><br>") +
        QStringLiteral("Use the special value <code>$blank</code> to match empty window classes.</p>"),
        ui.windowClassesBriefDescription
    );

    setContextualHelp(
        ui.windowOverridesContextualHelp,
        QStringLiteral("<p>Override blur parameters for specific window classes.</p>") +

        QStringLiteral("<p>The <strong>Window class</strong> field uses the same matching ") +
        QStringLiteral("rules as the force blur list: exact match, <code>/regex/</code> ") +
        QStringLiteral("for Perl compatible regular expressions, or <code>$blank</code> ") +
        QStringLiteral("for empty window classes. Use <strong>Detect…</strong> and click ") +
        QStringLiteral("a window to fill it in automatically.</p>") +

        QStringLiteral("<p>If several rules match a window, the <strong>first</strong> ") +
        QStringLiteral("matching rule (top to bottom) is used — reorder rules with the ") +
        QStringLiteral("arrow buttons.</p>") +

        QStringLiteral("<p>Settings left unchecked inherit the global values from the ") +
        QStringLiteral("other tabs.</p>"),
        ui.windowOverridesBriefDescription
    );
}

void BlurEffectConfig::setupSpinboxSliderSync()
{
    // Every slider gets a spinbox companion showing (and accepting) the exact value.
    const auto sync = [this](QSlider *slider, QSpinBox *spinbox) {
        connect(slider, &QSlider::valueChanged, spinbox, &QSpinBox::setValue);
        connect(spinbox, &QSpinBox::valueChanged, slider, &QSlider::setValue);
        spinbox->setValue(slider->value());
    };

    sync(ui.kcfg_BlurStrength, ui.spinboxBlurStrength);
    sync(ui.kcfg_NoiseStrength, ui.spinboxNoiseStrength);
    sync(ui.kcfg_Brightness, ui.spinboxBrightness);
    sync(ui.kcfg_Saturation, ui.spinboxSaturation);
    sync(ui.kcfg_Contrast, ui.spinboxContrast);
    sync(ui.kcfg_RefractionStrength, ui.spinboxRefractionStrength);
    sync(ui.kcfg_RefractionEdgeSize, ui.spinboxRefractionEdgeSize);
    sync(ui.kcfg_RefractionNormalPow, ui.spinboxRefractionNormalPow);
    sync(ui.kcfg_RefractionCornerRadius, ui.spinboxRefractionCornerRadius);
    sync(ui.kcfg_RefractionRGBFringing, ui.spinboxRefractionRGBFringing);
}

bool BlurEffectConfig::eventFilter(QObject *watched, QEvent *event)
{
    // The Desktop Effects KCM hosts this module in a generic dialog titled
    // "Configure — System Settings"; rename it when it appears. Embedded
    // (non-dialog) hosts are left alone.
    if (watched == widget() && event->type() == QEvent::Show) {
        if (auto *dialog = qobject_cast<QDialog *>(widget()->window())) {
            dialog->setWindowTitle(QStringLiteral("Configure — Better Blur DX"));
        }
    }
    return KCModule::eventFilter(watched, event);
}

void BlurEffectConfig::setupConstraints() {
#if defined(BBDX_X11)
    /**
     * X11 only has one mode available
     */
    ui.kcfg_BlitMode->setEnabled(false);
#endif

    // wallpaper mode expects the cache
    // and doesn't care about the flush interval
    auto slotBlitModeChanged = [this](int index) {
        switch (static_cast<BBDX::BlitMode>(index)) {
            case BBDX::BlitMode::WALLPAPER:
                ui.kcfg_BlurCacheIgnore->setEnabled(false);
                ui.kcfg_BlurCacheRateLimit->setEnabled(false);
                break;

            default:
                ui.kcfg_BlurCacheIgnore->setEnabled(true);
                ui.kcfg_BlurCacheRateLimit->setEnabled(true);
                break;
        }
    };
    connect(ui.kcfg_BlitMode, &QComboBox::currentIndexChanged, this, slotBlitModeChanged);
    slotBlitModeChanged(ui.kcfg_BlitMode->currentIndex());
}

void BlurEffectConfig::slotRefractionModeChanged(int index) {
    // 1 = concave
    // TODO: make this an enum
    const bool concave{index == 1};

    // Edge behaviour is not relevant for concave mode
    if (ui.kcfg_RefractionTextureRepeatMode) {
        ui.kcfg_RefractionTextureRepeatMode->setEnabled(!concave);
    }
    if (ui.labelRefractionTextureRepeatMode) {
        ui.labelRefractionTextureRepeatMode->setEnabled(!concave);
    }

    // Corner radius is only relevant for Concave as Basic breaks with low values
    if (ui.kcfg_RefractionCornerRadius) {
        ui.kcfg_RefractionCornerRadius->setEnabled(concave);
    }
    if (ui.spinboxRefractionCornerRadius) {
        ui.spinboxRefractionCornerRadius->setEnabled(concave);
    }
    if (ui.labelRefractionCornerRadius) {
        ui.labelRefractionCornerRadius->setEnabled(concave);
    }
}

void BlurEffectConfig::setupWindowOverrides()
{
    connect(ui.overridesList, &QListWidget::currentRowChanged, this, [this](int row) {
        loadEditorFromRule(row);
        updateOverrideButtonsState();
    });

    connect(ui.addOverrideButton, &QPushButton::clicked, this, [this]() {
        m_overrideRules.append(OverrideRule{});
        ui.overridesList->addItem(QString());
        updateOverrideItemLabel(m_overrideRules.size() - 1);
        ui.overridesList->setCurrentRow(m_overrideRules.size() - 1);
        ui.overrideClassEdit->setFocus();
    });
    connect(ui.removeOverrideButton, &QPushButton::clicked, this, [this]() {
        const int row = ui.overridesList->currentRow();
        if (row < 0 || row >= m_overrideRules.size()) {
            return;
        }
        m_overrideRules.removeAt(row);
        delete ui.overridesList->takeItem(row);
        writeRulesToText();
        updateOverrideButtonsState();
    });
    connect(ui.moveOverrideUpButton, &QToolButton::clicked, this, [this]() { moveOverrideRule(-1); });
    connect(ui.moveOverrideDownButton, &QToolButton::clicked, this, [this]() { moveOverrideRule(1); });

    connect(ui.overrideClassEdit, &QLineEdit::textEdited, this, &BlurEffectConfig::applyEditorToRule);
    connect(ui.detectWindowButton, &QPushButton::clicked, this, &BlurEffectConfig::slotDetectWindow);

    // Each checkbox toggles whether the rule overrides that setting. When the user
    // first enables one, seed the spinbox with the current global value so they
    // start from what they already see on screen.
    const auto wireCheck = [this](QCheckBox *check, QAbstractSpinBox *box, auto seedGlobal) {
        connect(check, &QCheckBox::toggled, this, [this, box, seedGlobal](bool checked) {
            box->setEnabled(checked);
            if (checked && !m_loadingEditor) {
                seedGlobal();
            }
            applyEditorToRule();
        });
    };
    wireCheck(ui.overrideRadiusCheck, ui.overrideRadiusBox, [this]() {
        const int row = ui.overridesList->currentRow();
        if (row >= 0 && row < m_overrideRules.size() && !m_overrideRules.at(row).cornerRadius) {
            ui.overrideRadiusBox->setValue(ui.kcfg_CornerRadius->value());
        }
    });
    wireCheck(ui.overrideBrightnessCheck, ui.overrideBrightnessBox, [this]() {
        const int row = ui.overridesList->currentRow();
        if (row >= 0 && row < m_overrideRules.size() && !m_overrideRules.at(row).brightness) {
            ui.overrideBrightnessBox->setValue(ui.kcfg_Brightness->value());
        }
    });
    wireCheck(ui.overrideSaturationCheck, ui.overrideSaturationBox, [this]() {
        const int row = ui.overridesList->currentRow();
        if (row >= 0 && row < m_overrideRules.size() && !m_overrideRules.at(row).saturation) {
            ui.overrideSaturationBox->setValue(ui.kcfg_Saturation->value());
        }
    });
    wireCheck(ui.overrideContrastCheck, ui.overrideContrastBox, [this]() {
        const int row = ui.overridesList->currentRow();
        if (row >= 0 && row < m_overrideRules.size() && !m_overrideRules.at(row).contrast) {
            ui.overrideContrastBox->setValue(ui.kcfg_Contrast->value());
        }
    });

    connect(ui.overrideRadiusBox, &QDoubleSpinBox::valueChanged, this, &BlurEffectConfig::applyEditorToRule);
    connect(ui.overrideBrightnessBox, &QSpinBox::valueChanged, this, &BlurEffectConfig::applyEditorToRule);
    connect(ui.overrideSaturationBox, &QSpinBox::valueChanged, this, &BlurEffectConfig::applyEditorToRule);
    connect(ui.overrideContrastBox, &QSpinBox::valueChanged, this, &BlurEffectConfig::applyEditorToRule);

    // The hidden kcfg_WindowOverrides text edit is the source of truth managed by
    // KConfigSkeleton. Mirror its content into the rule list whenever it changes
    // (e.g. after load() or defaults()), and mirror edits back into it (which
    // marks the KCM dirty).
    connect(ui.kcfg_WindowOverrides, &QPlainTextEdit::textChanged, this, &BlurEffectConfig::rebuildRulesFromText);

    rebuildRulesFromText();
}

void BlurEffectConfig::rebuildRulesFromText()
{
    if (m_syncingOverrides) {
        return;
    }
    m_syncingOverrides = true;

    m_overrideRules.clear();

    const QStringList lines = ui.kcfg_WindowOverrides->toPlainText().split(QChar('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList fields = line.split(QChar('\t'));
        if (fields.isEmpty()) {
            continue;
        }

        OverrideRule rule;
        rule.windowClass = fields.value(FieldClass).trimmed();

        bool ok = false;
        const double radius = fields.value(FieldCornerRadius).toDouble(&ok);
        if (ok) {
            rule.cornerRadius = radius;
        }
        const auto parseInt = [&fields](int field) -> std::optional<int> {
            bool ok = false;
            const int value = fields.value(field).toInt(&ok);
            return ok ? std::optional<int>(value) : std::nullopt;
        };
        rule.brightness = parseInt(FieldBrightness);
        rule.saturation = parseInt(FieldSaturation);
        rule.contrast = parseInt(FieldContrast);

        m_overrideRules.append(rule);
    }

    ui.overridesList->clear();
    for (int row = 0; row < m_overrideRules.size(); ++row) {
        ui.overridesList->addItem(QString());
        updateOverrideItemLabel(row);
    }

    m_syncingOverrides = false;

    if (!m_overrideRules.isEmpty()) {
        ui.overridesList->setCurrentRow(0);
    }
    loadEditorFromRule(ui.overridesList->currentRow());
    updateOverrideButtonsState();
}

void BlurEffectConfig::writeRulesToText()
{
    if (m_syncingOverrides) {
        return;
    }

    QStringList lines;
    for (const OverrideRule &rule : std::as_const(m_overrideRules)) {
        // A rule without a class matcher is useless; drop it. It stays visible
        // in the list (so a freshly added rule does not vanish) but is not saved.
        if (rule.windowClass.isEmpty()) {
            continue;
        }
        const QStringList fields = {
            rule.windowClass,
            rule.cornerRadius ? QString::number(*rule.cornerRadius, 'f', 1) : QString(),
            rule.brightness ? QString::number(*rule.brightness) : QString(),
            rule.saturation ? QString::number(*rule.saturation) : QString(),
            rule.contrast ? QString::number(*rule.contrast) : QString(),
        };
        lines.append(fields.join(QChar('\t')));
    }

    // The list already reflects this state, so block the
    // textChanged -> rebuildRulesFromText round-trip while we update the hidden field.
    const QString serialized = lines.join(QChar('\n'));
    if (ui.kcfg_WindowOverrides->toPlainText() != serialized) {
        m_syncingOverrides = true;
        ui.kcfg_WindowOverrides->setPlainText(serialized);
        m_syncingOverrides = false;
    }
}

void BlurEffectConfig::loadEditorFromRule(int row)
{
    const bool valid = row >= 0 && row < m_overrideRules.size();
    ui.overrideEditorGroup->setEnabled(valid);

    m_loadingEditor = true;
    if (valid) {
        const OverrideRule &rule = m_overrideRules.at(row);
        ui.overrideClassEdit->setText(rule.windowClass);
        // Long values should show their beginning, not their tail.
        ui.overrideClassEdit->setCursorPosition(0);
        ui.overrideRadiusCheck->setChecked(rule.cornerRadius.has_value());
        ui.overrideRadiusBox->setEnabled(rule.cornerRadius.has_value());
        ui.overrideRadiusBox->setValue(rule.cornerRadius.value_or(ui.kcfg_CornerRadius->value()));
        ui.overrideBrightnessCheck->setChecked(rule.brightness.has_value());
        ui.overrideBrightnessBox->setEnabled(rule.brightness.has_value());
        ui.overrideBrightnessBox->setValue(rule.brightness.value_or(ui.kcfg_Brightness->value()));
        ui.overrideSaturationCheck->setChecked(rule.saturation.has_value());
        ui.overrideSaturationBox->setEnabled(rule.saturation.has_value());
        ui.overrideSaturationBox->setValue(rule.saturation.value_or(ui.kcfg_Saturation->value()));
        ui.overrideContrastCheck->setChecked(rule.contrast.has_value());
        ui.overrideContrastBox->setEnabled(rule.contrast.has_value());
        ui.overrideContrastBox->setValue(rule.contrast.value_or(ui.kcfg_Contrast->value()));
    } else {
        ui.overrideClassEdit->clear();
        for (QCheckBox *check : {ui.overrideRadiusCheck, ui.overrideBrightnessCheck,
                                 ui.overrideSaturationCheck, ui.overrideContrastCheck}) {
            check->setChecked(false);
        }
    }
    m_loadingEditor = false;
}

void BlurEffectConfig::applyEditorToRule()
{
    if (m_loadingEditor) {
        return;
    }
    const int row = ui.overridesList->currentRow();
    if (row < 0 || row >= m_overrideRules.size()) {
        return;
    }

    OverrideRule &rule = m_overrideRules[row];
    rule.windowClass = ui.overrideClassEdit->text().trimmed();
    rule.cornerRadius = ui.overrideRadiusCheck->isChecked()
        ? std::optional<double>(ui.overrideRadiusBox->value()) : std::nullopt;
    rule.brightness = ui.overrideBrightnessCheck->isChecked()
        ? std::optional<int>(ui.overrideBrightnessBox->value()) : std::nullopt;
    rule.saturation = ui.overrideSaturationCheck->isChecked()
        ? std::optional<int>(ui.overrideSaturationBox->value()) : std::nullopt;
    rule.contrast = ui.overrideContrastCheck->isChecked()
        ? std::optional<int>(ui.overrideContrastBox->value()) : std::nullopt;

    updateOverrideItemLabel(row);
    writeRulesToText();
}

void BlurEffectConfig::updateOverrideItemLabel(int row)
{
    QListWidgetItem *item = ui.overridesList->item(row);
    if (!item || row >= m_overrideRules.size()) {
        return;
    }
    const OverrideRule &rule = m_overrideRules.at(row);

    item->setText(rule.windowClass.isEmpty()
        ? QStringLiteral("(new rule — set a window class)") : rule.windowClass);

    QStringList parts;
    if (rule.cornerRadius) {
        parts.append(QStringLiteral("corner radius %1 px").arg(*rule.cornerRadius));
    }
    if (rule.brightness) {
        parts.append(QStringLiteral("brightness %1%").arg(*rule.brightness));
    }
    if (rule.saturation) {
        parts.append(QStringLiteral("saturation %1%").arg(*rule.saturation));
    }
    if (rule.contrast) {
        parts.append(QStringLiteral("contrast %1%").arg(*rule.contrast));
    }
    item->setToolTip(parts.isEmpty()
        ? QStringLiteral("No overrides set — everything inherited") : parts.join(QStringLiteral(", ")));
}

void BlurEffectConfig::updateOverrideButtonsState()
{
    const int row = ui.overridesList->currentRow();
    const bool valid = row >= 0 && row < m_overrideRules.size();
    ui.removeOverrideButton->setEnabled(valid);
    ui.moveOverrideUpButton->setEnabled(valid && row > 0);
    ui.moveOverrideDownButton->setEnabled(valid && row < m_overrideRules.size() - 1);
}

void BlurEffectConfig::moveOverrideRule(int delta)
{
    const int row = ui.overridesList->currentRow();
    const int target = row + delta;
    if (row < 0 || row >= m_overrideRules.size() || target < 0 || target >= m_overrideRules.size()) {
        return;
    }

    m_overrideRules.swapItemsAt(row, target);
    QListWidgetItem *item = ui.overridesList->takeItem(row);
    ui.overridesList->insertItem(target, item);
    ui.overridesList->setCurrentRow(target);
    writeRulesToText();
}

void BlurEffectConfig::slotDetectWindow()
{
    // Ask the compositor to let the user pick a window. KWin turns the cursor
    // into a crosshair; clicking a window resolves the call with its properties,
    // Escape cancels it.
    ui.detectWindowButton->setEnabled(false);

    const QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.KWin"),
        QStringLiteral("/KWin"),
        QStringLiteral("org.kde.KWin"),
        QStringLiteral("queryWindowInfo"));
    // Picking can take as long as the user wants; don't use the 25 s default timeout.
    const QDBusPendingCall call = QDBusConnection::sessionBus().asyncCall(message, 300000);

    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        watcher->deleteLater();
        ui.detectWindowButton->setEnabled(true);

        const QDBusPendingReply<QVariantMap> reply = *watcher;
        if (reply.isError()) {
            // Escape during picking is a normal cancel, not an error worth a dialog.
            if (!reply.error().name().contains(QStringLiteral("UserCancelled"))) {
                QMessageBox::warning(widget()->window(), QStringLiteral("Detect window properties"),
                                     QStringLiteral("Could not query the window: %1").arg(reply.error().message()));
            }
            return;
        }
        showDetectedProperties(reply.value());
    });
}

void BlurEffectConfig::showDetectedProperties(const QVariantMap &info)
{
    const QString resourceClass = info.value(QStringLiteral("resourceClass")).toString();
    const QString resourceName = info.value(QStringLiteral("resourceName")).toString();
    const QString desktopFile = info.value(QStringLiteral("desktopFile")).toString();
    const QString caption = info.value(QStringLiteral("caption")).toString();

    QDialog dialog(widget()->window());
    dialog.setWindowTitle(QStringLiteral("Detected Window Properties"));
    // Size the dialog relative to the settings window so the property values
    // are not truncated.
    dialog.setMinimumWidth(qMax(480, widget()->window()->width() - 80));

    // Only class and name are matched by the effect; everything else is shown
    // read-only for reference (and easy copying).
    QList<QPair<QString, QString>> candidates;
    if (!resourceClass.isEmpty()) {
        candidates.append({QStringLiteral("Window class:"), resourceClass});
    }
    if (!resourceName.isEmpty() && resourceName != resourceClass) {
        candidates.append({QStringLiteral("Window name:"), resourceName});
    }

    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(candidates.size() > 1
        ? QStringLiteral("Select which property to use as the window class matcher:")
        : QStringLiteral("Detected window properties:"), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *group = new QButtonGroup(&dialog);

    const auto addValueRow = [&](QWidget *label, const QString &value) {
        auto *valueEdit = new QLineEdit(value, &dialog);
        valueEdit->setReadOnly(true);
        valueEdit->setCursorPosition(0);
        form->addRow(label, valueEdit);
    };

    // With a single candidate a lone radio button is just noise — show a plain
    // label and use the value directly on OK.
    if (candidates.size() > 1) {
        bool first = true;
        for (const auto &[label, value] : candidates) {
            auto *radio = new QRadioButton(label, &dialog);
            radio->setProperty("matchValue", value);
            radio->setChecked(first);
            first = false;
            group->addButton(radio);
            addValueRow(radio, value);
        }
    } else {
        for (const auto &[label, value] : candidates) {
            addValueRow(new QLabel(label, &dialog), value);
        }
    }
    if (!desktopFile.isEmpty()) {
        addValueRow(new QLabel(QStringLiteral("Desktop file:"), &dialog), desktopFile);
    }
    if (!caption.isEmpty()) {
        addValueRow(new QLabel(QStringLiteral("Title:"), &dialog), caption);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted || candidates.isEmpty()) {
        return;
    }
    QString chosen = candidates.first().second;
    if (const QAbstractButton *checked = group->checkedButton()) {
        chosen = checked->property("matchValue").toString();
    }
    ui.overrideClassEdit->setText(chosen);
    ui.overrideClassEdit->setCursorPosition(0);
    applyEditorToRule();
}

void BlurEffectConfig::save()
{
    KCModule::save();

    OrgKdeKwinEffectsInterface interface(QStringLiteral("org.kde.KWin"),
                                         QStringLiteral("/Effects"),
                                         QDBusConnection::sessionBus());

    interface.reconfigureEffect(QStringLiteral("better_blur_dx"));
}

} // namespace BBDX

#include "blur_config.moc"

#include "moc_blur_config.cpp"
