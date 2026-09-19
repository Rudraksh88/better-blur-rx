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
 * (TextureOpenGL) of KWin 6.7.0. surfaceGLTexture() validates the mirror at
 * runtime by comparing the mirrored size against the SurfaceItem's buffer size
 * and returns nullptr on any mismatch, so a layout change degrades to the
 * geometric mask path instead of reading garbage.
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

struct TextureOpenGLMirror : TextureMirror {
    QVarLengthArray<KWin::GLTexture *, 4> m_planes;
};
#endif

/**
 * The single RGBA GL texture backing a surface item, or nullptr when the
 * surface has no texture yet, is multi-planar (YUV), or the mirror does not
 * validate against this KWin build.
 */
KWin::GLTexture *surfaceGLTexture(const KWin::SurfaceItem *item, const char **reason = nullptr);

} // namespace BBDX
