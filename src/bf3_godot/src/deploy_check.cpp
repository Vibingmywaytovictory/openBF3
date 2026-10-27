#include "deploy_check.h"

#include "deploy_screens.h"
#include "native_screens.h"

#include "bf3/core/ui/hud_screen.h"

#include <algorithm>
#include <cstdio>

namespace bf3::godot_side {

namespace {

std::string seconds_text(double p_seconds) {
	char b[32];
	std::snprintf(b, sizeof(b), "%.2f s", p_seconds);
	return b;
}

std::string kit_text(uint8_t p_kit) {
	return "kit " + std::to_string(unsigned(p_kit));
}

}

bool DeployCheck::up(const Bf3UiScreens &p_screens, std::string_view p_partition) {
	const std::optional<UiScreenPhase> p = p_screens.phase(p_partition);
	return p.has_value() && *p != UiScreenPhase::Exiting;
}

bool DeployCheck::shown(const Bf3UiScreens &p_screens, std::string_view p_partition) {
	const std::optional<UiScreenPhase> p = p_screens.phase(p_partition);
	return p.has_value() && *p == UiScreenPhase::Shown;
}

void DeployCheck::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0u : 1u;
	lines_.push_back("[deploy] " + p_name + ": " + (p_pass ? "PASS" : "FAIL") + " (" + p_detail + ")");
}

void DeployCheck::enter(Phase p_phase, double p_now_s, const DeployCheckState &p_st) {
	phase_ = p_phase;
	phase_s_ = p_now_s;
	sub_ = 0;
	mark_ = p_st;
}

void DeployCheck::spawned(const std::string &p_row, std::string p_how, uint8_t p_kit, Phase p_next, double p_now_s) {
	spawn_row_ = p_row;
	spawn_primary_ = 0;
	spawn_how_ = std::move(p_how);
	spawn_kit_ = p_kit;
	after_ = p_next;
	spawn_shot_ = 0;
	enter(Phase::SpawnEnd, p_now_s, mark_);
}

void DeployCheck::tap(const DeployCheckIo &p_io, godot::Key p_key) {
	p_io.press(p_key, true);
	p_io.press(p_key, false);
}

void DeployCheck::shot(const DeployCheckIo &p_io, const std::string &p_name, double p_now_s) {
	if (shots_dir_.empty()) {
		return;
	}
	const std::string path = shots_dir_ + "/deploy_" + p_name + ".png";
	const bool ok = p_io.shot(path);
	lines_.push_back("[deploy] shot " + path + " at " + seconds_text(p_now_s - start_s_) + (ok ? "" : " FAILED"));
}

std::string DeployCheck::node_fact(Bf3UiScreens &r_screens, std::string_view p_partition, std::string_view p_path) {
	bf3::core::ui::HudScreen *s = r_screens.hud_screen(p_partition);
	const bf3::core::ui::SwfSceneNode *n = s != nullptr ? s->scene().find_node(p_path) : nullptr;
	if (n == nullptr) {
		return "missing";
	}
	return std::string(n->world_visible ? "shown" : "hidden") + " at frame " + std::to_string(n->frame);
}

bool DeployCheck::click_clip(
		Bf3UiScreens &r_screens, std::string_view p_partition, std::string_view p_path, const DeployCheckIo &p_io) {
	if (sub_ == 0) {
		const std::optional<godot::Vector2> at = r_screens.clip_centre(p_partition, p_path);
		if (!at) {
			return false;
		}
		click_at_ = *at;
		p_io.move(click_at_);
		sub_ = 1;
	} else if (sub_ == 1) {
		p_io.click(click_at_, true);
		sub_ = 2;
	} else if (sub_ == 2) {
		p_io.click(click_at_, false);
		sub_ = 3;
	}
	return sub_ == 3;
}

bool DeployCheck::frame(Bf3UiScreens &r_screens, double p_now_s, const DeployCheckIo &p_io) {
	if (done_) {
		return true;
	}
	if (start_s_ < 0.0) {
		start_s_ = p_now_s;
		phase_s_ = p_now_s;
		mark_ = p_io.state();
	}
	const std::vector<std::string> &log = r_screens.log();
	for (; log_seen_ < log.size(); ++log_seen_) {
		const std::string &l = log[log_seen_];
		const bool transition = l.starts_with("open ") || l.starts_with("close ") || l.starts_with("entered ") ||
				l.starts_with("exited ");
		if (transition && (l.find("spawn") != std::string::npos || l.find("killscreen") != std::string::npos)) {
			lines_.push_back("[deploy] " + seconds_text(p_now_s - start_s_) + " " + l);
		}
		if (phase_ == Phase::TabProbe && l.starts_with("open ")) {
			tab_opened_ += (tab_opened_.empty() ? "" : ", ") + l.substr(5);
		}
	}
	const DeployCheckState st = p_io.state();
	step(r_screens, p_now_s, p_io, st);
	if (!done_ && (p_now_s - phase_s_ > kDeployCheckStepSeconds || p_now_s - start_s_ > kDeployCheckSeconds)) {
		row("phase " + std::to_string(int(phase_)), false,
				"timed out after " + seconds_text(p_now_s - phase_s_) + ", alive " + std::to_string(st.alive) +
						", time to respawn " + seconds_text(st.time_to_respawn) + ", spawn actions " +
						std::to_string(st.spawn_actions));
		p_io.hold_scoreboard(false);
		done_ = true;
	}
	return done_;
}

void DeployCheck::step(
		Bf3UiScreens &r_screens, double p_now_s, const DeployCheckIo &p_io, const DeployCheckState &p_st) {
	const double in = p_now_s - phase_s_;
	switch (phase_) {
		case Phase::Join:
			if (!shown(r_screens, kSpawnScreenPcPartition) || !shown(r_screens, kSpawnButtonScreenPartition)) {
				phase_s_ = p_now_s;
				break;
			}
			if (in >= kDeployCheckSettleSeconds) {
				shot(p_io, "join", p_now_s);
				row("join.deploy_screen", !p_st.alive && p_st.time_to_respawn <= 0.0f,
						"spawnscreenpc and spawnbuttonscreen shown, time to respawn " +
								seconds_text(p_st.time_to_respawn));
				tap(p_io, godot::KEY_ENTER);
				enter(Phase::JoinDeploy, p_now_s, p_st);
			}
			break;
		case Phase::JoinDeploy:
			if (p_st.alive && p_st.own_spawns > mark_.own_spawns) {
				spawned("join.enter_deploys", "Enter on spawnscreenpc, spawned after " + seconds_text(in),
						kDeployCheckNoKit, Phase::Suicide, p_now_s);
			}
			break;
		case Phase::SpawnEnd:
			while (spawn_shot_ < kDeployCheckSpawnShots.size() && in >= kDeployCheckSpawnShots[spawn_shot_]) {
				char name[48];
				std::snprintf(name, sizeof(name), "spawn%u_%03d", unsigned(spawn_ends_ + 1),
						int(kDeployCheckSpawnShots[spawn_shot_] * 1000.0));
				shot(p_io, name, p_now_s);
				++spawn_shot_;
			}
			if (in >= kDeployCheckSpawnEndSeconds) {
				++spawn_ends_;
				row(spawn_row_,
						p_st.spawn_actions > mark_.spawn_actions &&
								(spawn_kit_ == kDeployCheckNoKit || p_st.own_kit == spawn_kit_) &&
								(spawn_primary_ == 0 || p_st.spawned_primary == spawn_primary_),
						spawn_how_ + ": " + std::to_string(p_st.spawn_actions - mark_.spawn_actions) +
								" flow Spawn action(s), spawned as " + kit_text(p_st.own_kit) + " with primary item " +
								std::to_string(p_st.spawned_primary));
				const bool hud = shown(r_screens, bf3::core::ui::kHudMpScreenPartition);
				const bool gone =
						!up(r_screens, kSpawnScreenPcPartition) && !up(r_screens, kSpawnButtonScreenPartition);
				row("spawn_end." + std::to_string(spawn_ends_),
						p_st.glitches > mark_.glitches && p_st.glitch_triggers > mark_.glitch_triggers &&
								p_st.dim_outs > mark_.dim_outs && hud && gone,
						"glitch actions " + std::to_string(p_st.glitches - mark_.glitches) + " (power " +
								std::to_string(p_st.glitch_power) + "), dim outs " +
								std::to_string(p_st.dim_outs - mark_.dim_outs) + ", hudmpscreen " +
								(hud ? "shown" : "not shown") + ", deploy screens " + (gone ? "gone" : "still up"));
				enter(after_, p_now_s, p_st);
			}
			break;
		case Phase::Suicide:
			if (sub_ == 0) {
				p_io.suicide();
				sub_ = 1;
			} else if (p_st.own_deaths > mark_.own_deaths && !p_st.alive) {
				enter(Phase::Counter, p_now_s, p_st);
			}
			break;
		case Phase::Counter:
			if (sub_ == 0 && shown(r_screens, bf3::core::ui::kKillScreenPartition) &&
					in >= kDeployCheckKillCardShotSeconds) {
				shot(p_io, spawn_ends_ < 2 ? "kill_card" : "kill_card" + std::to_string(spawn_ends_), p_now_s);
				sub_ = 1;
			}
			if (up(r_screens, kSpawnScreenPcPartition) && spawn_ends_ >= 3) {
				probe_step_ = 0;
				enter(Phase::TabProbe, p_now_s, p_st);
			} else if (up(r_screens, kSpawnScreenPcPartition)) {
				const bool second = spawn_ends_ >= 2;
				row(second ? "death2.counter_with_deploy" : "death.counter_with_deploy",
						up(r_screens, kSpawnTicketCounterPartition) &&
								!up(r_screens, bf3::core::ui::kKillScreenPartition),
						"the kill card closed and spawnscreenticketcountertdmscreen opened with spawnscreenpc, " +
								seconds_text(in) + " after the death");
				if (!second) {
					row("death.deploy_screen_counting",
							p_st.time_to_respawn > 0.0f && up(r_screens, kSpawnButtonScreenPartition) && !p_st.alive,
							"spawnscreenpc and spawnbuttonscreen opened with " + seconds_text(p_st.time_to_respawn) +
									" to respawn");
					enter(Phase::Counting, p_now_s, p_st);
					break;
				}
				tap(p_io, godot::KEY_ENTER);
				const DeployCheckState after = p_io.state();
				const bool fired = after.concepts_found > p_st.concepts_found;
				row("countdown.enter_while_counting", p_st.time_to_respawn > 0.0f && fired,
						"Enter on the frame spawnscreenpc opened, " + seconds_text(p_st.time_to_respawn) +
								" to respawn; spawngraphpc's Activate port " + (fired ? "fired" : "did not fire"));
				alive_while_counting_ = false;
				ready_s_ = -1.0;
				enter(Phase::EnterBlocked, p_now_s, after);
			}
			break;
		case Phase::Counting:
			if (in >= kDeployCheckCleanSeconds) {
				if (p_st.time_to_respawn > 0.0f) {
					shot(p_io, "counting", p_now_s);
				}
				lines_.push_back("[deploy] " + seconds_text(in) + " after spawnscreenpc opened, " +
						seconds_text(p_st.time_to_respawn) + " to respawn: lightbars " +
						node_fact(r_screens, kSpawnButtonScreenPartition, kDeployCheckLightBars) + ", deployStatus " +
						node_fact(r_screens, kSpawnButtonScreenPartition, kDeployCheckDeployStatus));
				p_io.kit_key(kDeployCheckKeyKit);
				enter(Phase::KitKey, p_now_s, p_st);
			}
			break;
		case Phase::KitKey:
			if (in >= kDeployCheckInputSeconds) {
				row("kit.key", p_st.spawn_kit == kDeployCheckKeyKit,
						"kit key " + std::to_string(unsigned(kDeployCheckKeyKit) + 1) + ": spawn " +
								kit_text(p_st.spawn_kit));
				enter(Phase::KitClick, p_now_s, p_st);
			}
			break;
		case Phase::KitClick:
			if (sub_ < 3) {
				click_clip(r_screens, kSpawnScreenPcPartition,
						std::string(kDeployCheckKitBar) + std::to_string(unsigned(kDeployCheckClickKit)), p_io);
			} else if (sub_++ > 3 && in >= kDeployCheckInputSeconds) {
				shot(p_io, "kit_click", p_now_s);
				row("kit.click", p_st.spawn_kit == kDeployCheckClickKit && p_st.kits_from_flow > mark_.kits_from_flow,
						"kitButton_" + std::to_string(unsigned(kDeployCheckClickKit)) + " click at " +
								std::to_string(int(click_at_.x)) + "," + std::to_string(int(click_at_.y)) + ": spawn " +
								kit_text(p_st.spawn_kit) + ", " +
								std::to_string(p_st.kits_from_flow - mark_.kits_from_flow) + " kit(s) from the flow");
				enter(Phase::GridOpen, p_now_s, p_st);
			}
			break;
		case Phase::GridOpen:
			if (sub_ < 3) {
				click_clip(r_screens, kSpawnScreenPcPartition, kDeployCheckPrimarySlot, p_io);
			} else if (in >= kDeployCheckGridSeconds) {
				shot(p_io, "grid", p_now_s);
				const bool listed = p_st.primary_count >= 2;
				row("grid.open", listed,
						"slotButton_0 click: " + std::to_string(p_st.primary_count) + " primaries, selected " +
								std::to_string(p_st.primary_selected));
				if (!listed) {
					enter(Phase::Done, p_now_s, p_st);
					break;
				}
				pick_index_ = (std::max(p_st.primary_selected, 0) + 1) % int32_t(p_st.primary_count);
				picked_ = p_st.primary_items[size_t(pick_index_)];
				enter(Phase::GridPick, p_now_s, p_st);
			}
			break;
		case Phase::GridPick:
			if (sub_ < 3) {
				click_clip(r_screens, kSpawnScreenPcPartition,
						std::string(kDeployCheckGridCell) + std::to_string(pick_index_) + "-s-0", p_io);
			} else if (in >= kDeployCheckGridSeconds) {
				shot(p_io, "grid_picked", p_now_s);
				row("grid.pick",
						p_st.picks_from_flow > mark_.picks_from_flow &&
								p_st.loadout_requests > mark_.loadout_requests && p_st.chosen_primary == picked_,
						"cell " + std::to_string(pick_index_) + " click: item " + std::to_string(picked_) + ", " +
								std::to_string(p_st.picks_from_flow - mark_.picks_from_flow) +
								" pick(s) from the flow, " +
								std::to_string(p_st.loadout_requests - mark_.loadout_requests) +
								" loadout request(s), kit bar primary " + std::to_string(p_st.chosen_primary));
				ready_s_ = -1.0;
				enter(Phase::Ready, p_now_s, p_st);
			}
			break;
		case Phase::TabProbe: {
			const DeployCheckTab &tab = kDeployCheckTabs[tab_];
			if (probe_step_ == 0) {
				if (p_st.time_to_respawn > 0.0f || in < kDeployCheckSettleSeconds) {
					break;
				}
				tab_opened_.clear();
				probe_step_ = 1;
			} else if (probe_step_ == 1) {
				if (click_clip(r_screens, kSpawnScreenPcPartition, tab.button, p_io)) {
					probe_s_ = p_now_s;
					probe_step_ = 2;
				}
			} else if (probe_step_ == 2 && p_now_s - probe_s_ >= kDeployCheckTabSeconds) {
				shot(p_io, std::string("tab_") + std::string(tab.name), p_now_s);
				const bool deploy_up = shown(r_screens, kSpawnButtonScreenPartition);
				row(std::string("tabs.") + std::string(tab.name) + ".way_out", deploy_up && !p_st.alive,
						std::string(tab.name) + " click opened " + (tab_opened_.empty() ? "nothing" : tab_opened_) +
								"; spawnscreenpc " + (up(r_screens, kSpawnScreenPcPartition) ? "up" : "closed") +
								", spawnbuttonscreen's DEPLOY " + (deploy_up ? "up" : "gone"));
				sub_ = 0;
				probe_step_ = 3;
			} else if (probe_step_ == 3) {
				if (click_clip(r_screens, kSpawnButtonScreenPartition, kDeployCheckDeployButton, p_io)) {
					probe_step_ = 4;
				}
			} else if (probe_step_ == 4 && p_st.alive && p_st.own_spawns > mark_.own_spawns) {
				const Phase next = tab_ + 1 < kDeployCheckTabs.size() ? Phase::Suicide : Phase::Done;
				spawned(std::string("tabs.") + std::string(tab.name) + ".deploys",
						"btnDeploy click after the " + std::string(tab.name) + " tab", kDeployCheckClickKit, next,
						p_now_s);
				spawn_primary_ = picked_;
				++tab_;
			}
			break;
		}
		case Phase::Ready:
			if (p_st.alive) {
				row("countdown.no_deploy", false, "spawned during the countdown");
				enter(Phase::Done, p_now_s, p_st);
				break;
			}
			if (p_st.time_to_respawn > 0.0f) {
				break;
			}
			if (ready_s_ < 0.0) {
				ready_s_ = p_now_s;
			}
			if (p_now_s - ready_s_ >= kDeployCheckSettleSeconds) {
				shot(p_io, "ready", p_now_s);
				p_io.hold_scoreboard(true);
				enter(Phase::TabHeld, p_now_s, p_st);
			}
			break;
		case Phase::TabHeld:
			if (sub_ == 0 && in >= kDeployCheckInputSeconds) {
				shot(p_io, "tab_held", p_now_s);
				tap(p_io, godot::KEY_ENTER);
				sub_ = 1;
			} else if (sub_ == 1 && in >= kDeployCheckInputSeconds + kDeployCheckRefuseSeconds) {
				row("tab.refuses_deploy", !p_st.alive && p_st.own_spawns == mark_.own_spawns,
						"Enter with Tab held: alive " + std::to_string(p_st.alive) + ", flow Spawn actions " +
								std::to_string(p_st.spawn_actions - mark_.spawn_actions) + "; OverviewMap_01 " +
								node_fact(r_screens, kSpawnScreenPcPartition, kDeployCheckOverviewMap));
				p_io.hold_scoreboard(false);
				sub_ = 2;
			} else if (sub_ == 2 && in >= 2.0 * kDeployCheckInputSeconds + kDeployCheckRefuseSeconds) {
				enter(Phase::ClickDeploy, p_now_s, p_st);
			}
			break;
		case Phase::ClickDeploy:
			if (sub_ < 3) {
				click_clip(r_screens, kSpawnButtonScreenPartition, kDeployCheckDeployButton, p_io);
			} else if (p_st.alive && p_st.own_spawns > mark_.own_spawns) {
				spawned("click.deploys",
						"btnDeploy click at " + std::to_string(int(click_at_.x)) + "," +
								std::to_string(int(click_at_.y)),
						kDeployCheckClickKit, Phase::Suicide, p_now_s);
				spawn_primary_ = picked_;
			}
			break;
		case Phase::EnterBlocked:
			alive_while_counting_ = alive_while_counting_ || p_st.alive;
			if (sub_ == 0 && in >= kDeployCheckCleanSeconds) {
				shot(p_io, "enter_refused", p_now_s);
				lines_.push_back("[deploy] " + seconds_text(in) + " after the refused Enter: OverviewMap_01 " +
						node_fact(r_screens, kSpawnScreenPcPartition, kDeployCheckOverviewMap) + ", " +
						seconds_text(p_st.time_to_respawn) + " to respawn");
				sub_ = 1;
			}
			if (p_st.time_to_respawn > 0.0f) {
				break;
			}
			if (ready_s_ < 0.0) {
				ready_s_ = p_now_s;
			}
			if (p_now_s - ready_s_ >= kDeployCheckSettleSeconds) {
				row("countdown.blocks_deploy", !alive_while_counting_ && !p_st.alive,
						"no spawn through the countdown and " + seconds_text(kDeployCheckSettleSeconds) + " after it");
				tap(p_io, godot::KEY_ENTER);
				enter(Phase::EnterDeploy, p_now_s, p_st);
			}
			break;
		case Phase::EnterDeploy:
			if (p_st.alive && p_st.own_spawns > mark_.own_spawns) {
				spawned("enter.deploys", "Enter at Time 0", kDeployCheckClickKit, Phase::Suicide, p_now_s);
				spawn_primary_ = picked_;
			}
			break;
		case Phase::Done:
			passed_ = failures_ == 0 && spawn_ends_ == 3 + kDeployCheckTabs.size();
			done_ = true;
			break;
	}
}

std::string DeployCheck::report() const {
	std::string out;
	for (const std::string &l : lines_) {
		out += l + "\n";
	}
	out += "[deploy] " + std::to_string(rows_) + " rows, " + std::to_string(failures_) + " failed\n";
	out += std::string("DEPLOY: ") + (exit_code() == 0 ? "PASS" : "FAIL");
	return out;
}

}
