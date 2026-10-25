#pragma once

#include "bf3/core/ui/as2_clip_loader.h"
#include "bf3/core/ui/hud_screen.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace bf3::core::ui {

class ImageManagerWidget final : public HudWidgetController {
public:
	using Callback = std::function<void(std::string_view p_message, ImageManagerWidget &r_manager)>;

	void construct(HudWidgetHost &r_host) override;
	void on_clip_load(HudWidgetHost &r_host) override;
	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override;
	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override;
	void advance(HudWidgetHost &r_host, double p_seconds) override;

	void update_image_url(HudWidgetHost &r_host, std::string_view p_url);
	void unload_image(HudWidgetHost &r_host);
	void show(HudWidgetHost &r_host);
	void hide(HudWidgetHost &r_host);
	void register_callback(Callback p_callback) { callback_ = std::move(p_callback); }

	bool is_loaded() const { return loaded_; }
	bool contains_image() const { return contains_image_; }
	bool currently_loading() const { return currently_loading_; }
	const std::string &url() const { return url_; }
	const As2Clip &holder() const { return holder_; }
	const As2ClipLoader &loader() const { return loader_; }

	bool resize_to_target = false;
	bool resize_to_target_width = false;
	bool resize_to_target_height = false;
	bool center_image = false;
	bool center_image_on_origo = false;
	int32_t id = 0;

private:
	void load_image(HudWidgetHost &r_host);
	void texture_done(HudWidgetHost &r_host, bool p_result);
	void image_fully_loaded(HudWidgetHost &r_host);
	void dequeue(HudWidgetHost &r_host);
	void handle_call(std::string_view p_message);
	void show_spinner(HudWidgetHost &r_host);
	void hide_spinner(HudWidgetHost &r_host);

	As2Clip holder_;
	As2ClipLoader loader_;
	std::string url_;
	std::optional<std::string> queued_;
	Callback callback_;
	float start_w_ = 0.0f;
	float start_h_ = 0.0f;
	bool loaded_ = false;
	bool contains_image_ = false;
	bool currently_loading_ = false;
	bool texture_pending_ = false;
	bool texture_requested_ = false;
};

}
