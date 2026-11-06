#include "squad_team_check.h"

#include "deploy_screens.h"
#include "level_session_menu.h"
#include "menu_check.h"
#include "native_screens.h"
#include "swf_view.h"

#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/ui_drawn_screens.h"
#include "bf3/core/util/format.h"

#include <algorithm>

namespace bf3::godot_side {

namespace {

namespace ui = bf3::core::ui;
namespace net = bf3::net;
using bf3::core::fmt;

constexpr std::string_view kSquadScreen = ui::kSquadScreenPartitions[0];
constexpr std::string_view kSquadIgmBackground = ui::kSquadScreenPartitions[2];
constexpr uint32_t kSquadCap = 4;

uint32_t team_count(std::span<const net::PlayerRow> p_players, uint8_t p_team) {
	return uint32_t(std::count_if(
			p_players.begin(), p_players.end(), [&](const net::PlayerRow &r) { return r.team == p_team; }));
}

uint32_t squad_count(std::span<const net::PlayerRow> p_players, uint8_t p_team, uint8_t p_squad) {
	return uint32_t(std::count_if(p_players.begin(), p_players.end(),
			[&](const net::PlayerRow &r) { return r.team == p_team && r.squad == p_squad; }));
}

bool shown(const Bf3UiScreens &p_screens, std::string_view p_partition) {
	const std::optional<UiScreenPhase> p = p_screens.phase(p_partition);
	return p.has_value() && *p == UiScreenPhase::Shown;
}

}

const net::PlayerRow *SquadTeamCheck::own(const SquadTeamCheckIo &p_io) const {
	for (const net::PlayerRow &r : p_io.players) {
		if (r.slot == p_io.own_slot) {
			return &r;
		}
	}
	return nullptr;
}

const net::PlayerRow *SquadTeamCheck::mate(const SquadTeamCheckIo &p_io) const {
	for (const net::PlayerRow &r : p_io.players) {
		if (!r.bot && r.slot != p_io.own_slot && net::player_name(r) == kSquadTeamPlayer2) {
			return &r;
		}
	}
	return nullptr;
}

std::string SquadTeamCheck::squad_name(uint8_t p_squad, const SquadTeamCheckIo &p_io) const {
	if (p_squad == 0 || p_io.names == nullptr || p_squad > p_io.names->size()) {
		return std::string();
	}
	return (*p_io.names)[p_squad - 1];
}

std::vector<SquadTeamCheck::ListRow> SquadTeamCheck::live_rows(
		Bf3UiScreens &r_screens, std::string_view p_class) const {
	std::vector<ListRow> out;
	ui::HudScreen *s = r_screens.hud_screen(kSquadScreen);
	if (s == nullptr) {
		return out;
	}
	for (const std::unique_ptr<ui::HudWidgetState> &w : s->widgets()) {
		if (w->retired || w->class_name != p_class) {
			continue;
		}
		const ui::SwfSceneNode *node = s->scene().find_node(w->path);
		if (node == nullptr || !node->world_visible) {
			continue;
		}
		ListRow r;
		r.path = w->path;
		r.label = s->clip(w->path + ".Label.txtField").text();
		r.members = s->clip(w->path + ".Members.txtField").text();
		r.local_tick = s->clip(w->path + ".Check").visible();
		out.push_back(std::move(r));
	}
	return out;
}

std::optional<godot::Vector2> SquadTeamCheck::row_at(
		Bf3UiScreens &r_screens, std::string_view p_class, const std::string &p_label) const {
	if (sub_ != 0) {
		return click_at_;
	}
	for (const ListRow &r : live_rows(r_screens, p_class)) {
		if (!p_label.empty() && r.label == p_label) {
			return r_screens.clip_centre(kSquadScreen, r.path);
		}
	}
	return std::nullopt;
}

bool SquadTeamCheck::visible(Bf3UiScreens &r_screens, std::string_view p_partition, std::string_view p_path) const {
	ui::HudScreen *s = r_screens.hud_screen(p_partition);
	const ui::SwfSceneNode *n = s != nullptr ? s->scene().find_node(p_path) : nullptr;
	return n != nullptr && n->world_visible;
}

bool SquadTeamCheck::click_at(std::optional<godot::Vector2> p_at, const SquadTeamCheckIo &p_io) {
	if (sub_ == 0) {
		if (!p_at) {
			return false;
		}
		click_at_ = *p_at;
		p_io.move(click_at_);
		sub_ = 1;
	} else if (sub_ == 1) {
		p_io.click(click_at_, true);
		sub_ = 2;
	} else if (sub_ == 2) {
		p_io.click(click_at_, false);
		sub_ = 3;
	}
	return sub_ >= 3;
}

bool SquadTeamCheck::click(
		Bf3UiScreens &r_screens, std::string_view p_partition, std::string_view p_path, const SquadTeamCheckIo &p_io) {
	return click_at(
			sub_ == 0 ? r_screens.clip_centre(p_partition, p_path) : std::optional<godot::Vector2>(click_at_), p_io);
}

void SquadTeamCheck::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0u : 1u;
	lines_.push_back("[squad-team] " + p_name + ": " + (p_pass ? "PASS" : "FAIL") + " (" + p_detail + ")");
}

void SquadTeamCheck::enter(Phase p_phase, double p_now_s) {
	phase_ = p_phase;
	phase_s_ = p_now_s;
	sub_ = 0;
	agreed_ = false;
}

void SquadTeamCheck::shot(const SquadTeamCheckIo &p_io, const std::string &p_name) {
	if (shots_dir_.empty() || !p_io.shot) {
		return;
	}
	const std::string path = shots_dir_ + "/squad_team_" + p_name + ".png";
	lines_.push_back("[squad-team] shot " + path + (p_io.shot(path) ? "" : " FAILED"));
}

std::string SquadTeamCheck::state_text(const SquadTeamCheckIo &p_io) const {
	std::string out;
	for (const net::PlayerRow &r : p_io.players) {
		out += fmt("%s%s slot %u team %u squad %u%s", out.empty() ? "" : "; ", std::string(net::player_name(r)).c_str(),
				unsigned(r.slot), unsigned(r.team), unsigned(r.squad), r.leader ? " leader" : "");
	}
	return out.empty() ? std::string("no players") : out;
}

void SquadTeamCheck::agree(Bf3UiScreens &r_screens, const std::string &p_name, const SquadTeamCheckIo &p_io) {
	const net::PlayerRow *me = own(p_io);
	const std::vector<ListRow> rows = live_rows(r_screens, kSquadTeamSquadRowClass);
	if (me == nullptr || !shown(r_screens, kSquadScreen)) {
		row(p_name, false, me == nullptr ? "no own PlayerList row" : "squadscreen is not shown");
		return;
	}
	bool ok = true;
	std::string fact;
	std::vector<uint8_t> listed;
	for (const ListRow &r : rows) {
		uint8_t squad = 0;
		for (uint8_t k = 1; p_io.names != nullptr && k <= p_io.names->size(); ++k) {
			if ((*p_io.names)[k - 1] == r.label) {
				squad = k;
				break;
			}
		}
		const uint32_t n = squad != 0 ? squad_count(p_io.players, me->team, squad) : 0;
		const std::string want = fmt("%u / %u", n, kSquadCap);
		const bool tick = squad != 0 && squad == me->squad;
		const bool same = squad != 0 && r.members == want && r.local_tick == tick;
		ok = ok && same;
		listed.push_back(squad);
		fact += fmt("%s'%s' '%s'%s (PlayerList squad %u: %s%s)", fact.empty() ? "" : ", ", r.label.c_str(),
				r.members.c_str(), r.local_tick ? " ticked" : "", unsigned(squad), want.c_str(), tick ? " ticked" : "");
	}
	for (const net::PlayerRow &p : p_io.players) {
		if (p.team == me->team && p.squad != 0 && std::find(listed.begin(), listed.end(), p.squad) == listed.end()) {
			ok = false;
			fact += fmt(
					"; PlayerList squad %u (%s) is not listed", unsigned(p.squad), squad_name(p.squad, p_io).c_str());
		}
	}
	row(p_name, ok && !rows.empty(),
			fmt("own team %u squad %u; squadscreen List_04: ", unsigned(me->team), unsigned(me->squad)) +
					(rows.empty() ? std::string("no rows") : fact));
}

bool SquadTeamCheck::frame(Bf3UiScreens &r_screens, double p_now_s, const SquadTeamCheckIo &p_io) {
	if (done_) {
		return true;
	}
	if (start_s_ < 0.0) {
		start_s_ = p_now_s;
		phase_s_ = p_now_s;
	}
	const double in = p_now_s - phase_s_;
	const net::PlayerRow *me = own(p_io);
	const net::PlayerRow *p2 = mate(p_io);
	if (phase_ != Phase::Join && phase_ != Phase::Done && (me == nullptr || p2 == nullptr)) {
		row("phase " + std::to_string(int(phase_)), false, "the own player or player2 left the PlayerList");
		enter(Phase::Done, p_now_s);
	}
	switch (phase_) {
		case Phase::Join:
			if (!extra_joined_) {
				std::string error;
				extra_joined_ = p_io.join_extra(&error);
				if (!extra_joined_) {
					row("player2.joins", false, error);
					enter(Phase::Done, p_now_s);
				}
				break;
			}
			if (me != nullptr && p2 != nullptr && me->team != 0 && p2->team != 0 &&
					shown(r_screens, kSpawnScreenPcPartition)) {
				row("player2.joins", true, state_text(p_io));
				enter(Phase::Teams, p_now_s);
			}
			break;
		case Phase::Teams: {
			if (me->team == p2->team) {
				own_team_ = me->team;
				enter(Phase::Menu, p_now_s);
				break;
			}
			if (sub_ != 0) {
				break;
			}
			const uint32_t mine = team_count(p_io.players, me->team), theirs = team_count(p_io.players, p2->team);
			net::SquadRequest q;
			q.op = net::SquadOp::SwitchTeam;
			if (mine + 1 <= theirs) {
				p_io.extra_request(q);
				lines_.push_back(fmt("[squad-team] player2 switches to the own team (%u against %u)", mine, theirs));
				sub_ = 1;
			} else if (theirs + 1 <= mine) {
				p_io.own_request(q);
				lines_.push_back(fmt("[squad-team] the own player switches to player2's team (%u against %u) by a "
									 "direct request, before the screens",
						mine, theirs));
				sub_ = 1;
			} else {
				row("teams.arranged", false,
						fmt("own team %u players, player2's %u: neither may switch by the balance rule; %s", mine,
								theirs, state_text(p_io).c_str()));
				enter(Phase::Done, p_now_s);
			}
			break;
		}
		case Phase::Menu:
			if (sub_ == 0 && in >= kSquadTeamSettleSeconds) {
				p_io.press(godot::KEY_ESCAPE, true);
				p_io.press(godot::KEY_ESCAPE, false);
				sub_ = 1;
			} else if (sub_ == 1 && shown(r_screens, kIngameMenuPartition) && in >= 2.0 * kSquadTeamSettleSeconds) {
				const std::string label =
						menu_row_label(r_screens.find_screen(kIngameMenuPartition), kSquadTeamMenuRow);
				row("menu.squad_team_enabled", label != "inactive" && label != "no row",
						"Escape on the deploy screen opened ingamemenump; SQUAD & TEAM row reads '" + label + "'");
				shot(p_io, "menu");
				enter(Phase::OpenSquads, p_now_s);
			}
			break;
		case Phase::OpenSquads:
			if (sub_ < 3) {
				const UiScreenSlot slot = r_screens.find_screen(kIngameMenuPartition);
				const std::optional<ui::SwfPoint> at = menu_row_stage_point(slot, kSquadTeamMenuRow, true);
				click_at(at && slot.view != nullptr ? std::optional<godot::Vector2>(slot.view->stage_to_canvas(*at))
													: std::nullopt,
						p_io);
			} else if (shown(r_screens, kSquadScreen) && shown(r_screens, kSquadIgmBackground)) {
				row("menu.opens_squadscreen", true,
						"a click on SQUAD & TEAM drew squadscreen over squadigmbackgroundscreen");
				enter(Phase::Initial, p_now_s);
			}
			break;
		case Phase::Initial:
			if (in >= kSquadTeamSettleSeconds) {
				shot(p_io, "initial");
				agree(r_screens, "list.initial", p_io);
				initial_squad_ = me->squad;
				enter(Phase::Create, p_now_s);
			}
			break;
		case Phase::Create:
			if (sub_ < 3) {
				click(r_screens, kSquadScreen, kSquadTeamCreateButton, p_io);
			} else if (me->squad != 0 && me->squad != initial_squad_ && me->leader &&
					squad_count(p_io.players, me->team, me->squad) == 1) {
				created_ = me->squad;
				const std::string left = left_first_
						? fmt(", after LEAVE SQUAD on the same button left squad %u", unsigned(initial_squad_))
						: std::string();
				row("create", true,
						fmt("CREATE SQUAD made squad %u (%s) with the own player its leader%s", unsigned(created_),
								squad_name(created_, p_io).c_str(), left.c_str()));
				enter(Phase::MateJoins, p_now_s);
			} else if (initial_squad_ != 0 && me->squad == 0 && !left_first_) {
				left_first_ = true;
				enter(Phase::Create, p_now_s);
			}
			break;
		case Phase::MateJoins:
			if (sub_ == 0 && in >= kSquadTeamSettleSeconds) {
				agree(r_screens, "list.after_create", p_io);
				net::SquadRequest q;
				q.op = net::SquadOp::Join;
				q.squad = created_;
				p_io.extra_request(q);
				sub_ = 1;
			} else if (sub_ == 1 && p2->squad == created_) {
				enter(Phase::SelectOwnSquad, p_now_s);
			}
			break;
		case Phase::SelectOwnSquad:
			if (in < kSquadTeamSettleSeconds) {
				break;
			}
			if (sub_ < 3) {
				click_at(row_at(r_screens, kSquadTeamSquadRowClass, squad_name(created_, p_io)), p_io);
			} else {
				enter(Phase::SelectMate, p_now_s);
			}
			break;
		case Phase::SelectMate:
			if (in < kSquadTeamSettleSeconds) {
				break;
			}
			if (sub_ < 3) {
				click_at(row_at(r_screens, kSquadTeamMemberRowClass, std::string(kSquadTeamPlayer2)), p_io);
			} else if (visible(r_screens, kSquadScreen, kSquadTeamKickButton)) {
				shot(p_io, "kick_offered");
				enter(Phase::Kick, p_now_s);
			} else if (in >= kSquadTeamSettleSeconds + kSquadTeamOfferSeconds) {
				shot(p_io, "kick_not_offered");
				row("kick", false,
						fmt("no KICK %.0f s after a click on player2's member row; UISquadComp SelectedSquad '%s', "
							"SelectedPlayer '%s'",
								kSquadTeamOfferSeconds, p_io.squad_data(ui::kSquadSelectedSquadKey).c_str(),
								p_io.squad_data(ui::kSquadSelectedPlayerKey).c_str()));
				enter(Phase::MateCreates, p_now_s);
			}
			break;
		case Phase::Kick:
			if (sub_ < 3) {
				click(r_screens, kSquadScreen, kSquadTeamKickButton, p_io);
			} else if (p2->squad != created_) {
				row("kick", me->squad == created_ && me->leader,
						fmt("KICK took player2 out of squad %u (now in %u); the own player %s", unsigned(created_),
								unsigned(p2->squad), me->leader ? "still leads it" : "lost the lead"));
				enter(Phase::MateCreates, p_now_s);
			}
			break;
		case Phase::MateCreates:
			if (sub_ == 0 && in >= kSquadTeamSettleSeconds) {
				agree(r_screens, "list.after_kick", p_io);
				net::SquadRequest q;
				q.op = net::SquadOp::Create;
				p_io.extra_request(q);
				sub_ = 1;
			} else if (sub_ == 1 && p2->squad != 0 && p2->squad != created_ && p2->leader) {
				mate_squad_ = p2->squad;
				enter(Phase::SelectSquad, p_now_s);
			}
			break;
		case Phase::SelectSquad:
			if (in < kSquadTeamSettleSeconds) {
				break;
			}
			if (sub_ < 3) {
				click_at(row_at(r_screens, kSquadTeamSquadRowClass, squad_name(mate_squad_, p_io)), p_io);
			} else if (in >= kSquadTeamSettleSeconds + kSquadTeamInputSeconds &&
					visible(r_screens, kSquadScreen, kSquadTeamJoinButton)) {
				shot(p_io, "join_offered");
				enter(Phase::JoinSquad, p_now_s);
			}
			break;
		case Phase::JoinSquad:
			if (sub_ < 3) {
				click(r_screens, kSquadScreen, kSquadTeamJoinButton, p_io);
			} else if (me->squad == mate_squad_) {
				row("join", !me->leader,
						fmt("JOIN on player2's squad %u (%s) moved the own player from %u", unsigned(mate_squad_),
								squad_name(mate_squad_, p_io).c_str(), unsigned(created_)));
				enter(Phase::Leave, p_now_s);
			}
			break;
		case Phase::Leave:
			if (sub_ == 0 && in < kSquadTeamSettleSeconds) {
				break;
			}
			if (!agreed_) {
				agreed_ = true;
				agree(r_screens, "list.after_join", p_io);
			}
			if (sub_ < 3) {
				click(r_screens, kSquadScreen, kSquadTeamCreateButton, p_io);
			} else if (me->squad != mate_squad_) {
				row("leave", p2->squad == mate_squad_,
						fmt("LEAVE SQUAD took the own player out of squad %u (now in %u); player2 stays in %u",
								unsigned(mate_squad_), unsigned(me->squad), unsigned(p2->squad)));
				enter(Phase::Switch, p_now_s);
			}
			break;
		case Phase::Switch:
			if (sub_ == 0 && in < kSquadTeamSettleSeconds) {
				break;
			}
			if (!agreed_) {
				agreed_ = true;
				agree(r_screens, "list.after_leave", p_io);
				const uint32_t mine = team_count(p_io.players, me->team);
				const uint32_t theirs = team_count(p_io.players, uint8_t(3u - me->team));
				if (theirs + 1 > mine) {
					row("switch_team", false,
							fmt("the own team has %u players and the other %u, so the balance rule refuses a switch; "
								"%s",
									mine, theirs, state_text(p_io).c_str()));
					enter(Phase::Done, p_now_s);
					break;
				}
				own_team_ = me->team;
			}
			if (sub_ < 3) {
				click(r_screens, kSquadScreen, kSquadTeamSwitchButton, p_io);
			} else if (shown(r_screens, kPopupGenericPartition)) {
				shot(p_io, "switch_popup");
				enter(Phase::SwitchConfirm, p_now_s);
			}
			break;
		case Phase::SwitchConfirm:
			if (in < kSquadTeamInputSeconds) {
				break;
			}
			if (sub_ < 3) {
				click(r_screens, kPopupGenericPartition, kSquadTeamPopupOk, p_io);
			} else if (me->team != own_team_ && me->team != 0) {
				row("switch_team", true,
						fmt("SWITCH TEAM and the popup's OK moved the own player from team %u to %u",
								unsigned(own_team_), unsigned(me->team)));
				enter(Phase::Switched, p_now_s);
			}
			break;
		case Phase::Switched:
			if (in >= 2.0 * kSquadTeamSettleSeconds) {
				shot(p_io, "switched");
				agree(r_screens, "list.after_switch", p_io);
				passed_ = true;
				enter(Phase::Done, p_now_s);
			}
			break;
		case Phase::Done:
			done_ = true;
			break;
	}
	if (!done_ && phase_ != Phase::Done &&
			(p_now_s - phase_s_ > kSquadTeamStepSeconds || p_now_s - start_s_ > kSquadTeamCheckSeconds)) {
		shot(p_io, "timeout");
		row("phase " + std::to_string(int(phase_)), false,
				fmt("timed out after %.1f s at input step %d; %s", p_now_s - phase_s_, int(sub_),
						state_text(p_io).c_str()));
		enter(Phase::Done, p_now_s);
	}
	return done_;
}

std::string SquadTeamCheck::report() const {
	std::string out;
	for (const std::string &l : lines_) {
		out += l + "\n";
	}
	out += fmt("[squad-team] %u rows, %u failed\n", rows_, failures_);
	out += std::string("SQUAD TEAM: ") + (exit_code() == 0 ? "PASS" : "FAIL");
	return out;
}

}
