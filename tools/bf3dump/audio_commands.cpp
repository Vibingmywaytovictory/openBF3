#include "audio_commands.h"

#include "bf3/core/audio/sound_stream.h"
#include "bf3/core/audio/sound_wave.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/host/ui_flow_session.h"
#include "bf3/sim/logic/sound_area_natives.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using namespace bf3::core;
namespace au = bf3::core::audio;

namespace bf3dump {

int cmd_audio_census(const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args);
int cmd_audio_render(const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args);
int cmd_audio_check(const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args);
int cmd_audio_bank(const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args);
int cmd_audio_wave(const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args);

namespace {

double thread_cpu_seconds() {
	FILETIME created, exited, kernel, user;
	if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
		return 0.0;
	}
	auto ticks = [](const FILETIME &p_t) { return (uint64_t(p_t.dwHighDateTime) << 32) | p_t.dwLowDateTime; };
	return double(ticks(kernel) + ticks(user)) * 1e-7;
}

bool print_install_unchanged(const InstallReader &p_reader, const InstallSnapshot &p_before) {
	const InstallSnapshot after = p_reader.snapshot();
	const SnapshotDiff diff = diff_snapshots(p_before, after);
	const bool clean = p_before.valid && after.valid && diff.clean();
	std::printf("install.unchanged %s%s%s\n", clean ? "yes" : "no", clean ? "" : " ", clean ? "" : diff.summary().c_str());
	return clean;
}

struct StreamRef {
	const std::vector<uint8_t> *chunk = nullptr;
	uint32_t offset = 0;
};

int cmd_audio_waves(const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args) {
	if (p_args.size() < 2) {
		std::fprintf(stderr, "audio waves <LEVEL> <MODE> [--decode] [--bench N]\n");
		return 2;
	}
	bool decode = false;
	int bench = 0;
	for (size_t i = 2; i < p_args.size(); ++i) {
		if (p_args[i] == "--decode") {
			decode = true;
		} else if (p_args[i] == "--bench" && i + 1 < p_args.size()) {
			bench = std::max(1, std::atoi(p_args[++i].c_str()));
		} else {
			std::fprintf(stderr, "audio waves: unknown option %s\n", p_args[i].c_str());
			return 2;
		}
	}
	const InstallSnapshot before = p_reader.snapshot();
	const auto t0 = std::chrono::steady_clock::now();
	Result<std::unique_ptr<level::LoadedLevel>> lvl = level::load_level(p_install, p_args[0], p_args[1]);
	if (!lvl) {
		std::fprintf(stderr, "load_level: %s\n", lvl.error().message.c_str());
		return 1;
	}
	const EbxDb &db = (*lvl)->db();
	const LevelMount &mount = (*lvl)->mount();
	std::fprintf(stderr, "level %s %s: %zu partitions in %.2f s\n", p_args[0].c_str(), p_args[1].c_str(), db.size(),
			std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

	size_t waves = 0, localised = 0, pooled = 0, read_errors = 0, variations = 0, segments = 0, scanned = 0, mismatches = 0, length_mismatches = 0;
	size_t stream_errors = 0, chunk_errors = 0, from_loc = 0, no_language = 0;
	double channel_seconds = 0.0;
	std::map<std::pair<int, uint32_t>, size_t> formats;
	std::map<int, size_t> codecs;
	std::vector<std::string> first_errors;

	std::vector<std::unique_ptr<std::vector<uint8_t>>> kept;
	std::vector<StreamRef> streams;
	auto note = [&](const std::string &p_line) {
		if (first_errors.size() < 12) {
			first_errors.push_back(p_line);
		}
	};
	for (size_t pi = 0; pi < db.size(); ++pi) {
		const EbxPartition &p = db.partition_at(pi);
		const EbxInstance primary = p.primary();
		if (primary.type_name() != "SoundWaveAsset") {
			continue;
		}
		++waves;
		gameplay::WorkBudget budget;
		Result<au::SoundWaveInfo> wave = au::read_sound_wave(db, primary, budget);
		if (!wave) {
			++read_errors;
			note(db.name_at(pi) + ": " + wave.error().message);
			continue;
		}
		localised += wave->localised() ? 1 : 0;
		pooled += wave->stream_pool.empty() ? 0 : 1;
		const std::optional<std::pair<uint32_t, uint32_t>> range = au::playable_variations(*wave, "English");
		if (!range) {
			++no_language;
			continue;
		}
		std::unordered_map<uint16_t, std::unique_ptr<std::vector<uint8_t>>> chunks;
		for (uint32_t v = range->first; v < range->first + range->second; ++v) {
			const au::WaveVariation &var = wave->variations[v];
			++variations;
			if (!chunks.count(var.chunk_index)) {
				const Guid id = wave->chunks[var.chunk_index].id;
				Result<std::vector<uint8_t>> bytes = mount.read_chunk(id);
				if (!bytes) {
					Result<std::optional<std::vector<uint8_t>>> loc = ui::read_toc_chunk(*p_install, "Win32/Loc/en", id);
					if (loc && loc->has_value()) {
						bytes = std::move(**loc);
						++from_loc;
					}
				}
				if (!bytes) {
					++chunk_errors;
					note(wave->name + ": chunk " + id.to_string() + ": " + bytes.error().message);
					chunks[var.chunk_index] = nullptr;
				} else {
					chunks[var.chunk_index] = std::make_unique<std::vector<uint8_t>>(std::move(*bytes));
				}
			}
			const std::vector<uint8_t> *chunk = chunks[var.chunk_index].get();
			for (const au::WaveSegment &seg : var.segments) {
				++segments;
				if (chunk == nullptr) {
					++stream_errors;
					continue;
				}
				Result<au::StreamScan> scan = au::scan_stream(*chunk, seg.samples_offset, decode);
				if (!scan) {
					++stream_errors;
					note(wave->name + ": segment at " + std::to_string(seg.samples_offset) + ": " + scan.error().message);
					continue;
				}
				++scanned;
				++codecs[int(scan->header.codec)];
				++formats[{ int(scan->header.channels), scan->header.sample_rate }];
				if (scan->decoded_frames != scan->header.total_frames) {
					++mismatches;
					note(wave->name + ": segment at " + std::to_string(seg.samples_offset) + ": decoded " + std::to_string(scan->decoded_frames) + " of " +
							std::to_string(scan->header.total_frames) + " frames");
				}
				if (std::abs(double(scan->header.total_frames) - double(seg.length_seconds) * scan->header.sample_rate) > 1.0) {
					++length_mismatches;
					note(wave->name + ": segment at " + std::to_string(seg.samples_offset) + ": " + std::to_string(scan->header.total_frames) +
							" frames vs SegmentLength " + std::to_string(seg.length_seconds) + " s x " + std::to_string(scan->header.sample_rate));
				}
				channel_seconds += double(scan->header.total_frames) * scan->header.channels / scan->header.sample_rate;
				if (bench > 0) {
					streams.push_back({ chunk, seg.samples_offset });
				}
			}
		}
		if (bench > 0) {
			for (auto &[index, bytes] : chunks) {
				if (bytes) {
					kept.push_back(std::move(bytes));
				}
			}
		}
	}
	for (const auto &[codec, n] : codecs) {
		std::printf("codec %d: %zu segments\n", codec, n);
	}
	for (const auto &[fmt, n] : formats) {
		std::printf("format %d ch %u Hz: %zu segments\n", fmt.first, fmt.second, n);
	}
	for (const std::string &e : first_errors) {
		std::printf("  note: %s\n", e.c_str());
	}
	const size_t errors = read_errors + chunk_errors + stream_errors;
	std::printf("audio waves %s %s: waves=%zu localised=%zu stream_pool=%zu no_language=%zu variations=%zu segments=%zu scanned=%zu decoded=%d "
				"frame_mismatches=%zu length_mismatches=%zu read_errors=%zu chunk_errors=%zu stream_errors=%zu errors=%zu chunks_from_loc=%zu "
				"channel_seconds=%.1f\n",
			p_args[0].c_str(), p_args[1].c_str(), waves, localised, pooled, no_language, variations, segments, scanned, decode ? 1 : 0, mismatches,
			length_mismatches, read_errors, chunk_errors, stream_errors, errors, from_loc, channel_seconds);
	bool bench_ok = true;
	if (bench > 0) {
		double best_cpu = 1e30, best_wall = 1e30;
		uint64_t frames_total = 0;
		for (int pass = 0; pass < bench; ++pass) {
			uint64_t frames = 0;
			size_t failed = 0;
			const double c0 = thread_cpu_seconds();
			const auto w0 = std::chrono::steady_clock::now();
			for (const StreamRef &s : streams) {
				Result<au::DecodedStream> d = au::decode_stream(*s.chunk, s.offset);
				if (d) {
					frames += d->frames();
				} else {
					++failed;
				}
			}
			const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
			const double cpu = thread_cpu_seconds() - c0;
			std::printf("bench pass %d: %zu streams, %llu frames, cpu %.1f ms, wall %.1f ms, %.4f ms per channel-second cpu, %.4f wall, %zu failed\n", pass,
					streams.size(), static_cast<unsigned long long>(frames), cpu * 1000.0, wall * 1000.0, cpu * 1000.0 / std::max(channel_seconds, 1e-9),
					wall * 1000.0 / std::max(channel_seconds, 1e-9), failed);
			best_cpu = std::min(best_cpu, cpu);
			best_wall = std::min(best_wall, wall);
			frames_total = frames;
			bench_ok = bench_ok && failed == 0;
		}
		std::printf("audio decode bench: passes=%d streams=%zu channel_seconds=%.1f best_cpu_ms_per_channel_second=%.4f best_wall_ms_per_channel_second=%.4f "
					"frames=%llu\n",
				bench, streams.size(), channel_seconds, best_cpu * 1000.0 / std::max(channel_seconds, 1e-9), best_wall * 1000.0 / std::max(channel_seconds, 1e-9),
				static_cast<unsigned long long>(frames_total));
	}
	const bool unchanged = print_install_unchanged(p_reader, before);
	return (errors == 0 && mismatches == 0 && length_mismatches == 0 && bench_ok && unchanged) ? 0 : 1;
}

int cmd_audio_loc(const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args) {
	if (p_args.empty()) {
		std::fprintf(stderr, "audio loc <lang> [--decode]\n");
		return 2;
	}
	const bool decode = p_args.size() >= 2 && p_args[1] == "--decode";
	const InstallSnapshot before = p_reader.snapshot();
	const std::string sb = "Win32/Loc/" + p_args[0];
	const SuperbundleSite *site = p_install->site(sb);
	if (site == nullptr) {
		std::fprintf(stderr, "no superbundle %s\n", sb.c_str());
		return 1;
	}
	Result<std::shared_ptr<const TocChunkMap>> map = p_install->chunk_toc(*site);
	if (!map) {
		std::fprintf(stderr, "%s: %s\n", sb.c_str(), map.error().message.c_str());
		return 1;
	}

	std::vector<Guid> ids;
	for (const auto &[id, s] : **map) {
		ids.push_back(id);
	}
	std::sort(ids.begin(), ids.end(), [](const Guid &a, const Guid &b) { return a.to_string() < b.to_string(); });
	size_t with_streams = 0, without = 0, streams = 0, errors = 0, mismatches = 0;
	uint64_t bytes = 0;
	double channel_seconds = 0.0, decode_ms = 0.0;
	std::map<std::pair<int, uint32_t>, size_t> formats;
	std::vector<std::string> notes;
	for (const Guid &id : ids) {
		Result<std::optional<std::vector<uint8_t>>> r = ui::read_toc_chunk(*p_install, sb, id);
		if (!r || !r->has_value()) {
			++errors;
			if (notes.size() < 12) {
				notes.push_back(id.to_string() + ": " + (r ? std::string("absent") : r.error().message));
			}
			continue;
		}
		const std::vector<uint8_t> &b = **r;
		bytes += b.size();
		size_t at = 0, n = 0;
		while (at < b.size() && b[at] == au::kStreamBlockHeader) {
			const auto t = std::chrono::steady_clock::now();
			Result<au::StreamScan> scan = au::scan_stream(b, at, decode);
			decode_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
			if (!scan) {
				++errors;
				if (notes.size() < 12) {
					notes.push_back(id.to_string() + " at " + std::to_string(at) + ": " + scan.error().message);
				}
				break;
			}
			++streams;
			++n;
			++formats[{ int(scan->header.channels), scan->header.sample_rate }];
			if (scan->decoded_frames != scan->header.total_frames) {
				++mismatches;
			}
			channel_seconds += double(scan->decoded_frames) * scan->header.channels / scan->header.sample_rate;
			if (scan->stream_bytes == 0) {
				break;
			}
			at += size_t(scan->stream_bytes);
		}
		if (n > 0) {
			++with_streams;
		} else {
			++without;
			if (notes.size() < 12) {
				notes.push_back(id.to_string() + ": no stream (" + std::to_string(b.size()) + " bytes)");
			}
		}
	}
	for (const auto &[fmt, n] : formats) {
		std::printf("format %d ch %u Hz: %zu streams\n", fmt.first, fmt.second, n);
	}
	for (const std::string &e : notes) {
		std::printf("  note: %s\n", e.c_str());
	}
	std::printf("audio loc %s: chunks=%zu bytes=%llu with_streams=%zu without_streams=%zu streams=%zu decoded=%d frame_mismatches=%zu errors=%zu "
				"channel_seconds=%.1f ms_per_channel_second=%.4f\n",
			p_args[0].c_str(), ids.size(), static_cast<unsigned long long>(bytes), with_streams, without, streams, decode ? 1 : 0, mismatches, errors,
			channel_seconds, decode_ms / std::max(channel_seconds, 1e-9));
	const bool unchanged = print_install_unchanged(p_reader, before);
	return (errors == 0 && mismatches == 0 && unchanged) ? 0 : 1;
}

bool parse_point(const std::string &p_text, Vec3 &r_point) {
	float v[3] = {};
	size_t at = 0;
	for (int i = 0; i < 3; ++i) {
		const size_t comma = i < 2 ? p_text.find(',', at) : p_text.size();
		if (comma == std::string::npos) {
			return false;
		}
		char *end = nullptr;
		const std::string part = p_text.substr(at, comma - at);
		v[i] = std::strtof(part.c_str(), &end);
		if (part.empty() || end != part.c_str() + part.size() || !std::isfinite(v[i])) {
			return false;
		}
		at = comma + 1;
	}
	r_point = Vec3{ v[0], v[1], v[2] };
	return true;
}

std::string area_target(const gameplay::LogicGraph &p_graph, uint32_t p_node, int32_t p_port) {
	std::string out;
	for (const gameplay::LogicEdge &e : p_graph.properties_from(p_node)) {
		if (e.source_port != p_port || e.target >= p_graph.nodes().size()) {
			continue;
		}
		out += (out.empty() ? "" : ",") +
				std::string(gameplay::logic_type_info(p_graph.nodes()[e.target].type).ebx_type) + ":" +
				std::to_string(e.target) + "#" + std::to_string(e.target_port);
	}
	return out.empty() ? "-" : out;
}

Vec3 shape_centre(const gameplay::AreaShape &p_shape) {
	if (p_shape.kind == gameplay::AreaShape::Kind::Box) {
		return p_shape.box.trans;
	}
	Vec3 c{};
	for (const Vec3 &p : p_shape.polygon) {
		c = vec::add(c, p);
	}
	return vec::scale(c, 1.0f / float(std::max<size_t>(1, p_shape.polygon.size())));
}

int cmd_audio_areas(
		const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args) {
	if (p_args.size() < 2) {
		std::fprintf(stderr, "audio areas <LEVEL> <MODE> [--at x,y,z]... [--list]\n");
		return 2;
	}
	std::vector<Vec3> points;
	bool list = false;
	for (size_t i = 2; i < p_args.size(); ++i) {
		Vec3 p;
		if (p_args[i] == "--at" && i + 1 < p_args.size() && parse_point(p_args[i + 1], p)) {
			points.push_back(p);
			++i;
		} else if (p_args[i] == "--list") {
			list = true;
		} else {
			std::fprintf(stderr, "audio areas: bad option %s\n", p_args[i].c_str());
			return 2;
		}
	}
	const InstallSnapshot before = p_reader.snapshot();
	Result<std::unique_ptr<level::LoadedLevel>> lvl = level::load_level(p_install, p_args[0], p_args[1]);
	if (!lvl) {
		std::fprintf(stderr, "load_level: %s\n", lvl.error().message.c_str());
		return 1;
	}
	Result<bf3::host::ClientLogicWorld> logic = bf3::host::build_client_logic(**lvl);
	if (!logic) {
		std::fprintf(stderr, "%s\n", logic.error().message.c_str());
		return 1;
	}
	Result<bf3::sim::logic::SoundAreaProbe> probe = bf3::sim::logic::SoundAreaProbe::create(logic->world, logic->graph);
	if (!probe) {
		std::fprintf(stderr, "%s\n", probe.error().message.c_str());
		return 1;
	}
	const gameplay::LogicGraph &g = *logic->graph;
	const gameplay::SoundAreaData &data = probe->natives().data();
	std::printf("audio areas %s %s: areas=%zu unshaped=%zu mixers=%zu mixer_entities=%zu open_paths=%zu "
				"curved_paths=%zu tilted_normals=%zu "
				"mixers_without_asset=%zu\n",
			p_args[0].c_str(), p_args[1].c_str(), data.areas.size(), data.unshaped, data.mixers.size(),
			data.mixer_entities.size(), data.open_paths, data.curved_paths, data.tilted_normals,
			data.mixers_without_asset);
	for (const audio::MixerAsset &m : data.mixers) {
		std::printf("mixer %s inputs=%zu outputs=%zu nodes=%zu\n", m.name.c_str(), m.inputs.size(), m.outputs.size(),
				m.nodes.size());
	}
	if (list) {
		for (const gameplay::AreaDef &a : data.areas) {
			const Vec3 c = shape_centre(a.shape);
			std::printf(
					"area %u %s %s centre=(%.2f, %.2f, %.2f) height=%.2f fade=%.2f curve=%d multiplier=%.3f -> %s\n",
					a.node, std::string(gameplay::logic_type_info(a.type).ebx_type).c_str(),
					a.shape.kind == gameplay::AreaShape::Kind::Box ? "box" : "prism", double(c.x), double(c.y),
					double(c.z), double(a.shape.height), double(a.fade_width), int(a.curve),
					double(a.proximity_multiplier), area_target(g, a.node, a.out_port).c_str());
		}
	}
	for (const Vec3 &p : points) {
		if (const Status s = probe->at(p); !s.ok()) {
			std::fprintf(stderr, "audio areas: %s\n", s.error().message.c_str());
			return 1;
		}
		const bf3::sim::logic::SoundAreaNatives &n = probe->natives();
		std::printf("at (%.3f, %.3f, %.3f):\n", double(p.x), double(p.y), double(p.z));
		for (size_t i = 0; i < n.areas().size(); ++i) {
			const bf3::sim::logic::SoundAreaValue &v = n.areas()[i];
			if (v.proximity != 0.0f) {
				std::printf("  area %u proximity=%.4f multiplier=%.4f distance=%.3f -> %s\n", v.node,
						double(v.proximity), double(v.multiplier), double(data.areas[i].shape.distance(p)),
						area_target(g, v.node, data.areas[i].out_port).c_str());
			}
		}
		for (size_t m = 0; m < data.mixers.size(); ++m) {
			const audio::MixerAsset &a = data.mixers[m];
			for (size_t i = 0; i < a.inputs.size(); ++i) {
				std::printf(
						"  %s input %u = %.4f\n", a.name.c_str(), a.inputs[i].name_hash, double(n.mixer_inputs(m)[i]));
			}
			for (size_t i = 0; i < a.outputs.size(); ++i) {
				std::printf("  %s output %u = %.4f\n", a.name.c_str(), a.outputs[i].name_hash,
						double(n.mixer_outputs(m)[i]));
			}
		}
		std::printf("  AreaType=%g\n", double(n.area_type()));
	}
	return print_install_unchanged(p_reader, before) ? 0 : 1;
}
}

const char *audio_usage() {
	return "  audio waves <LEVEL> <MODE> [--decode] [--bench N]\n"
		   "  audio loc <lang> [--decode]\n"
		   "  audio census <sb-spec>\n"
		   "  audio render <LEVEL> <MODE> <patch> --distance <m> [--rounds N] [--seed S] [--first-person] --out <wav>\n"
		   "  audio check [<LEVEL> <MODE>] [--g7] [--out-dir <dir>]\n"
		   "  audio bank <LEVEL> <MODE> [--loads N]\n"
		   "  audio wave <LEVEL> <MODE> <wave partition> [--envelope]\n"
		   "  audio areas <LEVEL> <MODE> [--at x,y,z]... [--list]\n";
}

int cmd_audio(const InstallReader &p_reader, std::shared_ptr<Install> p_install, const std::vector<std::string> &p_args) {
	if (p_args.empty()) {
		std::fprintf(stderr, "%s", audio_usage());
		return 2;
	}
	const std::vector<std::string> rest(p_args.begin() + 1, p_args.end());
	if (p_args[0] == "waves") {
		return cmd_audio_waves(p_reader, p_install, rest);
	}
	if (p_args[0] == "loc") {
		return cmd_audio_loc(p_reader, p_install, rest);
	}
	if (p_args[0] == "census") {
		return cmd_audio_census(p_reader, p_install, rest);
	}
	if (p_args[0] == "render") {
		return cmd_audio_render(p_reader, p_install, rest);
	}
	if (p_args[0] == "check") {
		return cmd_audio_check(p_reader, p_install, rest);
	}
	if (p_args[0] == "bank") {
		return cmd_audio_bank(p_reader, p_install, rest);
	}
	if (p_args[0] == "wave") {
		return cmd_audio_wave(p_reader, p_install, rest);
	}
	if (p_args[0] == "areas") {
		return cmd_audio_areas(p_reader, p_install, rest);
	}
	std::fprintf(stderr, "%s", audio_usage());
	return 2;
}

}
