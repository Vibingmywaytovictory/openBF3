#include "hud_feed_tdm_parts.h"

#include "bf3/host/kits.h"

namespace bf3::host::tdm_feed {

namespace ui = bf3::core::ui;
namespace sw = bf3::sim::weapons;

void feed_spawn(Frame &r_frame) {
	const HudFeedInput &in = r_frame.input;
	HudTdmFeedState &state = r_frame.state;
	ui::HudModel &model = r_frame.model;
	const bool match = r_frame.match;
	const sim::Tick now_tick = r_frame.now_tick;
	const bool dead = !in.alive || in.own_unspawned;
	ui::HudSpawnScreen &sp = model.spawn_screen;
	sp.visible = match && dead && !r_frame.card_up && !r_frame.card_pending && !r_frame.round_over;
	sp.at_join = !in.spawned_this_round;
	uint8_t kit = in.spawn_kit;
	if (kit >= kKitCount) {
		++state.bad_inputs;
		kit = 0;
	}
	sp.selected_kit = kit;
	const bool deploy_phase = r_frame.phase == net::MatchPhase::PreRound || r_frame.phase == net::MatchPhase::InRound;
	sp.can_deploy = match && dead && deploy_phase && now_tick >= in.own_respawn_tick;
	sp.seconds_to_deploy = sp.can_deploy ? 0.0f : seconds_until(now_tick, in.own_respawn_tick, state.bad_inputs);
	sp.revive_possible = match && dead && !sp.at_join && now_tick < in.own_revive_until;
	sp.seconds_revive_left = sp.revive_possible ? seconds_until(now_tick, in.own_revive_until, state.bad_inputs) : 0.0f;
	for (auto &items : sp.kit_items) {
		items.fill(0);
	}
	if (in.kits != nullptr) {
		for (uint8_t k = 0; k < kKitCount; ++k) {
			const Loadout &l = r_frame.loadouts[k];
			for (size_t i = 0; i < kHudSpawnSlots.size(); ++i) {
				const uint16_t def = l.defs[kHudSpawnSlots[i]];
				const HudStaticRow *r = def != sw::kNoWeapon ? r_frame.statics.row(def) : nullptr;
				sp.kit_items[k][i] = r != nullptr ? r->item_id : 0;
			}
		}
	}

	model.soldier.show_medic_revive = sp.revive_possible;
	model.soldier.time_until_revive = sp.seconds_revive_left;
	model.soldier.time_until_death = 0.0f;
	model.soldier.is_interactive_man_down = false;
	model.spawn_logic.time_to_respawn = sp.seconds_to_deploy;
	const sim::Tick d0 = state.own_death_tick;
	const sim::Tick d1 = state.own_death_respawn_tick;
	model.spawn_logic.start_time_to_respawn =
			d1 > d0 && d1 - d0 <= kMaxAheadTicks ? float(double(d1 - d0) / double(sim::kSimHz)) : 0.0f;
	model.spawn_points.can_spawn = sp.can_deploy;
	model.spawn_points.has_valid_spawn = match;
}

}
