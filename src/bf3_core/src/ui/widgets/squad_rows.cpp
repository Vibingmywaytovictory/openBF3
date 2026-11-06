#include "button.h"
#include "list.h"
#include "squad_data.h"

#include "bf3/core/ui/hud_squads.h"
#include "bf3/core/ui/hud_widgets.h"

namespace bf3::core::ui {

namespace {

constexpr float kPlusGap = 12.0f;
constexpr float kNoValueInset = 10.0f;
constexpr float kIconGap = 2.0f;
constexpr float kLeaderLabelInset = 30.0f;
constexpr int32_t kKitIconInnerFrame = 3;
constexpr int32_t kBoosterIconInnerFrame = 2;
constexpr int32_t kVoipOffFrame = 2;
constexpr uint32_t kWhite = 0xFFFFFFu;
constexpr uint32_t kHighlightedText = 0u;
constexpr uint32_t kSquadText = 8429618u;
constexpr uint32_t kFriendlyText = 7586815u;
constexpr std::string_view kLabel = "Label";
constexpr std::string_view kMembers = "Members";
constexpr std::string_view kSquadsPerRow = " / 4";
constexpr bool kSquadPrivacyOffered = false;

HudListLabel *row_label(HudWidgetHost &r_host, std::string_view p_name) {
	return controller_at<HudListLabel>(r_host, r_host.clip(p_name));
}

HudListLabel *attach_bold_label(HudWidgetHost &r_host, std::string_view p_dummy, std::string_view p_name) {
	const As2Clip dummy = r_host.clip(p_dummy);
	if (!dummy.exists()) {
		return nullptr;
	}
	const As2Clip label = attach_row_label(r_host, dummy, p_name);
	HudListLabel *l = controller_at<HudListLabel>(r_host, label);
	if (l == nullptr) {
		return nullptr;
	}
	if (const std::optional<float> size = dummy.text_size()) {
		l->set_text_size(*size);
	}
	l->set_interactive(true);
	l->set_type("bold");
	return l;
}

void goto_inner_icon(const As2Clip &p_icon, int32_t p_frame) {
	if (const As2Clip inner = p_icon.child("icon"); inner.exists()) {
		inner.goto_and_stop(p_frame);
	}
}

std::string dash_if_undefined(const As2Value &p_value) {
	const std::string s = p_value.to_string();
	return s == "undefined" ? std::string("-") : s;
}

void place_plus(HudWidgetHost &r_host, double p_width, std::string_view p_label, bool p_visible_only) {
	const As2Clip plus = r_host.clip("plus");
	if (!plus.exists() || (p_visible_only && !plus.visible())) {
		return;
	}
	const HudListLabel *l = row_label(r_host, p_label);
	const float text_width = l != nullptr ? l->text_width() : 0.0f;
	plus.set_x(std::min(float(p_width) - plus.width(), r_host.clip(p_label).x() + text_width + kPlusGap));
}

class SquadSpawnClass final : public RowBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		RowBaseClass::construct(r_host);
		attach_bold_label(r_host, "dummyTextField", kLabel);
	}

	void set_data(const As2Value &p_data) override {
		set_label(p_data.field("Label").to_string());
		goto_and_stop_number(host_->clip("rankIcon"), p_data.field("Rank").to_number() + 1.0);
		const As2Clip kit = host_->clip("kitIcon");
		goto_row_icon(kit, p_data.field("KitIcon"));
		goto_inner_icon(kit, kKitIconInnerFrame);
		const As2Clip booster = host_->clip("boosterIcon");
		goto_row_icon(booster, p_data.field("BoosterIcon"));
		goto_inner_icon(booster, kBoosterIconInnerFrame);
		set_mouse_enabled(true);
		RowBaseClass::set_data(p_data);
		goto_row_icon(host_->clip("voipIcon"), p_data.field("Voip"));
	}

	void set_width(double p_width) override {
		RowBaseClass::set_width(p_width);
		if (HudListLabel *l = row_label(*host_, kLabel)) {
			l->set_label_width(host_->clip("size_mc").width() - host_->clip(kLabel).x());
		}
		const As2Clip bg = host_->clip("bg");
		if (bg.exists()) {
			bg.set_width(float(as2_round(p_width - bg.x())));
		}
	}

	void reset(bool p_destroy) override {
		RowBaseClass::reset(p_destroy);
		if (p_destroy) {
			host_->remove_movie_clip(host_->clip(kLabel));
		}
	}

protected:
	void check_for_new_content() const override {
		RowBaseClass::check_for_new_content();
		place_plus(*host_, width(), kLabel, false);
	}

private:
	void set_label(std::string p_text) const {
		if (HudListLabel *l = row_label(*host_, kLabel)) {
			l->set_text(std::move(p_text));
		}
	}
};

class SquadListRowClass final : public RowBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		RowBaseClass::construct(r_host);
		for (const std::string_view name : { "Check", "privateIcon", "friendIcon" }) {
			r_host.clip(name).set_visible(false);
		}
		attach_bold_label(r_host, "dummyTextField", kLabel);
		attach_bold_label(r_host, "dummyValueField", kMembers);
		r_host.clip(kLabel).set_rgb(kWhite);
		r_host.clip(kMembers).set_rgb(kWhite);
		register_state_callback(r_host.path());
	}

	void set_data(const As2Value &p_data) override {
		set_mouse_enabled(true);
		if (as2_equals_true(p_data.field("useDirectAccess"))) {
			set_label(std::string());
			set_members(0.0);
			set_is_squad(As2Value::of(false));
			set_local_squad(As2Value::of(false));
			set_private(As2Value::of(false));
			set_friend(As2Value::of(false));
			update_text();
			setup_direct_access();
		} else {
			set_label(p_data.field("Label").to_string());
			set_members(p_data.field("Members").to_number());
			set_is_squad(p_data.field("IsSquad"));
			set_local_squad(p_data.field("LocalSquad"));
			set_private(p_data.field("IsPrivate"));
			set_friend(p_data.field("HasFriend"));
		}
		RowBaseClass::set_data(p_data);
	}

	void set_width(double p_width) override {
		const float value_inset = float(width()) - host_->clip(kMembers).x();
		RowBaseClass::set_width(p_width);
		const float w = float(p_width);
		const As2Clip label = host_->clip(kLabel);
		const As2Clip value = host_->clip(kMembers);
		if (HudListLabel *l = row_label(*host_, kLabel)) {
			l->set_label_width(w - label.x());
		}
		value.set_x(w - value_inset);
		if (HudListLabel *l = row_label(*host_, kMembers)) {
			l->set_label_width(w - value.x());
		}
		const As2Clip friend_icon = host_->clip("friendIcon");
		const As2Clip private_icon = host_->clip("privateIcon");
		friend_icon.set_x(value.x() - friend_icon.width() - kIconGap);
		private_icon.set_x(friend_icon.x() + (friend_icon.width() - private_icon.width()) / 2.0f);
	}

	As2Value label() const override {
		const HudListLabel *l = row_label(*host_, kLabel);
		return l != nullptr ? As2Value::of(l->text()) : As2Value::undefined();
	}

	void reset(bool p_destroy) override {
		RowBaseClass::reset(p_destroy);
		if (p_destroy) {
			host_->remove_movie_clip(host_->clip(kLabel));
			host_->remove_movie_clip(host_->clip(kMembers));
		}
	}

protected:
	void on_state(const std::string &) override { update_text(); }

	void check_for_new_content() const override {
		RowBaseClass::check_for_new_content();
		place_plus(*host_, width(), kLabel, true);
	}

	void member(std::string_view p_name, const As2Value &p_value) override {
		if (p_name == "members") {
			set_members(p_value.to_number());
		} else if (p_name == "isSquad") {
			set_is_squad(p_value);
		} else if (p_name == "isLocalSquad") {
			set_local_squad(p_value);
		} else if (p_name == "isPrivate") {
			set_private(p_value);
		} else if (p_name == "hasFriend") {
			set_friend(p_value);
		} else {
			RowBaseClass::member(p_name, p_value);
		}
	}

private:
	void set_label(std::string p_text) const {
		if (HudListLabel *l = row_label(*host_, kLabel)) {
			l->set_text(std::move(p_text));
		}
	}

	void set_members(double p_members) {
		members_ = p_members;
		const As2Clip label = host_->clip(kLabel);
		if (members_ < 0.0) {
			host_->clip(kMembers).set_visible(false);
			label.set_x(host_->self().x() + kNoValueInset);
		} else {
			host_->clip(kMembers).set_visible(true);
			label.set_x(host_->clip("dummyTextField").x());
		}
		update_text();
	}

	void set_is_squad(const As2Value &p_value) {
		is_squad_ = p_value.truthy();
		update_text();
	}

	void set_local_squad(const As2Value &p_value) {
		host_->clip("Check").set_visible(p_value.truthy());
		update_text();
	}

	void set_private(const As2Value &p_value) const { host_->clip("privateIcon").set_visible(p_value.truthy()); }

	void set_friend(const As2Value &p_value) const { host_->clip("friendIcon").set_visible(p_value.truthy()); }

	void update_text() const {
		if (HudListLabel *l = row_label(*host_, kMembers)) {
			l->set_text(As2Value::of(members_).to_string() + (is_squad_ ? std::string(kSquadsPerRow) : std::string()));
		}
		uint32_t colour = kWhite;
		if (highlighted()) {
			colour = kHighlightedText;
		} else if (members_ > 0.0) {
			colour = is_squad_ && host_->clip("Check").visible() ? kSquadText : kFriendlyText;
		}
		host_->clip(kLabel).set_rgb(colour);
		host_->clip(kMembers).set_rgb(colour);
	}

	void setup_direct_access() {
		const std::string &self = host_->path();
		register_direct_access_array({
				ListAccessItem{ "Label", "", UiDirectAccessOp::Text, host_->clip(kLabel).path() },
				ListAccessItem{ "Members", "members", UiDirectAccessOp::Member, self },
				ListAccessItem{ "IsSquad", "isSquad", UiDirectAccessOp::Member, self },
				ListAccessItem{ "LocalSquad", "isLocalSquad", UiDirectAccessOp::Member, self },
				ListAccessItem{ "IsPrivate", "isPrivate", UiDirectAccessOp::Member, self },
				ListAccessItem{ "HasFriend", "hasFriend", UiDirectAccessOp::Member, self },
		});
	}

	double members_ = 0.0;
	bool is_squad_ = false;
};

class SquadDetailedRowClass final : public RowBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		RowBaseClass::construct(r_host);
		swap_text_fields(r_host);
		register_state_callback(r_host.path());
		r_host.clip("voipIcon").goto_and_stop(kUiHudIconVoip);
		r_host.clip("squadLeaderIcon").goto_and_stop(kUiHudIconSquadLeader);
	}

	void set_data(const As2Value &p_data) override {
		if (as2_equals_true(p_data.field("useDirectAccess"))) {
			set_text(kLabel, "- ");
			set_text(kKills, "-");
			set_text(kDeaths, "-");
			set_text(kScore, "-");
			set_local_squad(As2Value::of(0));
			host_->clip("voipIcon").goto_and_stop(kVoipOffFrame);
			set_empty_row(As2Value::of(false));
			setup_direct_access();
		} else {
			set_text(kLabel, p_data.field("Label").to_string());
			set_text(kKills, dash_if_undefined(p_data.field("Kills")));
			set_text(kDeaths, dash_if_undefined(p_data.field("Deaths")));
			set_text(kScore, dash_if_undefined(p_data.field("Score")));
			set_local_squad(As2Value::of(p_data.field("LocalSquad").to_number()));
			goto_row_icon(host_->clip("kitIcon"), p_data.field("Kit"));
			goto_row_icon(host_->clip("voipIcon"), p_data.field("Voip"));
			set_booster(p_data.field("BoosterIcon"));
			set_squad_leader(As2Value::of(p_data.field("SquadLeader").truthy()));
			set_empty_row(As2Value::of(p_data.field("EmptyRow").truthy()));
		}
		set_mouse_enabled(true);
		RowBaseClass::set_data(p_data);
	}

	void set_width(double p_width) override {
		if (HudListLabel *l = row_label(*host_, kLabel)) {
			l->set_label_width(host_->clip("size_mc").width() - host_->clip(kLabel).x());
		}
		RowBaseClass::set_width(p_width);
	}

	As2Value label() const override {
		const HudListLabel *l = row_label(*host_, kLabel);
		return l != nullptr ? As2Value::of(l->text()) : As2Value::undefined();
	}

	void reset(bool p_destroy) override {
		RowBaseClass::reset(p_destroy);
		if (p_destroy) {
			for (const std::string_view name : { kLabel, kKills, kDeaths, kScore }) {
				host_->remove_movie_clip(host_->clip(name));
			}
		}
	}

	bool squad_leader() const { return squad_leader_; }
	bool in_local_squad() const { return in_local_squad_; }

protected:
	void on_state(const std::string &p_state) override {
		if (in_local_squad_) {
			host_->clip("bg").goto_and_stop(std::string_view(p_state + "Squad"));
		}
	}

	void check_for_new_content() const override {
		RowBaseClass::check_for_new_content();
		place_plus(*host_, width(), kLabel, false);
	}

	void member(std::string_view p_name, const As2Value &p_value) override {
		if (p_name == "booster") {
			set_booster(p_value);
		} else if (p_name == "isLocalSquad") {
			set_local_squad(p_value);
		} else if (p_name == "isSquadLeader") {
			set_squad_leader(p_value);
		} else if (p_name == "isEmptyRow") {
			set_empty_row(p_value);
		} else {
			RowBaseClass::member(p_name, p_value);
		}
	}

private:
	static constexpr std::string_view kKills = "KillsLabel";
	static constexpr std::string_view kDeaths = "DeathsLabel";
	static constexpr std::string_view kScore = "ScoreLabel";

	void swap_text_fields(HudWidgetHost &r_host) {
		constexpr std::array<std::string_view, 4> dummies{ "dummyTextField", "dummyKillsLabel", "dummyDeathsLabel",
			"dummyScoreLabel" };
		constexpr std::array<std::string_view, 4> names{ kLabel, kKills, kDeaths, kScore };
		for (size_t i = 0; i < dummies.size(); ++i) {
			const As2Clip dummy = r_host.clip(dummies[i]);
			if (i == 0) {
				label_start_x_ = dummy.x();
			}
			const float dummy_width = dummy.width();
			if (HudListLabel *l = attach_bold_label(r_host, dummies[i], names[i])) {
				l->set_label_width(dummy_width);
			}
		}
	}

	void set_text(std::string_view p_name, std::string p_text) const {
		if (HudListLabel *l = row_label(*host_, p_name)) {
			l->set_text(std::move(p_text));
		}
	}

	void set_booster(const As2Value &p_value) const {
		const As2Clip booster = host_->clip("BoosterIcon");
		goto_row_icon(booster, p_value);
		goto_inner_icon(booster, kBoosterIconInnerFrame);
	}

	void set_local_squad(const As2Value &p_value) {
		in_local_squad_ = as2_loose_equals(p_value, As2Value::of(1));
		state_changed();
	}

	void set_squad_leader(const As2Value &p_value) {
		squad_leader_ = p_value.truthy();
		const As2Clip label = host_->clip(kLabel);
		const As2Clip icon = host_->clip("squadLeaderIcon");
		if (!host_->clip(kKills).visible()) {
			label.set_x(host_->self().x() + kNoValueInset);
		} else if (squad_leader_) {
			label.set_x(label_start_x_ + kLeaderLabelInset);
			icon.set_visible(true);
		} else {
			label.set_x(label_start_x_);
			icon.set_visible(false);
		}
	}

	void set_empty_row(const As2Value &p_value) {
		const bool shown = !p_value.truthy();
		for (const std::string_view name : { kKills, kDeaths, kScore, std::string_view("squadLeaderIcon"),
					 std::string_view("BoosterIcon"), std::string_view("kitIcon"), std::string_view("voipIcon") }) {
			host_->clip(name).set_visible(shown);
		}
		set_squad_leader(As2Value::of(squad_leader_));
	}

	void setup_direct_access() {
		const std::string &self = host_->path();
		const auto at = [&](std::string_view p_name) { return host_->clip(p_name).path(); };
		register_direct_access_array({
				ListAccessItem{ "Label", "", UiDirectAccessOp::Text, at(kLabel) },
				ListAccessItem{ "Kills", "", UiDirectAccessOp::Text, at(kKills) },
				ListAccessItem{ "Deaths", "", UiDirectAccessOp::Text, at(kDeaths) },
				ListAccessItem{ "Score", "", UiDirectAccessOp::Text, at(kScore) },
				ListAccessItem{ "Kit", "", UiDirectAccessOp::GotoAndStop, at("kitIcon") },
				ListAccessItem{ "Voip", "", UiDirectAccessOp::GotoAndStop, at("voipIcon") },
				ListAccessItem{ "BoosterIcon", "booster", UiDirectAccessOp::Member, self },
				ListAccessItem{ "LocalSquad", "isLocalSquad", UiDirectAccessOp::Member, self },
				ListAccessItem{ "SquadLeader", "isSquadLeader", UiDirectAccessOp::Member, self },
				ListAccessItem{ "EmptyRow", "isEmptyRow", UiDirectAccessOp::Member, self },
		});
	}

	float label_start_x_ = 0.0f;
	bool in_local_squad_ = false;
	bool squad_leader_ = false;
};

std::string squad_label(const HudClientState &p_client, int32_t p_squad) {
	if (p_client.squad_names == nullptr || p_squad < 1 || size_t(p_squad) > p_client.squad_names->size()) {
		return std::string();
	}
	return (*p_client.squad_names)[size_t(p_squad - 1)];
}

int32_t own_squad(const HudModel &p_model) {
	return p_model.local_player < p_model.players.size() ? p_model.players[p_model.local_player].squad : 0;
}

As2Value available_squad_rows(const HudModel &p_model, const HudClientState &p_client) {
	As2Value rows = As2Value::array();
	const HudSquadList &list = p_model.squad.available_squads;
	const int32_t own = own_squad(p_model);
	for (size_t i = 0; i < std::min<size_t>(list.count, list.squads.size()); ++i) {
		const HudSquadEntry &e = list.squads[i];
		As2Value row = As2Value::object();
		row.set("Label", As2Value::of(squad_label(p_client, e.squad)));
		row.set("Members", As2Value::of(int32_t(e.members)));
		row.set("IsSquad", As2Value::of(true));
		row.set("LocalSquad", As2Value::of(e.squad == own));
		row.set("IsPrivate", As2Value::of(false));
		row.set("HasFriend", As2Value::of(false));
		row.set("Index", As2Value::of(int32_t(i)));
		row.set("id", As2Value::of(int32_t(e.squad)));
		rows.push(std::move(row));
	}
	return rows;
}

As2Value squad_member_row(const HudModel &p_model, uint8_t p_slot, int32_t p_local_squad, size_t p_row) {
	const HudPlayer &p = p_model.players[p_slot];
	As2Value row = As2Value::object();
	row.set("Label", As2Value::of(hud_player_name(p_model, p_slot)));
	row.set("Kills", As2Value::of(int32_t(p.kills)));
	row.set("Deaths", As2Value::of(int32_t(p.deaths)));
	row.set("Score", As2Value::of(p.score));
	row.set("LocalSquad", As2Value::of(p_local_squad));
	row.set("Kit", As2Value::of(int32_t(p.kit_icon)));
	row.set("Voip", As2Value::of(kVoipOffFrame));
	row.set("BoosterIcon", As2Value::of(int32_t(p.booster_icon)));
	row.set("SquadLeader", As2Value::of(p_model.squad_leaders[p_slot]));
	row.set("EmptyRow", As2Value::of(false));
	row.set("Index", As2Value::of(int32_t(p_row)));
	row.set("id", As2Value::of(int32_t(p_slot)));
	return row;
}

As2Value squad_member_rows(const HudModel &p_model, const HudClientState &p_client) {
	As2Value rows = As2Value::array();
	const int32_t squad = hud_selected_squad(p_model, p_client);
	if (squad == 0) {
		return rows;
	}
	const int32_t local = squad == own_squad(p_model) ? 1 : 0;
	size_t members = 0;
	for (size_t slot = 0; slot < std::min<size_t>(p_model.player_count, p_model.players.size()); ++slot) {
		const HudPlayer &p = p_model.players[slot];
		if (p.squad != squad || p.team == 0 || p.team != p_model.local_team) {
			continue;
		}
		rows.push(squad_member_row(p_model, uint8_t(slot), local, members));
		++members;
	}
	for (; members < kHudSquadSize; ++members) {
		As2Value row = As2Value::object();
		row.set("Label", As2Value::of(std::string()));
		row.set("EmptyRow", As2Value::of(true));
		row.set("LocalSquad", As2Value::of(local));
		rows.push(std::move(row));
	}
	return rows;
}

}

std::optional<As2Value> hud_squad_list_rows(
		const HudDataSource &p_query, const HudModel &p_model, const HudClientState &p_client) {
	if (p_query.category != kUiSquadCompPartition) {
		return std::nullopt;
	}
	if (p_query.key == kSquadAvailableSquadsKey) {
		return available_squad_rows(p_model, p_client);
	}
	if (p_query.key == kSquadMembersKey) {
		return squad_member_rows(p_model, p_client);
	}
	return std::nullopt;
}

std::optional<As2Value> hud_squad_button_item(
		const HudDataSource &p_source, const HudModel &p_model, const HudClientState &p_client) {
	if (p_source.category != kUiSquadCompPartition) {
		return std::nullopt;
	}
	const int32_t own = own_squad(p_model);
	const bool leader =
			p_model.local_player < p_model.squad_leaders.size() && p_model.squad_leaders[p_model.local_player];
	std::string_view label;
	bool visible = false;
	if (p_source.key == kSquadJoinButtonKey) {
		const int32_t selected = hud_selected_squad(p_model, p_client);
		const HudSquadList &list = p_model.squad.available_squads;
		bool open = false;
		for (size_t i = 0; i < std::min<size_t>(list.count, list.squads.size()); ++i) {
			open = open || (list.squads[i].squad == selected && list.squads[i].open);
		}
		label = kSquadJoinSid;
		visible = selected > 0 && selected != own && open;
	} else if (p_source.key == kSquadCreateButtonKey) {
		label = own != 0 ? kSquadLeaveSid : kSquadCreateSid;
		visible = p_model.local_team != 0;
	} else if (p_source.key == kSquadKickButtonKey) {
		label = kSquadKickSid;
		const uint8_t target = hud_selected_squad_player(p_model, p_client);
		visible = leader && target != kHudNoPlayer && target != p_model.local_player;
	} else {
		return std::nullopt;
	}
	As2Value item = As2Value::object();
	item.set("Label", As2Value::of(std::string(label)));
	item.set("Visible", As2Value::of(visible));
	return item;
}

std::optional<As2Value> hud_squad_source_value(
		const HudDataSource &p_source, const HudModel &p_model, const HudClientState &p_client) {
	if (p_source.category != kUiSquadCompPartition) {
		return std::nullopt;
	}
	if (p_source.key == kSquadHeaderNameKey) {
		return As2Value::of(squad_label(p_client, hud_selected_squad(p_model, p_client)));
	}
	if (p_source.key == kSquadPrivacyVisibleKey) {
		const int32_t own = own_squad(p_model);
		return As2Value::of(own != 0 && hud_selected_squad(p_model, p_client) == own);
	}
	if (p_source.key == kSquadPrivacyKey || p_source.key == kSquadPrivacyEnabledKey) {
		return As2Value::of(kSquadPrivacyOffered);
	}
	return std::nullopt;
}

std::optional<int32_t> hud_squad_list_selection(const HudDataSource &p_query, const HudModel &p_model) {
	if (p_query.category != kUiSquadCompPartition || p_query.key != kSquadSelectedSquadIndexKey) {
		return std::nullopt;
	}
	const HudSquadList &list = p_model.squad.available_squads;
	const int32_t own = own_squad(p_model);
	for (size_t i = 0; i < std::min<size_t>(list.count, list.squads.size()); ++i) {
		if (list.squads[i].squad == own) {
			return int32_t(i);
		}
	}
	return list.count > 0 ? std::optional<int32_t>(0) : std::nullopt;
}

void register_hud_widgets_squad_rows(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.List.Rows.SquadSpawn", [] { return std::make_unique<SquadSpawnClass>(); });
	r_registry.add("Widget.List.Rows.SquadListRow", [] { return std::make_unique<SquadListRowClass>(); });
	r_registry.add("Widget.List.Rows.SquadDetailedRow", [] { return std::make_unique<SquadDetailedRowClass>(); });
}

}
