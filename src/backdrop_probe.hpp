#pragma once

#include "kwin_compat.hpp"

#include <opengl/glframebuffer.h>
#include <opengl/gltexture.h>
#include <opengl/glutils.h>

#include <QElapsedTimer>
#include <map>
#include <QList>
#include <QObject>
#include <QRectF>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <vector>

namespace KWin {
class EffectWindow;
}

namespace BBDX {

/**
 * How bright the backdrop behind a window is, read from the blur the effect
 * computes anyway (docs/surfboard-integration.md, "Backdrop probes").
 *
 * A client registers a strip of its surface (setSurfaceProbe). Whenever the
 * effect re-blurs that window's backdrop, the strip of the final blurred
 * texture is scaled into `bins` texels in a tiny framebuffer and read into a
 * pixel buffer. Nothing waits on the GPU, and nothing flushes mid-frame: the
 * timer that polls the readback makes its fence after KWin has submitted the
 * frame (a fence made inside the frame flushed KWin's command stream early,
 * ~140 us of CPU on radeonsi), then looks at it with a zero timeout; once it
 * has signalled, the bins' luma is
 * compared with what was last sent and, if any bin moved, sent as the
 * surfaceProbe signal, at most every kMinIntervalMs (the last change always
 * goes out). A probe is captured at most every kMinCaptureMs with one
 * readback in flight; a blur pass skipped for either reason marks the probe
 * stale, and if no later pass captures it by the end of the interval the
 * window is re-blurred once (`refresh`), so the last change of a burst is
 * never missed.
 *
 * A probe is either a strip (setSurfaceProbe: `bins` values across a rect)
 * or a set of points (setSurfacePoints: one value per point, anywhere on the
 * surface, e.g. under labels laid out along a curve). Points cost what a
 * strip does: their bounding box is scaled into a small grid (texels at
 * least kPointCell logical px apart, at most kMaxGridTexels) with the same
 * single blit and readback, and each point's luma is interpolated from the
 * grid when the readback lands, outside the frame.
 */
class BackdropProbe : public QObject {
    Q_OBJECT
public:
    using Emit = std::function<void(const QString &appId, const QList<double> &luma)>;
    /** Re-blur (invalidate and repaint) every window under this probe key. */
    using Refresh = std::function<void(const QString &appId)>;
    BackdropProbe(Emit emitResult, Refresh refresh);
    ~BackdropProbe() override;

    /** rect: surface-local logical px; bins: 1..kMaxBins. False if invalid. */
    bool set(const QString &appId, const QRectF &rect, int bins);
    /** points: surface-local logical px, 1..kMaxBins of them. False if invalid. */
    bool setPoints(const QString &appId, const QList<QPointF> &points);
    bool clear(const QString &appId);
    /**
     * One line per probe: "appId x y width height bins" for a strip,
     * "appId x y width height count points" (their bounding box) for points.
     */
    QStringList list() const;
    bool isEmpty() const { return m_probes.empty(); }

    /** True if w has a probe (cheap; called for every blurred window). */
    bool wants(const KWin::EffectWindow *w) const;
    /**
     * The probe key for w: "<class>@dock" / "<class>@utility" for a window
     * of that type if probed (BBDX::surfaceTypeSuffix), else "<class>" if
     * probed, else "".
     */
    QString keyFor(const KWin::EffectWindow *w) const;
    /**
     * After the blur passes for w: sample `blurred` (the final blurred
     * backdrop, covering `backgroundRect` in logical px at its own size).
     * A surface-local point p lands at origin + p * scale in backgroundRect's
     * space (the window's position and paint transform, as the blur shape
     * is placed). The GL context is current (painting stage).
     */
    void capture(const KWin::EffectWindow *w,
                 const QPointF &origin,
                 const QSizeF &scale,
                 const QRect &backgroundRect,
                 KWin::GLFramebuffer *blurred);

    /** Drop all GL objects (effect teardown, context current). */
    void releaseGl();

    static constexpr int kMaxBins = 512;
    // Points: grid texels at least this far apart (logical px; the backdrop
    // is blurred, so neighbouring texels interpolate it well) and at most
    // this many of them.
    static constexpr double kPointCell = 8.0;
    static constexpr int kMaxGridTexels = 4096;
    static constexpr int kMinIntervalMs = 50;
    static constexpr int kMinCaptureMs = 33;
    // A bin must move by more than this (luma 0..1) for a new signal.
    static constexpr double kEpsilon = 1.5 / 255.0;

private:
    struct Probe {
        QRectF rect; // a strip; for points, their bounding box
        int bins = 0; // values sent: the strip's bins or the point count
        QList<QPointF> points; // empty for a strip
        double cell = 0.0; // points: the grid's texel spacing, logical px
        QSize grid; // the readback: bins x 1 for a strip
        // Points: each point's position in the grid of the readback in
        // flight, in texels (NaN off the blurred backdrop).
        std::vector<QPointF> gridAt;
        QList<double> sent;
        QList<double> latest; // read back, not yet sent (rate limit)
        bool hasLatest = false;
        std::unique_ptr<KWin::GLTexture> texture;
        std::unique_ptr<KWin::GLFramebuffer> framebuffer;
        GLuint pixelBuffer = 0;
        bool reading = false; // a readback queued, its fence not yet made or signalled
        GLsync fence = nullptr;
        QElapsedTimer sinceSent;
        QElapsedTimer sinceCapture;
        bool stale = false; // a re-blur since the last capture went unread
        // Bins [validFrom, validTo) of the readback in flight lie on the
        // blurred backdrop; the rest read -1.
        int validFrom = 0;
        int validTo = 0;
    };

    /** Drop GL objects of a probe whose readback size changes. */
    void resize(Probe &probe, const QSize &grid);
    /**
     * The texels of the blurred texture to read for a strip or points probe
     * (empty if none) and where they go in its framebuffer.
     */
    QRect stripSource(Probe &probe,
                      const QPointF &origin,
                      const QSizeF &scale,
                      const QRect &backgroundRect,
                      const QSize &textureSize,
                      QRect &destination);
    QRect pointSource(Probe &probe,
                      const QPointF &origin,
                      const QSizeF &scale,
                      const QRect &backgroundRect,
                      const QSize &textureSize,
                      QRect &destination);
    QList<double> pointLuma(const Probe &probe, const uchar *pixels) const;
    bool ensureGl(Probe &probe);
    void releaseGl(Probe &probe);
    void poll();
    void flush(const QString &appId, Probe &probe);
    void refreshStale();

    Emit m_emit;
    Refresh m_refresh;
    std::map<QString, Probe> m_probes;
    QTimer m_pollTimer;
    QTimer m_sendTimer;
    QTimer m_refreshTimer;
    bool m_loggedFormat = false;
};

} // namespace BBDX
