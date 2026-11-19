#include "terrain_scatter.h"

#include "bf3/core/util/format.h"
#include "godot_util.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace bf3::godot_side {

using namespace godot;
namespace render = bf3::render;
using bf3::core::fmt;

namespace {

constexpr float kKeepFactor = 1.5f;

double seconds_now() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

float rect_distance(const std::array<float, 4> &p_rect, float p_x, float p_z) {
	const float dx = std::fmax(std::fmax(p_rect[0] - p_x, p_x - p_rect[2]), 0.0f);
	const float dz = std::fmax(std::fmax(p_rect[1] - p_z, p_z - p_rect[3]), 0.0f);
	return std::sqrt(dx * dx + dz * dz);
}

}

TerrainScatterDraw::TerrainScatterDraw() :
		ready_mutex_(std::make_shared<std::mutex>()), ready_(std::make_shared<std::vector<Result>>()) {}

TerrainScatterDraw::~TerrainScatterDraw() {
	free_all();
}

void TerrainScatterDraw::begin(std::string p_name, std::shared_ptr<const render::TerrainScatter> p_scatter,
		const RID &p_scenario, const Transform3D &p_terrain, MeshOf p_mesh, const BoundsOf &p_bounds,
		NameOf p_name_of) {
	free_all();
	if (p_scatter == nullptr || p_scatter->levels().empty()) {
		return;
	}
	name_ = std::move(p_name);
	name_of_ = std::move(p_name_of);
	scatter_ = std::move(p_scatter);
	scenario_ = p_scenario;
	terrain_ = p_terrain;
	mesh_ = std::move(p_mesh);
	std::vector<render::ScatterFootprint> footprints;
	for (const render::ScatterType &t : scatter_->types()) {
		if (t.mesh < 0 || t.upright || !p_bounds) {
			continue;
		}
		if (footprints.size() <= size_t(t.mesh)) {
			footprints.resize(size_t(t.mesh) + 1);
		}
		if (const std::optional<AABB> box = p_bounds(uint32_t(t.mesh))) {
			footprints[size_t(t.mesh)] = std::array<float, 4>{ float(box->position.x), float(box->position.z),
				float(box->position.x + box->size.x), float(box->position.z + box->size.z) };
		}
	}
	footprints_ = std::make_shared<const std::vector<render::ScatterFootprint>>(std::move(footprints));
	jobs_ = std::make_unique<bf3::core::JobQueue>("scatter");
	size_t placed = 0;
	for (const render::ScatterType &t : scatter_->types()) {
		placed += t.mesh >= 0 && t.candidates > 0 ? 1 : 0;
	}
	std::string levels;
	for (uint8_t level : scatter_->levels()) {
		levels += fmt("%s%u (%.0f m)", levels.empty() ? "" : ", ", unsigned(level), double(scatter_->cell_size(level)));
	}
	UtilityFunctions::print(to_godot(fmt(
			"[scatter] %s: %zu scattering types in the data, %zu placed, %zu mask programs, tree levels %s, %.1f MiB",
			name_.c_str(), scatter_->types().size(), placed, scatter_->programs(), levels.c_str(),
			double(scatter_->bytes()) / (1024.0 * 1024.0))));
}

void TerrainScatterDraw::release(Cell &r_cell) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		for (const RID &rid : r_cell.instances) {
			rs->free_rid(rid);
		}
		for (const RID &rid : r_cell.multimeshes) {
			rs->free_rid(rid);
		}
	}
	r_cell.instances.clear();
	r_cell.multimeshes.clear();
}

void TerrainScatterDraw::free_all() {
	if (jobs_ != nullptr) {
		for (auto &[key, cell] : cells_) {
			if (cell.cancelled != nullptr) {
				cell.cancelled->store(true, std::memory_order_release);
			}
		}
		jobs_->stop();
		jobs_.reset();
	}
	for (auto &[key, cell] : cells_) {
		release(cell);
	}
	cells_.clear();
	uploads_.clear();
	{
		std::lock_guard lock(*ready_mutex_);
		ready_->clear();
	}
	scatter_.reset();
	mesh_ = nullptr;
	stats_ = ScatterDrawStats();
	reported_ = true;
}

void TerrainScatterDraw::upload(Cell &r_cell, const Result &p_result) {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (const render::ScatterBatch &batch : p_result.batches) {
		const render::ScatterType &type = scatter_->types()[batch.type];
		const RID mesh = mesh_ ? mesh_(uint32_t(type.mesh)) : RID();
		if (!mesh.is_valid() || batch.instances.empty()) {
			r_cell.types.push_back(Cell::TypeCount{ batch.type, 0, batch.coverage });
			continue;
		}
		PackedFloat32Array buffer;
		buffer.resize(int64_t(batch.instances.size()) * 12);
		float *w = buffer.ptrw();
		uint32_t tilted = 0;
		for (const render::ScatterInstance &inst : batch.instances) {
			const std::array<float, 12> rows = render::scatter_instance_rows(inst);
			std::copy(rows.begin(), rows.end(), w);
			w += 12;
			tilted += inst.up[1] < 1.0f ? 1 : 0;
		}
		const RID multimesh = rs->multimesh_create();
		rs->multimesh_allocate_data(
				multimesh, int32_t(batch.instances.size()), RenderingServer::MULTIMESH_TRANSFORM_3D);
		rs->multimesh_set_mesh(multimesh, mesh);
		rs->multimesh_set_buffer(multimesh, buffer);
		const RID instance = rs->instance_create2(multimesh, scenario_);
		rs->instance_set_transform(instance, terrain_);
		rs->instance_geometry_set_cast_shadows_setting(instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
		rs->instance_set_visible(instance, r_cell.visible);
		r_cell.multimeshes.push_back(multimesh);
		r_cell.instances.push_back(instance);
		r_cell.types.push_back(Cell::TypeCount{ batch.type, uint32_t(batch.instances.size()), batch.coverage, tilted });
		++stats_.multimeshes;
		stats_.instances += batch.instances.size();
	}
	r_cell.stats = p_result.stats;
	r_cell.ready = true;
	++stats_.cells_generated;
	stats_.generate_seconds += p_result.seconds;
}

void TerrainScatterDraw::update(const Vector3 &p_camera, double p_budget_ms) {
	if (scatter_ == nullptr) {
		return;
	}
	const double start = seconds_now();
	RenderingServer *rs = RenderingServer::get_singleton();
	const Vector3 local = terrain_.affine_inverse().xform(p_camera);
	std::map<Key, bool> wanted;
	for (uint8_t level : scatter_->levels()) {
		for (const render::ScatterCellId &id :
				scatter_->cells_within(level, local.x, local.z, scatter_->view_distance(level))) {
			wanted.emplace(Key{ id.level, id.x, id.z }, true);
			const Key key{ id.level, id.x, id.z };
			if (cells_.find(key) != cells_.end()) {
				continue;
			}
			Cell cell;
			cell.id = id;
			cell.cancelled = std::make_shared<std::atomic<bool>>(false);
			const std::shared_ptr<const render::TerrainScatter> scatter = scatter_;
			const std::shared_ptr<const std::vector<render::ScatterFootprint>> footprints = footprints_;
			const std::shared_ptr<std::atomic<bool>> cancelled = cell.cancelled;
			const std::shared_ptr<std::mutex> mutex = ready_mutex_;
			const std::shared_ptr<std::vector<Result>> ready = ready_;
			const bool posted = jobs_->post([scatter, footprints, cancelled, mutex, ready, id]() {
				if (cancelled->load(std::memory_order_acquire)) {
					return;
				}
				const double begin = seconds_now();
				Result result;
				result.id = id;
				result.batches = scatter->generate(id, &result.stats, *footprints);
				result.seconds = seconds_now() - begin;
				std::lock_guard lock(*mutex);
				ready->push_back(std::move(result));
			});
			if (posted) {
				cells_.emplace(key, std::move(cell));
				reported_ = false;
			}
		}
	}
	{
		std::lock_guard lock(*ready_mutex_);
		for (Result &r : *ready_) {
			uploads_.push_back(std::move(r));
		}
		ready_->clear();
	}
	for (auto it = cells_.begin(); it != cells_.end();) {
		Cell &cell = it->second;
		const bool want = wanted.find(it->first) != wanted.end();
		if (!want &&
				rect_distance(scatter_->cell_rect(cell.id), local.x, local.z) >
						kKeepFactor * scatter_->view_distance(cell.id.level)) {
			cell.cancelled->store(true, std::memory_order_release);
			for (const Cell::TypeCount &t : cell.types) {
				stats_.instances -= t.instances;
			}
			stats_.multimeshes -= cell.multimeshes.size();
			release(cell);
			++stats_.cells_freed;
			it = cells_.erase(it);
			continue;
		}
		if (cell.visible != want) {
			cell.visible = want;
			for (const RID &rid : cell.instances) {
				rs->instance_set_visible(rid, want);
			}
		}
		++it;
	}
	while (!uploads_.empty() && (seconds_now() - start) * 1000.0 < p_budget_ms) {
		const Result result = std::move(uploads_.front());
		uploads_.pop_front();
		const auto found = cells_.find(Key{ result.id.level, result.id.x, result.id.z });
		if (found == cells_.end() || found->second.ready) {
			continue;
		}
		upload(found->second, result);
	}
	stats_.upload_seconds += seconds_now() - start;
	uint64_t pending = uploads_.size();
	uint64_t drawn_cells = 0, drawn_instances = 0;
	for (const auto &[key, cell] : cells_) {
		pending += cell.ready ? 0 : 1;
		if (cell.ready && cell.visible) {
			++drawn_cells;
			for (const Cell::TypeCount &t : cell.types) {
				drawn_instances += t.instances;
			}
		}
	}
	stats_.cells_live = cells_.size();
	stats_.cells_pending = pending;
	stats_.cells_drawn = drawn_cells;
	stats_.instances_drawn = drawn_instances;
	if (pending == 0 && !reported_) {
		reported_ = true;
		report(local);
	}
}

void TerrainScatterDraw::report(const Vector3 &p_local) {
	const std::span<const render::ScatterType> types = scatter_->types();
	std::vector<uint64_t> instances(types.size(), 0);
	std::vector<double> masks(types.size(), 0.0);
	uint64_t candidates = 0, evaluated = 0, unmatched = 0, tilted = 0;
	double coverage = 0.0;
	for (const auto &[key, cell] : cells_) {
		if (!cell.ready || !cell.visible) {
			continue;
		}
		for (const Cell::TypeCount &t : cell.types) {
			instances[t.type] += t.instances;
			masks[t.type] += t.coverage;
			tilted += t.tilted;
		}
		candidates += cell.stats.candidates;
		evaluated += cell.stats.evaluated;
		unmatched += cell.stats.unmatched;
		coverage += cell.stats.coverage;
	}
	std::string per_type;
	uint64_t drawn_types = 0;
	for (size_t t = 0; t < types.size(); ++t) {
		drawn_types += instances[t] > 0 ? 1 : 0;
		per_type += fmt(" %zu:%llu/%.0f", t, static_cast<unsigned long long>(instances[t]), std::floor(masks[t]));
	}
	stats_.types_drawn = drawn_types;
	UtilityFunctions::print(to_godot(fmt(
			"[scatter] %s at (%.1f, %.1f): %llu cells drawn, %llu instances in %llu "
			"multimeshes, %llu of %zu types drawn, "
			"%llu tilted to the terrain under them, %llu candidates, %llu evaluated (mask sum %.0f), %llu "
			"without a program, "
			"generate %.3f s, cells made %llu freed %llu, per type%s",
			name_.c_str(), double(p_local.x), double(p_local.z), static_cast<unsigned long long>(stats_.cells_drawn),
			static_cast<unsigned long long>(stats_.instances_drawn),
			static_cast<unsigned long long>(stats_.multimeshes), static_cast<unsigned long long>(drawn_types),
			types.size(), static_cast<unsigned long long>(tilted), static_cast<unsigned long long>(candidates),
			static_cast<unsigned long long>(evaluated), coverage, static_cast<unsigned long long>(unmatched),
			stats_.generate_seconds, static_cast<unsigned long long>(stats_.cells_generated),
			static_cast<unsigned long long>(stats_.cells_freed), per_type.c_str())));
	for (size_t t = 0; t < types.size(); ++t) {
		const render::ScatterType &type = types[t];
		if (type.mesh < 0 || type.candidates == 0) {
			continue;
		}
		const std::string mesh = name_of_ ? name_of_(uint32_t(type.mesh)) : std::string();
		UtilityFunctions::print(to_godot(fmt("[scatter] %s type %zu %s: %llu drawn, mask sum %.1f, within %.0f m "
											 "(tree level %u, cell %.0f m), density %.4f per m2",
				name_.c_str(), t, mesh.c_str(), static_cast<unsigned long long>(instances[t]), masks[t],
				double(scatter_->view_distance(type.tree_level)), unsigned(type.tree_level), double(type.cell),
				double(type.density))));
	}
}

}
