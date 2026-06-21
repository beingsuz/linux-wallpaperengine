#include "ControlSocket.h"
#include "WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"

#include <cstring>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace WallpaperEngine::Application;

ControlSocket::ControlSocket (std::string path) : m_path (std::move (path)) {
    m_fd = socket (AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (m_fd < 0) {
	sLog.error ("ControlSocket: cannot create socket");
	return;
    }

    unlink (m_path.c_str ());

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy (addr.sun_path, m_path.c_str (), sizeof (addr.sun_path) - 1);

    if (bind (m_fd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) < 0 || listen (m_fd, 8) < 0) {
	sLog.error ("ControlSocket: cannot bind/listen on ", m_path);
	close (m_fd);
	m_fd = -1;
	return;
    }

    sLog.out ("ControlSocket listening on ", m_path);
}

ControlSocket::~ControlSocket () {
    if (m_fd >= 0) {
	close (m_fd);
	unlink (m_path.c_str ());
    }
}

void ControlSocket::poll (WallpaperApplication& app) {
    if (m_fd < 0) {
	return;
    }

    // Accept and service every pending client without blocking the render loop.
    int client;
    while ((client = accept (m_fd, nullptr, nullptr)) >= 0) {
	// Bound the read so a silent client can't stall rendering.
	timeval tv { 0, 50000 }; // 50ms
	setsockopt (client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));

	std::string request;
	char buffer[1024];
	ssize_t n;
	while ((n = read (client, buffer, sizeof (buffer))) > 0) {
	    request.append (buffer, n);
	    if (request.find ('\n') != std::string::npos) {
		break;
	    }
	}

	if (const auto pos = request.find ('\n'); pos != std::string::npos) {
	    request.erase (pos);
	}

	if (!request.empty ()) {
	    const std::string response = this->handle (app, request);
	    if (write (client, response.c_str (), response.size ()) < 0) {
		sLog.error ("ControlSocket: failed to write response");
	    }
	}

	close (client);
    }
}

std::string ControlSocket::handle (WallpaperApplication& app, const std::string& line) {
    std::istringstream iss (line);
    std::string cmd;
    iss >> cmd;

    const auto rest = [&iss] () {
	std::string r;
	std::getline (iss, r);
	if (!r.empty () && r.front () == ' ') {
	    r.erase (0, 1);
	}
	return r;
    };

    if (cmd == "ping") {
	return "pong\n";
    }
    if (cmd == "status") {
	return app.controlStatus ();
    }
    if (cmd == "speed") {
	float v = 1.0f;
	iss >> v;
	app.setPlaybackSpeed (v);
	return "ok\n";
    }
    if (cmd == "volume") {
	int v = 128;
	iss >> v;
	app.setVolume (v);
	return "ok\n";
    }
    if (cmd == "mute") {
	int m = 0;
	iss >> m;
	app.setMute (m != 0);
	return "ok\n";
    }
    if (cmd == "set") {
	std::string key;
	iss >> key;
	return app.setOption (key, rest ()) ? "ok\n" : "error\n";
    }
    if (cmd == "bg") {
	std::string screen;
	iss >> screen;
	return app.setBackground (screen, rest ()) ? "ok\n" : "error\n";
    }
    if (cmd == "property") {
	std::string screen, key;
	iss >> screen >> key;
	return app.setProperty (screen, key, rest ()) ? "ok\n" : "error\n";
    }
    if (cmd == "scaling") {
	std::string screen, mode;
	iss >> screen >> mode;
	return app.setScreenScaling (screen, mode) ? "ok\n" : "error\n";
    }
    if (cmd == "clamp") {
	std::string screen, mode;
	iss >> screen >> mode;
	return app.setScreenClamp (screen, mode) ? "ok\n" : "error\n";
    }
    if (cmd == "screenshot") {
	// Capture a rendered frame of the live wallpaper to a file (for theme-colour extraction and
	// static fallbacks). The save is async, so callers poll for the file after the "ok".
	return app.captureScreenshot (rest ()) ? "ok\n" : "error\n";
    }

    return "unknown command\n";
}
