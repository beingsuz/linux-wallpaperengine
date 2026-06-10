#include "WallpaperParser.h"

#include "ObjectParser.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Data::Parsers;

WallpaperUniquePtr WallpaperParser::parse (const JSON& file, Project& project) {
    switch (project.type) {
	case Project::Type_Scene:
	    return parseScene (file, project);
	case Project::Type_Video:
	    return parseVideo (file, project);
	case Project::Type_Web:
	    return parseWeb (file, project);
	case Project::Type_Image:
	    // Static image wallpaper. WE draws it as a fullscreen textured quad; the fork has no
	    // dedicated image renderer yet (the daemon shows the preview instead).
	    sLog.exception ("Image wallpapers are not yet supported by this engine");
	case Project::Type_Application:
	    // Application wallpapers re-target a running app's swap chain via Windows DLL injection;
	    // no portable Linux equivalent exists in this engine.
	    sLog.exception ("Application wallpapers are not supported on this platform");
	default:
	    sLog.exception ("Unexpected project type value found... This is likely a bug");
    }
}

SceneUniquePtr WallpaperParser::parseScene (const JSON& file, Project& project) {
    const auto scene = JSON::parse (project.assetLocator->readString (file));
    const auto camera = scene.require ("camera", "Scenes must have a camera section");
    const auto general = scene.require ("general", "Scenes must have a general section");
    // Some scenes ship "orthogonalprojection": null (or omit width/height),
    // expecting the projection to be auto-sized to the screen. Treat any of
    // those cases as auto instead of throwing "Projection must have a width".
    const auto projectionOpt = general.optional ("orthogonalprojection");
    const bool projectionAuto =
	!projectionOpt.has_value () || projectionOpt->optional ("auto", false);
    const auto objects = scene.require ("objects", "Scenes must have an objects section");
    const auto& properties = project.properties;

    // TODO: FIND IF THESE DEFAULTS ARE SENSIBLE OR NOT AND PERFORM PROPER VALIDATION WHEN CAMERA PREVIEW AND CAMERA
    // PARALLAX ARE PRESENT

    return std::make_unique <Scene> (
        WallpaperData {
            .filename = "",
            .project = project
        }, SceneData {
            .colors = {
                .ambient  = general.user ("ambientcolor", properties, glm::vec3 (0.0f)),
                .skylight = general.user ("skylightcolor", properties, glm::vec3 (0.0f)),
                .clear = general.user ("clearcolor", properties, glm::vec3 (1.0f)),
            },
            .camera = {
                .fade = general.user ("camerafade", properties, false),
                .preview = general.optional ("camerapreview", false),
                .bloom = {
                    .enabled = general.user ("bloom", properties, false),
                    .strength = general.user ("bloomstrength", properties, 0.0f),
                    .threshold = general.user ("bloomthreshold", properties, 0.0f),
                },
                .parallax = {
                    .enabled = general.user ("cameraparallax", properties, false),
                    .amount = general.user ("cameraparallaxamount", properties, 1.0f),
                    .delay = general.user ("cameraparallaxdelay", properties, 0.0f),
                    .mouseInfluence = general.user ("cameraparallaxmouseinfluence", properties, 1.0f),
                },
                .shake = {
                    .enabled = general.user ("camerashake", properties, false),
                    .amplitude = general.user ("camerashakeamplitude", properties, 0.0f),
                    .roughness = general.user ("camerashakeroughness", properties, 0.0f),
                    .speed = general.user ("camerashakespeed", properties, 0.0f),
                },
                .configuration = {
                    .center = camera.require <glm::vec3> ("center", "Camera must have a center position"),
                    .eye = camera.require <glm::vec3> ("eye", "Camera must have an eye position"),
                    .up = camera.require <glm::vec3> ("up", "Camera must have an up position"),
                },
                .projection = {
                    .width  = projectionAuto ? 0 : projectionOpt->optional <int> ("width",  0),
                    .height = projectionAuto ? 0 : projectionOpt->optional <int> ("height", 0),
                    .isAuto = projectionAuto,
                    // Wallpaper Engine's perspective camera defaults (verified in the binary at scene
                    // offsets nearz=0x14c, farz=0x150, fov=0x140): nearz 0.1, farz 10000, fov 50°.
                    // These feed the perspective projection used by 3D models and perspective particle
                    // systems — a near of 0 makes glm::perspective degenerate and far 1000 clips large
                    // or distant 3D content (Starscape's model + planets vanish). The 2D orthographic
                    // compositor does NOT use these directly (Camera::setOrthogonalProjection forces
                    // near 0 so flat layers at z=0 are never clipped).
                    .nearz = camera.user ("nearz", properties, 0.1f),
                    .farz = camera.user ("farz", properties, 10000.0f),
                    // Wallpaper Engine declares fov under `general` (often property-bound, e.g.
                    // Starscape's general.fov = {user:'fov', value:50}); older scenes put it under
                    // `camera`. Prefer general so the user `fov` property actually drives the camera,
                    // falling back to camera for compatibility.
                    .fov = general.optional ("fov").has_value ()
			       ? general.user ("fov", properties, 50.0f)
			       : camera.user ("fov", properties, 50.0f)
                }
            },
            .customSortOrder = general.optional<bool> ("customsortorder", false),
            .objects = parseObjects (objects, project),
        }
    );
}

VideoUniquePtr WallpaperParser::parseVideo (const JSON& file, Project& project) {
    return std::make_unique<Video> (WallpaperData { .filename = file, .project = project });
}

WebUniquePtr WallpaperParser::parseWeb (const JSON& file, Project& project) {
    return std::make_unique<Web> (WallpaperData {
	.filename = file,
	.project = project,
    });
}

ObjectList WallpaperParser::parseObjects (const JSON& objects, const Project& project) {
    ObjectList result = {};

    for (const auto& cur : objects) {
	result.emplace_back (ObjectParser::parse (cur, project));
    }

    return result;
}