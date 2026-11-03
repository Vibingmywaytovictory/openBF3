#pragma once

#include "bf3/audio/weapon_audio.h"
#include "bf3/core/path_guard.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::godot_side {

class Bf3GameAudio;
class Bf3GameFx;

class TraceCsv {
public:
	bool open(
			const std::string &p_path, std::string_view p_header, const core::PathGuard &p_guard, std::string *r_error);
	bool is_open() const { return file_.is_open(); }
	void write(const std::string &p_row);
	uint64_t rows() const { return rows_; }
	void close();

private:
	std::ofstream file_;
	uint64_t rows_ = 0;
};

struct FxTraces {
	std::string fx_path;
	std::string audio_path;
	bool opened = false;
	TraceCsv fx;
	TraceCsv audio;
	std::vector<audio::WeaponSoundStart> weapon_starts;
	uint64_t fx_frames = 0;
	uint64_t fx_stale = 0, weapon_stale = 0, impact_stale = 0;

	~FxTraces();
};

void write_fx_traces(FxTraces &r_traces, const core::PathGuard &p_guard, const Bf3GameFx *p_fx, Bf3GameAudio *p_audio);

}
