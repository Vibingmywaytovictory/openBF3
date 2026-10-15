// antdump: Godot-free CLI over bf3_core's Ant reader (clean-room implementation from
// research/probes/ant-animation-probe.md). Every command reads <input>: the install (`--install`, located like
// the game: OPENBF3_INSTALL, else the default path; `--install=<dir>` names it) through AntInstallSource, or an
// export directory of Ant payloads (tools/antdump/README.md). The install is only read; the tool writes only
// the --out file it is given, never under the install.
//
//   antdump census  <input> [--mp]                      codec census, DCT checks, channel mapping
//   antdump list    <input> <package>                   clips of a package (codec, keys, controller)
//   antdump clip    <input> <package> <clip name> [--digits N] [--out file.json]
//   antdump compare <input> <oracle.json>...            dump the oracle's clip and compare value by value
//   antdump bench   <input> [--mp] [--repeat N]         decode throughput (single thread)
//   antdump sample  <input> <package> <clip name> <frame>   one interpolated pose (first channels)
//   antdump rigmap  <input> <package> <clip name> [--skeleton <ebx partition>]...
//                                                       channel -> DOF -> rig joint table, timing; with the
//                                                       install, the joints by name in EBX SkeletonAssets
//   antdump sources <input>                             packages and banks with their provenance (tsv)
//   antdump parity  <export_dir> --install[=<dir>] [--rows-only]   the install source against an export
//
// <input> = <export_dir> | --install | --install=<dir>
#include "antdump_lib.h"
#include "mini_json.h"

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/path_guard.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace bf3::core::ant;
using namespace bf3::tools::antdump;

namespace {

int usage() {
	std::fprintf(stderr,
			"usage (<input> = <export_dir> | --install | --install=<dir>):\n"
			"  antdump census  <input> [--mp]\n"
			"  antdump list    <input> <package>\n"
			"  antdump clip    <input> <package> <clip name> [--digits N] [--out file.json]\n"
			"  antdump compare <input> <oracle.json>...\n"
			"  antdump bench   <input> [--mp] [--repeat N]\n"
			"  antdump sample  <input> <package> <clip name> <frame>\n"
			"  antdump rigmap  <input> <package> <clip name> [--skeleton <ebx partition>]...\n"
			"  antdump sources <input>\n"
			"  antdump parity  <export_dir> --install[=<dir>] [--rows-only]\n");
	return 2;
}

// Command line: the command, positional arguments, flags (with their values) and the input.
struct Args {
	std::string cmd;
	std::vector<std::string> pos; // after the command; the export directory first unless --install
	SourceOptions input;
	std::vector<std::pair<std::string, std::string>> flags; // --name value (value "" for switches)

	bool has(const char *p_flag) const {
		for (const auto &f : flags) {
			if (f.first == p_flag) {
				return true;
			}
		}
		return false;
	}
	const char *value(const char *p_flag) const {
		for (const auto &f : flags) {
			if (f.first == p_flag) {
				return f.second.c_str();
			}
		}
		return nullptr;
	}
	std::vector<std::string> values(const char *p_flag) const {
		std::vector<std::string> out;
		for (const auto &f : flags) {
			if (f.first == p_flag) {
				out.push_back(f.second);
			}
		}
		return out;
	}
};

bool parse_args(int argc, char **argv, Args &r_args) {
	if (argc < 2) {
		return false;
	}
	r_args.cmd = argv[1];
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--install") {
			r_args.input.install = true;
		} else if (a.rfind("--install=", 0) == 0) {
			r_args.input.install = true;
			r_args.input.install_dir = a.substr(10);
		} else if (a == "--digits" || a == "--out" || a == "--repeat" || a == "--skeleton") {
			if (i + 1 >= argc) {
				return false;
			}
			r_args.flags.emplace_back(a, argv[++i]);
		} else if (a == "--mp" || a == "--rows-only") {
			r_args.flags.emplace_back(a, "");
		} else {
			r_args.pos.push_back(a);
		}
	}
	// Without --install the first positional argument is the export directory.
	if (!r_args.input.install) {
		if (r_args.pos.empty()) {
			return false;
		}
		r_args.input.export_dir = r_args.pos.front();
		r_args.pos.erase(r_args.pos.begin());
	}
	return true;
}

struct Session {
	OpenedSource opened;
	AntLibrary library;
	const AntPackageSource &source() const { return *opened.source; }
};

// 0 = open; else the exit code (77 when the input does not exist, like bf3dump without an install).
int open_session(const Args &p_args, Session &r_session) {
	std::string error;
	bool unavailable = false;
	const auto t0 = std::chrono::steady_clock::now();
	if (!open_source(p_args.input, r_session.opened, error, &unavailable)) {
		std::fprintf(stderr, "antdump: %s\n", error.c_str());
		return unavailable ? 77 : 1;
	}
	std::fprintf(stderr, "antdump: %s\n", r_session.opened.description.c_str());
	if (!load_library(r_session.source(), r_session.library, error)) {
		std::fprintf(stderr, "antdump: %s\n", error.c_str());
		return 1;
	}
	std::fprintf(stderr, "antdump: input and s_basicassets ready in %.2f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
	return 0;
}

// The static package is owned by the library; other packages are loaded on demand.
const AntPackage *get_package(Session &r_session, const std::string &p_name, std::unique_ptr<AntPackage> &r_holder) {
	if (p_name == "s_basicassets") {
		return r_session.library.static_package();
	}
	const AntPackageInfo *info = r_session.source().find(p_name);
	if (info == nullptr) {
		std::fprintf(stderr, "antdump: unknown package %s\n", p_name.c_str());
		return nullptr;
	}
	std::string error;
	r_holder = load_package(r_session.source(), *info, error);
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

int cmd_census(const Args &p_args) {
	Session s;
	if (const int rc = open_session(p_args, s)) {
		return rc;
	}
	Census c;
	std::string error;
	if (!run_census(s.source(), s.library, p_args.has("--mp"), c, error)) {
		std::fprintf(stderr, "antdump: %s\n", error.c_str());
		return 1;
	}
	std::fputs(format_census(c).c_str(), stdout);
	return c.packages_failed == 0 ? 0 : 1;
}

int cmd_list(const Args &p_args) {
	Session s;
	if (const int rc = open_session(p_args, s)) {
		return rc;
	}
	std::unique_ptr<AntPackage> holder;
	const AntPackage *p = get_package(s, p_args.pos[0], holder);
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

int cmd_clip(const Args &p_args) {
	Session s;
	if (const int rc = open_session(p_args, s)) {
		return rc;
	}
	std::unique_ptr<AntPackage> holder;
	const AntPackage *p = get_package(s, p_args.pos[0], holder);
	if (!p) {
		return 1;
	}
	const GdStruct clip = AntLibrary::find_clip(*p, p_args.pos[1]);
	if (!clip.valid()) {
		std::fprintf(stderr, "antdump: no clip named \"%s\" in %s\n", p_args.pos[1].c_str(), p_args.pos[0].c_str());
		return 1;
	}
	const char *digits = p_args.value("--digits");
	std::string json, error;
	if (!clip_to_json(s.library, *p, clip, digits ? std::atoi(digits) : -1, json, error)) {
		std::fprintf(stderr, "antdump: %s\n", error.c_str());
		return 1;
	}
	if (const char *out = p_args.value("--out")) {
		return write_text(out, json) ? 0 : 1;
	}
	std::fwrite(json.data(), 1, json.size(), stdout);
	return 0;
}

int cmd_compare(const Args &p_args) {
	Session s;
	if (const int rc = open_session(p_args, s)) {
		return rc;
	}
	int failures = 0;
	for (const std::string &file : p_args.pos) {
		std::vector<uint8_t> bytes;
		std::string error;
		if (!read_whole_file(file, bytes, error)) {
			std::fprintf(stderr, "antdump: %s\n", error.c_str());
			return 1;
		}
		const std::string oracle(bytes.begin(), bytes.end());
		std::string package;
		if (!oracle_package(oracle, package, error)) {
			std::fprintf(stderr, "antdump: %s: %s\n", file.c_str(), error.c_str());
			return 1;
		}
		std::unique_ptr<AntPackage> holder;
		const AntPackage *p = get_package(s, package, holder);
		GdStruct clip;
		if (p == nullptr || !find_oracle_clip(s.library, *p, oracle, clip, error)) {
			std::printf("%s: FAIL %s\n", file.c_str(), error.c_str());
			++failures;
			continue;
		}
		const std::string clip_name(clip.name());
		std::string ours;
		CompareResult r;
		if (!clip_to_json(s.library, *p, clip, -1, ours, error) || !compare_clip_json(ours, oracle, r, error)) {
			std::printf("%s: FAIL %s\n", file.c_str(), error.c_str());
			++failures;
			continue;
		}
		const bool pass = r.shape_ok && r.max_track_diff <= 1e-5 && r.max_joint_diff <= 1e-5;
		failures += pass ? 0 : 1;
		std::printf("%s: %s  %s/%s  track values %llu max|diff| %.3g  joint values %llu max|diff| %.3g  shape %s\n", file.c_str(), pass ? "PASS" : "FAIL",
				package.c_str(), clip_name.c_str(), (unsigned long long)r.values, r.max_track_diff, (unsigned long long)r.joint_values, r.max_joint_diff,
				r.shape_ok ? "same" : "DIFFERENT");
		for (const std::string &d : r.differences) {
			std::printf("    %s\n", d.c_str());
		}
	}
	return failures == 0 ? 0 : 1;
}

int cmd_bench(const Args &p_args) {
	Session s;
	if (const int rc = open_session(p_args, s)) {
		return rc;
	}
	const bool mp = p_args.has("--mp");
	const char *rep = p_args.value("--repeat");
	const int repeat = rep ? std::max(1, std::atoi(rep)) : 3;
	using Clock = std::chrono::steady_clock;
	// Load (and time) every package parse.
	std::vector<std::unique_ptr<AntPackage>> packages;
	uint64_t package_bytes = 0;
	const Clock::time_point t0 = Clock::now();
	for (const AntPackageInfo &info : s.source().packages()) {
		if (info.packaging == AntPackagingType::Static || (mp && !is_mp_relevant(info.short_name))) {
			continue;
		}
		std::string error;
		auto p = load_package(s.source(), info, error);
		if (p) {
			package_bytes += p->byte_size();
			packages.push_back(std::move(p));
		}
	}
	const double load_s = std::chrono::duration<double>(Clock::now() - t0).count();
	// Static package parse time (re-parse a copy).
	std::vector<uint8_t> static_bytes;
	std::string error;
	for (const AntPackageInfo &info : s.source().packages()) {
		if (info.packaging == AntPackagingType::Static) {
			s.source().read(info, static_bytes, error);
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

int cmd_sample(const Args &p_args) {
	Session s;
	if (const int rc = open_session(p_args, s)) {
		return rc;
	}
	std::unique_ptr<AntPackage> holder;
	const AntPackage *p = get_package(s, p_args.pos[0], holder);
	const GdStruct clip = p ? AntLibrary::find_clip(*p, p_args.pos[1]) : GdStruct();
	DecodedClip d;
	ClipBinding b;
	if (!clip.valid() || !decode_clip(clip, d).ok() || !s.library.bind(*p, clip, b).ok()) {
		std::fprintf(stderr, "antdump: cannot decode/bind %s\n", p_args.pos[1].c_str());
		return 1;
	}
	const float frame = float(std::atof(p_args.pos[2].c_str()));
	ClipPose pose;
	sample_clip(d, frame, pose);
	std::printf("%s @ frame %.3f (%.4f s at %.0f fps)\n", p_args.pos[1].c_str(), frame, frame / b.fps, b.fps);
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

// An EBX SkeletonAsset's bone names and parents (Hierarchy), for the rigmap cross-check.
struct EbxSkeleton {
	std::string partition;
	std::vector<std::string> names;
	std::vector<int32_t> parents;
	int32_t find(std::string_view p_name) const {
		for (size_t i = 0; i < names.size(); ++i) {
			if (names[i] == p_name) {
				return int32_t(i);
			}
		}
		return -1;
	}
};

bool load_ebx_skeletons(const Session &p_session, const std::vector<std::string> &p_partitions, std::vector<EbxSkeleton> &r_skeletons) {
	if (p_partitions.empty()) {
		return true;
	}
	if (!p_session.opened.install) {
		std::fprintf(stderr, "antdump: --skeleton needs the install as input\n");
		return false;
	}
	const auto &install = p_session.opened.install;
	bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> lib = bf3::core::EbxLibrary::open(install, bf3::core::expand_superbundles(*install, "@all-mp"));
	if (!lib) {
		std::fprintf(stderr, "antdump: ebx library: %s\n", lib.error().message.c_str());
		return false;
	}
	for (const std::string &name : p_partitions) {
		const bf3::core::EbxPartition *part = (*lib)->partition_by_name(name);
		if (part == nullptr || !part->primary().is_a("SkeletonAsset")) {
			std::fprintf(stderr, "antdump: no SkeletonAsset partition %s in @all-mp\n", name.c_str());
			return false;
		}
		EbxSkeleton sk;
		sk.partition = name;
		const bf3::core::EbxInstance prim = part->primary();
		if (const std::optional<bf3::core::EbxValue> names = prim.get("BoneNames")) {
			for (bf3::core::EbxValue v : *names->as_array()) {
				sk.names.push_back(std::string(v.as_str().value_or("")));
			}
		}
		if (const std::optional<bf3::core::EbxValue> hierarchy = prim.get("Hierarchy")) {
			for (bf3::core::EbxValue v : *hierarchy->as_array()) {
				sk.parents.push_back(v.as_i32().value_or(-1));
			}
		}
		r_skeletons.push_back(std::move(sk));
	}
	return true;
}

int cmd_rigmap(const Args &p_args) {
	Session s;
	if (const int rc = open_session(p_args, s)) {
		return rc;
	}
	std::vector<EbxSkeleton> skeletons;
	if (!load_ebx_skeletons(s, p_args.values("--skeleton"), skeletons)) {
		return 1;
	}
	std::unique_ptr<AntPackage> holder;
	const AntPackage *p = get_package(s, p_args.pos[0], holder);
	const GdStruct clip = p ? AntLibrary::find_clip(*p, p_args.pos[1]) : GdStruct();
	DecodedClip d;
	ClipBinding b;
	ClipHeader h;
	if (!clip.valid() || !read_clip_header(clip, h)) {
		std::fprintf(stderr, "antdump: no clip named \"%s\"\n", p_args.pos[1].c_str());
		return 1;
	}
	const AntResult dr = decode_clip(clip, d);
	const AntResult br = s.library.bind(*p, clip, b);
	if (!dr.ok() || !br.ok() || b.rig == nullptr) {
		std::fprintf(stderr, "antdump: cannot decode/bind %s: %s %s\n", p_args.pos[1].c_str(), dr.message.c_str(), br.message.c_str());
		return 1;
	}
	const AntRig &rig = *b.rig;
	std::printf("clip %s / %s (%s): %u keys at frames %u..%u, EndFrame %u, Cycle %s, Additive %s\n", p->name().c_str(), std::string(clip.name()).c_str(),
			codec_name(d.codec), d.num_keys, d.key_times.empty() ? 0u : unsigned(d.key_times.front()), d.key_times.empty() ? 0u : unsigned(d.key_times.back()),
			h.end_frame, d.cycle ? "yes" : "no", d.additive ? "yes" : "no");
	std::printf("controller %s: FPS %g, FPSScale %g, NumTicks %g -> %u frames at %g fps = %.3f s\n", std::string(b.controller_name).c_str(), b.fps, b.fps_scale,
			b.num_ticks, h.end_frame, b.fps, b.fps > 0.0f ? double(h.end_frame) / b.fps : 0.0);
	uint32_t parent_first = 0;
	for (size_t i = 0; i < rig.joints.size(); ++i) {
		parent_first += rig.joints[i].parent < int32_t(i) ? 1 : 0;
	}
	std::printf("target %s: %zu DOFs; rig %s, skeleton %s, %zu joints (%u with the parent before the joint)\n", std::string(b.target_name).c_str(),
			b.dofs.size(), std::string(rig.name).c_str(), std::string(rig.skeleton_name).c_str(), rig.joints.size(), parent_first);
	uint32_t on_joint = 0;
	std::set<int32_t> joints_q, joints_t, joints_any;
	for (const ChannelTrack &t : b.tracks) {
		if (t.joint >= 0) {
			++on_joint;
			joints_any.insert(t.joint);
			(t.kind == 'q' ? joints_q : joints_t).insert(t.joint);
		}
	}
	std::printf("channels %zu: %u rotations, %u vec3, %u floats; on a rig joint %u, kind mismatches %u, delta-trajectory %u\n", b.tracks.size(), d.num_quats,
			d.num_vec3s, d.num_floats, on_joint, b.kind_mismatches, b.delta_traj_channels);
	std::printf("joints driven: %zu (%zu rotated, %zu translated/scaled); rig joints not driven: %zu (rest pose from the rig defaults)\n", joints_any.size(),
			joints_q.size(), joints_t.size(), rig.joints.size() - joints_any.size());
	for (const EbxSkeleton &sk : skeletons) {
		uint32_t named = 0, same_parent = 0;
		std::string other_parent;
		for (int32_t j : joints_any) {
			const int32_t e = sk.find(rig.joints[size_t(j)].name);
			if (e < 0) {
				continue;
			}
			++named;
			const int32_t rp = rig.joints[size_t(j)].parent;
			const int32_t ep = size_t(e) < sk.parents.size() ? sk.parents[size_t(e)] : -1;
			const std::string_view rig_parent = rp >= 0 ? rig.joints[size_t(rp)].name : std::string_view();
			const std::string_view ebx_parent = (ep >= 0 && size_t(ep) < sk.names.size()) ? std::string_view(sk.names[size_t(ep)]) : std::string_view();
			if (rig_parent == ebx_parent) {
				++same_parent;
			} else {
				other_parent += " " + std::string(rig.joints[size_t(j)].name) + " (rig " + std::string(rig_parent) + ", ebx " +
						(ebx_parent.empty() ? std::string("-") : std::string(ebx_parent)) + ")";
			}
		}
		uint32_t rig_named = 0;
		std::string rig_only;
		for (const AntJoint &j : rig.joints) {
			if (sk.find(j.name) >= 0) {
				++rig_named;
			} else {
				rig_only += " " + std::string(j.name);
			}
		}
		std::printf("ebx %s: %zu bones; driven joints with a bone of that name %u of %zu (same parent name %u); rig joints named there %u of %zu\n",
				sk.partition.c_str(), sk.names.size(), named, joints_any.size(), same_parent, rig_named, rig.joints.size());
		if (!other_parent.empty()) {
			std::printf("  other parent:%s\n", other_parent.c_str());
		}
		// Only worth listing when the skeleton covers the rig (the weapon skeleton names a small subset).
		if (!rig_only.empty() && rig_named + 8 >= rig.joints.size()) {
			std::printf("  rig joints without a bone there:%s\n", rig_only.c_str());
		}
	}
	std::printf("\n%-4s %-11s %-5s %-34s %-5s %-24s %-24s", "ch", "kind", "dof", "layout.slot", "joint", "name", "parent");
	for (size_t i = 0; i < skeletons.size(); ++i) {
		std::printf(" ebx%zu", i);
	}
	std::printf("\n");
	for (const ChannelTrack &t : b.tracks) {
		const AntDof &dof = b.dofs[t.dof];
		const std::string slot = std::string(dof.layout) + "." + std::string(dof.slot);
		const AntJoint *j = t.joint >= 0 ? &rig.joints[size_t(t.joint)] : nullptr;
		const std::string parent = (j && j->parent >= 0) ? std::string(rig.joints[size_t(j->parent)].name) : std::string("-");
		std::printf("%-4u %-11s %-5u %-34s %-5d %-24s %-24s", t.channel, dof_kind_name(dof.kind), t.dof, slot.c_str(), t.joint, j ? std::string(j->name).c_str() : "-",
				parent.c_str());
		for (const EbxSkeleton &sk : skeletons) {
			std::printf(" %4d", j ? sk.find(j->name) : -1);
		}
		std::printf("\n");
	}
	return 0;
}

int cmd_sources(const Args &p_args) {
	OpenedSource opened;
	std::string error;
	bool unavailable = false;
	if (!open_source(p_args.input, opened, error, &unavailable)) {
		std::fprintf(stderr, "antdump: %s\n", error.c_str());
		return unavailable ? 77 : 1;
	}
	std::fprintf(stderr, "antdump: %s\n", opened.description.c_str());
	std::printf("kind\tpartition\tname\tshort\tpackaging\tstreaming_guid\tchunk_size\tsuperbundle\tbundle\n");
	for (const char *kind : { "package", "bank" }) {
		const auto &rows = std::strcmp(kind, "package") == 0 ? opened.source->packages() : opened.source->all_banks();
		for (const AntPackageInfo &i : rows) {
			std::printf("%s\t%s\t%s\t%s\t%s\t%s\t%llu\t%s\t%s\n", i.is_asset_package ? kind : "set", i.partition.c_str(), i.name.c_str(), i.short_name.c_str(),
					packaging_name(i.packaging), i.streaming_guid.c_str(), (unsigned long long)i.chunk_size, i.superbundle.c_str(), i.bundle.c_str());
		}
	}
	return 0;
}

int cmd_parity(const Args &p_args) {
	if (!p_args.input.install || p_args.pos.empty()) {
		return usage();
	}
	OpenedSource install, exported;
	std::string error;
	bool unavailable = false;
	if (!open_source(p_args.input, install, error, &unavailable)) {
		std::fprintf(stderr, "antdump: %s\n", error.c_str());
		return unavailable ? 77 : 1;
	}
	SourceOptions export_options;
	export_options.export_dir = p_args.pos[0];
	if (!open_source(export_options, exported, error)) {
		std::fprintf(stderr, "antdump: %s\n", error.c_str());
		return 1;
	}
	std::printf("a: %s\nb: %s\n", install.description.c_str(), exported.description.c_str());
	if (const auto *src = dynamic_cast<const AntInstallSource *>(install.source.get())) {
		for (const std::string &w : src->report().warnings) {
			std::printf("  warning: %s\n", w.c_str());
		}
	}
	const auto t0 = std::chrono::steady_clock::now();
	ParityResult r;
	compare_sources(*install.source, *exported.source, !p_args.has("--rows-only"), r);
	std::fputs(format_parity(r).c_str(), stdout);
	std::printf("compared in %.2f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
	return r.ok() ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
	Args args;
	if (!parse_args(argc, argv, args)) {
		return usage();
	}
	const std::string &cmd = args.cmd;
	const size_t n = args.pos.size();
	if (cmd == "census") {
		return cmd_census(args);
	}
	if (cmd == "list" && n >= 1) {
		return cmd_list(args);
	}
	if (cmd == "clip" && n >= 2) {
		return cmd_clip(args);
	}
	if (cmd == "compare" && n >= 1) {
		return cmd_compare(args);
	}
	if (cmd == "bench") {
		return cmd_bench(args);
	}
	if (cmd == "sample" && n >= 3) {
		return cmd_sample(args);
	}
	if (cmd == "rigmap" && n >= 2) {
		return cmd_rigmap(args);
	}
	if (cmd == "sources") {
		return cmd_sources(args);
	}
	if (cmd == "parity") {
		return cmd_parity(args);
	}
	return usage();
}
