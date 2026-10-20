#pragma once

#include "bf3/host/rest_check.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3LevelSession;

struct CheckRow {
	std::string check;
	int result = 2;
	std::string detail;
};

int print_check_table(const char *p_title, const std::vector<CheckRow> &p_rows);

CheckRow check_surface_format();

CheckRow check_input_keys();

class ShotRunner {
public:
	struct FreeShot {
		std::string path;
		std::array<float, 5> view{};
	};
	struct SpawnShot {
		uint32_t index = 0;
		std::string mode = "stand";
	};

	struct TeleportShot {
		std::array<float, 4> at{};
	};
	static bool parse_teleports(const std::string &p_teleports, std::vector<TeleportShot> &r_teleports, std::string &r_error);
	void set_teleports(std::vector<TeleportShot> p_teleports) { teleport_ = std::move(p_teleports); }

	static bool parse(const std::string &p_shots, const std::string &p_spawns, std::vector<FreeShot> &r_free, std::vector<SpawnShot> &r_spawn,
			std::string &r_error);

	ShotRunner(Bf3LevelSession &p_session, std::vector<FreeShot> p_free, std::vector<SpawnShot> p_spawn, std::string p_dir);
	bool empty() const { return free_.empty() && spawn_.empty() && teleport_.empty(); }

	bool frame(uint64_t p_now_ns);

	void set_timing(double p_settle_s, uint32_t p_measure) {
		settle_s_ = p_settle_s;
		measure_frames_ = p_measure == 1 ? 2 : p_measure;
	}
	size_t saved() const { return saved_; }
	size_t failed() const { return failed_; }
	const std::vector<std::string> &lines() const { return lines_; }

private:
	Bf3LevelSession &session_;
	std::vector<FreeShot> free_;
	std::vector<SpawnShot> spawn_;
	std::vector<TeleportShot> teleport_;
	std::string dir_;
	size_t next_ = 0;
	int state_ = 0;
	uint64_t state_ns_ = 0;
	uint32_t life_ = 0;
	uint64_t frames_ = 0;
	uint32_t request_id_ = 7000;
	size_t saved_ = 0;
	size_t failed_ = 0;
	std::vector<std::string> lines_;

	double settle_s_ = 0.0;
	uint32_t measure_frames_ = 0;
	uint32_t measured_ = 0;
	uint64_t last_ns_ = 0;
	double view_cpu_ms_ = 0.0, view_gpu_ms_ = 0.0, total_cpu_ms_ = 0.0, total_gpu_ms_ = 0.0;
	uint64_t total_frames_ = 0;

	std::vector<float> view_cpu_samples_, view_gpu_samples_;
	double median_cpu_sum_ms_ = 0.0, median_gpu_sum_ms_ = 0.0;
	size_t median_views_ = 0;
	std::vector<float> run_cpu_samples_;
};

class M1Check {
public:

	M1Check(Bf3LevelSession &p_session, uint32_t p_courses, const std::string &p_phase = std::string());
	~M1Check();

	bool frame(uint64_t p_now_ns);
	int exit_code() const { return exit_code_; }

private:
	struct Course {
		uint32_t point = 0;
		bf3::sim::Vec3 feet{};
		uint16_t heading = 0;
	};
	enum class Phase : uint8_t {
		Prepare,
		E1Teleport,
		E1Segment,
		E1Report,
		E1dTeleport,
		E1dReport,
		E1dRespawn,
		E3,
		E2Start,
		E2Spawn,
		E2Settle,
		E2Record,
		Finish,
		Done,
	};

	void prepare();
	void add(const std::string &p_check, int p_result, const std::string &p_detail);
	void fail_phase(const std::string &p_check, const std::string &p_why);
	bool teleport(const bf3::sim::Vec3 &p_feet, uint16_t p_yaw, uint64_t p_now_ns);
	bool at_teleport(const bf3::sim::Vec3 &p_feet) const;
	void send_trace(uint32_t p_ticks);
	std::optional<bf3::net::DebugReport> take_report(uint32_t p_id);
	void collect_snapshot_states();
	void compare_snapshots(const std::vector<bf3::sim::SoldierState> &p_trace);
	void analyse_segment(const std::vector<bf3::sim::SoldierState> &p_trace);
	void analyse_drop(const std::vector<bf3::sim::SoldierState> &p_trace);
	void finish_e1();
	void judge_e2();
	void finish_rows();
	bool timed_out(uint64_t p_now_ns, double p_seconds) const;

	Bf3LevelSession &session_;
	uint32_t want_courses_ = 6;
	std::string phase_name_ = "all";
	bool phase_known_ = true;
	bool run_e1_ = true;
	size_t profile_begin_ = 0, profile_end_ = 2;
	std::vector<CheckRow> rows_;
	bool failed_ = false;
	int exit_code_ = 1;
	Phase phase_ = Phase::Prepare;
	uint64_t phase_ns_ = 0;

	std::unique_ptr<bf3::sim::CollisionWorld> world_;
	bf3::sim::SoldierMotor motor_;
	std::vector<bf3::sim::SettleResult> headless_rows_;
	std::vector<Course> courses_;
	std::optional<bf3::sim::Vec3> drop_;
	double prepare_seconds_ = 0.0;

	size_t course_ = 0;
	size_t segment_ = 0;
	int step_ = 0;
	bf3::sim::Tick step_tick_ = 0;
	bf3::sim::Tick walk_tick_ = 0;
	uint32_t trace_id_ = 100;
	uint32_t request_id_ = 5000;
	std::map<bf3::sim::Tick, bf3::sim::SoldierState> server_own_;
	uint64_t snapshots_compared_ = 0, snapshots_equal_ = 0;

	struct SpeedResult {
		size_t measured = 0, passed = 0;
		float lo = 1e9f, hi = 0.0f, worst_mean_error = 0.0f;
	};
	std::array<SpeedResult, 4> speeds_{};
	size_t jumps_ = 0, jumps_ok_ = 0;
	float apex_lo_ = 1e9f, apex_hi_ = -1e9f;
	size_t chains_ = 0, chains_ok_ = 0;
	std::string chain_ticks_;
	std::vector<std::string> e1_failures_;
	size_t drop_index_ = 0;
	float drop_height_ = 0.0f;
	uint32_t drop_life_ = 0;

	size_t profile_ = 0;
	std::vector<bf3::host::RestSample> samples_;
	uint64_t record_start_ns_ = 0;
	uint32_t e2_life_ = 0;
};

class M1LoadCheck {
public:
	M1LoadCheck(Bf3LevelSession &p_session, bool p_warm, ShotRunner *p_shots);

	bool frame(uint64_t p_now_ns, double p_delta);
	int exit_code() const { return exit_code_; }

private:
	void report();

	Bf3LevelSession &session_;
	bool warm_ = false;
	ShotRunner *shots_ = nullptr;
	int phase_ = 0;
	uint64_t start_ns_ = 0;
	std::vector<float> frame_ms_;
	int exit_code_ = 1;
};

}
