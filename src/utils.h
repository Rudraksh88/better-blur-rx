#pragma once

#include "kwin_compat.hpp"

#include <opengl/gltexture.h>
#include <opengl/glframebuffer.h>

#include <epoxy/gl.h>

#include <QMatrix4x4>
#include <QSize>
#include <QString>
#include <QtNumeric>

namespace BBDX
{

static const char LOG_PREFIX[]{"better_blur_dx:"};

/**
 * Build the brightness/contrast/saturation color transformation matrix.
 *
 * Values are expected to be relative to 1.0 (i.e. 1.0 = neutral). Shared by
 * the global blur color path and the per-window color overrides.
 */
inline QMatrix4x4 colorTransformMatrix(qreal saturation, qreal contrast, qreal brightness)
{
    QMatrix4x4 saturationMatrix;
    QMatrix4x4 contrastMatrix;
    QMatrix4x4 brightnessMatrix;

    if (!qFuzzyCompare(saturation, 1.0)) {
        const qreal rval = (1.0 - saturation) * 0.2126;
        const qreal gval = (1.0 - saturation) * 0.7152;
        const qreal bval = (1.0 - saturation) * 0.0722;

        saturationMatrix = QMatrix4x4(rval + saturation, rval, rval, 0.0,
                                      gval, gval + saturation, gval, 0.0,
                                      bval, bval, bval + saturation, 0.0,
                                      0.0, 0.0, 0.0, 1.0);
    }

    if (!qFuzzyCompare(contrast, 1.0)) {
        const float transl = (1.0 - contrast) / 2.0;

        contrastMatrix = QMatrix4x4(contrast, 0.0, 0.0, 0.0,
                                    0.0, contrast, 0.0, 0.0,
                                    0.0, 0.0, contrast, 0.0,
                                    transl, transl, transl, 1.0);
    }

    if (!qFuzzyCompare(brightness, 1.0)) {
        brightnessMatrix.scale(brightness, brightness, brightness);
    }

    return contrastMatrix * saturationMatrix * brightnessMatrix;
}

/**
 * Get texture size for offscreen framebuffer allocation during BlurEffect::blur()
 * Scaled down by 2^i
 *
 * For very small windows, the width and/or height of the last blur texture may be 0. Creation of
 * and/or usage of invalid textures to create framebuffers appears to cause performance issues.
 * https://github.com/taj-ny/kwin-effects-forceblur/issues/160
 */
QSize getTextureSize(const QRect &backgroundRect, const size_t i);

/**
 * When reading textures make the alpha channel a constant 1.
 *
 * At screen edges KWin seems to give us textures where (I'm assuming)
 * all RGBA values are 0.
 * This results in weird blur artifacts around screen edges.
 *
 * This workaround sort of replaces the artifacts with a dark gradient, which
 * technically isn't correct either but better than looking completely broken.
 */
void setTextureSwizzle(KWin::GLTexture *texture);

/**
 * Enable GL_SCISSOR_TEST and set an appropriate
 * scissor rect for the given dirtyRegion, backgroundRect
 *
 * implicitly targets the current attached framebuffer and
 * thus must be called after GLFramebuffer::pushFramebuffer()
 */
void setGLScissor(const KWin::Region &dirtyRegion, const KWin::Rect &backgroundRect);

/**
 * Cleanup for setGLScissor
 *
 * should be cleared right before drawing on the screen
 */
void clearGLScissor();

/**
 * Compatibility helper for loading the proper shader files
 *
 * Plasma <6.7 always needed 2 versions of a shader in the QRC path
 * - "legacy" (no suffix) and "core" (_core suffix)
 * with the "core" version loaded in OpenGL 3.1+ environments
 *
 * Plasma >=6.7 only wants the "core" shader and downgrades it internally
 * by injecting helper funtions
 */
QString shaderFilePath(const char *path);

/**
 * Version agnostic roundedIn/roundedOut helper for RectF
 */
KWin::Rect rectRoundedIn(KWin::RectF rect);
KWin::Rect rectRoundedOut(KWin::RectF rect);

/**
 * Version agnostic helper for KWin::Region(F)::translated()
 */
KWin::RegionF regionTranslatedF(KWin::RegionF region, QPointF translation);

}
