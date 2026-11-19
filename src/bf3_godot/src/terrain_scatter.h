#pragma once

#include "bf3/core/job_queue.h"
#include "bf3/render/terrain_scatter.h"

#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace bf3::godot_side {

struct ScatterDrawStats {
	uint64_t cells_generated = 0;
	uint64_t cells_freed = 0;
	uint64_t cells_live = 0;
	uint64_t cells_drawn = 0;
	uint64_t cells_pending = 0;
	uint64_t multimeshes = 0;
	uint64_t instances = 0;
	uint64_t instances_drawn = 0;
	uint64_t types_drawn = 0;
	double generate_seconds = 0.0;
	double upload_seconds = 0.0;
};

class TerrainScatterDraw {
public:
	using MeshOf = std::function<godot::RID(uint32_t)>;

	TerrainScatterDraw();
	~TerrainScatterDraw();
	TerrainScatterDraw(const TerrainScatterDraw &) = delete;
	TerrainScatterDraw &operator=(const TerrainScatterDraw &) = delete;

	void begin(std::string p_name, std::shared_ptr<const bf3::render::TerrainScatter> p_scatter,
			const godot::RID &p_scenario, const godot::Transform3D &p_terrain, MeshOf p_mesh);
	bool begun() const { return scatter_ != nullptr; }

	void update(const godot::Vector3 &p_camera, double p_budget_ms);
	void free_all();

	ScatterDrawStats stats() const { return stats_; }

private:
	using Key = std::tuple<uint8_t, int32_t, int32_t>;
	struct Result {
		bf3::render::ScatterCellId id;
		std::vector<bf3::render::ScatterBatch> batches;
		bf3::render::ScatterCellStats stats;
		double seconds = 0.0;
	};
	struct Cell {
		bf3::render::ScatterCellId id;
		std::shared_ptr<std::atomic<bool>> cancelled;
		bool ready = false;
		bool visible = false;
		std::vector<godot::RID> multimeshes;
		std::vector<godot::RID> instances;
		struct TypeCount {
			uint32_t type = 0;
			uint32_t instances = 0;
			double coverage = 0.0;
			uint32_t tilted = 0;
		};
		std::vector<TypeCount> types;
		bf3::render::ScatterCellStats stats;
	};

	void upload(Cell &r_cell, const Result &p_result);
	void release(Cell &r_cell);
	void report(const godot::Vector3 &p_local);

	std::string name_;
	std::shared_ptr<const bf3::render::TerrainScatter> scatter_;
	godot::RID scenario_;
	godot::Transform3D terrain_;
	MeshOf mesh_;
	std::unique_ptr<bf3::core::JobQueue> jobs_;
	std::shared_ptr<std::mutex> ready_mutex_;
	std::shared_ptr<std::vector<Result>> ready_;
	std::deque<Result> uploads_;
	std::map<Key, Cell> cells_;
	ScatterDrawStats stats_;
	bool reported_ = true;
};

}
