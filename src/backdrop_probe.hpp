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
    bool clear(const QString &appId);
    /** One "appId x y width height bins" line per probe. */
    QStringList list() const;
    bool isEmpty() const { return m_probes.empty(); }

    /** True if w has a probe (cheap; called for every blurred window). */
    bool wants(const KWin::EffectWindow *w) const;
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
    static constexpr int kMinIntervalMs = 50;
    static constexpr int kMinCaptureMs = 33;
    // A bin must move by more than this (luma 0..1) for a new signal.
    static constexpr double kEpsilon = 1.5 / 255.0;

private:
    struct Probe {
        QRectF rect;
        int bins = 0;
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

    /** The probe key for w ("<class>@dock" for dock windows if probed), or "". */
    QString keyFor(const KWin::EffectWindow *w) const;
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
