#include "CModel.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "WallpaperEngine/Render/Camera.h"

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using namespace WallpaperEngine::Data::Model;

namespace {
// .mdl vertex layout: 48-byte stride (pos[3] normal[3] tangent[4] uv[2]).
constexpr GLsizei MDL_VERTEX_STRIDE = 48;
constexpr uintptr_t MDL_POSITION_OFFSET = 0;
constexpr uintptr_t MDL_NORMAL_OFFSET = 12;
constexpr uintptr_t MDL_TANGENT_OFFSET = 24;
constexpr uintptr_t MDL_UV_OFFSET = 40;

// Linearly interpolate a keyframe channel at the given (already wrapped) frame.
float evalChannel (const AnglesAnimation::Channel& ch, const float frame) {
    if (ch.keys.empty ()) {
	return 0.0f;
    }
    if (frame <= ch.keys.front ().first) {
	return ch.keys.front ().second;
    }
    if (frame >= ch.keys.back ().first) {
	return ch.keys.back ().second;
    }
    for (size_t i = 1; i < ch.keys.size (); ++i) {
	if (frame <= ch.keys[i].first) {
	    const auto& [aFrame, aValue] = ch.keys[i - 1];
	    const auto& [bFrame, bValue] = ch.keys[i];
	    const float span = bFrame - aFrame;
	    const float t = span > 0.0f ? (frame - aFrame) / span : 0.0f;
	    return aValue + (bValue - aValue) * t;
	}
    }
    return ch.keys.back ().second;
}
} // namespace

CModel::CModel (Wallpapers::CScene& scene, const ModelObject& model) :
    CObject (scene, model), CRenderable (scene, model, *model.meshes.front ().material), m_model (model) { }

CModel::~CModel () {
    for (auto& mesh : this->m_meshes) {
	delete mesh.pass;
	if (mesh.ebo != GL_NONE) {
	    glDeleteBuffers (1, &mesh.ebo);
	}
	if (mesh.vbo != GL_NONE) {
	    glDeleteBuffers (1, &mesh.vbo);
	}
	if (mesh.vao != GL_NONE) {
	    glDeleteVertexArrays (1, &mesh.vao);
	}
    }
    if (this->m_depthRenderbuffer != GL_NONE) {
	glDeleteRenderbuffers (1, &this->m_depthRenderbuffer);
    }
}

const float& CModel::getBrightness () const { return this->m_brightness; }
const float& CModel::getUserAlpha () const { return this->m_alpha; }
const float& CModel::getAlpha () const { return this->m_alpha; }
const glm::vec3& CModel::getColor () const { return this->m_color; }
const glm::vec4& CModel::getColor4 () const { return this->m_color4; }
const glm::vec3& CModel::getCompositeColor () const { return this->m_color; }

void CModel::setup () {
    if (this->m_initialized) {
	return;
    }

    // Model materials declare textures:[null], which would deref null in CRenderable::setup();
    // resolve a neutral white base instead (generic3's albedo default is util/white anyway).
    try {
	this->m_texture = this->getContext ().resolveTexture ("util/white");
    } catch (const std::exception& e) {
	sLog.error ("CModel: cannot resolve util/white base texture: ", e.what ());
    }

    this->m_fboProvider = std::make_shared<FBOProvider> (this);
    // generic3's REFLECTION samples the scene FBO it's drawing into (a feedback loop), so shadow
    // it with a same-named copy we blit into before drawing (mirrors CParticle's REFRACT).
    const auto sceneFBO = this->getScene ().getFBO ();
    const glm::vec2 fboSize (
	static_cast<float> (sceneFBO->getRealWidth ()), static_cast<float> (sceneFBO->getRealHeight ())
    );
    this->m_reflectionFBO = this->m_fboProvider->create (
	"_rt_FullFrameBuffer", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0f, fboSize, fboSize
    );
    // g_Texture3 defaults to "_rt_MipMappedFrameBuffer"; alias it to the shadow copy too, else the
    // pass resolves it to the live scene FBO and feedback-loops on its own target.
    this->m_fboProvider->alias ("_rt_MipMappedFrameBuffer", "_rt_FullFrameBuffer");

    // Draw in .mdl declaration order (what WE does); it's load-bearing for depth — e.g. Starscape's
    // opaque body must draw before the translucent shell or the shell occludes it and the body whitens.
    for (const auto& source : this->m_model.meshes) {
	if (source.material != nullptr && !source.material->passes.empty ()) {
	    this->setupMesh (source);
	}
    }

    this->m_initialized = true;
    sLog.debug ("Loaded 3D model ", this->m_model.mesh, " with ", this->m_meshes.size (), " mesh(es)");
}

void CModel::setupMesh (const ModelMesh& source) {
    Mesh mesh;
    mesh.indexCount = static_cast<GLsizei> (source.indices.size ());

    // Build the pass from the mesh's real material; nothing about the shading is overridden here.
    mesh.pass = new CPass (
	*this, this->m_fboProvider, *source.material->passes.front (), std::nullopt, std::nullopt, std::nullopt
    );
    const GLuint program = mesh.pass->getProgramID ();

    GLint prevVao = 0;
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &prevVao);

    glGenVertexArrays (1, &mesh.vao);
    glBindVertexArray (mesh.vao);

    glGenBuffers (1, &mesh.vbo);
    glBindBuffer (GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData (
	GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (source.vertexData.size ()), source.vertexData.data (), GL_STATIC_DRAW
    );

    glGenBuffers (1, &mesh.ebo);
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData (
	GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr> (source.indices.size () * sizeof (uint16_t)),
	source.indices.data (), GL_STATIC_DRAW
    );

    // Bind the .mdl attributes to whatever locations the compiled generic3 assigned them.
    const auto bindAttribute = [program] (const char* name, const GLint size, const uintptr_t offset) {
	const GLint location = glGetAttribLocation (program, name);
	if (location >= 0) {
	    glEnableVertexAttribArray (location);
	    glVertexAttribPointer (
		location, size, GL_FLOAT, GL_FALSE, MDL_VERTEX_STRIDE, reinterpret_cast<void*> (offset)
	    );
	}
    };
    bindAttribute ("a_Position", 3, MDL_POSITION_OFFSET);
    bindAttribute ("a_Normal", 3, MDL_NORMAL_OFFSET);
    bindAttribute ("a_Tangent4", 4, MDL_TANGENT_OFFSET);
    bindAttribute ("a_TexCoord", 2, MDL_UV_OFFSET);

    glBindVertexArray (prevVao);

    mesh.pass->setDestination (this->getScene ().getFBO ());
    mesh.pass->setInput (this->getTexture ());
    mesh.pass->setModelMatrix (&this->m_modelMatrix);
    mesh.pass->setViewProjectionMatrix (&this->m_viewProjectionMatrix);
    mesh.pass->setModelViewProjectionMatrix (&this->m_mvpMatrix);
    mesh.pass->setModelViewProjectionMatrixInverse (&this->m_mvpMatrixInverse);
    // The one uniform generic3.vert needs that the 2D pass path doesn't: the camera eye position
    // (for v_ViewDir). Everything else comes from the material/scene via CPass.
    mesh.pass->addUniform ("g_EyePosition", &this->m_eyePosition);

    const GLuint vao = mesh.vao;
    const GLsizei indexCount = mesh.indexCount;
    mesh.pass->setGeometryCallback (
	[this, vao] () {
	    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &this->m_prevVao);
	    glBindVertexArray (vao);
	},
	[indexCount] () { glDrawElements (GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, nullptr); },
	[this] () { glBindVertexArray (this->m_prevVao); }
    );

    this->m_meshes.push_back (mesh);
}

void CModel::ensureDepthBuffer (const uint32_t width, const uint32_t height) {
    if (width == 0 || height == 0) {
	return;
    }
    if (this->m_depthRenderbuffer != GL_NONE && width == this->m_depthWidth && height == this->m_depthHeight) {
	return;
    }
    if (this->m_depthRenderbuffer == GL_NONE) {
	glGenRenderbuffers (1, &this->m_depthRenderbuffer);
    }
    glBindRenderbuffer (GL_RENDERBUFFER, this->m_depthRenderbuffer);
    glRenderbufferStorage (
	GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, static_cast<GLsizei> (width), static_cast<GLsizei> (height)
    );
    glBindRenderbuffer (GL_RENDERBUFFER, 0);
    this->m_depthWidth = width;
    this->m_depthHeight = height;
}

glm::mat4 CModel::computeModelMatrix () const {
    const glm::vec3 origin = this->m_model.origin != nullptr && this->m_model.origin->value != nullptr
	? this->m_model.origin->value->getVec3 ()
	: glm::vec3 (0.0f);
    const glm::vec3 scale = this->m_model.groupScale != nullptr && this->m_model.groupScale->value != nullptr
	? this->m_model.groupScale->value->getVec3 ()
	: glm::vec3 (1.0f);
    glm::vec3 angles = this->m_model.groupAngles != nullptr && this->m_model.groupAngles->value != nullptr
	? this->m_model.groupAngles->value->getVec3 ()
	: glm::vec3 (0.0f);

    // Apply the keyframe angles animation: fold time into [0, length] ("mirror" ping-pongs, else
    // loops), interpolate each channel; relative:true adds it on top of the base angles.
    if (const auto& anim = this->m_model.anglesAnimation; anim.present && anim.length > 0.0f) {
	float frame = this->getScene ().getTime () * anim.fps;
	if (anim.mode == "mirror") {
	    const float period = 2.0f * anim.length;
	    frame = std::fmod (frame, period);
	    if (frame < 0.0f) {
		frame += period;
	    }
	    if (frame > anim.length) {
		frame = period - frame;
	    }
	} else {
	    frame = std::fmod (frame, anim.length);
	    if (frame < 0.0f) {
		frame += anim.length;
	    }
	}
	const glm::vec3 animated (
	    evalChannel (anim.c0, frame), evalChannel (anim.c1, frame), evalChannel (anim.c2, frame)
	);
	angles = anim.relative ? angles + animated : animated;
    }

    glm::mat4 matrix = glm::translate (glm::mat4 (1.0f), origin);
    matrix = glm::rotate (matrix, angles.z, glm::vec3 (0.0f, 0.0f, 1.0f));
    matrix = glm::rotate (matrix, angles.y, glm::vec3 (0.0f, 1.0f, 0.0f));
    matrix = glm::rotate (matrix, angles.x, glm::vec3 (1.0f, 0.0f, 0.0f));
    matrix = glm::scale (matrix, scale);
    return matrix;
}

void CModel::render () {
    if (!this->m_initialized || this->m_meshes.empty ()) {
	return;
    }
    if (this->m_model.groupVisible != nullptr && this->m_model.groupVisible->value != nullptr
	&& !this->m_model.groupVisible->value->getBool ()) {
	return;
    }

    const Camera& camera = this->getScene ().getCamera ();
    float fov = camera.getFov ();
    if (fov < 1.0f || fov > 170.0f) {
	fov = 50.0f;
    }
    float nearZ = camera.getNearZ ();
    if (nearZ <= 0.0f) {
	nearZ = 0.1f;
    }
    float farZ = camera.getFarZ ();
    if (farZ <= nearZ) {
	farZ = 10000.0f;
    }
    const float aspect = camera.getHeight () > 0.0f ? camera.getWidth () / camera.getHeight () : 16.0f / 9.0f;

    const glm::vec3 eye = camera.getEye ();
    const glm::vec3 center = camera.getCenter ();
    glm::mat4 projection = glm::perspective (glm::radians (fov), aspect, nearZ, farZ);
    // Scene FBO is Y-down like the 2D layers; flip clip-space Y to keep the model upright.
    projection[1][1] *= -1.0f;
    const glm::mat4 view = glm::lookAt (eye, center, camera.getUp ());

    this->m_modelMatrix = this->computeModelMatrix ();
    this->m_viewProjectionMatrix = projection * view;
    this->m_mvpMatrix = this->m_viewProjectionMatrix * this->m_modelMatrix;
    this->m_mvpMatrixInverse = glm::inverse (this->m_mvpMatrix);
    this->m_eyePosition = eye;

    const auto sceneFBO = this->getScene ().getFBO ();

    // Snapshot the scene-so-far into the reflection copy so REFLECTION samples a stable image.
    if (this->m_reflectionFBO != nullptr) {
	const auto w = static_cast<GLint> (sceneFBO->getRealWidth ());
	const auto h = static_cast<GLint> (sceneFBO->getRealHeight ());
	glBindFramebuffer (GL_READ_FRAMEBUFFER, sceneFBO->getFramebuffer ());
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, this->m_reflectionFBO->getFramebuffer ());
	glBlitFramebuffer (0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    // Attach a private depth buffer so the model's sub-meshes occlude each other; 2D layers use
    // no depth, so it's detached afterwards.
    this->ensureDepthBuffer (sceneFBO->getRealWidth (), sceneFBO->getRealHeight ());
    glBindFramebuffer (GL_FRAMEBUFFER, sceneFBO->getFramebuffer ());
    glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, this->m_depthRenderbuffer);
    // glClear only clears depth when depth writes are on; a prior 2D pass may leave glDepthMask(FALSE),
    // which would no-op the clear and leave stale depth. Force the mask on before clearing.
    glEnable (GL_DEPTH_TEST);
    glDepthMask (GL_TRUE);
    glClear (GL_DEPTH_BUFFER_BIT);

    // The Y flip above mirrors the image, reversing triangle winding, so back-face culling would cull
    // the real front faces. Declare CW as front-facing while the model draws, restored below.
    // Restore the shared GL state the 2D layers expect (no depth, CCW front) and detach our depth
    // buffer, even on throw, so a failing model can't corrupt the 2D layers drawn after it.
    const auto restoreSceneState = [&] () {
	glFrontFace (GL_CCW);
	glDisable (GL_DEPTH_TEST);
	glDepthMask (GL_TRUE);
	glBindFramebuffer (GL_FRAMEBUFFER, sceneFBO->getFramebuffer ());
	glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    };

    glFrontFace (GL_CW);

    // Run each mesh's real generic3 pass in .mdl declaration order; CPass handles FBO/blend/depth/cull,
    // all scene+material uniforms, and the draw.
    try {
	for (const auto& mesh : this->m_meshes) {
	    mesh.pass->render ();
	}
    } catch (...) {
	restoreSceneState ();
	throw;
    }

    restoreSceneState ();
}
