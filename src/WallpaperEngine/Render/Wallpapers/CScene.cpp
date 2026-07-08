#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Render/Objects/CModel.h"
#include "WallpaperEngine/Render/Objects/CParticle.h"
#include "WallpaperEngine/Render/Objects/CSound.h"
#include "WallpaperEngine/Render/Objects/CText.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include "WallpaperEngine/Render/WallpaperState.h"

#include "CScene.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"

#include <algorithm>
#include <ranges>

extern float g_Time;
extern float g_TimeLast;

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Render::Wallpapers;

CScene::CScene (
    const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
    const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
) : CWallpaper (wallpaper, context, audioContext, scalingMode, clampMode) {
    // caller should check this, if not a std::bad_cast is good to throw
    auto scene = wallpaper.as<Scene> ();

    // setup scripting engine
    this->m_scriptEngine = std::make_unique<Scripting::ScriptEngine> (*this, context.getMediaSource ());
    // setup the scene camera
    this->m_camera = std::make_unique<Camera> (*this, scene->camera);

    float width = scene->camera.projection.width;
    float height = scene->camera.projection.height;

    // detect size if the orthogonal project is auto
    if (scene->camera.projection.isAuto) {
	glm::vec2 maxExtent = { 0.0f, 0.0f };

	for (const auto& object : scene->objects) {
	    if (!object->is<Image> ()) {
		continue;
	    }

	    const auto* image = object->as<Image> ();
	    if (!image->origin || !image->origin->value) {
		continue;
	    }

	    const glm::vec3 origin = image->origin->value->getVec3 ();
	    const glm::vec2 halfSize = image->size / 2.0f;

	    maxExtent.x = glm::max (maxExtent.x, glm::abs (origin.x) + halfSize.x);
	    maxExtent.y = glm::max (maxExtent.y, glm::abs (origin.y) + halfSize.y);
	}

	if (maxExtent.x > 0.0f && maxExtent.y > 0.0f) {
	    width = maxExtent.x * 2.0f;
	    height = maxExtent.y * 2.0f;
	} else {
	    // Use the first-captured output size, not the live one: an in-process rebuild must size the
	    // scene like the original build or the effect-composite chain samples misaligned buffers.
	    const auto fallback = this->getContext ().getStableOutputSize ();
	    width = fallback.x;
	    height = fallback.y;
	    sLog.debug ("Auto projection: falling back to screen resolution ", width, "x", height);
	}
    }

    this->m_parallaxDisplacement = { 0, 0 };

    // TODO: CONVERSION
    this->m_camera->setOrthogonalProjection (width, height);

    // setup framebuffers here as they're required for the scene setup
    this->setupFramebuffers ();

    const uint32_t sceneWidth = this->m_camera->getWidth ();
    const uint32_t sceneHeight = this->m_camera->getHeight ();

    this->_rt_shadowAtlas = this->create (
	"_rt_shadowAtlas", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth, sceneHeight },
	{ sceneWidth, sceneHeight }
    );
    this->alias ("_alias_lightCookie", "_rt_shadowAtlas");

    // set clear color
    const glm::vec3 clearColor = scene->colors.clear->value->getVec3 ();

    glClearColor (clearColor.r, clearColor.g, clearColor.b, 1.0f);

    // create all objects based off their dependencies
    for (const auto& object : scene->objects) {
	this->createObject (*object);
    }

    // copy over objects by render order
    for (const auto& object : scene->objects) {
	this->addObjectToRenderOrder (*object);
    }

    // "customsortorder" sorts by each object's "sortorder" key instead of declaration order; stable so
    // objects sharing a sortorder keep declaration order as the tiebreaker.
    if (scene->customSortOrder) {
	std::ranges::stable_sort (this->m_objectsByRenderOrder, [] (const CObject* a, const CObject* b) {
	    return a->getObject ().sortorder < b->getObject ().sortorder;
	});
    }

    // create extra framebuffers for the bloom effect
    this->_rt_4FrameBuffer = this->create (
	"_rt_4FrameBuffer", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth / 4, sceneHeight / 4 },
	{ sceneWidth / 4, sceneHeight / 4 }
    );
    this->_rt_8FrameBuffer = this->create (
	"_rt_8FrameBuffer", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth / 8, sceneHeight / 8 },
	{ sceneWidth / 8, sceneHeight / 8 }
    );
    this->_rt_Bloom = this->create (
	"_rt_Bloom", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth / 8, sceneHeight / 8 },
	{ sceneWidth / 8, sceneHeight / 8 }
    );

    //
    // Had to get a little creative with the effects to achieve the same bloom effect without any custom code
    // this custom image loads some effect files from the virtual container to achieve the same bloom effect
    // this approach requires of two extra draw calls due to the way the effect works in official WPE
    // (it renders directly to the screen, whereas here we never do that from a scene)
    //

    const auto bloomOrigin = glm::vec3 { sceneWidth / 2, sceneHeight / 2, 0.0f };
    const auto bloomSize = glm::vec2 { sceneWidth, sceneHeight };

    const JSON bloom
	= { { "image", "models/wpenginelinux.json" },
	    { "name", "bloomimagewpenginelinux" },
	    { "visible", true },
	    { "scale", "1.0 1.0 1.0" },
	    { "angles", "0.0 0.0 0.0" },
	    { "origin",
	      std::to_string (bloomOrigin.x) + " " + std::to_string (bloomOrigin.y) + " "
		  + std::to_string (bloomOrigin.z) },
	    { "size", std::to_string (bloomSize.x) + " " + std::to_string (bloomSize.y) },
	    { "id", -1 },
	    { "effects",
	      JSON::array (
		  { { { "file", "effects/wpenginelinux/bloomeffect.json" },
		      { "id", 15242000 },
		      { "name", "" },
		      { "passes",
			JSON::array (
			    { { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold",
				      this->getScene ().camera.bloom.threshold->value->getFloat () } } } },
			      { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold",
				      this->getScene ().camera.bloom.threshold->value->getFloat () } } } },
			      { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold",
				      this->getScene ().camera.bloom.threshold->value->getFloat () } } } } }
			) } } }
	      ) } };

    // create image for bloom passes (only when the scene requests scene-level bloom; the
    // wallpaper's own effect-layer bloom is handled separately by its CImage effect chain).
    if (scene->camera.bloom.enabled->value->getBool ()) {
	this->m_bloomObjectData = ObjectParser::parse (bloom, scene->project);
	this->m_bloomObject = this->createObject (*this->m_bloomObjectData);

	this->m_objectsByRenderOrder.push_back (this->m_bloomObject);
    }
}

CScene::~CScene () {
    // bloom object is in the objects list, so no need to explicitly delete it
    this->m_bloomObject = nullptr;

    for (const auto& val : this->m_objects | std::views::values) {
	delete val;
    }

    this->m_objectsByRenderOrder.clear ();
    this->m_objects.clear ();
}

Render::CObject* CScene::createObject (const Object& object) {
    Render::CObject* renderObject = nullptr;

    // ensure the item is not loaded already
    if (const auto current = this->m_objects.find (object.id); current != this->m_objects.end ()) {
	return current->second;
    }

    // check dependencies too!
    for (const auto& cur : object.dependencies) {
	// self-dependency is a possibility...
	if (cur == object.id) {
	    continue;
	}

	const auto dep
	    = std::ranges::find_if (this->getScene ().objects, [&cur] (const auto& o) { return o->id == cur; });

	if (dep != this->getScene ().objects.end ()) {
	    this->createObject (**dep);
	}
    }

    // check if the item has any parent and also create it first
    if (object.parent.has_value ()) {
	int parentId = object.parent.value ();

	const auto dep = std::ranges::find_if (this->getScene ().objects, [&parentId] (const auto& o) {
	    return o->id == parentId;
	});

	if (dep == this->getScene ().objects.end ()) {
	    sLog.exception ("Cannot find parent ", parentId, " for object ", object.id);
	}

	this->createObject (**dep);
    }

    renderObject = this->dispatchObjectType (object);

    if (renderObject != nullptr) {
	this->m_objects.emplace (renderObject->getId (), renderObject);
    }

    return renderObject;
}

Render::CObject* CScene::dispatchObjectType (const Object& object) {
    Render::CObject* renderObject = nullptr;

    if (object.is<Image> ()) {
	renderObject = new Objects::CImage (*this, *object.as<Image> ());
    } else if (object.is<Sound> ()) {
	renderObject = new Objects::CSound (*this, *object.as<Sound> ());
    } else if (object.is<Text> ()) {
	renderObject = new Objects::CText (*this, *object.as<Text> ());
    } else if (object.is<ModelObject> ()) {
	const auto& model = *object.as<ModelObject> ();
	if (model.meshes.empty () || model.meshes.front ().material == nullptr) {
	    sLog.error ("Model object ", object.id, " has no renderable meshes, skipping");
	    return nullptr;
	}
	renderObject = new Objects::CModel (*this, model);
    } else if (object.is<Particle> ()) {
	const auto& particleData = *object.as<Particle> ();

	if (this->getContext ().getApp ().getContext ().settings.general.disableParticles == true) {
	    sLog.debug ("Ignoring particle system (disabled in settings): ", particleData.name);
	    return nullptr;
	}

	renderObject = new Objects::CParticle (*this, particleData);
    } else {
	// A transform "group" container: make it a ScriptableObject so its transform + visibility are
	// script-drivable (a style-selector flips the group's visible; a plain CObject can't be driven).
	renderObject = new Scripting::ScriptableObject (*this, object);
    }

    try {
	renderObject->setup ();
    } catch (const std::exception& e) {
	sLog.error ("Failed to setup object ", object.id, ": ", e.what ());
	delete renderObject;
	renderObject = nullptr;
    }

    return renderObject;
}

void CScene::addObjectToRenderOrder (const Object& object) {
    const auto obj = this->m_objects.find (object.id);

    // ignores not created objects like particle systems
    if (obj == this->m_objects.end ()) {
	return;
    }

    // take into account any dependency first
    for (const auto& dep : object.dependencies) {
	// self-dependency is possible
	if (dep == object.id) {
	    continue;
	}

	// add the dependency to the list if it's created
	auto depIt = std::ranges::find_if (this->getScene ().objects, [&dep] (const auto& o) { return o->id == dep; });

	if (depIt != this->getScene ().objects.end ()) {
	    this->addObjectToRenderOrder (**depIt);
	} else {
	    sLog.error ("Cannot find dependency ", dep, " for object ", object.id);
	}
    }

    // ensure we're added only once to the render list
    const auto renderIt = std::ranges::find_if (this->m_objectsByRenderOrder, [&object] (const auto& o) {
	return o->getId () == object.id;
    });

    if (renderIt == this->m_objectsByRenderOrder.end ()) {
	this->m_objectsByRenderOrder.emplace_back (obj->second);
    }
}

ScriptEngine& CScene::getScriptEngine () const { return *this->m_scriptEngine; }
Camera& CScene::getCamera () const { return *this->m_camera; }

void CScene::renderFrame (const glm::ivec4& viewport) {
    // ensure the virtual mouse position is up to date
    this->updateMouse (viewport);

    // update the parallax position if required
    if (this->getScene ().camera.parallax.enabled->value->getBool ()
	&& !this->getContext ().getApp ().getContext ().settings.mouse.disableparallax) {
	const float influence = this->getScene ().camera.parallax.mouseInfluence->value->getFloat ();
	const float amount = this->getScene ().camera.parallax.amount->value->getFloat ();
	const float delay = glm::clamp (
	    this->getScene ().camera.parallax.delay->value->getFloat () * (g_Time - g_TimeLast), 0.0f, 1.0f
	);

	const glm::vec2 centeredMouse = this->m_mousePosition - glm::vec2 (0.5f, 0.5f);
	this->m_parallaxDisplacement
	    = glm::mix (this->m_parallaxDisplacement, (centeredMouse * amount) * influence, delay);
    }

    // run a tick in the javascript logic
    this->getScriptEngine ().tick ();

    // update main textures for images
    for (const auto& cur : this->m_objectsByRenderOrder) {
	if (!cur->is<Objects::CImage> ()) {
	    continue;
	}

	const Objects::CImage* image = cur->as<Objects::CImage> ();

#if !NDEBUG
	const std::string message = "Updating texture " + image->getImage ().model->filename;

	glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, message.c_str ());
#endif

	image->getTexture ()->update ();

#if !NDEBUG
	glPopDebugGroup ();
#endif
    }

    // bind the vertex array
    glBindVertexArray (this->m_vaoBuffer);
    // use the scene's framebuffer by default
    glBindFramebuffer (GL_FRAMEBUFFER, this->getWallpaperFramebuffer ());
    // ensure we render over the whole framebuffer
    glViewport (0, 0, this->m_sceneFBO->getRealWidth (), this->m_sceneFBO->getRealHeight ());

    // Force all channels on before the clear: a leaked alpha-disabled mask leaves stale alpha that
    // every alpha-blended effect writeback then composites against.
    glColorMask (true, true, true, true);
    // Clear color is global GL state, set once at construction. Another scene's ctor may have
    // overwritten it since ours ran (e.g. this scene was built, then other scenes were built/rendered
    // before it was displayed — a resident/preloaded scene never re-runs its ctor on bind). Re-apply
    // our own each frame so the scene FBO always clears to the intended colour, not a leaked one: a
    // non-black leak seeds the model<->bloom reflection feedback loop off zero and it grows to a gray
    // fixed point (the "RGB fan" over the whole sky).
    const glm::vec3 clearColor = this->getScene ().colors.clear->value->getVec3 ();
    glClearColor (clearColor.r, clearColor.g, clearColor.b, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    for (const auto& cur : this->m_objectsByRenderOrder) {
	const auto& debug = this->getContext ().getApp ().getContext ().settings.render.debug;
	if (debug.objectFilter.has_value () && cur->getId () != debug.objectFilter.value ()) {
	    continue;
	}
	if (std::ranges::find (debug.skipObjects, cur->getId ()) != debug.skipObjects.end ()) {
	    continue;
	}

	cur->render ();
    }
}

void CScene::updateMouse (const glm::ivec4& viewport) {
    // update virtual mouse position first
    const glm::dvec2 position = this->getContext ().getInputContext ().getMouseInput ().position ();

    // rollover the position to the last
    this->m_mousePositionLast = this->m_mousePosition;

    // calculate the current position of the mouse in viewport space [0, 1]
    double mouseX = glm::clamp ((position.x - viewport.x) / viewport.z, 0.0, 1.0);
    // Normalize Y coordinate (OpenGL convention: 0=bottom, 1=top)
    // Particle code expects this convention: 0=bottom results in negative Y (down), 1=top results in positive Y (up)
    double normalizedMouseY = glm::clamp ((position.y - viewport.y) / viewport.w, 0.0, 1.0);

    // Account for UV cropping when using fill/fit scaling modes
    // The scene may be rendered larger than viewport and cropped via UVs
    const auto uvs = this->getState ().getTextureUVs ();

    // Map mouse position from viewport space to scene UV space
    // UVs define what portion of the scene texture is visible
    this->m_mousePositionNormalized.x = uvs.ustart + mouseX * (uvs.uend - uvs.ustart);
    this->m_mousePositionNormalized.y = uvs.vstart + normalizedMouseY * (uvs.vend - uvs.vstart);

    // Invert previous normalization of Y to match what the shader expects
    double mouseY = 1.0 - normalizedMouseY;

    this->m_mousePosition.x = this->m_mousePositionNormalized.x;
    this->m_mousePosition.y = uvs.vstart + mouseY * (uvs.vend - uvs.vstart);
}

const Scene& CScene::getScene () const { return *this->getWallpaperData ().as<Scene> (); }

int CScene::getWidth () const { return this->m_camera->getWidth (); }

int CScene::getHeight () const { return this->m_camera->getHeight (); }

float CScene::getTime () const { return g_Time; }

float CScene::getDeltaTime () const { return g_Time - g_TimeLast; }

float CScene::getFps () const {
    const float dt = g_Time - g_TimeLast;
    // Guard against the first frame (where g_TimeLast is 0 so dt == g_Time)
    // and division by zero on the very first call.
    if (dt <= 1e-6f) {
	return 60.0f;
    }
    return 1.0f / dt;
}

const glm::vec2* CScene::getMousePosition () const { return &this->m_mousePosition; }

const glm::vec2* CScene::getMousePositionLast () const { return &this->m_mousePositionLast; }

const glm::vec2* CScene::getMousePositionNormalized () const { return &this->m_mousePositionNormalized; }

const glm::vec2* CScene::getParallaxDisplacement () const { return &this->m_parallaxDisplacement; }

const std::vector<CObject*>& CScene::getObjectsByRenderOrder () const { return this->m_objectsByRenderOrder; }

const CObject* CScene::getObject (int id) const {
    const auto object = this->m_objects.find (id);
    return object == this->m_objects.end () ? nullptr : object->second;
}

Render::CObject* CScene::createLayer (const std::string& modelPath, const std::string& workshopId) {
    // Resolve the asset path: scripts use the bare path (e.g. "models/full-pixel.json") but the asset
    // may live under the script's workshop id. Try the bare path first, then the workshop-scoped one.
    std::string path = modelPath;
    const auto resolves = [this] (const std::string& candidate) {
	try {
	    this->getAssetLocator ().readString (candidate);
	    return true;
	} catch (const std::exception&) {
	    return false;
	}
    };

    if (!resolves (path) && !workshopId.empty ()) {
	if (const auto slash = path.find ('/'); slash != std::string::npos) {
	    std::string scoped = path.substr (0, slash + 1) + "workshop/" + workshopId + "/" + path.substr (slash + 1);
	    if (resolves (scoped)) {
		path = std::move (scoped);
	    }
	}
    }

    // Allocate a fresh id above everything known (live + parse-time objects) so it can't collide.
    int newId = 0;
    for (const auto& id : this->m_objects | std::views::keys) {
	newId = std::max (newId, id);
    }
    for (const auto& object : this->getScene ().objects) {
	newId = std::max (newId, object->id);
    }
    ++newId;

    const glm::vec3 origin = { this->m_camera->getWidth () / 2.0f, this->m_camera->getHeight () / 2.0f, 0.0f };

    const JSON layer = {
	{ "image", path },
	{ "name", "runtime-layer-" + std::to_string (newId) },
	{ "visible", true },
	{ "scale", "1.0 1.0 1.0" },
	{ "angles", "0.0 0.0 0.0" },
	{ "origin", std::to_string (origin.x) + " " + std::to_string (origin.y) + " " + std::to_string (origin.z) },
	{ "id", newId },
    };

    try {
	ObjectUniquePtr data = ObjectParser::parse (layer, this->getScene ().project);
	Object* raw = data.get ();
	this->m_runtimeLayerData.push_back (std::move (data));

	Render::CObject* renderObject = this->createObject (*raw);
	if (renderObject == nullptr) {
	    return nullptr;
	}

	this->m_objectsByRenderOrder.push_back (renderObject);
	return renderObject;
    } catch (const std::exception& e) {
	sLog.error ("createLayer failed for ", modelPath, ": ", e.what ());
	return nullptr;
    }
}

int CScene::getScriptableLayerIndex (const CObject* layer) const {
    int index = 0;
    for (const auto* object : this->m_objectsByRenderOrder) {
	if (object == nullptr || !object->is<Scripting::ScriptableObject> ()) {
	    continue;
	}
	if (object == layer) {
	    return index;
	}
	++index;
    }
    return -1;
}

void CScene::moveLayerToScriptableIndex (CObject* layer, int index) {
    auto& order = this->m_objectsByRenderOrder;
    const auto current = std::ranges::find (order, layer);
    if (current == order.end ()) {
	return;
    }
    order.erase (current);

    // Insert just before the index-th scriptable layer; a negative/past-the-end index appends (top).
    auto insertPos = order.end ();
    if (index >= 0) {
	int scriptIndex = 0;
	for (auto it = order.begin (); it != order.end (); ++it) {
	    if (*it == nullptr || !(*it)->is<Scripting::ScriptableObject> ()) {
		continue;
	    }
	    if (scriptIndex == index) {
		insertPos = it;
		break;
	    }
	    ++scriptIndex;
	}
    }
    order.insert (insertPos, layer);
}
