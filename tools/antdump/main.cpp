// antdump: Godot-free CLI over bf3_core's Ant reader (clean-room implementation from
// research/probes/ant-animation-probe.md). Reads an export directory of Ant package payloads (see
// bf3/core/ant/ant_source.h); writes only the --out file it is given, never under the BF3 install.
//
//   antdump census  <export_dir> [--mp]                 codec census, DCT checks, channel mapping
//   antdump list    <export_dir> <package>              clips of a package (codec, keys, controller)
//   antdump clip    <export_dir> <package> <clip name> [--digits N] [--out file.json]
//   antdump compare <export_dir> <oracle.json>...       dump the oracle's clip and compare value by value
//   antdump bench   <export_dir> [--mp] [--repeat N]    decode throughput (single thread)
//   antdump sample  <export_dir> <package> <clip name> <frame>   one interpolated pose (first channels)
#include "antdump_lib.h"
#include "mini_json.h"

#include "bf3/core/install_reader.h"
#include "bf3/core/path_guard.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace bf3::core::ant;
using namespace bf3::tools::antdump;

namespace {

int usage() {
	std::fprintf(stderr,
			"usage:\n"
			"  antdump census  <export_dir> [--mp]\n"
			"  antdump list    <export_dir> <package>\n"
			"  antdump clip    <export_dir> <package> <clip name> [--digits N] [--out file.json]\n"
			"  antdump compare <export_dir> <oracle.json>...\n"
			"  antdump bench   <export_dir> [--mp] [--repeat N]\n"
			"  antdump sample  <export_dir> <package> <clip name> <frame>\n");
	return 2;
}

bool has_flag(int argc, char **argv, const char *p_flag) {
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], p_flag) == 0) {
			return true;
		}
	}
	return false;
}

const char *flag_value(int argc, char **argv, const char *p_flag) {
	for (int i = 1; i + 1 < argc; ++i) {
		if (std::strcmp(argv[i], p_flag) == 0) {
			return argv[i + 1];
		}
	}
	return nullptr;
}

struct Session {
	AntExportDirSource source;
	AntLibrary library;
};

bool open_session(const char *p_dir, Session &r_session) {
	std::string error;
	if (!r_session.source.open(p_dir, error) || !load_library(r_session.source, r_session.library, error)) {
		std::fprintf(stderr, "antdump: %s\n", error.c_str());
		return false;
	}
	return true;
}

// The static package is owned by the library; other packages are loaded on demand.
const AntPackage *get_package(Session &r_session, const std::string &p_name, std::unique_ptr<AntPackage> &r_holder) {
	if (p_name == "s_basicassets") {
		return r_session.library.static_package();
	}
	const AntPackageInfo *info = r_session.source.find(p_name);
	if (info == nullptr) {
		std::fprintf(stderr, "antdump: unknown package %s\n", p_name.c_str());
		return nullptr;
	}
	std::string error;
	r_holder = load_package(r_session.source, *info, error);
	if (!r_holder) {
		std::fprintf(stderr, "antdump: %s\n", error.c_str());
	}
	return r_holder.get();
}

bool write_text(const std::string &p_path, const std::string &p_text) {
	// Never write into the install, whatever the caller passes (plan §2.1).
	bf3::core::InstallReader install;
	bf3::core::PathGuard guard = install.write_guard();
	const auto verdict = guard.check_write_target(p_path);
	if (verdict != bf3::core::PathGuard::Verdict::Allowed) {
		std::fprintf(stderr, "antdump: refusing to write %s (%s)\n", p_path.c_str(), bf3::core::PathGuard::verdict_name(verdict));
		return false;
	}
	FILE *f = nullptr;
	if (fopen_s(&f, p_path.c_str(), "wb") != 0 || f == nullptr) {
		std::fprintf(stderr, "antdump: cannot write %s\n", p_path.c_str());
		return false;
	}
	std::fwrite(p_text.data(), 1, p_text.size(), f);
	std::fclose(f);
	return true;
}

int cmd_census(int argc, char **argv) {
	Session s;
	if (!open_session(argv[2], s)) {
		return 1;
	}
	Census c;
	std::string error;
	if (!run_census(s.source, s.library, has_flag(argc, argv, "--mp"), c, error)) {
		std::fprintf(stderr, "antdump: %s\n", error.c_str());
		return 1;
	}
	std::fputs(format_census(c).c_str(), stdout);
	return c.packages_failed == 0 ? 0 : 1;
}

int cmd_list(char **argv) {
	Session s;
	if (!open_session(argv[2], s)) {
		return 1;
	}
	std::unique_ptr<AntPackage> holder;
	const AntPackage *p = get_package(s, argv[3], holder);
	if (!p) {
		return 1;
	}
	std::printf("package %s: %s, %zu bytes, %zu objects, %zu layouts\n", p->name().c_str(), packaging_name(p->packaging()), p->byte_size(), p->object_count(),
			p->archive().layouts().size());
	for (uint32_t index : AntLibrary::clips(*p)) {
		const GdStruct clip = p->object(index);
		DecodedClip d;
		const AntResult r = decode_clip(clip, d);
		ClipBinding b;
		const AntResult br = s.library.bind(*p, clip, b);
		ClipHeader h;
		read_clip_header(clip, h);
		std::printf("  %-40s %-20s endFrame %4u keys %4u q %3u v %3u f %3u  %s  controller %s target %s rig %s\n", std::string(clip.name()).c_str(),
				std::string(clip.type_name()).c_str(), h.end_frame, r.ok() ? d.num_keys : 0, d.num_quats, d.num_vec3s, d.num_floats,
				r.ok() ? "ok" : r.status == AntStatus::Unsupported ? "UNSUPPORTED" : "ERROR", br.ok() ? std::string(b.controller_name).c_str() : "-",
				br.ok() ? std::string(b.target_name).c_str() : "-", (br.ok() && b.rig) ? std::string(b.rig->name).c_str() : "-");
	}
	return 0;
}

int cmd_clip(int argc, char **argv) {
	Session s;
	if (!open_session(argv[2], s)) {
		return 1;
	}
	std::unique_ptr<AntPackage> holder;
	const AntPackage *p = get_package(s, argv[3], holder);
	if (!p) {
		return 1;
	}
	const GdStruct clip = AntLibrary::find_clip(*p, argv[4]);
	if (!clip.valid()) {
		std::fprintf(stderr, "antdump: no clip named \"%s\" in %s\n", argv[4], argv[3]);
		return 1;
	}
	const char *digits = flag_value(argc, argv, "--digits");
	std::string json, error;
	if (!clip_to_json(s.library, *p, clip, digits ? std::atoi(digits) : -1, json, error)) {
		std::fprintf(stderr, "antdump: %s\n", error.c_str());
		return 1;
	}
	if (const char *out = flag_value(argc, argv, "--out")) {
		return write_text(out, json) ? 0 : 1;
	}
	std::fwrite(json.data(), 1, json.size(), stdout);
	return 0;
}

int cmd_compare(int argc, char **argv) {
	Session s;
	if (!open_session(argv[2], s)) {
		return 1;
	}
	int failures = 0;
	for (int i = 3; i < argc; ++i) {
		std::vector<uint8_t> bytes;
		std::string error;
		if (!read_whole_file(argv[i], bytes, error)) {
			std::fprintf(stderr, "antdump: %s\n", error.c_str());
			return 1;
		}
		const std::string oracle(bytes.begin(), bytes.end());
		std::string package;
		if (!oracle_package(oracle, package, error)) {
			std::fprintf(stderr, "antdump: %s: %s\n", argv[i], error.c_str());
			return 1;
		}
		std::unique_ptr<AntPackage> holder;
		const AntPackage *p = get_package(s, package, holder);
		GdStruct clip;
		if (p == nullptr || !find_oracle_clip(s.library, *p, oracle, clip, error)) {
			std::printf("%s: FAIL %s\n", argv[i], error.c_str());
			++failures;
			continue;
		}
		const std::string clip_name(clip.name());
		std::string ours;
		CompareResult r;
		if (!clip_to_json(s.library, *p, clip, -1, ours, error) || !compare_clip_json(ours, oracle, r, error)) {
			std::printf("%s: FAIL %s\n", argv[i], error.c_str());
			++failures;
			continue;
		}
		const bool pass = r.shape_ok && r.max_track_diff <= 1e-5 && r.max_joint_diff <= 1e-5;
		failures += pass ? 0 : 1;
		std::printf("%s: %s  %s/%s  track values %llu max|diff| %.3g  joint values %llu max|diff| %.3g  shape %s\n", argv[i], pass ? "PASS" : "FAIL",
				package.c_str(), clip_name.c_str(), (unsigned long long)r.values, r.max_track_diff, (unsigned long long)r.joint_values, r.max_joint_diff,
				r.shape_ok ? "same" : "DIFFERENT");
		for (const std::string &d : r.differences) {
			std::printf("    %s\n", d.c_str());
		}
	}
	return failures == 0 ? 0 : 1;
}

int cmd_bench(int argc, char **argv) {
	Session s;
	if (!open_session(argv[2], s)) {
		return 1;
	}
	const bool mp = has_flag(argc, argv, "--mp");
	const char *rep = flag_value(argc, argv, "--repeat");
	const int repeat = rep ? std::max(1, std::atoi(rep)) : 3;
	using Clock = std::chrono::steady_clock;
	// Load (and time) every package parse.
	std::vector<std::unique_ptr<AntPackage>> packages;
	uint64_t package_bytes = 0;
	const Clock::time_point t0 = Clock::now();
	for (const AntPackageInfo &info : s.source.packages()) {
		if (info.packaging == AntPackagingType::Static || (mp && !is_mp_relevant(info.short_name))) {
			continue;
		}
		std::string error;
		auto p = load_package(s.source, info, error);
		if (p) {
			package_bytes += p->byte_size();
			packages.push_back(std::move(p));
		}
	}
	const double load_s = std::chrono::duration<double>(Clock::now() - t0).count();
	// Static package parse time (re-parse a copy).
	std::vector<uint8_t> static_bytes;
	std::string error;
	for (const AntPackageInfo &info : s.source.packages()) {
		if (info.packaging == AntPackagingType::Static) {
			s.source.read(info, static_bytes, error);
		}
	}
	const Clock::time_point ts = Clock::now();
	AntPackage reparsed;
	const bool static_ok = reparsed.parse("s_basicassets", std::move(static_bytes), error);
	const double static_s = std::chrono::duration<double>(Clock::now() - ts).count();
	std::printf("parse: %zu packages, %.1f MB (read + parse) in %.3f s; s_basicassets parse %s in %.3f s (%zu objects)\n", packages.size(), double(package_bytes) / 1e6,
			load_s, static_ok ? "ok" : "FAILED", static_s, reparsed.object_count());

	struct Item {
		GdStruct clip;
		ClipCodec codec;
	};
	std::vector<Item> items;
	auto collect = [&](const AntPackage &p) {
		for (uint32_t i : AntLibrary::clips(p)) {
			const ClipCodec c = clip_codec(p.object(i));
			if (c == ClipCodec::Dct || c == ClipCodec::Raw || c == ClipCodec::Frame) {
				items.push_back(Item{ p.object(i), c });
			}
		}
	};
	collect(*s.library.static_package());
	for (const auto &p : packages) {
		collect(*p);
	}
	for (ClipCodec codec : { ClipCodec::Dct, ClipCodec::Raw, ClipCodec::Frame }) {
		uint64_t clips = 0, values = 0, bitstream = 0;
		double best = 1e30;
		for (int r = 0; r < repeat; ++r) {
			clips = values = bitstream = 0;
			const Clock::time_point t = Clock::now();
			DecodedClip d;
			DecodeStats st;
			for (const Item &it : items) {
				if (it.codec != codec) {
					continue;
				}
				if (decode_clip(it.clip, d, &st).ok()) {
					++clips;
					values += uint64_t(d.num_keys) * d.channel_count();
					bitstream += st.bitstream_bytes;
				}
			}
			best = std::min(best, std::chrono::duration<double>(Clock::now() - t).count());
		}
		std::printf("decode %-20s %5llu clips, %10llu channel-keys in %.3f s (best of %d): %.0f clips/s, %.1f M channel-keys/s", codec_name(codec),
				(unsigned long long)clips, (unsigned long long)values, best, repeat, double(clips) / best, double(values) / best / 1e6);
		if (codec == ClipCodec::Dct) {
			std::printf(", %.1f MB/s of bitstream (%.1f MB)", double(bitstream) / best / 1e6, double(bitstream) / 1e6);
		}
		std::printf("\n");
	}
	// Sampling throughput on the decoded DCT clips: one pose per call at fractional frames.
	{
		std::vector<DecodedClip> decoded;
		for (const Item &it : items) {
			if (it.codec == ClipCodec::Dct && decoded.size() < 2000) {
				DecodedClip d;
				if (decode_clip(it.clip, d).ok()) {
					decoded.push_back(std::move(d));
				}
			}
		}
		ClipPose pose;
		uint64_t samples = 0, channels = 0;
		const Clock::time_point t = Clock::now();
		for (const DecodedClip &d : decoded) {
			const float end = d.key_times.empty() ? 0.0f : float(d.key_times.back());
			for (int k = 0; k < 16; ++k) {
				sample_clip(d, end * (float(k) + 0.37f) / 16.0f, pose);
				++samples;
				channels += d.channel_count();
			}
		}
		const double sec = std::chrono::duration<double>(Clock::now() - t).count();
		std::printf("sample (slerp/lerp)       %llu poses, %llu channels in %.3f s: %.1f M channels/s, %.2f us/pose\n", (unsigned long long)samples,
				(unsigned long long)channels, sec, double(channels) / sec / 1e6, sec / double(samples) * 1e6);
	}
	return 0;
}

int cmd_sample(char **argv) {
	Session s;
	if (!open_session(argv[2], s)) {
		return 1;
	}
	std::unique_ptr<AntPackage> holder;
	const AntPackage *p = get_package(s, argv[3], holder);
	const GdStruct clip = p ? AntLibrary::find_clip(*p, argv[4]) : GdStruct();
	DecodedClip d;
	ClipBinding b;
	if (!clip.valid() || !decode_clip(clip, d).ok() || !s.library.bind(*p, clip, b).ok()) {
		std::fprintf(stderr, "antdump: cannot decode/bind %s\n", argv[4]);
		return 1;
	}
	const float frame = float(std::atof(argv[5]));
	ClipPose pose;
	sample_clip(d, frame, pose);
	std::printf("%s @ frame %.3f (%.4f s at %.0f fps)\n", argv[4], frame, frame / b.fps, b.fps);
	for (uint32_t c = 0; c < d.channel_count() && c < 12; ++c) {
		const AntDof &dof = b.dofs[b.tracks[c].dof];
		if (c < d.num_quats) {
			const Quat &q = pose.quats[c];
			std::printf("  %-32s q [%.5f %.5f %.5f %.5f]\n", (std::string(dof.layout) + "." + std::string(dof.slot)).c_str(), q.x, q.y, q.z, q.w);
		} else if (c < d.num_quats + d.num_vec3s) {
			const Vec3 &v = pose.vec3s[c - d.num_quats];
			std::printf("  %-32s v [%.5f %.5f %.5f]\n", (std::string(dof.layout) + "." + std::string(dof.slot)).c_str(), v.x, v.y, v.z);
		}
	}
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		return usage();
	}
	const std::string cmd = argv[1];
	if (cmd == "census") {
		return cmd_census(argc, argv);
	}
	if (cmd == "list" && argc >= 4) {
		return cmd_list(argv);
	}
	if (cmd == "clip" && argc >= 5) {
		return cmd_clip(argc, argv);
	}
	if (cmd == "compare" && argc >= 4) {
		return cmd_compare(argc, argv);
	}
	if (cmd == "bench") {
		return cmd_bench(argc, argv);
	}
	if (cmd == "sample" && argc >= 6) {
		return cmd_sample(argv);
	}
	return usage();
}
