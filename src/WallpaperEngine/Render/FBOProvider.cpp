#include "FBOProvider.h"
#include <glm/common.hpp>
#include <gmpxx.h>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;

// Child providers inherit the root's supersampling factor so every render target in the tree scales
// together (the scene FBO, per-object composites, bloom buffers, reflection copies).
FBOProvider::FBOProvider (const FBOProvider* parent) :
    m_parent (parent), m_renderScale (parent != nullptr ? parent->m_renderScale : 1.0f) { }

std::shared_ptr<CFBO> FBOProvider::create (const FBO& base, uint32_t flags, const glm::vec2 size) {
    const glm::vec2 scaled = glm::max (glm::round (size * this->m_renderScale), glm::vec2 (1.0f));
    return this->m_fbos[base.name] = std::make_shared<CFBO> (
	       base.name,
	       // TODO: PROPERLY DETERMINE FBO FORMAT BASED ON THE STRING
	       TextureFormat_ARGB8888, flags, base.scale, scaled.x / base.scale, scaled.y / base.scale,
	       scaled.x / base.scale, scaled.y / base.scale
	   );
}

std::shared_ptr<CFBO> FBOProvider::create (
    const std::string& name, TextureFormat format, uint32_t flags, float scale, glm::vec2 realSize,
    glm::vec2 textureSize
) {
    realSize = glm::max (glm::round (realSize * this->m_renderScale), glm::vec2 (1.0f));
    textureSize = glm::max (glm::round (textureSize * this->m_renderScale), glm::vec2 (1.0f));
    return this->m_fbos[name] = std::make_shared<CFBO> (
	       name, TextureFormat_ARGB8888, flags, scale, realSize.x, realSize.y, textureSize.x, textureSize.y
	   );
}

std::shared_ptr<CFBO> FBOProvider::alias (const std::string& newName, const std::string& original) {
    return this->m_fbos[newName] = this->m_fbos[original];
}

std::shared_ptr<CFBO> FBOProvider::find (const std::string& name) const {
    if (const auto it = this->m_fbos.find (name); it != this->m_fbos.end ()) {
	return it->second;
    }

    if (this->m_parent == nullptr) {
	return nullptr;
    }

    return this->m_parent->find (name);
}