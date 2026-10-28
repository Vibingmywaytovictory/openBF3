#include "antdump_lib.h"

#include "mini_json.h"

#include "bf3/core/install_reader.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/elapsed.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace bf3::tools::antdump {

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kQuatOffThreshold = 2e-3;

using bf3::core::seconds_since;

void append_number(std::string &r_out, double p_value, int p_digits) {
	char buf[64];
	if (p_digits >= 0) {
		const double scale = std::pow(10.0, p_digits);
		double v = std::round(p_value * scale) / scale;
		if (v == 0.0) {
			v = 0.0;
		}
		std::snprintf(buf, sizeof(buf), "%.*g", p_digits + 3, v);
	} else {
		std::snprintf(buf, sizeof(buf), "%.9g", p_value);
	}
	r_out += buf;
}

void append_string(std::string &r_out, std::string_view p_text) {
	r_out.push_back('"');
	for (char c : p_text) {
		if (c == '"' || c == '\\') {
			r_out.push_back('\\');
			r_out.push_back(c);
		} else if (static_cast<unsigned char>(c) < 0x20) {
			char buf[8];
			std::snprintf(buf, sizeof(buf), "\\u%04x", c);
			r_out += buf;
		} else {
			r_out.push_back(c);
		}
	}
	r_out.push_back('"');
}

void append_floats(std::string &r_out, const float *p_values, int p_count, int p_digits) {
	r_out.push_back('[');
	for (int i = 0; i < p_count; ++i) {
		if (i) {
			r_out.push_back(',');
		}
		append_number(r_out, p_values[i], p_digits);
	}
	r_out.push_back(']');
}

uint32_t header_key_count(const GdStruct &p_clip, ClipCodec p_codec) {
	uint32_t keys = 0;
	if (p_codec == ClipCodec::Frame) {
		return 1;
	}
	if (p_codec == ClipCodec::Curve) {
		const GdArray k = p_clip.array("Keys");
		return k.valid() && k.size() > 0 ? k.size() : 1;
	}
	p_clip.read_u32("NumKeys", keys);
	return keys;
}

}

bool load_library(const AntPackageSource &p_source, AntLibrary &r_library, std::string &r_error) {
	for (const AntPackageInfo &info : p_source.packages()) {
		if (info.packaging != AntPackagingType::Static) {
			continue;
		}
		std::unique_ptr<AntPackage> p = load_package(p_source, info, r_error);
		if (!p) {
			return false;
		}
		const AntResult r = r_library.set_static(std::move(p));
		if (!r.ok()) {
			r_error = r.message;
			return false;
		}
		return true;
	}
	r_error = "the source has no Static package (s_BasicAssets)";
	return false;
}

std::unique_ptr<AntPackage> load_package(const AntPackageSource &p_source, const AntPackageInfo &p_info, std::string &r_error) {
	std::vector<uint8_t> bytes;
	if (!p_source.read(p_info, bytes, r_error)) {
		return nullptr;
	}
	auto p = std::make_unique<AntPackage>();
	if (!p->parse(p_info.short_name, std::move(bytes), r_error)) {
		return nullptr;
	}
	return p;
}

bool run_census(const AntPackageSource &p_source, const AntLibrary &p_library, bool p_mp_only, Census &r_census, std::string &r_error) {
	return run_censuses(p_source, p_library, p_mp_only ? &r_census : nullptr, p_mp_only ? nullptr : &r_census, r_error);
}

bool run_censuses(const AntPackageSource &p_source, const AntLibrary &p_library, Census *r_mp, Census *r_all, std::string &r_error,
		const PackageLoader &p_load) {
	const Clock::time_point start = Clock::now();
	const AntPackage *static_package = p_library.static_package();
	if (static_package == nullptr) {
		r_error = "library has no static package";
		return false;
	}
	for (Census *c : { r_mp, r_all }) {
		if (c != nullptr) {
			*c = {};
			c->mp_only = c == r_mp;
			c->static_objects = static_package->object_count();
			c->static_layouts = static_package->archive().layouts().size();
		}
	}

	auto census_package = [&](const AntPackage &p_package, bool p_static, bool p_mp) {
		Census *const targets[2] = { p_mp ? r_mp : nullptr, r_all };
		for (uint32_t index : AntLibrary::clips(p_package)) {
			const GdStruct clip = p_package.object(index);
			const ClipCodec codec = clip_codec(clip);
			const std::string codec_label = codec == ClipCodec::Unknown ? std::string(clip.type_name()) : codec_name(codec);
			DecodedClip decoded;
			DecodeStats stats;
			const Clock::time_point t0 = Clock::now();
			const AntResult r = decode_clip(clip, decoded, &stats);
			const double decode_seconds = seconds_since(t0);
			const bool mapped_codec = codec == ClipCodec::Dct || codec == ClipCodec::Raw || codec == ClipCodec::Frame ||
					codec == ClipCodec::Vbr || codec == ClipCodec::Curve;
			ClipBinding b;
			const AntResult br = mapped_codec ? p_library.bind(p_package, clip, b) : AntResult{};
			for (Census *target : targets) {
				if (target == nullptr) {
					continue;
				}
				Census &r_census = *target;
				CodecCensus &cc = r_census.codecs[codec_label];
				++cc.clips;
				if (p_static) {
					++r_census.static_codecs[codec_label];
				}
				if (codec == ClipCodec::Dct) {
					r_census.dct_decode_seconds += decode_seconds;
				}
				if (r.status == AntStatus::Unsupported) {
					++cc.unsupported;
				} else if (!r.ok()) {
					++cc.errors;
					if (codec == ClipCodec::Dct && r.message.find("overrun") != std::string::npos) {
						++r_census.dct_overruns;
					}
					if (codec == ClipCodec::Raw) {
						++r_census.raw_other;
					}
					if (codec == ClipCodec::Frame) {
						++r_census.frame_other;
					}
					if (r_census.notes.size() < 20) {
						r_census.notes.push_back(p_package.name() + "/" + std::string(clip.name()) + ": " + r.message);
					}
				} else {
					++cc.decoded;
					cc.keys += decoded.num_keys;
					cc.channel_values += uint64_t(decoded.num_keys) * decoded.channel_count();
					if (decoded.num_keys != header_key_count(clip, codec) || decoded.key_times.size() != decoded.num_keys) {
						++cc.key_count_mismatch;
					}
					bool unit = true;
					for (const Quat &q : decoded.quats) {
						const double len = std::sqrt(double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w);
						unit = unit && std::fabs(len - 1.0) < 1e-4;
					}
					cc.non_unit_quats += unit ? 0 : 1;
					if (codec == ClipCodec::Dct) {
						++r_census.dct_ok;
						++r_census.dct_datasize_residual[stats.data_size_residual];
						r_census.dct_bitstream_bytes += stats.bitstream_bytes;
						r_census.dct_bits_read += stats.bits_read;
						if (stats.mean_abs_quat_error > kQuatOffThreshold) {
							++r_census.dct_q_off;
							if (r_census.notes.size() < 40) {
								char buf[256];
								std::snprintf(buf, sizeof(buf), "%s/%s: mean |q|-1 = %.5f (keys %u, q %u)", p_package.name().c_str(), std::string(clip.name()).c_str(),
										stats.mean_abs_quat_error, decoded.num_keys, decoded.num_quats);
								r_census.notes.push_back(buf);
							}
						}
						r_census.dct_worst_mean_q = std::max(r_census.dct_worst_mean_q, stats.mean_abs_quat_error);
					} else if (codec == ClipCodec::Raw) {
						++(stats.float_block_padded ? r_census.raw_padded : r_census.raw_exact);
					} else if (codec == ClipCodec::Frame) {
						++(stats.float_block_padded ? r_census.frame_other : r_census.frame_exact);
					}
				}
				if (mapped_codec) {
					if (br.status == AntStatus::NotFound) {
						++r_census.clips_without_controller;
					} else if (!br.ok()) {
						++r_census.bind_errors;
						if (r_census.notes.size() < 60) {
							r_census.notes.push_back(p_package.name() + "/" + std::string(clip.name()) + ": bind: " + br.message);
						}
					} else {
						++r_census.clips_with_controller;
						++cc.mapped;
						r_census.channels_total += b.tracks.size();
						r_census.channels_ok += b.tracks.size() - b.kind_mismatches;
						r_census.channels_on_delta_traj += b.delta_traj_channels;
						if (b.kind_mismatches == 0) {
							++r_census.clips_kind_consistent;
							++cc.mapped_perfect;
						}
					}
				}
			}
		}
	};

	census_package(*static_package, true, true);
	for (const AntPackageInfo &info : p_source.packages()) {
		const bool mp = is_mp_relevant(info.short_name);
		if (info.packaging == AntPackagingType::Static || (r_all == nullptr && !mp)) {
			continue;
		}
		Census *const targets[2] = { mp ? r_mp : nullptr, r_all };
		std::string error;
		std::unique_ptr<AntPackage> own;
		const AntPackage *p = nullptr;
		if (p_load) {
			p = p_load(info, error);
		} else {
			own = load_package(p_source, info, error);
			p = own.get();
		}
		for (Census *c : targets) {
			if (c == nullptr) {
				continue;
			}
			++c->packages;
			if (p == nullptr) {
				++c->packages_failed;
				c->failures.push_back(error);
			} else if (info.packaging == AntPackagingType::Chunk) {
				++c->chunk_packages;
				c->chunk_size_ok += p->byte_size() == info.chunk_size ? 1 : 0;
			}
		}
		if (p != nullptr) {
			census_package(*p, false, mp);
		}
	}
	for (Census *c : { r_mp, r_all }) {
		if (c != nullptr) {
			c->total_seconds = seconds_since(start);
		}
	}
	return true;
}

std::string format_census(const Census &c) {
	std::string out;
	char buf[512];
	std::snprintf(buf, sizeof(buf), "packages parsed %u, failed %u (mpOnly=%s); chunk packages %u, length == ChunkSize %u; total %.2f s\n", c.packages,
			c.packages_failed, c.mp_only ? "True" : "False", c.chunk_packages, c.chunk_size_ok, c.total_seconds);
	out += buf;
	std::snprintf(buf, sizeof(buf), "static s_basicassets: %llu objects, %llu layouts\n", (unsigned long long)c.static_objects, (unsigned long long)c.static_layouts);
	out += buf;
	out += "codec census (AnimationAsset subclasses, all packages incl. s_basicassets):";
	for (const auto &[name, cc] : c.codecs) {
		std::snprintf(buf, sizeof(buf), " %s=%u", name.c_str(), cc.clips);
		out += buf;
	}
	out += "\nstatic s_basicassets codecs:";
	for (const auto &[name, n] : c.static_codecs) {
		std::snprintf(buf, sizeof(buf), " %s=%u", name.c_str(), n);
		out += buf;
	}
	out += "\nper codec: clips / decoded / unsupported / errors / key-count mismatches / non-unit |q| / keys / channel values / mapped / mapped perfect\n";
	for (const auto &[name, cc] : c.codecs) {
		std::snprintf(buf, sizeof(buf), "  %-20s %5u %5u %5u %3u %3u %3u %9llu %12llu %5u %5u\n", name.c_str(), cc.clips, cc.decoded, cc.unsupported, cc.errors,
				cc.key_count_mismatch, cc.non_unit_quats, (unsigned long long)cc.keys, (unsigned long long)cc.channel_values, cc.mapped, cc.mapped_perfect);
		out += buf;
	}
	std::snprintf(buf, sizeof(buf), "DCT: ok %u, overruns %u, |q| off (mean |q|-1 > 2e-3) %u; worst mean |q|-1 %.5f; bitstream %llu bytes (%llu KiB), %llu bits read; decode %.3f s (%.1f MB/s)\n",
			c.dct_ok, c.dct_overruns, c.dct_q_off, c.dct_worst_mean_q, (unsigned long long)c.dct_bitstream_bytes, (unsigned long long)(c.dct_bitstream_bytes / 1024),
			(unsigned long long)c.dct_bits_read, c.dct_decode_seconds, c.dct_decode_seconds > 0 ? double(c.dct_bitstream_bytes) / 1e6 / c.dct_decode_seconds : 0.0);
	out += buf;
	out += "DCT DataSize - (12 + n + 8n + 2*subblocks + Data) histogram:";
	for (const auto &[k, n] : c.dct_datasize_residual) {
		std::snprintf(buf, sizeof(buf), " %lld:%u", (long long)k, n);
		out += buf;
	}
	std::snprintf(buf, sizeof(buf), "\nRaw: size-consistent %u, float block padded to 4 %u, other %u; Frame: consistent %u, other %u\n", c.raw_exact, c.raw_padded,
			c.raw_other, c.frame_exact, c.frame_other);
	out += buf;
	std::snprintf(buf, sizeof(buf), "mapping: clips with controller %u (without %u, bind errors %u); all channels kind-consistent %u; channels ok %llu/%llu; channels on DeltaTrajectory dofs %llu\n",
			c.clips_with_controller, c.clips_without_controller, c.bind_errors, c.clips_kind_consistent, (unsigned long long)c.channels_ok,
			(unsigned long long)c.channels_total, (unsigned long long)c.channels_on_delta_traj);
	out += buf;
	for (const std::string &f : c.failures) {
		out += "FAILED " + f + "\n";
	}
	for (const std::string &n : c.notes) {
		out += "  " + n + "\n";
	}
	return out;
}

bool clip_to_json(const AntLibrary &p_library, const AntPackage &p_package, const GdStruct &p_clip, int p_digits, std::string &r_json, std::string &r_error) {
	DecodedClip decoded;
	const AntResult r = decode_clip(p_clip, decoded);
	if (!r.ok()) {
		r_error = r.message;
		return false;
	}
	ClipBinding b;
	const AntResult br = p_library.bind(p_package, p_clip, b);
	if (!br.ok()) {
		r_error = "bind: " + br.message;
		return false;
	}
	std::string &o = r_json;
	o.clear();
	o += "{\n\"source\":{\"package\":";
	append_string(o, p_package.name());
	o += ",\"clip\":";
	append_string(o, p_clip.name());
	o += ",\"codec\":";
	append_string(o, p_clip.type_name());
	o += ",\"controller\":";
	append_string(o, b.controller_name);
	o += ",\"target\":";
	append_string(o, b.target_name);
	o += ",\"rig\":";
	append_string(o, b.rig ? b.rig->name : std::string_view());
	o += ",\"skeleton\":";
	append_string(o, b.rig ? b.rig->skeleton_name : std::string_view());
	o += "},\n\"fps\":";
	append_number(o, b.fps, -1);
	o += ",\"fpsScale\":";
	append_number(o, b.fps_scale, -1);
	o += ",\"numTicks\":";
	append_number(o, b.num_ticks, -1);
	o += ",\"endFrame\":" + std::to_string(decoded.end_frame) + ",\"keyTimes\":[";
	for (uint32_t k = 0; k < decoded.num_keys; ++k) {
		o += (k ? "," : "") + std::to_string(decoded.key_times[k]);
	}
	o += "],\n\"joints\":[";
	if (b.rig) {
		for (size_t j = 0; j < b.rig->joints.size(); ++j) {
			const AntJoint &joint = b.rig->joints[j];
			o += j ? ",\n  {\"name\":" : "\n  {\"name\":";
			append_string(o, joint.name);
			o += ",\"parent\":" + std::to_string(joint.parent) + ",\"t\":";
			append_floats(o, joint.rest_t, 3, p_digits);
			o += ",\"q\":";
			append_floats(o, joint.rest_q, 4, p_digits);
			o += "}";
		}
	}

	for (int pass = 0; pass < 2; ++pass) {
		o += pass == 0 ? "\n],\n\"tracks\":[" : "\n],\n\"floatTracks\":[";
		bool first = true;
		for (size_t c = 0; c < b.tracks.size(); ++c) {
			const ChannelTrack &t = b.tracks[c];
			const AntDof &dof = b.dofs[t.dof];
			const bool joint_channel = dof.kind == DofKind::Rotation || dof.kind == DofKind::Translation || dof.kind == DofKind::Scale;
			if (joint_channel != (pass == 0)) {
				continue;
			}
			const char *kind = dof.kind == DofKind::Rotation ? "q" : dof.kind == DofKind::Translation ? "t" : dof.kind == DofKind::Scale ? "s" : dof.kind == DofKind::Float ? "f" : "?";
			o += first ? "\n  {\"joint\":" : ",\n  {\"joint\":";
			first = false;
			o += std::to_string(t.joint) + ",\"name\":";
			append_string(o, dof.joint.empty() ? dof.slot : dof.joint);
			o += ",\"kind\":";
			append_string(o, kind);
			o += ",\"dof\":";
			append_string(o, std::string(dof.layout) + "." + std::string(dof.slot));
			o += ",\"channel\":" + std::to_string(t.channel) + ",\"values\":[";
			for (uint32_t k = 0; k < decoded.num_keys; ++k) {
				if (k) {
					o += ",";
				}
				if (t.kind == 'q') {
					const Quat &q = decoded.quat(k, t.channel);
					const float v[4] = { q.x, q.y, q.z, q.w };
					append_floats(o, v, 4, p_digits);
				} else if (t.kind == 'v') {
					const Vec3 &p = decoded.vec3(k, t.channel - decoded.num_quats);
					const float v[3] = { p.x, p.y, p.z };
					append_floats(o, v, 3, p_digits);
				} else {
					append_number(o, decoded.value(k, t.channel - decoded.num_quats - decoded.num_vec3s), p_digits);
				}
			}
			o += "]}";
		}
	}
	o += "\n]\n}\n";
	return true;
}

namespace {

void diff(CompareResult &r_result, std::string p_text) {
	r_result.shape_ok = false;
	if (r_result.differences.size() < 20) {
		r_result.differences.push_back(std::move(p_text));
	}
}

bool compare_numbers(const json::Value &p_a, const json::Value &p_b, double &r_max, uint64_t &r_count) {
	if (p_a.type == json::Value::Type::Number && p_b.type == json::Value::Type::Number) {
		r_max = std::max(r_max, std::fabs(p_a.num - p_b.num));
		++r_count;
		return true;
	}
	if (p_a.type != json::Value::Type::Array || p_b.type != json::Value::Type::Array || p_a.items.size() != p_b.items.size()) {
		return false;
	}
	for (size_t i = 0; i < p_a.items.size(); ++i) {
		if (!compare_numbers(p_a.items[i], p_b.items[i], r_max, r_count)) {
			return false;
		}
	}
	return true;
}

}

bool compare_clip_json(const std::string &p_ours, const std::string &p_oracle, CompareResult &r, std::string &r_error) {
	r = {};
	json::Value a, b;
	if (!json::parse(p_ours, a)) {
		r_error = "our JSON does not parse";
		return false;
	}
	if (!json::parse(p_oracle, b)) {
		r_error = "oracle JSON does not parse";
		return false;
	}
	r.shape_ok = true;
	const json::Value *sa = a.get("source"), *sb = b.get("source");
	if (!sa || !sb) {
		diff(r, "missing source");
	} else {

		const std::string oracle_clip = sb->string_or("clip", "!");
		if (oracle_clip != sa->string_or("clip", "?") && oracle_clip != sa->string_or("controller", "?")) {
			diff(r, "source.clip: " + sa->string_or("clip", "?") + " vs " + oracle_clip);
		}
		for (const char *k : { "package", "codec", "controller", "target", "rig", "skeleton" }) {
			if (sa->string_or(k, "?") != sb->string_or(k, "?")) {
				diff(r, std::string("source.") + k + ": " + sa->string_or(k, "?") + " vs " + sb->string_or(k, "?"));
			}
		}
	}
	for (const char *k : { "fps", "fpsScale", "numTicks", "endFrame" }) {
		if (std::fabs(a.number_or(k, -1) - b.number_or(k, -2)) > 1e-6) {
			diff(r, std::string(k) + " differs");
		}
	}
	double dummy = 0.0;
	uint64_t n = 0;
	const json::Value *ka = a.get("keyTimes"), *kb = b.get("keyTimes");
	if (!ka || !kb || !compare_numbers(*ka, *kb, dummy, n) || dummy != 0.0) {
		diff(r, "keyTimes differ");
	}
	const json::Value *ja = a.get("joints"), *jb = b.get("joints");
	if (!ja || !jb || ja->items.size() != jb->items.size()) {
		diff(r, "joint count differs");
	} else {
		for (size_t i = 0; i < ja->items.size(); ++i) {
			const json::Value &x = ja->items[i], &y = jb->items[i];
			if (x.string_or("name", "?") != y.string_or("name", "!") || x.number_or("parent", -9) != y.number_or("parent", -8)) {
				diff(r, "joint " + std::to_string(i) + " name/parent differs");
				continue;
			}
			for (const char *k : { "t", "q" }) {
				const json::Value *u = x.get(k), *v = y.get(k);
				if (!u || !v || !compare_numbers(*u, *v, r.max_joint_diff, r.joint_values)) {
					diff(r, "joint " + std::to_string(i) + " " + k + " shape differs");
				}
			}
		}
	}
	const json::Value *ta = a.get("tracks"), *tb = b.get("tracks");
	if (!ta || !tb || ta->items.size() != tb->items.size()) {
		diff(r, "track count differs");
	} else {
		for (size_t i = 0; i < ta->items.size(); ++i) {
			const json::Value &x = ta->items[i], &y = tb->items[i];
			for (const char *k : { "name", "kind", "dof" }) {
				if (x.string_or(k, "?") != y.string_or(k, "!")) {
					diff(r, "track " + std::to_string(i) + " " + k + ": " + x.string_or(k, "?") + " vs " + y.string_or(k, "!"));
				}
			}
			for (const char *k : { "joint", "channel" }) {
				if (x.number_or(k, -9) != y.number_or(k, -8)) {
					diff(r, "track " + std::to_string(i) + " " + k + " differs");
				}
			}
			const json::Value *u = x.get("values"), *v = y.get("values");
			if (!u || !v || !compare_numbers(*u, *v, r.max_track_diff, r.values)) {
				diff(r, "track " + std::to_string(i) + " values shape differs");
			}
		}
	}
	return true;
}

bool oracle_package(const std::string &p_oracle, std::string &r_package, std::string &r_error) {
	json::Value doc;
	if (!json::parse(p_oracle, doc) || doc.get("source") == nullptr) {
		r_error = "not a clip JSON";
		return false;
	}
	r_package = doc.get("source")->string_or("package", "");
	return !r_package.empty();
}

bool find_oracle_clip(const AntLibrary &p_library, const AntPackage &p_package, const std::string &p_oracle, GdStruct &r_clip, std::string &r_error) {
	json::Value doc;
	if (!json::parse(p_oracle, doc) || doc.get("source") == nullptr) {
		r_error = "not a clip JSON";
		return false;
	}
	const json::Value &src = *doc.get("source");
	const std::string name = src.string_or("clip", "");
	const std::string codec = src.string_or("codec", "");
	const double end_frame = doc.number_or("endFrame", -1);
	std::vector<double> key_times;
	if (const json::Value *k = doc.get("keyTimes")) {
		for (const json::Value &v : k->items) {
			key_times.push_back(v.num);
		}
	}
	uint32_t candidates = 0;
	for (uint32_t index : AntLibrary::clips(p_package)) {
		const GdStruct clip = p_package.object(index);
		bool named = clip.name() == name;
		if (!named) {
			ClipBinding b;
			named = p_library.bind(p_package, clip, b).ok() && b.controller_name == name;
		}
		if (!named || clip.type_name() != codec) {
			continue;
		}
		++candidates;
		DecodedClip d;
		if (!decode_clip(clip, d).ok() || double(d.end_frame) != end_frame || d.key_times.size() != key_times.size()) {
			continue;
		}
		bool same = true;
		for (size_t k = 0; k < key_times.size(); ++k) {
			same = same && double(d.key_times[k]) == key_times[k];
		}
		if (same) {
			r_clip = clip;
			return true;
		}
	}
	r_error = "no clip \"" + name + "\" with the oracle's codec/endFrame/keyTimes (" + std::to_string(candidates) + " candidates by name)";
	return false;
}

bool open_source(const SourceOptions &p_options, OpenedSource &r_opened, std::string &r_error, bool *r_unavailable) {
	r_opened = {};
	if (r_unavailable) {
		*r_unavailable = false;
	}
	if (!p_options.install) {
		auto source = std::make_unique<AntExportDirSource>();
		if (!std::filesystem::is_regular_file(std::filesystem::path(p_options.export_dir) / "res.tsv")) {
			if (r_unavailable) {
				*r_unavailable = true;
			}
			r_error = "no export directory at " + p_options.export_dir + " (res.tsv missing)";
			return false;
		}
		if (!source->open(p_options.export_dir, r_error)) {
			return false;
		}
		r_opened.description = "export directory " + p_options.export_dir + ": " + std::to_string(source->packages().size()) + " packages, " +
				std::to_string(source->all_banks().size()) + " banks";
		r_opened.source = std::move(source);
		return true;
	}
	bf3::core::InstallReader reader;
	bf3::core::InstallLocateOptions locate;
	locate.cmdline_path = p_options.install_dir;
	if (!reader.locate(locate)) {
		if (r_unavailable) {
			*r_unavailable = true;
		}
		r_error = "install not usable: " + reader.describe();
		return false;
	}
	bf3::core::Result<std::shared_ptr<bf3::core::Install>> install = bf3::core::Install::open(reader);
	if (!install) {
		r_error = "open install: " + install.error().message;
		return false;
	}
	bf3::core::Result<std::unique_ptr<AntInstallSource>> source = AntInstallSource::open(*install);
	if (!source) {
		r_error = "ant install source: " + source.error().message;
		return false;
	}
	const AntInstallSource::OpenReport &rep = (*source)->report();
	char line[256];
	std::snprintf(line, sizeof(line), ": %zu packages, %zu banks from %zu of %zu superbundles in %.2f s (%zu warnings)", (*source)->packages().size(),
			(*source)->all_banks().size(), rep.superbundles_used, rep.superbundles_scanned, rep.seconds, rep.warnings.size());
	r_opened.description = "install " + reader.status().root_utf8 + line;
	r_opened.install = *install;
	r_opened.source = std::move(*source);
	return true;
}

namespace {

std::string info_row(const AntPackageInfo &p_info) {
	return p_info.partition + " | " + p_info.name + " | " + p_info.short_name + " | " + packaging_name(p_info.packaging) + " | " + p_info.streaming_guid + " | " +
			std::to_string(p_info.chunk_size) + " | " + (p_info.is_asset_package ? "asset" : "set") + " | " + p_info.superbundle + " | " + p_info.bundle;
}

void compare_rows(const char *p_what, const std::vector<AntPackageInfo> &p_a, const std::vector<AntPackageInfo> &p_b, size_t &r_equal,
		std::vector<std::string> &r_differences) {
	constexpr size_t kMaxDifferences = 12;
	if (p_a.size() != p_b.size() && r_differences.size() < kMaxDifferences) {
		r_differences.push_back(std::string(p_what) + ": " + std::to_string(p_a.size()) + " vs " + std::to_string(p_b.size()) + " rows");
	}
	for (size_t i = 0; i < std::min(p_a.size(), p_b.size()); ++i) {
		const std::string a = info_row(p_a[i]), b = info_row(p_b[i]);
		if (a == b) {
			++r_equal;
		} else if (r_differences.size() < kMaxDifferences) {
			r_differences.push_back(std::string(p_what) + " row " + std::to_string(i) + ":\n      " + a + "\n   vs " + b);
		}
	}
}

}

bool ParityResult::ok() const {
	return packages_a == packages_b && package_rows_equal == packages_a && banks_a == banks_b && bank_rows_equal == banks_a && payloads_equal == payloads &&
			payload_errors == 0;
}

void compare_sources(const AntPackageSource &p_a, const AntPackageSource &p_b, bool p_payloads, ParityResult &r_result) {
	r_result = {};
	r_result.packages_a = p_a.packages().size();
	r_result.packages_b = p_b.packages().size();
	r_result.banks_a = p_a.all_banks().size();
	r_result.banks_b = p_b.all_banks().size();
	compare_rows("packages", p_a.packages(), p_b.packages(), r_result.package_rows_equal, r_result.differences);
	compare_rows("banks", p_a.all_banks(), p_b.all_banks(), r_result.bank_rows_equal, r_result.differences);
	if (!p_payloads) {
		return;
	}

	std::vector<std::pair<const AntPackageInfo *, const AntPackageInfo *>> items;
	for (size_t i = 0; i < std::min(p_a.all_banks().size(), p_b.all_banks().size()); ++i) {
		items.emplace_back(&p_a.all_banks()[i], &p_b.all_banks()[i]);
	}
	for (size_t i = 0; i < std::min(p_a.packages().size(), p_b.packages().size()); ++i) {
		if (p_a.packages()[i].packaging == AntPackagingType::Chunk) {
			items.emplace_back(&p_a.packages()[i], &p_b.packages()[i]);
		}
	}
	std::atomic<size_t> equal{ 0 }, errors{ 0 };
	std::atomic<uint64_t> bytes{ 0 };
	std::mutex mutex;
	bf3::core::parallel_for(items.size(), [&](size_t i) {
		std::vector<uint8_t> a, b;
		std::string error_a, error_b;
		const bool ok_a = p_a.read(*items[i].first, a, error_a);
		const bool ok_b = p_b.read(*items[i].second, b, error_b);
		std::string difference;
		if (!ok_a || !ok_b) {
			++errors;
			difference = items[i].first->partition + ": read failed: " + (ok_a ? std::string("-") : error_a) + " / " + (ok_b ? std::string("-") : error_b);
		} else if (a != b) {
			difference = items[i].first->partition + ": payloads differ (" + std::to_string(a.size()) + " vs " + std::to_string(b.size()) + " bytes)";
		} else {
			++equal;
			bytes += a.size();
		}
		if (!difference.empty()) {
			std::lock_guard lock(mutex);
			if (r_result.differences.size() < 24) {
				r_result.differences.push_back(std::move(difference));
			}
		}
	});
	r_result.payloads = items.size();
	r_result.payloads_equal = equal;
	r_result.payload_errors = errors;
	r_result.payload_bytes = bytes;
}

std::string format_parity(const ParityResult &p_result) {
	char buf[512];
	std::snprintf(buf, sizeof(buf),
			"packages: %zu vs %zu, rows equal %zu\n"
			"banks:    %zu vs %zu, rows equal %zu\n"
			"payloads: %zu compared, %zu byte-identical (%.1f MB), %zu read errors\n",
			p_result.packages_a, p_result.packages_b, p_result.package_rows_equal, p_result.banks_a, p_result.banks_b, p_result.bank_rows_equal,
			p_result.payloads, p_result.payloads_equal, double(p_result.payload_bytes) / 1e6, p_result.payload_errors);
	std::string out = buf;
	for (const std::string &d : p_result.differences) {
		out += "  " + d + "\n";
	}
	out += p_result.ok() ? "PARITY OK\n" : "PARITY FAILED\n";
	return out;
}

}
