#include "fx_trace.h"

#include "game_audio.h"
#include "game_fx.h"
#include "godot_util.h"
#include "level_session_game.h"

#include "bf3/fx/triggers.h"

#include <godot_cpp/variant/utility_functions.hpp>

namespace bf3::godot_side {

using godot::UtilityFunctions;

bool TraceCsv::open(
		const std::string &p_path, std::string_view p_header, const core::PathGuard &p_guard, std::string *r_error) {
	const std::filesystem::path path = utf8_path(p_path);
	const core::PathGuard::Verdict verdict = p_guard.check_write_target(path);
	if (verdict != core::PathGuard::Verdict::Allowed) {
		*r_error = p_path + ": refused by the path guard (" + core::PathGuard::verdict_name(verdict) + ")";
		return false;
	}
	file_.open(path, std::ios::binary | std::ios::trunc);
	if (!file_.is_open()) {
		*r_error = p_path + ": cannot be written";
		return false;
	}
	file_ << p_header << '\n';
	rows_ = 0;
	return true;
}

void TraceCsv::write(const std::string &p_row) {
	file_ << p_row << '\n';
	++rows_;
}

void TraceCsv::close() {
	if (file_.is_open()) {
		file_.close();
	}
}

FxTraces::~FxTraces() {
	if (fx.is_open()) {
		UtilityFunctions::print(to_godot("[game] fx trace: " + std::to_string(fx.rows()) + " trigger starts to " +
				fx_path + "; " + std::to_string(fx_stale) + " pre-join events dropped"));
	}
	if (audio.is_open()) {
		UtilityFunctions::print(to_godot("[game] audio trace: " + std::to_string(audio.rows()) +
				" weapon sound starts to " + audio_path + "; " + std::to_string(weapon_stale) +
				" pre-join weapon events and " + std::to_string(impact_stale) + " impact requests dropped"));
	}
	fx.close();
	audio.close();
}

void write_fx_traces(FxTraces &r_traces, const core::PathGuard &p_guard, const Bf3GameFx *p_fx, Bf3GameAudio *p_audio) {
	if (!r_traces.opened) {
		r_traces.opened = true;
		std::string error;
		if (!r_traces.fx_path.empty() &&
				!r_traces.fx.open(r_traces.fx_path, fx::kTriggerTraceHeader, p_guard, &error)) {
			UtilityFunctions::print(to_godot("[game] ERROR --fx-trace=" + error));
		}
		error.clear();
		if (!r_traces.audio_path.empty() &&
				!r_traces.audio.open(r_traces.audio_path, audio::kWeaponSoundTraceHeader, p_guard, &error)) {
			UtilityFunctions::print(to_godot("[game] ERROR --audio-trace=" + error));
		}
	}
	if (r_traces.fx.is_open() && p_fx != nullptr && p_fx->triggers() != nullptr &&
			p_fx->stats().frames != r_traces.fx_frames) {
		r_traces.fx_frames = p_fx->stats().frames;
		const fx::FxTriggers &t = *p_fx->triggers();
		for (const fx::TriggerStart &s : t.starts()) {
			r_traces.fx.write(fx::trigger_trace_row(s));
		}
		r_traces.fx_stale = t.stats().stale_dropped;
	}
	if (r_traces.audio.is_open() && p_audio != nullptr && p_audio->built()) {
		p_audio->trace_weapon_starts(true);
		r_traces.weapon_starts.clear();
		p_audio->take_weapon_starts(r_traces.weapon_starts);
		for (const audio::WeaponSoundStart &s : r_traces.weapon_starts) {
			r_traces.audio.write(audio::weapon_sound_trace_row(s));
		}
		const GameAudioStats st = p_audio->stats();
		r_traces.weapon_stale = st.router.stale_dropped;
		r_traces.impact_stale = st.impacts.stale_dropped;
	}
}

}
