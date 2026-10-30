#pragma once

#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::host {
class UiFlowSession;
}

namespace bf3::godot_side {

class Bf3LevelSession;
class Bf3GameAudio;

inline constexpr const char *kAudioAreasM16Patch = "sound/weapons/handheld/m16a3/weapon_assaultrifle_m16a4_02";

struct AudioAreasPoint {
	bf3::sim::Vec3 feet{};
	float yaw = 0.0f;
	int area_type = 0;
	std::string wave;
};

struct AudioAreasInputs {
	bool alive = false;
	const Bf3GameAudio *audio = nullptr;
	const bf3::host::UiFlowSession *flow = nullptr;
};

bool parse_audio_areas_points(const std::string &p_text, std::vector<AudioAreasPoint> &r_points, std::string &r_error);

class AudioAreasCheck {
public:
	explicit AudioAreasCheck(std::vector<AudioAreasPoint> p_points);
	bool step(Bf3LevelSession &r_session, const AudioAreasInputs &p_in, double p_delta);
	int exit_code() const { return failures_ == 0 && rows_ > 0 ? 0 : 1; }

private:
	enum class Phase : uint8_t { WaitAlive, Arrive, Settle, Done };

	void teleport(Bf3LevelSession &r_session);
	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void measure(const AudioAreasInputs &p_in);

	std::vector<AudioAreasPoint> points_;
	size_t next_ = 0;
	Phase phase_ = Phase::WaitAlive;
	double t_ = 0.0;
	double waited_ = 0.0;
	uint32_t request_id_ = 0;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
};

}
