#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class HudMessageOwner {
public:
	virtual ~HudMessageOwner() = default;
	virtual void move_row(const std::string &p_id) = 0;
	virtual void remove_row(const std::string &p_id) = 0;
	virtual void removed_row(const std::string &p_id) = 0;
};

class MessageBaseClass : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		r_host.self().goto_and_stop("start");
		message_height_ = r_host.clip("heightClip").height();
		r_host.self().set_alpha(0.0f);
	}

	void frame_script(HudWidgetHost &r_host, const HudFrameScript &p_fs) override {
		if (p_fs.path != r_host.path()) {
			return;
		}
		if (p_fs.calls("startAnimDone")) {
			start_anim_done();
		}
		if (p_fs.calls("outAnimationDone")) {
			out_animation_done();
		}
	}

	virtual void set_data(const As2Value &p_e) { data_ = p_e; }

	virtual void refresh(const As2Value &p_e) {
		data_ = p_e;
		if (p_e.field("RemoveFlag").to_number() == 1.0) {
			out_anim();
		}
	}

	virtual double get_height() const { return message_height_; }

	virtual void start_anim() {
		host_->self().set_alpha(100.0f);
		host_->self().goto_and_play("startAnim");
	}

	virtual void out_anim() {
		if (owner_ != nullptr) {
			owner_->remove_row(id());
		}
		host_->self().goto_and_play("outAnim");
	}

	void move(double p_y, bool p_instant) {
		double time = kMoveSeconds;
		if (p_instant) {
			time = 0.0;
			started_ = true;
		}
		host_->add_tween(host_->self(), HudTweenProperty::Y, float(p_y), time, HudTweenEase::ExpoOut);
	}

	void start_anim_done() { started_ = true; }

	void out_animation_done() {
		if (owner_ != nullptr) {
			owner_->removed_row(id());
		}
	}

	bool has_started() const { return started_; }
	void set_owner(HudMessageOwner *p_owner) { owner_ = p_owner; }
	std::string id() const { return as2_id(data_.field("Id")); }
	const As2Value &data() const { return data_; }

protected:
	static constexpr double kMoveSeconds = 0.4;
	HudWidgetHost *host_ = nullptr;
	HudMessageOwner *owner_ = nullptr;
	As2Value data_;
	double message_height_ = 0.0;
	bool started_ = false;
};

void set_both_texts(HudWidgetHost &r_host, const As2Clip &p_holder, std::string_view p_text) {
	const std::string shown = r_host.translate(p_text);
	p_holder.child("txt").set_text(shown);
	p_holder.child("txtBg").set_text(shown);
}

void fit_both_texts(const As2Clip &p_holder, float p_offset) {
	const float w = p_holder.child("txt").text_width() + p_offset;
	p_holder.child("txt").set_width(w);
	p_holder.child("txtBg").set_width(w);
}

class KillLogMessageClass final : public MessageBaseClass {
public:
	void set_data(const As2Value &p_e) override {
		MessageBaseClass::set_data(p_e);
		const As2Value &v = p_e.field("Value");
		set_color(v.field("KillerColor").to_number(), "killer");
		set_color(v.field("VictimColor").to_number(), "victim");
		set_message(v.field("Text").to_string());
		set_killer(v.field("KillerName").to_string(), v.field("KillerName").defined());
		set_victim(v.field("VictimName").to_string(), v.field("VictimName").defined());
		setup_icon(v.field("HeadShot").truthy());
		update_text_positions();
		start_anim();
	}

private:
	static constexpr float kTextOffset = 5.0f;

	As2Clip container() const { return host_->clip("container"); }

	void set_color(double p_color, std::string_view p_field) {
		const As2Clip c = container().child(p_field);
		if (p_color == double(kHudRelationFriendly)) {
			c.goto_and_stop("friendly");
		} else if (p_color == double(kHudRelationEnemy)) {
			c.goto_and_stop("enemy");
		} else if (p_color == double(kHudRelationSquad)) {
			c.goto_and_stop("squad");
		}
	}

	void set_message(const std::string &p_text) {
		As2Clip target = container().child("msg");
		if (p_text == kHudKillsIsNoSid) {
			target = container().child("victim");
			switch_message_ = true;
		}
		target.goto_and_stop("neutral");
		set_both_texts(*host_, target, p_text);
		fit_both_texts(target, kTextOffset);
	}

	void set_name(const As2Clip &p_field, const std::string &p_name, bool p_show) {
		p_field.set_visible(p_show);
		set_both_texts(*host_, p_field, p_name);
		fit_both_texts(p_field, kTextOffset);
		if (!p_show) {
			p_field.set_width(0.0f);
		}
	}

	void set_killer(const std::string &p_name, bool p_show) { set_name(container().child("killer"), p_name, p_show); }

	void set_victim(const std::string &p_name, bool p_show) { set_name(container().child(switch_message_ ? "msg" : "victim"), p_name, p_show); }

	void setup_icon(bool p_headshot) {
		const As2Clip icon = container().child("headShotIcon");
		if (p_headshot) {
			icon.goto_and_stop("KitRecon");
			icon.set_visible(true);
		} else {
			icon.set_visible(false);
		}
	}

	void update_text_positions() {
		const As2Clip c = container();
		const As2Clip killer = c.child("killer");
		const As2Clip msg = c.child("msg");
		const As2Clip icon = c.child("headShotIcon");
		const As2Clip victim = c.child("victim");
		msg.set_x(killer.x() + killer.width() + kTextOffset);
		icon.set_x(msg.x() + msg.width() + 10.0f);
		icon.set_y(17.0f);
		victim.set_x(icon.visible() ? icon.x() + 10.0f : msg.x() + msg.width() + kTextOffset);
		host_->self().set_x(-host_->self().width());
	}

	bool switch_message_ = false;
};

struct UiTint {
	uint32_t text = 0;
	uint32_t shadow = 0;
	uint32_t glow = 0;
};

constexpr std::array<std::string_view, 4> kUiColourSlotNameFrames{ "squad", "friendly", "", "enemy" };
constexpr float kTintShadowBlur = 8.0f;
constexpr float kTintShadowStrength = 1.7f;
constexpr double kTintShadowAngleDegrees = 90.0;
constexpr float kTintShadowDistance = 1.0f;
constexpr float kTintGlowBlur = 2.0f;
constexpr float kTintGlowStrength = 1.0f;

uint32_t rgb_of(SwfColor p_color) {
	return (uint32_t(p_color.r) << 16) | (uint32_t(p_color.g) << 8) | uint32_t(p_color.b);
}

SwfColor opaque(uint32_t p_rgb) {
	return SwfColor{ uint8_t((p_rgb >> 16) & 0xFFu), uint8_t((p_rgb >> 8) & 0xFFu), uint8_t(p_rgb & 0xFFu), 255 };
}

std::optional<UiTint> name_sprite_frame_tint(const SwfSceneNode &p_name_sprite, std::string_view p_label) {
	if (p_label.empty() || p_name_sprite.timeline == nullptr || !p_name_sprite.movie) {
		return std::nullopt;
	}
	const std::optional<uint32_t> frame = p_name_sprite.timeline->label_frame(p_label);
	if (!frame) {
		return std::nullopt;
	}
	std::optional<uint32_t> text, shadow, glow;
	for (const SwfDisplayEvent &e : p_name_sprite.timeline->events) {
		if (e.frame != *frame || !e.has_name) {
			continue;
		}
		const SwfFilterKind wanted = e.name == "txt" ? SwfFilterKind::DropShadow : SwfFilterKind::Glow;
		for (const SwfFilter &f : e.filters) {
			if (f.kind == wanted) {
				(e.name == "txt" ? shadow : glow) = rgb_of(f.color);
			}
		}
		if (e.name == "txt") {
			if (const SwfEditText *et = p_name_sprite.movie->find_edit_text(e.character)) {
				text = rgb_of(swf_edit_text_format(*p_name_sprite.movie, *et).color);
			}
		}
	}
	if (!text || !shadow || !glow) {
		return std::nullopt;
	}
	return UiTint{ *text, *shadow, *glow };
}

std::optional<UiTint> ui_colour_slot(const As2Clip &p_name_sprite, int32_t p_slot) {
	const SwfSceneNode *n = p_name_sprite.node();
	if (n == nullptr || p_slot < 0 || size_t(p_slot) >= kUiColourSlotNameFrames.size()) {
		return std::nullopt;
	}
	return name_sprite_frame_tint(*n, kUiColourSlotNameFrames[size_t(p_slot)]);
}

void tint_two_texts(const As2Clip &p_name_sprite, int32_t p_slot) {
	const std::optional<UiTint> t = ui_colour_slot(p_name_sprite, p_slot);
	if (!t) {
		return;
	}
	SwfFilter shadow;
	shadow.kind = SwfFilterKind::DropShadow;
	shadow.color = opaque(t->shadow);
	shadow.blur_x = shadow.blur_y = kTintShadowBlur;
	shadow.strength = kTintShadowStrength;
	shadow.angle = float(kTintShadowAngleDegrees * kDegToRad);
	shadow.distance = kTintShadowDistance;
	shadow.composite_source = true;
	shadow.passes = 1;
	SwfFilter glow;
	glow.kind = SwfFilterKind::Glow;
	glow.color = opaque(t->glow);
	glow.blur_x = glow.blur_y = kTintGlowBlur;
	glow.strength = kTintGlowStrength;
	glow.knockout = true;
	glow.composite_source = true;
	glow.passes = 1;
	const As2Clip txt = p_name_sprite.child("txt");
	txt.set_text_color(t->text);
	txt.set_filters({ shadow });
	p_name_sprite.child("txtBg").set_filters({ glow });
}

void tint_icon(const As2Clip &p_icon, const As2Clip &p_name_sprite, int32_t p_slot) {
	if (const std::optional<UiTint> t = ui_colour_slot(p_name_sprite, p_slot)) {
		p_icon.set_rgb(t->text);
	}
}

class LocalKillLogMessageClass final : public MessageBaseClass {
public:
	void set_data(const As2Value &p_e) override {
		MessageBaseClass::set_data(p_e);
		const As2Value &v = p_e.field("Value");
		const As2Clip c = container();
		const int32_t killer_slot = colour_slot(v.field("KillerColor"));
		tint_two_texts(c.child("killer"), killer_slot);
		tint_two_texts(c.child("victim"), colour_slot(v.field("VictimColor")));
		tint_icon(c.child("headShotIcon"), c.child("killer"), killer_slot);
		set_field("killer", v.field("KillerName"));
		set_field("victim", v.field("VictimName"));
		set_message(v.field("Text").to_string());
		setup_icon(v.field("HeadShot").truthy());
		update_text_positions();
		start_anim();
	}

private:
	static constexpr float kTextOffset = 5.0f;
	static constexpr float kKillerAloneX = 100.0f;
	static constexpr std::array<int32_t, 3> kColConvert{ 1, 3, 0 };

	As2Clip container() const { return host_->clip("container"); }

	static int32_t colour_slot(const As2Value &p_color) {
		const double c = p_color.to_number();
		return c == 0.0 || c == 1.0 || c == 2.0 ? kColConvert[size_t(c)] : -1;
	}

	void set_field(std::string_view p_field, const As2Value &p_name) {
		const As2Clip f = container().child(p_field);
		f.set_visible(p_name.defined());
		set_both_texts(*host_, f, p_name.to_string());
		fit_both_texts(f, kTextOffset);
	}

	void set_message(const std::string &p_text) {
		const As2Clip msg = container().child("msg");
		msg.goto_and_stop("neutral");
		set_both_texts(*host_, msg, p_text);
		fit_both_texts(msg, kTextOffset);
	}

	void setup_icon(bool p_headshot) {
		const As2Clip icon = container().child("headShotIcon");
		if (p_headshot) {
			icon.goto_and_stop("KitRecon");
			icon.set_visible(true);
		} else {
			icon.set_visible(false);
		}
	}

	void update_text_positions() {
		const As2Clip c = container();
		const As2Clip killer = c.child("killer");
		const As2Clip msg = c.child("msg");
		const As2Clip icon = c.child("headShotIcon");
		const As2Clip victim = c.child("victim");
		if (!victim.visible()) {
			killer.set_x(kKillerAloneX);
		}
		msg.set_x(killer.x() + killer.width() + kTextOffset);
		icon.set_x(msg.x() + msg.width() + 10.0f);
		icon.set_y(17.0f);
		victim.set_x(icon.visible() ? icon.x() + 10.0f : msg.x() + msg.width() + kTextOffset);
		host_->self().set_x(-(victim.x() + victim.width()) * 0.5f);
	}
};

class ScoreMessageClass final : public MessageBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		MessageBaseClass::construct(r_host);
		play_out_after_loop_ = false;
		r_host.self().set_x(kScoreX);
		r_host.self().stop();
	}

	void frame_script(HudWidgetHost &r_host, const HudFrameScript &p_fs) override {
		if (p_fs.path != r_host.path()) {
			return;
		}
		if (p_fs.calls("playOutAnimAfterLoop")) {
			if (!play_out_after_loop_) {
				r_host.self().goto_and_play("loop");
			}
			return;
		}
		if (p_fs.calls("outAnimComplete")) {
			if (owner_ != nullptr) {
				owner_->removed_row(id());
			}
			return;
		}
		MessageBaseClass::frame_script(r_host, p_fs);
	}

	void set_data(const As2Value &p_e) override {
		MessageBaseClass::set_data(p_e);
		const As2Value &v = p_e.field("Value");
		const std::array<std::string, 1> args{ v.field("Score").to_string() };
		const std::string text = hud_format_text(host_->translate(v.field("Text").to_string()), args);
		const As2Clip m = host_->clip("scoringMessage");
		m.child("txt").set_text(text);
		m.child("txtBg").set_text(text);
		start_anim();
	}

	void start_anim() override {
		host_->self().set_alpha(100.0f);
		host_->self().goto_and_play("start");
	}

	void refresh(const As2Value &p_e) override {
		MessageBaseClass::refresh(p_e);
		host_->clip("cursor_mc").set_visible(p_e.field("TotalMessages") == p_e.field("Index"));
	}

	void out_anim() override {
		play_out_after_loop_ = true;
		if (owner_ != nullptr) {
			owner_->remove_row(id());
			owner_->move_row(id());
		}
	}

private:
	static constexpr float kScoreX = 140.0f;
	bool play_out_after_loop_ = false;
};

class GameMessageClass final : public MessageBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		MessageBaseClass::construct(r_host);
		r_host.self().set_x(-r_host.self().width() / 2.0f);
		r_host.clip("txt.txtDisplay").set_auto_size(SwfTextAutoSize::Center);
	}

	void frame_script(HudWidgetHost &r_host, const HudFrameScript &p_fs) override {
		if (p_fs.path == r_host.path() + ".txt" && p_fs.calls("setSize")) {
			set_size();
			return;
		}
		if (p_fs.path == r_host.path() && p_fs.calls("outAnimComplete")) {
			out_anim_complete();
			return;
		}
		MessageBaseClass::frame_script(r_host, p_fs);
	}

	void set_data(const As2Value &p_e) override {
		MessageBaseClass::set_data(p_e);
		host_->clip("txt.txtDisplay").set_text(host_->translate(p_e.field("Value").field("Text").at(0).to_string()));
		start_anim();
	}

	void start_anim() override {
		host_->self().goto_and_play("in");
		host_->self().set_alpha(100.0f);
		host_->self().play();
	}

	void out_anim() override {
		host_->self().goto_and_play("out");
		if (owner_ != nullptr) {
			owner_->remove_row(id());
		}
	}

	double get_height() const override { return std::min(double(host_->clip("txt.txtDisplay").text_height()), kMaxHeight); }

private:
	static constexpr double kMaxHeight = 300.0;
	static constexpr double kWidthPad = 60.0;
	static constexpr int32_t kBackgroundShownFrame = 21;

	void set_size() {
		const As2Clip t = host_->clip("txt.txtDisplay");
		const double w = double(t.text_width()) + kWidthPad;
		const double h = std::min(double(t.text_height()), kMaxHeight);
		const As2Clip bg = host_->clip("txt.bg");
		if (HudBackgroundWidgetApi *api = controller_at<HudBackgroundWidgetApi>(*host_, bg)) {
			api->initialize_without_data();
			api->set_width(as2_round(w));
			api->set_height(as2_round(h) + 10.0);
		}
		bg.set_x(float(as2_round(-w / 2.0) + 5.0));
		bg.set_y(-5.0f);
		bg.goto_and_stop(kBackgroundShownFrame);
	}

	void out_anim_complete() {
		if (owner_ != nullptr) {
			owner_->removed_row(id());
		}
		if (host_->self().exists()) {
			host_->self().set_alpha(0.0f);
		}
	}
};

class HudMessageClass final : public HudWidgetController, public HudMessageApi, public HudMessageOwner {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override {
		if (p_node.message_query) {
			source_ = hud_message_source(p_node.message_query->key);
			on_initialize_ = p_node.message_query->on_initialize;
		}
		number_of_rows_ = p_node.number_of_rows;
	}

	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		inverted_list_ = param_true(r_host, "invertedList");
		invert_push_ = param_true(r_host, "invertPush");
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		ids_.clear();
		r_host.create_empty_movie_clip(r_host.self(), kHolder, r_host.self().next_highest_depth());
		r_host.clip("posGuide").set_visible(false);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "NumberOfRows") {
			max_row_count_ = p_value.to_number();
		} else if (p_key == "Message") {
			update_message_data(p_value);
		}
	}

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "NumberOfRows", As2Value::of(number_of_rows_) });
		if (p_client.messages != nullptr && source_ != HudMessageSource::None) {
			r_out.push_back({ "Message", p_client.messages->list(source_), on_initialize_ });
		}
	}

	void advance(HudWidgetHost &, double p_seconds) override {
		if (team_kill_.empty()) {
			return;
		}
		team_kill_left_ -= p_seconds;
		if (team_kill_left_ <= 0.0) {
			remove_team_kill_message();
		}
	}

	void move_row(const std::string &) override { refresh_pos(-1.0); }
	void remove_row(const std::string &) override {}

	void removed_row(const std::string &p_id) override {
		host_->remove_movie_clip(holder().child(kRowPrefix + p_id));
		ids_.erase(std::remove(ids_.begin(), ids_.end(), p_id), ids_.end());
		refresh_pos(0.0);
	}

	uint64_t rows_added() const override { return rows_added_; }
	size_t rows() const override { return ids_.size(); }
	double max_row_count() const override { return max_row_count_; }
	HudMessageSource source() const override { return source_; }

private:
	static constexpr std::string_view kHolder = "messageHolder";
	static constexpr std::string_view kTeamKillId = "99999";
	static constexpr std::string_view kTeamKillSid = "ID_H_ALERT_FAILED_FRIENDLY_FIRE";
	static constexpr double kTeamKillWaitSeconds = 5.0;
	static constexpr float kTeamKillY = -240.0f;
	inline static const std::string kRowPrefix = "message_";

	As2Clip holder() const { return host_->clip(kHolder); }

	MessageBaseClass *row(const std::string &p_id) const { return controller_at<MessageBaseClass>(*host_, holder().child(kRowPrefix + p_id)); }

	void update_message_data(const As2Value &p_list) {
		const size_t n = p_list.size();
		for (size_t i = 0; i < n; ++i) {
			As2Value e = p_list.at(i);
			const std::string id = as2_id(e.field("Id"));
			MessageBaseClass *mc = holder().child(kRowPrefix + id).exists() ? row(id) : nullptr;
			e.set("Index", As2Value::of(double(i)));
			e.set("TotalMessages", As2Value::of(double(n) - 1.0));
			if (!holder().child(kRowPrefix + id).exists()) {
				if (e.field("RemoveFlag").to_number() != 1.0) {
					add_row(e);
				}
			} else if (mc != nullptr) {
				mc->refresh(e);
			}
		}
		refresh_pos(0.0);
	}

	void add_row(const As2Value &p_e) {
		const std::string id = as2_id(p_e.field("Id"));
		const std::string type = p_e.field("Type").to_string();
		const As2Clip h = holder();
		const As2Clip mc = host_->attach_movie(h, type, kRowPrefix + id, h.next_highest_depth());
		if (type == "Subtitle") {
			host_->renderer(mc, "t1");
		}
		MessageBaseClass *r = controller_at<MessageBaseClass>(*host_, mc);
		if (r == nullptr) {
			return;
		}
		r->set_data(p_e);
		r->set_owner(this);
		ids_.push_back(id);
		rows_added_++;
		if (p_e.field("Value").field("TeamKill").truthy()) {
			send_team_kill_info();
		}
	}

	void refresh_pos(double p_offset) {
		const size_t n = ids_.size();
		MessageBaseClass *prev = nullptr;
		double y = 0.0;
		bool instant = false;
		if (inverted_list_) {
			for (size_t k = n; k-- > 0;) {
				MessageBaseClass *r = row(ids_[k]);
				if (r == nullptr) {
					continue;
				}
				instant = !r->has_started() && !(n > 1);
				if (invert_push_) {
					y = prev == nullptr ? r->get_height() * p_offset : y + r->get_height();
				} else {
					y = prev == nullptr ? -r->get_height() : y - r->get_height();
				}
				r->move(y, instant);
				prev = r;
			}
			return;
		}
		for (size_t k = 0; k < n; ++k) {
			MessageBaseClass *r = row(ids_[k]);
			if (r == nullptr) {
				continue;
			}
			if (!r->has_started()) {
				instant = true;
			}
			if (invert_push_) {
				y = prev == nullptr ? -r->get_height() : y - r->get_height();
			} else {
				y = prev == nullptr ? r->get_height() * p_offset : y + prev->get_height();
			}
			r->move(y, instant);
			prev = r;
		}
	}

	void send_team_kill_info() {
		if (!team_kill_.empty()) {
			return;
		}
		const As2Clip h = holder();
		const std::string name = kRowPrefix + std::string(kTeamKillId);
		if (h.child(name).exists()) {
			host_->remove_movie_clip(h.child(name));
		}
		const As2Clip mc = host_->attach_movie(h, "Game", name, h.next_highest_depth());
		MessageBaseClass *r = controller_at<MessageBaseClass>(*host_, mc);
		if (r == nullptr) {
			return;
		}
		As2Value e = As2Value::object();
		e.set("Type", As2Value::of("Game"));
		e.set("Id", As2Value::of(std::string(kTeamKillId)));
		As2Value v = As2Value::object();
		v.set("Text", As2Value::array({ As2Value::of(std::string(kTeamKillSid)) }));
		e.set("Value", std::move(v));
		r->set_data(e);
		mc.set_y(kTeamKillY);
		team_kill_ = name;
		team_kill_left_ = kTeamKillWaitSeconds;
	}

	void remove_team_kill_message() {
		if (MessageBaseClass *r = controller_at<MessageBaseClass>(*host_, holder().child(team_kill_))) {
			r->out_anim();
		}
		team_kill_.clear();
	}

	HudWidgetHost *host_ = nullptr;
	HudMessageSource source_ = HudMessageSource::None;
	bool on_initialize_ = true;
	int32_t number_of_rows_ = 0;
	double max_row_count_ = 1.0;
	bool inverted_list_ = false;
	bool invert_push_ = false;
	std::vector<std::string> ids_;
	uint64_t rows_added_ = 0;
	std::string team_kill_;
	double team_kill_left_ = 0.0;
};

}

void register_hud_widgets_hud_message(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Message.HudMessage", [] { return std::make_unique<HudMessageClass>(); });
	r_registry.add("Widget.Message.MessageTypes.KillLogMessage", [] { return std::make_unique<KillLogMessageClass>(); });
	r_registry.add("Widget.Message.MessageTypes.LocalKillLogMessage", [] { return std::make_unique<LocalKillLogMessageClass>(); });
	r_registry.add("Widget.Message.MessageTypes.ScoreMessage", [] { return std::make_unique<ScoreMessageClass>(); });
	r_registry.add("Widget.Message.MessageTypes.GameMessage", [] { return std::make_unique<GameMessageClass>(); });
}

}
