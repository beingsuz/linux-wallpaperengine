#include "TextureCache.h"

#include "AlbumTexture.h"
#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/FileSystem/Container.h"

#include "CTexture.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Parsers/TextureParser.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Assets;

TextureCache::TextureCache (RenderContext& context) : Helpers::ContextAware (context) {
    // these textures are special cases, so make sure they're created only upon request
    this->m_currentThumbnail = std::make_shared<AlbumTexture> (this->getContext ());

#if !NDEBUG
    glObjectLabel (GL_TEXTURE, this->m_currentThumbnail->getTextureID (0), -1, "$mediaThumbnail");
#endif

    this->m_previousThumbnail = std::make_shared<AlbumTexture> (this->getContext ());

#if !NDEBUG
    glObjectLabel (GL_TEXTURE, this->m_previousThumbnail->getTextureID (0), -1, "$mediaPreviousThumbnail");
#endif

    // load the latest texture (if available)
    this->m_currentThumbnail->load ();

    // add these to the cache and return the right one
    this->store ("$mediaThumbnail", this->m_currentThumbnail);
    this->store ("$mediaPreviousThumbnail", this->m_previousThumbnail);

    this->m_mediaCallback = this->getContext ().getMediaSource ().addAlbumArtListener (
	[this] (const Media::MediaSource::MediaInfo& data) {
	    if (this->m_currentThumbnail->isReady ()) {
		// copy over pixel data and setup the new texture with the new data
		this->m_previousThumbnail->copyContents (*this->m_currentThumbnail);
	    }

	    // load the next image
	    this->m_currentThumbnail->load ();
	}
    );
}

TextureCache::~TextureCache () { this->m_mediaCallback (); }

namespace {
// Build a CTexture from a filename found in the given locator's container.
std::shared_ptr<const TextureProvider> loadTextureFrom (
    RenderContext& context, const AssetLocator& locator, const std::string& filename
) {
    const auto contents = locator.texture (filename);
    auto stream = BinaryReader (contents);
    auto metadataLoader = [&locator] (const std::string& metaFilename) -> std::string {
	return locator.readString (std::filesystem::path ("materials") / metaFilename);
    };
    auto parsedTexture = TextureParser::parse (stream, filename, metadataLoader);
    auto texture = std::make_shared<CTexture> (context, std::move (parsedTexture));
#if !NDEBUG
    glObjectLabel (GL_TEXTURE, texture->getTextureID (0), -1, filename.c_str ());
#endif
    return texture;
}
} // namespace

std::shared_ptr<const TextureProvider> TextureCache::resolve (
    const std::string& filename, const AssetLocator& locator
) {
    if (const auto found = this->m_textureCache.find (filename); found != this->m_textureCache.end ()) {
	return found->second;
    }

    // Resolve from the requesting scene's OWN container first. This is what keeps two scenes'
    // same-named textures from colliding through the shared cache (the cache is cleared per build,
    // so it only ever holds the scene currently being built).
    try {
	auto texture = loadTextureFrom (this->getContext (), locator, filename);
	this->store (filename, texture);
	return texture;
    } catch (AssetLoadException&) {
	// not in this scene's container — fall through to a broader search
    }

    // Fallback: search every loaded background's container (rare — a texture that lives outside the
    // requesting scene's own container).
    for (const auto& project : this->getContext ().getApp ().getBackgrounds () | std::views::values) {
	try {
	    auto texture = loadTextureFrom (this->getContext (), *project->assetLocator, filename);
	    this->store (filename, texture);
	    return texture;
	} catch (AssetLoadException&) {
	    // ignored, this happens if we're looking at the wrong background
	}
    }

    // TODO: FILL IN WITH A CHECKERED PATTERN TEXTURE INSTEAD?
    throw AssetLoadException ("Cannot find file", filename, std::error_code ());
}

void TextureCache::clear () { this->m_textureCache.clear (); }

void TextureCache::store (const std::string& name, std::shared_ptr<const TextureProvider> texture) {
    this->m_textureCache.insert_or_assign (name, texture);
}
