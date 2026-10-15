// bf3dump audio census (gate G4) and audio render (audio_commands.h).

#include "audio_commands.h"

#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/sound_types.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/install_snapshot.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <string>

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

} // namespace

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
		// Compile every graph (allow_unimplemented), and the weapons strictly too.
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
			// The blob round trip and the validator run on every compiled program.
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
	// The handheld type set must be exactly the A2 set.
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

int cmd_audio_render(const InstallReader &, std::shared_ptr<Install>, const std::vector<std::string> &) {
	std::fprintf(stderr, "audio render: not built yet\n");
	return 1;
}

} // namespace bf3dump
