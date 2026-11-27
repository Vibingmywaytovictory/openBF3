#include "bf3/fx/placed_effects.h"

#include <algorithm>
#include <cmath>

namespace bf3::fx {

namespace {

bool mesh_emitter(const EffectDef &p_def, uint32_t p_emitter) {
	return p_emitter < p_def.emitters.size() && p_def.emitters[p_emitter].tmpl.emittable == EmittableType::ParticleMesh;
}

float distance(const Vec3 &p_a, const Vec3 &p_b) {
	const float dx = p_a.x - p_b.x, dy = p_a.y - p_b.y, dz = p_a.z - p_b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}

uint32_t mesh_emitter_count(const EffectDef &p_def) {
	uint32_t n = 0;
	for (const EffectComponentDef &c : p_def.components) {
		n += c.kind == ComponentKind::Emitter && mesh_emitter(p_def, c.emitter) ? 1u : 0u;
	}
	for (const EmitterDef &e : p_def.emitters) {
		for (const uint32_t child : e.children) {
			n += e.tmpl.emittable != EmittableType::ParticleMesh && mesh_emitter(p_def, child) ? 1u : 0u;
		}
	}
	return n;
}

void collect_mesh_names(const EffectDef &p_def, std::vector<std::string> &r_names) {
	for (const EmitterDef &e : p_def.emitters) {
		if (e.tmpl.emittable == EmittableType::ParticleMesh && !e.tmpl.mesh.empty() &&
				std::find(r_names.begin(), r_names.end(), e.tmpl.mesh) == r_names.end()) {
			r_names.push_back(e.tmpl.mesh);
		}
	}
}

void PlacedEffects::add(std::shared_ptr<const EffectDef> p_def, const Transform &p_transform, bool p_auto_start) {
	Slot slot;
	slot.def = std::move(p_def);
	slot.transform = p_transform;
	slot.auto_start = p_auto_start;
	slot.wanted = p_auto_start;
	++stats_.placed;
	stats_.auto_start += p_auto_start ? 1u : 0u;
	stats_.without_def += slot.def == nullptr ? 1u : 0u;
	slots_.push_back(std::move(slot));
}

void PlacedEffects::step(FxWorld &r_world, const FxView &p_view, uint8_t p_tier, uint64_t p_seed) {
	uint32_t running = 0, in_range_count = 0, ended = 0;
	for (size_t i = 0; i < slots_.size(); ++i) {
		Slot &slot = slots_[i];
		if (slot.def == nullptr) {
			continue;
		}
		if (!slot.wanted) {
			if (slot.handle.valid()) {
				if (r_world.alive(slot.handle)) {
					r_world.stop(slot.handle);
					++stats_.stops;
				}
				slot.handle = EffectHandle{};
			}
			continue;
		}
		// INFERRED: a placed effect runs while the camera is within its blueprint's CullDistance (always when 0), and one
		// that ended is not restarted. Settle: a note on placed EffectEntityData and CullDistance.
		const float cull = slot.def->cull_distance;
		const bool in_range = !(cull > 0.0f) || distance(slot.transform.trans, p_view.camera_position) <= cull;
		in_range_count += in_range ? 1u : 0u;
		const bool alive = slot.handle.valid() && r_world.alive(slot.handle);
		if (slot.handle.valid() && !alive) {
			slot.handle = EffectHandle{};
			slot.finished = in_range;
		}
		if (slot.finished) {
			ended += in_range ? 1u : 0u;
			continue;
		}
		if (in_range && !alive) {
			EffectStart start;
			start.transform = slot.transform;
			start.seed = core::splitmix64(p_seed ^ (uint64_t(i) + 1) ^ (stats_.starts << 32));
			start.tier = p_tier;
			// INFERRED: each placement is its own MaxInstanceCount pool. Settle: an answer to Q-FX2 (STATUS.md, open).
			start.owner = uint32_t(i) + 1;
			core::Result<EffectHandle> h = r_world.start(slot.def, start);
			if (h) {
				slot.handle = *h;
				stats_.started_placements += slot.started ? 0u : 1u;
				slot.started = true;
				stats_.logic_started_placements += slot.logic_started || slot.auto_start ? 0u : 1u;
				slot.logic_started = slot.logic_started || !slot.auto_start;
				++stats_.starts;
			} else {
				++stats_.start_failures;
			}
		} else if (!in_range && alive) {
			r_world.stop(slot.handle);
			slot.handle = EffectHandle{};
			++stats_.stops;
		}
		running += slot.handle.valid() ? 1u : 0u;
	}
	stats_.running = running;
	stats_.in_range = in_range_count;
	stats_.ended_in_range = ended;
}

void PlacedEffects::bind_logic(std::span<const uint32_t> p_nodes) {
	stats_.logic_bound = 0;
	for (size_t i = 0; i < slots_.size(); ++i) {
		slots_[i].logic_node = i < p_nodes.size() ? p_nodes[i] : kNoPlacedLogicNode;
		stats_.logic_bound += slots_[i].logic_node != kNoPlacedLogicNode ? 1u : 0u;
	}
}

void PlacedEffects::logic_event(uint32_t p_node, bool p_start) {
	if (p_node == kNoPlacedLogicNode) {
		return;
	}
	for (Slot &slot : slots_) {
		if (slot.logic_node != p_node) {
			continue;
		}
		slot.wanted = p_start;
		slot.finished = false;
		++(p_start ? stats_.logic_starts : stats_.logic_stops);
	}
}

void PlacedEffects::logic_reset() {
	for (Slot &slot : slots_) {
		slot.wanted = slot.auto_start;
		slot.finished = false;
	}
}

void PlacedEffects::forget() {
	slots_.clear();
	stats_ = PlacedEffectStats{};
}

}
