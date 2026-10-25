#include "bf3/core/ui/settings_store.h"

#include "bf3/core/util/key_values.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <cmath>

namespace bf3::core::ui {

namespace {

struct LoadState {
	const OptionsCatalog *catalog = nullptr;
	uint32_t version = 0;
	SettingsValues values;
	KeyBindingTable bindings;
	std::vector<std::pair<std::string, std::string>> unknown;
};

KeyOutcome parse_setting(std::string_view p_key, std::string_view p_value, LoadState &r_state) {
	if (p_key.starts_with("bind.")) {
		if (!r_state.bindings.apply_override(p_key, p_value)) {
			r_state.unknown.emplace_back(std::string(p_key), std::string(p_value));
		}
		return {};
	}
	const size_t i = r_state.catalog->option_index(p_key);
	if (i >= r_state.catalog->stored.size()) {
		r_state.unknown.emplace_back(std::string(p_key), std::string(p_value));
		return {};
	}
	const ProfileOption &o = r_state.catalog->stored[i];
	double v = 0.0;
	switch (o.kind) {
		case ProfileOptionKind::Bool: {
			bool b = false;
			const KeyOutcome k = parse_bool_value(p_value, b);
			if (k.fault != KeyFault::None) {
				return k;
			}
			v = b ? 1.0 : 0.0;
			break;
		}
		case ProfileOptionKind::Enum: {
			const auto it = std::find_if(o.items.begin(), o.items.end(),
					[&](const ProfileOptionItem &p_item) { return p_item.display_name == p_value; });
			if (it == o.items.end()) {
				return { KeyFault::BadValue, 0.0, "is not one of the option's items" };
			}
			v = double(it - o.items.begin());
			break;
		}
		case ProfileOptionKind::Int:
		case ProfileOptionKind::Float:
			if (!parse_finite(p_value, v)) {
				return { KeyFault::NotANumber };
			}
			if ((o.kind == ProfileOptionKind::Int && std::floor(v) != v) ||
					((v < o.min || v > o.max) && v != o.value)) {
				return { KeyFault::OutOfRange, v, "is outside the option's range" };
			}
			break;
		case ProfileOptionKind::String:
			if (o.max_length != 0 && p_value.size() > o.max_length) {
				return { KeyFault::BadValue, 0.0, "is longer than the option's MaxLength" };
			}
			r_state.values.texts[i] = std::string(p_value);
			return {};
	}
	r_state.values.numbers[i] = v;
	return {};
}

const KeyDef<LoadState> kSettingsKeys[] = {
	{ kSettingsVersionKey, KeyKind::U32, 1, 1000000, [](LoadState &s) -> void * { return &s.version; } },
};

const KeyValueGrammar<LoadState> kSettingsGrammar{ "settings", kSettingsKeys, kMaxSettingsBytes, kMaxSettingsLines,
	SIZE_MAX, true, '\n', true, true, key_fault_message, nullptr, parse_setting };

}

SettingsStore::SettingsStore(const OptionsCatalog &p_catalog, KeyBindingTable p_bindings) :
		catalog_(&p_catalog), bindings_(std::move(p_bindings)) {
	for (const ProfileOption &o : p_catalog.stored) {
		defaults_.numbers.push_back(o.default_number());
		defaults_.texts.push_back(o.text);
	}
	bindings_.reset();
	live_ = defaults_;
	saved_ = defaults_;
	applied_video_ = defaults_;
	previous_video_ = defaults_;
	saved_bindings_ = bindings_;
}

size_t SettingsStore::index_of(std::string_view p_option) const {
	return catalog_ == nullptr ? 0 : catalog_->option_index(p_option);
}

double SettingsStore::value(std::string_view p_option) const {
	const size_t i = index_of(p_option);
	return i < live_.numbers.size() ? live_.numbers[i] : 0.0;
}

double SettingsStore::default_value(std::string_view p_option) const {
	const size_t i = index_of(p_option);
	return i < defaults_.numbers.size() ? defaults_.numbers[i] : 0.0;
}

double SettingsStore::applied_value(std::string_view p_option) const {
	const size_t i = index_of(p_option);
	return i < applied_video_.numbers.size() ? applied_video_.numbers[i] : 0.0;
}

const std::string &SettingsStore::text(std::string_view p_option) const {
	static const std::string kEmpty;
	const size_t i = index_of(p_option);
	return i < live_.texts.size() ? live_.texts[i] : kEmpty;
}

bool SettingsStore::valid(size_t p_index, double p_value) const {
	if (catalog_ == nullptr || p_index >= catalog_->stored.size() || !std::isfinite(p_value)) {
		return false;
	}
	const ProfileOption &o = catalog_->stored[p_index];
	switch (o.kind) {
		case ProfileOptionKind::Bool:
			return p_value == 0.0 || p_value == 1.0;
		case ProfileOptionKind::Enum:
			return std::floor(p_value) == p_value && p_value >= 0.0 && p_value < double(o.items.size());
		case ProfileOptionKind::Int:
			return std::floor(p_value) == p_value && ((p_value >= o.min && p_value <= o.max) || p_value == o.value);
		case ProfileOptionKind::Float:
			return (p_value >= o.min && p_value <= o.max) || p_value == o.value;
		case ProfileOptionKind::String:
			return false;
	}
	return false;
}

bool SettingsStore::set_value(std::string_view p_option, double p_value) {
	const size_t i = index_of(p_option);
	if (!valid(i, p_value)) {
		return false;
	}
	if (live_.numbers[i] != p_value) {
		live_.numbers[i] = p_value;
		++revision_;
	}
	return true;
}

bool SettingsStore::set_text(std::string_view p_option, std::string_view p_text) {
	const size_t i = index_of(p_option);
	if (catalog_ == nullptr || i >= catalog_->stored.size() || catalog_->stored[i].kind != ProfileOptionKind::String ||
			(catalog_->stored[i].max_length != 0 && p_text.size() > catalog_->stored[i].max_length) ||
			p_text.find_first_of("\r\n#") != std::string_view::npos || p_text != trim(p_text)) {
		return false;
	}
	if (live_.texts[i] != p_text) {
		live_.texts[i] = std::string(p_text);
		++revision_;
	}
	return true;
}

double SettingsStore::setting_value(std::string_view p_source) const {
	const OptionSetting *s = catalog_ == nullptr ? nullptr : catalog_->setting(p_source);
	return s == nullptr ? 0.0 : value(s->option_id);
}

bool SettingsStore::change(std::string_view p_source, double p_value) {
	const OptionSetting *s = catalog_ == nullptr ? nullptr : catalog_->setting(p_source);
	if (s == nullptr || s->kind == OptionKind::DisplayMode) {
		return false;
	}
	return set_value(s->option_id, p_value);
}

bool SettingsStore::set_display_mode(uint32_t p_width, uint32_t p_height, uint32_t p_hertz) {
	const size_t w = index_of(kResolutionWidth);
	const size_t h = index_of(kResolutionHeight);
	const size_t z = index_of(kResolutionHertz);
	if (!valid(w, p_width) || !valid(h, p_height) || !valid(z, p_hertz)) {
		return false;
	}
	set_value(kResolutionWidth, p_width);
	set_value(kResolutionHeight, p_height);
	set_value(kResolutionHertz, p_hertz);
	return true;
}

bool SettingsStore::settings_changed() const {
	return !(live_ == saved_) || bindings_.overrides() != saved_bindings_.overrides();
}

void SettingsStore::copy_video(const SettingsValues &p_from, SettingsValues &r_to) const {
	for (std::string_view id : kVideoOptions) {
		const size_t i = index_of(id);
		if (i < p_from.numbers.size() && i < r_to.numbers.size()) {
			r_to.numbers[i] = p_from.numbers[i];
		}
	}
}

bool SettingsStore::video_equal(const SettingsValues &p_a, const SettingsValues &p_b) const {
	for (std::string_view id : kVideoOptions) {
		const size_t i = index_of(id);
		if (i < p_a.numbers.size() && i < p_b.numbers.size() && p_a.numbers[i] != p_b.numbers[i]) {
			return false;
		}
	}
	return true;
}

bool SettingsStore::video_settings_changed() const {
	return !video_equal(live_, applied_video_);
}

void SettingsStore::save() {
	saved_ = live_;
	saved_bindings_ = bindings_;
	++saved_revision_;
}

void SettingsStore::revert() {
	if (!settings_changed()) {
		return;
	}
	live_ = saved_;
	bindings_ = saved_bindings_;
	++revision_;
}

void SettingsStore::reset_to_defaults() {
	live_ = defaults_;
	bindings_.reset();
	++revision_;
}

void SettingsStore::apply_video() {
	previous_video_ = applied_video_;
	applied_video_ = live_;
	++revision_;
}

void SettingsStore::keep_video() {
	previous_video_ = applied_video_;
}

void SettingsStore::revert_video() {
	const bool saved_new = video_equal(saved_, applied_video_);
	copy_video(previous_video_, live_);
	if (saved_new) {
		copy_video(previous_video_, saved_);
		++saved_revision_;
	}
	applied_video_ = live_;
	previous_video_ = live_;
	++revision_;
}

std::string SettingsStore::saved_text() const {
	std::string out;
	out += std::string(kSettingsVersionKey) + " = " + std::to_string(kSettingsVersion) + "\n";
	if (catalog_ == nullptr) {
		return out;
	}
	for (size_t i = 0; i < catalog_->stored.size(); ++i) {
		const ProfileOption &o = catalog_->stored[i];
		out += o.id + " = ";
		const double v = saved_.numbers[i];
		switch (o.kind) {
			case ProfileOptionKind::Bool:
				out += v != 0.0 ? "true" : "false";
				break;
			case ProfileOptionKind::Enum:
				out += size_t(v) < o.items.size() ? o.items[size_t(v)].display_name : std::string();
				break;
			case ProfileOptionKind::Int:
				out += std::to_string(int64_t(v));
				break;
			case ProfileOptionKind::Float:
				out += float_text(v);
				break;
			case ProfileOptionKind::String:
				out += saved_.texts[i];
				break;
		}
		out += "\n";
	}
	for (const auto &[k, v] : saved_bindings_.overrides()) {
		out += k + " = " + v + "\n";
	}
	for (const auto &[k, v] : unknown_) {
		out += k + " = " + v + "\n";
	}
	return out;
}

Result<uint32_t> SettingsStore::load_text(std::string_view p_text) {
	if (catalog_ == nullptr) {
		return make_error("settings: no catalog");
	}
	LoadState start;
	start.catalog = catalog_;
	start.values = defaults_;
	start.bindings = bindings_;
	start.bindings.reset();
	Result<LoadState> parsed = parse_key_values(p_text, kSettingsGrammar, std::move(start));
	if (!parsed) {
		return parsed.error();
	}
	if (parsed->version == 0) {
		return make_error("settings: no version line");
	}
	if (parsed->version > kSettingsVersion) {
		return make_error("settings: version " + std::to_string(parsed->version) + " is newer than " +
				std::to_string(kSettingsVersion));
	}
	live_ = parsed->values;
	saved_ = parsed->values;
	applied_video_ = parsed->values;
	previous_video_ = parsed->values;
	bindings_ = parsed->bindings;
	saved_bindings_ = parsed->bindings;
	unknown_ = std::move(parsed->unknown);
	++revision_;
	return parsed->version;
}

}
