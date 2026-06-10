#pragma once

#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "WallpaperEngine/Data/Model/Wallpaper.h"

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render {
using namespace WallpaperEngine::Data::Model;

class Camera {
public:
    Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera);
    ~Camera ();

    void setOrthogonalProjection (const float width, const float height);

    // Runtime camera control for scripts (thisScene.setCameraTransforms). CModel re-reads
    // getEye/getCenter/getUp/getFov every frame so it follows these; 2D layers use the
    // orthographic getLookAt()/getProjection(), which these do NOT touch.
    void setEye (const glm::vec3& eye);
    void setCenter (const glm::vec3& center);
    void setUp (const glm::vec3& up);
    void setFov (float fov);

    [[nodiscard]] const glm::vec3& getCenter () const;
    [[nodiscard]] const glm::vec3& getEye () const;
    [[nodiscard]] const glm::vec3& getUp () const;
    // The scene's authored camera, ignoring any runtime script override. Camera-controller scripts
    // (thisScene.getCameraTransforms) must read this stable base each frame and recompute from it;
    // returning the already-overridden value feeds the script its own output and drifts the camera away.
    [[nodiscard]] const glm::vec3& getBaseCenter () const;
    [[nodiscard]] const glm::vec3& getBaseEye () const;
    [[nodiscard]] const glm::vec3& getBaseUp () const;
    [[nodiscard]] const glm::mat4& getProjection () const;
    [[nodiscard]] const glm::mat4& getLookAt () const;
    [[nodiscard]] Wallpapers::CScene& getScene () const;
    [[nodiscard]] bool isOrthogonal () const;
    [[nodiscard]] float getWidth () const;
    [[nodiscard]] float getHeight () const;
    [[nodiscard]] float getFov () const;
    [[nodiscard]] float getNearZ () const;
    [[nodiscard]] float getFarZ () const;

private:
    float m_width;
    float m_height;
    bool m_isOrthogonal = false;
    glm::mat4 m_projection = {};
    glm::mat4 m_lookat = {};
    const SceneData::Camera& m_camera;
    Wallpapers::CScene& m_scene;

    // Script-driven runtime overrides (fall back to the scene data until a script sets them).
    glm::vec3 m_runtimeEye = {};
    glm::vec3 m_runtimeCenter = {};
    glm::vec3 m_runtimeUp = {};
    float m_runtimeFov = 0.0f;
    bool m_hasRuntimeEye = false;
    bool m_hasRuntimeCenter = false;
    bool m_hasRuntimeUp = false;
    bool m_hasRuntimeFov = false;
};
} // namespace WallpaperEngine::Render
