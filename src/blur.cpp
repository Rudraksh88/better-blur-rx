/*
    SPDX-FileCopyrightText: 2010 Fredrik Höglund <fredrik@kde.org>
    SPDX-FileCopyrightText: 2011 Philipp Knechtges <philipp-dev@knechtges.com>
    SPDX-FileCopyrightText: 2018 Alex Nemeth <alex.nemeth329@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "blur.h"
// KConfigSkeleton
#include "blurconfig.h"

#include "blur_cache.hpp"
#include "kwin_compat.hpp"
#include "refraction_pass.hpp"
#include "settings.hpp"
#include "utils.h"
#include "window_manager.hpp"

#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 5, 80)
#  include <core/rect.h>
#  include <core/region.h>
#endif

#include <core/output.h>
#include <core/pixelgrid.h>
#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <effect/effect.h>
#include <effect/effecthandler.h>
#include <opengl/glplatform.h>
#include <opengl/glshadermanager.h>
#include <opengl/glutils.h>
#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 6, 4)
#include <scene/backgroundeffectitem.h>
#endif
#include <scene/decorationitem.h>
#include <scene/scene.h>
#include <scene/surfaceitem.h>
#include <scene/windowitem.h>
#include "kwin_texture_mirror.hpp"
#include <scene/windowitem.h>
#include <utils/xcbutils.h>
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
#include <wayland/blur.h>
#include <wayland/contrast.h>
#endif
#include <wayland/display.h>
#include <wayland/surface.h>
#include <x11window.h>
#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 6, 91)
// not shipped in KWin 6.6.90
// https://invent.kde.org/plasma/kwin/-/merge_requests/9214
#include <wayland/backgroundeffect_v1.h>
#include <wayland_server.h>
#endif

#include <QDBusConnection>
#include <QGuiApplication>
#include <QMatrix4x4>
#include <QPointer>
#include <QScreen>
#include <QTime>
#include <QTimer>
#include <QWindow>
#include <cmath> // for ceil()
#include <cstdlib>

#include <KConfigGroup>
#include <KSharedConfig>

#include <KDecoration3/Decoration>

#include <algorithm>
#include <opengl/glframebuffer.h>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

Q_LOGGING_CATEGORY(KWIN_BLUR, "kwin_effect_better_blur_dx", QtInfoMsg)

static void ensureResources()
{
    // Must initialize resources manually because the effect is a static lib.
    Q_INIT_RESOURCE(blur);
}

namespace BBDX {
using namespace KWin;

static const QByteArray s_blurAtomName = QByteArrayLiteral("_KDE_NET_WM_BLUR_BEHIND_REGION");

#if !defined(BBDX_X11) && KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
BlurManagerInterface *BlurEffect::s_blurManager = nullptr;
QTimer *BlurEffect::s_blurManagerRemoveTimer = nullptr;

ContrastManagerInterface *BlurEffect::s_contrastManager = nullptr;
QTimer *BlurEffect::s_contrastManagerRemoveTimer = nullptr;
#endif

// colorTransformMatrix now lives in utils.h (BBDX::colorTransformMatrix) so it
// can be shared with the per-window color override path in window_manager.cpp.

BlurEffect::BlurEffect()
{
    BlurConfig::instance(effects->config());
    ensureResources();

    m_onscreenPass.shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture,
                                                                              BBDX::shaderFilePath(":/effects/better_blur_dx/shaders/vertex.vert"),
                                                                              BBDX::shaderFilePath(":/effects/better_blur_dx/shaders/onscreen.frag"));
    if (!m_onscreenPass.shader) {
        qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "Failed to load onscreen pass shader";
        return;
    } else {
        m_onscreenPass.mvpMatrixLocation = m_onscreenPass.shader->uniformLocation("modelViewProjectionMatrix");
        m_onscreenPass.colorMatrixLocation = m_onscreenPass.shader->uniformLocation("colorMatrix");
        m_onscreenPass.offsetLocation = m_onscreenPass.shader->uniformLocation("offset");
        m_onscreenPass.halfpixelLocation = m_onscreenPass.shader->uniformLocation("halfpixel");
    }

#if BBDX_NOT_NEEDED
    m_roundedOnscreenPass.shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture,
                                                                                     QStringLiteral(":/effects/better_blur_dx/shaders/onscreen_rounded.vert"),
                                                                                     QStringLiteral(":/effects/better_blur_dx/shaders/onscreen_rounded.frag"));
    if (!m_roundedOnscreenPass.shader) {
        qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "Failed to load onscreen pass shader";
        return;
    } else {
        m_roundedOnscreenPass.mvpMatrixLocation = m_roundedOnscreenPass.shader->uniformLocation("modelViewProjectionMatrix");
        m_roundedOnscreenPass.colorMatrixLocation = m_roundedOnscreenPass.shader->uniformLocation("colorMatrix");
        m_roundedOnscreenPass.offsetLocation = m_roundedOnscreenPass.shader->uniformLocation("offset");
        m_roundedOnscreenPass.halfpixelLocation = m_roundedOnscreenPass.shader->uniformLocation("halfpixel");
        m_roundedOnscreenPass.boxLocation = m_roundedOnscreenPass.shader->uniformLocation("box");
        m_roundedOnscreenPass.cornerRadiusLocation = m_roundedOnscreenPass.shader->uniformLocation("cornerRadius");
        m_roundedOnscreenPass.opacityLocation = m_roundedOnscreenPass.shader->uniformLocation("opacity");
    }
#endif

    m_downsamplePass.shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture,
                                                                                BBDX::shaderFilePath(":/effects/better_blur_dx/shaders/vertex.vert"),
                                                                                BBDX::shaderFilePath(":/effects/better_blur_dx/shaders/downsample.frag"));
    if (!m_downsamplePass.shader) {
        qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "Failed to load downsampling pass shader";
        return;
    } else {
        m_downsamplePass.mvpMatrixLocation = m_downsamplePass.shader->uniformLocation("modelViewProjectionMatrix");
        m_downsamplePass.offsetLocation = m_downsamplePass.shader->uniformLocation("offset");
        m_downsamplePass.halfpixelLocation = m_downsamplePass.shader->uniformLocation("halfpixel");
    }

    m_upsamplePass.shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture,
                                                                              BBDX::shaderFilePath(":/effects/better_blur_dx/shaders/vertex.vert"),
                                                                              BBDX::shaderFilePath(":/effects/better_blur_dx/shaders/upsample.frag"));
    if (!m_upsamplePass.shader) {
        qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "Failed to load upsampling pass shader";
        return;
    } else {
        m_upsamplePass.mvpMatrixLocation = m_upsamplePass.shader->uniformLocation("modelViewProjectionMatrix");
        m_upsamplePass.offsetLocation = m_upsamplePass.shader->uniformLocation("offset");
        m_upsamplePass.halfpixelLocation = m_upsamplePass.shader->uniformLocation("halfpixel");
    }

    m_noisePass.shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture,
                                                                           BBDX::shaderFilePath(":/effects/better_blur_dx/shaders/vertex.vert"),
                                                                           BBDX::shaderFilePath(":/effects/better_blur_dx/shaders/noise.frag"));
    if (!m_noisePass.shader) {
        qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "Failed to load noise pass shader";
        return;
    } else {
        m_noisePass.mvpMatrixLocation = m_noisePass.shader->uniformLocation("modelViewProjectionMatrix");
        m_noisePass.noiseTextureSizeLocation = m_noisePass.shader->uniformLocation("noiseTextureSize");
    }

    // BBDX: managed extension objects
    m_windowManager = std::make_unique<BBDX::WindowManager>(this);

    m_blurCache = BBDX::BlurCache::create(this);
    if (!m_blurCache) {
        qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "Failed to create BlurCache";
        return;
    }

    m_refractionPass = BBDX::RefractionPass::create();
    if (!m_refractionPass) {
        qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "Failed to create RefractionPass";
        return;
    }

    initBlurStrengthValues();
    reconfigure(ReconfigureAll);

    if (effects->xcbConnection()) {
        net_wm_blur_region = effects->announceSupportProperty(s_blurAtomName, this);
    }

#if !defined(BBDX_X11) && KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    if (!s_blurManagerRemoveTimer) {
        s_blurManagerRemoveTimer = new QTimer(QCoreApplication::instance());
        s_blurManagerRemoveTimer->setSingleShot(true);
        s_blurManagerRemoveTimer->callOnTimeout([]() {
            s_blurManager->remove();
            s_blurManager = nullptr;
        });
    }
    s_blurManagerRemoveTimer->stop();
    if (!s_blurManager) {
        s_blurManager = new BlurManagerInterface(effects->waylandDisplay(), s_blurManagerRemoveTimer);
    }

    if (!s_contrastManagerRemoveTimer) {
        s_contrastManagerRemoveTimer = new QTimer(QCoreApplication::instance());
        s_contrastManagerRemoveTimer->setSingleShot(true);
        s_contrastManagerRemoveTimer->callOnTimeout([]() {
            s_contrastManager->remove();
            s_contrastManager = nullptr;
        });
    }
    s_contrastManagerRemoveTimer->stop();
    if (!s_contrastManager) {
        s_contrastManager = new ContrastManagerInterface(effects->waylandDisplay(), s_contrastManagerRemoveTimer);
    }
#endif

#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 6, 91)
    waylandServer()->backgroundEffectManager()->addBlurCapability();
#endif

    connect(effects, &EffectsHandler::windowAdded, this, &BlurEffect::slotWindowAdded);
    connect(effects, &EffectsHandler::windowDeleted, this, &BlurEffect::slotWindowDeleted);
#if defined(BBDX_X11)
    connect(effects, &EffectsHandler::screenRemoved, this, &BlurEffect::slotViewRemoved);
#else
    connect(effects, &EffectsHandler::viewRemoved, this, &BlurEffect::slotViewRemoved);
#endif
    connect(effects, &EffectsHandler::propertyNotify, this, &BlurEffect::slotPropertyNotify);
    connect(effects, &EffectsHandler::xcbConnectionChanged, this, [this]() {
        net_wm_blur_region = effects->announceSupportProperty(s_blurAtomName, this);
    });

    // Fetch the blur regions for all windows
    const auto stackingOrder = effects->stackingOrder();
    for (EffectWindow *window : stackingOrder) {
        slotWindowAdded(window);
    }

    m_valid = true;

    const bool previewRegistered = QDBusConnection::sessionBus().registerObject(
        QStringLiteral("/org/kde/KWin/Effects/BetterBlurDX"),
        this,
        QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals);
    if (previewRegistered) {
        // Published surface shapes live in this instance only: tell clients
        // to publish again after an effect reload.
        Q_EMIT surfaceShapesReset();
    } else {
        qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX
                             << "Could not register dock surface preview D-Bus object";
    }
}

BlurEffect::~BlurEffect()
{
    QDBusConnection::sessionBus().unregisterObject(
        QStringLiteral("/org/kde/KWin/Effects/BetterBlurDX"));
#if !defined(BBDX_X11) && KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    // When compositing is restarted, avoid removing the manager immediately.
    if (s_blurManager) {
        s_blurManagerRemoveTimer->start(1000);
    }

    if (s_contrastManager) {
        s_contrastManagerRemoveTimer->start(1000);
    }
#elif KWIN_VERSION >= KWIN_VERSION_CODE(6, 6, 91)
    waylandServer()->backgroundEffectManager()->removeBlurCapability();
#endif
}

bool BlurEffect::setSurfaceShape(const QString &appId,
                                 const QString &kind,
                                 double radius,
                                 double exponent,
                                 double arrowCenterX,
                                 double inset)
{
    if (!m_windowManager || appId.trimmed().isEmpty()) {
        return false;
    }
    static const QStringList kinds{
        QString(), QStringLiteral("inherit"), QStringLiteral("rounded"),
        QStringLiteral("squircle"), QStringLiteral("squircle-flat"),
        QStringLiteral("pointed"), QStringLiteral("alpha"),
        QStringLiteral("alpha-threshold"), QStringLiteral("region"),
    };
    const QString normalizedKind = kind.trimmed().toLower();
    if (!kinds.contains(normalizedKind)) {
        qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "setSurfaceShape: unknown kind" << kind;
        return false;
    }
    // "alpha-threshold" reads the radius argument as the saturation alpha T
    // (0.01..1); negative still inherits (the global AlphaMaskThreshold).
    const bool thresholdKind = normalizedKind == QStringLiteral("alpha-threshold");
    // Negative (or NaN) means inherit; anything else is clamped to the range
    // the shaders were designed for.
    const auto optional = [](double value, double lo, double hi) -> std::optional<qreal> {
        if (!std::isfinite(value) || value < 0.0) {
            return std::nullopt;
        }
        return std::clamp<qreal>(value, lo, hi);
    };
    const std::optional<qreal> exponentValue = (std::isfinite(exponent) && exponent > 0.0)
        ? std::optional<qreal>(std::clamp<qreal>(exponent, 2.0, 8.0))
        : std::nullopt;
    return m_windowManager->setSurfaceShape(appId.trimmed(),
                                            normalizedKind == QStringLiteral("inherit") ? QString() : normalizedKind,
                                            thresholdKind ? optional(radius, 0.01, 1.0)
                                                          : optional(radius, 0.0, 512.0),
                                            exponentValue,
                                            optional(arrowCenterX, 0.0, 65536.0),
                                            optional(inset, 0.0, 64.0));
}

bool BlurEffect::setSurfaceArrow(const QString &appId,
                                 const QString &edge,
                                 double height,
                                 double halfWidth,
                                 double shoulder,
                                 double tipRadius,
                                 double feather)
{
    if (!m_windowManager || appId.trimmed().isEmpty()) {
        return false;
    }
    static const QStringList edges{
        QStringLiteral("bottom"), QStringLiteral("top"),
        QStringLiteral("left"), QStringLiteral("right"),
    };
    const QString normalizedEdge = edge.trimmed().toLower();
    std::optional<int> edgeValue;
    if (!normalizedEdge.isEmpty() && normalizedEdge != QStringLiteral("inherit")) {
        const int index = edges.indexOf(normalizedEdge);
        if (index < 0) {
            qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "setSurfaceArrow: unknown edge" << edge;
            return false;
        }
        edgeValue = index;
    }
    const auto optional = [](double value, double lo, double hi) -> std::optional<qreal> {
        if (!std::isfinite(value) || value < 0.0) {
            return std::nullopt;
        }
        return std::clamp<qreal>(value, lo, hi);
    };
    return m_windowManager->setSurfaceArrow(appId.trimmed(),
                                            edgeValue,
                                            optional(height, 0.0, 64.0),
                                            optional(halfWidth, 0.5, 64.0),
                                            optional(shoulder, 0.0, 64.0),
                                            optional(tipRadius, 0.0, 32.0),
                                            optional(feather, 0.25, 4.0));
}

bool BlurEffect::clearSurfaceShape(const QString &appId)
{
    return m_windowManager && m_windowManager->clearSurfaceShape(appId.trimmed());
}

QStringList BlurEffect::surfaceShapes() const
{
    return m_windowManager ? m_windowManager->surfaceShapes() : QStringList();
}

QStringList BlurEffect::capabilities() const
{
    QStringList caps{
        QStringLiteral("api=1"),
        QStringLiteral("surface-shape"),
        QStringLiteral("surface-arrow"),
    };
#ifndef BBDX_X11
    caps.append(QStringLiteral("remnant-alpha"));
#endif
#if !defined(BBDX_X11) && KWIN_VERSION >= KWIN_VERSION_CODE(6, 6, 90)
    caps.append(QStringLiteral("alpha-mask"));
    // kind=alpha-threshold: per-surface threshold coverage mode.
    caps.append(QStringLiteral("alpha-threshold"));
    // ...whose antialiased rim gets the client's geometric coverage.
    caps.append(QStringLiteral("alpha-threshold-rim"));
    // ...and whose inset clips coverage to where the client's paint starts.
    caps.append(QStringLiteral("alpha-threshold-inset"));
    if (m_windowManager && m_windowManager->alphaMaskEnabled()) {
        caps.append(QStringLiteral("alpha-mask-enabled"));
        caps.append(QStringLiteral("alpha-mask-threshold=%1")
                        .arg(m_windowManager->alphaMaskThreshold(), 0, 'g', 4));
        caps.append(QStringLiteral("alpha-mask-mode=%1")
                        .arg(m_windowManager->alphaMaskMode() == 1
                                 ? QStringLiteral("silhouette") : QStringLiteral("threshold")));
        caps.append(QStringLiteral("alpha-mask-floor=%1")
                        .arg(m_windowManager->alphaMaskFloor(), 0, 'g', 4));
    }
#endif
    return caps;
}

bool BlurEffect::previewDockSurfaces(double plateBlurRadius,
                                     double plateMaskInset,
                                     double squircleExponent,
                                     double tooltipRadius,
                                     double tooltipSquircleExponent,
                                     double tooltipArrowHeight,
                                     double tooltipArrowHalf,
                                     double tooltipShoulder,
                                     double tooltipTipRadius,
                                     double tooltipInset,
                                     double tooltipFeather)
{
    return m_windowManager && m_windowManager->previewDockSurfaces(
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
        tooltipFeather);
}

void BlurEffect::initBlurStrengthValues()
{
    // This function creates an array of blur strength values that are evenly distributed

    // The range of the slider on the blur settings UI
    int numOfBlurSteps = 15;
    int remainingSteps = numOfBlurSteps;

    /*
     * Explanation for these numbers:
     *
     * The texture blur amount depends on the downsampling iterations and the offset value.
     * By changing the offset we can alter the blur amount without relying on further downsampling.
     * But there is a minimum and maximum value of offset per downsample iteration before we
     * get artifacts.
     *
     * The minOffset variable is the minimum offset value for an iteration before we
     * get blocky artifacts because of the downsampling.
     *
     * The maxOffset value is the maximum offset value for an iteration before we
     * get diagonal line artifacts because of the nature of the dual kawase blur algorithm.
     *
     * The expandSize value is the minimum value for an iteration before we reach the end
     * of a texture in the shader and sample outside of the area that was copied into the
     * texture from the screen.
     */

    // {minOffset, maxOffset, expandSize}
    blurOffsets.append({1.0, 2.0, 10}); // Down sample size / 2
    blurOffsets.append({2.0, 3.0, 20}); // Down sample size / 4
    blurOffsets.append({2.0, 5.0, 50}); // Down sample size / 8
    blurOffsets.append({3.0, 8.0, 150}); // Down sample size / 16
    // blurOffsets.append({5.0, 10.0, 400}); // Down sample size / 32
    // blurOffsets.append({7.0, ?.0});       // Down sample size / 64

    float offsetSum = 0;

    for (int i = 0; i < blurOffsets.size(); i++) {
        offsetSum += blurOffsets[i].maxOffset - blurOffsets[i].minOffset;
    }

    for (int i = 0; i < blurOffsets.size(); i++) {
        int iterationNumber = std::ceil((blurOffsets[i].maxOffset - blurOffsets[i].minOffset) / offsetSum * numOfBlurSteps);
        remainingSteps -= iterationNumber;

        if (remainingSteps < 0) {
            iterationNumber += remainingSteps;
        }

        float offsetDifference = blurOffsets[i].maxOffset - blurOffsets[i].minOffset;

        for (int j = 1; j <= iterationNumber; j++) {
            // {iteration, offset}
            blurStrengthValues.append({i + 1, blurOffsets[i].minOffset + (offsetDifference / iterationNumber) * j});
        }
    }
}

void BlurEffect::reconfigure(ReconfigureFlags flags)
{
    Q_UNUSED(flags);
    BlurConfig::self()->read();
    m_refractionPass->reconfigure();
    m_windowManager->reconfigure();
    m_blurCache->reconfigure();
    m_forceContrastParams = BlurConfig::forceContrastParams();

    int blurStrength = BlurConfig::blurStrength() - 1;
    m_iterationCount = blurStrengthValues[blurStrength].iteration;
    m_offset = blurStrengthValues[blurStrength].offset;
    m_expandSize = blurOffsets[m_iterationCount - 1].expandSize;
    m_noiseStrength = BlurConfig::noiseStrength();
    m_colorMatrix = colorTransformMatrix(BlurConfig::saturation() / 100.0,
                                         BlurConfig::contrast() / 100.0,
                                         BlurConfig::brightness() / 100.0);
#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 6, 4)
    for (auto &[window, data] : m_windows) {
        data.blurItem->setPixelsToExpandRepaintsBelowOpaqueRegions(m_expandSize);
    }
#endif

    for (EffectWindow *w : effects->stackingOrder()) {
        updateBlurRegion(w);
    }

    // Update all windows for the blur to take effect
    effects->addRepaintFull();
}

void BlurEffect::updateBlurRegion(EffectWindow *w)
{
    // WindowManager's constructor adds every existing window, and a
    // force-blurred one re-enters here (Window ctor -> updateForceBlurRegion
    // -> triggerBlurRegionUpdate) before m_windowManager has been assigned.
    // The constructor re-adds all windows afterwards, so skipping is safe.
    if (!m_windowManager) {
        return;
    }
    const RegionF previousBlurRegion = blurRegion(w);

    std::optional<RegionF> content;
    std::optional<RegionF> frame;
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    std::optional<qreal> saturation;
    std::optional<qreal> contrast;
#endif

    if (net_wm_blur_region != XCB_ATOM_NONE) {
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
        const QByteArray value = w->readProperty(net_wm_blur_region, XCB_ATOM_CARDINAL, 32);
        Region region;
        if (value.size() > 0 && !(value.size() % (4 * sizeof(uint32_t)))) {
            const uint32_t *cardinals = reinterpret_cast<const uint32_t *>(value.constData());
            for (unsigned int i = 0; i < value.size() / sizeof(uint32_t);) {
                int x = cardinals[i++];
                int y = cardinals[i++];
                int w = cardinals[i++];
                int h = cardinals[i++];
                region += Xcb::fromXNative(Rect(x, y, w, h)).toRect();
            }
        }
        if (!value.isNull()) {
            content = region;
        }
#else
        if (const auto x11Window = qobject_cast<X11Window *>(w->window())) {
            Xcb::Property wmBlurRegionProperty(false, x11Window->window(), net_wm_blur_region, XCB_ATOM_CARDINAL, 0, 32768);
            if (const auto cardinals = wmBlurRegionProperty.array<uint32_t>()) {
                if (cardinals->size() == 0 || cardinals->size() == 1) {
                    // It means blur background behind whole window.
                    content = RegionF();
                } else if (cardinals->size() % 4 == 0) {
                    RegionF region;
                    for (uint i = 0; i < cardinals->size();) {
                        const int x = (*cardinals)[i++];
                        const int y = (*cardinals)[i++];
                        const int w = (*cardinals)[i++];
                        const int h = (*cardinals)[i++];
                        region += Xcb::fromXNative(Rect(x, y, w, h));
                    }
                    content = region;
                }
            }
        }
#endif
    }

#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    if (SurfaceInterface *surface = w->surface()) {
        if (surface->blur()) {
            content = surface->blur()->region();
        }
        if (surface->contrast()) {
            saturation = surface->contrast()->saturation();
            contrast = surface->contrast()->contrast();
        }
    }
#else
    if (SurfaceInterface *surface = w->surface()) {
        if (!surface->blurRegion().isEmpty()) {
            content = surface->blurRegion();
        }
    }
#endif

    if (auto internal = w->internalWindow()) {
        const auto property = internal->property("kwin_blur");
        if (property.isValid()) {
            content = property.value<RegionF>();
        }
    }

    if (w->decorationHasAlpha() && decorationSupportsBlurBehind(w)) {
        frame = decorationBlurRegion(w);
    }

    // BBDX:
    m_windowManager->getFinalBlurRegion(w, content, frame);

    if (content.has_value() || frame.has_value()) {
        BlurEffectData &data = m_windows[w];
        data.content = content;
        data.frame = frame;
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
        if (m_forceContrastParams) {
            data.colorMatrix.reset();
        } else if (saturation || contrast) {
            data.colorMatrix = colorTransformMatrix(saturation.value_or(1.0), contrast.value_or(1.0), 1.0);
        } else {
            data.colorMatrix.reset();
        }
#endif
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 4)
        data.windowEffect = ItemEffect(w->windowItem());
#else
        if (!data.blurItem) {
            data.blurItem = std::make_unique<BackgroundEffectItem>(w->windowItem());
        }
        data.blurItem->setPixelsToExpandRepaintsBelowOpaqueRegions(m_expandSize);
        data.blurItem->setEffectBoundingRect(blurRegion(w).boundingRect());
#endif
    } else {
        if (auto it = m_windows.find(w); it != m_windows.end()) {
            effects->makeOpenGLContextCurrent();
            m_windows.erase(it);
        }
    }

    const RegionF currentBlurRegion = blurRegion(w);
    if (currentBlurRegion != previousBlurRegion) {
        // BackgroundEffectItem only tracks the blur's bounding rectangle. A
        // Plasma SVG mask can change its exact region without changing those
        // bounds, so the surface's small rectangular damage would otherwise
        // be the only part of the new mask that gets composited.
        //
        // Clearing cached coverage also handles a region whose bounds move
        // without changing size: its texture must then be filled from the new
        // background origin instead of being treated as already cached.
        m_windowManager->invalidateBlurCache(
            w,
            static_cast<uint>(BlurCacheInvalidationFlag::REGION),
            "Blur region changed");
        w->addRepaintFull();
    }
}

void BlurEffect::slotWindowAdded(EffectWindow *w)
{
    SurfaceInterface *surf = w->surface();

    if (surf) {
        windowBlurChangedConnections[w] = connect(surf, &SurfaceInterface::blurChanged, this, [this, w]() {
            if (w) {
                updateBlurRegion(w);

                // SurfaceInterface emits blurChanged while applying the
                // surface state, before KWin has handled the completed commit
                // and updated the window geometry. Plasma SVG masks can
                // therefore be clipped to the previous contentsRect here.
                // Resync the BackgroundEffectItem after the commit so its
                // effect bounds and repaint cover the final surface geometry.
                // Forced dock SDF masks don't qualify as Plasma surfaces.
                if (m_windowManager->windowIsPlasmaSurface(w)) {
                    const QPointer<EffectWindow> window(w);
                    const RectF boundsAtBlurSignal = blurRegion(w).boundingRect();
                    const RectF frameAtBlurSignal = w->frameGeometry();

                    QTimer::singleShot(0, this, [this, window, boundsAtBlurSignal, frameAtBlurSignal]() {
                        if (!window || !m_windowManager->windowIsPlasmaSurface(window.data())) {
                            return;
                        }

                        updateBlurRegion(window.data());

                        const RectF committedBounds = blurRegion(window.data()).boundingRect();
                        const RectF committedFrame = window->frameGeometry();
                        if (committedBounds != boundsAtBlurSignal || committedFrame != frameAtBlurSignal) {
                            qCDebug(KWIN_BLUR) << BBDX::LOG_PREFIX
                                                << "Resynced Plasma blur after surface commit:" << window->windowClass() << "\n"
                                                << "Frame:" << frameAtBlurSignal << "->" << committedFrame << "\n"
                                                << "Blur bounds:" << boundsAtBlurSignal << "->" << committedBounds;
                        }

                        window->addRepaintFull();
                    });
                }
            }
        });
#if !defined(BBDX_X11) && KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
        windowContrastChangedConnections[w] = connect(surf, &SurfaceInterface::contrastChanged, this, [this, w]() {
            if (w) {
                updateBlurRegion(w);
            }
        });
#endif
    }
    if (auto internal = w->internalWindow()) {
        internal->installEventFilter(this);
    }

    setupDecorationConnections(w);
    connect(w, &EffectWindow::windowDecorationChanged, this, [this, w]() {
        setupDecorationConnections(w);
        updateBlurRegion(w);
    });

    updateBlurRegion(w);
}

void BlurEffect::slotWindowDeleted(EffectWindow *w)
{
    if (auto it = m_windows.find(w); it != m_windows.end()) {
        effects->makeOpenGLContextCurrent();
        m_windows.erase(it);
    }
    if (auto it = windowBlurChangedConnections.find(w); it != windowBlurChangedConnections.end()) {
        disconnect(*it);
        windowBlurChangedConnections.erase(it);
    }
#if !defined(BBDX_X11) && KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    if (auto it = windowContrastChangedConnections.find(w); it != windowContrastChangedConnections.end()) {
        disconnect(*it);
        windowContrastChangedConnections.erase(it);
    }
#endif
}

void BlurEffect::slotViewRemoved(KWin::RenderView *view)
{
    for (auto &[window, data] : m_windows) {
        if (auto it = data.render.find(view); it != data.render.end()) {
            effects->makeOpenGLContextCurrent();
            data.render.erase(it);
        }
    }

    // BBDX: cleanup wallpaper
    m_blurCache->dropWallpaper(view);
}

void BlurEffect::slotPropertyNotify(EffectWindow *w, long atom)
{
    if (w && atom == net_wm_blur_region && net_wm_blur_region != XCB_ATOM_NONE) {
        updateBlurRegion(w);
    }
}

void BlurEffect::setupDecorationConnections(EffectWindow *w)
{
    if (!w->decoration()) {
        return;
    }

    connect(w->decoration(), &KDecoration3::Decoration::blurRegionChanged, this, [this, w]() {
        updateBlurRegion(w);
    });
}

bool BlurEffect::eventFilter(QObject *watched, QEvent *event)
{
    auto internal = qobject_cast<QWindow *>(watched);
    if (internal && event->type() == QEvent::DynamicPropertyChange) {
        QDynamicPropertyChangeEvent *pe = static_cast<QDynamicPropertyChangeEvent *>(event);
        if (pe->propertyName() == "kwin_blur") {
            if (auto w = effects->findWindow(internal)) {
                updateBlurRegion(w);
            }
        }
    }
    return false;
}

bool BlurEffect::enabledByDefault()
{
#if BBDX_NOT_NEEDED
    const auto context = effects->openglContext();
    if (!context || context->isSoftwareRenderer()) {
        return false;
    }
    GLPlatform *gl = context->glPlatform();

    if (gl->isIntel() && gl->chipClass() < SandyBridge) {
        return false;
    }
    if (gl->isPanfrost() && gl->chipClass() <= MaliT8XX) {
        return false;
    }
    // The blur effect works, but is painfully slow (FPS < 5) on Mali and VideoCore
    if (gl->isLima() || gl->isVideoCore4() || gl->isVideoCore3D()) {
        return false;
    }
    return true;
#endif
    return false;
}

bool BlurEffect::supported()
{
    const auto context = effects->openglContext();
    if (!context) {
        qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "Effect unsupported: No OpenGL context";
        return false;
    }

    if (!effects->isOpenGLCompositing()) {
        qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "Effect unsupported: No OpenGL compositing";
        return false;
    }

    qCInfo(KWIN_BLUR) << BBDX::LOG_PREFIX << "Effect supported";
    return true;
}

bool BlurEffect::decorationSupportsBlurBehind(const EffectWindow *w) const
{
    return w->decoration() && !w->decoration()->blurRegion().isNull();
}

RegionF BlurEffect::decorationBlurRegion(const EffectWindow *w) const
{
    if (!decorationSupportsBlurBehind(w)) {
        return RegionF();
    }

#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    Region decorationRegion = Region(Rect(w->decoration()->rect().toAlignedRect())) - w->contentsRect().toRect();
#else
    RegionF decorationRegion = RegionF(w->decoration()->rect()) - w->contentsRect();
#endif
    //! we return only blurred regions that belong to decoration region
    return decorationRegion.intersected(RegionF(w->decoration()->blurRegion()));
}

RegionF BlurEffect::blurRegion(EffectWindow *w) const
{
    RegionF region;

    // A closed window's Deleted remnant must not keep frosting the backdrop on
    // its own: force-blur ignores client regions, so without this cut the blur
    // outlives the buffer by a frame or two, visible as stuck edge ghosts when
    // the dock's thumbnail popup or Settings window closes.
    //
    // A client that asked for blur itself is a different case. Its remnant is
    // usually being animated out by another effect — slidingpopups keeps
    // Plasma popups and menus alive for the whole exit slide — and starving it
    // there drops the frost one frame after the close while the surface is
    // still visibly on screen. Those keep their blur until the remnant is
    // destroyed, which is what stock blur does.
    if (w->isDeleted() && !m_windowManager->blurRequestedByClient(w)) {
        return region;
    }

    if (auto it = m_windows.find(w); it != m_windows.end()) {
        const std::optional<RegionF> &content = it->second.content;
        const std::optional<RegionF> &frame = it->second.frame;
        if (content.has_value()) {
            if (content->isEmpty()) {
                // An empty region means that the blur effect should be enabled
                // for the whole window.
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
                region = Rect(w->contentsRect().toRect());
            } else {
                region = content->translated(w->contentsRect().topLeft().toPoint()) & w->contentsRect().toRect();
#else
                region = w->contentsRect();
            } else {
                region = content->translated(w->contentsRect().topLeft()) & w->contentsRect();
#endif
            }
            if (frame.has_value()) {
                region += frame.value();
            }
        } else if (frame.has_value()) {
            region = frame.value();
        }
    }

    return region;
}

#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
void BlurEffect::prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime)
#else
void BlurEffect::prePaintScreen(ScreenPrePaintData &data)
#endif
{
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 4)
    m_paintedDeviceArea = Region();
    m_currentDeviceBlur = Region();
#endif // KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 4)
#if defined(BBDX_X11)
    m_currentView = data.screen;
#else
    m_currentView = data.view;
#endif

    m_blurCache->flushAccumulatedDirtyRegions(data);

#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    effects->prePaintScreen(data, presentTime);
#else
    effects->prePaintScreen(data);
#endif
}

#if KWIN_VERSION < KWIN_VERSION_CODE(6, 5, 80)
void BlurEffect::prePaintWindow(EffectWindow *w, WindowPrePaintData &data, std::chrono::milliseconds presentTime)
#elif KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
void BlurEffect::prePaintWindow(RenderView *view, EffectWindow *w, WindowPrePaintData &data, std::chrono::milliseconds presentTime)
#else
void BlurEffect::prePaintWindow(RenderView *view, EffectWindow *w, WindowPrePaintData &data)
#endif
{
    // this effect relies on prePaintWindow being called in the bottom to top order

#if KWIN_VERSION < KWIN_VERSION_CODE(6, 5, 80)
    effects->prePaintWindow(w, data, presentTime);

    const QRegion oldOpaque = data.opaque;
    if (data.opaque.intersects(m_currentDeviceBlur)) {
        // to blur an area partially we have to shrink the opaque area of a window
        QRegion newOpaque;
        for (const QRect &rect : data.opaque) {
            newOpaque += rect.adjusted(m_expandSize, m_expandSize, -m_expandSize, -m_expandSize);
        }
        data.opaque = newOpaque;

        // we don't have to blur a region we don't see
        m_currentDeviceBlur -= newOpaque;
    }

    // if we have to paint a non-opaque part of this window that intersects with the
    // currently blurred region we have to redraw the whole region
    if ((data.paint - oldOpaque).intersects(m_currentDeviceBlur)) {
        data.paint += m_currentDeviceBlur;
    }

    // in case this window has regions to be blurred
    const QRegion blurArea = blurRegion(w).boundingRect().translated(w->pos().toPoint());

    // if this window or a window underneath the blurred area is painted again we have to
    // blur everything
    if (m_paintedDeviceArea.intersects(blurArea) || data.paint.intersects(blurArea)) {
        data.paint += blurArea;
        // we have to check again whether we do not damage a blurred area
        // of a window
        if (blurArea.intersects(m_currentDeviceBlur)) {
            data.paint += m_currentDeviceBlur;
        }
    }

    m_currentDeviceBlur += blurArea;

    m_paintedDeviceArea -= data.opaque;
    m_paintedDeviceArea += data.paint;
#elif KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 4)
    effects->prePaintWindow(view, w, data, presentTime);

    const Region oldOpaque = data.deviceOpaque;
    if (data.deviceOpaque.intersects(m_currentDeviceBlur)) {
        // to blur an area partially we have to shrink the opaque area of a window
        Region newOpaque;
        for (const Rect &rect : data.deviceOpaque.rects()) {
            newOpaque += rect.adjusted(m_expandSize, m_expandSize, -m_expandSize, -m_expandSize);
        }
        data.deviceOpaque = newOpaque;

        // we don't have to blur a region we don't see
        m_currentDeviceBlur -= newOpaque;
    }

    // if we have to paint a non-opaque part of this window that intersects with the
    // currently blurred region we have to redraw the whole region
    if ((data.devicePaint - oldOpaque).intersects(m_currentDeviceBlur)) {
        data.devicePaint += m_currentDeviceBlur;
    }

    // in case this window has regions to be blurred
    const Region blurArea = view->mapToDeviceCoordinatesAligned(QRectF(blurRegion(w).boundingRect()).translated(w->pos()));

    // if this window or a window underneath the blurred area is painted again we have to
    // blur everything
    if (m_paintedDeviceArea.intersects(blurArea) || data.devicePaint.intersects(blurArea)) {
        data.devicePaint += blurArea;
        // we have to check again whether we do not damage a blurred area
        // of a window
        if (blurArea.intersects(m_currentDeviceBlur)) {
            data.devicePaint += m_currentDeviceBlur;
        }
    }

    m_currentDeviceBlur += blurArea;

    m_paintedDeviceArea -= data.deviceOpaque;
    m_paintedDeviceArea += data.devicePaint;
#elif KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    effects->prePaintWindow(view, w, data, presentTime);
#else
    effects->prePaintWindow(view, w, data);
#endif

    // BBDX change:
    // blurred windows should be painted translucent
    // to avoid issues with repainting because KWin would
    // cull some areas leading to incomplete blits.
    //
    // Wallpaper mode does not need this because it
    // doesn't rely on blitting
    if (m_windowManager->windowIsBlurred(w) && m_blurCache->blitMode() != BlitMode::WALLPAPER) {
        data.setTranslucent();
    }
}

bool BlurEffect::shouldBlur(const EffectWindow *w, int mask, const WindowPaintData &data) const
{
    if (effects->activeFullScreenEffect() && !w->data(WindowForceBlurRole).toBool()) {
        return false;
    }

    if (w->isDesktop()) {
        return false;
    }

    bool scaled = !qFuzzyCompare(data.xScale(), 1.0) && !qFuzzyCompare(data.yScale(), 1.0);
    bool translated = data.xTranslation() || data.yTranslation();

    if ((scaled || (translated || (mask & PAINT_WINDOW_TRANSFORMED))) && !w->data(WindowForceBlurRole).toBool()) {
        // BBDX: keep tracking the transformed state (caches rely on it) but
        // always blur during transforms. Upstream gates this on
        // windowShouldBlurWhileTransformed() to suppress blur during minimize
        // and animations like Magic Lamp / Wobbly Windows; we prefer blur to
        // persist through all transforms.
        m_windowManager->setWindowIsTransformed(w, true);
        return true;
    }
    // BBDX:
    m_windowManager->setWindowIsTransformed(w, false);

    return true;
}

#if KWIN_VERSION < KWIN_VERSION_CODE(6, 7, 80)
void BlurEffect::drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const Region &deviceRegion, WindowPaintData &data)
{
    blur(renderTarget, viewport, w, mask, deviceRegion, data);

    // Draw the window over the blurred area
    effects->drawWindow(renderTarget, viewport, w, mask, deviceRegion, data);
}
#else
bool BlurEffect::drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const Region &deviceRegion, WindowPaintData &data)
{
    blur(renderTarget, viewport, w, mask, deviceRegion, data);

    // Draw the window over the blurred area. A false return means the item
    // renderer gave up on the frame (e.g. lost its EGL context); pass it on.
    return effects->drawWindow(renderTarget, viewport, w, mask, deviceRegion, data);
}
#endif

GLTexture *BlurEffect::ensureNoiseTexture()
{
    if (m_noiseStrength == 0) {
        return nullptr;
    }

    const qreal scale = std::max(1.0, QGuiApplication::primaryScreen()->logicalDotsPerInch() / 96.0);
    if (!m_noisePass.noiseTexture || m_noisePass.noiseTextureScale != scale || m_noisePass.noiseTextureStength != m_noiseStrength) {
        // Init randomness based on time
        std::srand((uint)QTime::currentTime().msec());

        QImage noiseImage(QSize(256, 256), QImage::Format_Grayscale8);

        for (int y = 0; y < noiseImage.height(); y++) {
            uint8_t *noiseImageLine = (uint8_t *)noiseImage.scanLine(y);

            for (int x = 0; x < noiseImage.width(); x++) {
                noiseImageLine[x] = std::rand() % m_noiseStrength;
            }
        }

        noiseImage = noiseImage.scaled(noiseImage.size() * scale);

        m_noisePass.noiseTexture = GLTexture::upload(noiseImage);
        if (!m_noisePass.noiseTexture) {
            return nullptr;
        }
        m_noisePass.noiseTexture->setFilter(GL_NEAREST);
        m_noisePass.noiseTexture->setWrapMode(GL_REPEAT);
        m_noisePass.noiseTextureScale = scale;
        m_noisePass.noiseTextureStength = m_noiseStrength;
    }

    return m_noisePass.noiseTexture.get();
}

void BlurEffect::blur(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const Region &deviceRegion, WindowPaintData &data)
{
    auto it = m_windows.find(w);
    if (it == m_windows.end()) {
        return;
    }

    BlurEffectData &blurInfo = it->second;
    BlurRenderData &renderInfo = blurInfo.render[m_currentView];

    if (!shouldBlur(w, mask, data)) {
        return;
    }

    // BBDX: only blur top level for performance reasons
    // TODO: this currently looks really bad e.g. when maximizing
    /*
    if (!m_windowManager->windowHasTopLevelBlur(w)) {
        return;
    }
    */

    // Compute the effective blur shape. Note that if the window is transformed, so will be the blur shape.
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    Region blurShape = blurRegion(w).translated(w->pos().toPoint());
    if (data.xScale() != 1 || data.yScale() != 1) {
        QPoint pt = blurShape.boundingRect().topLeft();
        Region scaledShape;
        for (const Rect &r : blurShape.rects()) {
            const QPointF topLeft(pt.x() + (r.x() - pt.x()) * data.xScale() + data.xTranslation(),
                                  pt.y() + (r.y() - pt.y()) * data.yScale() + data.yTranslation());
            const QPoint bottomRight(std::floor(topLeft.x() + r.width() * data.xScale()) - 1,
                                     std::floor(topLeft.y() + r.height() * data.yScale()) - 1);
            scaledShape += QRect(QPoint(std::floor(topLeft.x()), std::floor(topLeft.y())), bottomRight);
        }
        blurShape = scaledShape;
    } else if (data.xTranslation() || data.yTranslation()) {
        blurShape.translate(std::round(data.xTranslation()), std::round(data.yTranslation()));
    }
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 5, 80)
    const QRect backgroundRect = blurShape.boundingRect();
    const QRect scaledBackgroundRect = snapToPixelGrid(scaledRect(backgroundRect, viewport.scale()));
    const QRect deviceBackgroundRect = scaledBackgroundRect;
#else
    const Rect backgroundRect = blurShape.boundingRect();
    const Rect scaledBackgroundRect = backgroundRect.scaled(viewport.scale()).rounded();
    const Rect deviceBackgroundRect = viewport.mapToDeviceCoordinates(backgroundRect).rounded();
#endif
#else
    RegionF blurShape = blurRegion(w);
    if (data.xScale() != 1 || data.yScale() != 1) {
        blurShape.scale(data.xScale(), data.yScale());
    }
    if (data.xTranslation() || data.yTranslation()) {
        blurShape.translate(data.xTranslation(), data.yTranslation());
    }

    blurShape.translate(w->pos());

    const Rect backgroundRect = blurShape.boundingRect().rounded();
    const Rect scaledBackgroundRect = backgroundRect.scaled(viewport.scale()).rounded();
    const Rect deviceBackgroundRect = viewport.mapToDeviceCoordinates(backgroundRect).rounded();
#endif
    const auto opacity = m_windowManager->getEffectiveBlurOpacity(w, data);

    // Get the effective shape that will be actually blurred. It's possible that all of it will be clipped.
    QList<RectF> effectiveShape;
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 5, 80)
    effectiveShape.reserve(blurShape.rectCount());
    if (deviceRegion != infiniteRegion()) {
        for (const QRect &clipRect : deviceRegion) {
            const QRectF deviceClipRect = snapToPixelGridF(scaledRect(clipRect, viewport.scale()))
                                              .translated(-deviceBackgroundRect.topLeft());
            for (const QRect &shapeRect : blurShape) {
                const QRectF deviceShapeRect = snapToPixelGridF(scaledRect(shapeRect.translated(-backgroundRect.topLeft()), viewport.scale()));
                if (const QRectF intersected = deviceClipRect.intersected(deviceShapeRect); !intersected.isEmpty()) {
                    effectiveShape.append(intersected);
                }
            }
        }
    } else {
        for (const QRect &rect : blurShape) {
            effectiveShape.append(snapToPixelGridF(scaledRect(rect.translated(-backgroundRect.topLeft()), viewport.scale())));
        }
    }
#else
    effectiveShape.reserve(blurShape.rects().size());
    if (deviceRegion != Region::infinite()) {
        for (const Rect &clipRect : deviceRegion.rects()) {
            const RectF deviceClipRect = clipRect.translated(-deviceBackgroundRect.topLeft());
            for (const auto &shapeRect : blurShape.rects()) {
                const RectF deviceShapeRect = shapeRect.translated(-backgroundRect.topLeft()).scaled(viewport.scale()).rounded();
                if (const RectF intersected = deviceClipRect.intersected(deviceShapeRect); !intersected.isEmpty()) {
                    effectiveShape.append(intersected);
                }
            }
        }
    } else {
        for (const auto &rect : blurShape.rects()) {
            effectiveShape.append(rect.translated(-backgroundRect.topLeft()).scaled(viewport.scale()).rounded());
        }
    }
#endif
    if (effectiveShape.isEmpty()) {
        return;
    }

    // Maybe reallocate offscreen render targets. Keep in mind that the first one contains
    // original background behind the window, it's not blurred.
    GLenum textureFormat = GL_RGBA8;
    if (renderTarget.texture()) {
        textureFormat = renderTarget.texture()->internalFormat();
    }

    if (renderInfo.framebuffers.size() != (m_iterationCount + 1) || renderInfo.textures[0]->size() != backgroundRect.size() || renderInfo.textures[0]->internalFormat() != textureFormat) {
        renderInfo.framebuffers.clear();
        renderInfo.textures.clear();
        // BBDX:
        if (renderInfo.cache) {
            renderInfo.cache->invalidate(static_cast<uint>(BlurCacheInvalidationFlag::FULL), "New framebuffers required");
        }

        glClearColor(0, 0, 0, 0);
        for (size_t i = 0; i <= m_iterationCount; ++i) {
            auto texture = GLTexture::allocate(textureFormat, BBDX::getTextureSize(backgroundRect, i));
            if (!texture) {
                qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "Failed to allocate an offscreen texture";
                return;
            }
            texture->setFilter(GL_LINEAR);
            texture->setWrapMode(GL_CLAMP_TO_EDGE);

            auto framebuffer = std::make_unique<GLFramebuffer>(texture.get());
            if (!framebuffer->valid()) {
                qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "Failed to create an offscreen framebuffer";
                return;
            }
            GLFramebuffer::pushFramebuffer(framebuffer.get());
            glClear(GL_COLOR_BUFFER_BIT);
            GLFramebuffer::popFramebuffer();
            renderInfo.textures.push_back(std::move(texture));
            renderInfo.framebuffers.push_back(std::move(framebuffer));
        }
    }

    // Fetch the pixels behind the shape that is going to be blurred.
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 5, 80)
    const QRegion dirtyRegion = deviceRegion & backgroundRect;
#else
    const Region dirtyRegion = viewport.mapFromDeviceCoordinatesContained(deviceRegion) & backgroundRect;
#endif
#if BBDX_NOT_NEEDED
    for (const Rect &dirtyRect : dirtyRegion.rects()) {
        renderInfo.framebuffers[0]->blitFromRenderTarget(renderTarget, viewport, dirtyRect, dirtyRect.translated(-backgroundRect.topLeft()));
    }
#else
    // BBDX: prepare cache, bail if there is no cache entry
    m_blurCache->preparePaintData(&renderTarget,
                                  &viewport,
                                  m_currentView,
                                  w,
                                  &dirtyRegion,
                                  renderInfo.framebuffers[0].get(),
                                  &backgroundRect,
                                  &scaledBackgroundRect,
                                  renderInfo.cache);

    if (!renderInfo.cache.get()) {
        qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "Bailing due to missing cache entry";
        return;
    }
#endif

    // Upload the geometry: the first 6 vertices are used when downsampling and upsampling offscreen,
    // the remaining vertices are used when rendering on the screen.
    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));

    const int vertexCount = effectiveShape.size() * 6;
    if (auto result = vbo->map<GLVertex2D>(6 + m_blurCache->addedVertices() + vertexCount)) {
        auto map = *result;

        size_t vboIndex = 0;

        // The geometry that will be blurred offscreen, in logical pixels.
        {
            const RectF localRect = RectF(0, 0, backgroundRect.width(), backgroundRect.height());

            const float x0 = localRect.left();
            const float y0 = localRect.top();
            const float x1 = localRect.right();
            const float y1 = localRect.bottom();

            const float u0 = x0 / backgroundRect.width();
            const float v0 = 1.0f - y0 / backgroundRect.height();
            const float u1 = x1 / backgroundRect.width();
            const float v1 = 1.0f - y1 / backgroundRect.height();

            // first triangle
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y0),
                .texcoord = QVector2D(u0, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y1),
                .texcoord = QVector2D(u1, v1),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y1),
                .texcoord = QVector2D(u0, v1),
            };

            // second triangle
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y0),
                .texcoord = QVector2D(u0, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y0),
                .texcoord = QVector2D(u1, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y1),
                .texcoord = QVector2D(u1, v1),
            };
        }

        // BBDX:
        m_blurCache->setupVBO(map, vboIndex);

        // The geometry that will be painted on screen, in device pixels.
        for (const RectF &rect : effectiveShape) {
            const float x0 = rect.left();
            const float y0 = rect.top();
            const float x1 = rect.right();
            const float y1 = rect.bottom();

            const float u0 = x0 / scaledBackgroundRect.width();
            const float v0 = 1.0f - y0 / scaledBackgroundRect.height();
            const float u1 = x1 / scaledBackgroundRect.width();
            const float v1 = 1.0f - y1 / scaledBackgroundRect.height();

            // first triangle
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y0),
                .texcoord = QVector2D(u0, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y1),
                .texcoord = QVector2D(u1, v1),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y1),
                .texcoord = QVector2D(u0, v1),
            };

            // second triangle
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x0, y0),
                .texcoord = QVector2D(u0, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y0),
                .texcoord = QVector2D(u1, v0),
            };
            map[vboIndex++] = GLVertex2D{
                .position = QVector2D(x1, y1),
                .texcoord = QVector2D(u1, v1),
            };
        }

        vbo->unmap();
    } else {
        qCWarning(KWIN_BLUR) << BBDX::LOG_PREFIX << "Failed to map vertex buffer";
        return;
    }

    vbo->bindArrays();

    const BorderRadius cornerRadius = m_windowManager->getEffectiveBorderRadius(w);
    const bool pointedTooltipMask = m_windowManager->usesPointedTooltipMask(w);
    // A menu is a trusted, client-shaped surface, so it keeps its own (null)
    // radius above and would be composited unmasked -- with the aliased corners
    // of the region it quantized itself. It's masked here instead.
    const bool menuMask = !pointedTooltipMask && m_windowManager->usesMenuMask(w);
    const bool flatBottomSquircleMask =
        m_windowManager->usesFlatBottomSquircleMask(w);
    const int shapeMask = pointedTooltipMask
        ? 2
        : (menuMask
               ? (m_windowManager->menuSquircle() ? 1 : 0)
               : (!cornerRadius.isNull() && m_windowManager->usesSquircleMask(w)
                      ? (flatBottomSquircleMask ? 3 : 1)
                      : 0));
    // The alpha mask replaces every geometric mask below for eligible windows;
    // usesAlphaMask() already yields to explicit SDF overrides and menus.
    const bool alphaMask = shapeMask == 0 && !pointedTooltipMask && !menuMask
        && m_windowManager->usesAlphaMask(w);
    // Composite through the window's own alpha channel. False when the surface
    // texture isn't usable this frame (no texture yet, YUV, rotated buffer,
    // opaque format, mirror mismatch); the caller then falls back.
    // Diagnostics: the composite path is logged whenever it changes for a
    // window (alpha <-> geometric fallback), rate limited, so a silently
    // degraded or oscillating mask is visible in the journal without
    // per-frame spam.
    const char *alphaFallbackReason = nullptr;
    int alphaCandidateCount = 0;
    QVector4D alphaRootBox;
    const auto alphaFallback = [&](const char *reason) {
        alphaFallbackReason = reason;
        return false;
    };
    const auto noteAlphaPath = [&](bool usedAlpha) {
        const int state = usedAlpha ? 1 : 0;
        if (state == blurInfo.alphaPathState) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - blurInfo.alphaPathLogged < std::chrono::milliseconds(250)) {
            return;
        }
        blurInfo.alphaPathState = state;
        blurInfo.alphaPathLogged = now;
        if (usedAlpha) {
            qCInfo(KWIN_BLUR) << BBDX::LOG_PREFIX << "[alpha-diag]" << w->window()->resourceClass()
                                 << "-> alpha path, surfaces:" << alphaCandidateCount
                                 << "root box:" << alphaRootBox
                                 << "bg:" << scaledBackgroundRect.width() << "x" << scaledBackgroundRect.height();
        } else {
            qCInfo(KWIN_BLUR) << BBDX::LOG_PREFIX << "[alpha-diag]" << w->window()->resourceClass()
                                 << "-> geometric fallback:" << (alphaFallbackReason ? alphaFallbackReason : "unknown");
        }
    };
    // Debug only: one-time readback of the root texture's alpha at five
    // points, to tell a translucent surface from a transparent blur carrier.
    // Enable with QT_LOGGING_RULES="kwin_effect_better_blur_dx.debug=true".
    const auto sampleAlphaOnce = [&](GLTexture *texture) {
        if (blurInfo.alphaSampled || !KWIN_BLUR().isDebugEnabled()) {
            return;
        }
        blurInfo.alphaSampled = true;
        GLFramebuffer fbo(texture);
        if (!fbo.valid()) {
            return;
        }
        const int tw = texture->width();
        const int th = texture->height();
        const QPoint points[5] = {
            {tw / 2, th / 2}, {tw / 4, th / 4}, {3 * tw / 4, th / 4}, {tw / 4, 3 * th / 4}, {3 * tw / 4, 3 * th / 4}};
        QStringList alphas;
        GLFramebuffer::pushFramebuffer(&fbo);
        for (const QPoint &p : points) {
            unsigned char rgba[4] = {0, 0, 0, 0};
            glReadPixels(p.x(), p.y(), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            alphas.append(QString::number(rgba[3] / 255.0, 'f', 2));
        }
        GLFramebuffer::popFramebuffer();
        qCDebug(KWIN_BLUR) << BBDX::LOG_PREFIX << "[alpha-diag]" << w->window()->resourceClass()
                           << "root texture" << tw << "x" << th
                             << "alpha at center/quarters:" << alphas.join(QStringLiteral(" "));
    };
    const auto drawAlphaMasked = [&](float modulation) -> bool {
        WindowItem *windowItem = w->windowItem();
        SurfaceItem *rootItem = windowItem ? windowItem->surfaceItem() : nullptr;
        if (!rootItem) {
            return alphaFallback("no surface item");
        }

        // Root surface plus subsurfaces. GTK4 graphics offload moves a
        // widget's dmabuf into a subsurface and punches a transparent hole in
        // the root, toggling frame by frame; video players park their content
        // in one permanently. Coverage is the union (max alpha) over them.
        QVarLengthArray<SurfaceItem *, 8> items;
        const auto collect = [&items](auto &&self, Item *item) -> void {
            if (auto *surface = qobject_cast<SurfaceItem *>(item)) {
                items.append(surface);
            }
            const QList<Item *> children = item->childItems();
            for (Item *child : children) {
                self(self, child);
            }
        };
        collect(collect, rootItem);

        const RectF backgroundDevice(0.0, 0.0, scaledBackgroundRect.width(), scaledBackgroundRect.height());
        struct Candidate {
            BBDX::AlphaMaskSurface surface;
            qreal area;
            bool root;
        };
        QVarLengthArray<Candidate, 8> candidates;
        std::optional<RectF> rootDeviceSurface;
        const char *rootSkipReason = nullptr;
        for (SurfaceItem *surfaceItem : items) {
            const bool isRoot = surfaceItem == rootItem;
            const auto skip = [&](const char *reason) {
                if (isRoot) {
                    rootSkipReason = reason;
                }
            };
            if (!surfaceItem->hasAlphaChannel()) {
                skip("opaque buffer format");
                continue;
            }
            if (surfaceItem->bufferTransform().kind() != OutputTransform::Normal) {
                skip("transformed buffer");
                continue;
            }
            // KWin attaches a committed buffer to the item's texture lazily,
            // in ItemRendererOpenGL::createRenderNode() -> Item::preprocess(),
            // which runs after effects have drawn. On a frame where the buffer
            // was resized the texture still has the previous size here, so
            // every resize frame would fall back to the geometric mask and the
            // blur would flicker (seen on the dock's magnify animation). Run
            // the same preprocess step now: it is idempotent, KWin's later
            // call finds a matching size and no pending damage.
            const char *textureReason = nullptr;
            if (!BBDX::preprocessSurfaceTexture(surfaceItem, &textureReason)) {
                skip(textureReason);
                continue;
            }
            GLTexture *alphaTexture = BBDX::surfaceGLTexture(surfaceItem, &textureReason);
            if (!alphaTexture) {
                skip(textureReason);
                continue;
            }
            if (alphaTexture->target() != GL_TEXTURE_2D) {
                skip("non-2D texture target");
                continue;
            }
            const QSize bufferSize = surfaceItem->bufferSize();
            const RectF sourceBox = surfaceItem->bufferSourceBox();
            if (bufferSize.isEmpty() || sourceBox.isEmpty()) {
                skip("empty buffer or source box");
                continue;
            }

            // Surface in device pixels local to the blur's background rect,
            // placed exactly where KWin's item renderer draws it: every item
            // from the window item down translates by its own position
            // rounded to device pixels (ItemRendererOpenGL::createRenderNode),
            // the quad's corners round too (RenderGeometry, VertexSnappingMode::
            // Round), and the window transform applies on top of the window
            // item's rounded origin (WindowPaintData::toMatrix). At a
            // fractional scale an odd logical position lands on half a device
            // pixel; left unrounded, the mask would sit half a texel off the
            // drawn surface, the bilinear sample would bleed the edge's alpha
            // one pixel outwards, and threshold coverage (alpha / T) turns
            // such a bleed into a full-strength blur seam beside the edge.
            const qreal scale = viewport.scale();
            const auto roundToDevicePixel = [](const QPointF &p) {
                return QPointF(std::round(p.x()), std::round(p.y()));
            };
            QPointF itemOffset;
            for (Item *item = surfaceItem; item && item != windowItem; item = item->parentItem()) {
                itemOffset += roundToDevicePixel(item->position() * scale);
            }
            const QPointF windowOrigin = roundToDevicePixel(windowItem->position() * scale);
            const RectF deviceSurface = RectF{
                windowOrigin.x() + data.xTranslation() * scale + itemOffset.x() * data.xScale(),
                windowOrigin.y() + data.yTranslation() * scale + itemOffset.y() * data.yScale(),
                std::round(surfaceItem->rect().width() * scale) * data.xScale(),
                std::round(surfaceItem->rect().height() * scale) * data.yScale(),
            }.translated(-scaledBackgroundRect.x(), -scaledBackgroundRect.y());
            const RectF visible = deviceSurface.intersected(backgroundDevice);
            if (visible.isEmpty()) {
                skip("surface outside blur area");
                continue;
            }
            if (isRoot) {
                rootDeviceSurface = deviceSurface;
            }
            candidates.append(Candidate{
                BBDX::AlphaMaskSurface{
                    alphaTexture,
                    alphaTexture->matrix(NormalizedCoordinates),
                    QVector4D(deviceSurface.x(), deviceSurface.y(),
                              deviceSurface.width(), deviceSurface.height()),
                    QVector4D(sourceBox.x() / bufferSize.width(),
                              sourceBox.y() / bufferSize.height(),
                              sourceBox.width() / bufferSize.width(),
                              sourceBox.height() / bufferSize.height()),
                    QVector2D(1.0f / bufferSize.width(), 1.0f / bufferSize.height()),
                },
                visible.width() * visible.height(),
                isRoot,
            });
        }
        if (candidates.isEmpty()) {
            return alphaFallback(rootSkipReason ? rootSkipReason : "no usable surface texture");
        }
        // Root first, then the largest subsurfaces; the shader takes at most
        // kMaxAlphaMaskSurfaces.
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
            if (a.root != b.root) {
                return a.root;
            }
            return a.area > b.area;
        });
        BBDX::AlphaMaskSurfaces surfaces;
        for (const Candidate &candidate : candidates) {
            if (surfaces.size() >= BBDX::kMaxAlphaMaskSurfaces) {
                break;
            }
            surfaces.append(candidate.surface);
        }
        alphaCandidateCount = surfaces.size();
        alphaRootBox = surfaces[0].box;
        sampleAlphaOnce(surfaces[0].texture);
        // Per window: a published "alpha-threshold" shape overrides the
        // global mode and threshold; the uniforms are uploaded per draw.
        BBDX::AlphaMaskParams params{
            m_windowManager->alphaMaskModeFor(w),
            static_cast<float>(m_windowManager->alphaMaskThresholdFor(w)),
            static_cast<float>(m_windowManager->alphaMaskFloor()),
        };
        // Its inset clips coverage to the root surface inset to where the
        // client's paint starts. A side flush with the output edge keeps its
        // full coverage: there the paint runs on to the screen edge (a
        // docked plate's bottom, or a surface sliding off screen).
        if (const qreal clipInset = m_windowManager->alphaClipInsetFor(w); clipInset > 0.0 && rootDeviceSurface) {
            const RectF box = *rootDeviceSurface;
            const RectF screen = RectF(viewport.scaledRenderRect())
                                     .translated(-scaledBackgroundRect.x(), -scaledBackgroundRect.y());
            const qreal dx = clipInset * viewport.scale() * data.xScale();
            const qreal dy = clipInset * viewport.scale() * data.yScale();
            constexpr float open = 1.0e6f;
            params.clipBox = QVector4D(
                box.left() <= screen.left() + 0.5 ? -open : static_cast<float>(box.left() + dx),
                box.top() <= screen.top() + 0.5 ? -open : static_cast<float>(box.top() + dy),
                box.right() >= screen.right() - 0.5 ? open : static_cast<float>(box.right() - dx),
                box.bottom() >= screen.bottom() - 0.5 ? open : static_cast<float>(box.bottom() - dy));
        }
        m_blurCache->drawAlphaMaskedCached(viewport, renderInfo, vbo, vertexCount, modulation, surfaces, params);
        return true;
    };
    const auto drawFinalCached = [&]() {
        const float modulation = opacity * opacity;
        if (alphaMask) {
            const bool usedAlpha = drawAlphaMasked(modulation);
            noteAlphaPath(usedAlpha);
            if (usedAlpha) {
                return;
            }
        }
        if (cornerRadius.isNull() && !pointedTooltipMask && !menuMask) {
            m_blurCache->drawCached(viewport, renderInfo, vbo, vertexCount, modulation);
            return;
        }

        // A menu reserves the room for its shadow inside its own surface, so
        // its frame geometry is larger than the menu one sees. Its blur region
        // is the menu itself, which is what the mask has to be shaped after.
        const RectF maskRect = menuMask ? blurRegion(w).boundingRect()
                                        : RectF{0.0, 0.0,
                                                w->frameGeometry().width(),
                                                w->frameGeometry().height()};
        const RectF transformedRect = RectF{
            w->frameGeometry().x() + maskRect.x() * data.xScale() + data.xTranslation(),
            w->frameGeometry().y() + maskRect.y() * data.yScale() + data.yTranslation(),
            maskRect.width() * data.xScale(),
            maskRect.height() * data.yScale(),
        };
        const RectF nativeBox = transformedRect
                                    .scaled(viewport.scale())
                                    .rounded()
                                    .translated(-scaledBackgroundRect.topLeft());
        // The menu radius isn't rounded to whole device pixels: it's the exact
        // radius the style painted the menu with, and the mask is antialiased
        // anyway, so rounding it would only misplace the corner at fractional
        // scale factors.
        const BorderRadius nativeCornerRadius = pointedTooltipMask
            ? BorderRadius(m_windowManager->pointedMaskBodyRadius(w) * viewport.scale())
            : (menuMask
                   ? BorderRadius(m_windowManager->menuRadius() * viewport.scale())
                   : cornerRadius.scaled(viewport.scale()).rounded());
        const qreal tooltipInsetDevice =
            m_windowManager->pointedMaskInset(w) * viewport.scale();
        const qreal arrowHeightDevice =
            m_windowManager->pointedArrowHeightFor(w) * viewport.scale();
        // The arrow may sit on any edge. "Along" is the axis the arrow slides
        // on (x for bottom/top, y for left/right), "across" the one it points
        // along; the shader maps everything onto the bottom-edge SDF.
        const int arrowEdge = m_windowManager->pointedArrowEdgeFor(w);
        const bool verticalEdge = arrowEdge >= static_cast<int>(BBDX::ArrowEdge::Left);
        const qreal alongHalf = (verticalEdge ? nativeBox.height() : nativeBox.width()) * 0.5;
        const qreal acrossHalf = (verticalEdge ? nativeBox.width() : nativeBox.height()) * 0.5;
        const qreal bodyHalfWidth = std::max(0.0, alongHalf - tooltipInsetDevice);
        const qreal bodyHalfHeight = std::max(
            0.0,
            acrossHalf - tooltipInsetDevice - arrowHeightDevice * 0.5);
        const qreal bodyRadiusDevice = std::min(
            {m_windowManager->pointedMaskBodyRadius(w) * viewport.scale(),
             bodyHalfWidth,
             bodyHalfHeight});
        qreal arrowHalfDevice = std::min(
            m_windowManager->pointedArrowHalfFor(w) * viewport.scale(),
            bodyHalfWidth * 0.44);
        qreal shoulderDevice = 0.0;
        qreal tipRadiusDevice = 0.0;
        if (arrowHeightDevice > std::numeric_limits<qreal>::epsilon()) {
            const qreal flank = std::hypot(arrowHalfDevice, arrowHeightDevice);
            const qreal bodyRoom = std::max(
                0.0,
                bodyHalfWidth - bodyRadiusDevice - arrowHalfDevice - 2.0 * viewport.scale());
            const qreal requestedShoulder = std::min(
                m_windowManager->pointedArrowShoulderFor(w) * viewport.scale(), bodyRoom);
            const qreal requestedTip =
                m_windowManager->pointedArrowTipRadiusFor(w) * viewport.scale();
            const qreal budget = flank * 0.98;
            const qreal shoulderReserve = std::min(requestedShoulder, budget * 0.125);
            tipRadiusDevice = std::min(requestedTip, std::max(0.0, budget - shoulderReserve));
            shoulderDevice = std::min(
                requestedShoulder,
                std::max(0.0, budget - tipRadiusDevice));
        } else {
            arrowHalfDevice = 0.0;
        }
        const QVector4D tooltipGeometry(
            arrowHeightDevice, arrowHalfDevice, shoulderDevice, tipRadiusDevice);
        // The arrow defaults to the body's centre along its edge. A pointed
        // window that published an arrow position (D-Bus, or mined from the
        // region in Window::getFinalBlurRegion) moves it: device px relative
        // to the box centre along the edge, clamped so the joint stays on the
        // body exactly like the client's own drawing clamps. For left/right
        // edges the published coordinate is a frame-local y.
        qreal arrowOffsetDevice = 0.0;
        if (pointedTooltipMask) {
            if (const auto arrowCenter = m_windowManager->pointedArrowCenterX(w)) {
                const qreal arrowDevice = verticalEdge
                    ? (w->frameGeometry().y() + *arrowCenter * data.yScale()
                       + data.yTranslation()) * viewport.scale()
                        - scaledBackgroundRect.y()
                    : (w->frameGeometry().x() + *arrowCenter * data.xScale()
                       + data.xTranslation()) * viewport.scale()
                        - scaledBackgroundRect.x();
                const qreal boxCenterAlong = verticalEdge
                    ? nativeBox.verticalCenter() : nativeBox.horizontalCenter();
                const qreal arrowHalfExtent =
                    arrowHalfDevice + shoulderDevice;
                const qreal maxOffset = std::max(
                    0.0,
                    bodyHalfWidth
                        - bodyRadiusDevice
                        - arrowHalfExtent - 2.0 * viewport.scale());
                arrowOffsetDevice = std::clamp(
                    arrowDevice - boxCenterAlong, -maxOffset, maxOffset);
            }
        }
        const QVector4D tooltipStyle(
            tooltipInsetDevice,
            m_windowManager->pointedArrowFeatherFor(w),
            static_cast<float>(arrowOffsetDevice),
            m_windowManager->pointedTooltipSquircleExponent(w));
        m_blurCache->drawShapedCached(
            viewport,
            renderInfo,
            vbo,
            vertexCount,
            modulation,
            QVector4D(nativeBox.horizontalCenter(), nativeBox.verticalCenter(),
                      nativeBox.width() * 0.5, nativeBox.height() * 0.5),
            nativeCornerRadius.toVector(),
            m_windowManager->squircleExponentFor(w),
            m_windowManager->shapeInsetFor(w) * viewport.scale(),
            tooltipGeometry,
            tooltipStyle,
            shapeMask,
            arrowEdge);
    };

    // BBDX: rate limited
    if (!renderInfo.cache->isFlushing()) {
        drawFinalCached();
        return;
    }

    // The downsample pass of the dual Kawase algorithm: the background will be scaled down 50% every iteration.
    {
        ShaderManager::instance()->pushShader(m_downsamplePass.shader.get());

        QMatrix4x4 projectionMatrix;
        projectionMatrix.ortho(QRectF(0.0, 0.0, backgroundRect.width(), backgroundRect.height()));

        m_downsamplePass.shader->setUniform(m_downsamplePass.mvpMatrixLocation, projectionMatrix);
        m_downsamplePass.shader->setUniform(m_downsamplePass.offsetLocation, float(m_offset));

        for (size_t i = 1; i < renderInfo.framebuffers.size(); ++i) {
            // BBDX: read the blit from cache entry
            const GLFramebuffer *read;
            read = renderInfo.framebuffers[i - 1].get();
            const auto &draw = renderInfo.framebuffers[i];

            const QVector2D halfpixel(0.5 / read->colorAttachment()->width(),
                                      0.5 / read->colorAttachment()->height());
            m_downsamplePass.shader->setUniform(m_downsamplePass.halfpixelLocation, halfpixel);

            BBDX::setTextureSwizzle(read->colorAttachment());
            read->colorAttachment()->bind();

            GLFramebuffer::pushFramebuffer(draw.get());
            BBDX::setGLScissor(dirtyRegion, backgroundRect);
            vbo->draw(GL_TRIANGLES, 0, 6);
        }

        ShaderManager::instance()->popShader();
    }

    // The upsample pass of the dual Kawase algorithm: the background will be scaled up 200% every iteration.
    {
        ShaderManager::instance()->pushShader(m_upsamplePass.shader.get());

        QMatrix4x4 projectionMatrix;
        projectionMatrix.ortho(QRectF(0.0, 0.0, backgroundRect.width(), backgroundRect.height()));

        m_upsamplePass.shader->setUniform(m_upsamplePass.mvpMatrixLocation, projectionMatrix);
        m_upsamplePass.shader->setUniform(m_upsamplePass.offsetLocation, float(m_offset));

        for (size_t i = renderInfo.framebuffers.size() - 1; i > 1; --i) {
            GLFramebuffer::popFramebuffer();
            const auto &read = renderInfo.framebuffers[i];

            const QVector2D halfpixel(0.5 / read->colorAttachment()->width(),
                                      0.5 / read->colorAttachment()->height());
            m_upsamplePass.shader->setUniform(m_upsamplePass.halfpixelLocation, halfpixel);

            BBDX::setTextureSwizzle(read->colorAttachment());
            read->colorAttachment()->bind();

            BBDX::setGLScissor(dirtyRegion, backgroundRect);
            vbo->draw(GL_TRIANGLES, 0, 6);
        }

        ShaderManager::instance()->popShader();
    }

    // A per-window color override (brightness/saturation/contrast) takes
    // priority over the global matrix. On >= 6.6.90 the legacy per-window
    // data.colorMatrix path is compiled out, so this is the only per-window
    // color hook.
    const std::optional<QMatrix4x4> colorMatrixOverride = m_windowManager->getColorMatrixOverride(w);
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    const QMatrix4x4 colorMatrix = colorMatrixOverride ? *colorMatrixOverride
        : (blurInfo.colorMatrix ? *blurInfo.colorMatrix : m_colorMatrix);
#else
    const QMatrix4x4 colorMatrix = colorMatrixOverride ? *colorMatrixOverride : m_colorMatrix;
#endif
#if BBDX_NOT_NEEDED
    if (const BorderRadius cornerRadius = w->window()->borderRadius(); !cornerRadius.isNull()) {
        ShaderManager::instance()->pushShader(m_roundedOnscreenPass.shader.get());

        QMatrix4x4 projectionMatrix = viewport.projectionMatrix();
        projectionMatrix.translate(scaledBackgroundRect.x(), scaledBackgroundRect.y());

        GLFramebuffer::popFramebuffer();
        const auto &read = renderInfo.framebuffers[1];

        const QVector2D halfpixel(0.5 / read->colorAttachment()->width(),
                                  0.5 / read->colorAttachment()->height());

        const RectF transformedRect = RectF{
            w->frameGeometry().x() + data.xTranslation(),
            w->frameGeometry().y() + data.yTranslation(),
            w->frameGeometry().width() * data.xScale(),
            w->frameGeometry().height() * data.yScale(),
        };
        const RectF nativeBox = transformedRect
                                    .scaled(viewport.scale())
                                    .rounded()
                                    .translated(-scaledBackgroundRect.topLeft());
        const BorderRadius nativeCornerRadius = cornerRadius.scaled(viewport.scale()).rounded();

        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.mvpMatrixLocation, projectionMatrix);
        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.colorMatrixLocation, colorMatrix);
        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.halfpixelLocation, halfpixel);
        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.offsetLocation, float(m_offset));
        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.boxLocation, QVector4D(nativeBox.horizontalCenter(), nativeBox.verticalCenter(), nativeBox.width() * 0.5, nativeBox.height() * 0.5));
        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.cornerRadiusLocation, nativeCornerRadius.toVector());
        m_roundedOnscreenPass.shader->setUniform(m_roundedOnscreenPass.opacityLocation, modulation);

        read->colorAttachment()->bind();

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        vbo->draw(GL_TRIANGLES, 6, vertexCount);

        glDisable(GL_BLEND);

        ShaderManager::instance()->popShader();
    } else {
#endif
        if (!m_refractionPass->pushShader()) {
        ShaderManager::instance()->pushShader(m_onscreenPass.shader.get());
        } // indent intentional for KWin diff

        // BBDX: MVP matrix maps to backgroundRect for BlurCache
        QMatrix4x4 projectionMatrix;
        projectionMatrix.ortho(QRectF(0.0, 0.0, backgroundRect.width(), backgroundRect.height()));

        GLFramebuffer::popFramebuffer();
        const auto &read = renderInfo.framebuffers[1];

        const QVector2D halfpixel(0.5 / read->colorAttachment()->width(),
                                  0.5 / read->colorAttachment()->height());

        if (!m_refractionPass->setParameters(projectionMatrix,
                                             colorMatrix,
                                             halfpixel,
                                             float(m_offset),
                                             backgroundRect)) {
        m_onscreenPass.shader->setUniform(m_onscreenPass.mvpMatrixLocation, projectionMatrix);
        m_onscreenPass.shader->setUniform(m_onscreenPass.colorMatrixLocation, colorMatrix);
        m_onscreenPass.shader->setUniform(m_onscreenPass.halfpixelLocation, halfpixel);
        m_onscreenPass.shader->setUniform(m_onscreenPass.offsetLocation, float(m_offset));
        } // indent intentional for KWin diff

        BBDX::setTextureSwizzle(read->colorAttachment());
        read->colorAttachment()->bind();

#if BBDX_NOT_NEEDED
        if (modulation < 1.0) {
            glEnable(GL_BLEND);
            glBlendColor(0, 0, 0, modulation);
            glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
        }
#endif

        // BBDX:
        m_blurCache->drawToCache(renderInfo.cache.get(), vbo);

#if BBDX_NOT_NEEDED
        if (modulation < 1.0) {
            glDisable(GL_BLEND);
        }
#endif

        ShaderManager::instance()->popShader();
#if BBDX_NOT_NEEDED
    }
#endif

    if (m_noiseStrength > 0) {
        // Apply an additive noise onto the blurred image. The noise is useful to mask banding
        // artifacts, which often happens due to the smooth color transitions in the blurred image.

        glEnable(GL_BLEND);
#if BBDX_NOT_NEEDED
        if (opacity < 1.0) {
            glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE);
        } else {
#endif
            glBlendFunc(GL_ONE, GL_ONE);
#if BBDX_NOT_NEEDED
        }
#endif

        if (GLTexture *noiseTexture = ensureNoiseTexture()) {
            ShaderManager::instance()->pushShader(m_noisePass.shader.get());

            // BBDX: MVP matrix maps to backgroundRect for BlurCache
            QMatrix4x4 projectionMatrix;
            projectionMatrix.ortho(QRectF(0.0, 0.0, backgroundRect.width(), backgroundRect.height()));

            m_noisePass.shader->setUniform(m_noisePass.mvpMatrixLocation, projectionMatrix);
            m_noisePass.shader->setUniform(m_noisePass.noiseTextureSizeLocation, QVector2D(noiseTexture->width(), noiseTexture->height()));

            noiseTexture->bind();

            // BBDX:
            m_blurCache->drawToCache(renderInfo.cache.get(), vbo);

            ShaderManager::instance()->popShader();
        }

        glDisable(GL_BLEND);
    }

    // BBDX:
    BBDX::clearGLScissor();
    drawFinalCached();

    vbo->unbindArrays();
}

bool BlurEffect::isActive() const
{
    return m_valid && !effects->isScreenLocked();
}

bool BlurEffect::blocksDirectScanout() const
{
    return false;
}

} // namespace BBDX

#include "moc_blur.cpp"
