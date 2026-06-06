#include "SubprocessApp.h"
#include "WPSchemeHandlerFactory.h"
#include "WallpaperEngine/Data/Model/Project.h"

using namespace WallpaperEngine::WebBrowser::CEF;

SubprocessApp::SubprocessApp (WallpaperEngine::Application::WallpaperApplication& application) :
    m_application (&application) {
    for (const auto& info : this->m_application->getBackgrounds () | std::views::values) {
	this->m_handlerFactories[info->workshopId] = new WPSchemeHandlerFactory (*info);
    }
}

SubprocessApp::SubprocessApp (const std::vector<std::string>& workshopIds) {
    // Subprocess path: register the schemes by id only. The factory (which serves
    // resources) is never invoked here — that happens in the browser process — so
    // a null entry is enough to carry the scheme name through OnRegisterCustomSchemes.
    for (const auto& id : workshopIds) {
	this->m_handlerFactories[id] = nullptr;
    }
}

void SubprocessApp::OnRegisterCustomSchemes (CefRawPtr<CefSchemeRegistrar> registrar) {
    // register all the needed schemes, "wp" + the background id is going to be our scheme
    for (const auto& workshopId : this->m_handlerFactories | std::views::keys) {
	registrar->AddCustomScheme (
	    WPSchemeHandlerFactory::generateSchemeName (workshopId),
	    CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE | CEF_SCHEME_OPTION_FETCH_ENABLED
	);
    }
}

const WallpaperEngine::Application::WallpaperApplication& SubprocessApp::getApplication () const {
    return *this->m_application;
}

const std::map<std::string, WPSchemeHandlerFactory*>& SubprocessApp::getHandlerFactories () const {
    return this->m_handlerFactories;
}

void SubprocessApp::OnContextCreated (
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context
) {
    // Wallpaper Engine web API shim. Defines the listener-registration functions a
    // web wallpaper expects and small __wp* entry points the browser process calls
    // (via ExecuteJavaScript) to deliver audio, media and property updates.
    static const char* shim = R"JS(
(function(){
  if (window.__wpBridge) return; window.__wpBridge = true;
  var A=[],MP=[],PB=[],TH=[],TL=[];
  function fire(a,d){for(var i=0;i<a.length;i++){try{a[i](d);}catch(e){}}}
  window.wallpaperRegisterAudioListener=function(cb){if(typeof cb==='function')A.push(cb);};
  window.wallpaperRegisterMediaPropertiesListener=function(cb){if(typeof cb==='function')MP.push(cb);};
  window.wallpaperRegisterMediaPlaybackListener=function(cb){if(typeof cb==='function')PB.push(cb);};
  window.wallpaperRegisterMediaThumbnailListener=function(cb){if(typeof cb==='function')TH.push(cb);};
  window.wallpaperRegisterMediaTimelineListener=function(cb){if(typeof cb==='function')TL.push(cb);};
  window.__wpAudio=function(d){fire(A,d);};
  window.__wpMediaProps=function(d){fire(MP,d);};
  window.__wpMediaPlayback=function(d){fire(PB,d);};
  window.__wpMediaThumb=function(d){fire(TH,d);};
  window.__wpMediaTimeline=function(d){fire(TL,d);};
  window.__wpApplyProps=function(p){var l=window.wallpaperPropertyListener;if(l&&l.applyUserProperties){try{l.applyUserProperties(p);}catch(e){}}};
  window.__wpApplyGeneral=function(p){var l=window.wallpaperPropertyListener;if(l&&l.applyGeneralProperties){try{l.applyGeneralProperties(p);}catch(e){}}};
})();
)JS";
    frame->ExecuteJavaScript (shim, frame->GetURL (), 0);
}