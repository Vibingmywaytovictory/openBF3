#include "bf3/host/tdm_rules.h"

#include "bf3/core/gameplay/eor_timeline.h"
#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/util/finite.h"
#include "bf3/core/util/key_values.h"
#include "bf3/host/bot_fill.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/weapon_rng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

namespace bf3::host {

namespace lg = sim::logic;

namespace {

constexpr size_t kMaxRuleSlots = 256;
constexpr size_t kMaxDeathRing = 256;
constexpr uint32_t kNoTick = 0;

uint32_t ticks_of(float p_seconds) {
	return lg::logic_delay_ticks(std::isfinite(p_seconds) ? std::clamp(double(p_seconds), 0.0, 3600.0) : 0.0);
}

float dist2(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0], dy = p_a[1] - p_b[1], dz = p_a[2] - p_b[2];
	return dx * dx + dy * dy + dz * dz;
}

bool state_alive(const sim::SoldierState &p_state) {
	return (p_state.flags & sim::soldier_flags::kAlive) != 0 && p_state.mode != sim::MoveMode::Dead;
}

using bf3::core::finite;

bool segment_hits_sphere(const sim::Vec3 &p_origin, const sim::Vec3 &p_dir, float p_length, const sim::Vec3 &p_centre, float p_radius, float &r_t) {
	const float cx = p_centre[0] - p_origin[0], cy = p_centre[1] - p_origin[1], cz = p_centre[2] - p_origin[2];
	const float t = std::clamp(cx * p_dir[0] + cy * p_dir[1] + cz * p_dir[2], 0.0f, p_length);
	const float px = p_origin[0] + p_dir[0] * t - p_centre[0], py = p_origin[1] + p_dir[1] * t - p_centre[1], pz = p_origin[2] + p_dir[2] * t - p_centre[2];
	r_t = t;
	return px * px + py * py + pz * pz <= p_radius * p_radius;
}

size_t cut_name(std::string_view p_name, std::array<char, net::kMaxPlayerNameBytes> &r_out) {
	size_t used = 0;
	size_t i = 0;
	while (i < p_name.size()) {
		const unsigned char c = static_cast<unsigned char>(p_name[i]);
		size_t n = 0;
		uint32_t min = 0;
		if (c < 0x80) {
			n = 1;
		} else if ((c & 0xE0) == 0xC0) {
			n = 2;
			min = 0x80;
		} else if ((c & 0xF0) == 0xE0) {
			n = 3;
			min = 0x800;
		} else if ((c & 0xF8) == 0xF0) {
			n = 4;
			min = 0x10000;
		} else {
			break;
		}
		if (i + n > p_name.size() || used + n > r_out.size()) {
			break;
		}
		uint32_t cp = n == 1 ? c : (c & (0x7Fu >> n));
		bool ok = true;
		for (size_t k = 1; k < n; ++k) {
			const unsigned char d = static_cast<unsigned char>(p_name[i + k]);
			ok &= (d & 0xC0) == 0x80;
			cp = (cp << 6) | (d & 0x3Fu);
		}
		if (!ok || (n > 1 && cp < min) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || c == 0) {
			break;
		}
		for (size_t k = 0; k < n; ++k) {
			r_out[used++] = p_name[i + k];
		}
		i += n;
	}
	for (size_t k = used; k < r_out.size(); ++k) {
		r_out[k] = 0;
	}
	return used;
}

bool same_state(const net::MatchState &p_a, const net::MatchState &p_b) {
	return p_a.phase == p_b.phase && p_a.round_start_tick == p_b.round_start_tick && p_a.countdown_end_tick == p_b.countdown_end_tick &&
			p_a.kills == p_b.kills && p_a.kill_limit == p_b.kill_limit && p_a.winner == p_b.winner && p_a.killer_camera == p_b.killer_camera &&
			p_a.round_end_tick == p_b.round_end_tick && p_a.unspawn_tick == p_b.unspawn_tick && p_a.restart_tick == p_b.restart_tick &&
			p_a.restrict_until_tick == p_b.restrict_until_tick && p_a.restrict_mask == p_b.restrict_mask;
}

bool same_row(const net::PlayerRow &p_a, const net::PlayerRow &p_b) {
	return std::memcmp(&p_a, &p_b, sizeof(net::PlayerRow)) == 0;
}

using Key = core::KeyDef<TdmTuning>;
using core::KeyFault;
using core::KeyKind;
using core::KeyOutcome;

KeyOutcome parse_kit_weights(const Key &, std::string_view p_value, TdmTuning &r_tuning) {
	std::array<uint32_t, 4> w{};
	std::string_view rest = p_value;
	size_t k = 0;
	uint64_t sum = 0;
	while (!rest.empty()) {
		const size_t sp = rest.find_first_of(" \t");
		const std::string_view tok = rest.substr(0, sp);
		rest = sp == std::string_view::npos ? std::string_view() : core::trim(rest.substr(sp));
		int32_t v = 0;
		if (k >= w.size() || core::parse_i32_value(tok, 0, 1000000, v).fault != KeyFault::None) {
			return { KeyFault::BadValue };
		}
		w[k++] = uint32_t(v);
		sum += uint64_t(v);
	}
	if (k != w.size() || sum == 0) {
		return { KeyFault::BadValue };
	}
	r_tuning.bot_kit_weights = w;
	return {};
}

bool abort_target(std::string_view p_target, ImmunityAbortEntry &r_entry) {
	if (p_target == "move") {
		r_entry.move = true;
	} else if (p_target == "fire") {
		r_entry.buttons = sim::buttons::kFire;
	} else if (p_target == "jump") {
		r_entry.buttons = sim::buttons::kJump;
	} else if (p_target == "sprint") {
		r_entry.buttons = sim::buttons::kSprint;
	} else if (p_target == "zoom") {
		r_entry.buttons = sim::buttons::kZoom;
	} else if (p_target == "reload") {
		r_entry.buttons = sim::buttons::kReload;
	} else {
		return false;
	}
	return true;
}

KeyOutcome parse_immunity_abort_map(const Key &, std::string_view p_value, TdmTuning &r_tuning) {
	std::vector<ImmunityAbortEntry> map;
	std::string_view rest = p_value;
	while (!rest.empty()) {
		const size_t sp = rest.find_first_of(" \t");
		const std::string_view tok = rest.substr(0, sp);
		rest = sp == std::string_view::npos ? std::string_view() : core::trim(rest.substr(sp));
		const size_t colon = tok.find(':');
		ImmunityAbortEntry e;
		if (colon == std::string_view::npos || map.size() >= kMaxImmunityAbortEntries ||
				core::parse_i32_value(tok.substr(0, colon), -1000000, 1000000, e.value).fault != KeyFault::None ||
				!abort_target(tok.substr(colon + 1), e)) {
			return { KeyFault::BadValue };
		}
		map.push_back(e);
	}
	r_tuning.immunity_abort_map = std::move(map);
	return {};
}

#define BF3_TDM_FIELD(f) [](TdmTuning &t) -> void * { return &t.f; }

const Key kKeys[] = {
	{ "revive_wait_radius_m", KeyKind::Float, 0.0f, 1000.0f, BF3_TDM_FIELD(revive_wait_radius_m) },
	{ "revive_wait_s", KeyKind::Float, 0.0f, 3600.0f, BF3_TDM_FIELD(revive_wait_s) },
	{ "revive_takes_kill", KeyKind::Bool, 0, 0, BF3_TDM_FIELD(revive_takes_kill) },
	{ "defib_range_m", KeyKind::Float, 0.0f, 100.0f, BF3_TDM_FIELD(defib_range_m) },
	{ "corpse_up_m", KeyKind::Float, -10.0f, 10.0f, BF3_TDM_FIELD(corpse_up_m) },
	{ "corpse_radius_m", KeyKind::Float, 0.0f, 10.0f, BF3_TDM_FIELD(corpse_radius_m) },
	{ "living_radius_m", KeyKind::Float, 0.0f, 10.0f, BF3_TDM_FIELD(living_radius_m) },
	{ "score_kill", KeyKind::I32, -100000, 100000, BF3_TDM_FIELD(score_kill) },
	{ "score_headshot", KeyKind::I32, -100000, 100000, BF3_TDM_FIELD(score_headshot) },
	{ "score_revive", KeyKind::I32, -100000, 100000, BF3_TDM_FIELD(score_revive) },
	{ "score_spawn_squad", KeyKind::I32, -100000, 100000, BF3_TDM_FIELD(score_spawn_squad) },
	{ "score_revive_squad", KeyKind::I32, -100000, 100000, BF3_TDM_FIELD(score_revive_squad) },
	{ "score_squad_wipe", KeyKind::I32, -100000, 100000, BF3_TDM_FIELD(score_squad_wipe) },
	{ "bot_kit_weights", KeyKind::Custom, 0, 0, nullptr, {}, parse_kit_weights },
	{ "spawn_safe_distance_m", KeyKind::Float, 0.0f, 10000.0f, BF3_TDM_FIELD(spawn_safe_distance_m) },
	{ "friendly_block_radius_m", KeyKind::Float, 0.0f, 10000.0f, BF3_TDM_FIELD(friendly_block_radius_m) },
	{ "friendly_block_count", KeyKind::I32, 0, 1000, BF3_TDM_FIELD(friendly_block_count) },
	{ "friendly_block_s", KeyKind::Float, 0.0f, 3600.0f, BF3_TDM_FIELD(friendly_block_s) },
	{ "spawn_los_rays", KeyKind::I32, 0, 64, BF3_TDM_FIELD(spawn_los_rays) },
	{ "spawn_chest_height_m", KeyKind::Float, 0.0f, 10.0f, BF3_TDM_FIELD(spawn_chest_height_m) },
	{ "team_kill_weight", KeyKind::Float, -100.0f, 100.0f, BF3_TDM_FIELD(team_kill_weight) },
	{ "rotate_teams", KeyKind::Bool, 0, 0, BF3_TDM_FIELD(rotate_teams) },
	{ "suppression_resets_regen", KeyKind::Bool, 0, 0, BF3_TDM_FIELD(suppression_resets_regen) },
	{ "immunity_abort_map", KeyKind::Custom, 0, 0, nullptr, {}, parse_immunity_abort_map },
	{ "measured", KeyKind::Bool, 0, 0, BF3_TDM_FIELD(measured) },
};

#undef BF3_TDM_FIELD

std::string tdm_fault(const core::KeyFaultInfo &p_info) {
	const std::string where = core::key_fault_where(p_info);
	switch (p_info.fault) {
		case KeyFault::NoEquals:
			return where + ": no '='";
		case KeyFault::UnknownKey:
			return where + ": unknown key `" + std::string(p_info.key) + "`";
		case KeyFault::NotANumber:
		case KeyFault::NotAWholeNumber:
		case KeyFault::OutOfRange:
		case KeyFault::NotABool:
		case KeyFault::NotAChoice:
		case KeyFault::BadValue:
			return where + ": bad value for `" + std::string(p_info.key) + "`: `" + std::string(p_info.value) + "`";
		default:
			return core::key_fault_message(p_info);
	}
}

const core::KeyValueGrammar<TdmTuning> kGrammar{ "tdm tuning", kKeys, kMaxTdmTuningBytes, kMaxTdmTuningLines, SIZE_MAX,
	false, '\n', true, true, tdm_fault };
}

core::Result<TdmTuning> parse_tdm_tuning(std::string_view p_text) {
	return core::parse_key_values(p_text, kGrammar);
}

core::Result<TdmTuning> load_tdm_tuning(const std::string &p_path) {
	if (p_path.empty()) {
		return TdmTuning{};
	}
	std::error_code ec;
	const uintmax_t size = std::filesystem::file_size(p_path, ec);
	if (ec) {
		return core::make_error("tdm tuning: " + p_path + ": " + ec.message());
	}
	if (size > kMaxTdmTuningBytes) {
		return core::make_error("tdm tuning: " + p_path + ": " + std::to_string(size) + " bytes (limit " + std::to_string(kMaxTdmTuningBytes) + ")");
	}
	std::ifstream in(p_path, std::ios::binary);
	if (!in) {
		return core::make_error("tdm tuning: cannot open " + p_path);
	}
	std::string text(size_t(size), '\0');
	in.read(text.data(), std::streamsize(size));
	text.resize(size_t(in.gcount()));
	core::Result<TdmTuning> t = parse_tdm_tuning(text);
	if (!t) {
		return core::error_at(p_path, t.error());
	}
	return t;
}

struct TdmRules::Impl {
	struct Slot {
		bool connected = false;
		bool bot = false;
		bool deploy = false;
		bool alive = false;
		uint32_t life = 0;
		uint8_t kit = 0;
		uint8_t row_kit = 0;
		uint8_t team_at_death = 0;
		int16_t last_killer = -1;
		uint8_t last_killer_team = 0;
		uint32_t death_tick = kNoTick;
		uint32_t earliest = 0;
		uint32_t revive_until = 0;
		uint32_t last_damage = 0;
		bool damaged = false;
		bool pending_spawn = false;
		int16_t spawn_mate = kNoSquadMate;
		bool suicide = false;
		uint32_t last_request = UINT32_MAX;
		int64_t shot_cursor = -1;
		bool corpse_valid = false;
		bool corpse_fresh = false;
		sim::Vec3 corpse{};
		uint16_t corpse_yaw = 0;
		uint16_t kills = 0, deaths = 0;
		int32_t score = 0;
		std::array<char, net::kMaxPlayerNameBytes> name{};
		uint8_t name_bytes = 0;
	};

	std::shared_ptr<const core::gameplay::LogicGraph> graph;
	std::shared_ptr<const GunplayLevel> gunplay;
	std::shared_ptr<const BotLevel> bots;
	std::shared_ptr<const TdmTuning> tuning_holder;
	const TdmTuning *tuning = nullptr;
	std::shared_ptr<const core::gameplay::EorTimeline> eor;
	sim::MotorParams motor;
	MatchRulesConfig config;
	uint64_t match_seed = 0;
	uint32_t max_slots = 0;
	lg::RoundNativeConfig native_config;
	std::optional<lg::LogicRuntime> runtime;
	std::optional<lg::RoundNatives> natives;
	std::vector<lg::LogicNetworkedEvent> networked;
	uint32_t base = 0;
	bool based = false;
	uint32_t tick = 0;
	bool faulted = false;
	std::vector<Slot> slots;
	std::vector<DownedSoldier> downed;
	std::vector<uint8_t> revive_requests;
	std::vector<sim::FriendlyDeath> death_ring;
	std::vector<sim::SoldierState> last_states;
	net::MatchState state;
	net::MatchState sent_state;
	bool state_sent = false;
	std::vector<net::PlayerRow> rows;
	std::vector<net::PlayerRow> sent_rows;
	SquadTable squads;
	std::vector<uint8_t> switch_requests;
	std::unique_ptr<sim::NavQuery> nav;
	lg::RoundPhase last_phase = lg::RoundPhase::PreRound;
	bool was_restricted = false;
	mutable RulesStats stats;
	uint32_t delay_ticks = 300;
	uint32_t man_down_ticks = 780;
	uint32_t regen_delay_ticks = 600;
	float regen_per_tick = 5.0f / 60.0f;
	float max_health = 100.0f;
	float post_revive_health = 20.0f;

	uint32_t hint_tick() const { return tick + 1u; }

	void fault(const core::Error &p_error) {
		if (!faulted) {
			std::fprintf(stderr, "TdmRules: the round flow stops: %s\n", p_error.message.c_str());
		}
		faulted = true;
		++stats.runtime_faults;
	}

	uint8_t team_of(size_t p_slot) const {
		return p_slot < slots.size() && slots[p_slot].connected && natives ? natives->team_of(uint8_t(p_slot)) : 0;
	}

	uint8_t draw_kit(size_t p_slot, uint32_t p_life) const {
		const uint64_t r = sim::weapons::splitmix64(match_seed ^ uint64_t(p_slot) ^ (uint64_t(p_life) << 32));
		uint64_t sum = 0;
		for (const uint32_t w : tuning->bot_kit_weights) {
			sum += w;
		}
		if (sum == 0) {
			return 0;
		}
		uint64_t x = r % sum;
		for (uint8_t k = 0; k < kKitCount; ++k) {
			if (x < tuning->bot_kit_weights[k]) {
				return k;
			}
			x -= tuning->bot_kit_weights[k];
		}
		return 0;
	}

	core::Status start_runtime() {
		lg::LogicRuntimeOptions o;
		o.root_seed = match_seed;
		core::Result<lg::LogicRuntime> rt = lg::LogicRuntime::create(graph, o);
		if (!rt) {
			return core::error_at("tdm rules: logic runtime", rt.error());
		}
		core::Result<lg::RoundNatives> n = lg::RoundNatives::create(graph, native_config);
		if (!n) {
			return core::error_at("tdm rules: natives", n.error());
		}
		runtime.emplace(std::move(*rt));
		natives.emplace(std::move(*n));
		core::Result<lg::LogicTickStats> started = runtime->start();
		if (!started) {
			return core::error_at("tdm rules: logic start", started.error());
		}
		return core::Status::success();
	}

	bool near_assault_teammate(size_t p_victim, uint8_t p_team) const {
		if (p_victim >= last_states.size()) {
			return false;
		}
		const sim::Vec3 corpse = last_states[p_victim].position;
		const float r2 = tuning->revive_wait_radius_m * tuning->revive_wait_radius_m;
		for (size_t i = 0; i < slots.size() && i < last_states.size(); ++i) {
			const Slot &o = slots[i];
			if (i == p_victim || !o.connected || !o.alive || team_of(i) != p_team || o.row_kit != uint8_t(KitRole::Assault)) {
				continue;
			}
			if (state_alive(last_states[i]) && dist2(last_states[i].position, corpse) <= r2) {
				return true;
			}
		}
		return false;
	}

	bool revivable(uint8_t p_slot, uint8_t p_team, uint32_t p_tick) const {
		bool out = false;
		for (const DownedSoldier &d : downed) {
			out |= d.slot == p_slot && d.team == p_team && d.team == team_of(p_slot) && d.until_tick >= p_tick;
		}
		return out;
	}

	void revive_order(uint8_t p_slot, uint8_t p_reviver, std::vector<RulesOrder> &r_orders) {
		Slot &target = slots[p_slot];
		if (target.bot || target.deploy) {
			target.kit = target.row_kit;
		}
		RulesOrder o;
		o.kind = RulesOrder::Kind::Revive;
		o.slot = p_slot;
		o.position = target.corpse;
		o.yaw = target.corpse_yaw;
		o.health = post_revive_health;
		o.reviver = p_reviver;
		r_orders.push_back(o);
		remove_downed(p_slot);
		target.corpse_valid = false;
	}

	uint32_t team_count(uint8_t p_team) const {
		uint32_t n = 0;
		for (size_t i = 0; i < slots.size() && i < net::kMaxPlayerRows; ++i) {
			n += slots[i].connected && team_of(i) == p_team ? 1u : 0u;
		}
		return n;
	}

	SquadRefusal switch_refusal(uint8_t p_slot) const {
		const uint8_t from = team_of(p_slot);
		if (p_slot >= net::kMaxPlayerRows || from == 0) {
			return SquadRefusal::NoTeam;
		}
		const lg::RoundPhase phase = natives ? natives->phase() : lg::RoundPhase::Restarting;
		if (faulted || !runtime || (phase != lg::RoundPhase::PreRound && phase != lg::RoundPhase::InRound)) {
			return SquadRefusal::Phase;
		}
		const uint32_t cap = std::max<uint32_t>(1u, uint32_t(config.max_players) / 2u);
		const uint32_t leaving = team_count(from), joining = team_count(uint8_t(3u - from));
		if (joining + 1u > cap || joining + 1u > leaving) {
			return SquadRefusal::Balance;
		}
		return SquadRefusal::None;
	}

	void apply_switches(std::span<const sim::SoldierState> p_states, std::vector<RulesOrder> &r_orders) {
		for (const uint8_t slot : std::exchange(switch_requests, {})) {
			if (switch_refusal(slot) != SquadRefusal::None) {
				++stats.team_switches_refused;
				continue;
			}
			const uint8_t to = uint8_t(3u - team_of(slot));
			const core::Status st = natives->player_switch_team(slot, to);
			if (!st.ok()) {
				fault(st.error());
				return;
			}
			Slot &s = slots[slot];
			remove_downed(slot);
			s.corpse_valid = false;
			s.revive_until = 0;
			s.pending_spawn = false;
			if (s.alive || (slot < p_states.size() && state_alive(p_states[slot]))) {
				r_orders.push_back(RulesOrder{ RulesOrder::Kind::Unspawn, slot });
				s.alive = false;
				if (!s.deploy) {
					r_orders.push_back(RulesOrder{ RulesOrder::Kind::Spawn, slot });
				}
			}
			squads.follow_team(slot, to);
			++stats.team_switches;
		}
	}

	bool mate_ready(uint8_t p_slot, int p_mate, std::span<const sim::SoldierState> p_states) const {
		if (p_mate < 0 || p_mate == int(p_slot) || size_t(p_mate) >= slots.size() ||
				size_t(p_mate) >= p_states.size() || !squads.same_squad(p_slot, uint8_t(p_mate))) {
			return false;
		}
		const Slot &o = slots[size_t(p_mate)];
		const sim::SoldierState &st = p_states[size_t(p_mate)];
		return o.connected && o.alive && team_of(size_t(p_mate)) == team_of(p_slot) && state_alive(st) &&
				finite(st.position);
	}

	int squad_mate(uint8_t p_slot, uint8_t p_spawn_choice) const {
		const int mate = p_spawn_choice != 0 ? squads.mate(p_slot, uint8_t(p_spawn_choice - 1u)) : kNoSquadMate;
		return mate_ready(p_slot, mate, last_states) ? mate : kNoSquadMate;
	}

	sim::Vec3 squad_spawn_point(const sim::Vec3 &p_mate) {
		sim::NavPolyRef poly = 0;
		sim::Vec3 at{};
		if (nav != nullptr && nav->nearest(p_mate, kSquadSpawnNavExtents, &poly, &at) && finite(at)) {
			++stats.squad_spawns_on_nav;
			return at;
		}
		return p_mate;
	}

	bool squad_wiped(uint8_t p_victim) const {
		const std::span<const uint8_t> members = squads.members(squads.team_of(p_victim), squads.squad_of(p_victim));
		if (members.size() < 2) {
			return false;
		}
		for (const uint8_t slot : members) {
			if (slot < slots.size() && slots[slot].alive) {
				return false;
			}
		}
		return true;
	}

	void remove_downed(uint8_t p_slot) {
		std::erase_if(downed, [&](const DownedSoldier &d) { return d.slot == p_slot; });
	}

	void restart_round(uint32_t p_tick, std::vector<RulesOrder> &r_orders) {
		std::vector<uint8_t> teams(slots.size(), 0);
		const bool rotate = tuning->rotate_teams && natives && natives->params().rotate_team_on_new_round;
		for (size_t i = 0; i < slots.size(); ++i) {
			const uint8_t t = team_of(i);
			teams[i] = rotate && t != 0 ? uint8_t(3 - t) : t;
		}
		const core::Status st = start_runtime();
		if (!st.ok()) {
			fault(st.error());
			return;
		}
		if (rotate) {
			squads.swap_teams();
		}
		switch_requests.clear();
		base = p_tick;
		based = true;
		for (size_t i = 0; i < slots.size(); ++i) {
			Slot &s = slots[i];
			if (!s.connected || teams[i] == 0) {
				continue;
			}
			const core::Status e = natives->player_enter_team(uint8_t(i), s.bot, teams[i], *runtime);
			if (!e.ok()) {
				fault(e.error());
				return;
			}
			s.kills = 0;
			s.deaths = 0;
			s.score = 0;
			s.earliest = 0;
			s.revive_until = 0;
			s.pending_spawn = false;
			s.corpse_valid = false;
			s.last_killer = -1;
			r_orders.push_back(RulesOrder{ RulesOrder::Kind::Unspawn, uint8_t(i) });
			s.alive = false;
			if (!s.deploy && i < net::kMaxPlayerRows) {
				r_orders.push_back(RulesOrder{ RulesOrder::Kind::Spawn, uint8_t(i) });
			}
		}
		downed.clear();
		death_ring.clear();
		state = net::MatchState();
		state.unspawn_tick = p_tick;
		state.phase = net::MatchPhase::PreRound;
		state.kill_limit = natives->kill_limit();
		state.killer_camera = natives->killer_camera();
		last_phase = lg::RoundPhase::PreRound;
		was_restricted = false;
		++stats.restarts;
	}
};

TdmRules::TdmRules() :
		impl_(std::make_unique<Impl>()) {}

TdmRules::~TdmRules() = default;

core::Result<std::unique_ptr<TdmRules>> TdmRules::create(const SharedLevel &p_level, std::shared_ptr<const BotLevel> p_bots, const MatchRulesConfig &p_config,
		uint64_t p_match_seed, uint32_t p_max_slots) {
	if (p_config.mode != MatchRulesConfig::Mode::Tdm) {
		return core::make_error("tdm rules: the rules mode is not Tdm");
	}
	if (p_level.logic == nullptr) {
		return core::make_error("tdm rules: the level has no logic graph (SharedLoadOptions::tdm)");
	}
	if (p_level.gunplay == nullptr) {
		return core::make_error("tdm rules: the level has no gunplay tables (SharedLoadOptions::gunplay)");
	}
	if (p_level.eor == nullptr) {
		return core::make_error("tdm rules: the level has no end-of-round timeline (SharedLoadOptions::tdm)");
	}
	if (p_max_slots == 0 || p_max_slots > kMaxRuleSlots) {
		return core::make_error("tdm rules: " + std::to_string(p_max_slots) + " slots (1.." + std::to_string(kMaxRuleSlots) + ")");
	}
	std::unique_ptr<TdmRules> rules(new TdmRules());
	Impl &m = *rules->impl_;
	m.graph = p_level.logic;
	m.gunplay = p_level.gunplay;
	m.eor = p_level.eor;
	m.bots = std::move(p_bots);
	m.tuning_holder = p_level.tdm_tuning != nullptr ? p_level.tdm_tuning : std::make_shared<const TdmTuning>();
	m.tuning = m.tuning_holder.get();
	m.motor = p_level.motor;
	m.config = p_config;
	m.match_seed = p_match_seed;
	m.max_slots = p_max_slots;
	m.native_config.max_players = p_config.max_players;
	m.native_config.kill_limit_override = p_config.kill_limit_override;
	m.native_config.preround_override_s = p_config.preround_override_s;
	m.native_config.min_players_override = p_config.min_players_override;
	m.native_config.params = p_level.logic_natives;
	m.native_config.team_kill_weight = m.tuning->team_kill_weight;
	m.native_config.rotate_teams = m.tuning->rotate_teams;
	BF3_TRY(m.start_runtime());
	m.slots.resize(p_max_slots);
	m.last_states.assign(p_max_slots, sim::SoldierState());
	m.squads.configure(std::min<size_t>(p_max_slots, net::kMaxPlayerRows), p_level.squad_size);
	if (m.bots != nullptr && m.bots->nav != nullptr) {
		core::Result<std::unique_ptr<sim::NavQuery>> nav = sim::NavQuery::create(m.bots->nav);
		if (!nav) {
			return core::error_at("tdm rules: squad spawn nav query", nav.error());
		}
		m.nav = std::move(*nav);
	}
	const core::gameplay::SoldierDef &soldier = p_level.gunplay->soldier;
	m.delay_ticks = ticks_of(m.natives->player_killed_delay_s());
	m.man_down_ticks = soldier.man_down_time > 0.0f ? ticks_of(soldier.man_down_time) : 780u;
	m.regen_delay_ticks = soldier.regeneration_delay > 0.0f ? ticks_of(soldier.regeneration_delay) : 600u;
	m.regen_per_tick = (soldier.regeneration_rate > 0.0f && std::isfinite(soldier.regeneration_rate) ? soldier.regeneration_rate : 5.0f) / float(sim::kSimHz);
	m.max_health = p_level.gunplay->max_health > 0.0f ? p_level.gunplay->max_health : 100.0f;
	m.post_revive_health = soldier.post_revive_health > 0.0f ? std::min(soldier.post_revive_health, m.max_health) : 20.0f;
	m.state.phase = net::MatchPhase::PreRound;
	m.state.kill_limit = m.natives->kill_limit();
	m.state.killer_camera = m.natives->killer_camera();
	return rules;
}

std::optional<uint32_t> TdmRules::respawn_tick(uint8_t p_slot, uint32_t p_death_tick) {
	Impl &m = *impl_;
	if (p_slot >= m.slots.size() || !m.slots[p_slot].connected) {
		return std::nullopt;
	}
	Impl::Slot &s = m.slots[p_slot];
	s.death_tick = p_death_tick;
	s.earliest = p_death_tick + m.delay_ticks;
	const lg::RoundPhase phase = m.natives ? m.natives->phase() : lg::RoundPhase::Restarting;

	const bool moving = m.natives && m.natives->moves_at_death(p_slot);
	const bool revive = m.config.revive && m.gunplay->kits.defib_def != sim::weapons::kNoWeapon && !moving;
	if (phase == lg::RoundPhase::RoundOver || phase == lg::RoundPhase::Restarting || p_slot >= net::kMaxPlayerRows) {
		s.revive_until = 0;
		return std::nullopt;
	}
	if (s.deploy) {
		s.revive_until = revive ? p_death_tick + m.man_down_ticks : 0;
		return std::nullopt;
	}
	uint32_t at = s.earliest;
	if (s.bot && revive && m.near_assault_teammate(p_slot, m.team_of(p_slot))) {
		at = p_death_tick + std::min(ticks_of(m.tuning->revive_wait_s), m.man_down_ticks);
	}
	s.earliest = at;
	s.revive_until = revive ? std::min(p_death_tick + m.man_down_ticks, at) : 0;
	return at;
}

uint32_t TdmRules::earliest_respawn(uint8_t p_slot) const {
	const Impl &m = *impl_;
	return p_slot < m.slots.size() ? m.slots[p_slot].earliest : 0;
}

uint32_t TdmRules::revive_until(uint8_t p_slot) const {
	const Impl &m = *impl_;
	return p_slot < m.slots.size() ? m.slots[p_slot].revive_until : 0;
}

void TdmRules::hit(const net::HitEvent &p_event) {
	Impl &m = *impl_;
	if (p_event.victim < m.slots.size() && p_event.damage > 0.0f) {
		m.slots[p_event.victim].last_damage = m.hint_tick();
		m.slots[p_event.victim].damaged = true;
	}
}

void TdmRules::death(const net::DeathEvent &p_event) {
	Impl &m = *impl_;
	const uint8_t v = p_event.victim;
	if (v >= m.slots.size() || !m.slots[v].connected || !m.natives || !m.runtime) {
		return;
	}
	Impl::Slot &s = m.slots[v];
	const uint32_t now = m.hint_tick();
	if (s.death_tick != now) {
		(void)respawn_tick(v, now);
	}
	const uint8_t vteam = m.team_of(v);
	const bool fall = p_event.killer == v || p_event.killer >= m.slots.size() || !m.slots[p_event.killer].connected;
	const uint8_t kteam = fall ? 0 : m.team_of(p_event.killer);
	const lg::RoundPhase phase = m.natives->phase();
	if (!m.faulted) {
		const core::Status st = m.natives->player_died(v, vteam, fall ? -1 : int(p_event.killer), kteam, *m.runtime);
		if (!st.ok()) {
			m.fault(st.error());
		}
	}
	const bool counting = phase == lg::RoundPhase::PreRound || phase == lg::RoundPhase::InRound;
	s.last_killer = -1;
	s.last_killer_team = 0;
	if (counting && !fall && kteam != 0 && kteam != vteam) {
		Impl::Slot &k = m.slots[p_event.killer];
		k.kills = uint16_t(std::min<uint32_t>(k.kills + 1u, 0xFFFFu));
		k.score += m.tuning->score_kill + (p_event.headshot ? m.tuning->score_headshot : 0);
		s.last_killer = int16_t(p_event.killer);
		s.last_killer_team = kteam;
	}
	if (counting) {
		s.deaths = uint16_t(std::min<uint32_t>(s.deaths + 1u, 0xFFFFu));
	}
	if (m.team_of(v) != vteam) {
		s.revive_until = 0;
	}
	s.alive = false;
	s.team_at_death = vteam;
	if (s.last_killer >= 0 && m.squad_wiped(v)) {
		m.slots[size_t(s.last_killer)].score += m.tuning->score_squad_wipe;
		++m.stats.squad_wipes;
	}
	s.pending_spawn = false;
	s.corpse_valid = true;
	s.corpse_fresh = true;
	if (v < m.last_states.size()) {
		s.corpse = m.last_states[v].position;
		s.corpse_yaw = m.last_states[v].yaw;
	}
	if (s.bot) {
		s.kit = m.draw_kit(v, s.life + 1u);
	}
	m.remove_downed(v);
	if (s.revive_until > now && v < net::kMaxPlayerRows) {
		DownedSoldier d;
		d.slot = v;
		d.team = vteam;
		d.until_tick = s.revive_until;
		d.corpse = s.corpse;
		d.yaw = s.corpse_yaw;
		m.downed.insert(std::upper_bound(m.downed.begin(), m.downed.end(), d, [](const DownedSoldier &a, const DownedSoldier &b) { return a.slot < b.slot; }), d);
	}
	if (vteam != 0 && finite(s.corpse)) {
		if (m.death_ring.size() >= kMaxDeathRing) {
			m.death_ring.erase(m.death_ring.begin());
		}
		m.death_ring.push_back(sim::FriendlyDeath{ s.corpse, now, vteam });
	}
}

void TdmRules::detonation(const net::DetonationWire &p_event) {
	(void)p_event;
}

void TdmRules::revive(const net::ReviveEvent &p_event) {
	Impl &m = *impl_;
	if (p_event.revived >= m.slots.size() || p_event.reviver >= m.slots.size()) {
		return;
	}
	Impl::Slot &r = m.slots[p_event.reviver];
	Impl::Slot &d = m.slots[p_event.revived];
	++m.stats.revives;
	if (r.connected && p_event.reviver != p_event.revived) {
		const bool mates = m.squads.same_squad(p_event.reviver, p_event.revived);
		r.score += mates ? m.tuning->score_revive_squad : m.tuning->score_revive;
		m.stats.squad_revives += mates ? 1u : 0u;
	}
	if (d.deaths > 0) {
		--d.deaths;
	}
	if (m.tuning->revive_takes_kill && d.last_killer_team != 0 && m.natives && m.runtime && !m.faulted) {
		const core::Status st = m.natives->kill_undone(d.last_killer_team, *m.runtime);
		if (!st.ok()) {
			m.fault(st.error());
		}
	}
	d.last_killer = -1;
	d.last_killer_team = 0;
}

uint8_t TdmRules::on_connect(uint8_t p_slot, bool p_bot, std::string_view p_name) {
	Impl &m = *impl_;
	if (p_slot >= m.slots.size()) {
		return 0;
	}
	Impl::Slot &s = m.slots[p_slot];
	s = Impl::Slot();
	s.connected = true;
	s.bot = p_bot;
	s.deploy = !p_bot && m.config.deploy_screen;
	s.name_bytes = uint8_t(cut_name(p_name.empty() && p_bot ? std::string_view("bot") : p_name, s.name));
	if (p_bot) {
		s.kit = m.draw_kit(p_slot, 1);
	}
	if (p_slot >= net::kMaxPlayerRows || !m.natives || !m.runtime) {
		return 0;
	}
	if (!m.faulted) {
		const core::Status st = m.natives->player_enter(p_slot, p_bot, *m.runtime);
		if (!st.ok()) {
			m.fault(st.error());
		}
	}
	const uint8_t team = m.natives->team_of(p_slot);
	m.squads.assign(p_slot, team);
	return team;
}

void TdmRules::on_disconnect(uint8_t p_slot) {
	Impl &m = *impl_;
	if (p_slot >= m.slots.size() || !m.slots[p_slot].connected) {
		return;
	}
	if (m.natives && m.runtime && !m.faulted) {
		const core::Status st = m.natives->player_leave(p_slot, *m.runtime);
		if (!st.ok()) {
			m.fault(st.error());
		}
	}
	m.remove_downed(p_slot);
	m.squads.remove(p_slot);
	std::erase(m.switch_requests, p_slot);
	m.slots[p_slot] = Impl::Slot();
}

void TdmRules::on_spawned(uint8_t p_slot, uint32_t p_tick) {
	Impl &m = *impl_;
	if (p_slot >= m.slots.size() || !m.slots[p_slot].connected) {
		return;
	}
	Impl::Slot &s = m.slots[p_slot];
	s.alive = true;
	++s.life;
	s.pending_spawn = false;
	s.suicide = false;
	s.corpse_valid = false;
	s.corpse_fresh = false;
	s.revive_until = 0;
	s.last_damage = p_tick;
	s.damaged = false;
	s.row_kit = kit_of(p_slot) != kKeepKit ? kit_of(p_slot) : s.row_kit;
	m.remove_downed(p_slot);
	if (m.natives && m.runtime && !m.faulted) {
		const core::Status st = m.natives->player_spawned(p_slot, m.team_of(p_slot), *m.runtime);
		if (!st.ok()) {
			m.fault(st.error());
		}
	}
}

bool TdmRules::request_suicide(uint8_t p_slot) {
	Impl &m = *impl_;
	Impl::Slot *s = p_slot < m.slots.size() ? &m.slots[p_slot] : nullptr;
	const lg::RoundPhase phase = m.natives ? m.natives->phase() : lg::RoundPhase::Restarting;
	const bool open = phase == lg::RoundPhase::PreRound || phase == lg::RoundPhase::InRound;
	if (s == nullptr || !s->connected || !s->alive || !open || m.faulted) {
		++m.stats.suicides_refused;
		return false;
	}
	s->suicide = true;
	return true;
}

bool TdmRules::request_revive(uint8_t p_slot) {
	Impl &m = *impl_;
	if (p_slot >= m.slots.size() || !m.slots[p_slot].connected || m.slots[p_slot].alive ||
			!m.slots[p_slot].corpse_valid || !m.revivable(p_slot, m.team_of(p_slot), m.hint_tick()) || m.faulted) {
		return false;
	}
	if (std::find(m.revive_requests.begin(), m.revive_requests.end(), p_slot) == m.revive_requests.end()) {
		m.revive_requests.push_back(p_slot);
	}
	return true;
}

bool TdmRules::take_suicide(uint8_t p_slot) {
	Impl &m = *impl_;
	Impl::Slot *s = p_slot < m.slots.size() ? &m.slots[p_slot] : nullptr;
	if (s == nullptr || !s->suicide) {
		return false;
	}
	s->suicide = false;
	if (!s->alive) {
		return false;
	}
	++m.stats.suicides;
	return true;
}

SquadRefusal TdmRules::request_squad(uint8_t p_slot, const net::SquadRequest &p_request) {
	Impl &m = *impl_;
	++m.stats.squad_requests;
	const bool player = p_slot < m.slots.size() && p_slot < net::kMaxPlayerRows && m.slots[p_slot].connected;
	const uint8_t team = player ? m.team_of(p_slot) : 0;
	SquadRefusal why = SquadRefusal::NoTeam;
	if (player && p_request.op == net::SquadOp::SwitchTeam) {
		why = m.switch_refusal(p_slot);
		if (why == SquadRefusal::None) {
			if (std::find(m.switch_requests.begin(), m.switch_requests.end(), p_slot) == m.switch_requests.end()) {
				m.switch_requests.push_back(p_slot);
			}
		} else {
			++m.stats.team_switches_refused;
		}
		return why;
	}
	if (player) {
		switch (p_request.op) {
			case net::SquadOp::Join:
				why = m.squads.join(p_slot, team, p_request.squad);
				break;
			case net::SquadOp::Create:
				why = m.squads.create(p_slot, team);
				break;
			case net::SquadOp::Leave:
				why = m.squads.leave(p_slot);
				break;
			case net::SquadOp::Kick:
				why = m.squads.kick(p_slot, p_request.target);
				break;
			default:
				why = SquadRefusal::BadSquad;
				break;
		}
	}
	if (why != SquadRefusal::None) {
		++m.stats.squad_refused;
	}
	return why;
}

SpawnRefusal TdmRules::request_spawn(uint8_t p_slot, uint8_t p_kit, uint8_t p_spawn_choice, uint32_t p_tick) {
	Impl &m = *impl_;
	++m.stats.spawn_requests;
	SpawnRefusal why = SpawnRefusal::None;
	Impl::Slot *s = p_slot < m.slots.size() ? &m.slots[p_slot] : nullptr;
	if (s == nullptr || !s->connected || p_slot >= net::kMaxPlayerRows || m.team_of(p_slot) == 0) {
		why = SpawnRefusal::Spectator;
	} else if (s->last_request == p_tick) {
		why = SpawnRefusal::RateLimited;
	} else {
		s->last_request = p_tick;
		const lg::RoundPhase phase = m.natives ? m.natives->phase() : lg::RoundPhase::Restarting;
		if (!s->deploy || phase == lg::RoundPhase::RoundOver || phase == lg::RoundPhase::Restarting || m.faulted) {
			why = SpawnRefusal::Phase;
		} else if (s->alive || s->pending_spawn) {
			why = SpawnRefusal::Alive;
		} else if (p_kit >= kKitCount) {
			why = SpawnRefusal::BadKit;
		} else if (p_spawn_choice != 0 && m.squad_mate(p_slot, p_spawn_choice) == kNoSquadMate) {
			why = SpawnRefusal::SquadMate;
		} else if (p_tick < s->earliest) {
			why = SpawnRefusal::TooEarly;
		}
	}
	if (why != SpawnRefusal::None) {
		++m.stats.refused[size_t(why)];
		return why;
	}
	s->pending_spawn = true;
	s->spawn_mate = int16_t(m.squad_mate(p_slot, p_spawn_choice));
	s->kit = p_kit;
	s->revive_until = 0;
	m.remove_downed(p_slot);
	++m.stats.spawns_accepted;
	return SpawnRefusal::None;
}

core::Status TdmRules::step(uint32_t p_tick, const ServerView &p_view, const Gunplay &p_gunplay, std::span<sim::SoldierState> r_states,
		std::vector<RulesOrder> &r_orders) {
	Impl &m = *impl_;
	m.tick = p_tick;
	const size_t n = std::min(m.slots.size(), r_states.size());

	for (size_t i = 0; i < n; ++i) {
		Impl::Slot &s = m.slots[i];
		if (s.corpse_fresh) {
			s.corpse_fresh = false;
			if (finite(r_states[i].position)) {
				s.corpse = r_states[i].position;
				s.corpse_yaw = r_states[i].yaw;
			}
			for (DownedSoldier &d : m.downed) {
				if (d.slot == i) {
					d.corpse = s.corpse;
					d.yaw = s.corpse_yaw;
				}
			}
		}
	}

	m.networked.clear();
	if (!m.faulted && m.runtime && m.natives) {
		if (!m.based) {
			m.base = p_tick - 1u - m.runtime->tick();
			m.based = true;
		}
		const sim::Tick next = m.runtime->tick() + 1u;
		if (p_tick - m.base != next) {
			m.base = p_tick - next;
		}
		core::Status st = m.natives->apply(next, m.runtime->effects(), *m.runtime);
		if (st.ok()) {
			st = m.natives->tick_round(next, *m.runtime);
		}
		if (st.ok()) {
			core::Result<lg::LogicTickStats> stepped = m.runtime->step();
			if (!stepped) {
				st = stepped.error();
			} else if (m.runtime->faulted() && m.runtime->fault() != nullptr) {
				st = *m.runtime->fault();
			}
		}
		if (!st.ok()) {
			m.fault(st.error());
		} else {
			for (lg::LogicNetworkedEvent e : m.runtime->networked()) {
				if (m.networked.size() >= net::kMaxLogicEventsPerMessage) {
					++m.stats.networked_dropped;
					continue;
				}
				e.tick += m.base;
				m.networked.push_back(e);
			}
		}
	}

	if (m.natives) {
		const lg::RoundPhase phase = m.natives->phase();
		if (phase != m.last_phase) {
			if (phase == lg::RoundPhase::InRound) {
				m.state.round_start_tick = p_tick;
			} else if (phase == lg::RoundPhase::RoundOver) {
				m.state.round_end_tick = p_tick;
				++m.stats.rounds;
			}
			m.last_phase = phase;
		}
		if (m.natives->take_unspawn_all()) {
			m.state.unspawn_tick = p_tick;
			m.state.restart_tick = p_tick + ticks_of(m.eor->restart_delay_s);
			for (size_t i = 0; i < m.slots.size(); ++i) {
				Impl::Slot &s = m.slots[i];
				if (s.connected && i < net::kMaxPlayerRows) {
					r_orders.push_back(RulesOrder{ RulesOrder::Kind::Unspawn, uint8_t(i) });
					s.alive = false;
					s.pending_spawn = false;
					s.corpse_valid = false;
					++m.stats.unspawns;
				}
			}
			m.downed.clear();
		}
		if (m.natives->take_restart()) {
			m.restart_round(p_tick, r_orders);
		}
	}

	for (size_t i = 0; i < n; ++i) {
		Impl::Slot &s = m.slots[i];
		sim::SoldierState &st = r_states[i];
		if (!s.connected || !state_alive(st)) {
			continue;
		}
		if (st.damage > 0.0f) {
			s.last_damage = p_tick;
			s.damaged = true;
		}
		if (st.health < m.max_health && p_tick >= s.last_damage + m.regen_delay_ticks) {
			st.health = std::min(m.max_health, st.health + m.regen_per_tick);
			++m.stats.regen_ticks;
		}
	}

	std::erase_if(m.downed, [&](const DownedSoldier &d) { return p_tick > d.until_tick; });

	const uint16_t defib = m.gunplay->kits.defib_def;
	if (m.config.revive && defib != sim::weapons::kNoWeapon) {
		for (const sim::weapons::ShotEvent &e : p_gunplay.interval_shots()) {
			if (e.def != defib || e.player_slot >= n) {
				continue;
			}
			Impl::Slot &shooter = m.slots[e.player_slot];
			if (int64_t(e.shot_index) <= shooter.shot_cursor) {
				continue;
			}
			shooter.shot_cursor = int64_t(e.shot_index);
			++m.stats.defib_shots;
			const uint8_t team = p_gunplay.team(uint8_t(e.player_slot));
			if (!shooter.connected || !state_alive(r_states[e.player_slot]) || !finite(e.origin) ||
					!finite(e.direction)) {
				++m.stats.defib_refused;
				continue;
			}
			const float range = m.tuning->defib_range_m;
			float best_t = range + 1.0f;
			int best = -1;
			bool refused = false;
			for (size_t i = 0; i < n; ++i) {
				if (i == e.player_slot) {
					continue;
				}
				const Impl::Slot &o = m.slots[i];
				float t = 0;
				if (o.connected && state_alive(r_states[i])) {
					const sim::Vec3 c{ r_states[i].position[0], r_states[i].position[1] + 0.9f, r_states[i].position[2] };
					if (segment_hits_sphere(e.origin, e.direction, range, c, m.tuning->living_radius_m, t)) {
						refused = true;
					}
					continue;
				}
				if (!o.connected || !o.corpse_valid) {
					continue;
				}
				const sim::Vec3 c{ o.corpse[0], o.corpse[1] + m.tuning->corpse_up_m, o.corpse[2] };
				if (!segment_hits_sphere(e.origin, e.direction, range, c, m.tuning->corpse_radius_m, t)) {
					continue;
				}
				const sim::Vec3 delta{ c[0] - e.origin[0], c[1] - e.origin[1], c[2] - e.origin[2] };
				if (p_view.world().ray(e.origin, delta, sim::query_mask::kProjectile)) {
					continue;
				}
				if (!m.revivable(uint8_t(i), team, p_tick)) {
					refused = true;
					continue;
				}
				if (t < best_t) {
					best_t = t;
					best = int(i);
				}
			}
			if (best < 0) {
				++(refused ? m.stats.defib_refused : m.stats.defib_misses);
				continue;
			}
			m.revive_order(uint8_t(best), uint8_t(e.player_slot), r_orders);
		}
	}
	for (const uint8_t slot : std::exchange(m.revive_requests, {})) {
		if (slot < n && m.slots[slot].connected && !state_alive(r_states[slot]) && m.slots[slot].corpse_valid &&
				m.revivable(slot, m.team_of(slot), p_tick)) {
			m.revive_order(slot, slot, r_orders);
		}
	}

	if (m.natives && !m.faulted) {
		m.apply_switches(std::span<const sim::SoldierState>(r_states.data(), n), r_orders);
	}

	const lg::RoundPhase phase = m.natives ? m.natives->phase() : lg::RoundPhase::Restarting;
	for (size_t i = 0; i < m.slots.size(); ++i) {
		Impl::Slot &s = m.slots[i];
		if (s.connected && s.pending_spawn) {
			const bool open = phase == lg::RoundPhase::PreRound || phase == lg::RoundPhase::InRound;
			if (open && s.spawn_mate == kNoSquadMate) {
				r_orders.push_back(RulesOrder{ RulesOrder::Kind::Spawn, uint8_t(i) });
			} else if (open &&
					m.mate_ready(uint8_t(i), s.spawn_mate, std::span<const sim::SoldierState>(r_states.data(), n))) {
				const sim::SoldierState &mate = r_states[size_t(s.spawn_mate)];
				RulesOrder o;
				o.kind = RulesOrder::Kind::SpawnAt;
				o.slot = uint8_t(i);
				o.position = m.squad_spawn_point(mate.position);
				o.yaw = mate.yaw;
				r_orders.push_back(o);
				m.slots[size_t(s.spawn_mate)].score += m.tuning->score_spawn_squad;
				++m.stats.squad_spawns;
			} else if (open) {
				++m.stats.refused[size_t(SpawnRefusal::SquadMate)];
			}
			s.pending_spawn = false;
			s.spawn_mate = kNoSquadMate;
		}
	}

	if (m.natives) {
		const lg::RoundNatives &nat = *m.natives;
		net::MatchState &ms = m.state;
		ms.phase = phase == lg::RoundPhase::PreRound ? net::MatchPhase::PreRound
				: phase == lg::RoundPhase::InRound	 ? net::MatchPhase::InRound
				: phase == lg::RoundPhase::RoundOver ? net::MatchPhase::RoundOver
													 : net::MatchPhase::Restarting;
		ms.players_needed = uint16_t(std::min<uint32_t>(nat.players_needed(), UINT16_MAX));
		const sim::Tick end = nat.countdown_end_tick();
		ms.countdown_end_tick = phase == lg::RoundPhase::PreRound && end != 0 ? m.base + end : 0;
		ms.kills = { nat.kills(1), nat.kills(2) };
		ms.kill_limit = nat.kill_limit();
		ms.winner = nat.winner();
		ms.killer_camera = nat.killer_camera();
		ms.restrict_mask = nat.restriction_mask();
		if (nat.input_restricted()) {

			ms.restrict_until_tick = phase == lg::RoundPhase::PreRound && end != 0 ? m.base + end + 2u : UINT32_MAX;
			m.was_restricted = true;
		} else if (m.was_restricted) {
			if (ms.restrict_until_tick == UINT32_MAX) {
				ms.restrict_until_tick = p_tick + 1u;
			}
			m.was_restricted = false;
		}
	}
	m.rows.clear();
	for (size_t i = 0; i < m.slots.size() && i < net::kMaxPlayerRows; ++i) {
		const Impl::Slot &s = m.slots[i];
		if (!s.connected) {
			continue;
		}
		m.squads.follow_team(uint8_t(i), m.team_of(i));
		net::PlayerRow r;
		r.slot = uint8_t(i);
		r.team = m.team_of(i);
		r.kit = s.row_kit;
		r.bot = s.bot;
		r.alive = i < n && s.alive && state_alive(r_states[i]);
		r.name_bytes = s.name_bytes;
		r.kills = s.kills;
		r.deaths = s.deaths;
		r.score = s.score;
		r.name = s.name;
		r.squad = m.squads.squad_of(uint8_t(i));
		r.leader = r.squad != 0 && m.squads.leader(uint8_t(i));
		m.rows.push_back(r);
	}
	for (size_t i = 0; i < n && i < m.last_states.size(); ++i) {
		m.last_states[i] = r_states[i];
	}
	return core::Status::success();
}

size_t TdmRules::choose_spawn(uint8_t p_slot, std::span<const sim::SpawnPoint> p_points, const ServerView &p_view, uint32_t p_tick) const {
	const Impl &m = *impl_;
	const uint8_t team = m.team_of(p_slot);
	const std::span<const sim::SoldierState> states = p_view.states();
	std::vector<sim::Vec3> enemies, eyes;
	for (size_t i = 0; i < states.size() && i < m.slots.size(); ++i) {
		if (i == p_slot || !p_view.alive(uint8_t(i))) {
			continue;
		}
		const uint8_t t = p_view.team(uint8_t(i));
		if (t == 0 || t == team) {
			continue;
		}
		const sim::SoldierState &st = states[i];
		enemies.push_back(st.position);
		const size_t pose = size_t(st.pose) < sim::kPoseCount ? size_t(st.pose) : 0;
		const float eye = m.motor.poses[pose].eye_height > 0.0f ? m.motor.poses[pose].eye_height : 1.6f;
		eyes.push_back({ st.position[0], st.position[1] + eye, st.position[2] });
	}
	sim::SpawnQuery q;
	q.points = p_points;
	q.team = team;
	q.enemies = enemies;
	q.enemy_eyes = eyes;
	q.recent_deaths = m.death_ring;
	q.protection_radius = 10.0f;
	q.friendly_block_radius = m.tuning->friendly_block_radius_m;
	q.friendly_block_count = m.tuning->friendly_block_count;
	q.friendly_block_ticks = ticks_of(m.tuning->friendly_block_s);
	q.tick = p_tick;
	q.seed = sim::weapons::splitmix64(m.match_seed ^ uint64_t(p_slot) ^ (uint64_t(p_tick) << 8));
	q.max_los_rays = m.tuning->spawn_los_rays;
	q.world = &p_view.world();
	q.safe_distance = m.tuning->spawn_safe_distance_m;
	q.chest_height = m.tuning->spawn_chest_height_m;
	const size_t at = sim::choose_spawn_scored(q);
	if (at == SIZE_MAX || at >= p_points.size()) {
		++m.stats.spawn_fallbacks;
		return SIZE_MAX;
	}
	++m.stats.spawns_scored;
	for (const sim::Vec3 &e : enemies) {
		if (dist2(e, p_points[at].position) < 100.0f) {
			++m.stats.spawns_enemy_within_10m;
			break;
		}
	}
	return at;
}

sim::Command TdmRules::apply_input_restriction(const sim::Command &p_command) const {
	const Impl &m = *impl_;
	const sim::Command c = net::restrict_command(p_command, m.state.restrict_until_tick, m.state.restrict_mask);
	if (!(c == p_command)) {
		++m.stats.restricted_commands;
	}
	return c;
}

std::span<const DownedSoldier> TdmRules::downed() const {
	return impl_->downed;
}

net::MatchPhase TdmRules::phase() const {
	return impl_->state.phase;
}

bool TdmRules::input_restricted() const {
	const Impl &m = *impl_;
	return m.state.restrict_until_tick != 0 && m.tick + 1u < m.state.restrict_until_tick;
}

const net::MatchState &TdmRules::match_state() const {
	return impl_->state;
}

std::span<const net::PlayerRow> TdmRules::players() const {
	return impl_->rows;
}

bool TdmRules::match_state_changed() const {
	const Impl &m = *impl_;
	return !m.state_sent || !same_state(m.state, m.sent_state);
}

bool TdmRules::players_changed() const {
	const Impl &m = *impl_;
	if (m.rows.size() != m.sent_rows.size()) {
		return true;
	}
	for (size_t i = 0; i < m.rows.size(); ++i) {
		if (!same_row(m.rows[i], m.sent_rows[i])) {
			return true;
		}
	}
	return false;
}

void TdmRules::mark_sent() {
	Impl &m = *impl_;
	m.sent_state = m.state;
	m.state_sent = true;
	m.sent_rows = m.rows;
}

void TdmRules::mark_state_sent() {
	Impl &m = *impl_;
	m.sent_state = m.state;
	m.state_sent = true;
}

void TdmRules::mark_players_sent() {
	impl_->sent_rows = impl_->rows;
}

net::PlayerList TdmRules::player_list(sim::Tick p_server_tick, bool p_full) const {
	const Impl &m = *impl_;
	net::PlayerList l;
	l.server_tick = p_server_tick;
	l.full = p_full;
	if (p_full) {
		l.rows = m.rows;
		return l;
	}
	size_t j = 0;
	for (const net::PlayerRow &r : m.rows) {
		while (j < m.sent_rows.size() && m.sent_rows[j].slot < r.slot) {
			l.removed.push_back(m.sent_rows[j].slot);
			++j;
		}
		if (j < m.sent_rows.size() && m.sent_rows[j].slot == r.slot) {
			if (!same_row(m.sent_rows[j], r)) {
				l.rows.push_back(r);
			}
			++j;
		} else {
			l.rows.push_back(r);
		}
	}
	for (; j < m.sent_rows.size(); ++j) {
		l.removed.push_back(m.sent_rows[j].slot);
	}
	return l;
}

const SquadTable &TdmRules::squads() const {
	return impl_->squads;
}

uint8_t TdmRules::team_of(uint8_t p_slot) const {
	return impl_->team_of(p_slot);
}

uint8_t TdmRules::kit_of(uint8_t p_slot) const {
	const Impl &m = *impl_;
	if (p_slot >= m.slots.size() || !m.slots[p_slot].connected) {
		return kKeepKit;
	}
	const Impl::Slot &s = m.slots[p_slot];
	if (s.bot || s.deploy) {
		return s.kit;
	}
	return kKeepKit;
}

const sim::logic::LogicTrace &TdmRules::trace() const {
	static const sim::logic::LogicTrace s_empty;
	return impl_->runtime ? impl_->runtime->trace() : s_empty;
}

const RulesStats &TdmRules::stats() const {
	const Impl &m = *impl_;
	m.stats.balance_moves = m.natives ? m.natives->balance_moves() : 0;
	return m.stats;
}

bool TdmRules::faulted() const {
	return impl_->faulted;
}

const sim::logic::RoundNatives &TdmRules::natives() const {
	return *impl_->natives;
}

std::span<const sim::logic::LogicNetworkedEvent> TdmRules::networked() const {
	return impl_->networked;
}

std::optional<sim::logic::LogicGhost> TdmRules::ghost() const {
	const Impl &m = *impl_;
	if (m.faulted || !m.runtime) {
		return std::nullopt;
	}
	sim::logic::LogicGhost g = m.runtime->ghost();
	g.tick += m.base;
	for (sim::Tick &start : g.sequence_start) {
		if (start != sim::logic::LogicGhost::kNoStart) {
			start += m.base;
		}
	}
	return g;
}
}
