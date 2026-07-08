#include "WallpaperApplication.h"
#include <chrono>
#include <cmath>

#include <sstream>

#include "Steam/FileSystem/FileSystem.h"
#include "WallpaperEngine/Application/ApplicationState.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Audio/Drivers/Detectors/PulseAudioPlayingDetector.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Drivers/VideoFactories.h"
#include "WallpaperEngine/Render/RenderContext.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include "WallpaperEngine/Data/Dumpers/StringPrinter.h"
#include "WallpaperEngine/Data/Parsers/ProjectParser.h"

#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Debugging/CallStack.h"
#include "WallpaperEngine/FileSystem/Adapters/MediaCover.h"
#include "WallpaperEngine/Media/DBusMediaSource.h"

#if DEMOMODE
#include "recording.h"
#endif /* DEMOMODE */

#include <algorithm>
#include <climits>
#include <numeric>
#include <unistd.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <thread>

#define FULLSCREEN_CHECK_WAIT_TIME 250

float g_Time;
float g_TimeLast;
float g_Daytime;

using namespace WallpaperEngine::Assets;
using namespace WallpaperEngine::Application;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::FileSystem;

void CustomGLDebugCallback (
    GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam
) {
    if (severity != GL_DEBUG_SEVERITY_HIGH) {
	return;
    }

    sLog.error ("OpenGL error: ", message, ", type: ", type, ", id: ", id);

    std::vector<WallpaperEngine::Debugging::CallStack::CallInfo> callInfo;

    WallpaperEngine::Debugging::CallStack::GetCalls (callInfo);

    for (std::vector<WallpaperEngine::Debugging::CallStack::CallInfo>::size_type i = 0; i < callInfo.size (); ++i) {
	fprintf (
	    stderr, "[%3lu] %15lu: %s in %s\n", callInfo.size () - i, callInfo[i].offset, callInfo[i].function.c_str (),
	    callInfo[i].module.c_str ()
	);
    }
}

WallpaperApplication::WallpaperApplication (ApplicationContext& context) : m_context (context) {
    this->initializeSubsystems ();
    this->loadBackgrounds ();
    this->setupProperties ();
    this->setupBrowser ();
    this->initializePlaylists ();
}

void WallpaperApplication::initializeSubsystems () {
    // initialize player dbus (update every 2 seconds)
    m_mediaSource = std::make_unique<WallpaperEngine::Media::DBusMediaSource> (std::chrono::milliseconds (2000));
}

AssetLocatorUniquePtr WallpaperApplication::setupAssetLocator (const std::string& bg) const {
    auto container = std::make_unique<Container> ();

    const std::filesystem::path path = bg;

    container->registerAdapterFactory (std::make_unique<MediaCoverFactory> (*this->m_mediaSource));
    container->mount ("$mediaThumbnail", "$mediaThumbnail");
    container->mount (path, "/");

    try {
	container->mount (path / "scene.pkg", "/");
    } catch (std::runtime_error&) { }

    try {
	container->mount (path / "gifscene.pkg", "/");
    } catch (std::runtime_error&) { }

    try {
	container->mount (this->m_context.settings.general.assets, "/");
    } catch (std::runtime_error&) {
	sLog.exception ("Cannot find a valid assets folder, resolved to ", this->m_context.settings.general.assets);
    }

    // mount the current directory as root
    try {
	container->mount (std::filesystem::current_path (), "/");
    } catch (std::runtime_error&) { }

    auto& vfs = container->getVFS ();

    //
    // Had to get a little creative with the effects to achieve the same bloom effect without any custom code
    // these virtual files are loaded by an image in the scene that takes current _rt_FullFrameBuffer and
    // applies the bloom effect to render it out to the screen
    //

    // add the effect file for screen bloom

    // add some model for the image element even if it's going to waste rendering cycles
    vfs.add (
	"effects/wpenginelinux/bloomeffect.json",
	{ { "name", "camerabloom_wpengine_linux" },
	  { "group", "wpengine_linux_camera" },
	  { "dependencies", JSON::array () },
	  {
	      "passes",
	      JSON::array (
		  { { { "material", "materials/util/downsample_quarter_bloom.json" },
		      { "target", "_rt_4FrameBuffer" },
		      { "bind", JSON::array ({ { { "name", "_rt_FullFrameBuffer" }, { "index", 0 } } }) } },
		    { { "material", "materials/util/downsample_eighth_blur_v.json" },
		      { "target", "_rt_8FrameBuffer" },
		      { "bind", JSON::array ({ { { "name", "_rt_4FrameBuffer" }, { "index", 0 } } }) } },
		    { { "material", "materials/util/blur_h_bloom.json" },
		      { "target", "_rt_Bloom" },
		      { "bind", JSON::array ({ { { "name", "_rt_8FrameBuffer" }, { "index", 0 } } }) } },
		    { { "material", "materials/util/combine.json" },
		      { "target", "_rt_FullFrameBuffer" },
		      { "bind",
			JSON::array (
			    { { { "name", "_rt_imageLayerComposite_-1_a" }, { "index", 0 } },
			      { { "name", "_rt_Bloom" }, { "index", 1 } } }
			) } } }
	      ),
	  } }
    );

    vfs.add ("models/wpenginelinux.json", { { "material", "materials/wpenginelinux.json" } });

    vfs.add (
	"materials/wpenginelinux.json",
	{ { "passes",
	    JSON::array (
		{ { { "blending", "normal" },
		    { "cullmode", "nocull" },
		    { "depthtest", "disabled" },
		    { "depthwrite", "disabled" },
		    { "shader", "genericimage2" },
		    { "textures", JSON::array ({ "_rt_FullFrameBuffer" }) } } }
	    ) } }
    );

    vfs.add (
	"shaders/commands/copy.frag",
	"uniform sampler2D g_Texture0;\n"
	"in vec2 v_TexCoord;\n"
	"void main () {\n"
	"out_FragColor = texture (g_Texture0, v_TexCoord);\n"
	"}"
    );
    vfs.add (
	"shaders/commands/copy.vert",
	"in vec3 a_Position;\n"
	"in vec2 a_TexCoord;\n"
	"out vec2 v_TexCoord;\n"
	"void main () {\n"
	"gl_Position = vec4 (a_Position, 1.0);\n"
	"v_TexCoord = a_TexCoord;\n"
	"}"
    );

    return std::make_unique<AssetLocator> (std::move (container));
}

void WallpaperApplication::loadBackgrounds () {
    if (this->m_context.settings.render.mode == ApplicationContext::NORMAL_WINDOW
	|| this->m_context.settings.render.mode == ApplicationContext::EXPLICIT_WINDOW) {
	auto path = this->m_context.settings.general.defaultBackground;

	if (this->m_context.settings.general.defaultPlaylist.has_value ()
	    && !this->m_context.settings.general.defaultPlaylist->items.empty ()) {
	    path = this->m_context.settings.general.defaultPlaylist->items.front ();
	}

	this->m_backgrounds["default"] = this->loadBackground (path);
	// record resolved path so live-control rebuilds can reload it via screenBackgrounds
	this->m_context.settings.general.screenBackgrounds["default"] = path;
	return;
    }

    for (auto& [screen, path] : this->m_context.settings.general.screenBackgrounds) {
	// skip span group synthetic keys here, they're handled below
	if (screen.rfind ("span:", 0) == 0) {
	    continue;
	}
	// screens with no path should use the default; write it back so live-control can reload this screen
	if (path.empty ()) {
	    path = this->m_context.settings.general.defaultBackground;
	}

	this->m_backgrounds[screen] = this->loadBackground (path);
    }

    // Load one background per span group
    for (const auto& spanGroup : this->m_context.settings.general.spanGroups) {
	if (spanGroup.screens.empty ()) {
	    continue;
	}

	std::filesystem::path bgPath = spanGroup.background;
	if (bgPath.empty ()) {
	    bgPath = this->m_context.settings.general.defaultBackground;
	}

	// use the first screen's name as the group key for the loaded project
	const std::string groupKey = "span:" + spanGroup.screens.front ();
	this->m_backgrounds[groupKey] = this->loadBackground (bgPath);
    }
}

ProjectUniquePtr WallpaperApplication::loadBackground (const std::string& bg) {
    auto container = this->setupAssetLocator (bg);
    auto json = WallpaperEngine::Data::JSON::JSON::parse (container->readString ("project.json"));

    // when a background is loaded, reset the screenshot variables
    // this allows taking screenshots after a background changes
    // useful for playlists
    if (this->m_context.settings.screenshot.take) {
	this->m_nextFrameScreenshot = this->m_context.settings.screenshot.delay;

	if (this->m_videoDriver != nullptr) {
	    this->m_nextFrameScreenshot += this->m_videoDriver->getFrameCounter ();
	}

	this->m_screenShotTaken = false;
    }

    return WallpaperEngine::Data::Parsers::ProjectParser::parse (json, std::move (container));
}

std::vector<std::size_t>
WallpaperApplication::buildPlaylistOrder (const ApplicationContext::PlaylistDefinition& definition) {
    std::vector<std::size_t> order (definition.items.size ());
    std::iota (order.begin (), order.end (), 0);

    if (definition.settings.order == "random") {
	std::shuffle (order.begin (), order.end (), this->m_playlistRng);
    }

    return order;
}

void WallpaperApplication::initializePlaylists () {
    const bool hasDefaultPlaylist = this->m_context.settings.general.defaultPlaylist.has_value ();
    const bool hasScreenPlaylists = !this->m_context.settings.general.screenPlaylists.empty ();

    if (!hasDefaultPlaylist && !hasScreenPlaylists) {
	return;
    }

    const auto now = std::chrono::steady_clock::now ();

    auto registerPlaylist = [this, now] (
				const std::string& key, const ApplicationContext::PlaylistDefinition& playlist,
				std::optional<std::filesystem::path> currentPath
			    ) {
	if (playlist.items.empty ()) {
	    return;
	}

	ActivePlaylist state;

	state.definition = playlist;
	state.order = this->buildPlaylistOrder (playlist);

	if (state.order.empty ()) {
	    return;
	}

	if (currentPath.has_value ()) {
	    state.orderIndex = 0;

	    for (std::size_t i = 0; i < state.order.size (); i++) {
		if (playlist.items[state.order[i]] == currentPath.value ()) {
		    state.orderIndex = i;
		    break;
		}
	    }
	}

	const uint32_t delayMinutes = std::max<uint32_t> (1, state.definition.settings.delayMinutes);
	state.nextSwitch = now + std::chrono::minutes (delayMinutes);
	state.lastUpdate = now;

	this->m_activePlaylists.insert_or_assign (key, std::move (state));
    };

    if (hasDefaultPlaylist
	&& (this->m_context.settings.render.mode == ApplicationContext::NORMAL_WINDOW
	    || this->m_context.settings.render.mode == ApplicationContext::EXPLICIT_WINDOW)) {
	const auto& playlist = this->m_context.settings.general.defaultPlaylist.value ();
	const auto currentPath = playlist.items.empty ()
	    ? std::optional<std::filesystem::path> { this->m_context.settings.general.defaultBackground }
	    : std::optional<std::filesystem::path> { playlist.items.front () };
	registerPlaylist ("default", playlist, currentPath);
    }

    for (const auto& [screen, playlist] : this->m_context.settings.general.screenPlaylists) {
	const auto current = this->m_context.settings.general.screenBackgrounds.find (screen);
	const auto currentPath = current != this->m_context.settings.general.screenBackgrounds.end ()
	    ? std::optional<std::filesystem::path> { current->second }
	    : std::nullopt;
	registerPlaylist (screen, playlist, currentPath);
    }
}

void WallpaperApplication::ensureBrowserForProject (const Project& project) {
    if (!project.wallpaper->is<Web> ()) {
	return;
    }

    if (!this->m_browserContext) {
	this->m_browserContext = std::make_unique<WebBrowser::WebBrowserContext> (*this);
    }
}

bool WallpaperApplication::makeAnyViewportCurrent () const {
    if (!this->m_renderContext) {
	return false;
    }

    const auto& viewports = this->m_renderContext->getOutput ().getViewports ();

    if (viewports.empty ()) {
	return false;
    }

    viewports.begin ()->second->makeCurrent ();
    return true;
}

bool WallpaperApplication::preflightWallpaper (const std::string& path) {
    try {
	// avoid mutating state, just ensure project.json parses
	auto container = this->setupAssetLocator (path);
	const auto json = WallpaperEngine::Data::JSON::JSON::parse (container->readString ("project.json"));
	if (!json.contains ("type") || !json.contains ("file")) {
	    sLog.error ("Preflight failed for ", path, ": missing required fields");
	    return false;
	}
	return true;
    } catch (const std::exception& e) {
	sLog.error ("Preflight failed for ", path, ": ", e.what ());
	return false;
    }
}

bool WallpaperApplication::selectNextCandidate (ActivePlaylist& playlist, std::size_t& outOrderIndex) {
    if (playlist.order.empty ()) {
	return false;
    }

    std::size_t attempts = 0;
    std::size_t candidateOrderIndex = outOrderIndex;

    while (attempts < playlist.order.size ()) {
	const auto candidateIndex = playlist.order[candidateOrderIndex];

	if (!playlist.failedIndices.contains (candidateIndex)) {
	    outOrderIndex = candidateOrderIndex;
	    return true;
	}

	attempts++;
	candidateOrderIndex = (candidateOrderIndex + 1) % playlist.order.size ();
    }

    return false;
}

void WallpaperApplication::advancePlaylist (
    const std::string& screen, ActivePlaylist& playlist, const std::chrono::steady_clock::time_point& now
) {
    if (playlist.order.empty ()) {
	return;
    }

    playlist.orderIndex = (playlist.orderIndex + 1) % playlist.order.size ();

    if (playlist.orderIndex == 0 && playlist.definition.settings.order == "random") {
	std::shuffle (playlist.order.begin (), playlist.order.end (), this->m_playlistRng);
    }

    std::size_t candidateOrderIndex = playlist.orderIndex;

    if (!this->selectNextCandidate (playlist, candidateOrderIndex)) {
	sLog.error ("All playlist items failed for ", screen, ", keeping current wallpaper");
	const uint32_t delayMinutes = std::max<uint32_t> (1, playlist.definition.settings.delayMinutes);
	playlist.nextSwitch = now + std::chrono::minutes (delayMinutes);
	return;
    }

    const auto candidateIndex = playlist.order[candidateOrderIndex];
    const auto& candidatePath = playlist.definition.items[candidateIndex];

    if (!this->preflightWallpaper (candidatePath.string ())) {
	playlist.failedIndices.insert (candidateIndex);

	if (!this->selectNextCandidate (playlist, candidateOrderIndex)) {
	    sLog.error ("All playlist items failed for ", screen, ", keeping current wallpaper");
	    const uint32_t delayMinutes = std::max<uint32_t> (1, playlist.definition.settings.delayMinutes);
	    playlist.nextSwitch = now + std::chrono::minutes (delayMinutes);
	    return;
	}
    }

    playlist.orderIndex = candidateOrderIndex;
    const auto& nextPath = playlist.definition.items[playlist.order[playlist.orderIndex]];

    bool loaded = false;

    if (this->makeAnyViewportCurrent ()) {
	loaded = this->setBackground (screen, nextPath.string ());
    } else {
	sLog.error ("Cannot switch playlist on ", screen, ": no active viewport");
    }

    if (!loaded) {
	playlist.failedIndices.insert (playlist.order[playlist.orderIndex]);

	// Keep current position; next timer tick will retry advancement
	sLog.error ("Failed to load wallpaper for ", screen, ", will retry on next cycle");
    }

    const uint32_t delayMinutes = std::max<uint32_t> (1, playlist.definition.settings.delayMinutes);
    playlist.nextSwitch = now + std::chrono::minutes (delayMinutes);
}

void WallpaperApplication::updatePlaylists () {
    if (this->m_activePlaylists.empty ()) {
	return;
    }

    const auto now = std::chrono::steady_clock::now ();

    for (auto& [screen, playlist] : this->m_activePlaylists) {
	playlist.lastUpdate = now;

	if (playlist.definition.settings.mode != "timer") {
	    continue;
	}

	if (playlist.definition.items.size () <= 1) {
	    continue;
	}

	if (now < playlist.nextSwitch) {
	    continue;
	}

	this->advancePlaylist (screen, playlist, now);
    }
}

void WallpaperApplication::setupPropertiesForProject (const Project& project) {
    const bool listJson = this->m_context.settings.general.listPropertiesJson;
    std::vector<nlohmann::json> jsonProperties;

    // show properties if required
    for (const auto& [key, cur] : project.properties) {
	// update the value of the property
	auto override = this->m_context.settings.general.properties.find (key);

	if (override != this->m_context.settings.general.properties.end ()) {
	    if (!listJson) {
		sLog.out ("Applying override value for ", key);
	    }

	    cur->update (override->second, DynamicValue::UpdateSource::User);
	}

	if (this->m_context.settings.general.onlyListProperties) {
	    sLog.out (cur->dump ());
	}

	if (listJson) {
	    jsonProperties.push_back (cur->dumpJson ());
	}
    }

    if (listJson) {
	// Stable order the UI relies on: "order" field, then key as tiebreaker.
	std::ranges::stable_sort (jsonProperties, [] (const nlohmann::json& a, const nlohmann::json& b) {
	    const int oa = a.value ("order", 0);
	    const int ob = b.value ("order", 0);
	    if (oa != ob) {
		return oa < ob;
	    }
	    return a.value ("key", std::string {}) < b.value ("key", std::string {});
	});
	std::cout << nlohmann::json (jsonProperties).dump () << std::endl;
    }
}

void WallpaperApplication::setupProperties () {
    for (const auto& [background, info] : this->m_backgrounds) {
	this->setupPropertiesForProject (*info);
    }
}

void WallpaperApplication::setupBrowser () {
    // Init CEF here before the GL/EGL context exists: a late CefInitialize (live web-wallpaper swap)
    // aborts the engine. Pay the helper-process cost up front regardless of initial background type.
    if (this->m_browserContext) {
	return;
    }

    this->m_browserContext = std::make_unique<WebBrowser::WebBrowserContext> (*this);
}

bool WallpaperApplication::captureScreenshot (const std::filesystem::path& path) const {
    if (path.empty () || this->m_renderContext == nullptr || this->m_renderContext->getWallpapers ().empty ()) {
	return false;
    }

    // Reuse the startup --screenshot FBO-readback path; we're on the render thread so the GL context
    // is current and the wallpaper FBO holds the last rendered frame.
    this->takeScreenshot (path);
    return true;
}

void WallpaperApplication::takeScreenshot (const std::filesystem::path& filename) const {
    const int width = this->m_renderContext->getOutput ().getFullWidth ();
    const int height = this->m_renderContext->getOutput ().getFullHeight ();
    const bool vflip = this->m_renderContext->getOutput ().renderVFlip ();
    const auto& wallpapers = this->m_renderContext->getWallpapers ();

    struct ViewportCapture {
	uint8_t* buffer;
	int readWidth;
	int readHeight;
	int vpWidth;
	int vpHeight;
	int xoffset;
	float ustart, uend, vstart, vend;
    };

    std::vector<ViewportCapture> captures;
    int currentXOffset = 0;

    for (const auto& [screen, viewport] : this->m_renderContext->getOutput ().getViewports ()) {
	// activate opengl context so we can read from the framebuffer
	viewport->makeCurrent ();

	// find the wallpaper for this screen to read from its FBO
	const auto wallpaperIt = wallpapers.find (screen);
	if (wallpaperIt == wallpapers.end ()) {
	    sLog.error ("Cannot find wallpaper for screen ", screen);
	    continue;
	}

	const auto& wallpaper = wallpaperIt->second;
	const int vpWidth = viewport->viewport.z - viewport->viewport.x;
	const int vpHeight = viewport->viewport.w - viewport->viewport.y;

	// bind the wallpaper's FBO to read from it directly
	// this is more reliable than the default framebuffer on some drivers (NVIDIA/Wayland)
	glBindFramebuffer (GL_FRAMEBUFFER, wallpaper->getWallpaperFramebuffer ());

	// ensure rendering is complete before reading
	glFinish ();

	// make room for storing the pixel of this viewport. The FBO is at the supersampled
	// (--render-scale) size, so read it back at that resolution to capture the whole frame.
	const float renderScale = wallpaper->getRenderScale ();
	const int readWidth = static_cast<int> (std::lround (wallpaper->getWidth () * renderScale));
	const int readHeight = static_cast<int> (std::lround (wallpaper->getHeight () * renderScale));
	const auto bufferSize = readWidth * readHeight * 3;
	auto* buffer = new uint8_t[bufferSize];

	// read the FBO data into the pixel buffer
	glPixelStorei (GL_PACK_ALIGNMENT, 1);
	if (GLEW_VERSION_4_5) {
	    glReadnPixels (0, 0, readWidth, readHeight, GL_RGB, GL_UNSIGNED_BYTE, bufferSize, buffer);
	} else {
	    glReadPixels (0, 0, readWidth, readHeight, GL_RGB, GL_UNSIGNED_BYTE, buffer);
	}

	// restore default framebuffer
	glBindFramebuffer (GL_FRAMEBUFFER, 0);

	if (const GLenum error = glGetError (); error != GL_NO_ERROR) {
	    sLog.error ("Cannot obtain pixel data for screen ", screen, ". OpenGL error: ", error);
	    delete[] buffer;
	    continue;
	}

	// Get the UV coordinates which define the visible portion based on scaling mode
	const auto [ustart, uend, vstart, vend] = wallpaper->getState ().getTextureUVs ();

	captures.push_back (
	    { buffer, readWidth, readHeight, vpWidth, vpHeight, currentXOffset, ustart, uend, vstart, vend }
	);

	if (viewport->single) {
	    currentXOffset += vpWidth;
	}
    }

    const auto extension = filename.extension ();
    const std::string extStr = extension.string ();

    // Offload pixel processing and saving to a background thread to avoid hitches
    std::thread ([captures, width, height, vflip, extStr, filename] () {
	auto* bitmap = new uint8_t[width * height * 3] { 0 };

	for (const auto& capture : captures) {
	    // copy pixels to bitmap, sampling from the UV-defined region
	    for (int y = 0; y < capture.vpHeight; y++) {
		for (int x = 0; x < capture.vpWidth; x++) {
		    // interpolate within the UV range to get source coordinates
		    const float u
			= capture.ustart + (static_cast<float> (x) / capture.vpWidth) * (capture.uend - capture.ustart);
		    const float v = capture.vstart
			+ (static_cast<float> (y) / capture.vpHeight) * (capture.vend - capture.vstart);

		    // convert UV to pixel coordinates in the source buffer
		    const int srcX = std::clamp (static_cast<int> (u * capture.readWidth), 0, capture.readWidth - 1);
		    const int srcY = std::clamp (static_cast<int> (v * capture.readHeight), 0, capture.readHeight - 1);
		    const int srcIdx = (srcY * capture.readWidth + srcX) * 3;

		    const int xfinal = x + capture.xoffset;
		    // FBO content is not flipped like default framebuffer, so invert vflip logic
		    const int yfinal = vflip ? y : (capture.vpHeight - y - 1);

		    if (yfinal >= 0 && yfinal < height && xfinal >= 0 && xfinal < width) {
			bitmap[yfinal * width * 3 + xfinal * 3] = capture.buffer[srcIdx];
			bitmap[yfinal * width * 3 + xfinal * 3 + 1] = capture.buffer[srcIdx + 1];
			bitmap[yfinal * width * 3 + xfinal * 3 + 2] = capture.buffer[srcIdx + 2];
		    }
		}
	    }
	    delete[] capture.buffer;
	}

	if (extStr == ".bmp") {
	    stbi_write_bmp (filename.c_str (), width, height, 3, bitmap);
	} else if (extStr == ".png") {
	    stbi_write_png (filename.c_str (), width, height, 3, bitmap, width * 3);
	} else if (extStr == ".jpg" || extStr == ".jpeg") {
	    stbi_write_jpg (filename.c_str (), width, height, 3, bitmap, 100);
	}

	delete[] bitmap;
    }).detach ();
}

void WallpaperApplication::setupOutput () {
    const char* XDG_SESSION_TYPE = getenv ("XDG_SESSION_TYPE");

    if (!XDG_SESSION_TYPE) {
	sLog.exception (
	    "Cannot read environment variable XDG_SESSION_TYPE, window server detection failed. Please ensure proper "
	    "values are set"
	);
    }

    sLog.debug ("Checking for window servers: ");

    for (const auto& windowServer : sVideoFactories.getRegisteredDrivers ()) {
	sLog.debug ("\t", windowServer);
    }

    this->m_videoDriver = sVideoFactories.createVideoDriver (
	this->m_context.settings.render.mode, XDG_SESSION_TYPE, this->m_context, *this
    );
    this->m_fullScreenDetector
	= sVideoFactories.createFullscreenDetector (XDG_SESSION_TYPE, this->m_context, *this->m_videoDriver);
}

void WallpaperApplication::setupAudio () {
    // Capture audio whenever processing is enabled, not only when the initial background needs it:
    // backgrounds swapped in live must still get a real recorder or audio-reactive elements stay dead.
    if (this->m_context.settings.audio.audioprocessing) {
	this->m_audioRecorder
	    = std::make_unique<WallpaperEngine::Audio::Drivers::Recorders::PulseAudioPlaybackRecorder> (
		this->m_context.settings.audio.device
	    );
    } else {
	this->m_audioRecorder = std::make_unique<WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder> ();
    }

    if (this->m_context.settings.audio.automute) {
	m_audioDetector = std::make_unique<WallpaperEngine::Audio::Drivers::Detectors::PulseAudioPlayingDetector> (
	    this->m_context, *this->m_fullScreenDetector
	);
    } else {
	m_audioDetector = std::make_unique<WallpaperEngine::Audio::Drivers::Detectors::AudioPlayingDetector> (
	    this->m_context, *this->m_fullScreenDetector
	);
    }

    // initialize sdl audio driver
    m_audioDriver = std::make_unique<WallpaperEngine::Audio::Drivers::SDLAudioDriver> (
	this->m_context, *this->m_audioDetector, *this->m_audioRecorder
    );
    // initialize audio context
    m_audioContext = std::make_unique<WallpaperEngine::Audio::AudioContext> (*m_audioDriver);
}

void WallpaperApplication::prepareOutputs () {
    // initialize render context
    m_renderContext
	= std::make_unique<WallpaperEngine::Render::RenderContext> (*m_videoDriver, *this, *this->m_mediaSource);
    // create a new background for each screen

    // set all the specific wallpapers required (skip span group synthetic keys)
    for (const auto& [background, info] : this->m_backgrounds) {
	if (background.rfind ("span:", 0) == 0) {
	    continue;
	}
	const auto scalingIt = this->m_context.settings.general.screenScalings.find (background);
	const auto clampIt = this->m_context.settings.general.screenClamps.find (background);
	const auto scaling = scalingIt != this->m_context.settings.general.screenScalings.end ()
	    ? scalingIt->second
	    : this->m_context.settings.render.window.scalingMode;
	const auto clamp = clampIt != this->m_context.settings.general.screenClamps.end ()
	    ? clampIt->second
	    : this->m_context.settings.render.window.clamp;

	m_renderContext->setWallpaper (
	    background,
	    WallpaperEngine::Render::CWallpaper::fromWallpaper (
		*info->wallpaper, *m_renderContext, *m_audioContext, m_browserContext.get (), scaling, clamp
	    )
	);
    }

    // Set up span groups: one shared wallpaper per group, registered for each viewport
    for (const auto& spanGroup : this->m_context.settings.general.spanGroups) {
	if (spanGroup.screens.empty ()) {
	    continue;
	}

	const std::string groupKey = "span:" + spanGroup.screens.front ();
	const auto bgIt = this->m_backgrounds.find (groupKey);
	if (bgIt == this->m_backgrounds.end ()) {
	    continue;
	}

	// Compute the bounding box of all viewports in this span group
	const auto& viewports = m_renderContext->getOutput ().getViewports ();
	int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
	bool anyFound = false;

	for (const auto& screenName : spanGroup.screens) {
	    const auto vpIt = viewports.find (screenName);
	    if (vpIt == viewports.end ()) {
		sLog.error ("Span group screen not found: ", screenName);
		continue;
	    }
	    anyFound = true;
	    const auto& vp = vpIt->second;
	    const int x = vp->globalPosition.x;
	    const int y = vp->globalPosition.y;
	    const int w = vp->logicalSize.x;
	    const int h = vp->logicalSize.y;
	    sLog.debug (
		"SPAN DEBUG prepareOutputs: screen '", screenName, "' globalPos=(", x, ",", y, ") logicalSize=", w, "x",
		h
	    );
	    minX = std::min (minX, x);
	    minY = std::min (minY, y);
	    maxX = std::max (maxX, x + w);
	    maxY = std::max (maxY, y + h);
	}

	if (!anyFound) {
	    sLog.error ("No viewports found for span group, skipping");
	    continue;
	}

	sLog.debug (
	    "SPAN DEBUG prepareOutputs: bounding box=(", minX, ",", minY, ",", maxX - minX, ",", maxY - minY, ")"
	);

	WallpaperEngine::Render::CWallpaper::SpanInfo spanInfo;
	spanInfo.totalBounds = { minX, minY, maxX - minX, maxY - minY };

	// Create one shared wallpaper with the span group's scaling mode
	auto sharedWallpaper = WallpaperEngine::Render::CWallpaper::fromWallpaper (
	    *bgIt->second->wallpaper, *m_renderContext, *m_audioContext, m_browserContext.get (), spanGroup.scaling,
	    spanGroup.clamp
	);

	// Convert to shared_ptr so it can be registered for multiple viewports
	std::shared_ptr<WallpaperEngine::Render::CWallpaper> shared (std::move (sharedWallpaper));
	shared->setSpanInfo (spanInfo);

	// Register the same wallpaper for each screen in the span group
	for (const auto& screenName : spanGroup.screens) {
	    m_renderContext->setWallpaper (screenName, shared);
	}
    }
}

void WallpaperApplication::setupOpenGLDebugging () {
#if !NDEBUG
    glDebugMessageCallback (CustomGLDebugCallback, nullptr);
    glEnable (GL_DEBUG_OUTPUT_SYNCHRONOUS);
#endif
}

void WallpaperApplication::setup () {
    this->setupOutput ();
    this->setupAudio ();
    this->prepareOutputs ();
    this->setupOpenGLDebugging ();

    // Apply runtime state not baked in at creation (volume/mute, playback speed) so frame 1 honors it.
    this->applyAudioVolume ();
    this->applyPlaybackSpeed ();

    // Open the live control socket if requested
    if (!this->m_context.settings.controlSocket.empty ()) {
	this->m_controlSocket = std::make_unique<ControlSocket> (this->m_context.settings.controlSocket);
    }

    if (this->m_context.settings.general.dumpStructure) {
	auto prettyPrinter = Data::Dumpers::StringPrinter ();

	for (const auto& [background, info] : this->m_renderContext->getWallpapers ()) {
	    prettyPrinter.printWallpaper (info->getWallpaperData ());
	}

	std::cout << prettyPrinter.str () << std::endl;
    }

#if DEMOMODE
    // ensure only one background is running so everything can be properly caught
    if (this->m_renderContext->getWallpapers ().size () > 1) {
	sLog.exception ("Demo mode only supports one background");
    }

    int width = this->m_renderContext->getWallpapers ().begin ()->second->getWidth ();
    int height = this->m_renderContext->getWallpapers ().begin ()->second->getHeight ();
    std::vector<uint8_t> pixels (width * height * 3);
    bool initialized = false;
    int frame = 0;
#endif /* DEMOMODE */
}

void WallpaperApplication::render () {
    // Service any live control-socket requests before rendering the frame
    if (this->m_controlSocket) {
	this->m_controlSocket->poll (*this);
    }

    static time_t seconds;
    static struct tm* timeinfo;

    if (this->m_isPaused) {
	usleep (FULLSCREEN_CHECK_WAIT_TIME);
	if (this->m_fullScreenDetector->anythingFullscreen () && this->m_context.state.general.keepRunning) {
	    return;
	}
	m_renderContext->setPause (false);

	// account for paused duration in playlist timers
	const auto pausedNow = std::chrono::steady_clock::now ();
	const auto pausedDuration = pausedNow - this->m_pauseStart;

	for (auto& [_, playlist] : this->m_activePlaylists) {
	    if (!playlist.definition.settings.updateOnPause) {
		playlist.nextSwitch += pausedDuration;
		playlist.lastUpdate += pausedDuration;
	    }
	}

	this->m_isPaused = false;
    } else {
	// update g_Daytime
	time (&seconds);
	timeinfo = localtime (&seconds);
	g_Daytime = static_cast<float> ((timeinfo->tm_hour * 60) + timeinfo->tm_min) / (24.0f * 60.0f);

	// keep track of the previous frame's time
	g_TimeLast = g_Time;
	// calculate the current time value, scaled by playback speed (1.0 = normal); scaling the
	// clock animates scenes/particles faster or slower without affecting the render FPS.
	{
	    float playbackSpeed = this->m_context.settings.render.playbackSpeed;
	    if (playbackSpeed <= 0.0f) {
		playbackSpeed = 1.0f;
	    }
	    g_Time = m_videoDriver->getRenderTime () * playbackSpeed;
	}
	// update audio recorder
	m_audioDriver->update ();
	// update the media source
	m_mediaSource->update ();
	// update input information
	m_videoDriver->getInputContext ().update ();
	// process driver events
	m_videoDriver->dispatchEventQueue ();

	if (m_videoDriver->closeRequested ()) {
	    sLog.out ("Stop requested by driver");
	    this->m_context.state.general.keepRunning = false;
	}

#if DEMOMODE
	// wait for a full render cycle before actually starting
	// this gives some extra time for video and web decoders to set themselves up
	// because of size changes
	if (m_videoDriver->getFrameCounter () > (uint32_t)this->m_context.settings.render.maximumFPS) {
	    if (!initialized) {
		width = this->m_renderContext->getWallpapers ().begin ()->second->getWidth ();
		height = this->m_renderContext->getWallpapers ().begin ()->second->getHeight ();
		pixels.reserve (width * height * 3);
		init_encoder ("output.webm", width, height);
		initialized = true;
	    }

	    glBindFramebuffer (
		GL_FRAMEBUFFER, this->m_renderContext->getWallpapers ().begin ()->second->getWallpaperFramebuffer ()
	    );

	    glPixelStorei (GL_PACK_ALIGNMENT, 1);
	    glReadPixels (0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data ());
	    write_video_frame (pixels.data ());
	    frame++;

	    // stop after the given framecount
	    if (frame >= FRAME_COUNT) {
		this->m_context.state.general.keepRunning = false;
	    }
	}
#endif /* DEMOMODE */
	// check for fullscreen windows and wait until there's none fullscreen
	if (this->m_fullScreenDetector->anythingFullscreen () && this->m_context.state.general.keepRunning) {
	    this->m_isPaused = true;
	    this->m_pauseStart = std::chrono::steady_clock::now ();

	    m_renderContext->setPause (true);
	    return;
	}
    }

    this->updatePlaylists ();

    if (!this->m_context.settings.screenshot.take || this->m_screenShotTaken == true) {
	return;
    }

    if (this->m_videoDriver->getFrameCounter () < this->m_nextFrameScreenshot) {
	return;
    }

    this->takeScreenshot (this->m_context.settings.screenshot.path);
    this->m_screenShotTaken = true;
}

void WallpaperApplication::cleanup () {
    sLog.out ("Stopping");

#if DEMOMODE
    close_encoder ();
#endif /* DEMOMODE */

    SDL_Quit ();
}

void WallpaperApplication::show () {
    setup ();
    while (this->m_context.state.general.keepRunning) {
	render ();
    }
    cleanup ();
}

bool WallpaperApplication::abnormalTermination () const {
    return this->m_videoDriver->abnormalTermination ();
}

void WallpaperApplication::update (Render::Drivers::Output::OutputViewport* viewport) {
    // render the scene
    m_renderContext->render (viewport);
}

void WallpaperApplication::signal (int signal) {
    sLog.out ("Stop requested by signal ", signal);
    this->m_context.state.general.keepRunning = false;
}

const std::map<std::string, ProjectUniquePtr>& WallpaperApplication::getBackgrounds () const {
    return this->m_backgrounds;
}

ApplicationContext& WallpaperApplication::getContext () const { return this->m_context; }

const WallpaperEngine::Render::Drivers::Output::Output& WallpaperApplication::getOutput () const {
    return this->m_renderContext->getOutput ();
}

void WallpaperApplication::setDestinationFramebuffer (GLuint framebuffer) {
    this->m_destinationFramebuffer = framebuffer;
    // Update all wallpapers with the new destination framebuffer
    for (const auto& [screen, wallpaper] : this->m_renderContext->getWallpapers ()) {
	wallpaper->setDestinationFramebuffer (framebuffer);
    };
}

GLuint WallpaperApplication::getDestinationFramebuffer () const { return this->m_destinationFramebuffer; }

bool WallpaperApplication::setBackground (const std::string& screen, const std::string& path) {
    try {
	auto project = this->loadBackground (path);
	this->setupPropertiesForProject (*project);
	this->ensureBrowserForProject (*project);
	this->m_backgrounds[screen] = std::move (project);

	const auto scalingIt = this->m_context.settings.general.screenScalings.find (screen);
	const auto clampIt = this->m_context.settings.general.screenClamps.find (screen);
	const auto scaling = scalingIt != this->m_context.settings.general.screenScalings.end ()
	    ? scalingIt->second
	    : this->m_context.settings.render.window.scalingMode;
	const auto clamp = clampIt != this->m_context.settings.general.screenClamps.end ()
	    ? clampIt->second
	    : this->m_context.settings.render.window.clamp;

	if (this->m_renderContext) {
	    this->m_renderContext->setWallpaper (
		screen,
		WallpaperEngine::Render::CWallpaper::fromWallpaper (
		    *this->m_backgrounds[screen]->wallpaper, *this->m_renderContext, *this->m_audioContext,
		    this->m_browserContext.get (), scaling, clamp
		)
	    );
	}

	this->m_context.settings.general.screenBackgrounds[screen] = path;
	this->applyAudioVolume (); // keep the new wallpaper in sync with volume/mute
	this->applyPlaybackSpeed (); // ...and with playback speed
	return true;
    } catch (const std::exception& e) {
	sLog.error ("setBackground failed on ", screen, ": ", e.what ());
	return false;
    }
}

namespace {
// An effect's visibility is fixed when its image pass list is built (CImage::setup), so a property
// gating an effect's `visible` condition needs a rebuild; other live-read properties don't.
bool propertyGatesEffectVisibility (const Project& project, const std::string& key) {
    if (project.wallpaper == nullptr || !project.wallpaper->is<Scene> ()) {
	return false;
    }

    const auto* scene = project.wallpaper->as<Scene> ();
    for (const auto& object : scene->objects) {
	if (!object->is<Image> ()) {
	    continue;
	}
	for (const auto& effect : object->as<Image> ()->effects) {
	    if (effect->visible != nullptr && effect->visible->condition.has_value ()
		&& effect->visible->condition->name == key) {
		return true;
	    }
	}
    }
    return false;
}
} // namespace

void WallpaperApplication::stageProperty (const std::string& key, const std::string& value) {
    // Record a property override without touching the current wallpaper: the next (re)build reads it
    // via setupPropertiesForProject. Lets a launcher stage every saved property BEFORE `bg` so the
    // wallpaper builds once with the right values instead of rebuilding per structural property.
    this->m_context.settings.general.properties[key] = value;
}

bool WallpaperApplication::setProperty (const std::string& screen, const std::string& key, const std::string& value) {
    const auto bg = this->m_backgrounds.find (screen);
    if (bg == this->m_backgrounds.end ()) {
	return false;
    }

    // Only accept keys the current wallpaper actually declares.
    const auto propertyIt = bg->second->properties.find (key);
    if (propertyIt == bg->second->properties.end ()) {
	return false;
    }

    // Record the override so it survives future reloads / relaunches.
    this->m_context.settings.general.properties[key] = value;

    // Live update: the DynamicValue propagates to shader constants/uniforms, camera fov and per-object
    // `visible` conditions (all read each frame), so sliders/colours/toggles apply with no reload.
    // Compare through the property's own normalized form so a re-push of the same value (the daemon
    // re-sends every saved property on each switch) is recognized as a no-op.
    const std::string before = propertyIt->second->toString ();
    propertyIt->second->update (value, DynamicValue::UpdateSource::User);
    const bool changed = propertyIt->second->toString () != before;

    // Discrete toggles (bool/combo) usually change build-time scene structure a value update can't
    // reach, so rebuild in-process (flash-free) for those; sliders/colours stay pure live updates.
    const bool structural = propertyIt->second->is<Data::Model::PropertyBoolean> ()
	|| propertyIt->second->is<Data::Model::PropertyCombo> ();

    if (changed && (structural || propertyGatesEffectVisibility (*bg->second, key))) {
	const auto it = this->m_context.settings.general.screenBackgrounds.find (screen);
	if (it != this->m_context.settings.general.screenBackgrounds.end ()) {
	    return this->setBackground (screen, it->second.string ());
	}
    }

    // Live (non-structural) change: notify the scene's scripts so a script-driven wallpaper can react
    // (applyUserProperties). Structural changes rebuilt above and re-run init() with the new value.
    const auto& wallpapers = this->m_renderContext->getWallpapers ();
    if (const auto wpIt = wallpapers.find (screen); wpIt != wallpapers.end ()) {
	if (auto* scene = dynamic_cast<Render::Wallpapers::CScene*> (wpIt->second.get ())) {
	    scene->getScriptEngine ().dispatchUserProperty (key, *propertyIt->second);
	}
    }

    return true;
}

bool WallpaperApplication::setScreenScaling (const std::string& screen, const std::string& mode) {
    using WallpaperEngine::Render::WallpaperState;
    WallpaperState::TextureUVsScaling value;
    if (mode == "stretch") {
	value = WallpaperState::TextureUVsScaling::StretchUVs;
    } else if (mode == "fit") {
	value = WallpaperState::TextureUVsScaling::ZoomFitUVs;
    } else if (mode == "fill") {
	value = WallpaperState::TextureUVsScaling::ZoomFillUVs;
    } else if (mode == "default") {
	value = WallpaperState::TextureUVsScaling::DefaultUVs;
    } else {
	return false;
    }

    this->m_context.settings.general.screenScalings[screen] = value;
    const auto it = this->m_context.settings.general.screenBackgrounds.find (screen);
    return it != this->m_context.settings.general.screenBackgrounds.end ()
	&& this->setBackground (screen, it->second.string ());
}

bool WallpaperApplication::setScreenClamp (const std::string& screen, const std::string& mode) {
    TextureFlags value;
    if (mode == "clamp") {
	value = TextureFlags_ClampUVs;
    } else if (mode == "border") {
	value = TextureFlags_ClampUVsBorder;
    } else if (mode == "repeat") {
	value = TextureFlags_NoFlags;
    } else {
	return false;
    }

    this->m_context.settings.general.screenClamps[screen] = value;
    const auto it = this->m_context.settings.general.screenBackgrounds.find (screen);
    return it != this->m_context.settings.general.screenBackgrounds.end ()
	&& this->setBackground (screen, it->second.string ());
}

void WallpaperApplication::applyPlaybackSpeed () {
    if (!this->m_renderContext) {
	return;
    }

    for (const auto& [screen, wallpaper] : this->m_renderContext->getWallpapers ()) {
	wallpaper->setPlaybackSpeed (this->m_context.settings.render.playbackSpeed);
    }
}

void WallpaperApplication::setPlaybackSpeed (float speed) {
    this->m_context.settings.render.playbackSpeed = speed <= 0.0f ? 1.0f : speed;
    this->applyPlaybackSpeed (); // media (video) wallpapers; scenes use the scaled clock
}

void WallpaperApplication::applyAudioVolume () {
    if (!this->m_renderContext) {
	return;
    }

    const int volume = this->m_context.settings.audio.enabled ? this->m_context.settings.audio.volume : 0;
    for (const auto& [screen, wallpaper] : this->m_renderContext->getWallpapers ()) {
	wallpaper->setAudioVolume (volume);
    }
}

void WallpaperApplication::setVolume (int volume) {
    this->m_context.settings.audio.volume = volume;
    this->applyAudioVolume ();
}

void WallpaperApplication::setMute (bool muted) {
    this->m_context.settings.audio.enabled = !muted;
    this->applyAudioVolume ();
}

bool WallpaperApplication::setOption (const std::string& key, const std::string& value) {
    auto& settings = this->m_context.settings;
    const bool on = value == "true" || value == "1";

    if (key == "fps") {
	// 0 (or less) means uncapped — render as fast as the compositor allows.
	settings.render.maximumFPS = std::atoi (value.c_str ());
    } else if (key == "noautomute") {
	settings.audio.automute = !on;
    } else if (key == "disablemouse") {
	settings.mouse.enabled = !on;
	this->m_context.state.mouse.enabled = !on;
    } else if (key == "disableparallax") {
	settings.mouse.disableparallax = on;
    } else if (key == "nofullscreenpause") {
	settings.render.pauseOnFullscreen = !on;
    } else if (key == "renderscale") {
	settings.render.renderScale = std::clamp (static_cast<float> (std::atof (value.c_str ())), 0.5f, 2.0f);
	// Recreate the render targets at the new supersampling factor. Flash-free in-process
	// rebuild (the GL context/process stay alive); CWallpaper reads renderScale at construction.
	for (const auto& [screen, path] : settings.general.screenBackgrounds) {
	    this->setBackground (screen, path.string ());
	}
    } else if (key == "audiodevice") {
	settings.audio.device = (value == "default") ? "" : value;
	// Re-init the capture pipeline on the new device, then rebuild the wallpapers so they
	// bind the freshly-created audio context (they hold a reference to it).
	this->setupAudio ();
	for (const auto& [screen, path] : settings.general.screenBackgrounds) {
	    this->setBackground (screen, path.string ());
	}
    } else {
	return false;
    }

    return true;
}

std::string WallpaperApplication::controlStatus () const {
    std::stringstream ss;
    ss << "speed=" << this->m_context.settings.render.playbackSpeed << "\n";
    for (const auto& [screen, project] : this->m_backgrounds) {
	const auto it = this->m_context.settings.general.screenBackgrounds.find (screen);
	ss << "screen=" << screen
	   << " bg=" << (it != this->m_context.settings.general.screenBackgrounds.end () ? it->second.string () : "")
	   << "\n";
    }
    return ss.str ();
}