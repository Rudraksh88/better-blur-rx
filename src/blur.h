/*
    SPDX-FileCopyrightText: 2010 Fredrik Höglund <fredrik@kde.org>
    SPDX-FileCopyrightText: 2018 Alex Nemeth <alex.nemeth329@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QStringList>

#include "blur_cache.hpp"

#include "kwin_compat.hpp"

#include "refraction_pass.hpp"
#include "window_manager.hpp"
#include "settings.hpp"

#include <memory>
#include <opengl/glframebuffer.h>

#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 5, 80)
#  include <core/rect.h>
#  include <core/region.h>
#endif

#include <effect/effect.h>
#include <effect/effectwindow.h>
#include <opengl/glutils.h>
#include <scene/item.h>
#include <scene/scene.h>
#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 6, 4)
#include <scene/backgroundeffectitem.h>
#endif
#include <window.h>

#include <QList>

#include <optional>
#include <unordered_map>

namespace KWin {
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
class BlurManagerInterface;
class ContrastManagerInterface;
#endif
}

namespace BBDX {
using namespace KWin;

class BlurCacheEntry;

struct BlurRenderData
{
    /// Temporary render targets needed for the Dual Kawase algorithm, the first texture
    /// contains not blurred background behind the window, it's cached.
    std::vector<std::unique_ptr<GLTexture>> textures;
    std::vector<std::unique_ptr<GLFramebuffer>> framebuffers;

    std::unique_ptr<BBDX::BlurCacheEntry> cache;
};

struct BlurEffectData
{
    /// The region that should be blurred behind the window
    std::optional<RegionF> content;

    /// The region that should be blurred behind the frame
    std::optional<RegionF> frame;

    /**
     * The render data per render view, as they can have different
     *  color spaces and even different windows on them
     */
    std::unordered_map<RenderView *, BlurRenderData> render;

#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 4)
    ItemEffect windowEffect;
#else
    std::unique_ptr<BackgroundEffectItem> blurItem;
#endif

    /**
     * Color transformation matrix (brightness, contrast, and saturation).
     */
    std::optional<QMatrix4x4> colorMatrix;

    /// Alpha mask diagnostics: last composite path (-1 unknown, 0 geometric
    /// fallback, 1 alpha) and when it was last logged.
    int alphaPathState{-1};
    std::chrono::steady_clock::time_point alphaPathLogged{};
    bool alphaSampled{false};
};

class BlurEffect : public KWin::Effect
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin.Effects.BetterBlurDX")

public:
    BlurEffect();
    ~BlurEffect() override;

    static bool supported();
    static bool enabledByDefault();

    void reconfigure(ReconfigureFlags flags) override;
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    void prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime) override;
#else
    void prePaintScreen(ScreenPrePaintData &data) override;
#endif
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 5, 80)
    void prePaintWindow(EffectWindow *w, WindowPrePaintData &data, std::chrono::milliseconds presentTime) override;
#elif KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    void prePaintWindow(RenderView *view, EffectWindow *w, WindowPrePaintData &data, std::chrono::milliseconds presentTime) override;
#else
    void prePaintWindow(RenderView *view, EffectWindow *w, WindowPrePaintData &data) override;
#endif
    void drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const Region &deviceRegion, WindowPaintData &data) override;

    bool provides(Feature feature) override;
    bool isActive() const override;

    int requestedEffectChainPosition() const override
    {
        return 20;
    }

    bool eventFilter(QObject *watched, QEvent *event) override;

    bool blocksDirectScanout() const override;

Q_SIGNALS:
    /**
     * Emitted when the effect (re)starts: every shape published through
     * setSurfaceShape is gone and clients should publish again.
     */
    Q_SCRIPTABLE void surfaceShapesReset();

public Q_SLOTS:
    /**
     * Publish a blur shape for every window of one resource class (app_id),
     * in memory, on top of the configured window overrides.
     *
     * kind: "inherit" (or ""), "rounded", "squircle", "squircle-flat",
     *       "pointed", "alpha", "region".
     * radius: corner radius in logical px; < 0 inherits.
     * exponent: superellipse exponent (2..8) for squircle / pointed bodies;
     *           <= 0 inherits.
     * arrowCenterX: pointed mask arrow tip, frame-local logical x; < 0 or
     *               NaN inherits (falls back to the region-mined position).
     * inset: mask inset in logical px; < 0 inherits.
     * Returns false for an empty appId, unknown kind or non-finite value.
     */
    Q_SCRIPTABLE bool setSurfaceShape(const QString &appId,
                                      const QString &kind,
                                      double radius,
                                      double exponent,
                                      double arrowCenterX,
                                      double inset);
    /**
     * Publish the pointed-arrow geometry for one resource class. Creates the
     * class entry if setSurfaceShape was not called yet; clearSurfaceShape
     * removes both.
     *
     * edge: "bottom", "top", "left", "right"; "" or "inherit" = bottom.
     *       For left/right the shape's arrowCenterX is read as frame-local y.
     * height, halfWidth, shoulder, tipRadius: logical px; < 0 inherits the
     *       global PointedTooltip* value.
     * feather: antialiasing width multiplier (0.25..4); < 0 inherits.
     */
    Q_SCRIPTABLE bool setSurfaceArrow(const QString &appId,
                                      const QString &edge,
                                      double height,
                                      double halfWidth,
                                      double shoulder,
                                      double tipRadius,
                                      double feather);
    /// Remove a published shape and arrow; false if none was published for appId.
    Q_SCRIPTABLE bool clearSurfaceShape(const QString &appId);
    /// One "appId kind radius exponent arrowCenterX inset" line per shape.
    Q_SCRIPTABLE QStringList surfaceShapes() const;
    /**
     * What this build and configuration can do, so a client can tell "the
     * D-Bus API exists" from "alpha masking is actually on" without reading
     * kwinrc. Tokens:
     *   api=1                 contract version of the surface-shape methods
     *   surface-shape         setSurfaceShape / clearSurfaceShape / surfaceShapes
     *   surface-arrow         setSurfaceArrow (edge, height, ...)
     *   remnant-alpha         published classes keep their last
     *                         wp_alpha_modifier through the close animation
     *   alpha-mask            kind=alpha is compiled in (Plasma 6.7 Wayland)
     *   alpha-mask-enabled    the global AlphaMask toggle is on
     *   alpha-mask-threshold=<v>  current AlphaMaskThreshold
     *   alpha-mask-mode=threshold|silhouette  how alpha maps to coverage
     *   alpha-mask-floor=<v>  silhouette mode: alpha treated as transparent
     */
    Q_SCRIPTABLE QStringList capabilities() const;
    /// Update dock SDF geometry in memory for frame-grade Settings previews.
    Q_SCRIPTABLE bool previewDockSurfaces(double plateBlurRadius,
                                          double plateMaskInset,
                                          double squircleExponent,
                                          double tooltipRadius,
                                          double tooltipSquircleExponent,
                                          double tooltipArrowHeight,
                                          double tooltipArrowHalf,
                                          double tooltipShoulder,
                                          double tooltipTipRadius,
                                          double tooltipInset,
                                          double tooltipFeather);
    void slotWindowAdded(KWin::EffectWindow *w);
    void slotWindowDeleted(KWin::EffectWindow *w);
    void slotViewRemoved(KWin::RenderView *view);
    void slotPropertyNotify(KWin::EffectWindow *w, long atom);
    void setupDecorationConnections(EffectWindow *w);

private:
    void initBlurStrengthValues();
    RegionF blurRegion(EffectWindow *w) const;
    RegionF decorationBlurRegion(const EffectWindow *w) const;
    bool decorationSupportsBlurBehind(const EffectWindow *w) const;
    bool shouldBlur(const EffectWindow *w, int mask, const WindowPaintData &data) const;
    void updateBlurRegion(EffectWindow *w);
    void blur(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, const Region &deviceRegion, WindowPaintData &data);
    GLTexture *ensureNoiseTexture();

private:
    struct
    {
        std::unique_ptr<GLShader> shader;
        int mvpMatrixLocation;
        int colorMatrixLocation;
        int offsetLocation;
        int halfpixelLocation;
    } m_onscreenPass;

#if BBDX_NOT_NEEDED
    struct
    {
        std::unique_ptr<GLShader> shader;
        int mvpMatrixLocation;
        int colorMatrixLocation;
        int offsetLocation;
        int halfpixelLocation;
        int boxLocation;
        int cornerRadiusLocation;
        int opacityLocation;
    } m_roundedOnscreenPass;
#endif

    struct
    {
        std::unique_ptr<GLShader> shader;
        int mvpMatrixLocation;
        int offsetLocation;
        int halfpixelLocation;
    } m_downsamplePass;

    struct
    {
        std::unique_ptr<GLShader> shader;
        int mvpMatrixLocation;
        int offsetLocation;
        int halfpixelLocation;
    } m_upsamplePass;

    struct
    {
        std::unique_ptr<GLShader> shader;
        int mvpMatrixLocation;
        int noiseTextureSizeLocation;

        std::unique_ptr<GLTexture> noiseTexture;
        qreal noiseTextureScale = 1.0;
        int noiseTextureStength = 0;
    } m_noisePass;

    bool m_valid = false;
    long net_wm_blur_region = 0;
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 4)
    Region m_paintedDeviceArea; // keeps track of all painted areas (from bottom to top)
    Region m_currentDeviceBlur; // keeps track of currently blurred area of the windows (from bottom to top)
#endif

    RenderView *m_currentView = nullptr;

    QMatrix4x4 m_colorMatrix;
    size_t m_iterationCount; // number of times the texture will be downsized to half size
    int m_offset;
    int m_expandSize;
    int m_noiseStrength;

    struct OffsetStruct
    {
        float minOffset;
        float maxOffset;
        int expandSize;
    };

    QList<OffsetStruct> blurOffsets;

    struct BlurValuesStruct
    {
        int iteration;
        float offset;
    };

    QList<BlurValuesStruct> blurStrengthValues;

    QMap<EffectWindow *, QMetaObject::Connection> windowBlurChangedConnections;
#if !defined(BBDX_X11) && KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    QMap<EffectWindow *, QMetaObject::Connection> windowContrastChangedConnections;
#endif
    std::unordered_map<EffectWindow *, BlurEffectData> m_windows;

#if !defined(BBDX_X11) && KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    static BlurManagerInterface *s_blurManager;
    static QTimer *s_blurManagerRemoveTimer;

    static ContrastManagerInterface *s_contrastManager;
    static QTimer *s_contrastManagerRemoveTimer;
#endif

    // BBDX Mixins
    bool m_forceContrastParams{false};

    std::unique_ptr<BBDX::WindowManager> m_windowManager{};
    friend void BBDX::WindowManager::triggerBlurRegionUpdate(KWin::EffectWindow *w) const;
    friend void BBDX::WindowManager::invalidateBlurCache(KWin::EffectWindow *w, uint flags, const char *reason) const;
    friend void BBDX::WindowManager::flushWindowCaches(BBDX::Window *window) const;
    friend void BBDX::WindowManager::flushWindowCachesFor(BBDX::Window *window, std::chrono::milliseconds duration) const;
    std::unique_ptr<BBDX::BlurCache> m_blurCache{};
    friend void BBDX::BlurCache::flushAccumulatedDirtyRegions(KWin::ScreenPrePaintData &data) const;
    std::unique_ptr<BBDX::RefractionPass> m_refractionPass{};

public:
    WindowManager* windowManager() const { return m_windowManager.get(); }
    BlurCache* blurCache() const { return m_blurCache.get(); }
};

inline bool BlurEffect::provides(Effect::Feature feature)
{
    if (feature == Blur) {
        return true;
    }
    return KWin::Effect::provides(feature);
}

} // namespace BBDX
