#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"
#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/avm1.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

inline constexpr std::string_view kHudMpScreenPartition = "ui/flow/screen/hudmpscreen";
inline constexpr std::string_view kHudTdmScreenPartition = "ui/flow/screen/hudtdmscreen";
inline constexpr std::string_view kCrosshairDefaultPartition = "ui/flow/screen/weapon/crosshairdefault";
inline constexpr std::string_view kHudSharedScreenPartition = "ui/flow/screen/hudscreen";
inline constexpr std::string_view kKillScreenPartition = "ui/flow/screen/killscreen";
inline constexpr std::string_view kSpawnScreenPcPartition = "ui/flow/screen/spawnscreenpc";
inline constexpr std::string_view kScoreboardHud32ScreenPartition = "ui/flow/screen/scoreboards/scoreboardtwoteamshud32screen";
inline constexpr std::string_view kPreRoundWaitingScreenPartition = "ui/flow/screen/preroundwaitingscreen";
inline constexpr std::string_view kEorWinningTeamScreenPartition = "ui/flow/screen/eorwinningteamscreen";
inline constexpr std::string_view kHudScreenInstance = "instance1";
inline constexpr size_t kHudMaxScreenWidgets = 64;
inline constexpr size_t kHudMaxWidgetProperties = 32;

inline constexpr float kHudFootageSafeX = 49.806f;
inline constexpr float kHudFootageSafeY = 28.195f;

enum class HudAxisAlign : uint8_t {
	Near = 0,
	Centre = 1,
	Far = 2,
};

struct HudWidgetProperty {
	std::string name;
	std::string value;
};

struct HudDataSource {
	std::string name;
	int32_t key = 0;
	std::string category;
	std::string category_short_name;
	bool on_initialize = true;
	bool use_direct_access = false;
};

struct HudListBinding {
	HudDataSource list_query;
	std::string row_type;
	std::string empty_row_type;
	std::string navigation_type;
	std::string unfocus_mode;
	std::string disabled_rows;
	int32_t list_index = 0;
	int32_t row_spacing = 0;
	int32_t default_highlighted_row = -1;
	bool use_scroll_bar = false;
	bool high_light_on_update = false;
	bool visible = true;
	bool clear_list_at_navigation_event = false;
	bool data_includes_button_layout = false;
	bool fire_highlight_on_mouse_out = false;
	bool screen_rotation_enabled = false;
	bool send_index_with_event = false;
	bool spinner_enabled = false;
	bool use_3d_selection = false;
};

struct HudWidgetEventPort {
	std::string name;
	std::string query;
	bool output = false;
	bool operator==(const HudWidgetEventPort &) const = default;
};

inline constexpr size_t kHudMaxWidgetEvents = 64;

struct HudWidgetNode {
	std::string instance;
	std::string name;
	std::string widget_asset;
	std::vector<HudWidgetEventPort> events;
	int32_t focus_index = 0;
	bool always_in_focus = false;
	HudAxisAlign horizontal = HudAxisAlign::Centre;
	HudAxisAlign vertical = HudAxisAlign::Centre;
	int32_t z_depth_level = 0;
	std::vector<HudWidgetProperty> properties;
	std::vector<HudDataSource> sources;
	std::optional<HudDataSource> message_query;
	std::optional<HudDataSource> widget_path_query;
	std::optional<HudListBinding> list;
	int32_t number_of_rows = 0;
	std::optional<double> dispersion_base_offset;

	const HudDataSource *source(std::string_view p_name) const;
};

struct HudScreenAsset {
	std::string partition;
	std::string name;
	std::string movie;
	std::vector<HudWidgetNode> widgets;

	const HudWidgetNode *find(std::string_view p_instance) const;
};

Result<HudScreenAsset> read_hud_screen_asset(const EbxResolver &p_db, std::string_view p_partition);

std::string hud_screen_movie(std::string_view p_screen_name);
HudScreenAsset hud_movie_screen_asset(std::string_view p_movie);

struct As2Value {
	enum class Kind : uint8_t {
		Undefined,
		Bool,
		Number,
		String,
		NumberList,
		Object,
		List,
	};
	Kind kind = Kind::Undefined;
	bool boolean = false;
	double number = 0.0;
	std::string text;
	std::vector<double> list;
	std::vector<std::string> names;
	std::vector<As2Value> items;

	static As2Value undefined() { return {}; }
	static As2Value of(bool p_b);
	static As2Value of(double p_n);
	static As2Value of(int32_t p_n) { return of(double(p_n)); }
	static As2Value of(std::string p_s);
	static As2Value of(const char *p_s) { return of(std::string(p_s)); }
	static As2Value numbers(std::vector<double> p_list);
	static As2Value object();
	static As2Value array(std::vector<As2Value> p_items = {});

	As2Value &set(std::string_view p_name, As2Value p_value);
	const As2Value &field(std::string_view p_name) const;
	As2Value &push(As2Value p_value);
	size_t size() const { return items.size(); }
	const As2Value &at(size_t p_index) const;

	bool defined() const { return kind != Kind::Undefined; }
	bool truthy() const;
	double to_number() const;
	std::string to_string() const;
	bool operator==(const As2Value &) const = default;
};

struct HudDataEntry {
	std::string key;
	As2Value value;
	bool on_initialize = true;
};

using HudData = std::vector<HudDataEntry>;

const As2Value *hud_data_find(const HudData &p_data, std::string_view p_key);

struct HudWidgetInit {
	std::vector<HudWidgetProperty> properties;
	HudData data;

	const std::string *property(std::string_view p_name) const;
	const As2Value *value(std::string_view p_key) const { return hud_data_find(data, p_key); }
};

struct HudStageRects {
	SwfRect visible{ 0.0f, 1280.0f, 0.0f, 720.0f };
	SwfRect original{ 0.0f, 1280.0f, 0.0f, 720.0f };
	float safe_x = kHudFootageSafeX;
	float safe_y = kHudFootageSafeY;
};

float hud_align_axis(HudAxisAlign p_align, float p_original, float p_visible_min, float p_visible_size, float p_original_size, float p_safe);

class HudDamageIndicatorComp;
class HudMessageComp;
struct HudWorldTags;

struct HudPopupButton {
	std::string label;
	int32_t input_concept = -1;
};

struct HudPopup {
	std::string title;
	std::string text;
	std::vector<HudPopupButton> buttons;
	bool show_dimmer = true;
};

struct HudClientState {
	const HudMessageComp *messages = nullptr;
	const HudDamageIndicatorComp *damage = nullptr;
	const HudWorldTags *world_tags = nullptr;
	std::string platform = "win32";
	std::string reload_button;
	bool hardcore = false;
	bool minimap_small = true;
	bool show_air_radar = false;
	bool show_minimap_cross = false;
	bool latency_warning = false;
	float minimap_zoom = 0.0f;
	float vertical_fov_deg = 0.0f;
	float stage_height = 720.0f;
	std::function<std::string(uint32_t)> localise_hash;
	std::function<std::optional<std::string>(std::string_view)> translate;
	const HudPopup *popup = nullptr;
};

struct HudTilt {
	std::string path;
	float x_rotation = 0.0f;
	float y_rotation = 0.0f;
	float perspective_fov = 0.0f;
	bool operator==(const HudTilt &) const = default;
};

struct HudRendererString {
	std::string path;
	std::string renderer;
	bool operator==(const HudRendererString &) const = default;
};

enum class HudTweenEase : uint8_t {
	Linear,
	EaseOutBack,
	ExpoOut,
	EaseOutQuad,
};

enum class HudTweenProperty : uint8_t {
	Alpha,
	Width,
	Height,
	X,
	Y,
	Custom,
};

struct HudTween {
	std::string path;
	HudTweenProperty property = HudTweenProperty::Alpha;
	std::string name;
	HudTweenEase ease = HudTweenEase::Linear;
	float from = 0.0f;
	float to = 0.0f;
	double seconds = 0.0;
	double elapsed = 0.0;
	bool started = false;
	uint32_t on_complete = 0;
};

double hud_tween_ease(HudTweenEase p_ease, double p_t);

struct HudScreenStats {
	uint32_t instances = 0;
	uint32_t controllers = 0;
	uint32_t unknown_classes = 0;
	std::vector<std::string> unknown_class_paths;
	uint32_t widgets_initialized = 0;
	uint32_t widgets_missing = 0;
	uint32_t updates = 0;
	uint32_t refreshes = 0;
	uint32_t tweens_run = 0;
	As2Stats as2;
};

class HudScreen;
class UiDirectAccess;
struct UiDirectAccessApply;

struct HudFrameScript {
	std::string path;
	uint32_t frame = 0;
	const std::vector<std::string> *names = nullptr;

	bool calls(std::string_view p_name) const;
};

class HudRootData {
public:
	virtual ~HudRootData() = default;
	virtual As2Value get_data(std::string_view p_key) const = 0;
	virtual void set_data(std::string_view p_key, const As2Value &p_value) = 0;
};

class HudRootDataMap final : public HudRootData {
public:
	As2Value get_data(std::string_view p_key) const override;
	void set_data(std::string_view p_key, const As2Value &p_value) override;
	const std::map<std::string, As2Value, std::less<>> &values() const { return values_; }
	uint64_t reads() const { return reads_; }
	uint64_t writes() const { return writes_; }

private:
	std::map<std::string, As2Value, std::less<>> values_;
	mutable uint64_t reads_ = 0;
	uint64_t writes_ = 0;
};

class HudWidgetHost {
public:
	HudWidgetHost(HudScreen &r_screen, std::string p_path, std::string p_class);

	const As2Clip &self() const { return self_; }
	As2Clip clip(std::string_view p_relative) const;
	const std::string &path() const { return path_; }
	const std::string &class_name() const { return class_; }
	const As2Value *construct_param(std::string_view p_name) const;
	void set_construct_param(std::string p_name, As2Value p_value);

	void tilt(const As2Clip &p_clip, float p_x_rotation, float p_y_rotation, float p_perspective_fov);
	void renderer(const As2Clip &p_clip, std::string_view p_renderer);
	void add_tween(const As2Clip &p_clip, HudTweenProperty p_property, float p_to, double p_seconds, HudTweenEase p_ease, uint32_t p_on_complete = 0);
	void remove_tweens(const As2Clip &p_clip);
	void add_custom_tween(
			std::string_view p_name, float p_to, double p_seconds, HudTweenEase p_ease, uint32_t p_on_complete = 0);
	uint32_t set_interval(double p_seconds);
	void clear_interval(uint32_t p_id);
	void set_enter_frame(bool p_on);
	void exit_complete();
	std::optional<uint32_t> register_mc(const As2Clip &p_clip, const std::vector<As2Value> &p_args);
	void unregister_mc(uint32_t p_id);
	As2Clip attach_movie(const As2Clip &p_parent, std::string_view p_linkage, std::string_view p_name, int32_t p_depth);
	As2Clip create_empty_movie_clip(const As2Clip &p_parent, std::string_view p_name, int32_t p_depth);
	void remove_movie_clip(const As2Clip &p_clip);
	std::string translate(std::string_view p_text) const;
	const HudClientState &client() const;
	As2Value root_get_data(std::string_view p_key) const;
	void root_set_data(std::string_view p_key, const As2Value &p_value);
	HudScreen &screen() { return screen_; }

	void set_events(std::vector<HudWidgetEventPort> p_events) { events_ = std::move(p_events); }
	const std::vector<HudWidgetEventPort> &events() const { return events_; }
	bool fire_event(int32_t p_index, const As2Value &p_param = As2Value::of(0));
	void fire_query(std::string_view p_query, const As2Value &p_param);
	void request_focus();
	void set_mouse_target(const As2Clip &p_clip, bool p_enabled);
	void set_hit_area(const As2Clip &p_clip, const As2Clip &p_area);
	void add_mouse_wheel_listener();
	float xmouse(const As2Clip &p_clip) const;
	float ymouse(const As2Clip &p_clip) const;

private:
	HudScreen &screen_;
	std::string path_;
	std::string class_;
	As2Clip self_;
	std::map<std::string, As2Value, std::less<>> params_;
	std::vector<HudWidgetEventPort> events_;

	void emit(int32_t p_index, std::string p_query, const As2Value &p_param);
};

enum class HudMouseKind : uint8_t {
	Move,
	RollOver,
	RollOut,
	Press,
	Release,
	ReleaseOutside,
	Wheel,
};

struct HudMouse {
	float stage_x = 0.0f;
	float stage_y = 0.0f;
	int32_t button = 0;
	HudMouseKind kind = HudMouseKind::Move;
	std::string target;
	float wheel = 0.0f;
};

struct HudWidgetEvent {
	std::string widget;
	std::string instance;
	int32_t index = 0;
	std::string query;
	As2Value param;
};

using HudWidgetEventSink = std::function<void(const HudWidgetEvent &)>;

struct HudMouseTarget {
	std::string path;
	std::string widget;
	std::string hit_area;
};

class HudWidgetController {
public:
	virtual ~HudWidgetController() = default;
	virtual bool screen_initialized() const { return true; }
	virtual bool aligns_in_safe_rect() const { return true; }
	virtual void attach_node(HudWidgetHost &, const HudWidgetNode &) {}
	virtual void construct(HudWidgetHost &) {}
	virtual void on_clip_load(HudWidgetHost &) {}
	virtual void pre_update_screen_size(HudWidgetHost &) {}
	virtual void update_screen_size(HudWidgetHost &) {}
	virtual void initialize(HudWidgetHost &, const HudWidgetInit &) {}
	virtual void initialize_screen(HudWidgetHost &) {}
	virtual void update(HudWidgetHost &, std::string_view, const As2Value &) {}
	virtual void drive(HudWidgetHost &, const HudModel &, const HudClientState &) {}
	virtual void advance(HudWidgetHost &, double) {}
	virtual void tween_complete(HudWidgetHost &, uint32_t) {}
	virtual std::optional<float> tween_property(HudWidgetHost &, std::string_view) { return std::nullopt; }
	virtual void set_tween_property(HudWidgetHost &, std::string_view, float) {}
	virtual void interval(HudWidgetHost &, uint32_t) {}
	virtual void enter_frame(HudWidgetHost &) {}
	virtual bool begin_exit(HudWidgetHost &) { return true; }
	virtual void direct_access(HudWidgetHost &, const UiDirectAccessApply &) {}
	virtual void frame_script(HudWidgetHost &, const HudFrameScript &) {}
	virtual void bind(const HudModel &, const HudClientState &, HudData &) const {}
	virtual bool input_concept(HudWidgetHost &, int32_t, bool) { return false; }
	virtual bool key(HudWidgetHost &, int32_t, bool) { return false; }
	virtual bool mouse_move(HudWidgetHost &, const HudMouse &) { return false; }
	virtual bool mouse_press(HudWidgetHost &, const HudMouse &) { return false; }
	virtual bool mouse_release(HudWidgetHost &, const HudMouse &) { return false; }
	virtual bool mouse_wheel(HudWidgetHost &, const HudMouse &) { return false; }
	virtual void in_event(HudWidgetHost &, int32_t, const As2Value &) {}
	virtual void focus_gained(HudWidgetHost &) {}
	virtual void focus_lost(HudWidgetHost &) {}
};

std::unique_ptr<HudWidgetController> make_hud_widget_controller(std::string_view p_class);

bool hud_widget_class_known(std::string_view p_class);

struct HudWidgetState {
	std::string path;
	std::string class_name;
	std::string instance;
	bool screen_widget = false;
	bool initialized = false;
	bool retired = false;
	bool exiting = false;
	bool enter_frame = false;
	std::unique_ptr<HudWidgetController> controller;
	std::unique_ptr<HudWidgetHost> host;
	std::map<std::string, As2Value, std::less<>> sent;
};

class HudScreen {
public:
	static Result<std::unique_ptr<HudScreen>> open(SwfScene &r_scene, const SwfLibrary &p_library, const HudScreenAsset &p_asset, const SwfFontSet *p_fonts,
			HudClientState p_client = {}, HudStageRects p_rects = {});

	HudScreen(const HudScreen &) = delete;
	HudScreen &operator=(const HudScreen &) = delete;
	~HudScreen();

	void initialize(const HudModel &p_model);
	void refresh(const HudModel &p_model);
	void advance(double p_seconds);
	void set_client(HudClientState p_client);
	void set_stage_rects(const HudStageRects &p_rects);
	void set_focus(bool p_focused);
	bool has_focus() const { return focused_; }
	void begin_exit();
	bool enter_complete() const;
	bool exiting() const { return exiting_; }
	bool exit_complete() const { return exiting_ && exits_pending_ == 0 && (exit_called_ || exits_counted_ > 0); }
	size_t intervals() const { return intervals_.size(); }
	UiDirectAccess &direct_access();
	void set_event_sink(HudWidgetEventSink p_sink) { event_sink_ = std::move(p_sink); }
	void set_root_data(HudRootData *p_data) { root_data_ = p_data; }
	HudRootData *root_data() const { return root_data_; }
	bool deliver_in_event(
			std::string_view p_instance, std::string_view p_query, const As2Value &p_param = As2Value::of(0));
	bool input_concept(int32_t p_concept, bool p_pressed);
	bool key(int32_t p_code, bool p_pressed);
	bool mouse_move(float p_stage_x, float p_stage_y);
	bool mouse_button(float p_stage_x, float p_stage_y, int32_t p_button, bool p_pressed);
	bool mouse_wheel(float p_stage_x, float p_stage_y, float p_delta);
	std::string hit_target(float p_stage_x, float p_stage_y) const;
	const std::string &focused_widget() const { return focused_widget_; }
	void focus_widget(std::string_view p_path);
	const std::string &hovered() const { return hover_; }
	const std::vector<HudMouseTarget> &mouse_targets() const { return mouse_targets_; }
	uint64_t events_fired() const { return events_fired_; }
	const HudClientState &client() const { return client_; }
	const HudStageRects &stage_rects() const { return rects_; }

	bool initialized() const { return initialized_; }
	SwfScene &scene() { return *scene_; }
	const SwfScene &scene() const { return *scene_; }
	const SwfFontSet *fonts() const { return fonts_; }
	const HudScreenAsset &asset() const { return asset_; }
	const std::vector<std::unique_ptr<HudWidgetState>> &widgets() const { return widgets_; }
	const HudWidgetState *widget(std::string_view p_path) const;
	HudWidgetController *controller(std::string_view p_path);
	HudWidgetHost *host(std::string_view p_path);
	As2Clip clip(std::string_view p_path);

	const std::vector<HudTilt> &tilts() const { return tilts_; }
	const std::vector<HudRendererString> &renderer_strings() const { return renderers_; }
	uint32_t effects_revision() const { return effects_revision_; }
	const std::vector<HudTween> &tweens() const { return tweens_; }
	HudScreenStats &stats() { return stats_; }
	const HudScreenStats &stats() const { return stats_; }

private:
	friend class HudWidgetHost;
	HudScreen();

	void discover(SwfNodeId p_root);
	void retire(std::string_view p_path);
	void sweep();
	void on_frame_script(SwfNodeId p_node, uint32_t p_frame);
	const std::vector<std::string> &frame_names(const SwfSceneNode &p_node, uint32_t p_frame);
	const Avm1Movie &avm1_of(const SwfSceneNode &p_node);
	void construct_subtree(SwfNodeId p_root);
	void translate_texts(SwfNodeId p_root);
	void align(HudWidgetState &r_widget, const HudWidgetNode &p_node);
	void apply_tween(const HudTween &p_tween, float p_value);
	std::optional<std::string> class_of(const SwfSceneNode &p_node);
	std::vector<std::pair<std::string, As2Value>> construct_params_of(const SwfSceneNode &p_node);

	const SwfLibrary *library_ = nullptr;
	std::map<std::string, std::shared_ptr<const Avm1Movie>, std::less<>> avm1_;
	std::map<std::string, std::vector<std::string>, std::less<>> frame_names_;

	SwfScene *scene_ = nullptr;
	const SwfFontSet *fonts_ = nullptr;
	HudScreenAsset asset_;
	HudClientState client_;
	HudStageRects rects_;
	std::vector<std::unique_ptr<HudWidgetState>> widgets_;
	std::map<std::string, std::pair<float, float>, std::less<>> original_positions_;
	bool constructing_ = false;
	std::vector<HudTilt> tilts_;
	std::vector<HudRendererString> renderers_;
	uint32_t effects_revision_ = 0;
	std::vector<HudTween> tweens_;
	HudScreenStats stats_;
	bool initialized_ = false;
	bool focused_ = false;
	HudWidgetEventSink event_sink_;
	HudRootData *root_data_ = nullptr;
	uint64_t events_fired_ = 0;
	std::vector<HudMouseTarget> mouse_targets_;
	std::vector<std::string> wheel_listeners_;
	std::string hover_;
	std::string pressed_;
	float mouse_x_ = 0.0f;
	float mouse_y_ = 0.0f;
	std::string focused_widget_;
	int32_t focus_index_ = 0;
	struct Interval {
		std::string path;
		uint32_t id = 0;
		double period = 0.0;
		double elapsed = 0.0;
	};
	std::vector<Interval> intervals_;
	uint32_t next_interval_ = 1;
	double frame_clock_ = 0.0;
	bool exiting_ = false;
	bool exit_called_ = false;
	uint32_t exits_pending_ = 0;
	uint32_t exits_counted_ = 0;
	HudWidgetState *owner_of(std::string_view p_path);
	HudWidgetState *deepest_owner_of(std::string_view p_path);
	struct DirectAccessTarget;
	std::unique_ptr<DirectAccessTarget> direct_target_;
	std::unique_ptr<UiDirectAccess> direct_;
	void run_timers(double p_seconds);

	bool dispatch_mouse(const std::string &p_target, HudMouse p_mouse);
	bool takes_focus(const HudWidgetState &p_widget) const;
};

}
