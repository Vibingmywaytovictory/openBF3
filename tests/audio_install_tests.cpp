// M2a audio install suites (docs/design/m2a-audio.md §10.2). Each exits 77 (skipped) without an install, reads it
// only through the repo readers and checks install.unchanged (Install::snapshot before and after).
//   audio-oracles  gate G5 items 2-7: evaluated by the interpreter on compiled programs (item 1, the census, is
//                  audio.census / G4). The M16A3's node, voice and register numbers are those of its compiled program
//                  (design §4.5; registers are the data's ValueIndex values).
//   audio-cache    the compiled-program cache (design §8): cold -> converted, warm -> hit, a patched partition
//                  (an in-memory digest override) -> converted again, into a scratch cache root.
//   audio-fuzz     seeded mutations of real XAS chunks, the M16A3 patch partition and its program blob (asan
//                  preset): every mutant fails with an error or runs; no crash, hang or unbounded allocation.
//                  OPENBF3_FUZZ_ITERS overrides the 250 mutants per input.
#include "test_support.h"

#include "bf3/audio/audio_load.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/sound_stream.h"
#include "bf3/core/audio/sound_types.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_blob.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"

#include <doctest/doctest.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::audio;
namespace cp = bf3::core::audio::port;
namespace ba = bf3::audio;
namespace fs = std::filesystem;
using ba::SamplerTraceEntry;
using ba::SamplerTraceKind;
using Names = std::set<std::string>;

namespace {

constexpr const char *kM16 = "sound/weapons/handheld/m16a3/weapon_assaultrifle_m16a4_02";
constexpr const char *kM98b = "sound/weapons/handheld/m98_b/weapon_sniper_m98_b_02";
constexpr const char *kRpg7 = "sound/weapons/handheld/rpg-7/weapon_at_rpg7";
constexpr const char *kM1a2 = "sound/vehicles/land/m1a2_abrams/m1a2_abrams";

constexpr uint32_t kFire = 2088671869u;
constexpr uint32_t kStop = 1920007028u;
constexpr uint32_t kStep = 1968287961u;
constexpr uint32_t kIsFirstPerson = 824639024u;
constexpr uint32_t kEnableStep = 4198762218u;

// One install, the Metro TDM level, the M16A3 program and a bank of exactly its waves, for every case of the
// process (the snapshot is taken before anything is read).
struct Fixture {
	InstallReader reader;
	InstallSnapshot before;
	std::shared_ptr<Install> install;
	std::unique_ptr<level::LoadedLevel> metro;
	std::shared_ptr<const SoundProgram> m16;
	std::vector<SoundWaveInfo> m16_waves;
	std::shared_ptr<const WaveBank> m16_bank;
	std::unique_ptr<EbxLibrary> all_mp; // lazy: the other oracle patches
};

Fixture *fixture() {
	static std::unique_ptr<Fixture> f;
	static bool tried = false;
	if (tried) {
		return f.get();
	}
	tried = true;
	auto made = std::make_unique<Fixture>();
	if (!made->reader.locate({})) {
		return nullptr;
	}
	made->before = made->reader.snapshot();
	Result<std::shared_ptr<Install>> install = Install::open(made->reader);
	REQUIRE_MESSAGE(install.ok(), (install.ok() ? std::string() : install.error().message));
	made->install = *install;
	Result<std::unique_ptr<level::LoadedLevel>> lvl = level::load_level(made->install, "MP_Subway", "TeamDeathMatch0");
	REQUIRE_MESSAGE(lvl.ok(), (lvl.ok() ? std::string() : lvl.error().message));
	made->metro = std::move(*lvl);
	Result<std::shared_ptr<const SoundProgram>> prog = ba::compile_patch(made->metro->db(), kM16);
	REQUIRE_MESSAGE(prog.ok(), (prog.ok() ? std::string() : prog.error().message));
	made->m16 = *prog;
	size_t missing = 0;
	Result<std::vector<SoundWaveInfo>> waves =
			ba::read_program_waves(made->metro->db(), std::span<const std::shared_ptr<const SoundProgram>>(&made->m16, 1), &missing);
	REQUIRE(waves.ok());
	REQUIRE(missing == 0);
	made->m16_waves = *waves;
	Result<std::shared_ptr<const WaveBank>> bank = WaveBank::load(*made->install, made->metro->mount(), *waves, WaveBankOptions{});
	REQUIRE_MESSAGE(bank.ok(), (bank.ok() ? std::string() : bank.error().message));
	made->m16_bank = *bank;
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(made->install, expand_superbundles(*made->install, "@all-mp"));
	REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
	made->all_mp = std::move(*lib);
	f = std::move(made);
	return f.get();
}

// The fixture, or a skip (77) when there is no install.
Fixture *need_install() {
	Fixture *f = fixture();
	if (f == nullptr) {
		bf3::test::skip_install_case("no BF3 install found");
	}
	return f;
}

void check_unchanged(const Fixture &p_fixture) {
	const SnapshotDiff diff = diff_snapshots(p_fixture.before, p_fixture.reader.snapshot());
	CHECK_MESSAGE(diff.clean(), diff.summary());
}

std::unique_ptr<ba::PatchInstance> instance(std::shared_ptr<const SoundProgram> p_prog, std::shared_ptr<const WaveBank> p_bank, uint64_t p_seed) {
	Result<std::unique_ptr<ba::PatchInstance>> r = ba::PatchInstance::create(std::move(p_prog), std::move(p_bank), ba::AudioTuning{}, p_seed, 48000);
	REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
	return std::move(*r);
}

void place(ba::PatchInstance &r_inst, float p_metres) {
	ba::EmitterState e;
	e.position = ba::Vec3{ 0.0f, 0.0f, p_metres };
	r_inst.set_geometry(e, ba::ListenerState{});
}

void at(ba::PatchInstance &r_inst, float p_metres) {
	place(r_inst, p_metres);
	r_inst.evaluate(0.0);
}

struct Buses {
	std::vector<float> storage = std::vector<float>(size_t(MasterBus::Unknown) * 2 * 512, 0.0f);
	ba::BusBuffers b;
	Buses() {
		for (size_t i = 0; i < size_t(MasterBus::Unknown); ++i) {
			b.channels[i][0] = storage.data() + (i * 2) * 512;
			b.channels[i][1] = storage.data() + (i * 2 + 1) * 512;
		}
	}
	void block(ba::PatchInstance &r_inst, uint64_t p_first) {
		std::fill(storage.begin(), storage.end(), 0.0f);
		b.frames = 512;
		b.first_frame = p_first;
		r_inst.process_block(b);
	}
};

// The crossfader groups of a program: node, group, band.
struct Band {
	uint32_t node = 0, group = 0;
	float begin = 0.0f, end = 0.0f;
};
std::vector<Band> crossfader_bands(const SoundProgram &p_prog) {
	std::vector<Band> out;
	for (uint32_t ni = 0; ni < p_prog.nodes.size(); ++ni) {
		const SoundNode &n = p_prog.nodes[ni];
		if (n.type != SoundNodeType::MultiCrossfader) {
			continue;
		}
		for (uint32_t g = 0; g < n.children_count; ++g) {
			const SoundNode &c = p_prog.child_nodes[p_prog.children[n.children_begin + g]];
			out.push_back(Band{ ni, g, p_prog.floats[c.floats_begin + cp::GroupFadeBegin], p_prog.floats[c.floats_begin + cp::GroupFadeEnd] });
		}
	}
	return out;
}
bool has_band(const std::vector<Band> &p_bands, float p_begin, float p_end) {
	return std::any_of(p_bands.begin(), p_bands.end(), [&](const Band &b) { return b.begin == p_begin && b.end == p_end; });
}

// Sweeps distance 0-400 m in 0.25 m steps and checks every crossfader group of p_prog under reading R: outside the
// band it fades in over (the previous group's) and the band it fades out over, the amplitude is exactly 0 or 1;
// inside each band it is monotonic; across each band it swings by the full 1; and the first and last non-0/1 values
// lie within 0.25 m of the band edges. Returns the number of groups checked.
size_t check_bands(const char *p_name, std::shared_ptr<const SoundProgram> p_prog) {
	const std::vector<Band> bands = crossfader_bands(*p_prog);
	auto inst = instance(p_prog, nullptr, 1);
	constexpr size_t kSteps = 1601;
	std::map<std::pair<uint32_t, uint32_t>, std::vector<float>> amp;
	for (size_t i = 0; i < kSteps; ++i) {
		at(*inst, 0.25f * float(i));
		for (const Band &b : bands) {
			amp[{ b.node, b.group }].push_back(inst->crossfader_group_amplitude(b.node, b.group));
		}
	}
	size_t checked = 0;
	for (size_t k = 0; k < bands.size(); ++k) {
		const Band &b = bands[k];
		const std::vector<float> &a = amp[{ b.node, b.group }];
		std::vector<std::pair<float, float>> fades;
		std::vector<int> dirs; // +1 fades in, -1 fades out
		if (b.group > 0) {
			fades.push_back({ bands[k - 1].begin, bands[k - 1].end });
			dirs.push_back(+1);
		}
		fades.push_back({ b.begin, b.end });
		dirs.push_back(-1);
		bool ok = true;
		std::string why;
		float first_partial = -1.0f, last_partial = -1.0f;
		for (size_t i = 0; i < kSteps; ++i) {
			const float d = 0.25f * float(i);
			const bool inside = std::any_of(fades.begin(), fades.end(), [&](const auto &f) { return d > f.first && d < f.second; });
			if (a[i] != 0.0f && a[i] != 1.0f) {
				if (first_partial < 0.0f) {
					first_partial = d;
				}
				last_partial = d;
				if (!inside) {
					ok = false;
					why = "partial amplitude " + std::to_string(a[i]) + " outside the bands at " + std::to_string(d) + " m";
				}
			}
		}
		for (size_t fi = 0; fi < fades.size(); ++fi) {
			const size_t i0 = size_t(std::lround(fades[fi].first / 0.25f));
			const size_t i1 = std::min(kSteps - 1, size_t(std::lround(fades[fi].second / 0.25f)));
			for (size_t i = i0 + 1; i <= i1; ++i) {
				if ((dirs[fi] > 0 && a[i] < a[i - 1]) || (dirs[fi] < 0 && a[i] > a[i - 1])) {
					ok = false;
					why = "not monotonic at " + std::to_string(0.25f * float(i)) + " m";
				}
			}
			const float swing = a[i1] - a[i0];
			if (std::fabs(swing) != 1.0f) {
				ok = false;
				why = "swing " + std::to_string(swing) + " over [" + std::to_string(fades[fi].first) + ", " + std::to_string(fades[fi].second) + "]";
			}
		}
		if (first_partial >= 0.0f) {
			const float lo = fades.front().first, hi = fades.back().second;
			if (first_partial < lo - 0.25f || last_partial > hi + 0.25f) {
				ok = false;
				why = "changes from " + std::to_string(first_partial) + " to " + std::to_string(last_partial) + " m";
			}
		}
		CHECK_MESSAGE(ok, p_name, " node ", b.node, " group ", b.group, " [", b.begin, ", ", b.end, "]: ", why);
		++checked;
	}
	return checked;
}

std::shared_ptr<const SoundProgram> compile_all_mp(const Fixture &p_f, const char *p_name, bool p_allow_unimplemented) {
	CompileOptions o;
	o.allow_unimplemented = p_allow_unimplemented;
	Result<std::shared_ptr<const SoundProgram>> r = ba::compile_patch(*p_f.all_mp, p_name, o);
	REQUIRE_MESSAGE(r.ok(), p_name, ": ", (r.ok() ? std::string() : r.error().message));
	return *r;
}

// The M16A3 samplers by their first slot's voice (design §4.5).
const std::map<uint16_t, std::string> &m16_sampler_names() {
	static const std::map<uint16_t, std::string> names{ { 28, "corebassclose" }, { 26, "corebassdistant" }, { 18, "hifi_lmg" }, { 15, "noise_close" },
		{ 22, "noise_distant" }, { 20, "noise_far" }, { 24, "lfe" }, { 9, "rattle" }, { 7, "reflections_close" }, { 8, "reflections_far" },
		{ 17, "reload" }, { 13, "handling" }, { 12, "shell_casings" } };
	return names;
}
std::string sampler_name(const SoundProgram &p_prog, uint32_t p_node) {
	const SoundNode &n = p_prog.nodes[p_node];
	const uint16_t voice = n.plugins_count ? p_prog.plugin_refs[n.plugins_begin].voice : kNoRegister;
	const auto it = m16_sampler_names().find(voice);
	return it == m16_sampler_names().end() ? "node" + std::to_string(p_node) : it->second;
}

struct StartedSets {
	std::set<std::string> before_stop, at_stop;
	uint64_t starved = 0;
};
// Fire at p_metres (EnableStep 1, 9 Steps 75 ms apart), render to 0.75 s, then the stop and 1.5 s more; the samplers
// whose Trigger fired in each part.
StartedSets started(const Fixture &p_f, float p_metres, bool p_first_person) {
	auto inst = instance(p_f.m16, p_f.m16_bank, 0x5eed);
	ba::SamplerTrace trace;
	trace.entries.resize(4096);
	inst->set_sampler_trace(&trace);
	inst->set_parameter(kEnableStep, 1.0f);
	inst->set_parameter(kIsFirstPerson, p_first_person ? 1.0f : 0.0f);
	place(*inst, p_metres);
	Buses buses;
	StartedSets out;
	const uint64_t stop_frame = 36000;
	inst->post_event(kFire, 0);
	bool stopped = false;
	for (uint64_t frame = 0; frame < stop_frame + 72000; frame += 512) {
		for (uint64_t k = 1; k <= 9; ++k) {
			const uint64_t s = k * 3600;
			if (s >= frame && s < frame + 512) {
				inst->post_event(kStep, uint32_t(s - frame));
			}
		}
		if (!stopped && stop_frame >= frame && stop_frame < frame + 512) {
			inst->post_event(kStop, uint32_t(stop_frame - frame));
			stopped = true;
		}
		buses.block(*inst, frame);
	}
	for (uint32_t i = 0; i < trace.count; ++i) {
		const SamplerTraceEntry &e = trace.entries[i];
		if (e.kind == SamplerTraceKind::Trigger) {
			(e.frame < stop_frame ? out.before_stop : out.at_stop).insert(sampler_name(*p_f.m16, e.node));
		}
	}
	CHECK(trace.dropped == 0);
	out.starved = inst->stats().starved_slots;
	return out;
}

std::string join(const std::set<std::string> &p_set) {
	std::string s;
	for (const std::string &x : p_set) {
		s += (s.empty() ? "" : ",") + x;
	}
	return s;
}

// ---- fuzz helpers (the core.fuzz mutator) ---------------------------------------------------------------------

using Bytes = std::vector<uint8_t>;

size_t fuzz_iters(size_t p_default) {
	char buf[32] = {};
	size_t len = 0;
	if (getenv_s(&len, buf, sizeof(buf), "OPENBF3_FUZZ_ITERS") == 0 && len > 1) {
		const long v = std::strtol(buf, nullptr, 10);
		if (v > 0) {
			return size_t(v);
		}
	}
	return p_default;
}

void mutate(Bytes &r_bytes, std::mt19937_64 &r_rng, size_t p_hot = 0x200) {
	if (r_bytes.empty()) {
		return;
	}
	const unsigned mode = unsigned(r_rng() % 5);
	if (mode == 0) {
		r_bytes.resize(size_t(r_rng() % r_bytes.size()));
		return;
	}
	const unsigned count = 1 + unsigned(r_rng() % 8);
	for (unsigned i = 0; i < count; ++i) {
		const size_t at = (r_rng() % 3 == 0) ? size_t(r_rng() % std::min<size_t>(r_bytes.size(), p_hot)) : size_t(r_rng() % r_bytes.size());
		if (mode == 1) {
			r_bytes[at] ^= uint8_t(1u << (r_rng() % 8));
		} else if (mode == 2) {
			r_bytes[at] = uint8_t(r_rng());
		} else if (mode == 3 && at + 4 <= r_bytes.size()) {
			const uint32_t v = (r_rng() % 2) ? 0xFFFFFFFFu : uint32_t(r_rng());
			std::memcpy(r_bytes.data() + at, &v, 4);
		} else {
			r_bytes[at] = 0;
		}
	}
}

// A program that decoded or compiled: it must validate, instantiate and render a few blocks with events.
void run_program(SoundProgram p_prog, std::shared_ptr<const WaveBank> p_bank) {
	REQUIRE(validate_sound_program(p_prog).ok());
	auto prog = std::make_shared<SoundProgram>(std::move(p_prog));
	Result<std::unique_ptr<ba::PatchInstance>> inst = ba::PatchInstance::create(prog, std::move(p_bank), ba::AudioTuning{}, 3, 48000);
	if (!inst.ok()) {
		return;
	}
	Buses buses;
	place(**inst, 10.0f);
	(*inst)->set_parameter(kEnableStep, 1.0f);
	(*inst)->post_event(kFire, 0);
	for (uint64_t fr = 0; fr < 8 * 512; fr += 512) {
		if (fr == 2048) {
			(*inst)->post_event(kStep, 5);
			(*inst)->post_event(kStop, 300);
		}
		buses.block(**inst, fr);
	}
}

std::atomic<int> g_scratch{ 0 };
struct ScratchCache {
	fs::path base;
	ScratchCache() {
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		base = fs::path(tmp) / (L"openbf3-audio-cache-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
									   std::to_wstring(g_scratch++));
		fs::create_directories(base);
	}
	~ScratchCache() {
		std::error_code ec;
		fs::remove_all(base, ec);
	}
};

} // namespace

// =====================================================================================================================

TEST_SUITE("audio-oracles") {
	TEST_CASE("G5.2 M16A3 bands under reading R; the 301 m Fire gate") {
		Fixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::vector<Band> bands = crossfader_bands(*f->m16);
		const std::vector<std::pair<float, float>> want{ { 300.0f, 301.0f }, { 1.0f, 19.0f }, { 5.0f, 35.0f }, { 250.0f, 300.0f }, { 1.0f, 5.0f },
			{ 20.0f, 50.0f }, { 3.0f, 15.0f }, { 1.0f, 17.0f }, { 0.0f, 20.0f }, { 1.0f, 15.0f } };
		for (auto [b, e] : want) {
			CHECK_MESSAGE(has_band(bands, b, e), "band ", b, "-", e);
		}
		CHECK(bands.size() == 12); // 1 + 1 + 2 + 3 + 2 + 1 + 1 + 1 over the 8 crossfaders
		CHECK(check_bands(kM16, f->m16) == 12);
		auto inst = instance(f->m16, nullptr, 1);
		const auto v = [&](uint16_t r) { return inst->value(r); };
		at(*inst, 0.0f);
		CHECK(v(68) == 1.0f); // hifi
		for (float d : { 20.0f, 25.0f, 100.0f }) {
			at(*inst, d);
			CHECK(v(68) == 0.0f);
		}
		// handling layers: 1-15 (node 74), 1-17 (node 61, shell casings), 1-19 (node 6, reload)
		for (auto [node, edge] : { std::pair{ 74u, 15.0f }, std::pair{ 61u, 17.0f }, std::pair{ 6u, 19.0f } }) {
			at(*inst, edge - 1.0f);
			CHECK(inst->crossfader_group_amplitude(node, 0) > 0.0f);
			for (float d : { edge, edge + 0.25f, 200.0f }) {
				at(*inst, d);
				CHECK_MESSAGE(inst->crossfader_group_amplitude(node, 0) == 0.0f, "node ", node, " at ", d);
			}
		}
		// the far noise group v87: rises over 20-50, falls over 250-300
		const std::pair<float, float> v87[] = { { 20.0f, 0.0f }, { 35.0f, 0.5f }, { 50.0f, 1.0f }, { 250.0f, 1.0f }, { 275.0f, 0.5f }, { 300.0f, 0.0f } };
		for (auto [d, want87] : v87) {
			at(*inst, d);
			CHECK_MESSAGE(std::fabs(v(87) - want87) <= 1e-5f, "v87 at ", d, " m: ", v(87));
		}
		// The Fire gate: nothing starts at 302 m; something does at 299 m.
		for (auto [d, expect] : { std::pair{ 302.0f, false }, std::pair{ 299.0f, true } }) {
			auto g = instance(f->m16, f->m16_bank, 2);
			ba::SamplerTrace trace;
			trace.entries.resize(256);
			g->set_sampler_trace(&trace);
			g->set_parameter(kEnableStep, 1.0f);
			place(*g, d);
			Buses buses;
			g->post_event(kFire, 0);
			for (uint64_t fr = 0; fr < 48000; fr += 512) {
				buses.block(*g, fr);
			}
			CHECK_MESSAGE((g->stats().sampler_triggers > 0) == expect, "at ", d, " m: ", g->stats().sampler_triggers, " triggers");
			CHECK((trace.count > 0) == expect);
		}
		check_unchanged(*f);
	}

	TEST_CASE("G5.3 M16A3 pitch per burst, the Lfo, and the two distance HPFs") {
		Fixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		// 100 seeds x 100 bursts = 10,000 draws of v28; constant across the 9 Steps of each burst.
		float lo = 10.0f, hi = -10.0f;
		size_t bursts = 0, changed = 0;
		for (uint64_t seed = 1; seed <= 100; ++seed) {
			auto inst = instance(f->m16, nullptr, seed);
			inst->set_parameter(kEnableStep, 1.0f);
			place(*inst, 10.0f);
			inst->evaluate(0.01);
			for (int b = 0; b < 100; ++b) {
				inst->post_event(kFire, 0);
				inst->evaluate(0.075);
				const float v28 = inst->value(28);
				lo = std::min(lo, v28);
				hi = std::max(hi, v28);
				for (int s = 0; s < 9; ++s) {
					inst->post_event(kStep, 0);
					inst->evaluate(0.075);
					changed += inst->value(28) != v28 ? 1 : 0;
				}
				inst->post_event(kStop, 0);
				inst->evaluate(0.5);
				++bursts;
			}
		}
		CHECK(bursts == 10000);
		CHECK(changed == 0);
		CHECK(lo >= 1.21f);
		CHECK(hi <= 1.262f);
		CHECK(lo <= 1.21f * 1.005f);
		CHECK(hi >= 1.262f * 0.995f);
		MESSAGE("v28 over 10,000 bursts: [", lo, ", ", hi, "]");

		// Lfo v29 once the Adsr v75 is at full amplitude: range [0.96, 1.0], period 250 ms +- 1 ms; v30 = v28 x v29.
		{
			auto inst = instance(f->m16, nullptr, 9);
			inst->set_parameter(kEnableStep, 1.0f);
			place(*inst, 10.0f);
			inst->evaluate(0.0);
			inst->post_event(kFire, 0);
			const double dt = 0.0005;
			double t = 0.0;
			float mn = 10.0f, mx = -10.0f;
			std::vector<double> rising;
			float prev = 0.0f;
			bool have_prev = false, product_ok = true;
			for (int i = 0; i < 6000; ++i) {
				inst->evaluate(dt);
				t += dt;
				const float v29 = inst->value(29);
				product_ok = product_ok && inst->value(30) == inst->value(28) * v29;
				if (inst->value(75) >= 1.0f) {
					mn = std::min(mn, v29);
					mx = std::max(mx, v29);
					if (have_prev && prev < 0.98f && v29 >= 0.98f) {
						rising.push_back(t - dt + dt * double(0.98f - prev) / double(v29 - prev));
					}
					prev = v29;
					have_prev = true;
				}
			}
			CHECK(product_ok);
			CHECK(mn >= 0.96f - 1e-6f);
			CHECK(mx <= 1.0f + 1e-6f);
			CHECK(mn == doctest::Approx(0.96).epsilon(1e-4));
			CHECK(mx == doctest::Approx(1.0).epsilon(1e-4));
			REQUIRE(rising.size() >= 4);
			for (size_t i = 1; i < rising.size(); ++i) {
				CHECK(std::fabs((rising[i] - rising[i - 1]) - 0.25) <= 0.001);
			}
		}

		// The voice-14 HPF (v46 -> plugin 14.2) and the voice-6 HPF (v49 -> plugin 6.1).
		auto inst = instance(f->m16, nullptr, 1);
		const SoundPluginRef hpf14{ 14, 2 }, hpf6{ 6, 1 };
		const std::pair<float, float> v14[] = { { 0.0f, 0.0f }, { 17.0f, 0.0f }, { 108.5f, 155.0f }, { 150.0f, 225.30f }, { 200.0f, 310.0f }, { 300.0f, 310.0f } };
		for (auto [d, hz] : v14) {
			at(*inst, d);
			CHECK_MESSAGE(std::fabs(inst->plugin_attribute(hpf14, 0) - hz) <= 0.01f, "voice 14 HPF at ", d, " m: ", inst->plugin_attribute(hpf14, 0));
		}
		const std::pair<float, float> v6[] = { { 0.0f, 80.0f }, { 17.0f, 119.15f }, { 150.0f, 230.0f } };
		for (auto [d, hz] : v6) {
			at(*inst, d);
			CHECK_MESSAGE(std::fabs(inst->plugin_attribute(hpf6, 0) - hz) <= 0.01f, "voice 6 HPF at ", d, " m: ", inst->plugin_attribute(hpf6, 0));
		}
		inst->set_parameter(kIsFirstPerson, 1.0f);
		at(*inst, 17.0f);
		CHECK_MESSAGE(std::fabs(inst->plugin_attribute(hpf6, 0) - 83.90f) <= 0.01f, "first person: ", inst->plugin_attribute(hpf6, 0));
		check_unchanged(*f);
	}

	TEST_CASE("G5.4 other bands: M98B, RPG-7, M1A2") {
		Fixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const std::shared_ptr<const SoundProgram> m98b = compile_all_mp(*f, kM98b, false);
		const std::shared_ptr<const SoundProgram> rpg7 = compile_all_mp(*f, kRpg7, false);
		// The M1A2 has non-weapon node types: allow_unimplemented. Its crossfaders read v81, which is its Physics
		// node's Distance output (the weapons' v22/v23/v24), so distance drives them like the weapons'.
		const std::shared_ptr<const SoundProgram> m1a2 = compile_all_mp(*f, kM1a2, true);
		CHECK(has_band(crossfader_bands(*m98b), 300, 350));
		CHECK(has_band(crossfader_bands(*m98b), 300, 351));
		CHECK(has_band(crossfader_bands(*rpg7), 45, 90));
		CHECK(has_band(crossfader_bands(*m1a2), 15, 50));
		CHECK(has_band(crossfader_bands(*m1a2), 80, 100));
		const size_t n98 = check_bands(kM98b, m98b), nrpg = check_bands(kRpg7, rpg7), nm1 = check_bands(kM1a2, m1a2);
		MESSAGE("groups checked: M98B ", n98, ", RPG-7 ", nrpg, ", M1A2 ", nm1);
		CHECK(n98 == 13);
		CHECK(nrpg == 11);
		CHECK(nm1 == 7);
		check_unchanged(*f);
	}

	TEST_CASE("G5.5 the Cone curve: Y + Gain at its points, monotonic between") {
		Fixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		// Output 4 is the Cone "Cone" (Gain 0) on voice 0.
		REQUIRE(f->m16->outputs.size() == 5);
		const SoundOutput &cone = f->m16->outputs[4];
		CHECK(cone.kind == OutputKind::Cone);
		CHECK(cone.gain_db == 0.0f);
		REQUIRE(cone.attenuation.points.size() == 6);
		auto inst = instance(f->m16, nullptr, 1);
		const std::pair<float, float> pts[] = { { 19.766f, -17.917f }, { 54.614f, -34.566f }, { 91.156f, -41.149f }, { 120.438f, -46.957f },
			{ 179.002f, -67.091f } };
		for (auto [x, y] : pts) {
			at(*inst, x);
			CHECK_MESSAGE(std::fabs(inst->output_gain_db(4) - (y + cone.gain_db)) <= 0.01f, "at ", x, " m: ", inst->output_gain_db(4));
		}
		float prev = 1e9f;
		bool monotonic = true;
		for (int i = 0; i <= 800; ++i) {
			at(*inst, 0.25f * float(i));
			const float g = inst->output_gain_db(4);
			monotonic = monotonic && g <= prev + 1e-4f;
			prev = g;
		}
		CHECK(monotonic);
		check_unchanged(*f);
	}

	TEST_CASE("G5.6 M16A3 routing and audible sets (third person), first-person lfe and rattle") {
		Fixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const SoundProgram &p = *f->m16;
		const auto sends = [&](uint16_t voice) {
			std::vector<SoundConnection> out;
			const SoundVoice &sv = p.voices[voice];
			for (uint32_t k = 0; k < sv.plugins_count; ++k) {
				const SoundVoicePlugin &pl = p.plugins[sv.plugins_begin + k];
				if (pl.connection != kNoRegister) {
					out.push_back(p.connections[pl.connection]);
				}
			}
			return out;
		};
		const std::vector<SoundConnection> s4 = sends(4), s14 = sends(14);
		REQUIRE(s4.size() == 1);
		CHECK(s4[0].to_bus);
		CHECK(s4[0].bus == MasterBus::Lfe);
		REQUIRE(s14.size() == 2);
		CHECK((!s14[0].to_bus && !s14[1].to_bus));
		CHECK((std::set<uint16_t>{ s14[0].voice, s14[1].voice } == std::set<uint16_t>{ 1, 5 }));

		const StartedSets close = started(*f, 0.5f, false);
		CHECK_MESSAGE((close.before_stop == Names{ "corebassclose", "hifi_lmg", "noise_close" }), join(close.before_stop));
		CHECK_MESSAGE((close.at_stop == Names{ "reflections_close", "shell_casings" }), join(close.at_stop));
		for (float d : { 60.0f, 150.0f }) {
			const StartedSets away = started(*f, d, false);
			CHECK_MESSAGE((away.before_stop == Names{ "corebassdistant", "noise_far" }), d, " m: ", join(away.before_stop));
			if (d == 150.0f) {
				CHECK_MESSAGE((away.at_stop == Names{ "reflections_far" }), join(away.at_stop));
			}
			CHECK(away.starved == 0);
		}
		const StartedSets fp = started(*f, 0.5f, true);
		CHECK_MESSAGE(fp.before_stop.count("lfe") == 1, join(fp.before_stop));
		CHECK_MESSAGE(fp.before_stop.count("rattle") == 1, join(fp.before_stop));
		CHECK(close.before_stop.count("lfe") + close.before_stop.count("rattle") == 0);

		auto inst = instance(f->m16, nullptr, 1);
		at(*inst, 150.0f);
		CHECK(inst->plugin_attribute(SoundPluginRef{ 3, 1 }, 0) == 0.0f);
		CHECK(inst->plugin_attribute(SoundPluginRef{ 5, 1 }, 0) == doctest::Approx(0.5));
		at(*inst, 0.5f);
		CHECK(inst->plugin_attribute(SoundPluginRef{ 5, 1 }, 0) == 0.0f);
		check_unchanged(*f);
	}

	TEST_CASE("G5.7 fire mode: EnableStep 0 releases with the Fire, 1 only with the stop") {
		Fixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		for (float mode : { 0.0f, 1.0f }) {
			auto inst = instance(f->m16, nullptr, 1);
			inst->set_parameter(kEnableStep, mode);
			at(*inst, 10.0f);
			inst->post_event(kFire, 0);
			inst->evaluate(0.01);
			CHECK(inst->value(97) == 1.0f); // the gated Fire
			CHECK(inst->value(100) == (mode == 0.0f ? 1.0f : 0.0f));
			inst->evaluate(0.01);
			CHECK(inst->value(100) == 0.0f);
			inst->post_event(kStop, 0);
			inst->evaluate(0.01);
			if (mode == 1.0f) {
				CHECK(inst->value(100) == 1.0f);
			}
		}
		check_unchanged(*f);
	}
}

// =====================================================================================================================

TEST_SUITE("audio-cache") {
	TEST_CASE("compiled programs: cold, warm, patched patch, patched dependency") {
		Fixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const EbxPartition *part = f->metro->db().partition_by_name(kM16);
		REQUIRE(part != nullptr);
		gameplay::WorkBudget budget;
		CompileStats stats;
		Result<SoundProgram> direct = compile_sound_patch(f->metro->db(), *part, CompileOptions{}, budget, &stats);
		REQUIRE(direct.ok());
		REQUIRE(!stats.dependencies.empty());
		const std::vector<uint8_t> want = encode_sound_program(*direct);
		CHECK(want == encode_sound_program(*f->m16));

		ScratchCache scratch;
		const PathGuard guard = f->reader.write_guard();
		const gameplay::PartitionDigest real = gameplay::mount_partition_digest(f->metro->mount());
		std::string patched_name;
		const gameplay::PartitionDigest patched = [&](std::string_view p_name) -> std::optional<Sha1> {
			std::optional<Sha1> d = real(p_name);
			if (d && p_name == patched_name) {
				d->bytes[0] ^= 0xA5;
			}
			return d;
		};
		SoundProgramSources sources;
		sources.db = &f->metro->db();
		sources.digest = patched;
		const auto run = [&](CacheOutcome p_expect, const char *p_step) {
			Cache cache;
			CacheOptions o;
			o.root = scratch.base;
			o.guard = guard;
			REQUIRE(cache.open(o) == CacheStatus::Ok);
			Result<CachedSoundProgram> r = compile_sound_patch_cached(&cache, sources, kM16, CompileOptions{});
			REQUIRE_MESSAGE(r.ok(), p_step, ": ", (r.ok() ? std::string() : r.error().message));
			CHECK_MESSAGE(r->item.outcome == p_expect, p_step, ": ", std::string(cache_outcome_name(r->item.outcome)), " ", r->item.cache_error);
			CHECK_MESSAGE(encode_sound_program(r->program) == want, p_step);
		};
		run(CacheOutcome::Converted, "cold");
		run(CacheOutcome::Hit, "warm");
		patched_name = kM16;
		run(CacheOutcome::Converted, "patched patch");
		run(CacheOutcome::Hit, "patched patch, warm");
		patched_name = stats.dependencies.front();
		MESSAGE("patched dependency: ", patched_name);
		run(CacheOutcome::Converted, "patched dependency");
		run(CacheOutcome::Hit, "patched dependency, warm");
		patched_name.clear();
		run(CacheOutcome::Hit, "original again");
		check_unchanged(*f);
	}
}

// =====================================================================================================================

TEST_SUITE("audio-fuzz") {
	TEST_CASE("mutated XAS chunks, the M16A3 patch partition and its program blob") {
		Fixture *f = need_install();
		if (f == nullptr) {
			return;
		}
		const size_t iters = fuzz_iters(250);
		std::mt19937_64 rng(0xA0D10F22u);

		// XAS: three real chunks (the first variation of three M16A3 waves); every segment stream of each mutant is
		// scanned and read through a StreamDecoder to its end or its first error.
		size_t chunks = 0, streams_ok = 0, streams_failed = 0;
		uint64_t frames = 0;
		for (size_t w = 0; w < f->m16_waves.size() && chunks < 3; ++w) {
			const SoundWaveInfo &info = f->m16_waves[w];
			if (info.chunks.empty() || info.variations.empty() || info.localised()) {
				continue;
			}
			const WaveVariation &var = info.variations.front();
			if (var.chunk_index >= info.chunks.size()) {
				continue;
			}
			Result<Bytes> chunk = f->metro->mount().read_chunk(info.chunks[var.chunk_index].id);
			REQUIRE_MESSAGE(chunk.ok(), info.name);
			++chunks;
			for (size_t i = 0; i < iters; ++i) {
				Bytes b = *chunk;
				mutate(b, rng, 0x400);
				for (const WaveSegment &seg : var.segments) {
					Result<StreamScan> scan = scan_stream(b, seg.samples_offset, true);
					StreamDecoder dec;
					if (!dec.open(b, seg.samples_offset).ok()) {
						++streams_failed;
						continue;
					}
					std::vector<float> buf(size_t(256) * dec.header().channels);
					bool failed = false;
					// At most total_frames frames come out; the call cap bounds the loop either way.
					for (uint32_t calls = 0; calls < (dec.header().total_frames / 256) + 2; ++calls) {
						Result<uint32_t> n = dec.read(buf.data(), 256);
						if (!n.ok()) {
							failed = true;
							break;
						}
						frames += *n;
						if (*n == 0) {
							break;
						}
					}
					(failed || !scan.ok() ? streams_failed : streams_ok) += 1;
				}
			}
		}
		CHECK(chunks == 3);
		MESSAGE("xas: ", chunks, " chunks x ", iters, " mutants: ", streams_ok, " streams decoded, ", streams_failed, " refused, ", frames, " frames");

		// The patch partition through EbxPartition::parse and compile_sound_patch (strict and allow_unimplemented).
		Result<Bytes> plain = f->metro->mount().read_ebx(kM16);
		REQUIRE(plain.ok());
		size_t parsed = 0, compiled = 0, refused = 0;
		for (size_t i = 0; i < iters; ++i) {
			Bytes b = *plain;
			mutate(b, rng);
			Result<EbxPartition> part = EbxPartition::parse(std::move(b));
			if (!part.ok()) {
				++refused;
				continue;
			}
			++parsed;
			for (bool allow : { false, true }) {
				gameplay::WorkBudget budget;
				CompileOptions o;
				o.allow_unimplemented = allow;
				Result<SoundProgram> prog = compile_sound_patch(f->metro->db(), *part, o, budget);
				if (prog.ok()) {
					++compiled;
					run_program(std::move(*prog), f->m16_bank);
				} else {
					CHECK_FALSE(prog.error().message.empty());
				}
			}
		}
		MESSAGE("patch: ", iters, " mutants: ", refused, " refused by the parser, ", parsed, " parsed, ", compiled, " compiles ran");

		// The program blob.
		const Bytes blob = encode_sound_program(*f->m16);
		size_t decoded = 0;
		for (size_t i = 0; i < iters * 4; ++i) {
			Bytes b = blob;
			mutate(b, rng, blob.size());
			Result<SoundProgram> d = decode_sound_program(b);
			if (d.ok()) {
				++decoded;
				run_program(std::move(*d), f->m16_bank);
			}
		}
		MESSAGE("blob: ", iters * 4, " mutants of ", blob.size(), " bytes: ", decoded, " decoded and ran");
		check_unchanged(*f);
	}
}
