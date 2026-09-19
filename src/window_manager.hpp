#pragma once

#include "kwin_compat.hpp"
#include "window.hpp"

#include <effect/effect.h>
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>

#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 5, 80)
#  include <core/output.h>
#  include <core/region.h>
#endif

#include <QHash>
#include <QList>
#include <QMatrix4x4>
#include <QObject>
#include <QStringList>
#include <QRegularExpression>
#include <QString>

#include <memory>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace KWin {
    class BorderRadius;
}

namespace BBDX {
class BlurEffect;

/**
 * A single window-specific override entry.
 *
 * Holds the parsed class matcher (exact / regex / $blank, same convention as
 * the force-blur window classes) and the optionally overridden parameters.
 *
 * Brightness/saturation/contrast are stored relative to 1.0 (i.e. already
 * divided by 100) so they can be fed straight into BBDX::colorTransformMatrix.
 */
struct WindowOverride {
    QString className{};
    bool isRegex{false};
    bool isBlank{false};
    QRegularExpression regex{};

    std::optional<qreal> cornerRadius{};
    std::optional<qreal> brightness{};
    std::optional<qreal> saturation{};
    std::optional<qreal> contrast{};
    bool squircle{false};
    bool pointed{false};
    /// Shape the blur by the window's alpha channel (forces AlphaMask on).
    bool alphaMask{false};
    /// Keep the client's raw blur region (opts out of AlphaMask).
    bool rawRegion{false};
    /// The docked plate's squircle keeps its top radii but meets the screen
    /// edge with square lower corners.
    bool flatBottom{false};
};

/**
 * A shape a client published at runtime over D-Bus for one resource class.
 *
 * Every field is optional: an unset field inherits from the matching config
 * override (or the global value). Not persisted; cleared when the effect is
 * reloaded, which the effect announces with the surfaceShapesReset signal.
 */
struct SurfaceShape {
    /// "", rounded, squircle, squircle-flat, pointed, alpha or region.
    QString kind{};
    std::optional<qreal> radius{};
    /// Superellipse exponent for squircle bodies (2 = rounded rectangle).
    std::optional<qreal> exponent{};
    /// Pointed mask arrow tip centre, frame-local logical x.
    std::optional<qreal> arrowCenterX{};
    /// Mask inset in logical pixels (squircle plate / pointed body).
    std::optional<qreal> inset{};
};

class WindowManager : public QObject {
    Q_OBJECT

public:
    enum class WindowClassMatchMode {
        Whitelist,
        Blacklist,
    };

private:
    // pointer to the owning BlurEffect instance
    BBDX::BlurEffect *m_effect;

    // managed windows
    std::unordered_map<const KWin::EffectWindow *, std::unique_ptr<BBDX::Window>> m_windows{};

    // docks seperate for maximized calculation
    std::unordered_set<const KWin::EffectWindow *> m_docks{};

    // window classes
    QList<QString> m_windowClassesFixed{};
    QList<QRegularExpression> m_windowClassesRegex{};
    WindowClassMatchMode m_windowClassMatchMode{WindowClassMatchMode::Whitelist};

    // window/region types
    bool m_blurDecorations{false};
    bool m_blurDocks{false};
    bool m_blurMenus{false};

    // user configured border radius
    qreal m_userBorderRadius{0.0};
    bool m_alphaMask{false};
    qreal m_alphaMaskThreshold{0.5};

    // global color params (relative to 1.0), used to fill in
    // inherited values for window-specific overrides
    qreal m_brightness{1.0};
    qreal m_saturation{1.0};
    qreal m_contrast{1.0};

    // Runtime shape parameters. The dock owns the persisted source values;
    // Better Blur consumes them as uniforms on the next effect reconfigure.
    qreal m_squircleExponent{3.1};
    qreal m_plateMaskInset{0.0};
    qreal m_pointedTooltipRadius{5.0};
    qreal m_pointedTooltipSquircleExponent{2.0};
    qreal m_pointedTooltipArrowHeight{8.0};
    qreal m_pointedTooltipArrowHalf{6.3};
    qreal m_pointedTooltipShoulder{4.0};
    qreal m_pointedTooltipTipRadius{1.6};
    qreal m_pointedTooltipInset{1.0};
    qreal m_pointedTooltipFeather{1.0};
    qreal m_menuRadius{0.0};
    bool m_menuSquircle{false};
    std::optional<qreal> m_previewPlateBlurRadius{};

    // window-specific overrides, first match wins
    QList<WindowOverride> m_windowOverrides{};

    // Runtime per-surface shapes (D-Bus), keyed by exact resource class, and
    // the override each one resolves to once merged over the config table.
    QHash<QString, SurfaceShape> m_surfaceShapes{};
    QHash<QString, WindowOverride> m_surfaceOverrides{};

    /** First config override whose matcher accepts this resource class. */
    const WindowOverride *configOverrideForClass(const QString &windowClass) const;
    /** Recompute m_surfaceOverrides[appId] from config + m_surfaceShapes. */
    void rebuildSurfaceOverride(const QString &appId);
    /** Reconfigure, re-region and repaint every window of that class. */
    void applySurfaceShape(const QString &appId);
    /** The published shape for this window's class, nullptr if none. */
    const SurfaceShape *surfaceShapeFor(const KWin::EffectWindow *w) const;

    // match helpers
    bool matchesWindowClassFixed(const KWin::EffectWindow *w) const;
    bool matchesWindowClassRegex(const KWin::EffectWindow *w) const;
    bool overrideMatchesWindow(const WindowOverride &o, const KWin::EffectWindow *w) const;

    /**
     * Find a managed window, nullptr if not found
     */
    BBDX::Window* findWindow(const KWin::EffectWindow *w) const;

    /**
     * Collect BBDX Windows sorted by stackingOrder
     */
    std::vector<BBDX::Window *> windowsByStackingOrder() const;

public Q_SLOT:
    void slotWindowAdded(KWin::EffectWindow *w);
    void slotWindowDeleted(KWin::EffectWindow *w);

public:
    explicit WindowManager(BBDX::BlurEffect *effect);

    /**
     * reconfigure from BlurConfig
     */
    void reconfigure();

    /** Apply one validated, non-persistent dock SDF preview snapshot. */
    bool previewDockSurfaces(qreal plateBlurRadius,
                             qreal plateMaskInset,
                             qreal squircleExponent,
                             qreal tooltipRadius,
                             qreal tooltipSquircleExponent,
                             qreal tooltipArrowHeight,
                             qreal tooltipArrowHalf,
                             qreal tooltipShoulder,
                             qreal tooltipTipRadius,
                             qreal tooltipInset,
                             qreal tooltipFeather);

    /**
     * Runtime per-surface shapes (D-Bus).
     *
     * Values already validated and clamped by the caller; std::nullopt means
     * inherit. kind must be one of SurfaceShape's kinds or empty.
     */
    bool setSurfaceShape(const QString &appId,
                         const QString &kind,
                         std::optional<qreal> radius,
                         std::optional<qreal> exponent,
                         std::optional<qreal> arrowCenterX,
                         std::optional<qreal> inset);
    bool clearSurfaceShape(const QString &appId);
    /** One "appId kind radius exponent arrowCenterX inset" line per shape. */
    QStringList surfaceShapes() const;

    /** Squircle exponent for this window: published shape, else global. */
    qreal squircleExponentFor(const KWin::EffectWindow *w) const;
    /** Mask inset for a squircle body: published shape, else the plate inset. */
    qreal shapeInsetFor(const KWin::EffectWindow *w) const;

    /**
     * Refresh maximized state of a window / of all windows
     */
    void refreshMaximizedState(BBDX::Window *w) const;
    void refreshMaximizedStateAll() const;

    /**
     * getters
     */
    bool blurDecorations() const { return m_blurDecorations; }
    bool blurDocks() const { return m_blurDocks; }
    bool blurMenus() const { return m_blurMenus; }
    qreal userBorderRadius() const { return m_userBorderRadius; }
    qreal brightness() const { return m_brightness; }
    qreal saturation() const { return m_saturation; }
    qreal contrast() const { return m_contrast; }

    /**
     * Get the first matching window-specific override for w,
     * or nullptr if none matches.
     */
    const WindowOverride *overrideFor(const KWin::EffectWindow *w) const;

    /**
     * Get the color transformation matrix for a window if it has a
     * window-specific color override, or empty otherwise.
     */
    std::optional<QMatrix4x4> getColorMatrixOverride(const KWin::EffectWindow *w) const;

    /** Whether the matching override requests a superellipse corner mask. */
    bool usesSquircleMask(const KWin::EffectWindow *w) const;

    /**
     * Whether this window's blur is shaped by its own alpha channel.
     *
     * Requires the global AlphaMask toggle. Explicit SDF overrides (squircle,
     * pointed) and the menu mask keep priority; a "region" override opts out.
     * Otherwise an "alpha" override always qualifies, and by default so does
     * any undecorated surface that requested blur itself, i.e. the class of
     * CSD / layer-shell windows whose integer wl_region would otherwise draw
     * aliased corners.
     */
    bool usesAlphaMask(const KWin::EffectWindow *w) const;
    bool alphaMaskEnabled() const { return m_alphaMask; }
    qreal alphaMaskThreshold() const { return m_alphaMaskThreshold; }

    /** Whether the client asked for this surface's blur itself. */
    bool blurRequestedByClient(const KWin::EffectWindow *w) const;

    /** Whether that superellipse has square lower corners. */
    bool usesFlatBottomSquircleMask(const KWin::EffectWindow *w) const;

    /** Whether this surface uses the dock's integrated pointed-tooltip mask. */
    bool usesPointedTooltipMask(const KWin::EffectWindow *w) const;
    bool usesPointedMenuMask(const KWin::EffectWindow *w) const;
    qreal pointedMaskInset(const KWin::EffectWindow *w) const;
    /**
     * Body corner radius for a pointed-mask window: the override's radius
     * when set (e.g. the thumbnail popup's 10), else the shared tooltip
     * radius — so the tooltip's blank override keeps its configured shape.
     */
    qreal pointedMaskBodyRadius(const KWin::EffectWindow *w) const;
    /** Squircle exponent for the real tooltip; other pointed clients stay rounded. */
    qreal pointedTooltipSquircleExponent(const KWin::EffectWindow *w) const;

    /**
     * Arrow tip center x (frame-local logical) a pointed-mask window published
     * through its client blur region; nullopt = centre the arrow on the body.
     */
    std::optional<qreal> pointedArrowCenterX(const KWin::EffectWindow *w) const;

    /** Whether this is the dock plate governed by the plate-only SDF inset. */
    bool usesDockPlateMask(const KWin::EffectWindow *w) const;

    /**
     * Whether this is a menu or combo-box popup whose corners we round. Such a
     * surface shapes its own blur region, but only to whole pixels, so that its
     * corners are aliased; the mask replaces them with antialiased ones.
     */
    bool usesMenuMask(const KWin::EffectWindow *w) const;

    qreal squircleExponent() const { return m_squircleExponent; }
    qreal plateMaskInset() const { return m_plateMaskInset; }
    qreal menuRadius() const { return m_menuRadius; }
    bool menuSquircle() const { return m_menuSquircle; }
    qreal pointedTooltipRadius() const { return m_pointedTooltipRadius; }
    qreal pointedTooltipArrowHeight() const { return m_pointedTooltipArrowHeight; }
    qreal pointedTooltipArrowHalf() const { return m_pointedTooltipArrowHalf; }
    qreal pointedTooltipShoulder() const { return m_pointedTooltipShoulder; }
    qreal pointedTooltipTipRadius() const { return m_pointedTooltipTipRadius; }
    qreal pointedTooltipInset() const { return m_pointedTooltipInset; }
    qreal pointedTooltipFeather() const { return m_pointedTooltipFeather; }

    /**
     * Match an EffectWindow instance in the black/white list
     * XXX: should this also be moved to BBDX::Window?
     */
    bool shouldForceBlurWindowClass(const KWin::EffectWindow *w) const;

    /**
     * emits the wantsBlurRegionUpdate signal
     */
    void triggerBlurRegionUpdate(KWin::EffectWindow *w) const;

    /**
     * emits the windowInvalidatedBlurCache signal
     */
    void invalidateBlurCache(KWin::EffectWindow *w, uint flags, const char *reason) const;

    /**
     * Set the "window is transformed" flag on a window
     * (scaled, translated or PAINT_WINDOW_TRANSFORMED set)
     */
    void setWindowIsTransformed(const KWin::EffectWindow *w, bool toggle) const;

    /**
     * Check if this window should currently be blurred
     * even when PAINT_WINDOW_TRANSFORMED is set
     */
    bool windowShouldBlurWhileTransformed(const KWin::EffectWindow *w) const;

    /**
     * Check if this window is blurred in any way (requested or forced)
     */
    bool windowIsBlurred(const KWin::EffectWindow *w) const;

    /**
     * Check if this window is a Plasma surface using its requested blur mask.
     */
    bool windowIsPlasmaSurface(const KWin::EffectWindow *w) const;

    /**
     * Get the final blur region for a window, set in content/frame.
     *
     * Forwarded to BBDX::Window::getFinalBlurRegion() if w is managed
     * else does nothing.
     */
    void getFinalBlurRegion(const KWin::EffectWindow *w, std::optional<KWin::RegionF> &content, std::optional<KWin::RegionF> &frame) const;

    /**
     * Get effective border radius for requested window,
     * or empty if unmanaged
     */
    KWin::BorderRadius getEffectiveBorderRadius(const KWin::EffectWindow *w) const;

    /**
     * Get effective blur opacity for requested window
     */
    qreal getEffectiveBlurOpacity(const KWin::EffectWindow *w, KWin::WindowPaintData &data) const;

    /**
     * Check if the provided window's blur region is fully covered by
     * the frame geometry of other windows
     */
    bool windowIsBlurFullyCovered(KWin::EffectWindow *w) const;

    /**
     * Add a full repaint to all blurred windows
     */
    void repaintAllBlurredWindows() const;

    /**
     * Flush all caches of a window
     */
    void flushWindowCaches(BBDX::Window *window) const;
    void flushWindowCachesFor(BBDX::Window *window, std::chrono::milliseconds duration) const;

    /**
     * Flush all caches of all windows
     */
    void flushAllWindowCaches() const;
    void flushAllWindowCachesFor(std::chrono::milliseconds duration) const;
};

} // namespace KWin
