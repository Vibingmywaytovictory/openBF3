#include "hud_feed_tdm_parts.h"

#include "bf3/core/ui/hud_messages.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/sim/weapons/sim_time.h"

#include <algorithm>
#include <cmath>

namespace bf3::host::tdm_feed {

namespace ui = bf3::core::ui;
namespace sw = bf3::sim::weapons;

namespace {

void push_kill(ui::HudKillFeed &r_feed, uint32_t &r_serial, const ui::HudKill &p_line) {
	++r_serial;
	if (r_serial == 0) {
		r_serial = 1;
	}
	ui::HudKill line = p_line;
	line.serial = r_serial;
	r_feed.lines[r_serial % r_feed.lines.size()] = line;
	r_feed.newest_serial = r_serial;
}

void push_score(ui::HudScoreFeed &r_feed, uint32_t &r_serial, std::string_view p_sid, int32_t p_points) {
	++r_serial;
	if (r_serial == 0) {
		r_serial = 1;
	}
	ui::HudScoreEvent &e = r_feed.events[r_serial % r_feed.events.size()];
	e.serial = r_serial;
	e.sid_hash = ui::text_hash(p_sid);
	e.points = p_points;
	r_feed.newest_serial = r_serial;
}

}

void feed_kill_card(Frame &r_frame, std::span<const PresentEvent> p_drained) {
	const HudFeedInput &in = r_frame.input;
	HudTdmFeedState &state = r_frame.state;
	ui::HudModel &model = r_frame.model;
	const uint8_t own_slot = r_frame.own_slot;
	const double now_s = r_frame.now_s;
	ui::HudMessages &msg = model.messages;
	ui::HudKillCard &card = model.kill_card;
	for (const PresentEvent &e : p_drained) {
		if (e.kind == PresentKind::Spawn) {
			if (own_slot != ui::kHudNoPlayer && e.slot == own_slot) {
				state.kill_card_until_s = 0;
			}
			continue;
		}
		if (e.kind != PresentKind::Death) {
			continue;
		}
		if (e.aux >= ui::kHudMaxPlayers) {
			++state.bad_inputs;
			continue;
		}
		const uint8_t victim = e.aux;
		uint8_t killer = e.slot;
		if (killer >= ui::kHudMaxPlayers) {
			++state.bad_inputs;
			killer = ui::kHudNoPlayer;
		}
		const bool suicide = killer == victim || killer == ui::kHudNoPlayer;
		const HudStaticRow *row = e.def != sw::kNoWeapon ? r_frame.statics.row(e.def) : nullptr;
		if (e.def != sw::kNoWeapon && row == nullptr) {
			++state.unknown_defs;
		}
		ui::HudKill line;
		line.killer = killer;
		line.victim = victim;
		line.item_id = row != nullptr ? row->item_id : 0;
		line.weapon_name_sid_hash = row != nullptr ? row->name_sid_hash : 0;
		line.headshot = (e.flags & present_flags::kHeadshot) != 0;
		line.suicide = suicide;
		const int8_t kt =
				killer != ui::kHudNoPlayer && r_frame.present[killer] ? model.players[killer].team : int8_t(0);
		const int8_t vt = r_frame.present[victim] ? model.players[victim].team : int8_t(0);
		line.team_kill = !suicide && kt != 0 && kt == vt;
		push_kill(msg.kill_message, state.kill_serial, line);
		++state.deaths_fed;
		if ((e.flags & present_flags::kOwnShooter) != 0 && !suicide) {
			push_kill(msg.local_kill_message, state.local_kill_serial, line);
			if (!line.team_kill) {
				push_score(msg.score_message, state.score_serial, ui::kHudScoreKillSid, in.score_kill);
				if (line.headshot) {
					push_score(msg.score_message, state.score_serial, ui::kHudScoreHeadshotSid, in.score_headshot);
				}
			}
		}
		if ((e.flags & present_flags::kOwnVictim) == 0) {
			continue;
		}

		++state.kill_card_serial;
		if (state.kill_card_serial == 0) {
			state.kill_card_serial = 1;
		}
		++state.own_deaths;
		state.kill_card_from_s = now_s + kKillCardDelaySeconds;
		state.kill_card_until_s = state.kill_card_from_s + kKillCardSeconds;
		state.own_death_respawn_tick = e.extra;
		state.own_death_tick =
				e.time > 0 ? sim::Tick(std::min<int64_t>(e.time / sw::kTimeUnitsPerTick, int64_t(UINT32_MAX))) : 0;
		card = ui::HudKillCard{};
		card.serial = state.kill_card_serial;
		card.killer = suicide ? ui::kHudNoPlayer : killer;
		card.killer_kit = !suicide && r_frame.present[killer] ? model.players[killer].kit : int8_t(-1);
		card.headshot = line.headshot;
		card.suicide = suicide;
		card.item_id = row != nullptr ? row->item_id : 0;
		card.weapon_name_sid_hash = row != nullptr ? row->name_sid_hash : 0;
		const float hp = std::isfinite(e.value) ? std::clamp(e.value, 0.0f, 100.0f) : 0.0f;
		if (!std::isfinite(e.value) || e.value < 0.0f || e.value > 100.0f) {
			++state.bad_inputs;
		}
		card.killer_health = suicide ? 0.0f : hp;
		const HudKillCardSource &src = in.kill_card_source;
		card.distance_m = !suicide && src.killer_seen && std::isfinite(src.distance_m) && src.distance_m >= 0.0f
				? src.distance_m
				: -1.0f;
		const bool known_killer = !suicide && r_frame.present[killer];
		if (known_killer) {
			card.killer_name = model.players[killer].name;
			card.killer_score = model.players[killer].score;
			card.killer_player_kills = round_stats_kills(state.stats, killer, victim);
			card.player_killer_kills = round_stats_kills(state.stats, victim, killer);
		}
		card.killer_weapon_name = card.weapon_name_sid_hash;
		card.killer_weapon_icon = card.item_id;
		card.is_suicide = suicide && killer != ui::kHudNoPlayer;
		card.is_bad_luck = killer == ui::kHudNoPlayer;
		card.killer_weapon_type = row != nullptr ? row->weapon_class : 0;
		if (const HudStaticItem *item = r_frame.statics.item(card.item_id)) {
			card.killer_weapon_image_path = item->image_path;
		}
		hud_text(card.killer_basic_dog_tag, kDefaultBasicDogtag);
		hud_text(card.killer_advanced_dog_tag, kDefaultAdvancedDogtag);
	}
	r_frame.card_pending = card.serial != 0 && now_s < state.kill_card_from_s && now_s < state.kill_card_until_s;
	r_frame.card_up = card.serial != 0 && !r_frame.card_pending && now_s < state.kill_card_until_s;
	card.visible = r_frame.card_up;
	card.seconds_left = r_frame.card_up ? float(state.kill_card_until_s - now_s) : 0.0f;
}

}
