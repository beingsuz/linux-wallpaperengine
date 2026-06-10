#pragma once

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

#include "WallpaperEngine/Render/FBOProvider.h"
#include "WallpaperEngine/Render/Objects/CRenderable.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include "WallpaperEngine/Data/Model/Object.h"

namespace WallpaperEngine::Render::Objects {
/**
 * Renders a Wallpaper Engine 3D model object (top-level "model" -> .mdl mesh).
 *
 * This runs the wallpaper's OWN material pipeline with no hand-written shading: one
 * Effects::CPass per mesh, compiled from the mesh's real material (generic3) with its real
 * combos / constants / textures. Lighting, rim (RIMLIGHTING), toon ramp (SHADINGGRADIENT),
 * reflection (REFLECTION), colors, alpha and blend/depth/cull all come straight from the
 * wallpaper's material JSON exactly as Wallpaper Engine declares them — a 1:1 port of WE's
 * generic3 path. The scene's ambient/skylight, brightness and reflection buffer are supplied
 * by CPass automatically (same as 2D image layers); the model only adds the camera eye
 * position that generic3.vert needs for its view vector.
 *
 * The 2D compositor is orthographic, so the model uses a perspective camera (from the scene
 * camera) + a depth buffer attached to the scene framebuffer for sub-mesh occlusion (attached
 * only for this object's draw; the 2D layers use no depth).
 */
class CModel final : public CRenderable {
    friend CObject;

public:
    CModel (Wallpapers::CScene& scene, const WallpaperEngine::Data::Model::ModelObject& model);
    ~CModel () override;

    void setup () override;
    void render () override;

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;

private:
    struct Mesh {
	GLuint vao = GL_NONE;
	GLuint vbo = GL_NONE;
	GLuint ebo = GL_NONE;
	GLsizei indexCount = 0;
	Effects::CPass* pass = nullptr;
    };

    void setupMesh (const WallpaperEngine::Data::Model::ModelMesh& source);
    void ensureDepthBuffer (uint32_t width, uint32_t height);
    [[nodiscard]] glm::mat4 computeModelMatrix () const;

    const WallpaperEngine::Data::Model::ModelObject& m_model;
    std::vector<Mesh> m_meshes = {};
    std::shared_ptr<FBOProvider> m_fboProvider = nullptr;
    std::shared_ptr<CFBO> m_reflectionFBO = nullptr;

    // Per-frame state, fed to the passes by pointer.
    glm::mat4 m_modelMatrix = glm::mat4 (1.0f);
    glm::mat4 m_viewProjectionMatrix = glm::mat4 (1.0f);
    glm::mat4 m_mvpMatrix = glm::mat4 (1.0f);
    glm::mat4 m_mvpMatrixInverse = glm::mat4 (1.0f);
    glm::vec3 m_eyePosition = glm::vec3 (0.0f);

    // Neutral CRenderable values (material constants drive g_TintColor/g_TintAlpha; these
    // identity values feed g_Brightness/g_Alpha/g_Color without adding styling).
    float m_brightness = 1.0f;
    float m_alpha = 1.0f;
    glm::vec3 m_color = glm::vec3 (1.0f);
    glm::vec4 m_color4 = glm::vec4 (1.0f);

    GLint m_prevVao = 0;
    GLuint m_depthRenderbuffer = GL_NONE;
    uint32_t m_depthWidth = 0;
    uint32_t m_depthHeight = 0;
    bool m_initialized = false;
};
} // namespace WallpaperEngine::Render::Objects
