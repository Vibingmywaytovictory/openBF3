#include "bf3/host/ai_level_config.h"

#include "bf3/ai/squad/squad_world.h"
#include "bf3/core/cache/cache_key.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <utility>

namespace bf3::host {

namespace {

inline constexpr size_t kDerivedNeighbours = 2;

}

std::vector<ai::StrategicAreaDef> derive_strategic_areas(const BotLevel &p_level) {
	const std::vector<BotStrategicArea> &areas = p_level.areas;
	std::vector<ai::StrategicAreaDef> out(areas.size());
	std::vector<std::vector<size_t>> neighbours(areas.size());
	for (size_t i = 0; i < areas.size(); ++i) {
		const BotStrategicArea &a = areas[i];
		ai::StrategicAreaDef &d = out[i];
		d.name = a.name;
		d.radius = a.radius;
		d.side = a.team;
		if (a.kind == BotStrategicArea::Kind::TdmCluster) {
			d.corner = { a.centre.x - a.radius, a.centre.z - a.radius };
			d.centre = { a.centre.x, a.centre.z };
			d.height = a.centre.y;
		} else {
			d.from_control_point = true;
			d.control_point = uint32_t(i);
		}
		std::vector<std::pair<float, size_t>> near;
		for (size_t j = 0; j < areas.size(); ++j) {
			if (j != i) {
				near.push_back({ ai::distance_sq(a.centre, areas[j].centre), j });
			}
		}
		std::sort(near.begin(), near.end());
		for (size_t k = 0; k < std::min(kDerivedNeighbours, near.size()); ++k) {
			const size_t j = near[k].second;
			if (std::find(neighbours[i].begin(), neighbours[i].end(), j) == neighbours[i].end()) {
				neighbours[i].push_back(j);
				neighbours[j].push_back(i);
			}
		}
	}
	for (size_t i = 0; i < areas.size(); ++i) {
		for (const size_t j : neighbours[i]) {
			out[i].neighbours.push_back(areas[j].name);
		}
	}
	return out;
}

core::Result<CachedAiAreas> cached_strategic_areas(
		core::Cache *p_cache, const core::Sha256Digest &p_install, std::string_view p_level, const BotLevel &p_bots) {
	const std::vector<ai::StrategicAreaDef> derived = derive_strategic_areas(p_bots);
	core::CacheKeyInputs in;
	in.kind = kAiAreasCacheKind;
	in.converter_version = kAiAreasConverterVersion;
	in.engine = std::string(core::cache_engine_id());
	in.format_version = kAiAreasConverterVersion;
	in.params = "install=" + p_install.hex() + ";level=" + core::to_lower_ascii(p_level) +
			";mode=" + core::to_lower_ascii(p_bots.mode);
	const std::optional<core::CacheKey> key = core::make_cache_key(in);
	if (!key) {
		return core::make_error("ai areas: no cache key for " + std::string(p_level));
	}
	CachedAiAreas out;
	const auto accept = [&](std::span<const uint8_t> p_payload) -> core::Status {
		std::string text(p_payload.begin(), p_payload.end());
		ai::AiConfig read;
		BF3_TRY(ai::run_ai_script(text, kAiAreasCacheKind, read));
		if (read.strategic_areas != ai::strategic_areas_by_name(derived)) {
			return core::make_error("ai areas: the cached areas are not the level's");
		}
		out.text = std::move(text);
		return core::Status::success();
	};
	const auto produce = [&]() -> core::Result<std::vector<uint8_t>> {
		out.text = ai::format_strategic_areas(derived);
		return std::vector<uint8_t>(out.text.begin(), out.text.end());
	};
	core::Result<core::CachedItem> item = core::cached_convert(p_cache, *key, accept, produce);
	if (!item) {
		return core::error_at("ai areas", item.error());
	}
	out.outcome = item->outcome;
	return out;
}

}
