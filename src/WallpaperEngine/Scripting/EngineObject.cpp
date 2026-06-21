#include "EngineObject.h"
#include "ScriptEngine.h"
#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include <ranges>

using namespace WallpaperEngine::Scripting;

extern float g_Time;
extern float g_TimeLast;
extern float g_Daytime;

static uint32_t EngineInstanceId = 0;
std::map<uint32_t, EngineObject&> engineInstances;

JSValue engine_set_value (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return JS_EXCEPTION; }

JSValue engine_open_user_shortcut (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_UNDEFINED;
}

// Context queries. linux-wallpaperengine always runs as a desktop wallpaper (not a screensaver,
// not the WE editor, not mobile), so these return fixed truths. Scripts guard heavily on them
// (e.g. `if (engine.isWallpaper())`), and a missing method throws "not a function" — aborting the
// whole script — so they must exist even though the answers are constant.
JSValue engine_is_wallpaper (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewBool (ctx, true);
}
JSValue engine_is_screensaver (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewBool (ctx, false);
}
JSValue engine_is_desktop_device (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewBool (ctx, true);
}
JSValue engine_is_mobile_device (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewBool (ctx, false);
}
JSValue engine_is_running_in_editor (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewBool (ctx, false);
}

// engine.registerAsset(file, precache?) -> a handle carrying the asset path. Consumed by
// thisScene.createLayer(). precache is a no-op here (assets load lazily on use).
JSValue engine_register_asset (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString (argv[0])) {
	return JS_UNDEFINED;
    }
    JSValue handle = JS_NewObject (ctx);
    JS_SetPropertyStr (ctx, handle, "file", JS_DupValue (ctx, argv[0]));
    return handle;
}

// Orientation from the actual render dimensions.
JSValue engine_is_portrait (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    auto* obj = static_cast<EngineObject*> (JS_GetAnyOpaque (this_val, &classId));
    bool portrait = false;
    if (obj != nullptr) {
	portrait = obj->getScene ().getCamera ().getHeight () > obj->getScene ().getCamera ().getWidth ();
    }
    return JS_NewBool (ctx, portrait);
}
JSValue engine_is_landscape (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    auto* obj = static_cast<EngineObject*> (JS_GetAnyOpaque (this_val, &classId));
    bool landscape = true;
    if (obj != nullptr) {
	landscape = obj->getScene ().getCamera ().getWidth () >= obj->getScene ().getCamera ().getHeight ();
    }
    return JS_NewBool (ctx, landscape);
}

JSValue engine_get_frametime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewFloat64 (ctx, g_Time - g_TimeLast);
}

JSValue engine_get_runtime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewFloat64 (ctx, g_Time);
}

JSValue engine_get_daytime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewFloat64 (ctx, g_Daytime);
}

// engine.screenResolution -> { x, y }. Scripts read .x/.y (e.g. the camera controller's
// init()/cursor math); a missing value would crash with "cannot read .x of undefined".
JSValue engine_get_screenresolution (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    auto* obj = static_cast<EngineObject*> (JS_GetAnyOpaque (this_val, &classId));
    JSValue result = JS_NewObject (ctx);
    float width = 1920.0f;
    float height = 1080.0f;
    if (obj != nullptr) {
	width = obj->getScene ().getCamera ().getWidth ();
	height = obj->getScene ().getCamera ().getHeight ();
    }
    JS_SetPropertyStr (ctx, result, "x", JS_NewFloat64 (ctx, width));
    JS_SetPropertyStr (ctx, result, "y", JS_NewFloat64 (ctx, height));
    return result;
}

// engine.userProperties -> { <propertyName>: <currentValue>, ... }. Script-driven wallpapers read
// their combos/sliders/checkboxes through this (e.g. Makima's style selector reads
// engine.userProperties.mode_combo / style_left / style_big). Reading a key off an undefined object
// throws, which aborts the whole visibility script and leaves every layer at its default visible —
// that's why all styles showed at once.
JSValue engine_get_userproperties (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    auto* obj = static_cast<EngineObject*> (JS_GetAnyOpaque (this_val, &classId));
    JSValue result = JS_NewObject (ctx);
    if (obj != nullptr) {
	for (const auto& [name, property] : obj->getScene ().getScene ().project.properties) {
	    if (property == nullptr) {
		continue;
	    }
	    JS_SetPropertyStr (ctx, result, name.c_str (), obj->getEngine ().dynamicToJs (*property));
	}
    }
    return result;
}

JSValue engine_stop_interval (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    if (argc != 1) {
	return JS_EXCEPTION;
    }

    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_EXCEPTION;
    }

    int id = 0;

    JS_ToInt32 (ctx, &id, argv[0]);

    it->second.clearInterval (id);

    return JS_UNDEFINED;
}

JSValue engine_stop_timeout (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    if (argc != 1) {
	return JS_EXCEPTION;
    }

    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_EXCEPTION;
    }

    int id = 0;

    JS_ToInt32 (ctx, &id, argv[0]);

    it->second.clearTimeout (id);

    return JS_UNDEFINED;
}

JSValue engine_set_interval (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    if (argc < 1) {
	return JS_EXCEPTION;
    }

    int delay = 0;

    if (argc > 1) {
	JS_ToInt32 (ctx, &delay, argv[1]);
    }

    JSValue function = argv[0];

    if (!JS_IsFunction (ctx, function)) {
	return JS_EXCEPTION;
    }

    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_EXCEPTION;
    }

    int id = it->second.reserveNextIntervalId (function, delay);

    JSValue args[] = { JS_NewInt32 (ctx, id) };

    return JS_NewCFunctionData (ctx, engine_stop_interval, 2, magic, 1, args);
}

JSValue engine_set_timeout (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    if (argc < 1) {
	return JS_EXCEPTION;
    }

    int delay = 0;

    if (argc > 1) {
	JS_ToInt32 (ctx, &delay, argv[1]);
    }

    JSValue function = argv[0];

    if (!JS_IsFunction (ctx, function)) {
	return JS_EXCEPTION;
    }

    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_EXCEPTION;
    }

    int id = it->second.reserveNextTimeoutId (function, delay);

    JSValue args[] = { JS_NewInt32 (ctx, id) };

    return JS_NewCFunctionData (ctx, engine_stop_timeout, 2, magic, 1, args);
}

// Getter for the `average` property of the object returned by
// engine.registerAudioBuffers(). Returns a fresh JS array of the current
// frequency-band levels so scripts always read live audio. func_data[0] holds
// the resolution (16/32/64); magic holds the engine instance id.
JSValue engine_audio_buffer_average (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    int resolution = 64;
    JS_ToInt32 (ctx, &resolution, func_data[0]);

    JSValue arr = JS_NewArray (ctx);

    const auto it = engineInstances.find (magic);
    if (it == engineInstances.end ()) {
	return arr;
    }

    const auto& recorder = it->second.getScene ().getAudioContext ().getRecorder ();
    const float* data = resolution == 16 ? recorder.audio16 : (resolution == 32 ? recorder.audio32 : recorder.audio64);

    for (int i = 0; i < resolution; i++) {
	JS_SetPropertyUint32 (ctx, arr, i, JS_NewFloat64 (ctx, data[i]));
    }

    return arr;
}

// engine.registerAudioBuffers(resolution) -> { average: [<resolution> floats] }
// Used by audio-reactive scripts (particle rates, visualizer bars).
JSValue engine_register_audio_buffers (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    int resolution = 64;
    if (argc >= 1) {
	JS_ToInt32 (ctx, &resolution, argv[0]);
    }
    if (resolution != 16 && resolution != 32 && resolution != 64) {
	resolution = 64;
    }

    JSValue obj = JS_NewObject (ctx);
    JSValue data[] = { JS_NewInt32 (ctx, resolution) };
    JSValue getter = JS_NewCFunctionData (ctx, engine_audio_buffer_average, 0, magic, 1, data);
    JS_FreeValue (ctx, data[0]);

    JS_DefinePropertyGetSet (
	ctx, obj, JS_NewAtom (ctx, "average"), getter, JS_UNDEFINED, JS_PROP_ENUMERABLE
    );

    return obj;
}

EngineObject::EngineObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine), m_instanceId (++EngineInstanceId), m_classId (0) {
    this->m_definition = { .class_name = "IEngine" };
    JS_NewClassID (this->m_engine.getRuntime (), &this->m_classId);
    JS_NewClass (this->m_engine.getRuntime (), this->m_classId, &this->m_definition);
    this->m_instance = JS_NewObjectClass (this->m_engine.getContext (), this->m_classId);

    JS_DupValue (this->m_engine.getContext (), this->m_instance);

    // set properties
    JS_SetOpaque (this->m_instance, this);
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "frametime"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_frametime, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "runtime"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_runtime, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "timeOfDay"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_daytime, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "screenResolution"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_screenresolution, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    // canvasSize is the wallpaper draw size; alias it to the screen resolution (close enough for the
    // position math scripts do with it).
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "canvasSize"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_screenresolution, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "userProperties"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_userproperties, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_16",
	JS_NewInt32 (this->m_engine.getContext (), 16), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_32",
	JS_NewInt32 (this->m_engine.getContext (), 32), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_64",
	JS_NewInt32 (this->m_engine.getContext (), 64), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "setInterval",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_set_interval, "setInterval", 2, JS_CFUNC_generic_magic,
	    this->m_instanceId
	),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "setTimeout",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_set_timeout, "setTimeout", 2, JS_CFUNC_generic_magic,
	    this->m_instanceId
	),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "openUserShortcut",
	JS_NewCFunction (this->m_engine.getContext (), engine_open_user_shortcut, "openUserShortcut", 0),
	JS_PROP_ENUMERABLE
    );
    // Context-query methods (constant truths for a desktop wallpaper).
    const auto defBool = [&] (const char* name, JSCFunction* fn) {
	JS_DefinePropertyValueStr (
	    this->m_engine.getContext (), this->m_instance, name,
	    JS_NewCFunction (this->m_engine.getContext (), fn, name, 0), JS_PROP_ENUMERABLE
	);
    };
    defBool ("isWallpaper", engine_is_wallpaper);
    defBool ("isScreensaver", engine_is_screensaver);
    defBool ("isDesktopDevice", engine_is_desktop_device);
    defBool ("isMobileDevice", engine_is_mobile_device);
    defBool ("isRunningInEditor", engine_is_running_in_editor);
    defBool ("isPortrait", engine_is_portrait);
    defBool ("isLandscape", engine_is_landscape);
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "registerAsset",
	JS_NewCFunction (this->m_engine.getContext (), engine_register_asset, "registerAsset", 2), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "registerAudioBuffers",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_register_audio_buffers, "registerAudioBuffers", 1,
	    JS_CFUNC_generic_magic, this->m_instanceId
	),
	JS_PROP_ENUMERABLE
    );
    // TODO: ADD THE REST OF THE DEFINITION!

    // Register this instance so the magic-tagged C functions (setInterval,
    // setTimeout, registerAudioBuffers, ...) can find it by id. The destructor
    // erases it; without this insert every lookup fails and those APIs no-op.
    engineInstances.emplace (this->m_instanceId, *this);
}

EngineObject::~EngineObject () {
    // clear all the timeouts and intervals
    for (const auto& [id, timeout] : this->m_timeouts) {
	JS_FreeValue (this->m_engine.getContext (), timeout.callback);
    }
    for (const auto& [id, interval] : this->m_intervals) {
	JS_FreeValue (this->m_engine.getContext (), interval.callback);
    }

    engineInstances.erase (this->m_instanceId);
    this->m_intervals.clear ();
    this->m_timeouts.clear ();

    JS_FreeValue (this->m_engine.getContext (), this->m_instance);
}

uint32_t EngineObject::reserveNextTimeoutId (JSValue function, uint64_t duration) {
    const auto id = ++this->m_nextTimeoutId;

    this->m_timeouts[id] = Timeout { .callback = function,
				     .duration = std::chrono::milliseconds (duration),
				     .next = std::chrono::steady_clock::now () + std::chrono::milliseconds (duration) };

    return id;
}

uint32_t EngineObject::reserveNextIntervalId (JSValue function, uint64_t duration) {
    const auto id = ++this->m_nextIntervalId;

    this->m_intervals[id]
	= Timeout { .callback = function,
		    .duration = std::chrono::milliseconds (duration),
		    .next = std::chrono::steady_clock::now () + std::chrono::milliseconds (duration) };

    return id;
}

void EngineObject::clearInterval (uint32_t id) {
    const auto it = this->m_intervals.find (id);

    if (it == this->m_intervals.end ()) {
	return;
    }

    JS_FreeValue (this->getEngine ().getContext (), it->second.callback);

    this->m_intervals.erase (id);
}

void EngineObject::clearTimeout (uint32_t id) {
    const auto it = this->m_timeouts.find (id);

    if (it == this->m_timeouts.end ()) {
	return;
    }

    JS_FreeValue (this->getEngine ().getContext (), it->second.callback);

    this->m_timeouts.erase (id);
}

void EngineObject::tick () {
    const auto now = std::chrono::steady_clock::now ();

    // check any interval and run them if needed
    for (auto& timeout : this->m_intervals | std::views::values) {
	if (timeout.next > now) {
	    continue;
	}

	timeout.next = now + timeout.duration;

	JS_Call (this->m_engine.getContext (), timeout.callback, JS_NULL, 0, nullptr);
    }

    std::vector<uint32_t> removeTimeouts;

    // check any timeout and run them if needed
    for (auto& [id, timeout] : this->m_timeouts) {
	if (timeout.next > now) {
	    continue;
	}

	JS_Call (this->m_engine.getContext (), timeout.callback, JS_NULL, 0, nullptr);

	JS_FreeValue (this->m_engine.getContext (), timeout.callback);

	removeTimeouts.push_back (id);
    }

    for (auto id : removeTimeouts) {
	this->m_timeouts.erase (id);
    }
}