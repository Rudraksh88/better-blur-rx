#include "backdrop_probe.hpp"
#include "utils.h"

#include <effect/effecthandler.h>
#include <effect/effectwindow.h>
#include <window.h>

#include <QLoggingCategory>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(BACKDROP_PROBE, "kwin_effect_better_blur_dx.backdrop_probe", QtInfoMsg)

namespace {
const QString kDockKeySuffix = QStringLiteral("@dock");
// While a readback is in flight, look at its fence this often.
constexpr int kPollMs = 4;
}

namespace BBDX {

BackdropProbe::BackdropProbe(Emit emitResult, Refresh refresh)
    : m_emit(std::move(emitResult))
    , m_refresh(std::move(refresh))
{
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(kMinCaptureMs);
    connect(&m_refreshTimer, &QTimer::timeout, this, &BackdropProbe::refreshStale);
    m_pollTimer.setInterval(kPollMs);
    m_pollTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_pollTimer, &QTimer::timeout, this, &BackdropProbe::poll);
    m_sendTimer.setSingleShot(true);
    connect(&m_sendTimer, &QTimer::timeout, this, [this] {
        for (auto it = m_probes.begin(); it != m_probes.end(); ++it) {
            if (it->second.hasLatest) {
                flush(it->first, it->second);
            }
        }
    });
}

BackdropProbe::~BackdropProbe() = default;

bool BackdropProbe::set(const QString &appId, const QRectF &rect, int bins)
{
    if (appId.isEmpty() || bins < 1 || bins > kMaxBins || !std::isfinite(rect.x()) || !std::isfinite(rect.y())
        || !std::isfinite(rect.width()) || !std::isfinite(rect.height()) || rect.width() <= 0.0
        || rect.height() <= 0.0) {
        return false;
    }
    Probe &probe = m_probes[appId];
    if (probe.bins != bins && (probe.texture || probe.reading)) {
        if (KWin::effects->makeOpenGLContextCurrent()) {
            releaseGl(probe);
        }
    }
    if (probe.bins != bins || probe.rect != rect) {
        // A new strip: whatever comes next is news.
        probe.sent.clear();
        probe.hasLatest = false;
    }
    probe.rect = rect;
    probe.bins = bins;
    return true;
}

bool BackdropProbe::clear(const QString &appId)
{
    auto it = m_probes.find(appId);
    if (it == m_probes.end()) {
        return false;
    }
    if ((it->second.texture || it->second.reading) && KWin::effects->makeOpenGLContextCurrent()) {
        releaseGl(it->second);
    }
    m_probes.erase(it);
    return true;
}

QStringList BackdropProbe::list() const
{
    QStringList lines;
    for (auto it = m_probes.cbegin(); it != m_probes.cend(); ++it) {
        lines.append(QStringLiteral("%1 %2 %3 %4 %5 %6")
                         .arg(it->first)
                         .arg(it->second.rect.x())
                         .arg(it->second.rect.y())
                         .arg(it->second.rect.width())
                         .arg(it->second.rect.height())
                         .arg(it->second.bins));
    }
    return lines;
}

QString BackdropProbe::keyFor(const KWin::EffectWindow *w) const
{
    const QString windowClass = w->window()->resourceClass();
    if (w->isDock()) {
        const QString dockKey = windowClass + kDockKeySuffix;
        if (m_probes.contains(dockKey)) {
            return dockKey;
        }
    }
    return m_probes.contains(windowClass) ? windowClass : QString();
}

bool BackdropProbe::wants(const KWin::EffectWindow *w) const
{
    return !m_probes.empty() && !keyFor(w).isEmpty();
}

bool BackdropProbe::ensureGl(Probe &probe)
{
    if (probe.framebuffer && probe.framebuffer->valid() && probe.pixelBuffer) {
        return true;
    }
    releaseGl(probe);
    probe.texture = KWin::GLTexture::allocate(GL_RGBA8, QSize(probe.bins, 1));
    if (!probe.texture) {
        return false;
    }
    probe.texture->setFilter(GL_NEAREST);
    probe.framebuffer = std::make_unique<KWin::GLFramebuffer>(probe.texture.get());
    if (!probe.framebuffer->valid()) {
        releaseGl(probe);
        return false;
    }
    glGenBuffers(1, &probe.pixelBuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, probe.pixelBuffer);
    glBufferData(GL_PIXEL_PACK_BUFFER, probe.bins * 4, nullptr, GL_STREAM_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return true;
}

void BackdropProbe::releaseGl(Probe &probe)
{
    if (probe.fence) {
        glDeleteSync(probe.fence);
        probe.fence = nullptr;
    }
    probe.reading = false;
    if (probe.pixelBuffer) {
        glDeleteBuffers(1, &probe.pixelBuffer);
        probe.pixelBuffer = 0;
    }
    probe.framebuffer.reset();
    probe.texture.reset();
}

void BackdropProbe::releaseGl()
{
    m_pollTimer.stop();
    for (auto &[key, probe] : m_probes) {
        releaseGl(probe);
    }
}

void BackdropProbe::capture(const KWin::EffectWindow *w,
                            const QPointF &origin,
                            const QSizeF &scale,
                            const QRect &backgroundRect,
                            KWin::GLFramebuffer *blurred)
{
    const QString key = keyFor(w);
    if (key.isEmpty() || !blurred || !blurred->colorAttachment() || backgroundRect.isEmpty()) {
        return;
    }
    Probe &probe = m_probes[key];
    if (probe.reading || (probe.sinceCapture.isValid() && probe.sinceCapture.elapsed() < kMinCaptureMs)) {
        // One readback in flight, and a capture per interval: read this
        // state later (refreshStale) unless another pass gets there first.
        probe.stale = true;
        if (!m_refreshTimer.isActive()) {
            m_refreshTimer.start();
        }
        return;
    }
    QElapsedTimer cost;
    cost.start();

    // The strip in the blurred texture's pixels (top-left origin, as the
    // blur passes and KWin's blits use it).
    const QSize textureSize = blurred->colorAttachment()->size();
    const qreal fx = qreal(textureSize.width()) / backgroundRect.width();
    const qreal fy = qreal(textureSize.height()) / backgroundRect.height();
    const QRectF strip(origin.x() + probe.rect.x() * scale.width() - backgroundRect.x(),
                       origin.y() + probe.rect.y() * scale.height() - backgroundRect.y(),
                       probe.rect.width() * scale.width(),
                       probe.rect.height() * scale.height());
    const QRectF stripTexels(strip.x() * fx, strip.y() * fy, strip.width() * fx, strip.height() * fy);
    const QRectF covered = stripTexels.intersected(QRectF(QPointF(0, 0), QSizeF(textureSize)));
    if (covered.isEmpty()) {
        return;
    }
    // Only bins wholly on the blurred backdrop are sampled (a strip wider
    // than the window's blur region reads -1 beyond it, not a stretched copy).
    const qreal binWidth = stripTexels.width() / probe.bins;
    const int from = std::clamp(int(std::ceil((covered.left() - stripTexels.left()) / binWidth - 1e-6)), 0, probe.bins);
    const int to = std::clamp(int(std::floor((covered.right() - stripTexels.left()) / binWidth + 1e-6)), 0, probe.bins);
    if (to <= from || !ensureGl(probe)) {
        return;
    }
    const int left = int(std::lround(stripTexels.left() + from * binWidth));
    const int right = std::max(left + 1, int(std::lround(stripTexels.left() + to * binWidth)));
    const int top = int(std::floor(covered.top()));
    const int bottom = std::max(top + 1, int(std::ceil(covered.bottom())));
    const QRect source(left, top, right - left, bottom - top);
    probe.validFrom = from;
    probe.validTo = to;
    probe.stale = false;
    probe.sinceCapture.restart();
    if (!m_loggedFormat) {
        m_loggedFormat = true;
        qCInfo(BACKDROP_PROBE) << LOG_PREFIX << "sampling blurred backdrop, format"
                               << Qt::hex << blurred->colorAttachment()->internalFormat();
    }

    // Scale the strip into bins x 1 (the backdrop is already blurred, so a
    // linear blit is a fair average), then read it into the pixel buffer.
    // The blur passes leave their dirty-region scissor on; it would clip the
    // blit's destination.
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) {
        glDisable(GL_SCISSOR_TEST);
    }
    KWin::GLFramebuffer::pushFramebuffer(blurred);
    probe.framebuffer->blitFromFramebuffer(source, QRect(from, 0, to - from, 1), GL_LINEAR);
    KWin::GLFramebuffer::popFramebuffer();
    if (scissor) {
        glEnable(GL_SCISSOR_TEST);
    }

    KWin::GLFramebuffer::pushFramebuffer(probe.framebuffer.get());
    glBindBuffer(GL_PIXEL_PACK_BUFFER, probe.pixelBuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, probe.bins, 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    KWin::GLFramebuffer::popFramebuffer();
    probe.reading = true; // fenced from poll(), after this frame is submitted
    if (!m_pollTimer.isActive()) {
        m_pollTimer.start();
    }
    qCDebug(BACKDROP_PROBE) << LOG_PREFIX << "capture" << key << "source" << source << "cpu"
                            << cost.nsecsElapsed() / 1000 << "us";
}

void BackdropProbe::poll()
{
    bool pending = false;
    bool current = false;
    for (auto it = m_probes.begin(); it != m_probes.end(); ++it) {
        Probe &probe = it->second;
        if (!probe.reading) {
            continue;
        }
        if (!current) {
            if (!KWin::effects->makeOpenGLContextCurrent()) {
                m_pollTimer.stop();
                return;
            }
            current = true;
        }
        QElapsedTimer cost;
        cost.start();
        if (!probe.fence) {
            // The frame that queued the readback has been submitted by now
            // (painting runs to completion before timers), so this flushes
            // nothing of KWin's.
            probe.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            qCDebug(BACKDROP_PROBE) << LOG_PREFIX << "fence" << it->first << "cpu" << cost.nsecsElapsed() / 1000
                                    << "us";
            pending = true;
            continue;
        }
        const GLenum state = glClientWaitSync(probe.fence, 0, 0);
        if (state == GL_TIMEOUT_EXPIRED) {
            pending = true;
            continue;
        }
        glDeleteSync(probe.fence);
        probe.fence = nullptr;
        probe.reading = false;
        if (state == GL_WAIT_FAILED) {
            continue;
        }
        QList<double> luma;
        luma.reserve(probe.bins);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, probe.pixelBuffer);
        const auto *pixels = static_cast<const uchar *>(
            glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, probe.bins * 4, GL_MAP_READ_BIT));
        if (pixels) {
            for (int i = 0; i < probe.bins; ++i) {
                if (i < probe.validFrom || i >= probe.validTo) {
                    luma.append(-1.0); // off the blurred backdrop
                    continue;
                }
                const uchar *p = pixels + 4 * i;
                luma.append((0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) / 255.0);
            }
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        if (luma.size() != probe.bins) {
            continue;
        }
        const QList<double> &reference = probe.hasLatest ? probe.latest : probe.sent;
        bool moved = reference.size() != luma.size();
        for (int i = 0; !moved && i < luma.size(); ++i) {
            moved = std::abs(luma[i] - reference[i]) > kEpsilon;
        }
        qCDebug(BACKDROP_PROBE) << LOG_PREFIX << "readback" << it->first << "moved" << moved << "cpu"
                                << cost.nsecsElapsed() / 1000 << "us";
        if (moved) {
            probe.latest = luma;
            probe.hasLatest = true;
            flush(it->first, probe);
        }
    }
    if (!pending) {
        m_pollTimer.stop();
    }
}

void BackdropProbe::refreshStale()
{
    bool waiting = false;
    for (auto &[key, probe] : m_probes) {
        if (!probe.stale) {
            continue;
        }
        if (probe.reading) {
            waiting = true; // its readback first
            continue;
        }
        probe.stale = false;
        if (m_refresh) {
            m_refresh(key);
        }
    }
    if (waiting) {
        m_refreshTimer.start();
    }
}

void BackdropProbe::flush(const QString &appId, Probe &probe)
{
    if (!probe.hasLatest) {
        return;
    }
    if (probe.sinceSent.isValid() && probe.sinceSent.elapsed() < kMinIntervalMs) {
        const int wait = kMinIntervalMs - int(probe.sinceSent.elapsed());
        if (!m_sendTimer.isActive() || m_sendTimer.remainingTime() > wait) {
            m_sendTimer.start(wait);
        }
        return;
    }
    probe.sent = probe.latest;
    probe.hasLatest = false;
    probe.sinceSent.restart();
    qCDebug(BACKDROP_PROBE) << LOG_PREFIX << "send" << appId << probe.sent.size() << "bins";
    if (m_emit) {
        m_emit(appId, probe.sent);
    }
}

} // namespace BBDX
