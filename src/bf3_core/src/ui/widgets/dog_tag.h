#pragma once

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_screen.h"

#include <functional>
#include <string>
#include <string_view>

namespace bf3::core::ui {

class ImageManagerWidget;

class DogTagTextWidget final : public HudWidgetController {
public:
	static constexpr float kTextFieldWidth = 190.0f;

	void construct(HudWidgetHost &r_host) override;

	void set_text(std::string_view p_text);
	const std::string &text() const { return text_; }

	bool inverted = false;

private:
	HudWidgetHost *host_ = nullptr;
	std::string text_;
};

class DogTagWidget final : public HudWidgetController {
public:
	static constexpr float kTagWidth = 256.0f;
	static constexpr float kTagHeight = 256.0f;
	static constexpr float kGamertagX = 49.0f;
	static constexpr float kGamertagY = 116.0f;
	static constexpr float kClanX = 49.0f;
	static constexpr float kClanY = 95.0f;
	static constexpr float kStatsX = 34.0f;
	static constexpr float kStatsY = 137.0f;
	static constexpr float kStatNameX = 34.0f;
	static constexpr float kStatNameY = 156.0f;
	static constexpr std::string_view kInactiveBasic = "UI/Art/Persistence/DogTag/inactivetag_left";
	static constexpr std::string_view kInactiveAdvanced = "UI/Art/Persistence/DogTag/inactivetag_right";

	void construct(HudWidgetHost &r_host) override;

	void initialize_tag(std::string_view p_type, bool p_is_static);
	void update_dog_tag(As2Value p_data);
	ImageManagerWidget *image_manager() const;

	const std::string &type() const { return type_; }
	bool is_static() const { return is_static_; }
	const As2Clip &image_manager_clip() const { return image_manager_; }
	const As2Clip &name_field() const { return name_field_; }
	const As2Clip &clan_field() const { return clan_field_; }
	const As2Clip &stats_field() const { return stats_field_; }
	const As2Clip &tag_name_field() const { return tag_name_field_; }

private:
	void add_image_manager();
	void update_text_fields(const As2Value &p_data);
	As2Clip text_field(As2Clip p_field, bool p_right_aligned, bool p_inverted);
	void place_field(
			As2Clip &r_field, bool p_right_aligned, bool p_inverted, float p_x, float p_y, const As2Value &p_content);

	HudWidgetHost *host_ = nullptr;
	std::string type_;
	bool is_static_ = false;
	As2Clip image_manager_;
	As2Clip name_field_;
	As2Clip clan_field_;
	As2Clip stats_field_;
	As2Clip tag_name_field_;
};

class DogTagPairWidget final : public HudWidgetController {
public:
	using Callback = std::function<void(bool p_loaded)>;

	static constexpr float kBasicX = 19.0f;
	static constexpr float kBasicY = -65.0f;
	static constexpr float kBasicRotation = 8.0f;
	static constexpr float kAdvancedX = 190.0f;
	static constexpr float kAdvancedY = -45.0f;
	static constexpr float kAdvancedRotation = -5.0f;
	static constexpr int32_t kCurrentlySelectedTag = 0;
	static constexpr std::string_view kBasic = "basic";
	static constexpr std::string_view kAdvanced = "advanced";

	void construct(HudWidgetHost &r_host) override;
	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override;
	bool input_concept(HudWidgetHost &r_host, int32_t p_concept, bool p_pressed) override;

	void initialize_pair(const As2Value &p_init_data);
	void update_basic_data(const As2Value &p_data);
	void update_advanced_data(const As2Value &p_data);
	void switch_active_tag();
	void register_callback(Callback p_callback) { callback_ = std::move(p_callback); }

	bool is_loaded() const { return loaded_; }
	bool is_static() const { return static_; }
	const std::string &selected() const { return selected_; }
	const As2Clip &basic_tag() const { return basic_; }
	const As2Clip &advanced_tag() const { return advanced_; }
	DogTagWidget *basic() const;
	DogTagWidget *advanced() const;

private:
	void initialize_tag(const As2Clip &p_tag, std::string_view p_type, float p_x, float p_y, float p_rotation);
	void image_callback();
	static void set_color(const As2Clip &p_tag, bool p_inactive);

	HudWidgetHost *host_ = nullptr;
	As2Clip holder_;
	As2Clip basic_;
	As2Clip advanced_;
	std::string selected_ = std::string(kBasic);
	Callback callback_;
	bool loaded_ = false;
	bool static_ = false;
};

}
