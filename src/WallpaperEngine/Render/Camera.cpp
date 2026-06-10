#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;

Camera::Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera) :
    m_width (0), m_height (0), m_camera (camera), m_scene (scene) {
    // get the lookat position
    // TODO: ENSURE THIS IS ONLY USED WHEN NOT DOING AN ORTOGRAPHIC CAMERA AS IT THROWS OFF POINTS
    this->m_lookat = glm::lookAt (this->getEye (), this->getCenter (), this->getUp ());
}

Camera::~Camera () = default;

const glm::vec3& Camera::getCenter () const {
    return this->m_hasRuntimeCenter ? this->m_runtimeCenter : this->m_camera.configuration.center;
}

const glm::vec3& Camera::getEye () const {
    return this->m_hasRuntimeEye ? this->m_runtimeEye : this->m_camera.configuration.eye;
}

const glm::vec3& Camera::getBaseCenter () const { return this->m_camera.configuration.center; }
const glm::vec3& Camera::getBaseEye () const { return this->m_camera.configuration.eye; }
const glm::vec3& Camera::getBaseUp () const { return this->m_camera.configuration.up; }

const glm::vec3& Camera::getUp () const {
    return this->m_hasRuntimeUp ? this->m_runtimeUp : this->m_camera.configuration.up;
}

void Camera::setEye (const glm::vec3& eye) {
    this->m_runtimeEye = eye;
    this->m_hasRuntimeEye = true;
}

void Camera::setCenter (const glm::vec3& center) {
    this->m_runtimeCenter = center;
    this->m_hasRuntimeCenter = true;
}

void Camera::setUp (const glm::vec3& up) {
    this->m_runtimeUp = up;
    this->m_hasRuntimeUp = true;
}

void Camera::setFov (const float fov) {
    this->m_runtimeFov = fov;
    this->m_hasRuntimeFov = true;
}

const glm::mat4& Camera::getProjection () const { return this->m_projection; }

const glm::mat4& Camera::getLookAt () const { return this->m_lookat; }

bool Camera::isOrthogonal () const { return this->m_isOrthogonal; }

Wallpapers::CScene& Camera::getScene () const { return this->m_scene; }

float Camera::getWidth () const { return this->m_width; }

float Camera::getHeight () const { return this->m_height; }

float Camera::getFov () const {
    return this->m_hasRuntimeFov ? this->m_runtimeFov : this->m_camera.projection.fov->value->getFloat ();
}

float Camera::getNearZ () const { return this->m_camera.projection.nearz->value->getFloat (); }

float Camera::getFarZ () const { return this->m_camera.projection.farz->value->getFloat (); }

void Camera::setOrthogonalProjection (const float width, const float height) {
    this->m_width = width;
    this->m_height = height;

    // The 2D compositor is decoupled from the perspective camera's near/far (which default to WE's
    // 0.1 / 10000 for 3D content). Flat scene layers sit at z=0, so the orthographic near plane must
    // stay at 0 — using the perspective near of 0.1 would clip every z=0 layer (the scene goes blank,
    // leaving only the separately-projected particle systems). The far plane is kept generous.
    const float nearz = 0.0f;
    const float farz = std::max (this->m_camera.projection.farz->value->getFloat (), 1000.0f);

    this->m_projection = glm::ortho<float> (-width / 2.0, width / 2.0, -height / 2.0, height / 2.0, nearz, farz);
    this->m_projection = glm::translate (this->m_projection, this->getEye ());
    this->m_isOrthogonal = true;
}