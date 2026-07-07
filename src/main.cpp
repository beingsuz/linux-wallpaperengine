#include <csignal>
#include <iostream>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"

#include <cstring>

WallpaperEngine::Application::WallpaperApplication* app;

void signalhandler (const int sig) {
    if (app == nullptr) {
	return;
    }

    app->signal (sig);
}

void initLogging () {
    sLog.addOutput (new std::ostream (std::cout.rdbuf ()));
    sLog.addError (new std::ostream (std::cerr.rdbuf ()));
}

int main (int argc, char* argv[]) {
    // CEF spawns helper processes (render/gpu/zygote/utility) by re-launching THIS
    // binary with a "--type=..." switch. They MUST hand off to CEF before anything
    // else runs: any file IO or GL/Wayland init first closes the inherited ICU-data
    // file descriptor, after which the helper aborts ("Invalid file descriptor to
    // ICU data received") and takes web wallpapers down with it. Do it before
    // logging, argument parsing and background loading.
    for (int i = 1; i < argc; i++) {
	if (strncmp (argv[i], "--type=", 7) == 0) {
	    return WallpaperEngine::WebBrowser::WebBrowserContext::executeSubprocess (argc, argv);
	}
    }

    try {
	// if type parameter is specified, this is a subprocess, so no logging should be enabled from our side
	bool enableLogging = true;
	const std::string typeZygote = "--type=zygote";
	const std::string typeUtility = "--type=utility";

	for (int i = 1; i < argc; i++) {
	    if (strncmp (typeZygote.c_str (), argv[i], typeZygote.size ()) == 0) {
		enableLogging = false;
		break;
	    }

	    if (strncmp (typeUtility.c_str (), argv[i], typeUtility.size ()) == 0) {
		enableLogging = false;
		break;
	    }
	}

	if (enableLogging) {
	    initLogging ();
	}

	WallpaperEngine::Application::ApplicationContext appContext (argc, argv);

	appContext.loadSettingsFromArgv ();

	app = new WallpaperEngine::Application::WallpaperApplication (appContext);

	// halt if either list-properties option was specified
	if (appContext.settings.general.onlyListProperties || appContext.settings.general.listPropertiesJson) {
	    delete app;
	    return 0;
	}

	// attach signals to gracefully stop
	std::signal (SIGINT, signalhandler);
	std::signal (SIGTERM, signalhandler);
	std::signal (SIGKILL, signalhandler);

	// show the wallpaper application
	app->show ();

	// remove signal handlers before destroying app
	std::signal (SIGINT, SIG_DFL);
	std::signal (SIGTERM, SIG_DFL);
	std::signal (SIGKILL, SIG_DFL);

	// A clean stop (signal, socket quit) returns 0; an abnormal one (the driver lost its only
	// output) returns non-zero so the launcher's supervisor relaunches instead of leaving the
	// wallpaper dead.
	const bool abnormal = app->abnormalTermination ();

	delete app;

	return abnormal ? 1 : 0;
    } catch (const std::exception& e) {
	std::cerr << e.what () << std::endl;
	return 1;
    }
}