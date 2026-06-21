#include "ScriptableObjectAdapter.h"

#include <cmath>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <utility>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::Scripting::Adapters;

#define SCRIPTABLE_OPAQUE_MAGIC 0xdeadbeef

struct OpaqueScriptableObjectAdapter {
    unsigned int magic;
    ScriptableObjectAdapter& adapter;
    WallpaperEngine::Scripting::ScriptableObject& object;
};

// ---- ILayer methods. Exposed as functions returned by the exotic property getter below. Each
// recovers its layer via fromJS(this_val), so `thisLayer.method()` binds correctly. ----

static glm::vec3 layer_read_vec3 (JSContext* ctx, JSValueConst v) {
    glm::vec3 out (0.0f);
    if (!JS_IsObject (v)) {
	return out;
    }
    JSValue jx = JS_GetPropertyStr (ctx, v, "x");
    JSValue jy = JS_GetPropertyStr (ctx, v, "y");
    JSValue jz = JS_GetPropertyStr (ctx, v, "z");
    double d = 0.0;
    if (JS_ToFloat64 (ctx, &d, jx) == 0) {
	out.x = static_cast<float> (d);
    }
    if (JS_ToFloat64 (ctx, &d, jy) == 0) {
	out.y = static_cast<float> (d);
    }
    if (JS_ToFloat64 (ctx, &d, jz) == 0) {
	out.z = static_cast<float> (d);
    }
    JS_FreeValue (ctx, jx);
    JS_FreeValue (ctx, jy);
    JS_FreeValue (ctx, jz);
    return out;
}

// thisLayer.getParent() -> the parent layer, or undefined.
static JSValue layer_get_parent (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* self = ScriptableObjectAdapter::fromJS (this_val);
    if (self == nullptr || !self->getObject ().parent.has_value ()) {
	return JS_UNDEFINED;
    }
    auto& scene = self->getScene ();
    auto* parent = const_cast<WallpaperEngine::Render::CObject*> (scene.getObject (self->getObject ().parent.value ()));
    if (parent == nullptr || !parent->is<WallpaperEngine::Scripting::ScriptableObject> ()) {
	return JS_UNDEFINED;
    }
    return scene.getScriptEngine ().getAdapters ().object->instantiate (
	*parent->as<WallpaperEngine::Scripting::ScriptableObject> ()
    );
}

// thisLayer.getChildren() -> array of layers parented to this one.
static JSValue layer_get_children (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSValue arr = JS_NewArray (ctx);
    auto* self = ScriptableObjectAdapter::fromJS (this_val);
    if (self == nullptr) {
	return arr;
    }
    const int myId = self->getId ();
    auto& scene = self->getScene ();
    uint32_t index = 0;
    for (auto* object : scene.getObjectsByRenderOrder ()) {
	if (object == nullptr || !object->is<WallpaperEngine::Scripting::ScriptableObject> ()) {
	    continue;
	}
	const auto& parent = object->getObject ().parent;
	if (parent.has_value () && parent.value () == myId) {
	    JS_SetPropertyUint32 (
		ctx, arr, index++,
		scene.getScriptEngine ().getAdapters ().object->instantiate (
		    *object->as<WallpaperEngine::Scripting::ScriptableObject> ()
		)
	    );
	}
    }
    return arr;
}

// thisLayer.rotateObjectSpace(angles) -> add a local rotation (degrees) to the layer's angles.
static JSValue layer_rotate_object_space (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) {
	return JS_UNDEFINED;
    }
    auto* self = ScriptableObjectAdapter::fromJS (this_val);
    if (self == nullptr) {
	return JS_UNDEFINED;
    }
    const glm::vec3 delta = layer_read_vec3 (ctx, argv[0]);
    try {
	auto& angles = self->getProperty ("angles");
	angles.update (angles.getVec3 () + delta, DynamicValue::UpdateSource::Script);
    } catch (const std::exception&) {
	// layer has no angles property (non-image group); nothing to rotate
    }
    return JS_UNDEFINED;
}

// Compose the layer's TRS as a glm column-major matrix: translate(origin) * Ry * Rx * Rz * scale,
// with angles in degrees. (Best-effort; matches the engine's Y-X-Z euler convention.)
static glm::mat4 layer_trs (WallpaperEngine::Scripting::ScriptableObject* self) {
    glm::vec3 origin (0.0f);
    glm::vec3 angles (0.0f);
    glm::vec3 scale (1.0f);
    try { origin = self->getProperty ("origin").getVec3 (); } catch (const std::exception&) {}
    try { angles = self->getProperty ("angles").getVec3 (); } catch (const std::exception&) {}
    try { scale = self->getProperty ("scale").getVec3 (); } catch (const std::exception&) {}

    glm::mat4 m (1.0f);
    m = glm::translate (m, origin);
    m = glm::rotate (m, glm::radians (angles.y), glm::vec3 (0.0f, 1.0f, 0.0f));
    m = glm::rotate (m, glm::radians (angles.x), glm::vec3 (1.0f, 0.0f, 0.0f));
    m = glm::rotate (m, glm::radians (angles.z), glm::vec3 (0.0f, 0.0f, 1.0f));
    m = glm::scale (m, scale);
    return m;
}

// Build a JS Mat4 (the pure-JS global class, column-major) from a glm matrix.
static JSValue build_js_mat4 (JSContext* ctx, const glm::mat4& mat) {
    JSValue global = JS_GetGlobalObject (ctx);
    JSValue ctor = JS_GetPropertyStr (ctx, global, "Mat4");
    JSValue result = JS_CallConstructor (ctx, ctor, 0, nullptr);
    JSValue m = JS_GetPropertyStr (ctx, result, "m");
    const float* p = glm::value_ptr (mat);
    for (uint32_t i = 0; i < 16; i++) {
	JS_SetPropertyUint32 (ctx, m, i, JS_NewFloat64 (ctx, p[i]));
    }
    JS_FreeValue (ctx, m);
    JS_FreeValue (ctx, ctor);
    JS_FreeValue (ctx, global);
    return result;
}

// thisLayer.getTransformMatrix() -> Mat4 of the layer's local TRS.
static JSValue layer_get_transform_matrix (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* self = ScriptableObjectAdapter::fromJS (this_val);
    if (self == nullptr) {
	return JS_UNDEFINED;
    }
    return build_js_mat4 (ctx, layer_trs (self));
}

// thisLayer.lookAt(center, up?) / lookAtYaw(...). magic 0 = yaw+pitch, 1 = yaw only.
static JSValue layer_look_at (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    if (argc < 1) {
	return JS_UNDEFINED;
    }
    auto* self = ScriptableObjectAdapter::fromJS (this_val);
    if (self == nullptr) {
	return JS_UNDEFINED;
    }
    glm::vec3 origin (0.0f);
    try { origin = self->getProperty ("origin").getVec3 (); } catch (const std::exception&) {}

    glm::vec3 dir = layer_read_vec3 (ctx, argv[0]) - origin;
    if (glm::length (dir) < 1e-6f) {
	return JS_UNDEFINED;
    }
    dir = glm::normalize (dir);
    const float yaw = glm::degrees (std::atan2 (dir.x, -dir.z));
    const float pitch = magic == 1 ? 0.0f : glm::degrees (std::asin (glm::clamp (dir.y, -1.0f, 1.0f)));
    try {
	auto& angles = self->getProperty ("angles");
	angles.update (glm::vec3 (pitch, yaw, angles.getVec3 ().z), DynamicValue::UpdateSource::Script);
    } catch (const std::exception&) {}
    return JS_UNDEFINED;
}

// thisLayer.setParent(parent, ...) -> reparent by ILayer handle, name, or id. Only the parent link
// changes; per-frame transform resolution walks parents, so the child inherits the new transform.
static JSValue layer_set_parent (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) {
	return JS_UNDEFINED;
    }
    auto* self = ScriptableObjectAdapter::fromJS (this_val);
    if (self == nullptr) {
	return JS_UNDEFINED;
    }

    auto& scene = self->getScene ();
    int parentId = -1;

    if (auto* asLayer = ScriptableObjectAdapter::fromJS (argv[0]); asLayer != nullptr) {
	parentId = asLayer->getId ();
    } else if (JS_IsNumber (argv[0])) {
	int v = 0;
	JS_ToInt32 (ctx, &v, argv[0]);
	parentId = v;
    } else if (JS_IsString (argv[0])) {
	const char* nm = JS_ToCString (ctx, argv[0]);
	if (nm != nullptr) {
	    for (auto* o : scene.getObjectsByRenderOrder ()) {
		if (o != nullptr && o->getObject ().name == nm) {
		    parentId = o->getId ();
		    break;
		}
	    }
	    JS_FreeCString (ctx, nm);
	}
    }

    // The Object is owned mutably by the scene (ObjectUniquePtr); getObject() hands out a const view,
    // so re-seating the parent link via const_cast is well-defined here.
    if (parentId >= 0 && parentId != self->getId ()) {
	const_cast<WallpaperEngine::Data::Model::Object&> (self->getObject ()).parent = parentId;
    }
    return JS_UNDEFINED;
}

JSValue scriptableobject_property_get (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    JSClassID classId = 0;

    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (obj_val, &classId));

    if (!container || container->magic != SCRIPTABLE_OPAQUE_MAGIC) {
	return JS_EXCEPTION;
    }

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return JS_EXCEPTION;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });

    // The object's name isn't a DynamicValue property, but scripts read layer.name heavily
    // (name.includes("Big")/"Nude"/... to categorise layers), so expose it directly.
    if (std::strcmp (name, "name") == 0) {
	return JS_NewString (ctx, container->object.getObject ().name.c_str ());
    }

    // ILayer methods (return a callable; bound to this layer via fromJS on call).
    if (std::strcmp (name, "getParent") == 0) {
	return JS_NewCFunction (ctx, layer_get_parent, "getParent", 0);
    }
    if (std::strcmp (name, "getChildren") == 0) {
	return JS_NewCFunction (ctx, layer_get_children, "getChildren", 0);
    }
    if (std::strcmp (name, "rotateObjectSpace") == 0) {
	return JS_NewCFunction (ctx, layer_rotate_object_space, "rotateObjectSpace", 1);
    }
    if (std::strcmp (name, "getTransformMatrix") == 0) {
	return JS_NewCFunction (ctx, layer_get_transform_matrix, "getTransformMatrix", 0);
    }
    if (std::strcmp (name, "lookAt") == 0) {
	return JS_NewCFunctionMagic (ctx, layer_look_at, "lookAt", 2, JS_CFUNC_generic_magic, 0);
    }
    if (std::strcmp (name, "lookAtYaw") == 0) {
	return JS_NewCFunctionMagic (ctx, layer_look_at, "lookAtYaw", 2, JS_CFUNC_generic_magic, 1);
    }
    if (std::strcmp (name, "setParent") == 0) {
	return JS_NewCFunction (ctx, layer_set_parent, "setParent", 2);
    }

    try {
	// find the property inside, otherwise return undefined
	auto& property = container->object.getProperty (name);

	return container->adapter.getEngine ().dynamicToJs (property);
    } catch (const std::exception& e) {
	return JS_UNDEFINED;
    }
}

int scriptableobject_property_set (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
) {
    JSClassID classId = 0;

    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (obj_val, &classId));

    if (!container || container->magic != SCRIPTABLE_OPAQUE_MAGIC) {
	return -1;
    }

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return -1;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });

    // Writing a layer property from a script (e.g. layer.visible = false, layer.origin = vec) must
    // reach the live DynamicValue so the change actually renders. This setter used to be a no-op,
    // which is why script-driven wallpapers (Makima's style selector) that hide/show layers showed
    // every layer at once. Push the JS value into the matching property.
    try {
	auto& property = container->object.getProperty (name);

	if (JS_IsBool (val)) {
	    property.update (JS_ToBool (ctx, val) != 0, DynamicValue::UpdateSource::User);
	} else if (JS_IsNumber (val)) {
	    double number = 0.0;
	    JS_ToFloat64 (ctx, &number, val);
	    property.update (static_cast<float> (number), DynamicValue::UpdateSource::User);
	} else if (JS_IsObject (val)) {
	    // A Vec2/Vec3-like { x, y, z } (origin/scale/angles).
	    const auto component = [ctx, &val] (const char* key) -> float {
		JSValue field = JS_GetPropertyStr (ctx, val, key);
		double number = 0.0;
		if (JS_IsNumber (field)) {
		    JS_ToFloat64 (ctx, &number, field);
		}
		JS_FreeValue (ctx, field);
		return static_cast<float> (number);
	    };
	    property.update (
		glm::vec3 (component ("x"), component ("y"), component ("z")), DynamicValue::UpdateSource::User
	    );
	}
    } catch (const std::exception&) {
	// Unknown property — silently ignore (matches the previous behaviour for non-registered keys).
    }

    return 1;
}

ScriptableObjectAdapter::ScriptableObjectAdapter (ScriptEngine& engine, std::string name) :
    ObjectAdapter (engine), m_exoticMethods (), m_name (std::move (name)) {
    // Route property reads/writes on layer objects through our handlers (must be set before
    // registerType installs the exotic table). Without this the getters/setters below are never
    // called — layer.name reads undefined and layer.visible = ... is a no-op, which is why
    // script-driven wallpapers (Makima) couldn't read names or hide/show layers.
    this->m_exoticMethods.get_property = scriptableobject_property_get;
    this->m_exoticMethods.set_property = scriptableobject_property_set;

    this->registerType (
	{
	    .class_name = m_name.c_str (),
	    .exotic = &m_exoticMethods,
	}
    );
}

JSValue ScriptableObjectAdapter::instantiate (ScriptableObject& object) {
    JSValue result = this->ObjectAdapter::instantiate (object);
    JS_SetOpaque (
	result,
	new OpaqueScriptableObjectAdapter { .magic = SCRIPTABLE_OPAQUE_MAGIC, .adapter = *this, .object = object }
    );

    return result;
}

JSValue ScriptableObjectAdapter::instantiate (DynamicValue& value) {
    throw std::runtime_error ("Cannot create a ScriptableObject instance from a DynamicValue");
}

WallpaperEngine::Scripting::ScriptableObject* ScriptableObjectAdapter::fromJS (JSValue value) {
    JSClassID classId = 0;
    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (value, &classId));

    if (container == nullptr || container->magic != SCRIPTABLE_OPAQUE_MAGIC) {
	return nullptr;
    }

    return &container->object;
}