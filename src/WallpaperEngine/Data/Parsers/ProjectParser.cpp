#include <algorithm>

#include "ProjectParser.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperParser.h"

#include "PropertyParser.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/FileSystem/Container.h"

using namespace WallpaperEngine::Data::Parsers;

static int backgroundId = 0;

ProjectUniquePtr ProjectParser::parse (const JSON& data, AssetLocatorUniquePtr container) {
    const auto general = data.optional ("general");
    const auto workshopId = data.optional ("workshopid");
    auto actualWorkshopId = std::to_string (--backgroundId);
    // The declared "type" string is only a label in WE; the real decision is the main file's
    // extension / URL scheme. Keep it as a fallback, but don't require it.
    auto type = data.optional<std::string> ("type", "");
    const auto fileValue = data.require ("file", "Project's main file missing");
    const std::string fileName = fileValue.is_string () ? fileValue.template get<std::string> () : "";

    if (workshopId.has_value ()) {
	if (workshopId->is_number ()) {
	    actualWorkshopId = std::to_string (workshopId->get<int> ());
	} else if (workshopId->is_string ()) {
	    actualWorkshopId = workshopId->get<std::string> ();
	} else {
	    sLog.error ("Invalid workshop id: ", workshopId->dump ());
	}
    }

    // lowercase for consistency
    std::ranges::transform (type, type.begin (), tolower);

    auto result = std::make_unique<Project> (Project {
	.title = data.require<std::string> ("title", "Project title missing"),
	.type = parseType (type, fileName),
	.workshopId = actualWorkshopId,
	.supportsAudioProcessing = general.has_value () && general.value ().optional ("supportsaudioprocessing", false),
	.properties = parseProperties (general),
	.assetLocator = std::move (container),
    });

    result->wallpaper = WallpaperParser::parse (fileValue, *result);

    return result;
}

Project::Type ProjectParser::parseType (const std::string& type, const std::string& file) {
    // Wallpaper Engine decides the wallpaper kind from the main file's extension / URL scheme,
    // NOT the declared "type" string (which it only stores as a label) — FUN_14011e530. Mirror
    // that exactly: a scene ships a .json/.pkg file, a video a .mp4/..., web an .html (or a bare
    // URL), an image a .png/..., an application an .exe. The declared string is only a fallback
    // for an ambiguous/missing file.
    std::string f = file;
    std::ranges::transform (f, f.begin (), tolower);

    if (f.starts_with ("http://") || f.starts_with ("https://") || f.starts_with ("www.")) {
	return Project::Type_Web;
    }

    const auto dot = f.find_last_of ('.');
    const std::string ext = dot != std::string::npos ? f.substr (dot + 1) : "";

    if (ext == "json" || ext == "pkg") {
	return Project::Type_Scene;
    }
    if (ext == "html" || ext == "htm") {
	return Project::Type_Web;
    }
    if (ext == "mp4" || ext == "webm" || ext == "mkv" || ext == "avi" || ext == "mov" || ext == "m4v" || ext == "wmv") {
	return Project::Type_Video;
    }
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "bmp" || ext == "webp") {
	return Project::Type_Image;
    }
    if (ext == "exe") {
	return Project::Type_Application;
    }

    // Ambiguous file (e.g. a bare URL with no scheme) — trust the declared type string.
    if (type == "scene") {
	return Project::Type_Scene;
    }
    if (type == "video") {
	return Project::Type_Video;
    }
    if (type == "web") {
	return Project::Type_Web;
    }
    if (type == "application") {
	return Project::Type_Application;
    }
    if (type == "image") {
	return Project::Type_Image;
    }

    sLog.exception ("Cannot determine project type from file '", file, "' (declared type '", type, "')");
}

Properties ProjectParser::parseProperties (const std::optional<JSON>& data) {
    if (!data.has_value ()) {
	return {};
    }

    const auto properties = data.value ().optional ("properties");

    if (!properties.has_value ()) {
	return {};
    }

    Properties result = {};

    for (const auto& cur : properties.value ().items ()) {
	const auto& property = PropertyParser::parse (cur.value (), cur.key ());

	// ignore properties that failed, these are generally groups
	if (property == nullptr) {
	    continue;
	}

	result.emplace (cur.key (), property);
    }

    return result;
}
