// ModeDef reader: a LogicWorld over the level's included sub-worlds, then the gameplay entities in
// it with their effective values (research/topics/02-soldier-kits-modes.md §3.1-§3.12). The walk,
// every query and every loop here draw from one WorkBudget (cost model: logic_world.cpp).
#include "bf3/core/gameplay/mode_def.h"

#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/ids.h"
#include "gameplay_read.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bf3::core::gameplay {

using namespace detail;

namespace {

struct ModeAlias {
	const char *alias;
	const char *id;
};

constexpr ModeAlias kModeAliases[] = {
	{ "CQL", "ConquestLarge0" },
	{ "CQS", "ConquestSmall0" },
	{ "CQAL", "ConquestAssaultLarge0" },
	{ "CQAS", "ConquestAssaultSmall0" },
	{ "RL", "RushLarge0" },
	{ "SR", "SquadRush0" },
	{ "SQR", "SquadRush0" },
	{ "TDM", "TeamDeathMatch0" },
	{ "TDMC", "TeamDeathMatchC0" },
	{ "SDM", "SquadDeathMatch0" },
	{ "SQDM", "SquadDeathMatch0" },
	{ "DOM", "Domination0" },
	{ "GM", "GunMaster0" },
	{ "TS", "TankSuperiority0" },
	{ "SCV", "Scavenger0" },
	{ "CTF", "CaptureTheFlag0" },
	{ "AS", "AirSuperiority0" },
};

// Interface constants a mode may list in all (retail modes list under 100). Every Fields entry
// read becomes one, so the cap also bounds the scan when sub-world scopes repeat a large array.
constexpr size_t kMaxInterfaceConstants = 65536;

// Interface field names worth printing by name (their ids are ebx_name_hash of the name).
constexpr const char *kKnownInterfaceNames[] = {
	"InitialTicketCount",
	"AreaValue",
	"BaseCount",
	"SpawnDelay",
	"InitialSpawnDelay",
	"CaptureRadius",
	"LocationTextSid",
	"LocationNameSid",
	"Excluded",
	"Enabled",
	"TimeToGetControl",
	"TimeToLoseControl",
	"HoldToInteractTime",
	"StartingLifeCount",
	"MaxKillCount",
	"SpawnAlwaysAllowed",
	"Team",
	"TeamId",
	"InitialOwnerTeam",
	"Id",
	"Transform",
	"BlueprintTransform",
};

double eff_number(const LogicWorld &p_world, const LogicObject &p_object, std::string_view p_field, double p_fallback) {
	const std::optional<PropertyValue> v = p_world.effective(p_object, p_field);
	const std::optional<double> n = v ? v->as_f64() : std::nullopt;
	return n.value_or(p_fallback);
}

std::optional<double> eff_opt(const LogicWorld &p_world, uint32_t p_scope, const EbxInstance &p_entity, std::string_view p_field) {
	const std::optional<PropertyValue> v = p_world.effective(p_scope, p_entity, p_field);
	return v ? v->as_f64() : std::nullopt;
}

int64_t eff_int(const LogicWorld &p_world, const LogicObject &p_object, std::string_view p_field, int64_t p_fallback) {
	const std::optional<PropertyValue> v = p_world.effective(p_object, p_field);
	if (!v || (v->kind == PropertyValue::Kind::Field && v->field.kind() == EbxValue::Kind::Enum)) {
		return p_fallback;
	}
	if (const std::optional<int64_t> i = v->as_i64()) {
		return *i;
	}
	// A float field or constant: rounded, when it fits.
	if (const std::optional<double> f = v->as_f64(); f && std::isfinite(*f) && std::fabs(*f) < 9.0e18) {
		return static_cast<int64_t>(std::llround(*f));
	}
	return p_fallback;
}

bool eff_bool(const LogicWorld &p_world, const LogicObject &p_object, std::string_view p_field, bool p_fallback) {
	const std::optional<PropertyValue> v = p_world.effective(p_object, p_field);
	const std::optional<bool> b = v ? v->as_bool() : std::nullopt;
	return b.value_or(p_fallback);
}

int eff_team(const LogicWorld &p_world, const LogicObject &p_object, std::string_view p_field) {
	const std::optional<PropertyValue> v = p_world.effective(p_object, p_field);
	const std::optional<int> t = v ? v->as_team() : std::nullopt;
	return t.value_or(0);
}

std::string eff_enum(const LogicWorld &p_world, const LogicObject &p_object, std::string_view p_field) {
	const std::optional<PropertyValue> v = p_world.effective(p_object, p_field);
	if (!v) {
		return {};
	}
	if (const std::optional<std::string_view> e = v->enum_name()) {
		return copy_text(p_world.budget(), *e);
	}
	if (const std::optional<std::string_view> s = v->as_str()) {
		return copy_text(p_world.budget(), *s);
	}
	return {};
}

// A usable string id: designer placeholders such as "TEAM 1 Untitled" are not ids.
bool usable_sid(std::string_view p_text) {
	return !p_text.empty() && p_text.find(' ') == std::string_view::npos;
}

// LocationTextSid from the nearest enclosing interface that sets one.
std::string location_sid(const LogicWorld &p_world, uint32_t p_scope) {
	std::optional<uint32_t> at = p_scope;
	for (size_t guard = 0; at && guard < 64; ++guard) {
		const std::optional<PropertyValue> v = p_world.interface_value(*at, field_id("LocationTextSid"));
		const std::optional<std::string_view> s = v ? v->as_str() : std::nullopt;
		if (s && usable_sid(*s)) {
			return copy_text(p_world.budget(), *s);
		}
		at = p_world.scopes()[*at].parent;
	}
	return {};
}

std::string scope_prefab(const LogicWorld &p_world, uint32_t p_scope) {
	return copy_text(p_world.budget(), p_world.scope_name(p_scope));
}

// The fuse of a Rush crate prefab scope (see the header). Every crate scope of one prefab shares
// its connection set, so each pass over it is charged again.
Status read_fuse(const LogicWorld &p_world, uint32_t p_scope, McomDef &r_mcom) {
	WorkBudget &budget = p_world.budget();
	const LogicScope &scope = p_world.scopes()[p_scope];
	BF3_TRY(budget.charge_type(scope.blueprint.data())); // crate scopes may share one blueprint
	const std::optional<EbxInstance> descriptor = p_world.db().follow(scope.blueprint.data(), "Descriptor");
	if (!descriptor) {
		return Status::success();
	}
	BF3_TRY(budget.charge_type(descriptor->data()));
	const LogicConnections &set = p_world.connections(p_scope);
	// Event connections by source, so each step below only looks at one entity's outputs.
	BF3_TRY(budget.charge(set.event.size()));
	std::unordered_map<InstanceKey, std::vector<uint32_t>, InstanceKeyHash> events_from;
	for (uint32_t i = 0; i < set.event.size(); ++i) {
		events_from[InstanceKey::of(set.event[i].source)].push_back(i);
	}
	const auto from = [&](const EbxInstance &p_source) -> const std::vector<uint32_t> & {
		static const std::vector<uint32_t> none;
		const auto it = events_from.find(InstanceKey::of(p_source));
		return it == events_from.end() ? none : it->second;
	};
	std::array<std::unordered_map<InstanceKey, std::optional<double>, InstanceKeyHash>, 2> fuse_after; // by SyncedBool value
	const int32_t in = field_id("In");
	BF3_TRY(budget.charge(set.property.size()));
	for (const LogicConnection &feed : set.property) {
		if (feed.source != *descriptor || feed.target_field != in || !feed.target.is_a("SyncedBoolEntityData")) {
			continue;
		}
		const EbxInstance synced = feed.target;
		bool value = false;
		if (const std::optional<PropertyValue> v = p_world.interface_value(p_scope, feed.source_field); v && v->as_bool()) {
			value = *v->as_bool();
		} else {
			BF3_TRY(budget.charge_type(synced.data())); // many feeds may name one SyncedBool
			if (const std::optional<EbxValue> stored = synced.get("In")) {
				value = stored->as_bool().value_or(false);
			}
		}
		// Several feeds may drive the same SyncedBool: each (SyncedBool, value) is followed once.
		const auto [memo, added] = fuse_after[value ? 1 : 0].emplace(InstanceKey::of(synced), std::nullopt);
		if (!added) {
			if (memo->second) {
				r_mcom.fuse = memo->second;
				r_mcom.long_fuse_switch = value;
				return Status::success();
			}
			continue;
		}
		const int32_t fired = value ? field_id("OnTrue") : field_id("OnFalse");
		// The gates the SyncedBool drives, in first-seen order, with their state after it fires:
		// Default, then its Open/Close inputs from the fired output in connection order.
		std::vector<EbxInstance> gates;
		std::unordered_map<InstanceKey, bool, InstanceKeyHash> gate_open;
		BF3_TRY(budget.charge(from(synced).size()));
		for (const uint32_t edge_at : from(synced)) {
			const LogicConnection &edge = set.event[edge_at];
			if (!edge.target.is_a("EventGateEntityData")) {
				continue;
			}
			const EbxInstance &gate = edge.target;
			auto [it, first] = gate_open.emplace(InstanceKey::of(gate), false);
			if (first) {
				BF3_TRY(budget.charge_type(gate.data()));
				it->second = gate.get("Default") ? gate.get("Default")->as_bool().value_or(false) : false;
				gates.push_back(gate);
			}
			if (edge.source_field == fired) {
				if (edge.target_field == field_id("Open")) {
					it->second = true;
				} else if (edge.target_field == field_id("Close")) {
					it->second = false;
				}
			}
		}
		std::optional<double> fuse;
		for (const EbxInstance &gate : gates) {
			if (!gate_open[InstanceKey::of(gate)]) {
				continue;
			}
			BF3_TRY(budget.charge(from(gate).size()));
			for (const uint32_t out_at : from(gate)) {
				const LogicConnection &out = set.event[out_at];
				if (out.source_field == field_id("Out") && out.target.is_a("DelayTriggerEntityData") &&
						out.target_field == field_id("Trigger")) {
					if (const std::optional<double> delay = eff_opt(p_world, p_scope, out.target, "Delay")) {
						fuse = delay;
						break;
					}
				}
			}
			if (fuse) {
				break;
			}
		}
		memo->second = fuse;
		if (fuse) {
			r_mcom.fuse = fuse;
			r_mcom.long_fuse_switch = value;
			return Status::success();
		}
	}
	return Status::success();
}

// Every array is charged before its scan; nullopt once the budget is spent (the caller checks it).
std::optional<GameModeSizeDef> read_size(const EbxResolver &p_db, const std::string &p_level, std::string_view p_mode, WorkBudget &r_budget) {
	const EbxPartition *partition = p_db.partition_by_name("levels/" + p_level + "/" + p_level + "/settings_win32");
	if (!partition || !r_budget.take(partition->instance_count())) {
		return std::nullopt;
	}
	std::string_view stem = p_mode;
	while (!stem.empty() && stem.back() >= '0' && stem.back() <= '9') {
		stem.remove_suffix(1);
	}
	const auto read = [&](const EbxStruct &p_size, bool p_fallback) {
		GameModeSizeDef out;
		out.name = text(r_budget, p_size, "Name");
		out.short_name = text(r_budget, p_size, "ShortName");
		out.fallback = p_fallback;
		out.player_count = static_cast<int64_t>(number_or(p_size, "PlayerCount", 0));
		out.rounds_per_map = static_cast<int64_t>(number_or(p_size, "RoundsPerMap", 0));
		out.force_squad = boolean(p_size, "ForceSquad").value_or(false);
		if (const OptionalEbxArray array = array_of(r_budget, p_size, "Teams")) {
			for (const EbxValue t : *array) {
				if (const std::optional<EbxStruct> s = t.as_struct()) {
					out.teams.push_back({ static_cast<int64_t>(number_or(*s, "PlayerCount", 0)), static_cast<int64_t>(number_or(*s, "SquadSize", 0)) });
				}
			}
		}
		return out;
	};
	for (const EbxInstance &settings : partition->instances_of("GameModeSettings")) {
		if (!r_budget.take_type(settings.data())) {
			return std::nullopt;
		}
		const OptionalEbxArray infos = array_of(r_budget, settings.data(), "Information");
		if (!infos) {
			continue;
		}
		for (const EbxValue info_value : *infos) {
			const std::optional<EbxStruct> info = info_value.as_struct();
			if (!info || text_view(*info, "Platform") != "GamePlatform_Win32") {
				continue;
			}
			if (const OptionalEbxArray array = array_of(r_budget, *info, "Sizes")) {
				for (const EbxValue size_value : *array) {
					const std::optional<EbxStruct> size = size_value.as_struct();
					if (size && (iequals_ascii(text_view(*size, "Name"), stem) || iequals_ascii(text_view(*size, "MetaIdentifier"), stem))) {
						return read(*size, false);
					}
				}
			}
			if (const std::optional<EbxValue> fallback = info->get("DefaultSize"); fallback && fallback->as_struct()) {
				return read(*fallback->as_struct(), true);
			}
		}
	}
	return std::nullopt;
}

} // namespace

int64_t GameModeSizeDef::squad_size() const {
	for (const GameModeTeamSizeDef &t : teams) {
		if (t.squad_size > 0) {
			return t.squad_size;
		}
	}
	return 0;
}

const TicketCounterDef *ModeDef::tickets(int p_team) const {
	for (const TicketCounterDef &t : ticket_counters) {
		if (t.team == p_team) {
			return &t;
		}
	}
	return nullptr;
}

const LifeCounterDef *ModeDef::lives(int p_team) const {
	for (const LifeCounterDef &l : life_counters) {
		if (l.team == p_team) {
			return &l;
		}
	}
	return nullptr;
}

const KillCounterDef *ModeDef::kills(int p_team) const {
	for (const KillCounterDef &k : kill_counters) {
		if (k.team == p_team) {
			return &k;
		}
	}
	return nullptr;
}

const TeamDef *ModeDef::team(int p_team) const {
	for (const TeamDef &t : teams) {
		if (t.team == p_team) {
			return &t;
		}
	}
	return nullptr;
}

size_t ModeDef::flag_count() const {
	return static_cast<size_t>(std::count_if(capture_points.begin(), capture_points.end(), [](const CapturePointDef &p) { return !p.is_hq(); }));
}

std::optional<double> ModeDef::flag_area_value() const {
	std::optional<double> out;
	for (const CapturePointDef &p : capture_points) {
		if (p.is_hq()) {
			continue;
		}
		if (out && *out != p.area_value) {
			return std::nullopt;
		}
		out = p.area_value;
	}
	return out;
}

std::string canonical_mode_id(std::string_view p_mode) {
	for (const ModeAlias &a : kModeAliases) {
		if (iequals_ascii(p_mode, a.alias)) {
			return a.id;
		}
	}
	return std::string(p_mode);
}

Result<std::vector<std::string>> level_modes(const EbxResolver &p_db, std::string_view p_level_id, WorkBudget *p_budget) {
	WorkBudget own;
	WorkBudget &budget = p_budget != nullptr ? *p_budget : own;
	std::vector<std::string> out;
	const std::string id = to_lower_ascii(p_level_id);
	const EbxPartition *partition = p_db.partition_by_name("levels/" + id + "/" + id + "/description_win32");
	if (!partition) {
		return out;
	}
	BF3_TRY(budget.charge_type(partition->primary().data()));
	if (const OptionalEbxArray array = array_of(budget, partition->primary().data(), "Categories")) {
		for (const EbxValue category_value : *array) {
			const std::optional<EbxStruct> category = category_value.as_struct();
			if (!category || !iequals_ascii(text_view(*category, "Category"), "GameMode")) {
				continue;
			}
			if (const OptionalEbxArray list = array_of(budget, category->get("Mode"))) {
				for (const EbxValue m : *list) {
					if (const std::optional<std::string_view> s = m.as_str()) {
						out.push_back(copy_text(budget, *s));
					}
				}
			}
		}
	}
	BF3_TRY(budget.status());
	return out;
}

Result<ModeDef> read_mode_def(const EbxResolver &p_db, std::string_view p_level_id, std::string_view p_mode, WorkBudget *p_budget) {
	WorkBudget own;
	WorkBudget &budget = p_budget != nullptr ? *p_budget : own;
	const std::string id = to_lower_ascii(p_level_id);
	std::string mode = canonical_mode_id(p_mode);
	Result<std::vector<std::string>> listed_modes = level_modes(p_db, id, &budget);
	if (!listed_modes) {
		return error_at("mode " + id, listed_modes.error());
	}
	for (const std::string &listed : *listed_modes) {
		if (iequals_ascii(listed, mode)) {
			mode = listed; // the spelling the data uses
		}
	}
	const std::string root_name = "levels/" + id + "/" + id;
	const EbxPartition *root = p_db.partition_by_name(root_name);
	if (!root) {
		return make_error("mode: no level partition `" + root_name + "`");
	}
	if (!root->primary().is_a("LevelData")) {
		return make_error("mode: `" + root_name + "` is a " + std::string(root->primary().type_name()) + ", not LevelData");
	}
	LogicWorldOptions options;
	options.mode = mode;
	options.budget = &budget;
	Result<LogicWorld> built = LogicWorld::build(p_db, root->primary(), options);
	if (!built) {
		return error_at("mode " + id + " " + mode, built.error());
	}
	const LogicWorld &world = *built;

	ModeDef out;
	out.level_id = std::string(p_level_id);
	BF3_TRY(budget.charge_type(root->primary().data()));
	if (const std::optional<EbxValue> name = root->primary().get("Name"); name && name->as_str()) {
		const std::string_view n = *name->as_str();
		const size_t slash = n.find_last_of("/\\");
		out.level_id = std::string(slash == std::string_view::npos ? n : n.substr(slash + 1));
	}
	out.mode_id = mode;
	out.subworlds = world.stats().subworlds;
	if (out.subworlds.empty()) {
		return make_error("mode: level " + id + " includes no sub-world for mode `" + mode + "`");
	}
	out.scopes = world.scopes().size();
	out.objects = world.objects().size();
	out.walk_steps = world.stats().walk_steps;
	BF3_TRY(budget.charge(world.scopes().size()));
	for (uint32_t s = 0; s < world.scopes().size(); ++s) {
		out.unresolved_connections += world.connections(s).unresolved;
	}

	// The sub-worlds' own interface constants.
	for (uint32_t s = 1; s < world.scopes().size(); ++s) {
		BF3_TRY(budget.charge(1));
		const LogicScope &scope = world.scopes()[s];
		if (!scope.reference || !scope.reference->is_a("SubWorldReferenceObjectData")) {
			continue;
		}
		BF3_TRY(budget.charge_type(scope.blueprint.data())); // scopes may share one blueprint
		const std::optional<EbxInstance> descriptor = p_db.follow(scope.blueprint.data(), "Descriptor");
		if (!descriptor) {
			continue;
		}
		BF3_TRY(budget.charge_type(descriptor->data()));
		const std::optional<EbxValue> fields = descriptor->get("Fields");
		const OptionalEbxArray array = fields ? fields->as_array() : OptionalEbxArray();
		if (!array) {
			continue;
		}
		if (array->size() > kMaxInterfaceConstants - out.interface_constants.size()) {
			return make_error("mode: level " + id + " lists more than " + std::to_string(kMaxInterfaceConstants) + " interface constants");
		}
		BF3_TRY(budget.charge(array_cost(*array)));
		for (const EbxValue field : *array) {
			InterfaceConstantDef c;
			c.subworld = copy_text(budget, world.scope_name(s));
			const std::optional<EbxValue> fid = field.get("Id");
			c.id = fid ? fid->as_i32().value_or(0) : 0;
			const std::optional<EbxValue> value = field.get("Value");
			c.value = copy_text(budget, value ? value->as_str().value_or(std::string_view()) : std::string_view());
			for (const char *known : kKnownInterfaceNames) {
				if (field_id(known) == c.id) {
					c.name = known;
					break;
				}
			}
			out.interface_constants.push_back(std::move(c));
		}
	}

	// MCOM crates: the scopes that hold IET_RushCrate* interactions, in first-seen order, with the
	// interactions of each (collected here so the crates need no second pass over every object).
	std::vector<std::pair<uint32_t, std::vector<const LogicObject *>>> crates;
	std::unordered_map<uint32_t, size_t> crate_at;
	for (const LogicObject &object : world.objects()) {
		BF3_TRY(budget.charge(1));
		BF3_TRY(budget.charge_type(object.entity.data()));
		const EbxInstance &e = object.entity;
		if (e.is_a("TeamEntityData")) {
			const std::optional<PropertyValue> team_value = world.effective(object, "Team");
			std::optional<EbxInstance> team_data;
			if (team_value && team_value->kind == PropertyValue::Kind::Ref) {
				team_data = team_value->ref;
			} else if (const std::optional<EbxRef> r = team_value ? team_value->as_ref() : std::nullopt) {
				team_data = p_db.resolve(e.partition(), *r);
			}
			if (!team_data) {
				continue;
			}
			Result<TeamDef> team = read_team_data(p_db, *team_data, &budget);
			if (!team) {
				continue;
			}
			team->team = eff_team(world, object, "Id");
			if (!out.team(team->team)) {
				out.teams.push_back(std::move(*team));
			}
		} else if (e.is_a("TicketCounterEntityData")) {
			TicketCounterDef t;
			t.team = eff_team(world, object, "TeamId");
			t.initial_tickets = eff_int(world, object, "InitialTicketCount", 0);
			t.loss_per_min = eff_number(world, object, "TicketLossPerMin", 0);
			t.loss_start = eff_number(world, object, "TicketLossStart", 0);
			t.loss_when_lost_all = eff_number(world, object, "TicketLossWhenLostAll", 0);
			t.loss_when_lost_all_control_points = eff_number(world, object, "TicketLossWhenLostAllControlsPoint", 0);
			t.halt_on_equal_point_count = eff_bool(world, object, "HaltTicketLossOnEqualPointCount", false);
			t.decrease_tickets = eff_enum(world, object, "DecreaseTickets");
			t.prefab = scope_prefab(world, object.scope);
			out.ticket_counters.push_back(std::move(t));
		} else if (e.is_a("CapturePointEntityData")) {
			CapturePointDef c;
			c.prefab = scope_prefab(world, object.scope);
			c.location_sid = location_sid(world, object.scope);
			c.capturable_type = eff_enum(world, object, "CapturableType");
			c.initial_owner = eff_team(world, object, "InitialOwnerTeam");
			c.area_value = eff_number(world, object, "AreaValue", 0);
			c.time_to_get_control = eff_number(world, object, "TimeToGetControl", 0);
			c.time_to_lose_control = eff_number(world, object, "TimeToLoseControl", 0);
			c.added_multiplier_per_player = eff_number(world, object, "AddedMultiplierPerPlayer", 0);
			c.max_capture_multiplier = eff_number(world, object, "MaxCaptureMultiplier", 0);
			c.return_multiplier = eff_number(world, object, "ReturnMultiplier", 0);
			c.capture_radius = eff_number(world, object, "CaptureRadius", 0);
			c.min_nr_to_take_control = eff_int(world, object, "MinNrToTakeControl", 0);
			c.enemy_ticket_loss_when_captured = eff_int(world, object, "EnemyTicketLossWhenCaptured", 0);
			c.seesaw = eff_bool(world, object, "SeesawCapturing", false);
			c.enabled = eff_bool(world, object, "Enabled", true);
			out.capture_points.push_back(std::move(c));
		} else if (e.is_a("LifeCounterEntityData")) {
			LifeCounterDef l;
			l.team = eff_team(world, object, "TeamId");
			l.attacker = eff_bool(world, object, "Attacker", false);
			l.starting_lives = eff_int(world, object, "StartingLifeCount", 0);
			l.base_count = eff_int(world, object, "BaseCount", 0);
			l.fallback_time = eff_number(world, object, "FallbackTime", 0);
			l.simple_reinforce = eff_bool(world, object, "SimpleReinforce", false);
			l.prefab = scope_prefab(world, object.scope);
			out.life_counters.push_back(std::move(l));
		} else if (e.is_a("KillCounterEntityData")) {
			KillCounterDef k;
			k.team = eff_team(world, object, "TeamId");
			k.max_kills = eff_int(world, object, "MaxKillCount", 0);
			k.enemy_weight = eff_number(world, object, "EnemyWeight", 0);
			k.type = copy_text(budget, e.type_name());
			k.prefab = scope_prefab(world, object.scope);
			out.kill_counters.push_back(std::move(k));
		} else if (e.is_a("GameInteractionEntityData")) {
			const std::string type = eff_enum(world, object, "InteractionEntityType");
			if (type == "IET_RushCrateArm" || type == "IET_RushCrateDisarm") {
				const auto [it, added] = crate_at.emplace(object.scope, crates.size());
				if (added) {
					crates.push_back({ object.scope, {} });
				}
				crates[it->second].second.push_back(&object);
			}
		} else if (e.is_a("VehicleSpawnReferenceObjectData")) {
			VehicleSpawnDef v;
			v.blueprint = ref_name(budget, p_db, e.data(), "Blueprint");
			v.team = eff_team(world, object, "Team");
			v.spawn_delay = eff_number(world, object, "SpawnDelay", 0);
			v.initial_spawn_delay = eff_number(world, object, "InitialSpawnDelay", 0);
			v.enabled = eff_bool(world, object, "Enabled", true);
			out.vehicle_spawns.push_back(std::move(v));
		}
	}
	for (const auto &[scope, interactions] : crates) {
		BF3_TRY(budget.charge(1 + interactions.size()));
		McomDef m;
		m.prefab = scope_prefab(world, scope);
		for (const LogicObject *crate_object : interactions) {
			const LogicObject &object = *crate_object;
			const std::string type = eff_enum(world, object, "InteractionEntityType");
			const std::optional<PropertyValue> hold = world.effective(object, "HoldToInteractTime");
			const std::optional<double> time = hold ? hold->as_f64() : std::nullopt;
			if (type == "IET_RushCrateArm") {
				m.arm_time = time;
				const std::optional<PropertyValue> radius = world.effective(object, "UseWithinRadius");
				m.radius = radius ? radius->as_f64() : std::nullopt;
			} else if (type == "IET_RushCrateDisarm") {
				m.disarm_time = time;
			}
		}
		BF3_TRY(read_fuse(world, scope, m));
		out.mcoms.push_back(std::move(m));
	}
	out.query_steps = world.query_steps();
	// A query or copy that ran out of budget answered nullopt / empty: fail instead.
	if (const Status spent = budget.status(); !spent) {
		return error_at("mode " + id + " " + mode, spent.error());
	}
	if (world.query_budget_spent()) {
		return make_error("mode " + id + " " + mode + ": property queries exceed " + std::to_string(options.max_query_steps) + " resolver steps");
	}
	// Data order follows the blueprint walk; present per-team lists by team number (stable).
	const auto by_team = [](const auto &p_a, const auto &p_b) { return p_a.team < p_b.team; };
	std::stable_sort(out.teams.begin(), out.teams.end(), by_team);
	std::stable_sort(out.ticket_counters.begin(), out.ticket_counters.end(), by_team);
	std::stable_sort(out.life_counters.begin(), out.life_counters.end(), by_team);
	std::stable_sort(out.kill_counters.begin(), out.kill_counters.end(), by_team);
	out.size = read_size(p_db, id, mode, budget);
	if (const Status spent = budget.status(); !spent) {
		return error_at("mode " + id + " " + mode, spent.error());
	}
	out.work = budget.used();
	return out;
}

std::optional<double> ScoringTable::score(std::string_view p_sid) const {
	for (const ScoringEntryDef &e : entries) {
		if (e.sid == p_sid) {
			return e.score;
		}
	}
	return std::nullopt;
}

Result<ScoringTable> read_scoring(const EbxResolver &p_db, std::string_view p_partition, WorkBudget *p_budget) {
	WorkBudget own;
	WorkBudget &budget = p_budget != nullptr ? *p_budget : own;
	const std::string name = to_lower_ascii(p_partition);
	const EbxPartition *partition = p_db.partition_by_name(name);
	if (!partition) {
		return make_error("scoring: no partition `" + name + "`");
	}
	ScoringTable out;
	BF3_TRY(budget.charge(partition->instance_count()));
	for (const EbxInstance &t : partition->instances_of("ScoringTypeData")) {
		BF3_TRY(budget.charge_type(t.data()));
		ScoringEntryDef e;
		e.sid = text(budget, t.data(), "DescriptionSid");
		e.score = number_or(t.data(), "Score", 0);
		e.measuring = text(budget, t.data(), "Measuring");
		e.bucket = text(budget, t.data(), "Bucket");
		out.entries.push_back(std::move(e));
	}
	if (const Status spent = budget.status(); !spent) {
		return error_at("scoring " + name, spent.error());
	}
	return out;
}

} // namespace bf3::core::gameplay
