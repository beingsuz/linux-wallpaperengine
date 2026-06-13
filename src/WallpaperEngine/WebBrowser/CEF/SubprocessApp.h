#pragma once

#include "WPSchemeHandlerFactory.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"
#include "include/cef_app.h"
#include "include/cef_render_process_handler.h"

#include <string>
#include <vector>

namespace WallpaperEngine::Application {
class WallpaperApplication;
}

namespace WallpaperEngine::WebBrowser::CEF {
class SubprocessApp : public CefApp, public CefRenderProcessHandler {
public:
    explicit SubprocessApp (WallpaperEngine::Application::WallpaperApplication& application);
    /**
     * Lightweight construction for CEF helper (subprocess) processes: they only
     * register the fixed wp scheme and must NOT load backgrounds, as the file IO
     * would close the inherited ICU data descriptor before CEF reads it.
     */
    SubprocessApp () = default;

    void OnRegisterCustomSchemes (CefRawPtr<CefSchemeRegistrar> registrar) override;

    // Render-process side of the Wallpaper Engine web API bridge: inject the
    // window.wallpaper* shim into every frame before its own scripts run, so web
    // wallpapers can register audio/media/property listeners. The browser process
    // then drives those listeners via ExecuteJavaScript (see CWeb).
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler () override { return this; }
    void OnContextCreated (
        CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context
    ) override;

protected:
    const WallpaperEngine::Application::WallpaperApplication& getApplication () const;

private:
    WallpaperEngine::Application::WallpaperApplication* m_application = nullptr;
    IMPLEMENT_REFCOUNTING (SubprocessApp);
    DISALLOW_COPY_AND_ASSIGN (SubprocessApp);
};
}
