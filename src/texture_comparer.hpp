#pragma once

#include <opengl/glshadermanager.h>
#include <opengl/glshader.h>
#include <opengl/gltexture.h>

#include <epoxy/gl.h>

#include <memory>

namespace BBDX {

class TextureComparer {
    // compute shader - we need to handle this ourselves :p
    GLuint m_computeShader{0};
    GLint m_dirtyRectLocation{};

    // regular vert+frag so let KWin handle it
    std::unique_ptr<KWin::GLShader> m_glueShader{nullptr};

    // shared buffer for the counter
    GLuint m_counterBuffer{0};

    // the glue query object
    GLuint m_glueQuery{0};

    TextureComparer() = default;

public:
    /**
     * Create a new TextureComparer
     * nullptr on error
     */
    static std::unique_ptr<TextureComparer> create();

    /**
     * Clean up GL resources
     */
    ~TextureComparer();

    /**
     * No copying
     */
    TextureComparer(TextureComparer &other) = delete;
    TextureComparer& operator=(TextureComparer &other) = delete;

    /**
     * Get non-owning copy of the query object ID
     *
     * Should only be used to see the result after compareAndUpdate()
     */
    GLuint queryObject() const {  return m_glueQuery; }

    /**
     * Compare and update cachedBlit with freshBlit
     * within the localDirtyRegion
     *
     * The result of the comparison can be found using the
     * query object returned by queryObject()
     */
    void compareAndUpdate(KWin::GLTexture *freshBlit, KWin::GLTexture *cachedBlit, const KWin::Region &localDirtyRegion) const;
};

}
