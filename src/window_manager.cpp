#include "window_manager.hpp"

#include "blur.h"
#include "blurconfig.h"
#include "kwin_compat.hpp"
#include "utils.h"
#include "window.hpp"

#include <core/outputlayer.h>
#include <core/pixelgrid.h>
#include <core/renderviewport.h>
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>
#include <qloggingcategory.h>
#include <scene/borderradius.h>
#include <scene/windowitem.h>
#include <window.h>

#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 5, 80)
#  include <core/output.h>
#  include <core/region.h>
#endif

#include <QList>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QString>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

Q_LOGGING_CATEGORY(WINDOW_MANAGER, "kwin_effect_better_blur_dx.window_manager", QtInfoMsg)

BBDX::WindowManager::WindowManager(BBDX::BlurEffect *effect) {
    m_effect = effect;

    // add existing windows
    for (const auto &window : KWin::effects->stackingOrder()) {
        slotWindowAdded(window);
    }

    connect(KWin::effects, &KWin::EffectsHandler::windowAdded, this, &WindowManager::slotWindowAdded);
    connect(KWin::effects, &KWin::EffectsHandler::windowDeleted, this, &WindowManager::slotWindowDeleted);
}

void BBDX::WindowManager::slotWindowAdded(KWin::EffectWindow *w) {
    auto window = std::make_unique<BBDX::Window>(this, w);

    m_windows.insert_or_assign(w, std::move(window));

    if (w->isDock()) {
        m_docks.insert(w);
        refreshMaximizedStateAll();
    }

    qCDebug(WINDOW_MANAGER) << BBDX::LOG_PREFIX << "Window added:" << *(m_windows[w]);
}

void BBDX::WindowManager::slotWindowDeleted(KWin::EffectWindow *w) {
    if (const auto it = m_windows.find(w); it != m_windows.end()) {
        qCDebug(WINDOW_MANAGER) << BBDX::LOG_PREFIX << "Window removed:" << *(it->second);
        m_windows.erase(it);
    }

    if (const auto it = m_docks.find(w); it != m_docks.end()) {
        m_docks.erase(it);
        refreshMaximizedStateAll();
    }
}

BBDX::Window* BBDX::WindowManager::findWindow(const KWin::EffectWindow *w) const {
    if (const auto it = m_windows.find(w); it != m_windows.end()) {
        return it->second.get();
    }
    return nullptr;
}

std::vector<BBDX::Window *> BBDX::WindowManager::windowsByStackingOrder() const {
    std::vector<BBDX::Window *> windows{};

    for (const auto &[kWindow, bbdxWindow] : m_windows) {

        // insert sorted by stackingOrder
        for (auto it = windows.begin(); ; it++) {
            if (it == windows.end()) {
                windows.insert(it, bbdxWindow.get());
                break;
            }

            if ((*it)->effectwindow()->window()->stackingOrder() > kWindow->window()->stackingOrder()) {
                windows.insert(it, bbdxWindow.get());
                break;
            }
        }
    }

    return windows;
}

void BBDX::WindowManager::reconfigure() {
    auto config = BBDX::BlurConfig::self();

    if (!config) {
        qCWarning(WINDOW_MANAGER) << BBDX::LOG_PREFIX
                                  << "WindowManager::reconfigure() called before BlurConfig::read()";
        return;
    }

    QList<QString> windowClassesFixed{};
    QList<QRegularExpression> windowClassesRegex{};

    for (const auto &line : config->windowClasses().split("\n", Qt::SkipEmptyParts)) {
        if (line.length() >= 2 && line.startsWith(QChar('/')) && line.endsWith(QChar('/'))) {
            // regex pattern

            QString pattern = line.sliced(1, line.length() - 2);
            QRegularExpression regex{pattern};

            if (!regex.isValid()) {
                qCWarning(WINDOW_MANAGER) << BBDX::LOG_PREFIX
                                          << "Ignoring malformed regex pattern:" << pattern
                                          << "-" << regex.errorString();
                continue;
            }

            regex.optimize();
            windowClassesRegex.append(std::move(regex));

        } else {
            // fixed string
            // $blank -> empty window class

            if (line == QStringLiteral("$blank")) {
                windowClassesFixed.append(QStringLiteral(""));
            } else {
                windowClassesFixed.append(line);
            }
        }
    }

    m_windowClassesFixed = std::move(windowClassesFixed);
    m_windowClassesRegex = std::move(windowClassesRegex);

    if (config->blurMatching()) {
        m_windowClassMatchMode = WindowClassMatchMode::Whitelist;
    } else {
        m_windowClassMatchMode = WindowClassMatchMode::Blacklist;
    }

    m_blurDecorations = config->blurDecorations();
    m_blurDocks = config->blurDocks();
    m_blurMenus = config->blurMenus();

    m_userBorderRadius = config->cornerRadius();
    m_alphaMask = config->alphaMask();
    m_alphaMaskThreshold = std::clamp<qreal>(config->alphaMaskThreshold(), 0.01, 1.0);
    m_alphaMaskMode = std::clamp(config->alphaMaskMode(), 0, 1);
    m_alphaMaskFloor = std::clamp<qreal>(config->alphaMaskFloor(), 0.0, 0.5);

    m_brightness = config->brightness() / 100.0;
    m_saturation = config->saturation() / 100.0;
    m_contrast = config->contrast() / 100.0;

    m_squircleExponent = std::clamp<qreal>(config->squircleExponent(), 2.0, 8.0);
    m_plateMaskInset = std::clamp<qreal>(config->plateMaskInset(), 0.0, 8.0);
    m_pointedTooltipRadius = std::clamp<qreal>(config->pointedTooltipRadius(), 0.5, 64.0);
    m_pointedTooltipSquircleExponent =
        std::clamp<qreal>(config->pointedTooltipSquircleExponent(), 2.0, 8.0);
    m_pointedTooltipArrowHeight = std::clamp<qreal>(config->pointedTooltipArrowHeight(), 0.0, 64.0);
    m_pointedTooltipArrowHalf = std::clamp<qreal>(config->pointedTooltipArrowHalf(), 0.5, 64.0);
    m_pointedTooltipShoulder = std::clamp<qreal>(config->pointedTooltipShoulder(), 0.0, 64.0);
    m_pointedTooltipTipRadius = std::clamp<qreal>(config->pointedTooltipTipRadius(), 0.0, 32.0);
    m_pointedTooltipInset = std::clamp<qreal>(config->pointedTooltipInset(), 0.0, 8.0);
    m_pointedTooltipFeather = std::clamp<qreal>(config->pointedTooltipFeather(), 0.25, 4.0);
    m_menuRadius = std::clamp<qreal>(config->menuRadius(), 0.0, 64.0);
    m_menuSquircle = config->menuSquircle();
    m_previewPlateBlurRadius.reset();

    // Parse window-specific overrides.
    // One row per line, tab separated:
    //   <class>\t<cornerRadius>\t<brightness>\t<saturation>\t<contrast>\t<shape>
    // An empty field means "inherit" (use the global value).
    QList<WindowOverride> windowOverrides{};
    for (const auto &line : config->windowOverrides().split(QChar('\n'), Qt::SkipEmptyParts)) {
        const auto fields = line.split(QChar('\t'));
        if (fields.isEmpty()) {
            continue;
        }

        WindowOverride o{};

        const QString classField = fields.value(0);
        if (classField.length() >= 2 && classField.startsWith(QChar('/')) && classField.endsWith(QChar('/'))) {
            const QString pattern = classField.sliced(1, classField.length() - 2);
            QRegularExpression regex{pattern};
            if (!regex.isValid()) {
                qCWarning(WINDOW_MANAGER) << BBDX::LOG_PREFIX
                                          << "Ignoring override with malformed regex pattern:" << pattern
                                          << "-" << regex.errorString();
                continue;
            }
            regex.optimize();
            o.isRegex = true;
            o.regex = std::move(regex);
        } else if (classField == QStringLiteral("$blank")) {
            o.isBlank = true;
        } else {
            o.className = classField;
        }

        // helper: parse optional double field, empty -> nullopt
        const auto parseOpt = [&fields](int idx) -> std::optional<qreal> {
            const QString field = fields.value(idx).trimmed();
            if (field.isEmpty()) {
                return std::nullopt;
            }
            bool ok = false;
            const qreal value = field.toDouble(&ok);
            return ok ? std::optional<qreal>(value) : std::nullopt;
        };

        o.cornerRadius = parseOpt(1);
        if (const auto b = parseOpt(2)) {
            o.brightness = *b / 100.0;
        }
        if (const auto s = parseOpt(3)) {
            o.saturation = *s / 100.0;
        }
        if (const auto c = parseOpt(4)) {
            o.contrast = *c / 100.0;
        }
        const QString maskKind = fields.value(5).trimmed();
        o.flatBottom = maskKind.compare(QStringLiteral("squircle-flat"), Qt::CaseInsensitive) == 0;
        o.squircle = o.flatBottom
            || maskKind.compare(QStringLiteral("squircle"), Qt::CaseInsensitive) == 0;
        o.pointed = maskKind.compare(QStringLiteral("pointed"), Qt::CaseInsensitive) == 0;
        o.alphaMask = maskKind.compare(QStringLiteral("alpha"), Qt::CaseInsensitive) == 0;
        o.rawRegion = maskKind.compare(QStringLiteral("region"), Qt::CaseInsensitive) == 0;

        windowOverrides.append(std::move(o));
    }
    m_windowOverrides = std::move(windowOverrides);

    // Published shapes sit on top of the (possibly changed) config table.
    for (auto it = m_surfaceShapes.constBegin(); it != m_surfaceShapes.constEnd(); ++it) {
        rebuildSurfaceOverride(it.key());
    }

    for (const auto &[_, window] : m_windows) {
        window->reconfigure();
    }
}

const BBDX::WindowOverride *BBDX::WindowManager::configOverrideForClass(const QString &windowClass) const {
    for (const auto &o : m_windowOverrides) {
        if (o.isBlank) {
            if (windowClass.isEmpty()) {
                return &o;
            }
        } else if (o.isRegex) {
            if (o.regex.match(windowClass).hasMatch()) {
                return &o;
            }
        } else if (o.className == windowClass) {
            return &o;
        }
    }
    return nullptr;
}

void BBDX::WindowManager::rebuildSurfaceOverride(const QString &appId) {
    const auto shapeIt = m_surfaceShapes.constFind(appId);
    if (shapeIt == m_surfaceShapes.constEnd()) {
        m_surfaceOverrides.remove(appId);
        return;
    }
    const SurfaceShape &shape = *shapeIt;

    WindowOverride merged{};
    if (const WindowOverride *base = configOverrideForClass(appId)) {
        merged = *base;
    }
    // The merged entry always matches this exact class, whatever the base
    // matcher was (a regex may have contributed the inherited values).
    merged.className = appId;
    merged.isRegex = false;
    merged.isBlank = false;
    merged.regex = QRegularExpression();

    if (!shape.kind.isEmpty() && shape.kind != QStringLiteral("inherit")) {
        merged.squircle = false;
        merged.flatBottom = false;
        merged.pointed = false;
        merged.alphaMask = false;
        merged.rawRegion = false;
        if (shape.kind == QStringLiteral("squircle")) {
            merged.squircle = true;
        } else if (shape.kind == QStringLiteral("squircle-flat")) {
            merged.squircle = true;
            merged.flatBottom = true;
        } else if (shape.kind == QStringLiteral("pointed")) {
            merged.pointed = true;
        } else if (shape.kind == QStringLiteral("alpha")) {
            merged.alphaMask = true;
        } else if (shape.kind == QStringLiteral("region")) {
            merged.rawRegion = true;
        }
        // "rounded": all flags off, radius (below or inherited) applies.
    }
    if (shape.radius) {
        merged.cornerRadius = shape.radius;
    }
    m_surfaceOverrides.insert(appId, std::move(merged));
}

void BBDX::WindowManager::applySurfaceShape(const QString &appId) {
    for (const auto &[effectWindow, window] : m_windows) {
        if (effectWindow->window()->resourceClass() != appId) {
            continue;
        }
        // A window KWin retains for its close animation has no surface any
        // more; re-regioning it would drop its blur entry mid-animation.
        // Leave remnants with whatever they had. (X11 windows never have a
        // Wayland surface, so the check is Wayland-only.)
#ifndef BBDX_X11
        if (!window->effectwindow()->surface()) {
            continue;
        }
#endif
        // reconfigure() picks up the radius and invalidates the cache; the
        // region has to be rebuilt too because the kind decides which path
        // getFinalBlurRegion() takes.
        window->reconfigure();
        triggerBlurRegionUpdate(window->effectwindow());
        window->effectwindow()->addRepaintFull();
    }
}

const BBDX::SurfaceShape *BBDX::WindowManager::surfaceShapeFor(const KWin::EffectWindow *w) const {
    if (m_surfaceShapes.isEmpty()) {
        return nullptr;
    }
    const auto it = m_surfaceShapes.constFind(w->window()->resourceClass());
    return it == m_surfaceShapes.constEnd() ? nullptr : &*it;
}

bool BBDX::WindowManager::setSurfaceShape(const QString &appId,
                                          const QString &kind,
                                          std::optional<qreal> radius,
                                          std::optional<qreal> exponent,
                                          std::optional<qreal> arrowCenterX,
                                          std::optional<qreal> inset) {
    if (appId.isEmpty()) {
        return false;
    }
    // Shape fields are replaced as a set; arrow geometry published with
    // setSurfaceArrow is kept.
    SurfaceShape &shape = m_surfaceShapes[appId];
    shape.kind = kind;
    shape.radius = radius;
    shape.exponent = exponent;
    shape.arrowCenterX = arrowCenterX;
    shape.inset = inset;
    rebuildSurfaceOverride(appId);
    applySurfaceShape(appId);
    qCInfo(WINDOW_MANAGER) << BBDX::LOG_PREFIX << "Surface shape set:" << appId << kind
                           << "radius" << (radius ? QString::number(*radius) : QStringLiteral("inherit"))
                           << "exponent" << (exponent ? QString::number(*exponent) : QStringLiteral("inherit"))
                           << "arrowX" << (arrowCenterX ? QString::number(*arrowCenterX) : QStringLiteral("inherit"))
                           << "inset" << (inset ? QString::number(*inset) : QStringLiteral("inherit"));
    return true;
}

bool BBDX::WindowManager::clearSurfaceShape(const QString &appId) {
    if (m_surfaceShapes.remove(appId) == 0) {
        return false;
    }
    m_surfaceOverrides.remove(appId);
    applySurfaceShape(appId);
    qCInfo(WINDOW_MANAGER) << BBDX::LOG_PREFIX << "Surface shape cleared:" << appId;
    return true;
}

bool BBDX::WindowManager::setSurfaceArrow(const QString &appId,
                                          std::optional<int> edge,
                                          std::optional<qreal> height,
                                          std::optional<qreal> halfWidth,
                                          std::optional<qreal> shoulder,
                                          std::optional<qreal> tipRadius,
                                          std::optional<qreal> feather) {
    if (appId.isEmpty()) {
        return false;
    }
    // Arrow fields are replaced as a set; the shape fields are kept. A class
    // without a published shape gets one that inherits everything else.
    SurfaceShape &shape = m_surfaceShapes[appId];
    shape.arrowEdge = edge;
    shape.arrowHeight = height;
    shape.arrowHalf = halfWidth;
    shape.arrowShoulder = shoulder;
    shape.arrowTipRadius = tipRadius;
    shape.arrowFeather = feather;
    rebuildSurfaceOverride(appId);
    // Geometry only: no region or radius change, a repaint is enough.
    for (const auto &[effectWindow, window] : m_windows) {
        if (effectWindow->window()->resourceClass() != appId) {
            continue;
        }
#ifndef BBDX_X11
        if (!window->effectwindow()->surface()) {
            continue;
        }
#endif
        {
            invalidateBlurCache(window->effectwindow(),
                                static_cast<uint>(BlurCacheInvalidationFlag::REGION),
                                "Surface arrow changed");
            window->effectwindow()->addRepaintFull();
        }
    }
    static const char *const edgeNames[] = {"bottom", "top", "left", "right"};
    qCInfo(WINDOW_MANAGER) << BBDX::LOG_PREFIX << "Surface arrow set:" << appId
                           << "edge" << (edge ? edgeNames[std::clamp(*edge, 0, 3)] : "inherit")
                           << "height" << (height ? QString::number(*height) : QStringLiteral("inherit"))
                           << "half" << (halfWidth ? QString::number(*halfWidth) : QStringLiteral("inherit"))
                           << "shoulder" << (shoulder ? QString::number(*shoulder) : QStringLiteral("inherit"))
                           << "tip" << (tipRadius ? QString::number(*tipRadius) : QStringLiteral("inherit"))
                           << "feather" << (feather ? QString::number(*feather) : QStringLiteral("inherit"));
    return true;
}

QStringList BBDX::WindowManager::surfaceShapes() const {
    QStringList lines;
    const auto fmt = [](const std::optional<qreal> &v) {
        return v ? QString::number(*v) : QStringLiteral("inherit");
    };
    static const char *const edgeNames[] = {"bottom", "top", "left", "right"};
    for (auto it = m_surfaceShapes.constBegin(); it != m_surfaceShapes.constEnd(); ++it) {
        const SurfaceShape &s = *it;
        lines.append(QStringLiteral("%1 %2 %3 %4 %5 %6 | %7 %8 %9 %10 %11 %12")
                         .arg(it.key(),
                              s.kind.isEmpty() ? QStringLiteral("inherit") : s.kind,
                              fmt(s.radius), fmt(s.exponent), fmt(s.arrowCenterX), fmt(s.inset))
                         .arg(s.arrowEdge ? QString::fromLatin1(edgeNames[std::clamp(*s.arrowEdge, 0, 3)]) : QStringLiteral("inherit"),
                              fmt(s.arrowHeight), fmt(s.arrowHalf), fmt(s.arrowShoulder),
                              fmt(s.arrowTipRadius), fmt(s.arrowFeather)));
    }
    lines.sort();
    return lines;
}

bool BBDX::WindowManager::hasSurfaceShape(const KWin::EffectWindow *w) const {
    return surfaceShapeFor(w) != nullptr;
}

int BBDX::WindowManager::pointedArrowEdgeFor(const KWin::EffectWindow *w) const {
    if (const SurfaceShape *shape = surfaceShapeFor(w); shape && shape->arrowEdge) {
        return std::clamp(*shape->arrowEdge, 0, 3);
    }
    return static_cast<int>(ArrowEdge::Bottom);
}

qreal BBDX::WindowManager::pointedArrowHeightFor(const KWin::EffectWindow *w) const {
    if (const SurfaceShape *shape = surfaceShapeFor(w); shape && shape->arrowHeight) {
        return *shape->arrowHeight;
    }
    return m_pointedTooltipArrowHeight;
}

qreal BBDX::WindowManager::pointedArrowHalfFor(const KWin::EffectWindow *w) const {
    if (const SurfaceShape *shape = surfaceShapeFor(w); shape && shape->arrowHalf) {
        return *shape->arrowHalf;
    }
    return m_pointedTooltipArrowHalf;
}

qreal BBDX::WindowManager::pointedArrowShoulderFor(const KWin::EffectWindow *w) const {
    if (const SurfaceShape *shape = surfaceShapeFor(w); shape && shape->arrowShoulder) {
        return *shape->arrowShoulder;
    }
    return m_pointedTooltipShoulder;
}

qreal BBDX::WindowManager::pointedArrowTipRadiusFor(const KWin::EffectWindow *w) const {
    if (const SurfaceShape *shape = surfaceShapeFor(w); shape && shape->arrowTipRadius) {
        return *shape->arrowTipRadius;
    }
    return m_pointedTooltipTipRadius;
}

qreal BBDX::WindowManager::pointedArrowFeatherFor(const KWin::EffectWindow *w) const {
    if (const SurfaceShape *shape = surfaceShapeFor(w); shape && shape->arrowFeather) {
        return *shape->arrowFeather;
    }
    return m_pointedTooltipFeather;
}

qreal BBDX::WindowManager::squircleExponentFor(const KWin::EffectWindow *w) const {
    if (const SurfaceShape *shape = surfaceShapeFor(w); shape && shape->exponent) {
        return *shape->exponent;
    }
    return m_squircleExponent;
}

qreal BBDX::WindowManager::shapeInsetFor(const KWin::EffectWindow *w) const {
    if (const SurfaceShape *shape = surfaceShapeFor(w); shape && shape->inset) {
        return *shape->inset;
    }
    return usesDockPlateMask(w) ? m_plateMaskInset : 0.0;
}

bool BBDX::WindowManager::previewDockSurfaces(qreal plateBlurRadius,
                                               qreal plateMaskInset,
                                               qreal squircleExponent,
                                               qreal tooltipRadius,
                                               qreal tooltipSquircleExponent,
                                               qreal tooltipArrowHeight,
                                               qreal tooltipArrowHalf,
                                               qreal tooltipShoulder,
                                               qreal tooltipTipRadius,
                                               qreal tooltipInset,
                                               qreal tooltipFeather) {
    const qreal values[] = {
        plateBlurRadius,
        plateMaskInset,
        squircleExponent,
        tooltipRadius,
        tooltipSquircleExponent,
        tooltipArrowHeight,
        tooltipArrowHalf,
        tooltipShoulder,
        tooltipTipRadius,
        tooltipInset,
        tooltipFeather,
    };
    if (std::ranges::any_of(values, [](qreal value) { return !std::isfinite(value); })) {
        return false;
    }

    m_previewPlateBlurRadius = std::clamp<qreal>(plateBlurRadius, 1.0, 64.0);
    m_plateMaskInset = std::clamp<qreal>(plateMaskInset, 0.0, 8.0);
    m_squircleExponent = std::clamp<qreal>(squircleExponent, 2.0, 8.0);
    m_pointedTooltipRadius = std::clamp<qreal>(tooltipRadius, 0.5, 32.0);
    m_pointedTooltipSquircleExponent =
        std::clamp<qreal>(tooltipSquircleExponent, 2.0, 8.0);
    m_pointedTooltipArrowHeight = std::clamp<qreal>(tooltipArrowHeight, 0.0, 32.0);
    m_pointedTooltipArrowHalf = std::clamp<qreal>(tooltipArrowHalf, 0.5, 32.0);
    m_pointedTooltipShoulder = std::clamp<qreal>(tooltipShoulder, 0.0, 32.0);
    m_pointedTooltipTipRadius = std::clamp<qreal>(tooltipTipRadius, 0.0, 32.0);
    m_pointedTooltipInset = std::clamp<qreal>(tooltipInset, 0.0, 8.0);
    m_pointedTooltipFeather = std::clamp<qreal>(tooltipFeather, 0.25, 4.0);
    repaintAllBlurredWindows();
    return true;
}

bool BBDX::WindowManager::overrideMatchesWindow(const WindowOverride &o, const KWin::EffectWindow *w) const {
    const QString windowClass = w->window()->resourceClass();
    const QString windowName = w->window()->resourceName();

    if (o.isBlank) {
        return windowClass.isEmpty() || windowName.isEmpty();
    }

    if (o.isRegex) {
        return o.regex.match(windowClass).hasMatch() || o.regex.match(windowName).hasMatch();
    }

    return o.className == windowClass || o.className == windowName;
}

const BBDX::WindowOverride *BBDX::WindowManager::overrideFor(const KWin::EffectWindow *w) const {
    if (!m_surfaceOverrides.isEmpty()) {
        const auto it = m_surfaceOverrides.constFind(w->window()->resourceClass());
        if (it != m_surfaceOverrides.constEnd()) {
            return &*it;
        }
    }
    for (const auto &o : m_windowOverrides) {
        if (overrideMatchesWindow(o, w)) {
            return &o;
        }
    }
    return nullptr;
}

std::optional<QMatrix4x4> BBDX::WindowManager::getColorMatrixOverride(const KWin::EffectWindow *w) const {
    const WindowOverride *o = overrideFor(w);
    if (!o || (!o->brightness && !o->saturation && !o->contrast)) {
        return std::nullopt;
    }

    return BBDX::colorTransformMatrix(o->saturation.value_or(m_saturation),
                                      o->contrast.value_or(m_contrast),
                                      o->brightness.value_or(m_brightness));
}

bool BBDX::WindowManager::usesSquircleMask(const KWin::EffectWindow *w) const {
    const WindowOverride *o = overrideFor(w);
    return o && o->squircle;
}

bool BBDX::WindowManager::usesAlphaMask(const KWin::EffectWindow *w) const {
    if (!m_alphaMask) {
        return false;
    }
    const WindowOverride *o = overrideFor(w);
    if (o && (o->squircle || o->pointed || o->rawRegion)) {
        return false;
    }
    if (usesPointedTooltipMask(w) || usesMenuMask(w)) {
        return false;
    }
    if (o && o->alphaMask) {
        return true;
    }
    // Default scope: undecorated surfaces that asked for blur themselves.
    // Force-blurred windows keep the geometric path unless they opt in, so
    // an intentionally translucent app never loses blur to its own alpha.
    return !w->hasDecoration() && blurRequestedByClient(w);
}

bool BBDX::WindowManager::blurRequestedByClient(const KWin::EffectWindow *w) const {
    const Window *window = findWindow(w);
    return window && window->blurRequestedByClient();
}

bool BBDX::WindowManager::usesFlatBottomSquircleMask(const KWin::EffectWindow *w) const {
    const WindowOverride *o = overrideFor(w);
    return o && o->flatBottom;
}

bool BBDX::WindowManager::usesPointedTooltipMask(const KWin::EffectWindow *w) const {
    if (usesPointedMenuMask(w)) {
        return true;
    }
    const WindowOverride *o = overrideFor(w);
    if (o && o->pointed) {
        return true;
    }
    return w->window()->resourceClass() == QStringLiteral("surfboard-tooltip");
}

bool BBDX::WindowManager::usesPointedMenuMask(const KWin::EffectWindow *w) const {
    const Window *window = findWindow(w);
    return window && window->isPointedMenu();
}

qreal BBDX::WindowManager::pointedMaskInset(const KWin::EffectWindow *w) const {
    if (const SurfaceShape *shape = surfaceShapeFor(w); shape && shape->inset) {
        return *shape->inset;
    }
    return usesPointedMenuMask(w) ? 1.0 : m_pointedTooltipInset;
}

qreal BBDX::WindowManager::pointedMaskBodyRadius(const KWin::EffectWindow *w) const {
    if (usesPointedMenuMask(w)) {
        return m_menuRadius;
    }
    const WindowOverride *o = overrideFor(w);
    if (o && o->cornerRadius) {
        return *o->cornerRadius;
    }
    return m_pointedTooltipRadius;
}

qreal BBDX::WindowManager::pointedTooltipSquircleExponent(const KWin::EffectWindow *w) const {
    if (const SurfaceShape *shape = surfaceShapeFor(w); shape && shape->exponent) {
        return *shape->exponent;
    }
    // Thumbnail popups share the pointed mask but intentionally keep their
    // rounded-rectangle body. Only the dock's name tooltip opts into a
    // squircle, and 2 is the rounded-body sentinel consumed by the shader.
    return w->window()->resourceClass() == QStringLiteral("surfboard-tooltip")
        ? m_pointedTooltipSquircleExponent : 2.0;
}

std::optional<qreal> BBDX::WindowManager::pointedArrowCenterX(const KWin::EffectWindow *w) const {
    // A published position wins over the one mined from the blur region.
    if (const SurfaceShape *shape = surfaceShapeFor(w); shape && shape->arrowCenterX) {
        return shape->arrowCenterX;
    }
    const auto window = findWindow(w);
    if (!window)
        return std::nullopt;
    return window->pointedArrowCenterX();
}

bool BBDX::WindowManager::usesDockPlateMask(const KWin::EffectWindow *w) const {
    return w->window()->resourceClass() == QStringLiteral("surfboard-plate");
}

bool BBDX::WindowManager::usesMenuMask(const KWin::EffectWindow *w) const {
    // Menus can't be told apart by their class -- they carry the class of the
    // application they belong to -- so a window override can't reach them and
    // the window type is what we match on, as with the dock and its tooltips.
    if (m_menuRadius <= 0.0) {
        return false;
    }

    // Window::isMenu() is what the rest of the effect means by a menu, and it
    // has to be reused here: a Wayland menu is an xdg_popup, which carries none
    // of the X11 window types, so isPopupWindow() is the only one of them that
    // answers for it. A combo box popup is a menu for our purpose too, and says
    // so by its own type wherever the types exist at all.
    // The dock's own menu is a layer-shell window that only looks like a menu,
    // so no window type answers for it; it says what it is by its class, as the
    // plate and the tooltip do. Its submenus are real popups and need nothing.
    if (w->window()->resourceClass() == QStringLiteral("surfboard-menu")) {
        return true;
    }

    const BBDX::Window *window = findWindow(w);
    const bool menu = window != nullptr
        ? window->isMenu()
        : (w->isMenu() || w->isDropdownMenu() || w->isPopupMenu() || w->isPopupWindow());
    return menu || w->isComboBox();
}

void BBDX::WindowManager::refreshMaximizedState(BBDX::Window *window) const {
    /**
     * TODO: convert to RegionF
     */

    const KWin::EffectWindow *w = window->effectwindow();

    const KWin::LogicalOutput* screen = w->screen();

    KWin::Region effectiveScreenRegion = KWin::Region(screen->geometry());

    for (const auto &dock : m_docks) {
        if (dock->screen() != screen)  {
            continue;
        }

        // we need to "expand" the docks to their entire screen edge
        // or the boundingRect is wrong
        KWin::Rect dock_rect{dock->frameGeometry().toRect()};

        if (dock_rect.width() > dock_rect.height()) {
            // horizontal
            dock_rect.setX(dock->screen()->geometry().x());
            dock_rect.setWidth(dock->screen()->geometry().width());
        } else {
            // vertical
            dock_rect.setY(dock->screen()->geometry().y());
            dock_rect.setHeight(dock->screen()->geometry().height());
        }

        effectiveScreenRegion -= KWin::Region(dock_rect);
    }

    const KWin::Rect effectiveScreenRect{effectiveScreenRegion.boundingRect()};
    const KWin::Rect windowRect{KWin::Rect(w->frameGeometry().toRect())};

    bool maximizedHorizontal{
        windowRect.left() <= effectiveScreenRect.left()
        && windowRect.right() >= effectiveScreenRect.right()
    };

    bool maximizedVertical{
        windowRect.top() <= effectiveScreenRect.top()
        && windowRect.bottom() >= effectiveScreenRect.bottom()
    };

    if (maximizedHorizontal && maximizedVertical) {
        window->setMaximizedState(Window::MaximizedState::Complete);
    } else if (maximizedHorizontal && !maximizedVertical) {
        window->setMaximizedState(Window::MaximizedState::Horizontal);
    } else if (!maximizedHorizontal && maximizedVertical) {
        window->setMaximizedState(Window::MaximizedState::Vertical);
    } else {
        window->setMaximizedState(Window::MaximizedState::Restored);
    }
}

void BBDX::WindowManager::refreshMaximizedStateAll() const {
    for (const auto &[w, window] : m_windows) {
        refreshMaximizedState(window.get());
    }
}

bool BBDX::WindowManager::matchesWindowClassFixed(const KWin::EffectWindow *w) const {
    if (m_windowClassesFixed.contains(w->window()->resourceClass()))
        return true;

    if (m_windowClassesFixed.contains(w->window()->resourceName()))
        return true;

    return false;
}

bool BBDX::WindowManager::matchesWindowClassRegex(const KWin::EffectWindow *w) const {
    for (const auto &regex : m_windowClassesRegex) {
        if (auto m = regex.match(w->window()->resourceClass()); m.hasMatch())
            return true;

        if (auto m = regex.match(w->window()->resourceName()); m.hasMatch())
            return true;
    }

    return false;
}

bool BBDX::WindowManager::shouldForceBlurWindowClass(const KWin::EffectWindow *w) const {
    if (matchesWindowClassFixed(w) || matchesWindowClassRegex(w)) {
        switch (m_windowClassMatchMode) {
            case WindowManager::WindowClassMatchMode::Whitelist:
                return true;
            case WindowManager::WindowClassMatchMode::Blacklist:
                return false;
            [[unlikely]] default:
                return false;
        }
    } else {
        switch (m_windowClassMatchMode) {
            case WindowManager::WindowClassMatchMode::Whitelist:
                return false;
            case WindowManager::WindowClassMatchMode::Blacklist:
                return true;
            [[unlikely]] default:
                return false;
        }
    }
}

void BBDX::WindowManager::triggerBlurRegionUpdate(KWin::EffectWindow *w) const {
    m_effect->updateBlurRegion(w);
}

void BBDX::WindowManager::invalidateBlurCache(KWin::EffectWindow *w, uint flags, const char *reason) const {
    if (auto it = m_effect->m_windows.find(w); it != m_effect->m_windows.end()) {
        BBDX::BlurEffectData &blurInfo = it->second;
        for (auto &[_, renderInfo] : blurInfo.render) {
            if (renderInfo.cache) {
                renderInfo.cache->invalidate(flags, reason);
            }
        }
    }
}

void BBDX::WindowManager::setWindowIsTransformed(const KWin::EffectWindow *w, bool toggle) const {
    const auto window = findWindow(w);

    if (!window)
        return;

    window->setIsTransformed(toggle);
}

bool BBDX::WindowManager::windowShouldBlurWhileTransformed(const KWin::EffectWindow *w) const {
    const auto window = findWindow(w);

    // assume false for unmanaged windows
    if (!window)
        return false;

    return window->shouldBlurWhileTransformed();
}

bool BBDX::WindowManager::windowIsBlurred(const KWin::EffectWindow *w) const {
    const auto window = findWindow(w);

    // assume false for unmanaged windows
    if (!window)
        return false;

    return window->isBlurred();
}

bool BBDX::WindowManager::windowIsPlasmaSurface(const KWin::EffectWindow *w) const {
    const auto window = findWindow(w);

    if (!window) {
        return false;
    }

    return window->isPlasmaSurface();
}

void BBDX::WindowManager::getFinalBlurRegion(const KWin::EffectWindow *w, std::optional<KWin::RegionF> &content, std::optional<KWin::RegionF> &frame) const {
    const auto window = findWindow(w);
    if (!window)
        return;

    window->getFinalBlurRegion(content, frame);
}

KWin::BorderRadius BBDX::WindowManager::getEffectiveBorderRadius(const KWin::EffectWindow *w) const {
    const auto withOverrideCorners = [this, w](const KWin::BorderRadius &radius) {
        if (usesFlatBottomSquircleMask(w)) {
            // Constructor order is TL, TR, BR, BL. Keeping this conversion at
            // the manager boundary also covers the live plate-radius preview.
            return KWin::BorderRadius(radius.topLeft(), radius.topRight(), 0.0, 0.0);
        }
        return radius;
    };
    if (m_previewPlateBlurRadius
        && w->window()->resourceClass() == QStringLiteral("surfboard-plate")) {
        return withOverrideCorners(KWin::BorderRadius(*m_previewPlateBlurRadius));
    }
    const auto window = findWindow(w);

    // unmanaged windows can never be force blurred
    if (!window)
        return KWin::BorderRadius();

    return withOverrideCorners(window->getEffectiveBorderRadius());
}

qreal BBDX::WindowManager::getEffectiveBlurOpacity(const KWin::EffectWindow *w, KWin::WindowPaintData &data) const {
    const auto window = findWindow(w);

    // for unmanaged windows just use the paint data
    if (!window)
        return data.opacity();

    return window->getEffectiveBlurOpacity(data);
}

bool BBDX::WindowManager::windowIsBlurFullyCovered(KWin::EffectWindow *w) const {
    const auto window = findWindow(w);

    // for unmanaged windows assume false so they
    // don't get throttled accidentally
    if (!window)
        return false;

    return window->isBlurFullyCovered();
}

void BBDX::WindowManager::repaintAllBlurredWindows() const {
    for (const auto &[kWindow, bbdxWindow] : m_windows) {
        if (!bbdxWindow->isBlurred()) {
            continue;
        }

        const_cast<KWin::EffectWindow *>(kWindow)->addRepaintFull();
    }
}

void BBDX::WindowManager::flushWindowCaches(BBDX::Window *window) const {
    auto it = m_effect->m_windows.find(window->effectwindow());
    if (it == m_effect->m_windows.end()) {
        return;
    }

    auto &effectData = it->second;
    for (auto &[view, renderData] : effectData.render) {
        if (auto cacheEntry = renderData.cache.get()) {
            cacheEntry->flush();
        }
    }
}

void BBDX::WindowManager::flushWindowCachesFor(BBDX::Window *window, std::chrono::milliseconds duration) const {
    auto it = m_effect->m_windows.find(window->effectwindow());
    if (it == m_effect->m_windows.end()) {
        return;
    }

    auto &effectData = it->second;
    for (auto &[view, renderData] : effectData.render) {
        if (auto cacheEntry = renderData.cache.get()) {
            cacheEntry->flushFor(duration);
        }
    }
}

void BBDX::WindowManager::flushAllWindowCaches() const {
    for (const auto &[kWindow, bbdxWindow] : m_windows) {
        flushWindowCaches(bbdxWindow.get());
    }
}

void BBDX::WindowManager::flushAllWindowCachesFor(std::chrono::milliseconds duration) const {
    for (const auto &[kWindow, bbdxWindow] : m_windows) {
        flushWindowCachesFor(bbdxWindow.get(), duration);
    }
}
