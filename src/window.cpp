#include "window.hpp"

#include "kwin_compat.hpp"
#include "utils.h"
#include "window_manager.hpp"
#include "blur_cache.hpp"

#include <KDecoration3/Decoration>
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>
#include <effect/globals.h>
#include <scene/borderradius.h>
#include <window.h>
#ifndef BBDX_X11
#include <wayland/surface.h>
#endif

#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 5, 80)
#  include <core/region.h>
#endif

#include <QEasingCurve>
#include <QLoggingCategory>
#include <QTimer>
#include <QVariant>
#include <QtNumeric>
#include <QtPreprocessorSupport>

#include <chrono>
#include <optional>

Q_LOGGING_CATEGORY(BBDX_WINDOW, "kwin_effect_better_blur_dx.window", QtInfoMsg)

BBDX::Window::Window(BBDX::WindowManager *wm, KWin::EffectWindow *w) {
    m_windowManager = wm;
    m_effectwindow = w;
    reconfigure();
    slotWindowFrameGeometryChanged();
    connect(w, &KWin::EffectWindow::minimizedChanged, this, &BBDX::Window::slotMinimizedChanged);
    connect(w, &KWin::EffectWindow::windowFullScreenChanged, this, &BBDX::Window::slotWindowFullScreenChanged);
    connect(w, &KWin::EffectWindow::windowFrameGeometryChanged, this, [this]() {
        slotWindowFrameGeometryChanged();

        const bool sdfOverride = m_windowManager->usesSquircleMask(m_effectwindow)
            || m_windowManager->usesPointedTooltipMask(m_effectwindow);
        if (sdfOverride) {
            // The frame-geometry signal can arrive while KWin is still
            // committing the new contentsRect. An immediate region update then
            // leaves BackgroundEffectItem clipped to the previous tooltip body
            // height, outside of which the arrow receives no blur at all.
            // Resync on the next event-loop turn after both geometries agree.
            QTimer::singleShot(0, this, [this]() {
                if (m_windowManager->usesSquircleMask(m_effectwindow)
                    || m_windowManager->usesPointedTooltipMask(m_effectwindow)) {
                    triggerBlurRegionUpdate();
                    m_effectwindow->addRepaintFull();
                }
            });
        }
    });
    connect(w, &KWin::EffectWindow::windowStartUserMovedResized, this, &BBDX::Window::slotWindowStartUserMovedResized);
    connect(w, &KWin::EffectWindow::windowFinishUserMovedResized, this, &BBDX::Window::slotWindowFinishUserMovedResized);
    connect(w, &KWin::EffectWindow::windowOpacityChanged, this, &BBDX::Window::slotWindowOpacityChanged);
    connect(w->window(), &KWin::Window::windowClassChanged, this, &BBDX::Window::reconfigure);
#ifndef BBDX_X11
    if (auto *surface = w->surface()) {
        connect(surface, &KWin::SurfaceInterface::committed, this, [this]() {
            refreshTooltipBackground();
        });
    }
#endif
}

void BBDX::Window::slotMinimizedChanged() {
    refreshMaximizedState();
    if (m_maximizedState == MaximizedState::Complete
        && m_isMinimized) {
        m_restoresMaximized = true;
    }
}

void BBDX::Window::slotWindowFullScreenChanged() {
    // windowFrameGeometryChanged occurs before this
    // so we need to catch it explicitly to update our tracker
    refreshMaximizedState();
}

void BBDX::Window::slotWindowStartUserMovedResized() {
    if (blurOriginIs(BlurOrigin::ForcedContent)) {
        // Don't allow blurring while transformed during move/resize
        // to avoid dragging an off-looking rectangular blur region
        // behind windows affected by Wobbly Windows.
        m_shouldBlurWhileTransformed = true;
        m_blurWhileTransformedTransitionState = TransformState::Started;
        m_blurWhileTransformedTransitionStart = std::chrono::steady_clock::now();
    } else {
        m_shouldBlurWhileTransformed = false;
        m_blurWhileTransformedTransitionState = TransformState::None;
    }
}

void BBDX::Window::slotWindowFinishUserMovedResized() {
    if (blurOriginIs(BlurOrigin::ForcedContent)) {
        // After move/resize force blurring while transformed.
        // While still suboptimal (the Wobbly Windows effect doesn't end
        // after finishing move/resize) this at least assures blur
        // is always set afterwards.
        m_shouldBlurWhileTransformed = true;
        m_blurWhileTransformedTransitionState = TransformState::Ended;
        m_blurWhileTransformedTransitionStart = std::chrono::steady_clock::now();
    } else {
        m_shouldBlurWhileTransformed = false;
        m_blurWhileTransformedTransitionState = TransformState::None;
    }
}

void BBDX::Window::slotWindowFrameGeometryChanged() {
    updateForceBlurRegion();
    refreshMaximizedState();

    // Just mark cache region dirty to force a full flush here.
    // BlurEffect::blur() may upgrade this to realloc buffers
    // in case their size doesn't match anymore
    m_windowManager->invalidateBlurCache(m_effectwindow,
                                         static_cast<uint>(BlurCacheInvalidationFlag::REGION),
                                         "frameGeometry changed");
    refreshTooltipBackground();

    // Not sure if this is the best place to unset
    // this but seems to work fine for now
    m_restoresMaximized = false;
}

void BBDX::Window::refreshTooltipBackground() {
    if (m_effectwindow->window()->resourceClass() != QStringLiteral("surfboard-tooltip")) {
        return;
    }
    // The helper reuses a mapped surface. A label or position change must
    // resample the whole background, even if the buffer size stays the same
    // or the cache's normal refresh interval has not elapsed. Full scene
    // damage is essential: invalidating only the cached result can otherwise
    // re-blur a mixture of new pixels and the previous location's blit.
    m_windowManager->invalidateBlurCache(m_effectwindow,
                                        static_cast<uint>(BlurCacheInvalidationFlag::REGION),
                                        "Tooltip surface committed");
    m_windowManager->flushWindowCaches(this);
    m_effectwindow->addRepaintFull();
}

void BBDX::Window::slotWindowOpacityChanged(KWin::EffectWindow *w, qreal oldOpacity, qreal newOpacity) {
    Q_UNUSED(oldOpacity);
    if (w->window()->isActive() && !m_originalOpacityActive.has_value()) {
        m_originalOpacityActive = newOpacity;
    } else if (!w->window()->isActive() && !m_originalOpacityInactive.has_value()) {
        m_originalOpacityInactive = newOpacity;
    }
}

void BBDX::Window::setIsTransformed(bool toggle) {
    if (m_isTransformed == toggle)
        return;

    m_isTransformed = toggle;

    // Unset m_shouldBlurWhileTransformed flag
    // on the first non-transformed paint.
    // Needed e.g. for Magic Lamp effect to not draw blur.
    if (!toggle) {
        m_shouldBlurWhileTransformed = false;
    }
}

void BBDX::Window::setMaximizedState(MaximizedState state) {
    const bool fullscreen = effectwindow()->isFullScreen();
    const bool minimized = effectwindow()->isMinimized();

    if (m_maximizedState == state
        && m_isFullScreen == fullscreen
        && m_isMinimized == minimized)
        return;

    m_maximizedState = state;
    m_isFullScreen = fullscreen;
    m_isMinimized = minimized;

    qCDebug(BBDX_WINDOW) << BBDX::LOG_PREFIX << "MaximizedState changed:" << *this;
}

void BBDX::Window::setIsBlurFullyCovered(bool toggle) {
    if (m_isBlurFullyCovered == toggle) {
        return;
    }

    m_isBlurFullyCovered = toggle;

    // blur was frozen, we need a full repaint to
    // refresh everything
    if (!toggle) {
        effectwindow()->addRepaintFull();
    }

    qCDebug(BBDX_WINDOW) << BBDX::LOG_PREFIX << "BlurFullyCovered changed:" << *this;
}

bool BBDX::Window::shouldBlurWhileTransformed() const {
    // While minimized there's no reason to blur
    if (m_isMinimized) {
        return false;
    }

    // While completely maximized we can always blur.
    // Avoids weirdness while (de-)maximzing by dragging the
    // titlebar with Wobbly Windows enabled.
    if (m_maximizedState == MaximizedState::Complete && !m_restoresMaximized) {
        return true;
    }

    return m_shouldBlurWhileTransformed;
}

void BBDX::Window::refreshMaximizedState() {
    m_windowManager->refreshMaximizedState(this);
}

void BBDX::Window::updateForceBlurRegion() {
    if (!m_shouldForceBlur) {
        if (m_forceBlurContent.has_value() || m_forceBlurFrame.has_value()) {
            m_forceBlurContent.reset();
            m_forceBlurFrame.reset();
            triggerBlurRegionUpdate();
        }
        return;
    }

    KWin::RegionF content{};
    KWin::RegionF frame{};

    // We'll push both content and frame through the "raw" frame later
    // for full control over the blurred region.
    // So unlike BlurEffect these are frameGeometry local not contentsRect local

    // frameGeometry rounded in to avoid leaking "blur stripes" around window
    content |= BBDX::rectRoundedIn(
        m_effectwindow->frameGeometry()
            .translated(-m_effectwindow->frameGeometry().topLeft())
    );

    // content at this point has content + decorations
    // so to just get the decorations clip the contentsRect
    if (m_effectwindow->decoration()) {
        frame = content - BBDX::rectRoundedOut(m_effectwindow->contentsRect());
    }

    // now clip the frame on decorated windows to get the actual content
    for (const auto &rect : frame.rects()) {
        content -= rect;
    }

    // unchanged
    if (content == m_forceBlurContent && frame == m_forceBlurFrame) {
        return;
    }

    qCDebug(BBDX_WINDOW) << BBDX::LOG_PREFIX
                         << "Force blur regions changed:" << m_effectwindow->windowClass() << "\n"
                         << "content" << content << "\n"
                         << "frame" << frame;

    m_forceBlurContent = std::move(content);
    m_forceBlurFrame = std::move(frame);
    triggerBlurRegionUpdate();
}

void BBDX::Window::triggerBlurRegionUpdate() const {
    m_windowManager->triggerBlurRegionUpdate(m_effectwindow);
}

bool BBDX::Window::opacityChangedFromOriginal() {
    if (effectwindow()->window()->isActive()) {
        return !qFuzzyCompare(m_originalOpacityActive.value_or(1.0), effectwindow()->opacity());
    } else {
        return !qFuzzyCompare(m_originalOpacityInactive.value_or(1.0), effectwindow()->opacity());
    }
}

void BBDX::Window::blurOriginSet(BlurOrigin origin) {
    m_blurOriginMask |= static_cast<unsigned int>(origin);
}

void BBDX::Window::blurOriginUnset(BlurOrigin origin) {
    m_blurOriginMask &= ~static_cast<unsigned int>(origin);
}

bool BBDX::Window::blurRequestedByClient() const {
    return blurOriginIs(BlurOrigin::RequestedContent)
        || blurOriginIs(BlurOrigin::RequestedFrame);
}

bool BBDX::Window::blurOriginIs(BlurOrigin origin) const {
    return !!(m_blurOriginMask & static_cast<unsigned int>(origin));
}

QString BBDX::Window::blurOriginToString() const {
    QString s{};
    if (blurOriginIs(BlurOrigin::RequestedContent))
        s.append("RequestedContent,");
    if (blurOriginIs(BlurOrigin::RequestedFrame))
        s.append("RequestedFrame,");
    if (blurOriginIs(BlurOrigin::ForcedContent))
        s.append("ForcedContent,");
    if (blurOriginIs(BlurOrigin::ForcedFrame))
        s.append("ForcedFrame,");

    if (s.isEmpty()) {
        s = "None";
    } else {
        s.removeLast();
    }

    return s;
}

QString BBDX::Window::maximizedStateToString() const {
    QString s;

    switch (m_maximizedState) {
        case MaximizedState::Restored:
            s = QStringLiteral("Restored");
            break;
        case MaximizedState::Vertical:
            s = QStringLiteral("Vertical");
            break;
        case MaximizedState::Horizontal:
            s = QStringLiteral("Horizontal");
            break;
        case MaximizedState::Complete:
            s = QStringLiteral("Complete");
            break;
        default:
            s = QStringLiteral("Unknown");
            break;
    }

    if (m_isFullScreen)
        s.append("+FullScreen");

    if (m_isMinimized)
        s.append("+Minimized");

    return s;
}

bool BBDX::Window::neverForceBlur() const {
    if (effectwindow()->isDesktop())
        return true;

    if (!m_blurMenus && isMenu())
        return true;

    if (!m_blurDocks && effectwindow()->isDock())
        return true;

    const QString windowClass = effectwindow()->window()->resourceClass();
    const KWin::Layer layer = effectwindow()->window()->layer();

    if (windowClass == QStringLiteral("xwaylandvideobridge"))
        return true;

    if ((windowClass == "spectacle" || windowClass == "org.kde.spectacle")
        && (layer == KWin::Layer::OverlayLayer || layer == KWin::Layer::ActiveLayer))
        return true;

    // Some application-managed popups (notably browser extension panels on Wayland)
    // show up as undecorated Unknown windows with no caption. They don't expose a
    // useful opaque region to the compositor, so force-blurring them produces a
    // rectangular halo around the popup body.
    if (effectwindow()->windowType() == KWin::WindowType::Unknown
        && !effectwindow()->hasDecoration()
        && effectwindow()->caption().isEmpty()) {
        return true;
    }

    // don't touch KWin internal windows
    // this includes the snapping assistant zones
    // and they don't handle blur well at all
    if (effectwindow()->internalWindow()) return true;

    return false;
}

bool BBDX::Window::shouldForceBlur() const {
    if (neverForceBlur())
        return false;

    // The dock and Settings share one process name, which is intentionally
    // blacklisted to protect the dock's giant transparent input surface. The
    // Settings toplevel has a dedicated app-id, so put only that real window
    // on the regular compositor-owned force-blur geometry path.
    if (effectwindow()->window()->resourceClass()
        == QStringLiteral("surfboard-settings-surface")) {
        return true;
    }

    return m_windowManager->shouldForceBlurWindowClass(effectwindow());
}

void BBDX::Window::reconfigure() {
    // must be set before shouldForceBlur()
    m_blurMenus = m_windowManager->blurMenus();
    m_blurDocks = m_windowManager->blurDocks();

    if (shouldForceBlur()) {
        m_shouldForceBlur = true;
    } else {
        m_shouldForceBlur = false;
    }

    m_userBorderRadius = m_windowManager->userBorderRadius();

    if (const auto *o = m_windowManager->overrideFor(m_effectwindow); o && o->cornerRadius) {
        m_hasRadiusOverride = true;
        m_overrideRadius = *o->cornerRadius;
    } else {
        m_hasRadiusOverride = false;
    }

    slotWindowOpacityChanged(effectwindow(), 0.0, effectwindow()->opacity());
    m_windowManager->invalidateBlurCache(m_effectwindow,
                                         static_cast<uint>(BlurCacheInvalidationFlag::REGION),
                                         "Reconfigured window");
    updateForceBlurRegion();
}

void BBDX::Window::getFinalBlurRegion(std::optional<KWin::RegionF> &content, std::optional<KWin::RegionF> &frame) {
    unsigned int oldBlurOriginMask = m_blurOriginMask;

    // always clip blur regions into the frameGeometry
    // to avoid leaving stripes outside
    // (usually bottom and/or right side of the window)
    // The force-blur regiongs are pre-clipped
    const auto clipRect = BBDX::rectRoundedIn(
        m_effectwindow->frameGeometry()
        .translated(-m_effectwindow->frameGeometry().topLeft())
    );

    // If we already have a blur region at this point
    // the window requested it.
    // This tracker allows us to later decide if we want
    // to trust the window or use user parameters
    // e.g. for corner radius.
    if (content.has_value()) {
        // content is local to contentsRect which is local to frameGeometry
        const auto contentTopLeft = m_effectwindow->contentsRect().topLeft();
        content = BBDX::regionTranslatedF(
            BBDX::regionTranslatedF(*content, contentTopLeft) & clipRect,
            -contentTopLeft
        );

        blurOriginSet(BlurOrigin::RequestedContent);
        blurOriginUnset(BlurOrigin::ForcedContent);
    } else {
        blurOriginUnset(BlurOrigin::RequestedContent);
    }
    if (frame.has_value()) {
        frame = *frame & clipRect;

        blurOriginSet(BlurOrigin::RequestedFrame);
        blurOriginUnset(BlurOrigin::ForcedFrame);
    } else {
        blurOriginUnset(BlurOrigin::RequestedFrame);
    }

    // A shape override deliberately transfers boundary ownership from the
    // client's integer Wayland region to Better Blur's antialiased SDF. Use
    // the full content bounds as the coarse region; the final mask pass clips
    // it to the configured squircle. Restrict this to undecorated surfaces so
    // decorations and content offsets never get silently folded together.
    // The menu helper opts in per surface, not by a class-wide override (its
    // submenus share the same class). A full-width body followed by one narrow
    // bottom rectangle is the explicit metadata shape, including a 1px band
    // when the configured arrow height is zero. Native Kvantum regions have
    // shadow insets or rounded scanlines and cannot match this contract.
    m_pointedMenu = false;
    if (content.has_value() && !effectwindow()->isPopupWindow()
        && effectwindow()->window()->resourceClass() == QStringLiteral("surfboard-menu")
        && !effectwindow()->hasDecoration()) {
        const auto rects = content->rects();
        if (rects.size() == 2) {
            const auto &body = rects[0].top() < rects[1].top() ? rects[0] : rects[1];
            const auto &tip = rects[0].top() < rects[1].top() ? rects[1] : rects[0];
            const auto bounds = effectwindow()->contentsRect();
            m_pointedMenu = body.left() == 0 && body.top() == 0
                && body.width() == bounds.width() && body.height() > 0
                && tip.top() == body.bottom() && tip.bottom() == bounds.height()
                && tip.height() >= 1 && tip.height() <= 32
                && tip.left() > 0 && tip.right() < body.right()
                && tip.width() > 0 && tip.width() < body.width();
        }
    }
    if ((m_pointedMenu || m_windowManager->usesSquircleMask(effectwindow())
         || m_windowManager->usesPointedTooltipMask(effectwindow()))
        && !effectwindow()->hasDecoration()) {
        // The pointed mask centres its arrow on the body. A client that wants
        // it elsewhere (the thumbnail popup: body clamped at a screen edge,
        // arrow still on the icon) publishes a two-rect blur region — the body
        // plus a narrow rect in the bottom arrow band. The region itself is
        // discarded just below (force-blur owns the area), so mine the arrow's
        // center x out of it first. `content` is contents-local here; store
        // frame-local to match the mask rect the draw pass works in.
        if (m_pointedMenu || m_windowManager->usesPointedTooltipMask(effectwindow())) {
            std::optional<qreal> arrowCenterX;
            if (content.has_value()) {
                const auto rects = content->rects();
                if (rects.size() > 1) {
                    const auto bounds = content->boundingRect();
                    const KWin::RectF *arrowRect = nullptr;
                    for (const auto &rect : rects) {
                        if (!arrowRect || rect.top() > arrowRect->top())
                            arrowRect = &rect;
                    }
                    if (arrowRect && arrowRect->width() < bounds.width()) {
                        arrowCenterX = arrowRect->horizontalCenter()
                            + m_effectwindow->contentsRect().left();
                    }
                }
            }
            m_pointedArrowCenterX = arrowCenterX;
        }
        content = KWin::RegionF{};
        frame.reset();
        // Menu close animations keep the client-requested blur with the
        // fading buffer, just like their native counterparts.
        if (m_pointedMenu) {
            blurOriginSet(BlurOrigin::RequestedContent);
            blurOriginUnset(BlurOrigin::ForcedContent);
        } else {
            blurOriginSet(BlurOrigin::ForcedContent);
            blurOriginUnset(BlurOrigin::RequestedContent);
        }
        blurOriginUnset(BlurOrigin::RequestedFrame);
        blurOriginUnset(BlurOrigin::ForcedFrame);
        if (m_blurOriginMask != oldBlurOriginMask) {
            qCDebug(BBDX_WINDOW) << BBDX::LOG_PREFIX << "Blur origin changed:" << *this;
        }
        return;
    }

    // Respect exact blur regions supplied by trusted client-shaped surfaces.
    // We used to respect all requested blur regions but some are just
    // completely bogus (e.g. some KDE apps using Breeze like setting just part of the window blurred
    // - even if by default that blur isn't even visible *sigh*).
    // (This check implies BlurOrigin::RequestedContent + some extra heuristics
    // and thus must occur after {content,frame}.has_value() checks)
    if (hasTrustedBlurRegion()) {
        if (m_blurOriginMask != oldBlurOriginMask) {
            qCDebug(BBDX_WINDOW) << BBDX::LOG_PREFIX << "Blur origin changed:" << *this;
        }
        return;
    }

    // the effective blurred region
    KWin::RegionF effectiveRegion{};

    // outside of the Plasma surface special case
    // content should always be overriden as it can have
    // "garbage data" (e.g. Dolphin's theming sets blur region for like 4 random Rects
    // scattered around the window)
    if (m_forceBlurContent.has_value()) {
        for (const auto &rect : m_forceBlurContent->rects()) {
            effectiveRegion |= rect;
        }
        blurOriginSet(BlurOrigin::ForcedContent);
        blurOriginUnset(BlurOrigin::RequestedContent);
    } else {
        blurOriginUnset(BlurOrigin::ForcedContent);
    }

    if (frame) {
        // preserve frame if it already exists as it's likely more accurate
        // (e.g. already has its corners rounded or an outline that shouldn't be blured)
        for (const auto &rect : frame->rects()) {
            effectiveRegion |= rect;
        }
    } else if (m_forceBlurFrame && m_windowManager->blurDecorations()) {
        // else fall back to user requested forced frame ("Blur Decorations As Well" option)
        for (const auto &rect : m_forceBlurFrame->rects()) {
            effectiveRegion |= rect;
        }
        blurOriginSet(BlurOrigin::ForcedFrame);
        blurOriginUnset(BlurOrigin::RequestedFrame);
    } else {
        // no frame / CSD
        blurOriginUnset(BlurOrigin::ForcedFrame);
    }

    // push the effective forced regions through frame only
    // which gives us full control over geometry and implicitly
    // preserves window provided content
    // (which in force-blurred cases is likely just covered)
    frame = effectiveRegion;

    if (m_blurOriginMask != oldBlurOriginMask) {
        qCDebug(BBDX_WINDOW) << BBDX::LOG_PREFIX << "Blur origin changed:" << *this;
    }

    // this isn't fatal but we only expect one to be set, so log if that's not the case
    if (blurOriginIs(BlurOrigin::RequestedContent) && blurOriginIs(BlurOrigin::ForcedContent)) {
        qCWarning(BBDX_WINDOW) << BBDX::LOG_PREFIX
                               << "BlurOrigin::RequestedContent and BlurOrigin::ForcedContent"
                               << "both set on window"
                               << effectwindow()->window()->resourceClass();
    }

    if (blurOriginIs(BlurOrigin::RequestedFrame) && blurOriginIs(BlurOrigin::ForcedFrame)) {
        qCWarning(BBDX_WINDOW) << BBDX::LOG_PREFIX
                               << "BlurOrigin::RequestedFrame and BlurOrigin::ForcedFrame"
                               << "both set on window"
                               << effectwindow()->window()->resourceClass();
    }
}

KWin::BorderRadius BBDX::Window::getEffectiveBorderRadius() {
    // An explicit per-window radius override is authoritative: it bypasses the
    // plasma-surface / window-provided-radius handling below (the user asked
    // for this exact radius). Fullscreen/maximized windows stay square.
    if (m_hasRadiusOverride) {
        if (m_isFullScreen || m_maximizedState == MaximizedState::Complete) {
            return KWin::BorderRadius();
        }
        return KWin::BorderRadius(m_overrideRadius);
    }

    // always respect window provided radius
    // (although this seems to only ever be set by Breeze's
    // "Round bottom corners of windows with no borders"
    // currently)
    const KWin::BorderRadius windowCornerRadius = m_effectwindow->window()->borderRadius();

    // Client-shaped surfaces already describe their blur boundary exactly.
    // Applying the global circular radius would destroy non-circular masks
    // such as a dock's squircle. An explicit per-window override above stays
    // authoritative when reshaping is deliberately requested.
    if (hasTrustedBlurRegion()) {
        return windowCornerRadius;
    }

    // Breeze has a "Round bottom corners of windows with no borders"
    // option which sets radius for the *bottom corners only* (because
    // it assumes no blur behind the top corners).
    // We provide a "Blur decorations as well option" which breaks said
    // assumption. Assuming all corners are rounded equally we'll just
    // copy the bottom radius to their respective top corners.
    if (!windowCornerRadius.isNull()) {
        // No adjustments relevant for CSD windows
        if (!effectwindow()->hasDecoration())
            return windowCornerRadius;

        // If not force-blurring decorations we don't need
        // any adjustments
        if (!(blurOriginIs(BlurOrigin::ForcedFrame)))
            return windowCornerRadius;

        // if top radius is set explicitly by decoration keep it
        const qreal topLeft = windowCornerRadius.topLeft() > 0.0 ?
            windowCornerRadius.topLeft() : windowCornerRadius.bottomLeft();
        const qreal topRight = windowCornerRadius.topRight() > 0.0 ?
            windowCornerRadius.topRight() : windowCornerRadius.bottomRight();

        return KWin::BorderRadius(topLeft,
                                  topRight,
                                  windowCornerRadius.bottomLeft(),
                                  windowCornerRadius.bottomRight());
    }

    // Fullscreen/completely maximized windows don't need radius.
    if (m_isFullScreen || m_maximizedState == MaximizedState::Complete) {
        return KWin::BorderRadius();
    }

    // fallback to configured radius
    if (m_userBorderRadius > 0.0) {
        // If decoration is blurred or window has CSD we need to round all corners
        // because blur might bleed from all corners.
        // Else we assume the decoration isn't blurred at all i.e. the top corners
        // are already correct and we only need to round the bottom ones.
        if (blurOriginIs(BlurOrigin::RequestedFrame) || blurOriginIs(BlurOrigin::ForcedFrame)) {
            return KWin::BorderRadius(m_userBorderRadius);
        } else if (!effectwindow()->hasDecoration()) {
            return KWin::BorderRadius(m_userBorderRadius);
        } else {
            return KWin::BorderRadius(0.0, 0.0, m_userBorderRadius, m_userBorderRadius);
        }
    } else {
        return KWin::BorderRadius();
    }
}

qreal BBDX::Window::getEffectiveBlurOpacity(KWin::WindowPaintData &data) {
#ifndef BBDX_X11
    if (effectwindow()->window()->resourceClass() == QStringLiteral("surfboard-tooltip")) {
        if (const auto *surface = effectwindow()->surface()) {
            // wp_alpha_modifier is committed with the tooltip buffer. GTK
            // pixel alpha (or an independent DBus animation) cannot keep the
            // blur and content in the same frame. KWin applies this multiplier
            // to the SurfaceItem, not WindowPaintData, so include it here once.
            return data.opacity() * surface->alphaMultiplier();
        }
    }
#endif
    // Plasma surfaces expect their opacity to affect
    // the blur e.g. to hide the blurred surface alongside
    // themselves by adjusting their opacity at run time.
    //
    // The vast majority of blurred surfaces don't want/need this
    if (isPlasmaSurface() && opacityChangedFromOriginal()) [[unlikely]] {
        return effectwindow()->opacity() * data.opacity();
    }

    // ease into and out of the phase without blur
    // if moving/resizing with Wobbly Windows
    // TODO: maybe move partly this to a separate function
    //       - feels kind of out-of place here
    if (m_isTransformed && m_blurWhileTransformedTransitionState != TransformState::None) [[unlikely]] {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_blurWhileTransformedTransitionStart).count();

        // animation takes 250ms max for now
        const qreal progress = elapsed / 250.0;

        // we need to queue a full repaint here to
        // avoid flickering due to blur-region-clipping
        effectwindow()->addRepaintFull();

        switch (m_blurWhileTransformedTransitionState) {
            case TransformState::Started:
                // De-maximizing a window by dragging the titlebar
                // while wobbly windows is active behaves weird:
                // - drag (before de-maximize) already marked "transformed" after mouse moved a bit
                // - on actual de-maximize blur briefly reappears (not marked "transformed"?)
                // - then it's marked "transformed" again
                // So while we're maximized stay fully blurred.
                if (m_maximizedState == MaximizedState::Complete) {
                    m_blurWhileTransformedTransitionStart = std::chrono::steady_clock::now();
                    return data.opacity();
                }

                if (progress >= 1.0) {
                    // fade out done
                    // We can stop blurring now until
                    // slotWindowFinishUserMovedResized gets called.
                    m_shouldBlurWhileTransformed = false;
                    return 0.0;
                } else {
                    // fade out in progress
                    const QEasingCurve curve{QEasingCurve::OutCubic};
                    return data.opacity() * (1.0 - curve.valueForProgress(progress));
                }
            case TransformState::Ended:
                // Needed to avoid a flicker in case de-maximizing by dragging
                // the titlebar is cancelled (drag stopped before de-maximize)
                // which still triggers slotWindowFinishUserMovedResized
                // TODO: this should only be the case if MaximizedState stays Complete
                //       throughout the entire UserMovedResized i.e. we need to track that
                if (m_maximizedState == MaximizedState::Complete) {
                    m_blurWhileTransformedTransitionState = TransformState::None;
                    return data.opacity();
                }

                if (progress >= 1.0) {
                    // fade in done
                    m_blurWhileTransformedTransitionState = TransformState::None;
                    return data.opacity();
                } else {
                    // fade in in progress
                    const QEasingCurve curve{QEasingCurve::InCubic};
                    return data.opacity() * curve.valueForProgress(progress);
                }
            default:
                break;
        }
    }

    return data.opacity();
}

bool BBDX::Window::isPlasmaSurface() const {
    // Plasma surfaces must specify their own blur
    if (!(blurOriginIs(BlurOrigin::RequestedContent)))
        return false;

    // Plasma surfaces (afaik) never have decorations
    if (effectwindow()->hasDecoration())
        return false;

    // These window classes are known to be Plasma surfaces
    const auto resourceClass = effectwindow()->window()->resourceClass();
    if (resourceClass == QStringLiteral("org.kde.plasmashell")
        || resourceClass == QStringLiteral("plasmashell")
        || resourceClass == QStringLiteral("org.kde.krunner")
        || resourceClass == QStringLiteral("krunner"))
        return true;

    // If a window is special it's very likely a Plasma surface
    if (effectwindow()->isSpecialWindow())
        return true;

    // (right-click) menus (while technically not Plasma surfaces) behave pretty much the same
    // i.e. they also set their own apropriate roundness and shouldn't need the
    // user override
    if (isMenu())
        return true;

    // Some popups like the Meta+Ctrl+ESC killer have an empty window class,
    // but are still considered KWin::WindowType::Normal
    // Let's just assume an empty class means they're likely Plasma surfaces as well
    if (resourceClass == QStringLiteral(""))
        return true;

    // Likely not a Plasma surface then
    return false;
}

bool BBDX::Window::hasTrustedBlurRegion() const {
    if (isPlasmaSurface()) {
        return true;
    }

    // Toolkits can expose layer-shell surfaces as ordinary EffectWindows, so
    // isDock()/isSpecialWindow() are not sufficient classifiers. Undecorated,
    // captionless surfaces that explicitly request blur own compositor-style
    // geometry such as panel and dock squircles. Force-blurred surfaces still
    // continue through the normal Better Blur geometry path.
    return blurOriginIs(BlurOrigin::RequestedContent)
        && !effectwindow()->hasDecoration()
        && effectwindow()->caption().isEmpty();
}

bool BBDX::Window::isMenu() const {
    return (effectwindow()->isMenu()
            || effectwindow()->isDropdownMenu()
            || effectwindow()->isPopupMenu()
            || effectwindow()->isPopupWindow());
}

bool BBDX::Window::isBlurred() const {
    return m_blurOriginMask > 0;
}

namespace BBDX {
QDebug operator<<(QDebug &debug, const BBDX::Window &window) {
    debug << "\n";
    debug << "windowClass:" << window.effectwindow()->windowClass() << "\n";
    debug << "windowType:" << window.effectwindow()->windowType() << "\n";
    debug << "isPlasmaSurface:" << window.isPlasmaSurface() << "\n";
    debug << "hasTrustedBlurRegion:" << window.hasTrustedBlurRegion() << "\n";
    debug << "isMenu:" << window.isMenu() << "\n";
    debug << "blurOrigin:" << window.blurOriginToString() << "\n";
    debug << "maximizedState:" << window.maximizedStateToString() << "\n";
    debug << "isBlurFullyCovered:" << window.isBlurFullyCovered() << "\n";
    return debug;
}
} // namespace BBDX
