#pragma once
// The S0 self-test inside the engine ("-- --s0-selftest"): install reader and read-only self-test,
// ServerHost stepping our Jolt for N seconds while Godot's own Jolt runs a RigidBody3D of the
// scene, ENet loopback ping, engine version and precision. Prints a PASS/FAIL table; the exit
// code is non-zero when any check fails.

#include <string>
#include <vector>

namespace godot {
class RigidBody3D;
}

namespace bf3::godot_side {

class GameSession;

class S0SelfTest {
public:
	S0SelfTest(GameSession &p_session, double p_seconds);
	void begin();
	void physics_tick();
	// Returns true when finished (table printed).
	bool process(double p_delta);
	int exit_code() const { return failed_ ? 1 : 0; }

private:
	enum class Result { Pass, Fail, Skip, Info };
	struct Row {
		std::string check;
		Result result;
		std::string detail;
	};
	void add(const std::string &p_check, Result p_result, const std::string &p_detail);
	void finish();
	void print_table();

	GameSession &session_;
	double seconds_;
	double elapsed_ = 0.0;
	bool finished_ = false;
	bool failed_ = false;
	std::vector<Row> rows_;

	godot::RigidBody3D *body_ = nullptr;
	float body_y0_ = 0.0f;
	float body_min_y_ = 1e30f;
	uint64_t physics_ticks_ = 0;
	uint64_t server_ticks_at_begin_ = 0;
	uint64_t server_hash_at_begin_ = 0;
	uint64_t frames_ = 0;
	uint64_t window_start_ns_ = 0;
};

} // namespace bf3::godot_side
