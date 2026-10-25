#include "bf3/core/gameplay/eor_timeline.h"

#include "bf3/core/gameplay/logic_types.h"
#include "gameplay_read.h"

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core::gameplay {

using namespace detail;

namespace {

constexpr int32_t kEndOfRound = logic_port_id("EndOfRound");
constexpr int32_t kEndOfRoundComplete = logic_port_id("EndOfRoundComplete");
constexpr int32_t kFadeIn = logic_port_id("FadeIn");
constexpr int32_t kOnDone = logic_port_id("OnDone");
constexpr int32_t kRequestPreEndOfRound = logic_port_id("RequestPreEndOfRound");
constexpr int32_t kRequestEndOfRound = logic_port_id("RequestEndOfRound");
constexpr int32_t kOnPreEndOfRoundReady = logic_port_id("OnPreEndOfRoundReady");
constexpr int32_t kScoreboard = logic_port_id("Scoreboard");

struct Connection {
	Guid source{};
	int32_t source_event = 0;
	Guid target{};
	int32_t target_event = 0;
};

struct Prefab {
	std::map<Guid, EbxInstance> instances;
	std::vector<Connection> connections;
	Guid descriptor{};

	const EbxInstance *find(const Guid &p_guid) const {
		const auto it = instances.find(p_guid);
		return it == instances.end() ? nullptr : &it->second;
	}

	bool is(const Guid &p_guid, std::string_view p_type) const {
		const EbxInstance *i = find(p_guid);
		return i != nullptr && i->is_a(p_type);
	}

	std::vector<Connection> from(const Guid &p_source, int32_t p_event) const {
		std::vector<Connection> out;
		for (const Connection &c : connections) {
			if (c.source == p_source && c.source_event == p_event) {
				out.push_back(c);
			}
		}
		return out;
	}

	std::vector<Connection> into(const Guid &p_target, int32_t p_event) const {
		std::vector<Connection> out;
		for (const Connection &c : connections) {
			if (c.target == p_target && c.target_event == p_event) {
				out.push_back(c);
			}
		}
		return out;
	}
};

std::optional<Guid> ref_guid(
		const EbxResolver &p_db, const EbxPartition &p_partition, const std::optional<EbxValue> &p_value) {
	const std::optional<EbxRef> r = p_value ? p_value->as_ref() : std::nullopt;
	if (!r) {
		return std::nullopt;
	}
	const std::optional<EbxInstance> i = p_db.resolve(p_partition, *r);
	return i ? std::optional<Guid>(i->guid()) : std::nullopt;
}

std::optional<float> seconds_of(const EbxInstance &p_instance, std::string_view p_field) {
	const std::optional<double> v = number(p_instance.data(), p_field);
	if (!v || !std::isfinite(*v) || *v < 0.0 || *v > 3600.0) {
		return std::nullopt;
	}
	return float(*v);
}

std::optional<float> delay_between(const Prefab &p_prefab, const Guid &p_source, int32_t p_source_event,
		const Guid &p_target, int32_t p_target_event) {
	for (const Connection &c : p_prefab.from(p_source, p_source_event)) {
		if (!p_prefab.is(c.target, "DelayEntityData") || c.target_event != logic_port::kIn) {
			continue;
		}
		for (const Connection &out : p_prefab.from(c.target, logic_port::kOut)) {
			const bool direct = out.target == p_target && out.target_event == p_target_event;
			bool gated = false;
			if (p_prefab.is(out.target, "EventGateEntityData") && out.target_event == logic_port::kIn) {
				for (const Connection &g : p_prefab.from(out.target, logic_port::kOut)) {
					gated |= g.target == p_target && g.target_event == p_target_event;
				}
			}
			if (direct || gated) {
				return seconds_of(*p_prefab.find(c.target), "Delay");
			}
		}
	}
	return std::nullopt;
}

}

Result<EorTimeline> read_eor_timeline(const EbxResolver &p_db, WorkBudget *p_budget) {
	WorkBudget own;
	WorkBudget &budget = p_budget != nullptr ? *p_budget : own;
	const std::string where = "eor timeline " + std::string(kEndOfRoundLogicPartition);
	const EbxPartition *partition = p_db.partition_by_name(kEndOfRoundLogicPartition);
	if (partition == nullptr) {
		return make_error(where + ": partition not found");
	}
	BF3_TRY(budget.charge(partition->instance_count()));
	Prefab prefab;
	for (size_t i = 0; i < partition->instance_count(); ++i) {
		if (const std::optional<EbxInstance> instance = partition->instance(i)) {
			prefab.instances.emplace(instance->guid(), *instance);
		}
	}
	const EbxInstance blueprint = partition->primary();
	if (!blueprint.is_a("LogicPrefabBlueprint")) {
		return make_error(where + ": the primary instance is a " + std::string(blueprint.type_name()));
	}
	const std::optional<Guid> descriptor = ref_guid(p_db, *partition, blueprint.get("Descriptor"));
	if (!descriptor) {
		return make_error(where + ": no Descriptor");
	}
	prefab.descriptor = *descriptor;
	const OptionalEbxArray connections = array_of(budget, blueprint.data(), "EventConnections");
	if (!connections) {
		return make_error(where + ": no EventConnections");
	}
	for (const EbxValue c : *connections) {
		const std::optional<Guid> source = ref_guid(p_db, *partition, c.get_path("Source"));
		const std::optional<Guid> target = ref_guid(p_db, *partition, c.get_path("Target"));
		const std::optional<EbxValue> source_event = c.get_path("SourceEvent.Id");
		const std::optional<EbxValue> target_event = c.get_path("TargetEvent.Id");
		const std::optional<int64_t> se = source_event ? source_event->as_i64() : std::nullopt;
		const std::optional<int64_t> te = target_event ? target_event->as_i64() : std::nullopt;
		if (source && target && se && te) {
			prefab.connections.push_back({ *source, int32_t(*se), *target, int32_t(*te) });
		}
	}
	const std::vector<EbxInstance> eors = partition->instances_of("UIEndOfRoundEntityData");
	if (eors.size() != 1) {
		return make_error(where + ": " + std::to_string(eors.size()) + " UIEndOfRoundEntityData instances");
	}
	const EbxInstance &eor = eors.front();
	EorTimeline out;
	const std::optional<float> pre_eor = seconds_of(eor, "PreEorTime");
	const std::optional<float> eor_time = seconds_of(eor, "EorTime");
	if (!pre_eor || !eor_time) {
		return make_error(where + ": no PreEorTime or EorTime");
	}
	out.pre_eor_s = *pre_eor;
	out.eor_s = *eor_time;

	std::optional<float> fade;
	for (const Connection &c : prefab.from(prefab.descriptor, kEndOfRound)) {
		if (c.target_event != kFadeIn || !prefab.is(c.target, "FadeEntityData")) {
			continue;
		}
		for (const Connection &done : prefab.from(c.target, kOnDone)) {
			if (done.target == eor.guid() && done.target_event == kRequestPreEndOfRound) {
				fade = seconds_of(*prefab.find(c.target), "FadeTime");
			}
		}
	}
	if (!fade) {
		return make_error(where + ": no FadeEntity from EndOfRound to RequestPreEndOfRound");
	}
	out.fade_s = *fade;

	std::optional<float> restart;
	for (const Connection &c : prefab.into(prefab.descriptor, kEndOfRoundComplete)) {
		if (!prefab.is(c.source, "DelayEntityData") || c.source_event != logic_port::kOut) {
			continue;
		}
		for (const Connection &in : prefab.into(c.source, logic_port::kIn)) {
			if (in.source == prefab.descriptor && in.source_event == kEndOfRound) {
				restart = seconds_of(*prefab.find(c.source), "Delay");
			}
		}
	}
	if (!restart) {
		return make_error(where + ": no Delay from EndOfRound to EndOfRoundComplete");
	}
	out.restart_delay_s = *restart;

	std::optional<float> scoreboard;
	for (const Connection &c : prefab.connections) {
		if (c.target_event == kScoreboard && prefab.is(c.target, "UIGraphEntityData")) {
			scoreboard = delay_between(prefab, eor.guid(), kOnPreEndOfRoundReady, c.target, kScoreboard);
		}
	}
	if (!scoreboard) {
		return make_error(where + ": no Delay from OnPreEndOfRoundReady to a graph's Scoreboard");
	}
	out.scoreboard_delay_s = *scoreboard;
	const std::optional<float> request =
			delay_between(prefab, eor.guid(), kOnPreEndOfRoundReady, eor.guid(), kRequestEndOfRound);
	if (!request) {
		return make_error(where + ": no Delay from OnPreEndOfRoundReady to RequestEndOfRound");
	}
	out.request_eor_delay_s = *request;
	if (const Status spent = budget.status(); !spent) {
		return error_at(where, spent.error());
	}
	return out;
}

}
