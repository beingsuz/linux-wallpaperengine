#include "WebBrowserContext.h"
#include "CEF/BrowserApp.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/WebBrowser/CEF/SubprocessApp.h"
#include "include/cef_app.h"
#include "include/cef_render_handler.h"
#include <filesystem>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using namespace WallpaperEngine::WebBrowser;

int WebBrowserContext::executeSubprocess (int argc, char* argv[]) {
    CefMainArgs main_args (argc, argv);

    // The subprocess only needs to register the fixed wp scheme (the workshop id
    // travels in the URL host), so no argv scraping or file IO is needed — file IO
    // would close the inherited ICU data descriptor before CefExecuteProcess reads it.
    const CefRefPtr<CefApp> app = new CEF::SubprocessApp ();
    const int exitCode = CefExecuteProcess (main_args, app, nullptr);
    // A helper process always terminates here; CefExecuteProcess returns its exit
    // code (>= 0). Guard against -1 just in case so we still exit cleanly.
    return exitCode < 0 ? 0 : exitCode;
}

// TODO: THIS IS USED TO GENERATE A RANDOM FOLDER FOR THE CHROME PROFILE, MAYBE A DIFFERENT APPROACH WOULD BE BETTER?
namespace uuid {
static std::random_device rd;
static std::mt19937 gen (rd ());
static std::uniform_int_distribution<> dis (0, 15);
static std::uniform_int_distribution<> dis2 (8, 11);

std::string generate_uuid_v4 () {
    std::stringstream ss;
    int i;
    ss << std::hex;
    for (i = 0; i < 8; i++) {
	ss << dis (gen);
    }
    ss << "-";
    for (i = 0; i < 4; i++) {
	ss << dis (gen);
    }
    ss << "-4";
    for (i = 0; i < 3; i++) {
	ss << dis (gen);
    }
    ss << "-";
    ss << dis2 (gen);
    for (i = 0; i < 3; i++) {
	ss << dis (gen);
    }
    ss << "-";
    for (i = 0; i < 12; i++) {
	ss << dis (gen);
    };
    return ss.str ();
}
}

WebBrowserContext::WebBrowserContext (WallpaperEngine::Application::WallpaperApplication& wallpaperApplication) :
    m_browserApplication (nullptr), m_wallpaperApplication (wallpaperApplication) {
    CefMainArgs main_args (
	this->m_wallpaperApplication.getContext ().getArgc (), this->m_wallpaperApplication.getContext ().getArgv ()
    );

    // This is only ever reached by the browser process: CEF helper processes are
    // detected by their --type switch and handed off in executeSubprocess() at the
    // top of main(). So we always create the browser app and never call
    // CefExecuteProcess here — invoking it in the browser put content's ICU loader
    // into "child" mode, making it expect the data via a (never-passed) fd and fail
    // with "Invalid file descriptor to ICU data received".
    this->m_browserApplication = new CEF::BrowserApp (wallpaperApplication);

    // Configurate Chromium
    CefSettings settings;
    std::string cache_path = (std::filesystem::temp_directory_path () / uuid::generate_uuid_v4 ()).string ();

    // Point CEF at the directory holding its resources (icudtl.dat, *.pak, locales/).
    // Without these, subprocesses fail to load ICU ("Invalid file descriptor to ICU
    // data received") and web wallpapers crash. They sit next to the executable.
    char proc_path[4096];
    const ssize_t proc_len = readlink ("/proc/self/exe", proc_path, sizeof (proc_path) - 1);
    if (proc_len > 0) {
	proc_path[proc_len] = '\0';
	const std::string exe_dir = std::filesystem::path (proc_path).parent_path ().string ();
	CefString (&settings.resources_dir_path) = exe_dir;
	CefString (&settings.locales_dir_path) = exe_dir + "/locales";
    }

    cef_string_utf8_to_utf16 (cache_path.c_str (), cache_path.length (), &settings.root_cache_path);
    settings.windowless_rendering_enabled = true;
    // Run without the Chromium sandbox. A wallpaper renders local, trusted content,
    // and the sandbox's fd remapping is what breaks ICU-data loading here ("Invalid
    // file descriptor to ICU data received"). Disabling it lets every process read
    // icudtl.dat from resources_dir_path directly.
    settings.no_sandbox = true;

    // spawns two new processess

    if (!CefInitialize (main_args, settings, this->m_browserApplication, nullptr)) {
	sLog.exception ("CefInitialize: failed");
    }
}

WebBrowserContext::~WebBrowserContext () {
    sLog.out ("Shutting down CEF");
    CefShutdown ();
}
