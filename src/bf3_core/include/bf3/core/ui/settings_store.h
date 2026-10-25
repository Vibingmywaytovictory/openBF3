#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/key_bindings.h"
#include "bf3/core/ui/options.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core::ui {

inline constexpr uint32_t kSettingsVersion = 1;
inline constexpr size_t kMaxSettingsBytes = 256 * 1024;
inline constexpr size_t kMaxSettingsLines = 4096;
inline constexpr std::string_view kSettingsVersionKey = "version";

inline constexpr std::string_view kVideoOptions[] = { kResolutionWidth, kResolutionHeight, kResolutionHertz,
	"FullscreenEnabled", "FullscreenScreen" };

struct SettingsValues {
	std::vector<double> numbers;
	std::vector<std::string> texts;
	bool operator==(const SettingsValues &) const = default;
};

class SettingsStore {
public:
	SettingsStore() = default;
	SettingsStore(const OptionsCatalog &p_catalog, KeyBindingTable p_bindings);

	const OptionsCatalog *catalog() const { return catalog_; }

	double value(std::string_view p_option) const;
	const std::string &text(std::string_view p_option) const;
	bool set_value(std::string_view p_option, double p_value);
	bool set_text(std::string_view p_option, std::string_view p_text);
	double default_value(std::string_view p_option) const;

	double setting_value(std::string_view p_source) const;
	bool change(std::string_view p_source, double p_value);
	bool set_display_mode(uint32_t p_width, uint32_t p_height, uint32_t p_hertz);

	KeyBindingTable &key_bindings() { return bindings_; }
	const KeyBindingTable &key_bindings() const { return bindings_; }
	const KeyBindingTable &saved_key_bindings() const { return saved_bindings_; }

	bool settings_changed() const;
	bool video_settings_changed() const;
	void save();
	void revert();
	void reset_to_defaults();
	void apply_video();
	void keep_video();
	void revert_video();

	uint64_t revision() const { return revision_; }
	void touch() { ++revision_; }

	std::string saved_text() const;
	Result<uint32_t> load_text(std::string_view p_text);
	const std::vector<std::pair<std::string, std::string>> &unknown() const { return unknown_; }

private:
	size_t index_of(std::string_view p_option) const;
	bool valid(size_t p_index, double p_value) const;
	void copy_video(const SettingsValues &p_from, SettingsValues &r_to) const;
	bool video_equal(const SettingsValues &p_a, const SettingsValues &p_b) const;

	const OptionsCatalog *catalog_ = nullptr;
	SettingsValues defaults_;
	SettingsValues live_;
	SettingsValues saved_;
	SettingsValues applied_video_;
	SettingsValues previous_video_;
	KeyBindingTable bindings_;
	KeyBindingTable saved_bindings_;
	std::vector<std::pair<std::string, std::string>> unknown_;
	uint64_t revision_ = 0;
};

}
