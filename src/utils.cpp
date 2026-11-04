#include "utils.h"

#include "kwin_compat.hpp"
#include "kwin_texture_mirror.hpp"

#include <scene/surfaceitem.h>

#if !defined(BBDX_X11) && KWIN_VERSION >= KWIN_VERSION_CODE(6, 7, 80)
#include <compositor.h>
#include <core/renderdevice.h>
#include <effect/effecthandler.h>
#include <opengl/eglcontext.h>
#include <scene/itemrenderer.h>
#include <scene/workspacescene.h>
#endif

#include <opengl/gltexture.h>
#include <opengl/glframebuffer.h>

#include <epoxy/gl.h>

#include <QSize>
#include <QLoggingCategory>
#include <qloggingcategory.h>

Q_LOGGING_CATEGORY(BBDX_UTILS, "kwin_effect_better_blur_dx.utils", QtInfoMsg)

QSize BBDX::getTextureSize(const QRect &backgroundRect, const size_t i) {
    return QSize(std::max(1, backgroundRect.width() / (1 << i)),
                 std::max(1, backgroundRect.height() / (1 << i)));
}

void BBDX::setTextureSwizzle(KWin::GLTexture *texture) {
    texture->setSwizzle(GL_RED, GL_GREEN, GL_BLUE, GL_ONE);
}

void BBDX::setGLScissor(const KWin::Region &dirtyRegion, const KWin::Rect &backgroundRect) {
    const auto fbo = KWin::GLFramebuffer::currentFramebuffer();
    if (!fbo) {
        qCWarning(BBDX_UTILS) << "BBDX::setGLScissor() called with no GLFramebuffer attached";
        return;
    }

    const auto texture = fbo->colorAttachment();

    const double scaleX{static_cast<double>(texture->width()) / static_cast<double>(backgroundRect.width())};
    const double scaleY{static_cast<double>(texture->height()) / static_cast<double>(backgroundRect.height())};

    // slight expansion to ensure we don't cut of edges
    const KWin::RectF boundingRect{dirtyRegion.translated(-backgroundRect.topLeft()).boundingRect().adjusted(-8, -8, 8, 8)};

    const double scaledLeft{std::max(0.0, std::floor(boundingRect.left() * scaleX))};
    const double scaledTop{std::max(0.0, std::floor(boundingRect.top() * scaleY))};
    const double scaledRight{std::min(static_cast<double>(texture->width()), std::ceil(boundingRect.right() * scaleX))};
    const double scaledBottom{std::min(static_cast<double>(texture->height()), std::ceil(boundingRect.bottom() * scaleY))};

    const int glWidth{std::max(0, static_cast<int>(scaledRight - scaledLeft))};
    const int glHeight{std::max(0, static_cast<int>(scaledBottom - scaledTop))};

    const int glX{static_cast<int>(scaledLeft)};
    const int glY{static_cast<int>(texture->height() - (scaledTop + glHeight))};

    glEnable(GL_SCISSOR_TEST);
    glScissor(glX, glY, glWidth, glHeight);
}

void BBDX::clearGLScissor() {
    glScissor(0, 0, 0, 0);
    glDisable(GL_SCISSOR_TEST);
}

QString BBDX::shaderFilePath(const char *path) {
    QString shader{path};

#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 6, 90)
    shader.insert(shader.lastIndexOf("."), "_core");
#endif

    qCDebug(BBDX_UTILS) << "Loading shader file:" << shader;
    return shader;
}

KWin::Rect BBDX::rectRoundedIn(KWin::RectF rect) {
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    return KWin::Rect(QPoint(std::ceil(rect.left()), std::ceil(rect.top())),
                      QPoint(std::floor(rect.right()), std::floor(rect.bottom())));
#else
    return rect.roundedIn();
#endif
}

KWin::Rect BBDX::rectRoundedOut(KWin::RectF rect) {
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    return KWin::Rect(QPoint(std::floor(rect.left()), std::floor(rect.top())),
                      QPoint(std::ceil(rect.right()), std::ceil(rect.bottom())));
#else
    return rect.roundedIn();
#endif
}

KWin::RegionF BBDX::regionTranslatedF(KWin::RegionF region, QPointF translation) {
#if KWIN_VERSION < KWIN_VERSION_CODE(6, 6, 90)
    // < 6.7 maps RegionF to integral (Q)Region
    return region.translated(translation.toPoint());
#else
    return region.translated(translation);
#endif
}


// ---- kwin_texture_mirror.hpp

#if !defined(BBDX_X11) && KWIN_VERSION >= KWIN_VERSION_CODE(6, 7, 80)
namespace {
/**
 * The render device whose EGL context is drawing the current frame, as far as
 * an effect can tell. KWin 6.8 keeps one texture per render device but gives
 * effects no handle on the device of the view being painted (the scene's
 * painted delegate is private). In-tree effects use the primary device; do the
 * same, and only when its context is the current one, so an output driven by a
 * secondary GPU falls back to the geometric mask instead of sampling a texture
 * that belongs to another context.
 */
KWin::RenderDevice *currentRenderDevice(const char **reason) {
    const auto fail = [reason](const char *why) -> KWin::RenderDevice * {
        if (reason) {
            *reason = why;
        }
        return nullptr;
    };
    KWin::Compositor *compositor = KWin::Compositor::self();
    KWin::RenderDevice *device = compositor ? compositor->primaryDevice() : nullptr;
    if (!device) {
        return fail("no primary render device");
    }
    const std::shared_ptr<KWin::EglContext> context = device->eglContext();
    if (!context || context.get() != KWin::EglContext::currentContext()) {
        return fail("frame not drawn on the primary render device");
    }
    return device;
}
} // namespace
#endif

#if !defined(BBDX_X11) && KWIN_VERSION >= KWIN_VERSION_CODE(6, 6, 90)
KWin::GLTexture *BBDX::surfaceGLTexture(const KWin::SurfaceItem *item, const char **reason) {
    const auto fail = [reason](const char *why) -> KWin::GLTexture * {
        if (reason) {
            *reason = why;
        }
        return nullptr;
    };
    if (!item) {
        return fail("no surface item");
    }
    // KWin::Texture is only forward-declared in the packaged headers; the
    // mirror replicates its layout (see kwin_texture_mirror.hpp).
#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 7, 80)
    KWin::RenderDevice *device = currentRenderDevice(reason);
    if (!device) {
        return nullptr;
    }
    const auto *mirror = reinterpret_cast<const BBDX::TextureOpenGLMirror *>(item->texture(device));
#else
    const auto *mirror = reinterpret_cast<const BBDX::TextureOpenGLMirror *>(item->texture());
#endif
    if (!mirror) {
        return fail("surface has no texture yet");
    }
    const QSize bufferSize = item->bufferSize();
    if (bufferSize.isEmpty()) {
        return fail("empty buffer");
    }
    if (mirror->m_size.isEmpty()) {
        return fail("texture not attached yet");
    }
    if (mirror->m_size != bufferSize) {
        // Layout mismatch or a texture that doesn't describe this buffer:
        // never trust the plane pointers.
        return fail("texture size != buffer size (mirror mismatch?)");
    }
#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 7, 80)
    if (mirror->m_context.get() != static_cast<void *>(KWin::EglContext::currentContext())) {
        // The texture was created for the current device's context; anything
        // else means the mirrored layout is off.
        return fail("texture context != current context (mirror mismatch?)");
    }
    KWin::GLTexture *texture = mirror->m_texture;
    if (!texture) {
        return fail("texture has no GL texture");
    }
#else
    if (mirror->m_planes.size() != 1 || !mirror->m_planes[0]) {
        // Multi-planar (YUV) content has no usable alpha.
        return fail("not a single-plane texture (YUV?)");
    }
    KWin::GLTexture *texture = mirror->m_planes[0];
#endif
    if (texture->size() != bufferSize) {
        return fail("GL texture size != buffer size");
    }
    return texture;
}
#else
KWin::GLTexture *BBDX::surfaceGLTexture(const KWin::SurfaceItem *, const char **reason) {
    if (reason) {
        *reason = "alpha mask needs KWin >= 6.7";
    }
    return nullptr;
}
#endif

bool BBDX::preprocessSurfaceTexture(KWin::SurfaceItem *item, const char **reason) {
    if (!item) {
        if (reason) {
            *reason = "no surface item";
        }
        return false;
    }
    // Item's override is public; SurfaceItem only narrows its visibility.
#if !defined(BBDX_X11) && KWIN_VERSION >= KWIN_VERSION_CODE(6, 7, 80)
    KWin::RenderDevice *device = currentRenderDevice(reason);
    KWin::ItemRenderer *renderer = device && KWin::effects && KWin::effects->scene()
        ? static_cast<KWin::Scene *>(KWin::effects->scene())->renderer(device)
        : nullptr;
    if (!renderer) {
        if (reason && device) {
            *reason = "no item renderer for the primary render device";
        }
        return false;
    }
    static_cast<KWin::Item *>(item)->preprocess(renderer);
#else
    static_cast<KWin::Item *>(item)->preprocess();
#endif
    return true;
}
