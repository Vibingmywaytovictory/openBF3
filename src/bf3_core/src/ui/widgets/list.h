#pragma once

#include "hud_widget_support.h"

#include "bf3/core/ui/ui_direct_access.h"

namespace bf3::core::ui {

struct ListAccessItem {
	std::string value;
	std::string member;
	UiDirectAccessOp type = UiDirectAccessOp::Text;
	std::string object;
};

class ListOwner {
public:
	virtual ~ListOwner() = default;
	virtual std::vector<uint32_t> register_access_data(const std::vector<ListAccessItem> &p_items, double p_index) = 0;
	virtual void unregister_access_data(const std::vector<uint32_t> &p_ids) = 0;
	virtual void row_released(double p_index, bool p_value) = 0;
	virtual void row_pressed(double p_index) = 0;
	virtual void row_mouse_over(double p_index, bool p_value) = 0;
	virtual void row_highlighted(double p_index, bool p_value) = 0;
};

class RowStateListener {
public:
	virtual ~RowStateListener() = default;
	virtual void row_state(const std::string &p_state) = 0;
};

class HudListLabel {
public:
	virtual ~HudListLabel() = default;
	virtual void set_text(std::string p_text) = 0;
	virtual const std::string &text() const = 0;
	virtual void set_label_width(float p_width) = 0;
	virtual float label_width() const = 0;
	virtual void set_type(std::string_view p_type) = 0;
	virtual std::string type() const = 0;
	virtual float text_width() const = 0;
	virtual void set_scrollable(bool p_scrollable) = 0;
	virtual void set_interactive(bool p_interactive) = 0;
	virtual const std::string &label_state() const = 0;
	virtual void set_state_style(std::string_view p_state, uint32_t p_color, const SwfFilter &p_glow) = 0;
	virtual void set_text_size(float p_size) = 0;
};

inline constexpr std::string_view kSpawnPointListCategory = "ui/uicomponents/uispawnpointcomp";
inline constexpr int32_t kSpawnPointListKey = 1755421927;

inline constexpr int32_t kDefaultSpawnPointKey = -233610778;

std::optional<As2Value> hud_model_list_rows(
		const HudDataSource &p_query, const HudModel &p_model, const HudClientState &p_client);
std::optional<int32_t> hud_model_list_selection(const HudDataSource &p_query, const HudModel &p_model);
std::optional<As2Value> hud_menu_list_rows(const HudDataSource &p_query, const HudModel &p_model);

class RowBaseClass : public HudWidgetController {
public:
	static constexpr double kNoId = -123456789.0;

	void construct(HudWidgetHost &r_host) override;
	bool mouse_move(HudWidgetHost &r_host, const HudMouse &p_mouse) override;
	bool mouse_press(HudWidgetHost &r_host, const HudMouse &p_mouse) override;
	bool mouse_release(HudWidgetHost &r_host, const HudMouse &p_mouse) override;
	void direct_access(HudWidgetHost &r_host, const UiDirectAccessApply &p_apply) override;

	virtual void set_data(const As2Value &p_data);
	const As2Value &data() const { return data_; }
	void set_row_index(double p_index) { index_ = p_index; }
	double row_index() const { return index_; }
	void set_list(std::string p_path) { list_path_ = std::move(p_path); }
	const std::string &list_path() const { return list_path_; }
	void set_type(std::string p_type) { type_ = std::move(p_type); }
	const std::string &type() const { return type_; }
	void set_highlighted(bool p_highlighted);
	bool highlighted() const { return highlighted_; }
	void set_mouse_over_row(bool p_over);
	bool mouse_over_row() const { return over_; }
	void set_disabled(bool p_disabled);
	bool disabled() const { return disabled_; }
	virtual void set_width(double p_width);
	double width() const;
	double height() const;
	void set_mouse_enabled(bool p_enabled);
	bool mouse_enabled() const { return use_mouse_; }
	void set_use_3d(bool p_use_3d) { use_3d_ = p_use_3d; }
	void set_id(const As2Value &p_id);
	const As2Value &id() const { return id_; }
	virtual As2Value label() const { return As2Value::of("ERROR_GetLabel"); }
	const As2Value &button_layouts() const { return button_layouts_; }
	virtual void reset(bool p_destroy);
	void register_state_callback(std::string p_listener_path);
	const std::string &state() const { return state_; }

protected:
	void state_changed();
	virtual void check_for_new_content() const;
	virtual void on_state(const std::string &) {}
	virtual void member(std::string_view p_name, const As2Value &p_value);
	void register_direct_access_array(std::vector<ListAccessItem> p_items);
	ListOwner *owner() const;
	void set_bg_state(const std::string &p_state) const;

	HudWidgetHost *host_ = nullptr;

private:
	void check_states(const As2Value &p_data);
	void setup_mouse_interaction();
	void turn_off_mouse_interaction();
	void handle_glow_size() const;

	As2Value data_;
	double index_ = 0.0;
	std::string list_path_;
	std::string type_;
	std::string state_;
	As2Value id_ = As2Value::of(kNoId);
	As2Value button_layouts_ = As2Value::array();
	std::vector<std::string> state_listeners_;
	std::vector<uint32_t> direct_access_ids_;
	bool use_direct_access_ = false;
	bool disabled_ = false;
	bool over_ = false;
	bool highlighted_ = false;
	bool pressed_ = false;
	bool use_mouse_ = false;
	bool has_mouse_listeners_ = false;
	bool use_3d_ = false;
	std::string unfocus_mode_ = "normal";
};

std::optional<std::vector<As2Value>> hud_model_direct_rows(const HudDataSource &p_query, const HudModel &p_model);

}
