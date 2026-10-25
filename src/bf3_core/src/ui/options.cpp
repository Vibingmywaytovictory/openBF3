#include "bf3/core/ui/options.h"

#include "ui_ebx.h"

#include <algorithm>
#include <cmath>

namespace bf3::core::ui {

namespace {

constexpr std::string_view kOn = "ID_M_OPTIONS_ON";
constexpr std::string_view kOff = "ID_M_OPTIONS_OFF";
constexpr std::string_view kYes = "ID_M_OPTIONS_YES";
constexpr std::string_view kNo = "ID_M_OPTIONS_NO";

constexpr OptionSource kSources[] = {
	{ 2036655012, "VerticalLook", "Scheme0FlipY", OptionValue::Profile, "ID_M_OPTIONS_NORMAL", "ID_M_OPTIONS_INVERT" },
	{ 1197323515, "VerticalFlight", "Scheme2FlipY", OptionValue::Profile, "ID_M_OPTIONS_NORMAL",
			"ID_M_OPTIONS_INVERT" },
	{ -1726867587, "MouseSensitivity", "MouseSensitivity", OptionValue::Profile, {}, {} },
	{ 745800453, "MouseSensitivityVehicle", "MouseSensitivityVehicle", OptionValue::Profile, {}, {} },
	{ -2059361310, "MouseRawInput", "MouseRawInput", OptionValue::Profile, kOff, kOn },
	{ -875263044, "Sensitivity", "Scheme0Sensitivity", OptionValue::Profile, {}, {} },
	{ -1557825949, "Vibration", "Vibration", OptionValue::Profile, kOff, kOn },
	{ 282440422, "Subtitles", "EnableSubtitles", OptionValue::Profile, kNo, kYes },
	{ 380131439, "HintSystem", "HintSystem", OptionValue::Profile, kNo, kYes },
	{ 996376763, "ReloadHint", "ReloadHint", OptionValue::Profile, kNo, kYes },
	{ 1511321364, "ZoomToggle", "HoldButtonToZoom", OptionValue::Profile, "ID_M_TOGGLE", "ID_M_OPTIONS_HOLD" },
	{ 1163588491, "BreathToggle", "HoldForBreathControl", OptionValue::Profile, "ID_M_TOGGLE", "ID_M_OPTIONS_HOLD" },
	{ 2123762470, "NetworkFrequencyFactor", "UserFrequencyFactor", OptionValue::Profile, {}, {} },
	{ -1506947434, "MinimapType", "MinimapType", OptionValue::Profile, {}, {} },
	{ 1768075368, "MinimapTextureOpacity", "MinimapTextureOpacity", OptionValue::Profile, {}, {} },
	{ -446156347, "MasterVolume", "Volume", OptionValue::Profile, {}, {} },
	{ -1243243194, "YourSoundSystem", "YourSoundSystem", OptionValue::Profile, {}, {} },
	{ -28028280, "StereoMode", "StereoMode", OptionValue::Profile, kOff, kOn },
	{ 1021755323, "VoiceOverLanguage", "VOLanguage", OptionValue::Profile, {}, {} },
	{ -738813438, "FullscreenScreen", "FullscreenScreen", OptionValue::Monitor, {}, {} },
	{ -505783905, "Resolution", kResolutionWidth, OptionValue::DisplayMode, {}, {} },
	{ 225189035, "FullscreenEnabled", "FullscreenEnabled", OptionValue::Profile, kOff, kOn },
	{ 1759039576, "Brightness", "Brightness", OptionValue::Profile, {}, {} },
	{ 1574386880, "VSync", "VSync", OptionValue::Profile, kOff, kOn },
	{ 944518935, "FieldOfView", "FieldOfView", OptionValue::Profile, {}, {} },
	{ -1903337626, "MotionBlur", "MotionBlur", OptionValue::Profile, {}, {} },
	{ -908194755, "Stereoscopy", "Stereoscopy", OptionValue::Profile, kOff, kOn },
	{ 778093026, "StereoConvergence", "StereoConvergence", OptionValue::Profile, {}, {} },
	{ 32383342, "ColorblindSupport", "ColorblindSupport", OptionValue::Unbacked, kOff, kOn },
	{ 1089114246, "OverallGraphicsQuality", "OverallGraphicsQuality", OptionValue::Profile, {}, {} },
	{ -522365085, "TextureQuality", "TextureQuality", OptionValue::Profile, {}, {} },
	{ -408184070, "ShadowQuality", "ShadowQuality", OptionValue::Profile, {}, {} },
	{ 1319531928, "EffectsQuality", "EffectsQuality", OptionValue::Profile, {}, {} },
	{ -102852753, "MeshQuality", "MeshQuality", OptionValue::Profile, {}, {} },
	{ 300599499, "TerrainQuality", "TerrainQuality", OptionValue::Profile, {}, {} },
	{ 1475987109, "UndergrowthQuality", "UndergrowthQuality", OptionValue::Profile, {}, {} },
	{ -78368714, "AntiAliasingDeferred", "AntiAliasingDeferred", OptionValue::Profile, {}, {} },
	{ -502331603, "AntiAliasingPost", "AntiAliasingPost", OptionValue::Profile, {}, {} },
	{ 2082600515, "MotionBlurEnabled", "MotionBlurEnabled", OptionValue::Profile, kOff, kOn },
	{ -1132558216, "AnisotropicFilter", "AnisotropicFilter", OptionValue::Profile, {}, {} },
	{ 312315940, "AmbientOcclusion", "AmbientOcclusion", OptionValue::Profile, {}, {} },
};

using detail::text_of;

Result<ProfileOption> option_of(const EbxInstance &p_i) {
	ProfileOption o;
	o.id = text_of(p_i.get("UniqueId"));
	o.category = text_of(p_i.get("Category"));
	if (o.id.empty()) {
		return make_error("profile options: an option without UniqueId");
	}
	const EbxStruct d = p_i.data();
	if (p_i.is_a("ProfileOptionDataBool")) {
		o.kind = ProfileOptionKind::Bool;
		o.value = detail::flag_or(d, "Value", false) ? 1.0 : 0.0;
		o.max = 1.0;
		o.step = 1.0;
	} else if (p_i.is_a("ProfileOptionDataInt") || p_i.is_a("ProfileOptionDataFloat")) {
		o.kind = p_i.is_a("ProfileOptionDataInt") ? ProfileOptionKind::Int : ProfileOptionKind::Float;
		const std::optional<double> mn = detail::number(d.get("Min"));
		const std::optional<double> mx = detail::number(d.get("Max"));
		const std::optional<double> st = detail::number(d.get("Step"));
		const std::optional<double> v = detail::number(d.get("Value"));
		if (!mn || !mx || !st || !v) {
			return make_error("profile options: " + o.id + " lacks Min, Max, Step or Value");
		}
		o.min = *mn;
		o.max = *mx;
		o.step = *st;
		o.value = *v;
	} else if (p_i.is_a("ProfileOptionDataEnum")) {
		o.kind = ProfileOptionKind::Enum;
		const std::optional<EbxValue> items = d.get("Items");
		const OptionalEbxArray arr = items ? items->as_array() : OptionalEbxArray();
		if (!arr || arr->empty() || arr->size() > kMaxProfileOptionItems) {
			return make_error("profile options: " + o.id + " has no Items or more than " +
					std::to_string(kMaxProfileOptionItems));
		}
		for (const EbxValue &v : *arr) {
			const std::optional<EbxStruct> s = v.as_struct();
			if (!s) {
				return make_error("profile options: " + o.id + " has an item that is not a struct");
			}
			o.items.push_back({ text_of(s->get("DisplayName")), detail::flag_or(*s, "Default", false) });
		}
		o.max = double(o.items.size() - 1);
		o.step = 1.0;
		o.value = double(o.default_item());
	} else if (p_i.is_a("ProfileOptionDataString")) {
		o.kind = ProfileOptionKind::String;
		o.text = text_of(d.get("Value"));
		o.max_length = uint32_t(std::max(0, detail::i32_or(d, "MaxLength", 0)));
	} else {
		return make_error("profile options: " + o.id + " is a " + std::string(p_i.type_name()));
	}
	return o;
}

OptionRowType row_type_of(std::string_view p_enum, bool &r_known) {
	r_known = true;
	if (p_enum.starts_with("TextSelectorRow")) {
		return OptionRowType::TextSelector;
	}
	if (p_enum.starts_with("BarSelectorRow")) {
		return OptionRowType::BarSelector;
	}
	r_known = false;
	return OptionRowType::TextSelector;
}

}

uint32_t ProfileOption::default_item() const {
	for (size_t i = 0; i < items.size(); ++i) {
		if (items[i].is_default) {
			return uint32_t(i);
		}
	}
	return 0;
}

double ProfileOption::default_number() const {
	return kind == ProfileOptionKind::Enum ? double(default_item()) : value;
}

const ProfileOption *ProfileOptions::find(std::string_view p_id) const {
	const auto it = std::find_if(options.begin(), options.end(), [&](const ProfileOption &o) { return o.id == p_id; });
	return it == options.end() ? nullptr : &*it;
}

Result<ProfileOptions> read_profile_options(const EbxResolver &p_db) {
	const EbxPartition *part = p_db.partition_by_name(kProfileOptionsPartition);
	if (part == nullptr) {
		return make_error("profile options: no partition " + std::string(kProfileOptionsPartition));
	}
	const EbxInstance root = part->primary();
	if (!root.is_a("ProfileOptionsAsset")) {
		return make_error("profile options: the primary is a " + std::string(root.type_name()));
	}
	ProfileOptions out;
	out.file_name = text_of(root.get("FileName"));
	out.file_size = uint32_t(std::max(0, detail::i32_or(root.data(), "FileSize", 0)));
	out.auto_save_on_quit = detail::flag_or(root.data(), "AutoSaveOnQuit", false);
	const std::vector<EbxInstance> options = p_db.follow_all(root.data(), "Options");
	if (options.empty() || options.size() > kMaxProfileOptions) {
		return make_error("profile options: " + std::to_string(options.size()) + " options");
	}
	for (const EbxInstance &i : options) {
		Result<ProfileOption> o = option_of(i);
		if (!o) {
			return o.error();
		}
		if (out.find(o->id) != nullptr) {
			return make_error("profile options: " + o->id + " given twice");
		}
		out.options.push_back(std::move(*o));
	}
	return out;
}

std::string_view options_tab_name(OptionsTab p_tab) {
	constexpr std::array<std::string_view, kOptionsTabCount> kNames = { "controls", "gameplay", "audio", "video",
		"keybindings" };
	return size_t(p_tab) < kNames.size() ? kNames[size_t(p_tab)] : std::string_view("?");
}

Result<std::vector<OptionsListRow>> read_options_rows(const EbxResolver &p_db, OptionsTab p_tab) {
	if (size_t(p_tab) >= kOptionsTabCount) {
		return make_error("options: no such tab");
	}
	const std::string_view name = kOptionsScreens[size_t(p_tab)];
	const EbxPartition *part = p_db.partition_by_name(name);
	if (part == nullptr) {
		return make_error("options: no partition " + std::string(name));
	}
	std::vector<OptionsListRow> out;
	for (const EbxInstance &n : p_db.follow_all(part->primary().data(), "Nodes")) {
		if (!n.is_a("WidgetNode")) {
			continue;
		}
		const std::optional<EbxInstance> b = p_db.follow(n.data(), "DataBinding");
		if (!b || !b->is_a("UINestedListDataBinding")) {
			continue;
		}
		const std::string list = text_of(n.get("InstanceName"));
		const std::optional<EbxValue> lists = b->get("NestedLists");
		const OptionalEbxArray arr = lists ? lists->as_array() : OptionalEbxArray();
		if (!arr) {
			continue;
		}
		for (const EbxValue &v : *arr) {
			const std::optional<EbxStruct> s = v.as_struct();
			if (!s) {
				continue;
			}
			if (out.size() >= kMaxOptionsListRows) {
				return make_error("options: " + std::string(name) + " has more than " +
						std::to_string(kMaxOptionsListRows) + " rows");
			}
			OptionsListRow r;
			r.tab = p_tab;
			r.list = list;
			r.label = text_of(s->get("Label"));
			bool known = false;
			const std::string row = text_of(s->get("RowType"));
			r.row = row_type_of(row, known);
			if (!known) {
				return make_error("options: " + r.label + " has row type " + row);
			}
			if (const std::optional<EbxStruct> src = detail::sub(*s, "ListDataSource")) {
				r.data_key = detail::i32_or(*src, "DataKey", 0);
			}
			if (const std::optional<EbxStruct> show = detail::sub(*s, "DynamicShowList")) {
				r.show_key = detail::i32_or(*show, "DataKey", 0);
			}
			r.hidden_on_pc = detail::flag_or(*s, "HiddenOnPC", false);
			out.push_back(std::move(r));
		}
	}
	return out;
}

std::span<const OptionSource> option_sources() {
	return kSources;
}

const OptionSource *option_source_of_key(int32_t p_data_key) {
	for (const OptionSource &s : kSources) {
		if (s.data_key == p_data_key) {
			return &s;
		}
	}
	return nullptr;
}

const OptionSource *option_source_named(std::string_view p_source) {
	for (const OptionSource &s : kSources) {
		if (s.source == p_source) {
			return &s;
		}
	}
	return nullptr;
}

const ProfileOption *OptionsCatalog::option(std::string_view p_id) const {
	const size_t i = option_index(p_id);
	return i < stored.size() ? &stored[i] : nullptr;
}

size_t OptionsCatalog::option_index(std::string_view p_id) const {
	const auto it = std::find_if(stored.begin(), stored.end(), [&](const ProfileOption &o) { return o.id == p_id; });
	return size_t(it - stored.begin());
}

const OptionSetting *OptionsCatalog::setting(std::string_view p_source) const {
	const auto it = std::find_if(
			settings.begin(), settings.end(), [&](const OptionSetting &s) { return s.source == p_source; });
	return it == settings.end() ? nullptr : &*it;
}

Result<OptionsCatalog> read_options_catalog(const EbxResolver &p_db) {
	Result<ProfileOptions> profile = read_profile_options(p_db);
	if (!profile) {
		return profile.error();
	}
	OptionsCatalog out;
	out.profile_file = profile->file_name;
	out.stored = std::move(profile->options);
	for (size_t t = 0; t < kOptionsTabCount; ++t) {
		Result<std::vector<OptionsListRow>> rows = read_options_rows(p_db, OptionsTab(t));
		if (!rows) {
			return rows.error();
		}
		for (const OptionsListRow &r : *rows) {
			if (r.hidden_on_pc) {
				continue;
			}
			const OptionSource *src = option_source_of_key(r.data_key);
			if (src == nullptr) {
				return make_error("options: " + std::string(options_tab_name(r.tab)) + " row " + r.label + " key " +
						std::to_string(r.data_key) + " has no measured source");
			}
			OptionSetting s;
			s.source = std::string(src->source);
			s.tab = r.tab;
			s.list = r.list;
			s.label = r.label;
			s.row = r.row;
			s.data_key = r.data_key;
			s.option_id = std::string(src->profile_id);
			if (src->value == OptionValue::Unbacked && out.option(src->profile_id) == nullptr) {
				ProfileOption o;
				o.id = std::string(src->profile_id);
				o.kind = ProfileOptionKind::Bool;
				o.max = 1.0;
				o.step = 1.0;
				o.from_profile = false;
				out.stored.push_back(std::move(o));
			}
			const ProfileOption *o = out.option(src->profile_id);
			if (o == nullptr) {
				return make_error("options: " + s.source + " has no profile option " + std::string(src->profile_id));
			}
			s.default_value = o->default_number();
			if (src->value == OptionValue::DisplayMode) {
				s.kind = OptionKind::DisplayMode;
				if (out.option(kResolutionHeight) == nullptr || out.option(kResolutionHertz) == nullptr) {
					return make_error("options: Resolution lacks ResolutionHeight or ResolutionHertz");
				}
			} else if (src->value == OptionValue::Monitor) {
				s.kind = OptionKind::Monitor;
				s.min = o->min;
				s.max = o->max;
				s.step = o->step;
			} else if (o->kind == ProfileOptionKind::Bool) {
				if (src->off_sid.empty() || src->on_sid.empty()) {
					return make_error("options: bool " + s.source + " has no choice labels");
				}
				s.kind = OptionKind::Bool;
				s.max = 1.0;
				s.step = 1.0;
				s.choices = { std::string(src->off_sid), std::string(src->on_sid) };
			} else if (o->kind == ProfileOptionKind::Enum) {
				s.kind = OptionKind::Enum;
				s.max = o->max;
				s.step = 1.0;
				for (const ProfileOptionItem &i : o->items) {
					s.choices.push_back(i.display_name);
				}
			} else if (o->kind == ProfileOptionKind::Int || o->kind == ProfileOptionKind::Float) {
				s.kind = OptionKind::Number;
				s.min = o->min;
				s.max = o->max;
				s.step = o->step;
				if (s.row == OptionRowType::TextSelector) {
					if (!(o->step > 0.0) || std::floor(o->step) != o->step ||
							(o->max - o->min) / o->step > double(kMaxProfileOptionItems * 4)) {
						return make_error("options: " + s.source + " is a text row over too many numbers");
					}
					for (double v = o->min; v <= o->max + o->step * 0.5; v += o->step) {
						s.choices.push_back(std::to_string(int64_t(std::llround(v))));
					}
				}
			} else {
				return make_error("options: " + s.source + " is backed by a string option");
			}
			out.settings.push_back(std::move(s));
		}
	}
	return out;
}

}
