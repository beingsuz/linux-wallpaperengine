#include "VectorModule.h"

#include "WallpaperEngine/Scripting/ScriptEngine.h"

#include <cmath>
#include <map>

using namespace WallpaperEngine::Scripting::Modules;

static constexpr double RAD2DEG = 57.295779513082320876798154814105;

static uint32_t VectorModuleInstanceId = 0;
// Module-def -> instance id, so the (static) init callback can stamp the right magic on the exports.
static std::map<JSModuleDef*, uint32_t> vectorModuleDefs;

JSValue wevector_angle2 (JSContext*, JSValueConst, int, JSValueConst*, int);

// QuickJS contract: JS_AddModuleExport declares exports (constructor); JS_SetModuleExport assigns their
// values from inside the init callback (instantiation). The original code had these swapped, leaving
// WEVector.vectorAngle2 undefined ("not a function") so any importing script (e.g. the 3D camera) threw.
int wevector_init (JSContext* ctx, JSModuleDef* m) {
    const auto it = vectorModuleDefs.find (m);
    const uint32_t instanceId = it != vectorModuleDefs.end () ? it->second : 0;

    JS_SetModuleExport (
	ctx, m, "vectorAngle2",
	JS_NewCFunctionMagic (ctx, wevector_angle2, "vectorAngle2", 1, JS_CFUNC_generic_magic, instanceId)
    );
    return 0;
}

// vectorAngle2(Vec2) -> heading in degrees. Reads .x/.y off the vector object (a VectorAdapter
// Vec2 or any {x,y}). Wallpaper Engine uses this for the camera's heading angle.
JSValue wevector_angle2 (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    if (argc != 1) {
	return JS_EXCEPTION;
    }

    double x = 0.0;
    double y = 0.0;
    JSValue jx = JS_GetPropertyStr (ctx, argv[0], "x");
    JSValue jy = JS_GetPropertyStr (ctx, argv[0], "y");
    JS_ToFloat64 (ctx, &x, jx);
    JS_ToFloat64 (ctx, &y, jy);
    JS_FreeValue (ctx, jx);
    JS_FreeValue (ctx, jy);

    return JS_NewFloat64 (ctx, std::atan2 (y, x) * RAD2DEG);
}

VectorModule::VectorModule (ScriptEngine& engine) : ScriptModule (engine, "WEVector", wevector_init) {
    this->m_instanceId = ++VectorModuleInstanceId;
    vectorModuleDefs.emplace (this->getDefinition (), this->m_instanceId);

    JS_AddModuleExport (this->getEngine ().getContext (), this->getDefinition (), "vectorAngle2");
}

VectorModule::~VectorModule () { vectorModuleDefs.erase (this->getDefinition ()); }
