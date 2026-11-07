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
// While a readback is in flight, look at its fence this often.
constexpr int kPollMs = 4;

double luma(const uchar *p)
{
    return (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) / 255.0;
}
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
    resize(probe, QSize(bins, 1));
    if (probe.bins != bins || probe.rect != rect || !probe.points.isEmpty()) {
        // A new strip: whatever comes next is news.
        probe.sent.clear();
        probe.hasLatest = false;
    }
    probe.rect = rect;
    probe.bins = bins;
    probe.points.clear();
    probe.gridAt.clear();
    probe.cell = 0.0;
    return true;
}

bool BackdropProbe::setPoints(const QString &appId, const QList<QPointF> &points)
{
    if (appId.isEmpty() || points.isEmpty() || points.size() > kMaxBins) {
        return false;
    }
    double left = points.first().x();
    double top = points.first().y();
    double right = left;
    double bottom = top;
    for (const QPointF &point : points) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            return false;
        }
        left = std::min(left, point.x());
        top = std::min(top, point.y());
        right = std::max(right, point.x());
        bottom = std::max(bottom, point.y());
    }
    const QRectF bounds(left, top, right - left, bottom - top);
    // Texels on a lattice from the top-left point, kPointCell apart or more
    // so the grid stays within kMaxGridTexels.
    double cell = std::max(kPointCell, std::sqrt(bounds.width() * bounds.height() / kMaxGridTexels));
    QSize grid;
    for (;;) {
        grid = QSize(int(std::ceil(bounds.width() / cell - 1e-9)) + 1, int(std::ceil(bounds.height() / cell - 1e-9)) + 1);
        if (qint64(grid.width()) * grid.height() <= kMaxGridTexels) {
            break;
        }
        cell *= 1.25;
    }
    Probe &probe = m_probes[appId];
    resize(probe, grid);
    if (probe.points != points) {
        probe.sent.clear();
        probe.hasLatest = false;
    }
    probe.rect = bounds;
    probe.bins = int(points.size());
    probe.points = points;
    probe.cell = cell;
    probe.gridAt.clear();
    return true;
}

void BackdropProbe::resize(Probe &probe, const QSize &grid)
{
    if (probe.grid != grid && (probe.texture || probe.reading)) {
        if (KWin::effects->makeOpenGLContextCurrent()) {
            releaseGl(probe);
        }
    }
    probe.grid = grid;
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
        QString line = QStringLiteral("%1 %2 %3 %4 %5 %6")
                           .arg(it->first)
                           .arg(it->second.rect.x())
                           .arg(it->second.rect.y())
                           .arg(it->second.rect.width())
                           .arg(it->second.rect.height())
                           .arg(it->second.bins);
        if (!it->second.points.isEmpty()) {
            line += QStringLiteral(" points");
        }
        lines.append(line);
    }
    return lines;
}

QString BackdropProbe::keyFor(const KWin::EffectWindow *w) const
{
    const QString windowClass = w->window()->resourceClass();
    if (const QString suffix = surfaceTypeSuffix(w); !suffix.isEmpty()) {
        const QString typedKey = windowClass + suffix;
        if (m_probes.contains(typedKey)) {
            return typedKey;
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
    if (probe.grid.isEmpty()) {
        return false;
    }
    probe.texture = KWin::GLTexture::allocate(GL_RGBA8, probe.grid);
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
    glBufferData(GL_PIXEL_PACK_BUFFER, probe.grid.width() * probe.grid.height() * 4, nullptr, GL_STREAM_READ);
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
    const QSize textureSize = blurred->colorAttachment()->size();

    QRect destination;
    const QRect source = probe.points.isEmpty()
        ? stripSource(probe, origin, scale, backgroundRect, textureSize, destination)
        : pointSource(probe, origin, scale, backgroundRect, textureSize, destination);
    if (source.isEmpty() || !ensureGl(probe)) {
        return;
    }
    probe.stale = false;
    probe.sinceCapture.restart();
    if (!m_loggedFormat) {
        m_loggedFormat = true;
        qCInfo(BACKDROP_PROBE) << LOG_PREFIX << "sampling blurred backdrop, format"
                               << Qt::hex << blurred->colorAttachment()->internalFormat();
    }

    // Scale the strip into bins x 1, or the points' box into their grid (the
    // backdrop is already blurred, so a linear blit is a fair average), then
    // read it into the pixel buffer.
    // The blur passes leave their dirty-region scissor on; it would clip the
    // blit's destination.
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) {
        glDisable(GL_SCISSOR_TEST);
    }
    KWin::GLFramebuffer::pushFramebuffer(blurred);
    probe.framebuffer->blitFromFramebuffer(source, destination, GL_LINEAR);
    KWin::GLFramebuffer::popFramebuffer();
    if (scissor) {
        glEnable(GL_SCISSOR_TEST);
    }

    KWin::GLFramebuffer::pushFramebuffer(probe.framebuffer.get());
    glBindBuffer(GL_PIXEL_PACK_BUFFER, probe.pixelBuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, probe.grid.width(), probe.grid.height(), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    KWin::GLFramebuffer::popFramebuffer();
    probe.reading = true; // fenced from poll(), after this frame is submitted
    if (!m_pollTimer.isActive()) {
        m_pollTimer.start();
    }
    qCDebug(BACKDROP_PROBE) << LOG_PREFIX << "capture" << key << "source" << source << "cpu"
                            << cost.nsecsElapsed() / 1000 << "us";
}

QRect BackdropProbe::stripSource(Probe &probe,
                                 const QPointF &origin,
                                 const QSizeF &scale,
                                 const QRect &backgroundRect,
                                 const QSize &textureSize,
                                 QRect &destination)
{
    // The strip in the blurred texture's pixels (top-left origin, as the
    // blur passes and KWin's blits use it).
    const qreal fx = qreal(textureSize.width()) / backgroundRect.width();
    const qreal fy = qreal(textureSize.height()) / backgroundRect.height();
    const QRectF strip(origin.x() + probe.rect.x() * scale.width() - backgroundRect.x(),
                       origin.y() + probe.rect.y() * scale.height() - backgroundRect.y(),
                       probe.rect.width() * scale.width(),
                       probe.rect.height() * scale.height());
    const QRectF stripTexels(strip.x() * fx, strip.y() * fy, strip.width() * fx, strip.height() * fy);
    const QRectF covered = stripTexels.intersected(QRectF(QPointF(0, 0), QSizeF(textureSize)));
    if (covered.isEmpty()) {
        return QRect();
    }
    // Only bins wholly on the blurred backdrop are sampled (a strip wider
    // than the window's blur region reads -1 beyond it, not a stretched copy).
    const qreal binWidth = stripTexels.width() / probe.bins;
    const int from = std::clamp(int(std::ceil((covered.left() - stripTexels.left()) / binWidth - 1e-6)), 0, probe.bins);
    const int to = std::clamp(int(std::floor((covered.right() - stripTexels.left()) / binWidth + 1e-6)), 0, probe.bins);
    if (to <= from) {
        return QRect();
    }
    const int left = int(std::lround(stripTexels.left() + from * binWidth));
    const int right = std::max(left + 1, int(std::lround(stripTexels.left() + to * binWidth)));
    const int top = int(std::floor(covered.top()));
    const int bottom = std::max(top + 1, int(std::ceil(covered.bottom())));
    destination = QRect(from, 0, to - from, 1);
    probe.validFrom = from;
    probe.validTo = to;
    return QRect(left, top, right - left, bottom - top);
}

QRect BackdropProbe::pointSource(Probe &probe,
                                 const QPointF &origin,
                                 const QSizeF &scale,
                                 const QRect &backgroundRect,
                                 const QSize &textureSize,
                                 QRect &destination)
{
    // Surface-local logical px to the blurred texture's pixels (top-left
    // origin), as for a strip.
    const qreal fx = qreal(textureSize.width()) / backgroundRect.width();
    const qreal fy = qreal(textureSize.height()) / backgroundRect.height();
    const auto texel = [&](const QPointF &p) {
        return QPointF((origin.x() + p.x() * scale.width() - backgroundRect.x()) * fx,
                       (origin.y() + p.y() * scale.height() - backgroundRect.y()) * fy);
    };
    // The grid's texel centres lie on the lattice from the bounding box's
    // top-left point, `cell` apart: its source spans half a cell beyond the
    // outer lattice points, on whole texels and on the texture.
    const QPointF topLeft = texel(probe.rect.topLeft());
    const qreal cellX = probe.cell * scale.width() * fx;
    const qreal cellY = probe.cell * scale.height() * fy;
    const QRectF wanted(topLeft.x() - cellX / 2, topLeft.y() - cellY / 2,
                        probe.grid.width() * cellX, probe.grid.height() * cellY);
    const int left = std::max(0, int(std::floor(wanted.left())));
    const int top = std::max(0, int(std::floor(wanted.top())));
    const int right = std::min(textureSize.width(), int(std::ceil(wanted.right())));
    const int bottom = std::min(textureSize.height(), int(std::ceil(wanted.bottom())));
    if (right <= left || bottom <= top) {
        return QRect();
    }
    const QRect source(left, top, right - left, bottom - top);
    // Where each point falls in the grid as that source is actually scaled
    // into it; a point off the source is off the blurred backdrop.
    const qreal toGridX = qreal(probe.grid.width()) / source.width();
    const qreal toGridY = qreal(probe.grid.height()) / source.height();
    probe.gridAt.clear();
    probe.gridAt.reserve(probe.points.size());
    for (const QPointF &point : std::as_const(probe.points)) {
        const QPointF t = texel(point);
        if (t.x() < source.left() || t.x() > source.left() + source.width() || t.y() < source.top()
            || t.y() > source.top() + source.height()) {
            probe.gridAt.emplace_back(qQNaN(), qQNaN());
            continue;
        }
        probe.gridAt.emplace_back((t.x() - source.left()) * toGridX - 0.5, (t.y() - source.top()) * toGridY - 0.5);
    }
    destination = QRect(QPoint(0, 0), probe.grid);
    return source;
}

QList<double> BackdropProbe::pointLuma(const Probe &probe, const uchar *pixels) const
{
    // Rows come back bottom-up: the blit keeps the source's top-left
    // orientation, glReadPixels starts at GL's row 0, the grid's last row.
    const int width = probe.grid.width();
    const int height = probe.grid.height();
    const auto at = [&](int x, int y) {
        return luma(pixels + 4 * (qsizetype(height - 1 - y) * width + x));
    };
    QList<double> values;
    values.reserve(probe.bins);
    for (const QPointF &g : probe.gridAt) {
        if (!std::isfinite(g.x()) || !std::isfinite(g.y())) {
            values.append(-1.0);
            continue;
        }
        const double u = std::clamp(g.x(), 0.0, double(width - 1));
        const double v = std::clamp(g.y(), 0.0, double(height - 1));
        const int x0 = int(u);
        const int y0 = int(v);
        const int x1 = std::min(x0 + 1, width - 1);
        const int y1 = std::min(y0 + 1, height - 1);
        const double tx = u - x0;
        const double ty = v - y0;
        const double upper = at(x0, y0) * (1 - tx) + at(x1, y0) * tx;
        const double lower = at(x0, y1) * (1 - tx) + at(x1, y1) * tx;
        values.append(upper * (1 - ty) + lower * ty);
    }
    return values;
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
        const auto *pixels = static_cast<const uchar *>(glMapBufferRange(
            GL_PIXEL_PACK_BUFFER, 0, probe.grid.width() * probe.grid.height() * 4, GL_MAP_READ_BIT));
        if (pixels && !probe.points.isEmpty()) {
            if (probe.gridAt.size() == size_t(probe.bins)) {
                luma = pointLuma(probe, pixels);
            }
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        } else if (pixels) {
            for (int i = 0; i < probe.bins; ++i) {
                if (i < probe.validFrom || i >= probe.validTo) {
                    luma.append(-1.0); // off the blurred backdrop
                    continue;
                }
                luma.append(::luma(pixels + 4 * i));
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
