#include "audio_commands.h"

#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/sound_types.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/audio/audio_analysis.h"
#include "bf3/audio/audio_check.h"
#include "bf3/audio/audio_load.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/wav_io.h"
#include "bf3/audio/weapon_sound.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/audio/sound_stream.h"
#include "bf3/core/audio/sound_wave.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

using namespace bf3::core;
namespace au = bf3::core::audio;

namespace bf3dump {

namespace {

bool unchanged_line(const InstallReader &p_reader, const InstallSnapshot &p_before) {
	const InstallSnapshot after = p_reader.snapshot();
	const SnapshotDiff diff = diff_snapshots(p_before, after);
	const bool clean = p_before.valid && after.valid && diff.clean();
	std::printf("install.unchanged %s%s%s\n", clean ? "yes" : "no", clean ? "" : " ", clean ? "" : diff.summary().c_str());
	return clean;
}

}

int cmd_audio_census(const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args) {
	if (p_args.size() != 1) {
		std::fprintf(stderr, "audio census <sb-spec>\n");
		return 2;
	}
	const InstallSnapshot before = p_reader.snapshot();
	const auto t0 = std::chrono::steady_clock::now();
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(p_install, expand_superbundles(*p_install, p_args[0]));
	if (!lib) {
		std::fprintf(stderr, "open: %s\n", lib.error().message.c_str());
		return 1;
	}
	const EbxLibrary::LoadReport report = (*lib)->load_all();
	std::fprintf(stderr, "%zu partitions loaded in %.1f s (%zu failed)\n", report.partitions,
			std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), report.failed);

	size_t patches = 0, under_sound = 0, masters = 0, mixers = 0, waves = 0, configs = 0;
	size_t patch_nodes = 0, all_nodes = 0, voices = 0, connections = 0, sen0 = 0, rou0 = 0, other_named = 0;
	size_t graphs = 0, compiled = 0, compile_failures = 0, unknown_types = 0, feedback = 0, max_nodes = 0, max_registers = 0, max_voices = 0;
	size_t handheld = 0, handheld_strict = 0, blob_failures = 0, blob_bytes = 0;
	std::set<std::string> patch_types, all_types, handheld_types;
	std::map<std::string, size_t> type_counts;
	std::vector<std::string> failures;
	for (const EbxLibrary::Entry &e : (*lib)->entries()) {
		const std::string_view type = e.partition->primary().type_name();
		if (type == "SoundWaveAsset") {
			++waves;
			continue;
		}
		if (type == "SoundPatchConfigurationAsset") {
			++configs;
			continue;
		}
		const bool patch = type == "SoundPatchAsset";
		const bool master = type == "SoundMasterPatchAsset";
		const bool mixer = type == "MixerAsset";
		if (!patch && !master && !mixer) {
			continue;
		}
		patches += patch ? 1 : 0;
		masters += master ? 1 : 0;
		mixers += mixer ? 1 : 0;
		under_sound += (patch && e.name.starts_with("sound/")) ? 1 : 0;
		const bool weapon = patch && e.name.starts_with("sound/weapons/handheld/");
		handheld += weapon ? 1 : 0;
		size_t here = 0;
		for (size_t i = 0; i < e.partition->instance_count(); ++i) {
			const std::optional<EbxInstance> in = e.partition->instance(i);
			const std::string t(in->type_name());
			if (t.size() > 8 && t.ends_with("NodeData")) {
				++all_nodes;
				all_types.insert(t);
				if (patch) {
					++patch_nodes;
					++here;
					patch_types.insert(t);
					++type_counts[t];
					if (weapon) {
						handheld_types.insert(t);
					}
				}
			}
		}
		max_nodes = std::max(max_nodes, here);
		if (mixer) {
			continue;
		}

		++graphs;
		au::CompileOptions options;
		options.allow_unimplemented = true;
		gameplay::WorkBudget budget;
		au::CompileStats stats;
		Result<au::SoundProgram> prog = au::compile_sound_patch(**lib, *e.partition, options, budget, &stats);
		unknown_types += stats.unknown_types;
		if (!prog) {
			++compile_failures;
			if (failures.size() < 20) {
				failures.push_back(std::string(e.name) + ": " + prog.error().message);
			}
			continue;
		}
		++compiled;
		{

			const std::vector<uint8_t> blob = au::encode_sound_program(*prog);
			blob_bytes += blob.size();
			Result<au::SoundProgram> back = au::decode_sound_program(blob);
			if (!back || au::encode_sound_program(*back) != blob) {
				++blob_failures;
				if (failures.size() < 20) {
					failures.push_back(std::string(e.name) + " (blob): " + (back ? std::string("re-encode differs") : back.error().message));
				}
			}
		}
		voices += prog->voices.size();
		connections += prog->connections.size();
		feedback += stats.feedback_edges;
		max_registers = std::max<size_t>(max_registers, prog->registers);
		max_voices = std::max(max_voices, prog->voices.size());
		for (const au::SoundVoicePlugin &p : prog->plugins) {
			if (p.connection == au::kNoRegister) {
				continue;
			}
			if (p.id == au::fourcc("Sen0")) {
				++sen0;
			} else if (p.id == au::fourcc("Rou0")) {
				++rou0;
			} else {
				++other_named;
			}
		}
		if (weapon) {
			gameplay::WorkBudget strict_budget;
			Result<au::SoundProgram> strict = au::compile_sound_patch(**lib, *e.partition, au::CompileOptions{}, strict_budget);
			if (strict) {
				++handheld_strict;
			} else if (failures.size() < 20) {
				failures.push_back(std::string(e.name) + " (strict): " + strict.error().message);
			}
		}
	}

	std::set<std::string> a2;
	for (const au::SoundNodeTypeInfo &info : au::sound_node_types()) {
		if (info.a2 && info.role == au::SoundNodeRole::Patch) {
			a2.insert(std::string(info.ebx_name));
		}
	}
	size_t unknown_patch_types = 0;
	for (const std::string &t : all_types) {
		if (!au::sound_node_type(t)) {
			++unknown_patch_types;
			std::printf("  unknown node type %s\n", t.c_str());
		}
	}
	for (const auto &[t, n] : type_counts) {
		std::printf("  type %-34s %6zu\n", t.c_str(), n);
	}
	for (const std::string &f : failures) {
		std::printf("  failure: %s\n", f.c_str());
	}
	const bool handheld_set_ok = handheld_types == a2;
	std::printf("audio census %s: patches=%zu under_sound=%zu masters=%zu mixers=%zu configurations=%zu waves=%zu patch_node_instances=%zu "
				"patch_types=%zu all_node_instances=%zu all_types=%zu voices=%zu graphs=%zu compiled=%zu compile_failures=%zu unknown_types=%zu "
				"connections=%zu sen0=%zu rou0=%zu other_named=%zu feedback_edges=%zu handheld=%zu handheld_types=%zu handheld_set_a2=%d "
				"handheld_strict=%zu max_nodes=%zu max_registers=%zu max_voices=%zu blob_failures=%zu blob_bytes=%zu\n",
			p_args[0].c_str(), patches, under_sound, masters, mixers, configs, waves, patch_nodes, patch_types.size(), all_nodes, all_types.size(), voices,
			graphs, compiled, compile_failures, unknown_types + unknown_patch_types, connections, sen0, rou0, other_named, feedback, handheld,
			handheld_types.size(), handheld_set_ok ? 1 : 0, handheld_strict, max_nodes, max_registers, max_voices, blob_failures, blob_bytes);
	const bool unchanged = unchanged_line(p_reader, before);
	return (compile_failures == 0 && blob_failures == 0 && handheld_strict == handheld && handheld_set_ok && unchanged) ? 0 : 1;
}

namespace {

struct LevelAudio {
	std::unique_ptr<level::LoadedLevel> level;
	std::shared_ptr<const au::SoundProgram> program;
	std::shared_ptr<const au::WaveBank> bank;
};

Result<LevelAudio> load_patch_audio(std::shared_ptr<Install> p_install, const std::string &p_level, const std::string &p_mode, std::string_view p_patch) {
	LevelAudio out;
	Result<std::unique_ptr<level::LoadedLevel>> lvl = level::load_level(p_install, p_level, p_mode);
	if (!lvl) {
		return lvl.error();
	}
	out.level = std::move(*lvl);
	Result<std::shared_ptr<const au::SoundProgram>> prog = bf3::audio::compile_patch(out.level->db(), p_patch);
	if (!prog) {
		return prog.error();
	}
	out.program = *prog;
	size_t missing = 0;
	Result<std::vector<au::SoundWaveInfo>> waves =
			bf3::audio::read_program_waves(out.level->db(), std::span<const std::shared_ptr<const au::SoundProgram>>(&out.program, 1), &missing);
	if (!waves) {
		return waves.error();
	}
	if (missing != 0) {
		return make_error(std::string(p_patch) + ": " + std::to_string(missing) + " waves not in the level");
	}
	Result<std::shared_ptr<const au::WaveBank>> bank = au::WaveBank::load(*p_install, out.level->mount(), std::move(*waves), au::WaveBankOptions{});
	if (!bank) {
		return bank.error();
	}
	out.bank = *bank;
	return out;
}

std::filesystem::path with_suffix(const std::filesystem::path &p_path, const std::string &p_suffix) {
	std::filesystem::path out = p_path;
	out.replace_filename(p_path.stem().string() + p_suffix + p_path.extension().string());
	return out;
}

float max_abs_diff(std::span<const float> a, std::span<const float> b) {
	if (a.size() != b.size()) {
		return INFINITY;
	}
	float m = 0.0f;
	for (size_t i = 0; i < a.size(); ++i) {
		m = std::max(m, std::fabs(a[i] - b[i]));
	}
	return m;
}

}

int cmd_audio_render(const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args) {
	if (p_args.size() < 3) {
		std::fprintf(stderr, "audio render <LEVEL> <MODE> <patch> --distance <m> [--rounds N] [--seed S] [--first-person] --out <wav>\n");
		return 2;
	}
	float distance = 10.0f;
	uint32_t rounds = 10;
	uint64_t seed = 1;
	bool first_person = false;
	std::string out_path;
	for (size_t i = 3; i < p_args.size(); ++i) {
		const std::string &a = p_args[i];
		if (a == "--distance" && i + 1 < p_args.size()) {
			distance = float(std::atof(p_args[++i].c_str()));
		} else if (a == "--rounds" && i + 1 < p_args.size()) {
			rounds = uint32_t(std::max(1, std::atoi(p_args[++i].c_str())));
		} else if (a == "--seed" && i + 1 < p_args.size()) {
			seed = std::strtoull(p_args[++i].c_str(), nullptr, 10);
		} else if (a == "--first-person") {
			first_person = true;
		} else if (a == "--out" && i + 1 < p_args.size()) {
			out_path = p_args[++i];
		} else {
			std::fprintf(stderr, "audio render: unknown option %s\n", a.c_str());
			return 2;
		}
	}
	if (out_path.empty()) {
		std::fprintf(stderr, "audio render: --out <wav> is required\n");
		return 2;
	}
	const InstallSnapshot before = p_reader.snapshot();
	Result<LevelAudio> la = load_patch_audio(p_install, p_args[0], p_args[1], to_lower_ascii(p_args[2]));
	if (!la) {
		std::fprintf(stderr, "audio render: %s\n", la.error().message.c_str());
		return 1;
	}
	const bf3::audio::AudioTuning tuning;
	Result<bf3::audio::WeaponSoundBinding> binding = bf3::audio::bind_weapon_sound(la->program, 0.075, bf3::audio::kMixRate);
	if (!binding) {
		std::fprintf(stderr, "audio render: %s\n", binding.error().message.c_str());
		return 1;
	}
	bf3::audio::MixerCore mixer(bf3::audio::MixerConfig{});
	const double seconds = 0.25 + double(rounds) * 0.075 + 5.0;
	bf3::audio::WavRecorder recorder(bf3::audio::kMixRate, seconds + 1.0);
	mixer.set_tap(&recorder);
	Result<std::unique_ptr<bf3::audio::PatchInstance>> inst = bf3::audio::PatchInstance::create(la->program, la->bank, tuning, seed, bf3::audio::kMixRate);
	if (!inst) {
		std::fprintf(stderr, "audio render: %s\n", inst.error().message.c_str());
		return 1;
	}
	const bf3::audio::InstanceHandle handle = mixer.next_handle();
	bf3::audio::WeaponSoundDriver driver(*binding, bf3::audio::WeaponFireMode::Automatic, handle);
	driver.prepare(**inst, first_person);
	bf3::audio::EmitterState e;
	e.position = { 0.0f, 0.0f, distance };
	e.forward = { 0.0f, 0.0f, -1.0f };
	(*inst)->set_geometry(e, bf3::audio::ListenerState{});
	bf3::audio::MixerCommand start;
	start.type = bf3::audio::MixerCommandType::Start;
	start.handle = handle;
	start.instance = inst->get();
	const uint64_t first = uint64_t(0.25 * bf3::audio::kMixRate);
	start.at_frame = first - 2048;
	if (!mixer.push(start)) {
		return 1;
	}
	(void)inst->release();
	bool ok = driver.trigger_down(mixer, first);
	for (uint32_t r = 1; r < rounds; ++r) {
		ok = ok && driver.round_fired(mixer, r);
	}
	const uint64_t stop = driver.round_frame(rounds - 1) + 1800;
	ok = ok && driver.trigger_up(mixer, stop);
	bf3::audio::MixerCommand stop_cmd;
	stop_cmd.type = bf3::audio::MixerCommandType::Stop;
	stop_cmd.handle = handle;
	stop_cmd.at_frame = stop + 1024;
	ok = ok && mixer.push(stop_cmd);
	std::vector<float> buf(512 * 2);
	const uint64_t total = uint64_t(seconds * bf3::audio::kMixRate);
	const auto t0 = std::chrono::steady_clock::now();
	while (mixer.clock() < total) {
		mixer.mix(buf.data(), 512);
		mixer.reclaim();
	}
	const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	const Status saved = recorder.save(p_reader.write_guard(), out_path);
	const std::span<const float> mix = recorder.samples();
	std::printf("audio render %s: distance=%.2f rounds=%u seed=%llu frames=%zu rms_db=%.2f peak_db=%.2f render_ms=%.1f starved=%llu decode_errors=%llu "
				"late_commands=%llu saved=%d\n",
			p_args[2].c_str(), distance, rounds, static_cast<unsigned long long>(seed), mix.size() / 2, bf3::audio::rms_db(mix, 2, first, stop - first),
			bf3::audio::peak_db(mix, 2, 0, mix.size() / 2), wall * 1000.0, static_cast<unsigned long long>(mixer.stats().starved_voices.load()),
			static_cast<unsigned long long>(mixer.stats().decode_errors.load()), static_cast<unsigned long long>(mixer.stats().late_commands.load()),
			saved.ok() ? 1 : 0);
	if (!saved) {
		std::fprintf(stderr, "audio render: %s\n", saved.error().message.c_str());
	}
	const bool unchanged = unchanged_line(p_reader, before);
	return ok && saved.ok() && unchanged ? 0 : 1;
}

int cmd_audio_check(const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args) {
	std::string level_id = "MP_Subway", mode = "TeamDeathMatch0";
	bool g7 = false;
	std::filesystem::path out_dir;
	size_t i = 0;
	if (p_args.size() >= 2 && !p_args[0].starts_with("--")) {
		level_id = p_args[0];
		mode = p_args[1];
		i = 2;
	}
	for (; i < p_args.size(); ++i) {
		if (p_args[i] == "--g7") {
			g7 = true;
		} else if (p_args[i] == "--out-dir" && i + 1 < p_args.size()) {
			out_dir = p_args[++i];
		} else {
			std::fprintf(stderr, "audio check: unknown option %s\n", p_args[i].c_str());
			return 2;
		}
	}
	const InstallSnapshot before = p_reader.snapshot();
	const auto t0 = std::chrono::steady_clock::now();
	Result<LevelAudio> la = load_patch_audio(p_install, level_id, mode, bf3::audio::kM16Patch);
	if (!la) {
		std::fprintf(stderr, "audio check: %s\n", la.error().message.c_str());
		return 1;
	}
	const double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	const bf3::audio::AudioTuning tuning;
	const bf3::audio::CheckScript script = bf3::audio::m16_check_script();
	const auto t1 = std::chrono::steady_clock::now();
	Result<bf3::audio::OfflineCheck> run = bf3::audio::run_check_offline(script, la->program, la->bank, tuning);
	if (!run) {
		std::fprintf(stderr, "audio check: %s\n", run.error().message.c_str());
		return 1;
	}
	const double render_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
	bf3::audio::CheckReport report = run->report;
	if (g7) {
		bf3::audio::add_model_trace_rows(report, *run, script, *la->program, *la->bank, bf3::audio::kMixRate);

		bf3::audio::CheckScript bypass = script;
		bypass.segments = { { "bypass17", 17.0f, false, 1, script.segments[0].seed + 50, false, false } };
		bf3::audio::CheckScript pinned = bypass;
		pinned.segments[0].pin_noise_hpf = true;
		Result<bf3::audio::OfflineCheck> fa = bf3::audio::run_check_offline(bypass, la->program, la->bank, tuning);
		Result<bf3::audio::OfflineCheck> fb = bf3::audio::run_check_offline(pinned, la->program, la->bank, tuning);
		if (!fa || !fb) {
			std::fprintf(stderr, "audio check: bypass render failed\n");
			return 1;
		}
		const float fdiff = max_abs_diff(fa->mix, fb->mix);
		float v46 = -1.0f;
		{
			Result<std::unique_ptr<bf3::audio::PatchInstance>> probe = bf3::audio::PatchInstance::create(la->program, la->bank, tuning, 1, bf3::audio::kMixRate);
			if (probe) {
				bf3::audio::EmitterState e;
				e.position = { 0.0f, 0.0f, 17.0f };
				(*probe)->set_geometry(e, bf3::audio::ListenerState{});
				(*probe)->evaluate(0.0);
				v46 = (*probe)->plugin_attribute(bf3::audio::kM16NoiseHpf, 0);
			}
		}
		char rule[160];
		std::snprintf(rule, sizeof(rule), "== 0 (max |difference|; v46 at 17 m = %.3f Hz, rms %.1f dBFS)", v46, bf3::audio::rms_db(fa->mix, 2, 0, fa->total_frames));
		report.rows.push_back({ "bypass.0hz_17m", double(fdiff), rule, fdiff == 0.0f && v46 == 0.0f && fa->mix.size() == fb->mix.size() });

		bf3::audio::OfflineCheckOptions varied;
		varied.call_frames = { 37, 512, 1000, 480, 4096, 1, 129 };
		Result<bf3::audio::OfflineCheck> again = bf3::audio::run_check_offline(script, la->program, la->bank, tuning, varied);
		bf3::audio::CheckScript other = script;
		for (bf3::audio::CheckSegment &seg : other.segments) {
			seg.seed += 100;
		}
		Result<bf3::audio::OfflineCheck> moved = bf3::audio::run_check_offline(other, la->program, la->bank, tuning);
		if (!again || !moved) {
			std::fprintf(stderr, "audio check: determinism render failed\n");
			return 1;
		}
		const float same = max_abs_diff(run->mix, again->mix);
		const float diff = max_abs_diff(run->mix, moved->mix);
		report.rows.push_back({ "determinism.same_seed_varied_calls", double(same), "== 0 (max |difference|)", same == 0.0f });
		report.rows.push_back({ "determinism.other_seed", double(diff), "> 0 (max |difference|)", diff > 0.0f && std::isfinite(diff) });

		const auto counter = [&](const char *p_name, uint64_t p_value) { report.rows.push_back({ p_name, double(p_value), "== 0", p_value == 0 }); };
		counter("counters.starved_voices", run->starved_voices);
		counter("counters.decode_errors", run->decode_errors);
		counter("counters.deferred_events", run->deferred_events);
		counter("counters.late_commands", run->late_commands);
		counter("counters.dropped_tap_trace_record", run->tap_dropped + run->trace_dropped + run->record_dropped);
	}
	if (!out_dir.empty()) {
		const PathGuard guard = p_reader.write_guard();
		const std::filesystem::path wav = out_dir / "m16_offline.wav";
		Status a = bf3::audio::write_wav(guard, wav, run->mix, 2, bf3::audio::kMixRate);
		Status b = bf3::audio::write_wav(guard, with_suffix(wav, "_core"), run->core_tap, 1, bf3::audio::kMixRate);
		Status c = bf3::audio::write_wav(guard, with_suffix(wav, "_noise"), run->noise_tap, 1, bf3::audio::kMixRate);
		const bool written = a.ok() && b.ok() && c.ok();
		report.rows.push_back({ "wavs.written", written ? 3.0 : 0.0, "== 3 (" + wav.string() + ")", written });
		if (!written) {
			std::fprintf(stderr, "audio check: %s\n", (!a ? a : !b ? b : c).error().message.c_str());
		}
	}
	std::printf("%s", report.table().c_str());
	size_t failed = 0;
	for (const bf3::audio::CheckRow &r : report.rows) {
		failed += r.pass ? 0 : 1;
	}
	std::printf("audio check %s %s: rows=%zu failed=%zu far_noise_hpf_hz=%.2f load_s=%.2f render_s=%.2f audio_s=%.2f\n", level_id.c_str(), mode.c_str(),
			report.rows.size(), failed, run->far_noise_hpf_hz, load_s, render_s, double(run->total_frames) / bf3::audio::kMixRate);
	const bool unchanged = unchanged_line(p_reader, before);
	return failed == 0 && unchanged ? 0 : 1;
}

int cmd_audio_wave(const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args) {
	if (p_args.size() < 3) {
		std::fprintf(stderr, "audio wave <LEVEL> <MODE> <wave partition> [--envelope]\n");
		return 2;
	}
	const bool envelope = p_args.size() > 3 && p_args[3] == "--envelope";
	const InstallSnapshot before = p_reader.snapshot();
	Result<std::unique_ptr<level::LoadedLevel>> lvl = level::load_level(p_install, p_args[0], p_args[1]);
	if (!lvl) {
		std::fprintf(stderr, "load_level: %s\n", lvl.error().message.c_str());
		return 1;
	}
	const EbxDb &db = (*lvl)->db();
	const EbxPartition *part = db.partition_by_name(to_lower_ascii(p_args[2]));
	if (part == nullptr) {
		std::fprintf(stderr, "audio wave: no partition %s\n", p_args[2].c_str());
		return 1;
	}
	gameplay::WorkBudget budget;
	Result<au::SoundWaveInfo> wave = au::read_sound_wave(db, part->primary(), budget);
	if (!wave) {
		std::fprintf(stderr, "audio wave: %s\n", wave.error().message.c_str());
		return 1;
	}
	std::printf("wave %s: channels=%u chunks=%zu variations=%zu selection=%s\n", wave->name.c_str(), wave->channels, wave->chunks.size(), wave->variations.size(),
			wave->selection.c_str());
	std::unordered_map<uint16_t, std::vector<uint8_t>> chunks;
	for (size_t v = 0; v < wave->variations.size(); ++v) {
		const au::WaveVariation &var = wave->variations[v];
		if (!chunks.count(var.chunk_index)) {
			Result<std::vector<uint8_t>> bytes = (*lvl)->mount().read_chunk(wave->chunks[var.chunk_index].id);
			chunks[var.chunk_index] = bytes ? std::move(*bytes) : std::vector<uint8_t>{};
		}
		const std::vector<uint8_t> &chunk = chunks[var.chunk_index];
		std::printf(" variation %zu: chunk %u segments %zu loop [%d, %d]\n", v, var.chunk_index, var.segments.size(), var.first_loop_segment,
				var.last_loop_segment);
		for (size_t s = 0; s < var.segments.size(); ++s) {
			Result<au::DecodedStream> d = au::decode_stream(chunk, var.segments[s].samples_offset);
			if (!d) {
				std::printf("  seg %zu: %s\n", s, d.error().message.c_str());
				continue;
			}
			const uint32_t ch = d->header.channels;
			std::vector<float> mono(d->frames());
			for (uint32_t f = 0; f < d->frames(); ++f) {
				double sum = 0.0;
				for (uint32_t c = 0; c < ch; ++c) {
					sum += d->samples[size_t(f) * ch + c];
				}
				mono[f] = float(sum / ch / 32768.0);
			}
			const std::vector<uint64_t> on = bf3::audio::detect_onsets(mono, 1, d->header.sample_rate, bf3::audio::OnsetOptions{});
			uint32_t peak_at = 0;
			float peak = 0.0f;
			for (uint32_t f = 0; f < mono.size(); ++f) {
				if (std::fabs(mono[f]) > peak) {
					peak = std::fabs(mono[f]);
					peak_at = f;
				}
			}
			uint32_t first_loud = 0;
			for (uint32_t f = 0; f < mono.size(); ++f) {
				if (std::fabs(mono[f]) >= 0.1f * peak) {
					first_loud = f;
					break;
				}
			}
			std::string ons;
			for (uint64_t o : on) {
				ons += " " + std::to_string(o);
			}
			std::printf("  seg %2zu: %u Hz %u ch %6u frames (%.1f ms, listed %.1f ms) rms %.1f peak %.1f dBFS at %u, first -20 dB at %u, onsets:%s\n", s,
					d->header.sample_rate, ch, d->frames(), 1000.0 * d->frames() / d->header.sample_rate, 1000.0 * var.segments[s].length_seconds,
					bf3::audio::rms_db(mono, 1, 0, mono.size()), bf3::audio::peak_db(mono, 1, 0, mono.size()), peak_at, first_loud, ons.c_str());
			if (envelope) {
				std::string head;
				char hc[16];
				for (uint32_t f = 0; f + 8 <= std::min<size_t>(mono.size(), 192); f += 8) {
					std::snprintf(hc, sizeof(hc), " %.0f", bf3::audio::peak_db(mono, 1, f, 8));
					head += hc;
				}
				std::printf("     head 8-frame peaks:%s\n", head.c_str());
				std::string env;
				char cell[16];
				const uint32_t step = d->header.sample_rate / 200;
				for (uint32_t f = 0; f + step <= mono.size(); f += step) {
					std::snprintf(cell, sizeof(cell), " %.0f", bf3::audio::rms_db(mono, 1, f, step));
					env += cell;
				}
				std::printf("     5ms:%s\n", env.c_str());
			}
		}
	}
	const bool unchanged = unchanged_line(p_reader, before);
	return unchanged ? 0 : 1;
}

int cmd_audio_bank(const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args) {
	if (p_args.size() < 2) {
		std::fprintf(stderr, "audio bank <LEVEL> <MODE> [--loads N]\n");
		return 2;
	}
	int loads = 2;
	for (size_t i = 2; i < p_args.size(); ++i) {
		if (p_args[i] == "--loads" && i + 1 < p_args.size()) {
			loads = std::max(1, std::atoi(p_args[++i].c_str()));
		} else {
			std::fprintf(stderr, "audio bank: unknown option %s\n", p_args[i].c_str());
			return 2;
		}
	}
	const InstallSnapshot before = p_reader.snapshot();
	Result<std::unique_ptr<level::LoadedLevel>> lvl = level::load_level(p_install, p_args[0], p_args[1]);
	if (!lvl) {
		std::fprintf(stderr, "load_level: %s\n", lvl.error().message.c_str());
		return 1;
	}
	const EbxDb &db = (*lvl)->db();
	Result<std::vector<au::SoundWaveInfo>> waves = bf3::audio::read_level_waves(db);
	if (!waves) {
		std::fprintf(stderr, "audio bank: %s\n", waves.error().message.c_str());
		return 1;
	}
	std::shared_ptr<const au::WaveBank> bank;
	double first_ms = 0.0, warm_ms = 0.0;
	for (int l = 0; l < loads; ++l) {
		bank.reset();
		const auto t0 = std::chrono::steady_clock::now();
		Result<std::shared_ptr<const au::WaveBank>> b = au::WaveBank::load(*p_install, (*lvl)->mount(), *waves, au::WaveBankOptions{});
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if (!b) {
			std::fprintf(stderr, "audio bank: %s\n", b.error().message.c_str());
			return 1;
		}
		bank = *b;
		if (l == 0) {
			first_ms = ms;
		}
		warm_ms = ms;
	}

	size_t patches = 0, compiled = 0, starved = 0;
	std::set<Guid> named;
	std::vector<std::string> failures;
	for (size_t pi = 0; pi < db.size(); ++pi) {
		if (db.partition_at(pi).primary().type_name() != "SoundPatchAsset" || !db.name_at(pi).starts_with("sound/weapons/handheld/")) {
			continue;
		}
		++patches;
		Result<std::shared_ptr<const au::SoundProgram>> prog = bf3::audio::compile_patch(db, db.name_at(pi));
		if (!prog) {
			if (failures.size() < 10) {
				failures.push_back(db.name_at(pi) + ": " + prog.error().message);
			}
			continue;
		}
		++compiled;
		for (const au::SoundWaveRef &w : (*prog)->waves) {
			if (!named.insert(w.partition).second) {
				continue;
			}
			const std::optional<au::WaveId> id = bank->find(w.partition);
			if (!id || !bank->resident(*id)) {
				++starved;
				if (failures.size() < 10) {
					failures.push_back("not resident: " + w.name);
				}
			}
		}
	}
	for (const std::string &f : failures) {
		std::printf("  failure: %s\n", f.c_str());
	}
	const au::WaveBankStats &st = bank->stats();
	std::printf("audio bank %s %s: waves=%zu resident_waves=%zu chunks=%zu resident_mib=%.1f budget_mib=%.0f skipped_localised=%zu weapon_patches=%zu "
				"compiled_strict=%zu named_waves=%zu starved=%zu loads=%d first_load_ms=%.1f warm_load_ms=%.1f\n",
			p_args[0].c_str(), p_args[1].c_str(), st.waves, st.resident_waves, st.chunks, double(st.resident_bytes) / (1024.0 * 1024.0),
			double(au::WaveBankOptions{}.resident_budget_bytes) / (1024.0 * 1024.0), st.skipped_localised, patches, compiled, named.size(), starved, loads,
			first_ms, warm_ms);
	const bool unchanged = unchanged_line(p_reader, before);
	return (compiled == patches && starved == 0 && unchanged) ? 0 : 1;
}

}
