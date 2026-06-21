#pragma once

#include "../Builders/ColorBuilder.h"
#include "../Utils/TypeCaster.h"
#include "DynamicValue.h"
#include "WallpaperEngine/Logging/Log.h"

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace WallpaperEngine::Data::Model {
using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::Data::Builders;

struct PropertyData {
    std::string name;
    std::string text;
    /** Editor sort order; consumers (e.g. the settings UI) render properties sorted by it. */
    int order = 0;
};

struct SliderData {
    float min;
    float max;
    float step;
};

struct ComboData {
    /** value -> label, used to validate an incoming selection. */
    std::map<std::string, std::string> values;
    /** (value, label) pairs in the wallpaper's declared order, for UI display. */
    std::vector<std::pair<std::string, std::string>> options;
};

class Property : public DynamicValue, public TypeCaster, public PropertyData {
public:
    explicit Property (PropertyData data) : DynamicValue (), TypeCaster (), PropertyData (std::move (data)) { }

    using DynamicValue::update;
    virtual void update (const std::string& value, UpdateSource source) = 0;
    [[nodiscard]] virtual std::string dump () const = 0;
    /**
     * Machine-readable description of the property + its value, for external consumers (the AGS
     * settings UI). Common keys: key/type/text/order/value (+ slider min/max/step, combo options[]).
     */
    [[nodiscard]] virtual nlohmann::json dumpJson () const = 0;

protected:
    /** Common fields every property JSON carries; each type fills in "value" (+ type-specific keys). */
    [[nodiscard]] nlohmann::json baseJson (const char* typeName) const {
	return nlohmann::json {
	    { "key", this->name },
	    { "type", typeName },
	    { "text", this->text },
	    { "order", this->order },
	};
    }
};

class PropertySlider final : public Property, SliderData {
public:
    PropertySlider (PropertyData data, SliderData sliderData, const float value) :
	Property (std::move (data)), SliderData (sliderData) {
	this->Property::update (value, UpdateSource::Initialization);
    }

    using Property::update;
    void update (const std::string& value, UpdateSource source) override { this->update (std::stof (value), source); }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - slider" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tMin: " << this->min << std::endl
	   << "\tMax: " << this->max << std::endl
	   << "\tStep: " << this->step << std::endl
	   << "\tValue: " << this->toString () << std::endl;

	return ss.str ();
    }

    [[nodiscard]] nlohmann::json dumpJson () const override {
	auto json = this->baseJson ("slider");
	json["value"] = this->getFloat ();
	json["min"] = this->min;
	json["max"] = this->max;
	json["step"] = this->step;
	return json;
    }
};

class PropertyBoolean final : public Property {
public:
    explicit PropertyBoolean (PropertyData data, const bool value) : Property (std::move (data)) {
	this->Property::update (value, UpdateSource::Initialization);
    }

    using Property::update;
    void update (const std::string& value, UpdateSource source) override {
	this->update (value == "true" || value == "1", source);
    }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - boolean" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->toString () << std::endl;

	return ss.str ();
    }

    [[nodiscard]] nlohmann::json dumpJson () const override {
	auto json = this->baseJson ("bool");
	json["value"] = this->getBool ();
	return json;
    }
};

class PropertyColor final : public Property {
public:
    explicit PropertyColor (PropertyData data, const std::string& value) : Property (std::move (data)) {
	this->PropertyColor::update (value, UpdateSource::Initialization);
    }

    using Property::update;
    void update (const std::string& value, UpdateSource source) override {
	// Property colors are always 0..1 floats in Wallpaper Engine's convention;
	// force float parsing so values like "1 1 1" become white, not near-black.
	this->update (ColorBuilder::parse (value, 1.0f, true), source);
    }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - color" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->toString () << std::endl;

	return ss.str ();
    }

    [[nodiscard]] nlohmann::json dumpJson () const override {
	auto json = this->baseJson ("color");
	// Wallpaper Engine's color convention is space-separated 0..1 floats ("r g b"); emit that
	// (not DynamicValue's comma-separated form) so consumers parse the channels directly.
	const glm::vec3 c = this->getVec3 ();
	json["value"] = std::to_string (c.x) + " " + std::to_string (c.y) + " " + std::to_string (c.z);
	return json;
    }
};

class PropertyCombo final : public Property, ComboData {
public:
    PropertyCombo (PropertyData data, ComboData comboData, const std::string& value) :
	Property (std::move (data)), ComboData (std::move (comboData)) {
	this->PropertyCombo::update (value, UpdateSource::Initialization);
    }

    using Property::update;
    void update (const std::string& value, UpdateSource source) override {
	if (this->values.contains (value) == false) {
	    sLog.error ("Combo value not found in combo options: ", value);
	    return;
	}

	// search for the value in the combo options or default to the textual value
	this->DynamicValue::update (value, source);
    }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - combo" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->toString () << std::endl
	   << "Values: " << std::endl;

	for (const auto& [key, value] : this->values) {
	    ss << "\t\t" << key << " = " << value << std::endl;
	}

	return ss.str ();
    }

    [[nodiscard]] nlohmann::json dumpJson () const override {
	auto json = this->baseJson ("combo");
	json["value"] = this->toString ();
	auto options = nlohmann::json::array ();
	for (const auto& [value, label] : this->options) {
	    options.push_back ({ { "label", label }, { "value", value } });
	}
	json["options"] = options;
	return json;
    }
};

class PropertyText final : public Property {
public:
    explicit PropertyText (PropertyData data) : Property (std::move (data)) { }

    using Property::update;
    void update (const std::string& value, UpdateSource source) override { this->DynamicValue::update (value, source); }

    [[nodiscard]] std::string toString () const override { return this->text; }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - text" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->toString () << std::endl;

	return ss.str ();
    }

    [[nodiscard]] nlohmann::json dumpJson () const override {
	auto json = this->baseJson ("text");
	json["value"] = this->toString ();
	return json;
    }
};

class PropertySceneTexture final : public Property {
public:
    explicit PropertySceneTexture (PropertyData data, const std::string& value) : Property (std::move (data)) {
	this->PropertySceneTexture::update (value, UpdateSource::Initialization);
    }

    void update (const std::string& value, UpdateSource source) override { this->DynamicValue::update (value, source); }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - scene texture" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->m_value << std::endl;

	return ss.str ();
    }

    [[nodiscard]] nlohmann::json dumpJson () const override {
	auto json = this->baseJson ("scenetexture");
	json["value"] = this->toString ();
	return json;
    }

private:
    std::string m_value;
};

class PropertyFile final : public Property {
public:
    explicit PropertyFile (PropertyData data, const std::string& value) : Property (std::move (data)) {
	this->PropertyFile::update (value, UpdateSource::Initialization);
    }

    void update (const std::string& value, UpdateSource source) override { this->DynamicValue::update (value, source); }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - file" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->m_value << std::endl;

	return ss.str ();
    }

    [[nodiscard]] nlohmann::json dumpJson () const override {
	auto json = this->baseJson ("file");
	json["value"] = this->toString ();
	return json;
    }

private:
    std::string m_value;
};

class PropertyTextInput final : public Property {
public:
    explicit PropertyTextInput (PropertyData data, const std::string& value) : Property (std::move (data)) {
	this->PropertyTextInput::update (value, UpdateSource::Initialization);
    }

    void update (const std::string& value, UpdateSource source) override { this->DynamicValue::update (value, source); }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - textinput" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->m_value << std::endl;

	return ss.str ();
    }

    [[nodiscard]] nlohmann::json dumpJson () const override {
	auto json = this->baseJson ("textinput");
	json["value"] = this->toString ();
	return json;
    }

private:
    std::string m_value;
};
}