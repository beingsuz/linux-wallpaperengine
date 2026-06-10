#include "VectorModule.h"

#include "WallpaperEngine/Scripting/ScriptEngine.h"

#include <cmath>

using namespace WallpaperEngine::Scripting::Modules;

static constexpr double RAD2DEG = 57.295779513082320876798154814105;

static uint32_t VectorModuleInstanceId = 0;

int wevector_init (JSContext* ctx, JSModuleDef* m) {
    JS_AddModuleExport (ctx, m, "vectorAngle2");
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

    JS_SetModuleExport (
	this->getEngine ().getContext (), this->getDefinition (), "vectorAngle2",
	JS_NewCFunctionMagic (
	    this->getEngine ().getContext (), wevector_angle2, "vectorAngle2", 1, JS_CFUNC_generic_magic,
	    this->m_instanceId
	)
    );
}

VectorModule::~VectorModule () = default;
