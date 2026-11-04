#pragma once

/**
 * Read-only access to a SurfaceItem's GL texture on KWin >= 6.7.
 *
 * KWin 6.7 moved surface textures behind KWin::Texture / KWin::TextureOpenGL.
 * Neither class is exported from libkwin (no typeinfo, no planes() symbol) and
 * the packaged headers ship only the OpenGL half, so an effect cannot call
 * TextureOpenGL::planes(). The layout is small and stable within a release
 * series, so mirror it and read the plane pointers directly.
 *
 * Layout source: src/scene/texture.h and the installed scene/texture.h
 * (TextureOpenGL) of KWin 6.7.0 and 6.8.2. KWin 6.8 keeps one texture per
 * render device and holds a single GL texture per TextureOpenGL (multi-planar
 * buffers import as one external image) next to the owning EGL context; its
 * installed scene/texture.h is again only the OpenGL half and includes itself
 * in place of the base class. surfaceGLTexture() validates the mirror at
 * runtime by comparing the mirrored size against the SurfaceItem's buffer size
 * (and, on 6.8, the mirrored context against the current one) and returns
 * nullptr on any mismatch, so a layout change degrades to the geometric mask
 * path instead of reading garbage.
 */

#include "kwin_version.hpp"

#include <QSize>
#include <QVarLengthArray>
#include <memory>

namespace KWin {
class GLTexture;
class SurfaceItem;
}

namespace BBDX {

#if !defined(BBDX_X11) && KWIN_VERSION >= KWIN_VERSION_CODE(6, 6, 90)
struct TextureMirror {
    virtual ~TextureMirror() = default;
    QSize m_size;
    bool m_isFloatingPoint = false;
    std::shared_ptr<void> m_releasePoint;
};

#if KWIN_VERSION >= KWIN_VERSION_CODE(6, 7, 80)
struct TextureOpenGLMirror : TextureMirror {
    KWin::GLTexture *m_texture = nullptr; // std::unique_ptr<GLTexture>
    std::shared_ptr<void> m_context;      // std::shared_ptr<EglContext>
};
#else
struct TextureOpenGLMirror : TextureMirror {
    QVarLengthArray<KWin::GLTexture *, 4> m_planes;
};
#endif
#endif

/**
 * The single RGBA GL texture backing a surface item, or nullptr when the
 * surface has no texture yet, is multi-planar (YUV), or the mirror does not
 * validate against this KWin build.
 */
KWin::GLTexture *surfaceGLTexture(const KWin::SurfaceItem *item, const char **reason = nullptr);

/**
 * Attach a surface item's committed buffer to its texture now, the way KWin's
 * item renderer does later in the frame (Item::preprocess()), so that
 * surfaceGLTexture() sees this frame's buffer. On KWin >= 6.8 textures are per
 * render device; returns false (with *reason) when the frame is not being drawn
 * on a device the effect can address, in which case surfaceGLTexture() would
 * fail too.
 */
bool preprocessSurfaceTexture(KWin::SurfaceItem *item, const char **reason = nullptr);

} // namespace BBDX
