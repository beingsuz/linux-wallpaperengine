#pragma once

#include <string>

namespace WallpaperEngine::Application {
class WallpaperApplication;

/**
 * Small Unix-domain control socket. Lets an external client drive a running
 * wallpaper process (swap backgrounds, tweak properties, query status) without
 * restarting it. Line-based: one request line in, one response line out.
 */
class ControlSocket {
public:
    explicit ControlSocket (std::string path);
    ~ControlSocket ();

    /** Non-blocking: service any pending client requests against the app. */
    void poll (WallpaperApplication& app);

private:
    std::string handle (WallpaperApplication& app, const std::string& line);

    std::string m_path;
    int m_fd = -1;
};
}
