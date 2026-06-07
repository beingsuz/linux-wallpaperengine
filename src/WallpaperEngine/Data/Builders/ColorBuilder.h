#pragma once

#include "WallpaperEngine/Data/Model/Color.h"

#include <glm/vec4.hpp>
#include <string>

namespace WallpaperEngine::Data::Builders {
class ColorBuilder {
public:
    /**
     * White color constant
     */
    static const Model::Color White;
    /**
     * Black color constant
     */
    static const Model::Color Black;

    // forceFloat: when true, the value is always interpreted as 0..1 floats even
    // without a decimal point. Used for property colors (e.g. "1 1 1" = white),
    // which would otherwise be misread as 0..255 integers (=> near-black).
    static WallpaperEngine::Data::Model::Color
    parse (const std::string& value, float alpha = 1.0f, bool forceFloat = false);
};
}