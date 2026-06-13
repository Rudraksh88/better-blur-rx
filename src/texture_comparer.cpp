#include "texture_comparer.hpp"

#include <epoxy/gl_generated.h>
#include <opengl/glshadermanager.h>
#include <opengl/glshader.h>
#include <opengl/gltexture.h>

#include <QLoggingCategory>
#include <QFile>

#include <epoxy/gl.h>

#include <memory>

Q_LOGGING_CATEGORY(BBDX_TEXTURE_COMPARER, "kwin_effect_better_blur_dx.texture_comparer", QtInfoMsg)

std::unique_ptr<BBDX::TextureComparer> BBDX::TextureComparer::create() {
    auto textureComparer = std::unique_ptr<TextureComparer>(new TextureComparer());

    // for vert+frag just use the KWin helpers
    textureComparer->m_glueShader = KWin::ShaderManager::instance()->generateShaderFromFile(KWin::ShaderTraits{},
                                                                           QStringLiteral(":/effects/better_blur_dx/shaders/texture_compare_and_update.vert"),
                                                                           QStringLiteral(":/effects/better_blur_dx/shaders/texture_compare_and_update.frag"));
    if (!textureComparer->m_glueShader) {
        qCWarning(BBDX_TEXTURE_COMPARER) << "Failed to load texture compare glue shaders";
        return nullptr;
    }

    // we need to handle the compute shader manually
    QFile shaderFile{QStringLiteral(":/effects/better_blur_dx/shaders/texture_compare_and_update.comp")};
    if (!shaderFile.open(QIODevice::ReadOnly)) {
        qCWarning(BBDX_TEXTURE_COMPARER) << "Failed to open tecture compare compute shader";
        return nullptr;
    }
    QByteArray shaderSource = shaderFile.readAll();

    // this process roughly mirrors what KWin does in
    // GLShader::compile()
    // minus all the preprocessing magic which is vert/frag specific

    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);

    const GLchar *src = shaderSource.constData();
    const GLint srcLength = shaderSource.length();
    glShaderSource(shader, 1, &src, &srcLength);

    // compile
    glCompileShader(shader);

    // configure log buffer
    GLint maxLength, length;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &maxLength);
    QByteArray log(maxLength, 0);
    glGetShaderInfoLog(shader, maxLength, &length, log.data());

    // compile status
    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

    if (status == 0) {
        qCCritical(BBDX_TEXTURE_COMPARER) << "Compute shader compilation failed:\n" << log;
        glDeleteShader(shader);
        return nullptr;
    } else {
        qCDebug(BBDX_TEXTURE_COMPARER) << "Compute shader compilation log:\n" << log;
    }

    textureComparer->m_computeShader = glCreateProgram();
    glAttachShader(textureComparer->m_computeShader, shader);
    glDeleteShader(shader);

    // now link the program similar to GLShader::link()
    glLinkProgram(textureComparer->m_computeShader);

    // reconfigure log buffer
    glGetProgramiv(textureComparer->m_computeShader, GL_INFO_LOG_LENGTH, &maxLength);
    log = QByteArray{maxLength, 0};
    glGetProgramInfoLog(textureComparer->m_computeShader, maxLength, &length, log.data());

    // link status
    glGetProgramiv(textureComparer->m_computeShader, GL_LINK_STATUS, &status);

    if (status == 0) {
        qCCritical(BBDX_TEXTURE_COMPARER) << "Compute shader linking failed:\n" << log;
        // ~TextureComparer() handles glDeleteProgram
        return nullptr;
    } else {
        qCDebug(BBDX_TEXTURE_COMPARER) << "Compute shader linking log:\n" << log;
    }

    // store locations of uniform params
    textureComparer->m_dirtyRectLocation = glGetUniformLocation(textureComparer->m_computeShader, "u_dirtyRect");

    // supplementary resources
    glGenBuffers(1, &textureComparer->m_counterBuffer);

    // allocate a single GLuint (the change counter)
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, textureComparer->m_counterBuffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glGenQueries(1, &textureComparer->m_glueQuery);

    return textureComparer;
}

BBDX::TextureComparer::~TextureComparer() {
    if (m_computeShader > 0) {
        glDeleteProgram(m_computeShader);
        m_computeShader = 0;
    }

    if (m_counterBuffer > 0) {
        glDeleteBuffers(1, &m_counterBuffer);
    }

    if (m_glueQuery > 0) {
        glDeleteQueries(1, &m_glueQuery);
    }
}

void BBDX::TextureComparer::compareAndUpdate(KWin::GLTexture *freshBlit, KWin::GLTexture *cachedBlit, const KWin::Region &localDirtyRegion) const {
    // bind the textures
    // TODO: colorspace might be different
    glBindImageTexture(0, freshBlit->texture(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(1, cachedBlit->texture(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);

    // reset and bind counter
    const GLuint zero = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_counterBuffer);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GLuint), &zero);
    // slot 2 - matching compute shader
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_counterBuffer);

    // prepare compute shader
    glUseProgram(m_computeShader);

    for (const auto &rect : localDirtyRegion.rects()) {
        // bind dirtyRect
        glUniform4i(m_dirtyRectLocation, rect.x(), rect.y(), rect.width(), rect.height());

        // dispatch in 16x16 workgroup blocks (ceiled, matching compute shader params)
        glDispatchCompute((rect.width() + 15) / 16, (rect.height() + 15) / 16, 1);
    }

    // wait for compute to be done, then fire the query
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

    KWin::ShaderManager::instance()->pushShader(m_glueShader.get()); 

    // "draw" a single point to check the result of
    // the compute shader
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);

    glBeginQuery(GL_ANY_SAMPLES_PASSED, m_glueQuery);
    glDrawArrays(GL_POINTS, 0, 1);
    glEndQuery(GL_ANY_SAMPLES_PASSED);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);

    KWin::ShaderManager::instance()->popShader();
}
