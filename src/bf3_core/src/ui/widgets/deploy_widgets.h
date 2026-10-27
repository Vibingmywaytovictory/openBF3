#pragma once

#include "hud_widget_support.h"

namespace bf3::core::ui {

class HudSpawnTimer {
public:
	virtual ~HudSpawnTimer() = default;
	virtual void deploy_release() = 0;
	virtual bool deployable() const = 0;
	virtual double current_time() const = 0;
	virtual double start_time() const = 0;
	virtual bool can_spawn() const = 0;
	virtual int32_t current_frame() const = 0;
	virtual uint32_t deploys_fired() const = 0;
};
struct HudBarSpec {
	bool visible = true;
	float alpha = 100.0f;
	uint32_t color = 0xFFFFFFu;
	bool animate = false;
	double target_value = 100.0;
	double value = 0.0;
};

class HudAdvancedBar {
public:
	virtual ~HudAdvancedBar() = default;
	virtual void initialize_bars(const std::vector<HudBarSpec> &p_bars) = 0;
	virtual void set_value_on_index(size_t p_index, const As2Value &p_value) = 0;
	virtual void set_use_glow(bool p_use) = 0;
	virtual void set_glow_color(uint32_t p_rgb) = 0;
	virtual size_t bar_count() const = 0;
	virtual double value(size_t p_index) const = 0;
	virtual float bar_width(size_t p_index) const = 0;
	virtual float start_width() const = 0;
};

class HudKitBar {
public:
	virtual ~HudKitBar() = default;
	virtual int32_t current_kit() const = 0;
	virtual int32_t current_slot() const = 0;
	virtual bool changing_item() const = 0;
	virtual void slot_clicked(int32_t p_slot) = 0;
	virtual void kit_button_pressed(const std::string &p_name) = 0;
};
}
