#include "ScriptableObject.h"

#include "ScriptEngine.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"

#include <ranges>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Scripting;

ScriptableObject::ScriptableObject (Wallpapers::CScene& scene, const Object& object) : CObject (scene, object) {
    // register common dynamic values
    this->registerProperty ("origin", *object.origin->value);
    this->registerProperty ("scale", *object.groupScale->value);
    this->registerProperty ("angles", *object.groupAngles->value);
    this->registerProperty ("visible", *object.groupVisible->value);
}

DynamicValue& ScriptableObject::getProperty (const std::string& name) {
    const auto it = this->m_properties.find (name);

    if (it == this->m_properties.end ()) {
	sLog.exception ("Property '" + name + "' not found on object '" + this->getObject ().name + "'");
    }

    return it->second.value;
}

const std::map<std::string, ScriptableObject::PropertyEntry>& ScriptableObject::getProperties () const {
    return this->m_properties;
}

void ScriptableObject::registerProperty (const std::string& name, DynamicValue& value) {
    // Last registration wins: a derived CImage/CText re-registers names over the base group fallbacks
    // with the values the renderer actually reads, so the derived value must override.
    const std::string key = name + "_" + std::to_string (this->getId ());
    // PropertyEntry has a reference member (not assignable), so erase before re-emplacing.
    this->m_properties.erase (name);
    const auto [it, inserted] = this->m_properties.emplace (name, PropertyEntry { .key = key, .value = value });

    // queueScript self-guards against duplicate keys, so re-registering re-points without a 2nd module.
    this->getScene ().getScriptEngine ().queueScript (it->second.key, it->second.value, *this);
}
