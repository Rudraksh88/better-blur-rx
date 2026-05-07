#include "blurconfig.h"
#include "refraction_pass.hpp"
#include "utils.h"

#include <opengl/glshader.h>
#include <opengl/glshadermanager.h>

#include <QLoggingCategory>
#include <QRect>

#include <algorithm>

Q_LOGGING_CATEGORY(REFRACTION_PASS, "kwin_effect_better_blur_dx.refraction_pass", QtInfoMsg)

BBDX::RefractionPass::RefractionPass() {
    // The vertex shaders should always be the one of the
    // respective contrast pass.
    // The refraction uses a modified version of
    // the contrast fragment shader.

    m_shader = KWin::ShaderManager::instance()->generateShaderFromFile(
            KWin::ShaderTrait::MapTexture,
            QStringLiteral(":/effects/better_blur_dx/shaders/vertex.vert"),
            QStringLiteral(":/effects/better_blur_dx/shaders/refraction.frag"));

    if (!m_shader) {
        qCWarning(REFRACTION_PASS) << BBDX::LOG_PREFIX << "Failed to load refraction pass shader";
        return;
    } else {
        // contrast parameters
        m_mvpMatrixLocation = m_shader->uniformLocation("modelViewProjectionMatrix");
        m_colorMatrixLocation = m_shader->uniformLocation("colorMatrix");
        m_offsetLocation = m_shader->uniformLocation("offset");
        m_halfpixelLocation = m_shader->uniformLocation("halfpixel");
        // refraction parameters
        m_refractionRectSizeLocation = m_shader->uniformLocation("refractionRectSize");
        m_refractionEdgeSizePixelsLocation = m_shader->uniformLocation("refractionEdgeSizePixels");
        m_refractionCornerRadiusPixelsLocation = m_shader->uniformLocation("refractionCornerRadiusPixels");
        m_refractionStrengthLocation = m_shader->uniformLocation("refractionStrength");
        m_refractionNormalPowLocation = m_shader->uniformLocation("refractionNormalPow");
        m_refractionRGBFringingLocation = m_shader->uniformLocation("refractionRGBFringing");
        m_refractionTextureRepeatModeLocation = m_shader->uniformLocation("refractionTextureRepeatMode");
        m_refractionModeLocation = m_shader->uniformLocation("refractionMode");
    }
}

void BBDX::RefractionPass::reconfigure() {
    auto config = KWin::BlurConfig::self();

    if (!config) {
        qCWarning(REFRACTION_PASS) << BBDX::LOG_PREFIX
                                   << "RefractionPass::reconfigure() called before BlurConfig::read()";
        return;
    }

    // mark enabled if strength > 0
    m_enabled = config->refractionStrength() > 0;

    // scaled up by 10.0
    constexpr double scaleEdgeSizePixels{10.0};
    m_edgeSizePixels = static_cast<double>(config->refractionEdgeSize()) * scaleEdgeSizePixels;

    // snapped to nearest step
    constexpr double maxCornerRadiusPixels{200.0};
    constexpr double steps{30.0};
    constexpr double stepSize{maxCornerRadiusPixels / steps}; // ~6.6667, matching step size from blur_config.ui
    const double snapped{std::round(static_cast<double>(config->refractionCornerRadius()) / stepSize) * stepSize};
    m_cornerRadiusPixels = snapped;

    // expects range 0.0-1.0
    // max value from blur_config.ui
    constexpr double maxStrength{30.0};
    m_strength = static_cast<double>(config->refractionStrength()) / maxStrength;

    // XXX: why scaled down?
    constexpr double scaleNormalPow{0.5};
    m_normalPow = static_cast<double>(config->refractionNormalPow()) * scaleNormalPow;

    // expects range 0.0-1.0
    // max value from blur_config.ui
    constexpr double maxRGBFringing{30.0};
    m_RGBFringing = static_cast<double>(config->refractionRGBFringing()) / maxRGBFringing;

    // integer mode selectors
    m_textureRepeatMode = config->refractionTextureRepeatMode();
    m_mode = config->refractionMode();
}

bool BBDX::RefractionPass::pushShader() const {
    if (!enabled())
        return false;

    KWin::ShaderManager::instance()->pushShader(m_shader.get());

    return true;
}


bool BBDX::RefractionPass::setParameters(const QMatrix4x4 &projectionMatrix,
                                         const QMatrix4x4 &colorMatrix,
                                         const QVector2D &halfpixel,
                                         const float offset,
                                         const QRect &scaledBackgroundRect) const {
    if (!enabled())
        return false;

    // contrast parameters
    m_shader->setUniform(m_mvpMatrixLocation, projectionMatrix);
    m_shader->setUniform(m_colorMatrixLocation, colorMatrix);
    m_shader->setUniform(m_halfpixelLocation, halfpixel);
    m_shader->setUniform(m_offsetLocation, offset);
    // refraction parameters
    m_shader->setUniform(m_refractionRectSizeLocation,
                                     QVector2D(scaledBackgroundRect.width(), scaledBackgroundRect.height()));
    m_shader->setUniform(m_refractionEdgeSizePixelsLocation,
                                     std::min(static_cast<float>(m_edgeSizePixels),
                                              static_cast<float>(std::min(scaledBackgroundRect.width() / 2,
                                                                          scaledBackgroundRect.height() / 2))));
    m_shader->setUniform(m_refractionCornerRadiusPixelsLocation, static_cast<float>(m_cornerRadiusPixels));
    m_shader->setUniform(m_refractionStrengthLocation, static_cast<float>(m_strength));
    m_shader->setUniform(m_refractionNormalPowLocation, static_cast<float>(m_normalPow));
    m_shader->setUniform(m_refractionRGBFringingLocation, static_cast<float>(m_RGBFringing));
    m_shader->setUniform(m_refractionTextureRepeatModeLocation, m_textureRepeatMode);
    m_shader->setUniform(m_refractionModeLocation, m_mode);

    return true;
}
