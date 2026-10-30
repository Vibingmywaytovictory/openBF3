#include "audio_areas_check.h"

#include "game_audio.h"
#include "level_session.h"

#include "bf3/audio/weapon_sound.h"
#include "bf3/core/util/format.h"
#include "bf3/host/client_session.h"
#include "bf3/host/ui_flow_session.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/soldier_motor.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <cstdlib>
#include <sstream>

namespace bf3::godot_side {

using namespace godot;
using bf3::core::fmt;

namespace {

constexpr double kAliveTimeout = 90.0;
constexpr double kArriveTimeout = 10.0;
constexpr double kSettleSeconds = 1.0;
constexpr float kArriveXz = 0.5f;
constexpr float kArriveY = 1.5f;
constexpr size_t kMaxPoints = 32;

float horizontal_distance(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0];
	const float dz = p_a[2] - p_b[2];
	return std::sqrt(dx * dx + dz * dz);
}

}

bool parse_audio_areas_points(const std::string &p_text, std::vector<AudioAreasPoint> &r_points, std::string &r_error) {
	std::stringstream ss(p_text);
	std::string item;
	while (std::getline(ss, item, '|')) {
		if (item.empty()) {
			continue;
		}
		std::stringstream fields(item);
		std::string f;
		std::vector<std::string> parts;
		while (std::getline(fields, f, ',')) {
			parts.push_back(f);
		}
		AudioAreasPoint p;
		bool ok = parts.size() == 6;
		float v[5] = {};
		for (size_t i = 0; ok && i < 5; ++i) {
			char *end = nullptr;
			v[i] = std::strtof(parts[i].c_str(), &end);
			ok = !parts[i].empty() && end == parts[i].c_str() + parts[i].size() && std::isfinite(v[i]);
		}
		if (!ok || r_points.size() >= kMaxPoints || parts[5].empty()) {
			r_error = "--audio-areas-points entry is not x,y,z,yaw,areatype,wave (at most 32): " + item;
			return false;
		}
		p.feet = sim::Vec3{ v[0], v[1], v[2] };
		p.yaw = v[3];
		p.area_type = int(v[4]);
		p.wave = parts[5];
		r_points.push_back(p);
	}
	if (r_points.empty()) {
		r_error = "--audio-areas-points names no point";
		return false;
	}
	return true;
}

AudioAreasCheck::AudioAreasCheck(std::vector<AudioAreasPoint> p_points) : points_(std::move(p_points)) {}

void AudioAreasCheck::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0 : 1;
	UtilityFunctions::print(
			String::utf8(("AUDIO AREAS ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail).c_str()));
}

void AudioAreasCheck::teleport(Bf3LevelSession &r_session) {
	const AudioAreasPoint &p = points_[next_];
	r_session.mapper().script_view(p.yaw, 0.0f);
	host::ClientSession *client = r_session.client();
	if (client == nullptr) {
		return;
	}
	net::DebugRequest q;
	q.kind = net::DebugKind::Teleport;
	q.id = ++request_id_;
	q.position = p.feet;
	q.yaw = sim::quantize_yaw(p.yaw);
	client->send_debug(q);
}

void AudioAreasCheck::measure(const AudioAreasInputs &p_in) {
	const AudioAreasPoint &p = points_[next_];
	const std::string name = "point" + std::to_string(next_);
	const sim::logic::SoundAreaNatives *areas = p_in.flow != nullptr ? p_in.flow->sound_areas() : nullptr;
	if (areas == nullptr || p_in.audio == nullptr) {
		row(name + ".evaluator", false, "no sound area natives or no game audio");
		return;
	}
	const core::Vec3 at = areas->listener();
	const float evaluated = areas->area_type();
	size_t inside = 0;
	for (const sim::logic::SoundAreaValue &v : areas->areas()) {
		inside += v.proximity > 0.0f ? 1 : 0;
	}
	row(name + ".evaluator", areas->listening() && evaluated == float(p.area_type),
			fmt("listener (%.2f, %.2f, %.2f), %zu areas sounding, AreaType %g, core.audio.areas says %d", double(at.x),
					double(at.y), double(at.z), inside, double(evaluated), p.area_type));
	const std::optional<float> mixed = p_in.audio->mixer_value(audio::kHashAreaType);
	row(name + ".mixer", mixed.has_value() && *mixed == evaluated,
			mixed ? fmt("the mixer holds AreaType %g", double(*mixed)) : std::string("the mixer holds no AreaType"));
	core::Result<std::vector<std::string>> waves =
			p_in.audio->observe_area_waves(kAudioAreasM16Patch, mixed.value_or(-1.0f));
	std::string list;
	bool found = false;
	if (waves) {
		for (const std::string &w : *waves) {
			list += (list.empty() ? "" : " ") + w;
			found = found || w.find(p.wave) != std::string::npos;
		}
	}
	row(name + ".wave", waves.ok() && found,
			waves ? "the M16A3 observer plays " + list + ", want " + p.wave : "observer: " + waves.error().message);
}

bool AudioAreasCheck::step(Bf3LevelSession &r_session, const AudioAreasInputs &p_in, double p_delta) {
	const host::PresentationFrame &f = r_session.frame();
	switch (phase_) {
		case Phase::WaitAlive: {
			waited_ += p_delta;
			const bool in_round = f.match_valid && f.match.phase == net::MatchPhase::InRound;
			const sim::logic::SoundAreaNatives *areas = p_in.flow != nullptr ? p_in.flow->sound_areas() : nullptr;
			if (!p_in.alive || !in_round || areas == nullptr || p_in.audio == nullptr) {
				if (waited_ > kAliveTimeout) {
					row("level", false,
							fmt("after %.0f s: soldier %s, round %s, sound areas %s, audio %s", kAliveTimeout,
									p_in.alive ? "alive" : "not spawned", in_round ? "in round" : "not in round",
									areas != nullptr ? "bound" : "missing",
									p_in.audio != nullptr ? "built" : "missing"));
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			const sim::logic::SoundAreaNativeStats &s = areas->stats();
			row("level", s.areas > 0 && s.mixers > 0,
					fmt("in round with %zu shaped sound areas (%zu unshaped), %zu mixer assets, %zu mixer entities",
							s.areas, s.unshaped, s.mixers, s.mixer_entities));
			InputMapper &mapper = r_session.mapper();
			mapper.set_scripted(true);
			mapper.release_all();
			mapper.set_free_camera(false);
			r_session.cameras().set_free(false);
			teleport(r_session);
			t_ = 0.0;
			phase_ = Phase::Arrive;
			return false;
		}
		case Phase::Arrive: {
			t_ += p_delta;
			teleport(r_session);
			const AudioAreasPoint &p = points_[next_];
			const sim::Vec3 &at = f.own_curr.position;
			if (horizontal_distance(at, p.feet) > kArriveXz || std::fabs(at[1] - p.feet[1]) > kArriveY) {
				if (t_ > kArriveTimeout) {
					row("point" + std::to_string(next_) + ".place", false,
							fmt("the soldier is %.2f m from the teleport target after %.0f s",
									double(horizontal_distance(at, p.feet)), kArriveTimeout));
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			t_ = 0.0;
			phase_ = Phase::Settle;
			return false;
		}
		case Phase::Settle: {
			t_ += p_delta;
			r_session.mapper().script_view(points_[next_].yaw, 0.0f);
			if (t_ < kSettleSeconds) {
				return false;
			}
			measure(p_in);
			++next_;
			if (next_ < points_.size()) {
				teleport(r_session);
				t_ = 0.0;
				phase_ = Phase::Arrive;
				return false;
			}
			const audio::AreaFeedStats &feed = p_in.audio->area_feed();
			row("feed", feed.posts > 0 && feed.push_failures == 0,
					fmt("the area feed posted AreaType %llu times over %llu audio steps, %llu ring refusals",
							(unsigned long long)feed.posts, (unsigned long long)feed.steps,
							(unsigned long long)feed.push_failures));
			phase_ = Phase::Done;
			return true;
		}
		case Phase::Done:
			return true;
	}
	return true;
}

}
