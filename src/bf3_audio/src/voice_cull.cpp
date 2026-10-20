#include "bf3/audio/voice_cull.h"

#include "bf3/core/audio/hdr_setting.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace bf3::audio {

namespace {

using core::audio::kNoScope;
using core::audio::ScopeTable;
using core::audio::SoundScope;

bool older(const CullVoice &a, size_t ia, const CullVoice &b, size_t ib) {
	return a.start_frame != b.start_frame ? a.start_frame < b.start_frame : ia < ib;
}

float distance_between(const CullVoice &a, const CullVoice &b) {
	const float dx = a.position[0] - b.position[0], dy = a.position[1] - b.position[1], dz = a.position[2] - b.position[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct Culler {
	std::span<const CullVoice> v;
	std::span<CullVerdict> out;
	CullCounts &counts;
	std::array<uint8_t, kMaxCullVoices> mark{};

	bool kept(size_t i) const { return out[i] == CullVerdict::Keep; }

	void cull(size_t i, CullVerdict p_drop) {
		if (v[i].looping) {
			out[i] = CullVerdict::Pending;
			++counts.pending;
			return;
		}
		out[i] = p_drop;
		switch (p_drop) {
			case CullVerdict::Precull:
				++counts.precull;
				break;
			case CullVerdict::Scope:
				++counts.scope;
				break;
			case CullVerdict::Cap:
				++counts.cap;
				break;
			default:
				break;
		}
	}

	template <class Member, class Better>
	void keep_best(size_t p_keep, Member p_member, Better p_better, CullVerdict p_drop) {
		const size_t n = v.size();
		for (size_t a = 0; a < n; ++a) {
			mark[a] = 0;
			if (!kept(a) || !p_member(a)) {
				continue;
			}
			size_t rank = 0;
			for (size_t b = 0; b < n && rank < p_keep; ++b) {
				if (b != a && kept(b) && p_member(b) && p_better(b, a)) {
					++rank;
				}
			}
			mark[a] = rank >= p_keep ? 1 : 0;
		}
		for (size_t a = 0; a < n; ++a) {
			if (mark[a] != 0) {
				cull(a, p_drop);
			}
		}
	}

	void duplicates(uint16_t p_scope, const SoundScope &p_s, uint32_t p_mix_rate) {
		const size_t n = v.size();
		const double window = double(p_s.duplicate_time_s) * double(p_mix_rate);

		size_t last = SIZE_MAX;
		for (size_t round = 0; round < n; ++round) {
			size_t next = SIZE_MAX;
			for (size_t a = 0; a < n; ++a) {
				if (!kept(a) || !v[a].starting || v[a].scope != p_scope) {
					continue;
				}
				if (last != SIZE_MAX && !older(v[last], last, v[a], a)) {
					continue;
				}
				if (next == SIZE_MAX || older(v[a], a, v[next], next)) {
					next = a;
				}
			}
			if (next == SIZE_MAX) {
				return;
			}
			last = next;
			for (size_t b = 0; b < n; ++b) {
				if (b == next || !kept(b) || v[b].scope != p_scope || !older(v[b], b, v[next], next)) {
					continue;
				}
				const double dt = double(v[next].start_frame) - double(v[b].start_frame);
				if (dt < window && distance_between(v[b], v[next]) <= p_s.duplicate_radius_m) {
					cull(next, CullVerdict::Scope);
					break;
				}
			}
		}
	}
};

}

void cull_voices(std::span<const CullVoice> p_voices, const ScopeTable *p_scopes, float p_precull_db, uint32_t p_max_voices, uint64_t p_frame, uint32_t p_mix_rate,
		std::span<CullVerdict> r_verdicts, CullCounts &r_counts) {
	(void)p_frame;
	const size_t n = std::min(p_voices.size(), r_verdicts.size());
	for (size_t i = 0; i < n; ++i) {
		r_verdicts[i] = CullVerdict::Keep;
	}
	if (p_voices.size() != r_verdicts.size() || n > kMaxCullVoices) {
		return;
	}
	Culler c{ p_voices, r_verdicts, r_counts };

	for (size_t i = 0; i < n; ++i) {
		const CullVoice &v = p_voices[i];
		if (v.starting && v.hdr && !(v.loudness_db >= p_precull_db)) {
			c.cull(i, CullVerdict::Precull);
		}
	}

	if (p_scopes != nullptr) {
		for (size_t i = 0; i < n; ++i) {
			const uint16_t s = p_voices[i].scope;
			if (s == kNoScope || s >= p_scopes->scopes.size()) {
				continue;
			}
			bool first = true;
			for (size_t j = 0; j < i && first; ++j) {
				first = p_voices[j].scope != s;
			}
			if (!first) {
				continue;
			}
			const SoundScope &sc = p_scopes->scopes[s];
			const auto member = [&](size_t k) { return p_voices[k].scope == s; };
			if (sc.duplicate_radius_m > 0.0f && sc.duplicate_time_s > 0.0f) {
				c.duplicates(s, sc, p_mix_rate);
			}
			if (sc.closest > 0) {
				c.keep_best(
						sc.closest, member,
						[&](size_t b, size_t a) {
							const float db = p_voices[b].distance_m, da = p_voices[a].distance_m;
							return db != da ? db < da : older(p_voices[b], b, p_voices[a], a);
						},
						CullVerdict::Scope);
			}
			if (sc.latest > 0) {
				c.keep_best(sc.latest, member, [&](size_t b, size_t a) { return older(p_voices[a], a, p_voices[b], b); }, CullVerdict::Scope);
			}
			if (sc.max_instances > 0) {
				c.keep_best(sc.max_instances, member, [&](size_t b, size_t a) { return older(p_voices[b], b, p_voices[a], a); }, CullVerdict::Scope);
			}
		}
	}

	const auto weight = [&](size_t k) { return uint64_t(std::clamp<uint32_t>(p_voices[k].voices, 1, kMaxCullVoices)); };
	uint64_t total = 0;
	uint64_t pt1_margin = 0;
	for (size_t i = 0; i < n; ++i) {
		total += c.kept(i) ? weight(i) : 0;
		pt1_margin = std::max<uint64_t>(pt1_margin, c.kept(i) ? std::min<uint32_t>(p_voices[i].resume_margin, kMaxCullVoices) : 0u);
	}
	if (total + pt1_margin > p_max_voices) {
		const auto score = [&](size_t k) {
			const float s = (p_voices[k].loudness_db + (std::isfinite(p_voices[k].hold_db) ? p_voices[k].hold_db : 0.0f)) * p_voices[k].priority;
			return std::isfinite(s) ? s : -std::numeric_limits<float>::max();
		};
		const auto better = [&](size_t b, size_t a) {
			const float sb = score(b), sa = score(a);
			return sb != sa ? sb > sa : older(p_voices[b], b, p_voices[a], a);
		};
		for (size_t a = 0; a < n; ++a) {
			c.mark[a] = 0;
			if (!c.kept(a)) {
				continue;
			}
			uint64_t before = weight(a) + std::min<uint32_t>(p_voices[a].resume_margin, kMaxCullVoices);
			for (size_t b = 0; b < n && before <= p_max_voices; ++b) {
				if (b != a && c.kept(b) && better(b, a)) {
					before += weight(b);
				}
			}
			c.mark[a] = before > p_max_voices ? 1 : 0;
		}
		for (size_t a = 0; a < n; ++a) {
			if (c.mark[a] != 0) {
				c.cull(a, CullVerdict::Cap);
			}
		}
	}
}

MixerA3Assets load_mixer_a3(const core::EbxResolver &p_db, std::span<const std::shared_ptr<const core::audio::SoundProgram>> p_programs) {
	MixerA3Assets out;
	core::Result<core::audio::HdrSetting> hdr = core::audio::read_hdr_setting(p_db);
	if (hdr) {
		out.hdr = *hdr;
	} else {
		out.notes += "A3: no HDR window (fixed window kept): " + hdr.error().message + "\n";
	}
	core::Result<core::audio::ScopeTable> scopes = core::audio::read_scopes(p_db);
	if (scopes) {
		std::vector<core::Guid> patches;
		patches.reserve(p_programs.size());
		for (const std::shared_ptr<const core::audio::SoundProgram> &p : p_programs) {
			if (p) {
				patches.push_back(p->partition);
			}
		}
		std::sort(patches.begin(), patches.end());
		patches.erase(std::unique(patches.begin(), patches.end()), patches.end());
		const core::Status mapped = core::audio::map_patch_scopes(p_db, patches, *scopes, nullptr, &out.skipped_patches);
		if (!mapped.ok()) {
			out.notes += "A3: patch scopes not mapped: " + mapped.error().message + "\n";
			scopes->patch_scopes.clear();
		}
		out.scoped_patches = scopes->patch_scopes.size();
		out.scopes = std::make_shared<const core::audio::ScopeTable>(std::move(*scopes));
	} else {
		out.notes += "A3: no scopes: " + scopes.error().message + "\n";
	}
	return out;
}

void apply_mixer_a3(const MixerA3Assets &p_assets, MixerConfig &r_config) {
	r_config.hdr = p_assets.hdr;
	r_config.scopes = p_assets.scopes;
}

}
