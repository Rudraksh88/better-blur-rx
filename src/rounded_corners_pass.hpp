#pragma once

#include "kwin_compat.hpp"

#include <opengl/glshader.h>

#include <QMatrix4x4>
#include <QVector2D>
#include <QVector4D>
#include <QtNumeric>

#include <opengl/glvertexbuffer.h>

#include <memory>

namespace KWin {
    class BorderRadius;
    class EffectWindow;
    class GLVertexBuffer;
    class RenderViewport;
    class WindowPaintData;
}

namespace BBDX {
    class BlurCache;
}

namespace BBDX {
struct BlurRenderData;

class RoundedCornersPass {
private:
    std::unique_ptr<KWin::GLShader> m_shader{nullptr};
    int m_mvpMatrixLocation;
    int m_boxLocation;
    int m_cornerRadiusLocation;
    int m_squircleLocation;
    int m_modulationLocation;

    RoundedCornersPass() = default;

public:
    /**
     * Loads required shaders and sets up shader uniformLocations
     * nullptr on error
     */
    static std::unique_ptr<RoundedCornersPass> create();

    /**
     * Apply rounded corners with a mask from renderInfo.framebuffer[0]
     * which should contain the raw un-blurred pixels of the blur area.
     */
    void apply(const KWin::BorderRadius &cornerRadius,
               bool squircle,
               const KWin::Rect &backgroundRect,
               BBDX::BlurRenderData &renderInfo,
               const KWin::EffectWindow *w,
               const KWin::WindowPaintData &data,
               KWin::GLVertexBuffer *vbo,
               const BBDX::BlurCache *blurCache) const;
};

} // namespace BBDX
