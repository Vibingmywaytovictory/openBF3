#include "bf3/core/ui/loading_screen.h"

#include "bf3/core/ui/as2_clip_loader.h"
#include "bf3/core/util/splitmix64.h"
#include "hud_widget_support.h"
#include "scroll_text.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace bf3::core::ui {

namespace {

constexpr double kHintIntervalSeconds = 13.0;
constexpr float kDescriptionPadding = 10.0f;
constexpr std::string_view kPlatformWin32 = "win32";
constexpr std::string_view kPlatformPs3 = "ps3";
constexpr std::string_view kPlatformXenon = "xenon";
constexpr std::string_view kDebugLevelName = "OILFIELDS";
constexpr std::string_view kWaitingMessage = "ID_M_LOADING";

struct ModeText {
	std::string_view game_mode;
	std::string_view header;
	std::string_view description;
};

constexpr std::array<ModeText, 13> kModeTexts{ {
		{ "team deathmatch", "ID_M_LOAD_TEAM_DEATHMATCH", "ID_M_TEAM_DEATHMATCH_DESC" },
		{ "squad deathmatch", "ID_M_LOAD_SQUAD_DEATHMATCH", "ID_M_SQUAD_DEATHMATCH_DESC" },
		{ "conquest", "ID_M_LOAD_CONQUEST", "ID_M_CONQUEST_DESC" },
		{ "rush", "ID_M_LOAD_RUSH", "ID_M_RUSH_DESC" },
		{ "squad rush", "ID_M_LOAD_SQUAD_RUSH", "ID_M_SQUAD_RUSH_DESC" },
		{ "conquest assault", "ID_M_LOAD_CONQUEST_ASSAULT", "ID_M_CONQUEST_ASSAULT_DESC" },
		{ "capturetheflag", "ID_M_LOAD_GAMEMODE_CTF", "ID_M_XP5_CTF_DESC" },
		{ "gunmaster", "ID_M_LOAD_GUNMASTER", "ID_M_LOAD_GUNMASTER_DESCR" },
		{ "domination", "ID_M_LOAD_DOMINATION", "ID_M_LOAD_DOMINATION_DESCR" },
		{ "team deathmatch cq", "ID_M_LOAD_TEAM_DEATHMATCHC", "ID_M_TEAM_DEATHMATCHC_DESC" },
		{ "tank superiority", "ID_M_LOAD_TANK_SUPERIORITY", "ID_M_LOAD_TANK_SUPERIORITY_DESC" },
		{ "scavenger", "ID_M_LOAD_SCAVENGER", "ID_M_LOAD_SCAVENGER_DESC" },
		{ "air superiority", "ID_M_LOAD_AIRSUPERIORITY", "ID_M_LOAD_AIRSUPERIORITY_DESC" },
} };

struct ModeId {
	std::string_view prefix;
	std::string_view game_mode;
};

constexpr std::array<ModeId, 14> kModeIds{ {
		{ "TeamDeathMatchC", "team deathmatch cq" },
		{ "TeamDeathMatch", "team deathmatch" },
		{ "SquadDeathMatch", "squad deathmatch" },
		{ "ConquestAssault", "conquest assault" },
		{ "ConquestLarge", "conquest" },
		{ "ConquestSmall", "conquest" },
		{ "SquadRush", "squad rush" },
		{ "RushLarge", "rush" },
		{ "CaptureTheFlag", "capturetheflag" },
		{ "GunMaster", "gunmaster" },
		{ "Domination", "domination" },
		{ "TankSuperiority", "tank superiority" },
		{ "Scavenger", "scavenger" },
		{ "AirSuperiority", "air superiority" },
} };

const ModeText *mode_text(const As2Value &p_mode) {
	if (p_mode.kind != As2Value::Kind::String) {
		return nullptr;
	}
	for (const ModeText &m : kModeTexts) {
		if (m.game_mode == p_mode.text) {
			return &m;
		}
	}
	return nullptr;
}

bool empty_or_undefined(const As2Value &p_v) {
	return !p_v.defined() || as2_loose_equals(p_v, As2Value::of(""));
}

class LoadingScreenClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		r_host.clip("upperText").set_auto_size(true);
		fetch_data(r_host);
		set_header(r_host);
		set_subheader(r_host);
		set_description(r_host);
		set_level_image(r_host);
		set_squad(r_host);
		set_server_name(r_host);
		set_server_description(r_host);
		hint_interval_ = r_host.set_interval(kHintIntervalSeconds);
		update_hint(r_host);
	}

	void interval(HudWidgetHost &r_host, uint32_t p_id) override {
		if (p_id == hint_interval_) {
			update_hint(r_host);
		}
	}

	void advance(HudWidgetHost &r_host, double) override {
		if (loader_.state() == As2ClipLoader::State::Loading && loader_.poll() == As2ClipLoader::State::Loaded) {
			resized_ = std::nullopt;
		}
		const SwfRect visible = r_host.screen().stage_rects().visible;
		if (resized_ != visible) {
			on_resize(r_host, visible);
		}
	}

	bool input_concept(HudWidgetHost &r_host, int32_t p_concept, bool p_pressed) override {
		if (!p_pressed || !show_join_squad()) {
			return false;
		}
		if (p_concept == int32_t(UiInputConcept::Activate)) {
			join_squad(r_host, true);
			return true;
		}
		if (p_concept == int32_t(UiInputConcept::Deactivate)) {
			join_squad(r_host, false);
			return true;
		}
		return false;
	}

	const As2ClipLoader &loader() const { return loader_; }

private:
	void fetch_data(HudWidgetHost &r_host) {
		game_type_ = r_host.root_get_data("gameType");
		game_mode_ = r_host.root_get_data("gameMode");
		level_image_ = r_host.root_get_data("levelImage");
		level_name_ = r_host.root_get_data("levelName");
		level_description_ = r_host.root_get_data("levelDescription");
		difficulty_ = r_host.root_get_data("playerDifficulty");
		platform_ = r_host.root_get_data("platform");
		join_squad_ = r_host.root_get_data("joinSquad");
		join_squad_has_showed_ = r_host.root_get_data("joinSquadHasShowed");
		server_name_ = r_host.root_get_data("serverName");
		server_description_ = r_host.root_get_data("serverDescription");
		server_description_ = r_host.root_get_data("serverDescription");
		enter_button_circle_ = r_host.root_get_data("isEnterButtonCircle");
	}

	bool singleplayer_or_coop() const {
		return as2_loose_equals(game_type_, As2Value::of(std::string(kLoadingScreenGameTypeSingleplayer))) ||
				as2_loose_equals(game_type_, As2Value::of(std::string(kLoadingScreenGameTypeCoop)));
	}

	void set_header(HudWidgetHost &r_host) const {
		const As2Clip header = r_host.clip("header");
		if (!singleplayer_or_coop()) {
			const ModeText *m = mode_text(game_mode_);
			header.set_text(m != nullptr ? r_host.translate(m->header) : std::string());
		} else {
			header.set_text(r_host.translate("ID_M_MISSION"));
		}
		if (empty_or_undefined(level_name_)) {
			header.set_text("");
		}
	}

	void set_subheader(HudWidgetHost &r_host) const {
		r_host.clip("subheader")
				.set_text(level_name_.defined() ? r_host.translate(level_name_.to_string()) : std::string());
	}

	void set_level_image(HudWidgetHost &r_host) {
		if (empty_or_undefined(level_image_)) {
			return;
		}
		loader_.load_clip(r_host.clip("bgHolder"), std::string(kSwfImageScheme) + level_image_.to_string());
	}

	void set_description(HudWidgetHost &r_host) const {
		const As2Clip upper = r_host.clip("upperText");
		if (singleplayer_or_coop()) {
			upper.set_text(
					level_description_.defined() ? r_host.translate(level_description_.to_string()) : std::string());
		} else {
			const ModeText *m = mode_text(game_mode_);
			upper.set_text(m != nullptr ? r_host.translate(m->description) : std::string());
		}
		const As2Clip top = r_host.clip("bg.bgTop");
		top.set_height(std::max(upper.y() + upper.height() + kDescriptionPadding, top.height()));
	}

	void update_hint(HudWidgetHost &r_host) const {
		const As2Value next = r_host.root_get_data("nextHint");
		const As2Clip hint = r_host.clip("hint");
		if (empty_or_undefined(next)) {
			hint.set_visible(false);
			return;
		}
		hint.set_text(r_host.translate(next.to_string()));
		hint.set_visible(true);
	}

	bool show_join_squad() const {
		if (join_squad_has_showed_.truthy() || as2_loose_equals(platform_, As2Value::of(std::string(kPlatformWin32))) ||
				singleplayer_or_coop()) {
			return false;
		}
		for (const std::string_view mode : { "rush", "conquest", "capturetheflag" }) {
			if (as2_loose_equals(game_mode_, As2Value::of(std::string(mode)))) {
				return true;
			}
		}
		return false;
	}

	void set_squad(HudWidgetHost &r_host) {
		const As2Clip join = r_host.clip("mcJoinSquad");
		if (show_join_squad()) {
			join.child("PS3Buttons").set_visible(as2_loose_equals(platform_, As2Value::of(std::string(kPlatformPs3))));
			join.child("xenonButtons")
					.set_visible(as2_loose_equals(platform_, As2Value::of(std::string(kPlatformXenon))));
			if (!enter_button_circle_.truthy()) {
				const As2Clip cross = join.child("PS3Buttons").child("cross");
				const As2Clip circle = join.child("PS3Buttons").child("circle");
				const float cross_y = cross.y();
				cross.set_y(circle.y());
				circle.set_y(cross_y);
			}
			join_squad(r_host, r_host.root_get_data("joinSquad").truthy());
		} else {
			join.set_visible(false);
		}
		r_host.root_set_data("joinSquadHasShowed", As2Value::of(true));
	}

	void join_squad(HudWidgetHost &r_host, bool p_join) const {
		r_host.root_set_data("joinSquad", As2Value::of(p_join));
		const As2Clip join = r_host.clip("mcJoinSquad");
		const As2Clip accept = join.child("joinSquadText");
		const As2Clip decline = join.child("declineSquadText");
		join.child("joinSquadHL").set_x(accept.x() + accept.text_width());
		join.child("joinSquadHL").set_visible(p_join);
		join.child("declineSquadHL").set_x(decline.x() + decline.text_width());
		join.child("declineSquadHL").set_visible(!p_join);
	}

	void set_server_name(HudWidgetHost &r_host) const {
		const As2Clip info = r_host.clip("rspServerInfo");
		if (empty_or_undefined(server_name_)) {
			info.set_visible(false);
			return;
		}
		info.set_visible(true);
		if (SimpleScrollTextWidget *name = controller_at<SimpleScrollTextWidget>(r_host, info.child("serverName"))) {
			name->set_text(server_name_.to_string());
		}
	}

	void set_server_description(HudWidgetHost &r_host) const {
		const As2Clip descr = r_host.clip("rspServerInfo.serverDescr");
		if (empty_or_undefined(server_description_)) {
			descr.set_visible(false);
			return;
		}
		descr.set_visible(true);
		if (SimpleTextBlockScrollWidget *text = controller_at<SimpleTextBlockScrollWidget>(r_host, descr)) {
			text->set_text(server_description_.to_string());
		}
	}

	void on_resize(HudWidgetHost &r_host, const SwfRect &p_visible) {
		resized_ = p_visible;
		const As2Clip holder = r_host.clip("bgHolder");
		holder.set_width(p_visible.width());
		holder.set_height(p_visible.height());
	}

	As2Value game_type_;
	As2Value game_mode_;
	As2Value level_image_;
	As2Value level_name_;
	As2Value level_description_;
	As2Value difficulty_;
	As2Value platform_;
	As2Value join_squad_;
	As2Value join_squad_has_showed_;
	As2Value server_name_;
	As2Value server_description_;
	As2Value enter_button_circle_;
	As2ClipLoader loader_;
	std::optional<SwfRect> resized_;
	uint32_t hint_interval_ = 0;
};

class LoadingMessageWidget final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override { debug_ = param_true(r_host, "DEBUG_ENABLED"); }

	void on_clip_load(HudWidgetHost &r_host) override {
		texts_ = { waiting_message_, debug_ ? std::string(kDebugLevelName) : level_name(r_host) };
		index_ = 0;
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		r_host.self().set_visible(true);
		if (const std::string *invert = p_init.property("InvertVisible")) {
			invert_visible_ = *invert == "true";
		}
		if (const std::string *waiting = p_init.property("WaitingMessage")) {
			waiting_message_ = *waiting;
			texts_ = { waiting_message_, debug_ ? std::string(kDebugLevelName) : level_name(r_host) };
		}
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "LevelName") {
			texts_ = { waiting_message_, p_value.defined() ? p_value.to_string() : std::string() };
			index_ = 0;
		} else if (p_key == "Finished") {
			play_out_after_loop_ = as2_equals_true(p_value);
		} else if (p_key == "Visible") {
			bool visible = true;
			if (p_value.kind == As2Value::Kind::String) {
				visible = to_lower_ascii(p_value.to_string()) != "false";
			} else if (p_value.defined()) {
				visible = p_value.truthy();
			}
			r_host.self().set_visible(invert_visible_ ? !visible : visible);
		}
	}

	void frame_script(HudWidgetHost &r_host, const HudFrameScript &p_script) override {
		if (p_script.path != r_host.path() + ".text") {
			return;
		}
		if (p_script.calls("setNextText")) {
			set_next_text(r_host);
		} else if (p_script.calls("playOutAnimAfterLoop") && !play_out_after_loop_) {
			r_host.clip("text").goto_and_play("loop");
		}
	}

	const std::vector<std::string> &texts() const { return texts_; }
	size_t index() const { return index_; }

private:
	static std::string level_name(HudWidgetHost &r_host) {
		const As2Value name = r_host.root_get_data("levelName");
		return name.defined() ? name.to_string() : std::string();
	}

	void set_next_text(HudWidgetHost &r_host) {
		if (texts_.empty()) {
			return;
		}
		const std::string shown = r_host.translate(texts_[index_]);
		r_host.clip("text.msg.txt").set_text(shown);
		r_host.clip("text.msg.txtBg").set_text(shown);
		index_ = (index_ + 1) % texts_.size();
	}

	std::vector<std::string> texts_;
	std::string waiting_message_ = std::string(kWaitingMessage);
	size_t index_ = 0;
	bool debug_ = false;
	bool invert_visible_ = false;
	bool play_out_after_loop_ = false;
};

}

std::string_view loading_screen_game_mode(std::string_view p_mode_id) {
	for (const ModeId &m : kModeIds) {
		if (p_mode_id.starts_with(m.prefix)) {
			return m.game_mode;
		}
	}
	return {};
}

std::string loading_screen_movie(std::string_view p_asset_path) {
	return to_lower_ascii(p_asset_path);
}

std::string loading_message_path(const HudScreen &p_screen) {
	for (const std::unique_ptr<HudWidgetState> &w : p_screen.widgets()) {
		if (!w->retired && w->class_name == kLoadingMessageClass) {
			return w->path;
		}
	}
	return {};
}

LoadingScreenSource loading_screen_source(const level::LevelInfo &p_info, std::string_view p_mode_id,
		std::vector<std::string> p_hints, std::string p_platform, uint64_t p_seed) {
	LoadingScreenSource s;
	s.game_type = std::string(p_info.coop ? kLoadingScreenGameTypeCoop
					: p_info.multiplayer  ? kLoadingScreenGameTypeMultiplayer
										  : kLoadingScreenGameTypeSingleplayer);
	s.game_mode = std::string(loading_screen_game_mode(p_mode_id));
	s.level_image = p_info.loading.image_path;
	s.level_name = p_info.name_sid;
	s.level_description = p_info.description;
	s.platform = std::move(p_platform);
	s.hints = std::move(p_hints);
	s.seed = p_seed;
	return s;
}

LoadingScreenData::LoadingScreenData(LoadingScreenSource p_source) : source_(std::move(p_source)) {
	order_.resize(source_.hints.size());
	for (size_t i = 0; i < order_.size(); ++i) {
		order_[i] = i;
	}
	uint64_t state = source_.seed;
	for (size_t i = order_.size(); i > 1; --i) {
		std::swap(order_[i - 1], order_[size_t(splitmix64_next(state) % i)]);
	}
}

std::string LoadingScreenData::next_hint() const {
	if (order_.empty()) {
		return {};
	}
	last_hint_ = source_.hints[order_[next_]];
	next_ = (next_ + 1) % order_.size();
	++served_;
	return last_hint_;
}

As2Value LoadingScreenData::get_data(std::string_view p_key) const {
	if (const auto it = written_.find(p_key); it != written_.end()) {
		return it->second;
	}
	if (p_key == "nextHint") {
		return As2Value::of(next_hint());
	}
	if (p_key == "gameType") {
		return As2Value::of(source_.game_type);
	}
	if (p_key == "gameMode") {
		return As2Value::of(source_.game_mode);
	}
	if (p_key == "levelImage") {
		return As2Value::of(source_.level_image);
	}
	if (p_key == "levelName") {
		return As2Value::of(source_.level_name);
	}
	if (p_key == "levelDescription") {
		return As2Value::of(source_.level_description);
	}
	if (p_key == "platform") {
		return As2Value::of(source_.platform);
	}
	if (p_key == "serverName" || p_key == "serverDescription") {
		return As2Value::of("");
	}
	if (p_key == "isEnterButtonCircle") {
		return As2Value::of(false);
	}
	return As2Value::undefined();
}

void LoadingScreenData::set_data(std::string_view p_key, const As2Value &p_value) {
	written_[std::string(p_key)] = p_value;
}

void register_hud_widgets_loading_screen(HudWidgetRegistry &r_registry) {
	r_registry.add("Screen.LoadingScreen", [] { return std::make_unique<LoadingScreenClass>(); });
	r_registry.add(kLoadingMessageClass, [] { return std::make_unique<LoadingMessageWidget>(); });
}

}
