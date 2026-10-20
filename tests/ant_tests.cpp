#include <doctest/doctest.h>

#include "antdump_lib.h"
#include "bf3/core/ant/ant_clip.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/ant/ant_library.h"
#include "bf3/core/ant/ant_package.h"
#include "bf3/core/ant/ant_source.h"
#include "bf3/core/ant/gd_archive.h"
#include "test_support.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <vector>

using namespace bf3::core::ant;
namespace antdump = bf3::tools::antdump;

namespace {

class BitWriter {
public:
	void put(int32_t p_value, uint32_t p_width) {
		for (uint32_t i = 0; i < p_width; ++i) {
			const uint32_t bit = (uint32_t(p_value) >> (p_width - 1 - i)) & 1u;
			if (pos_ % 8 == 0) {
				bytes.push_back(0);
			}
			bytes.back() |= uint8_t(bit << (7 - pos_ % 8));
			++pos_;
		}
	}
	std::vector<uint8_t> bytes;

private:
	uint64_t pos_ = 0;
};

void put32(std::vector<uint8_t> &r_b, size_t p_at, uint32_t p_v) {
	for (int i = 0; i < 4; ++i) {
		r_b[p_at + i] = uint8_t(p_v >> (8 * i));
	}
}
void put64(std::vector<uint8_t> &r_b, size_t p_at, uint64_t p_v) {
	put32(r_b, p_at, uint32_t(p_v));
	put32(r_b, p_at + 4, uint32_t(p_v >> 32));
}
void put16(std::vector<uint8_t> &r_b, size_t p_at, uint16_t p_v) {
	r_b[p_at] = uint8_t(p_v);
	r_b[p_at + 1] = uint8_t(p_v >> 8);
}

struct FieldSpec {
	const char *name;
	int32_t slot;
	uint32_t hash;
	uint32_t element_size;
	uint32_t offset;
	uint16_t flags;
	int layout;
};

struct LayoutSpec {
	const char *name;
	uint32_t hash;
	uint32_t size;
	uint32_t align;
	std::vector<FieldSpec> fields;
};

std::vector<uint8_t> build_reflection(const std::vector<LayoutSpec> &p_layouts) {
	std::vector<uint8_t> out(8 + 8 * p_layouts.size(), 0);
	put64(out, 0, p_layouts.size());
	std::vector<size_t> at(p_layouts.size());

	size_t cursor = out.size();
	std::vector<std::vector<uint8_t>> blobs;
	for (size_t i = 0; i < p_layouts.size(); ++i) {
		const LayoutSpec &l = p_layouts[i];
		std::string table(1, '\0');
		table += l.name;
		table.push_back('\0');
		std::vector<uint32_t> name_offsets;
		for (const FieldSpec &f : l.fields) {
			name_offsets.push_back(uint32_t(table.size()));
			table += f.name;
			table.push_back('\0');
		}
		std::vector<uint8_t> b(32 + 32 * l.fields.size() + table.size(), 0);
		const int32_t min_slot = l.fields.empty() ? 0 : l.fields.front().slot;
		const int32_t max_slot = l.fields.empty() ? -1 : l.fields.back().slot;
		put32(b, 0, uint32_t(min_slot));
		put32(b, 4, uint32_t(max_slot));
		put32(b, 8, l.size);
		put32(b, 12, l.align);
		put32(b, 16, uint32_t(32 + 32 * l.fields.size()));
		put32(b, 20, uint32_t(table.size()));
		b[24] = 1;
		put32(b, 28, l.hash);
		std::memcpy(b.data() + 32 + 32 * l.fields.size(), table.data(), table.size());
		while (b.size() % 8) {
			b.push_back(0);
		}
		at[i] = cursor;
		cursor += b.size();
		blobs.push_back(std::move(b));
		(void)name_offsets;
	}
	for (size_t i = 0; i < p_layouts.size(); ++i) {
		const LayoutSpec &l = p_layouts[i];
		std::vector<uint8_t> &b = blobs[i];
		uint32_t name_at = uint32_t(1 + std::strlen(l.name) + 1);
		for (size_t k = 0; k < l.fields.size(); ++k) {
			const FieldSpec &f = l.fields[k];
			const size_t e = 32 + 32 * k;
			put32(b, e, f.hash);
			put32(b, e + 4, f.element_size);
			put32(b, e + 8, f.offset);
			put32(b, e + 12, name_at);
			put16(b, e + 16, 1);
			put16(b, e + 18, f.flags);
			put16(b, e + 20, 4);
			put64(b, e + 24, f.layout >= 0 ? at[size_t(f.layout)] : 0);
			name_at += uint32_t(std::strlen(f.name) + 1);
		}
		put64(out, 8 + 8 * i, at[i]);
		out.insert(out.end(), b.begin(), b.end());
	}
	return out;
}

void append_blob(std::vector<uint8_t> &r_out, const char *p_id, const std::vector<uint8_t> &p_payload) {
	const size_t at = r_out.size();
	r_out.resize(at + 16, 0);
	std::memcpy(r_out.data() + at, p_id, 7);
	r_out[at + 7] = 'l';
	put32(r_out, at + 8, uint32_t(16 + p_payload.size()));
	put32(r_out, at + 12, uint32_t(p_payload.size()));
	r_out.insert(r_out.end(), p_payload.begin(), p_payload.end());
}

constexpr uint32_t kThingHash = 0x12345678;

std::vector<uint8_t> build_package() {
	const std::vector<LayoutSpec> layouts = {
		{ "UInt32", 7, 4, 4, {} },
		{ "Float", 10, 4, 4, {} },
		{ "Guid", 16, 16, 4, {} },
		{ "String", 17, 16, 8, {} },
		{ "Thing", kThingHash, 52, 8,

				{ { "__guid", -2, 16, 16, 32, 0, 2 }, { "__name", -1, 17, 16, 0, 0, 3 }, { "Count", 0, 7, 4, 48, 0, 0 }, { "Values", 1, 10, 16, 16, 1, 1 } } },
	};
	std::vector<uint8_t> data(32 + 56 + 16 + 8, 0);
	put32(data, 0, GdArchive::kDataMagic);
	put32(data, 16, kThingHash);
	put16(data, 28, 32);
	const size_t inst = 32;
	const size_t floats = inst + 56;
	const size_t text = floats + 16;
	put32(data, inst + 0, 6);
	put32(data, inst + 4, 6);
	put64(data, inst + 8, text);
	put32(data, inst + 16, 3);
	put32(data, inst + 20, 3);
	put64(data, inst + 24, floats);
	data[inst + 32] = 0x00;
	data[inst + 35] = 0x42;
	put32(data, inst + 48, 7);
	const float v[3] = { 1.5f, -2.0f, 3.0f };
	std::memcpy(data.data() + floats, v, 12);
	std::memcpy(data.data() + text, "Hello", 6);

	std::vector<uint8_t> stream;
	append_blob(stream, "GD.REFL", build_reflection(layouts));
	append_blob(stream, "GD.DATA", data);
	std::vector<uint8_t> archive(16, 0);
	std::memcpy(archive.data(), "GD.STRMl", 8);
	archive.insert(archive.end(), stream.begin(), stream.end());
	put32(archive, 8, uint32_t(archive.size()));
	put32(archive, 12, uint32_t(data.size() + 16));

	std::vector<uint8_t> pkg = { 0, 0, 0, 1, 0, 0, 0, 0x18, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
	pkg.insert(pkg.end(), archive.begin(), archive.end());
	return pkg;
}

double quat_angle_deg(const Quat &p_a, const Quat &p_b) {
	const double dot = double(p_a.x) * p_b.x + double(p_a.y) * p_b.y + double(p_a.z) * p_b.z + double(p_a.w) * p_b.w;
	const double s = dot < 0.0 ? -1.0 : 1.0;
	const double dx = p_a.x - s * p_b.x, dy = p_a.y - s * p_b.y, dz = p_a.z - s * p_b.z, dw = p_a.w - s * p_b.w;
	const double chord = std::sqrt(dx * dx + dy * dy + dz * dz + dw * dw);
	return 4.0 * std::asin(std::min(1.0, chord / 2.0)) * 180.0 / std::numbers::pi;
}

double dct_reference(const int32_t p_c[8], uint32_t p_n, uint32_t p_qms, uint32_t p_qmb) {
	double v = 0.0;
	for (uint32_t i = 0; i < 8; ++i) {
		const double ci = i == 0 ? 0.25 : 0.5;
		v += p_c[i] * ci * std::cos((2.0 * p_n + 1.0) * i * std::numbers::pi / 16.0) * (1.0 + 0.1 * p_qms * i) / p_qmb;
	}
	return v;
}

std::string env_var(const char *p_name) {
	char *value = nullptr;
	size_t size = 0;
	std::string out;
	if (_dupenv_s(&value, &size, p_name) == 0 && value != nullptr) {
		out = value;
	}
	std::free(value);
	return out;
}

struct AntData {
	antdump::OpenedSource opened;
	AntLibrary library;
	bool ok = false;
	std::string why;
	const AntPackageSource &source() const { return *opened.source; }
};

AntData *ant_data() {
	static AntData *data = [] {
		auto *d = new AntData();
		antdump::SourceOptions options;
		options.export_dir = env_var("OPENBF3_ANT_DATA");
		std::string error;
		bool unavailable = true;
		bool opened = false;
		if (!options.export_dir.empty()) {
			opened = antdump::open_source(options, d->opened, error, &unavailable);
			if (!opened && unavailable) {
				MESSAGE("OPENBF3_ANT_DATA: " << error << "; trying the install");
			}
		}
		if (!opened && unavailable) {
			options.install = true;
			opened = antdump::open_source(options, d->opened, error, &unavailable);
		}
		if (!opened) {
			d->why = error;
			if (!unavailable) {
				FAIL_CHECK("Ant input does not open: " << error);
			}
			return d;
		}
		MESSAGE("Ant input: " << d->opened.description);
		if (!antdump::load_library(d->source(), d->library, error)) {
			FAIL_CHECK("s_basicassets: " << error);
			return d;
		}
		d->ok = true;
		return d;
	}();
	return data;
}

void optional_input_missing(const char *p_why) {
	std::printf("NOT RUN: %s\n", p_why);
}

AntData *data_or_skip() {
	AntData *d = ant_data();
	if (!d->ok) {
		bf3::test::skip_install_case(d->why.empty() ? "the Ant input did not open" : d->why.c_str());
		return nullptr;
	}
	return d;
}

struct PackageCache {
	std::map<std::string, std::unique_ptr<AntPackage>> parsed;
	std::map<std::string, std::string> errors;
	size_t reused = 0;
};

PackageCache &package_cache() {
	static PackageCache s_cache;
	return s_cache;
}

std::string package_key(const AntPackageInfo &p_info) {
	std::string name = p_info.name;
	std::transform(name.begin(), name.end(), name.begin(), [](unsigned char p_c) { return char(std::tolower(p_c)); });
	return std::to_string(int(p_info.packaging)) + "|" + name + "|" + p_info.streaming_guid;
}

const AntPackage *cached_package(const AntData &p_data, const AntPackageInfo &p_info, std::string &r_error) {
	PackageCache &c = package_cache();
	const std::string key = package_key(p_info);
	if (auto it = c.parsed.find(key); it != c.parsed.end()) {
		++c.reused;
		return it->second.get();
	}
	if (auto it = c.errors.find(key); it != c.errors.end()) {
		r_error = it->second;
		return nullptr;
	}
	std::unique_ptr<AntPackage> p = antdump::load_package(p_data.source(), p_info, r_error);
	if (!p) {
		c.errors[key] = r_error;
		return nullptr;
	}
	return (c.parsed[key] = std::move(p)).get();
}

struct Censuses {
	bool ok = false;
	std::string error;
	antdump::Census mp, all;
};

const Censuses &censuses(const AntData &p_data) {
	static Censuses s_censuses;
	static bool s_done = false;
	if (!s_done) {
		s_done = true;
		s_censuses.ok = antdump::run_censuses(p_data.source(), p_data.library, &s_censuses.mp, &s_censuses.all, s_censuses.error,
				[&](const AntPackageInfo &p_info, std::string &r_error) { return cached_package(p_data, p_info, r_error); });
		MESSAGE("censuses: one pass, " << package_cache().reused << " packages reused from the bank case");
	}
	return s_censuses;
}

}

TEST_SUITE("ant-unit") {
	TEST_CASE("ant: GenericData archive, fields by name") {
		const std::vector<uint8_t> bytes = build_package();
		AntPackage p;
		std::string error;
		REQUIRE_MESSAGE(p.parse("synthetic", bytes, error), error);
		CHECK(p.packaging() == AntPackagingType::Chunk);
		CHECK(p.meta().asset_count == 1);
		CHECK(p.archive().layouts().size() == 5);
		REQUIRE(p.object_count() == 1);
		const GdStruct o = p.object(0);
		CHECK(o.type_name() == "Thing");
		CHECK(o.name() == "Hello");
		CHECK(o.guid().index() == 0x42);
		CHECK(p.find(0x42).valid());
		uint32_t count = 0;
		CHECK(o.read_u32("Count", count));
		CHECK(count == 7);
		std::vector<float> values;
		CHECK(o.array("Values").copy_f32(values));
		CHECK(values == std::vector<float>{ 1.5f, -2.0f, 3.0f });
		float f = 0.0f;
		CHECK_FALSE(o.read_float("Missing", f));
		std::string_view s;
		CHECK_FALSE(o.read_string("Count", s));
	}

	TEST_CASE("ant: malformed packages fail cleanly (truncation and byte flips)") {
		const std::vector<uint8_t> good = build_package();
		std::string error;
		for (size_t n = 0; n < good.size(); ++n) {
			AntPackage p;
			CHECK_FALSE(p.parse("t", std::vector<uint8_t>(good.begin(), good.begin() + std::ptrdiff_t(n)), error));
		}

		for (size_t i = 0; i < good.size(); ++i) {
			for (uint8_t x : { uint8_t(0xFF), uint8_t(0x80), uint8_t(0x01) }) {
				std::vector<uint8_t> bad = good;
				bad[i] ^= x;
				AntPackage p;
				if (p.parse("t", std::move(bad), error) && p.object_count() == 1) {
					const GdStruct o = p.object(0);
					uint32_t c;
					std::vector<float> v;
					o.read_u32("Count", c);
					o.array("Values").copy_f32(v);
					(void)o.name();
				}
			}
		}
		AntPackage p;
		CHECK_FALSE(p.parse("t", { 0, 0, 0, 9 }, error));
		CHECK_FALSE(p.parse("t", { 0, 0, 0, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, error));
	}

	TEST_CASE("ant: DCT codec on synthetic input") {

		const uint16_t key_times[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
		const uint8_t desc[2] = { 0x00, 0x20 };
		const int16_t dx[2] = { 0, 1000 }, dy[2] = { 0, -500 }, dz[2] = { 0, 0 }, dw[2] = { 16384, 0 };

		const uint16_t widths[2] = { 0x6500, 0xF320 };
		BitWriter w;

		w.put(-200, 9);
		w.put(3, 3);
		w.put(-1, 2);

		w.put(-20, 6);
		w.put(9, 5);
		w.put(255, 9);
		w.put(-4, 3);
		w.put(1, 2);
		DctInput in;
		in.num_keys = 10;
		in.num_quats = 1;
		in.num_vec3s = 1;
		in.quantize_mult_block = 4096;
		in.quantize_mult_subblock = 3;
		in.catch_all_bit_count = 9;
		in.key_times = key_times;
		in.dof_table_desc = desc;
		in.delta_base[0] = dx;
		in.delta_base[1] = dy;
		in.delta_base[2] = dz;
		in.delta_base[3] = dw;
		in.bits_per_subblock = widths;
		in.data = w.bytes;
		DecodedClip out;
		DecodeStats stats;
		const AntResult r = decode_dct(in, out, &stats);
		REQUIRE_MESSAGE(r.ok(), r.message);
		CHECK(stats.bits_read == 14 + 25);
		CHECK(out.num_keys == 10);
		for (uint32_t k = 0; k < 10; ++k) {
			CHECK(out.quat(k, 0).w == doctest::Approx(1.0));
			CHECK(out.quat(k, 0).x == doctest::Approx(0.0));
		}
		const int32_t b0x[8] = { 1000, -200 }, b0y[8] = { -500, 3 }, b0z[8] = { 0, -1 };
		const int32_t b1x[8] = { 1000 - 20, 255 }, b1y[8] = { -500 + 9, -4 }, b1z[8] = { 0, 1 };
		for (uint32_t k = 0; k < 10; ++k) {
			const bool first = k < 8;
			const uint32_t n = k % 8;
			CHECK(out.vec3(k, 0).x == doctest::Approx(dct_reference(first ? b0x : b1x, n, 3, 4096)).epsilon(1e-6));
			CHECK(out.vec3(k, 0).y == doctest::Approx(dct_reference(first ? b0y : b1y, n, 3, 4096)).epsilon(1e-6));
			CHECK(out.vec3(k, 0).z == doctest::Approx(dct_reference(first ? b0z : b1z, n, 3, 4096)).epsilon(1e-6));
		}

		std::vector<uint8_t> shorter(w.bytes.begin(), w.bytes.end() - 1);
		in.data = shorter;
		CHECK(decode_dct(in, out).status == AntStatus::Malformed);
		in.data = w.bytes;
		const uint16_t wrong_widths[1] = { 0 };
		in.bits_per_subblock = wrong_widths;
		CHECK(decode_dct(in, out).status == AntStatus::Malformed);
	}

	TEST_CASE("ant: sampling interpolates between sparse keys") {
		DecodedClip c;
		c.codec = ClipCodec::Raw;
		c.num_keys = 2;
		c.num_quats = 1;
		c.num_vec3s = 1;
		c.num_floats = 1;
		c.key_times = { 0, 4 };
		const float h = float(std::sqrt(0.5));
		c.quats = { Quat{ 0, 0, 0, 1 }, Quat{ 0, 0, h, h } };
		c.vec3s = { Vec3{ 0, 0, 0 }, Vec3{ 4, 8, -4 } };
		c.floats = { 1.0f, 3.0f };
		c.end_frame = 4;
		ClipPose pose;
		sample_clip(c, 2.0f, pose);
		CHECK(quat_angle_deg(pose.quats[0], Quat{ 0, 0, 0, 1 }) == doctest::Approx(45.0).epsilon(1e-4));
		CHECK(pose.quats[0].z == doctest::Approx(std::sin(std::numbers::pi / 8)).epsilon(1e-5));
		CHECK(pose.vec3s[0].y == doctest::Approx(4.0));
		CHECK(pose.floats[0] == doctest::Approx(2.0));
		sample_clip(c, -3.0f, pose);
		CHECK(pose.vec3s[0].x == 0.0f);
		sample_clip(c, 99.0f, pose);
		CHECK(pose.vec3s[0].x == 4.0f);
		sample_clip_seconds(c, 1.0f / 30.0f, 30.0f, pose);
		CHECK(pose.vec3s[0].x == doctest::Approx(1.0));
		DecodedClip dense;
		REQUIRE(bake_frames(c, dense).ok());
		CHECK(dense.num_keys == 5);
		CHECK(dense.vec3(3, 0).z == doctest::Approx(-3.0));
	}

	TEST_CASE("ant: bake_frames and sample_clip reject untrusted frame ranges and inconsistent clips") {
		DecodedClip c;
		c.codec = ClipCodec::Raw;
		c.num_keys = 1;
		c.num_quats = 1;
		c.key_times = { 0 };
		c.quats = { Quat{} };
		DecodedClip dense;
		for (uint32_t end : { 0xFFFFFFFFu, 0x7FFFFFFFu, 0x10000u }) {
			c.end_frame = end;
			CHECK(bake_frames(c, dense).status == AntStatus::Malformed);
			CHECK(dense.num_keys == 0);
			CHECK(dense.key_times.empty());
		}
		c.end_frame = kMaxFrame;
		REQUIRE(bake_frames(c, dense).ok());
		CHECK(dense.num_keys == 65536);
		CHECK(dense.key_times.back() == 65535);
		CHECK(dense.consistent());
		c.end_frame = 0;
		c.key_times = { 3, 1 };
		c.num_keys = 2;
		c.quats = { Quat{}, Quat{} };
		CHECK(bake_frames(c, dense).status == AntStatus::Malformed);
		c.quats = { Quat{} };
		CHECK_FALSE(c.consistent());
		CHECK(bake_frames(c, dense).status == AntStatus::Malformed);
		ClipPose pose;
		CHECK_FALSE(sample_clip(c, 1.0f, pose));
		REQUIRE(pose.quats.size() == 1);
		CHECK(pose.quats[0].w == 1.0f);
		DecodedClip empty;
		CHECK(bake_frames(empty, dense).status == AntStatus::Malformed);
	}

	TEST_CASE("ant: DCT 32-bit catch-all coefficient plus DeltaBase does not overflow") {

		const uint16_t key_times[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
		const uint8_t desc[1] = { 0x10 };
		const int16_t dx[1] = { 32767 }, dy[1] = { -32768 }, dz[1] = { 0 }, dw[1] = { 0 };
		const uint16_t widths[1] = { 0xFF00 };
		BitWriter w;
		w.put(INT32_MAX, 32);
		w.put(INT32_MIN, 32);
		DctInput in;
		in.num_keys = 9;
		in.num_vec3s = 1;
		in.quantize_mult_block = 1;
		in.catch_all_bit_count = 32;
		in.key_times = key_times;
		in.dof_table_desc = desc;
		in.delta_base[0] = dx;
		in.delta_base[1] = dy;
		in.delta_base[2] = dz;
		in.delta_base[3] = dw;
		in.bits_per_subblock = widths;
		in.data = w.bytes;
		DecodedClip out;
		const AntResult r = decode_dct(in, out);
		REQUIRE_MESSAGE(r.ok(), r.message);
		CHECK(out.vec3(0, 0).x == doctest::Approx(32767.0 * 0.25));
		CHECK(out.vec3(8, 0).x == doctest::Approx((double(INT32_MAX) + 32767.0) * 0.25).epsilon(1e-7));
		CHECK(out.vec3(8, 0).y == doctest::Approx((double(INT32_MIN) - 32768.0) * 0.25).epsilon(1e-7));

		const uint16_t unsorted[9] = { 0, 1, 2, 3, 5, 4, 6, 7, 8 };
		in.key_times = unsorted;
		CHECK(decode_dct(in, out).status == AntStatus::Malformed);

		std::vector<uint16_t> many(size_t(1) << 20, 0);
		in.key_times = many;
		in.num_keys = uint32_t(many.size());
		in.num_vec3s = 17;
		const AntResult big = decode_dct(in, out);
		CHECK(big.status == AntStatus::Malformed);
		CHECK(big.message.find("bound") != std::string::npos);
	}
}

TEST_SUITE("ant") {
	TEST_CASE("ant: every bank of the export parses") {
		AntData *d = data_or_skip();
		if (!d) {
			return;
		}
		const AntPackage &s = *d->library.static_package();
		CHECK(s.packaging() == AntPackagingType::Static);
		CHECK(s.object_count() == 62766);
		CHECK(s.archive().layouts().size() == 261);
		CHECK(s.byte_size() == 22481708);
		uint32_t parsed = 0, failed = 0, sets = 0, chunk_ok = 0, chunks = 0;
		const auto t0 = std::chrono::steady_clock::now();
		for (const AntPackageInfo &info : d->source().all_banks()) {
			std::string error;

			std::unique_ptr<AntPackage> own;
			const AntPackage *p = nullptr;
			if (info.is_asset_package) {
				p = cached_package(*d, info, error);
			} else {
				own = antdump::load_package(d->source(), info, error);
				p = own.get();
			}
			if (!p) {
				++failed;
				MESSAGE(error);
				continue;
			}
			++parsed;
			sets += p->packaging() == AntPackagingType::AnimationSet ? 1 : 0;
			if (info.short_name == "b_basicassetsmp") {
				CHECK(p->archive().layouts().size() == 22);
				CHECK(p->byte_size() == 15929912);
			}
		}
		for (const AntPackageInfo &info : d->source().packages()) {
			if (info.packaging == AntPackagingType::Chunk) {
				std::string error;
				const AntPackage *p = cached_package(*d, info, error);
				++chunks;
				chunk_ok += (p && p->byte_size() == info.chunk_size) ? 1 : 0;
				if (p && info.short_name == "m9") {
					CHECK(p->archive().layouts().size() == 13);
					CHECK(p->object_count() == 11);
				}
			}
		}
		MESSAGE("banks parsed " << parsed << " (" << sets << " animation sets) + " << chunks << " chunks in "
								<< std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s");
		CHECK(failed == 0);
		CHECK(parsed == 397);
		CHECK(chunks == 88);
		CHECK(chunk_ok == 88);
		CHECK(d->source().packages().size() == 322);
	}

	TEST_CASE("ant: census of the MP-relevant packages equals the probe's") {
		AntData *d = data_or_skip();
		if (!d) {
			return;
		}
		const Censuses &both = censuses(*d);
		REQUIRE_MESSAGE(both.ok, both.error);
		antdump::Census c = both.mp;
		MESSAGE(antdump::format_census(c));
		CHECK(c.packages == 125);
		CHECK(c.packages_failed == 0);
		CHECK(c.codecs["DctAnimationAsset"].clips == 3298);
		CHECK(c.codecs["RawAnimationAsset"].clips == 1490);
		CHECK(c.codecs["FrameAnimationAsset"].clips == 659);
		CHECK(c.codecs["VbrAnimationAsset"].clips == 146);
		CHECK(c.codecs["CurveAnimationAsset"].clips == 35);
		CHECK(c.static_codecs["DctAnimationAsset"] == 228);
		CHECK(c.static_codecs["RawAnimationAsset"] == 86);
		CHECK(c.static_codecs["FrameAnimationAsset"] == 22);
		CHECK(c.static_codecs["VbrAnimationAsset"] == 27);
		CHECK(c.static_codecs["CurveAnimationAsset"] == 5);
		CHECK(c.dct_ok == 3298);
		CHECK(c.dct_overruns == 0);
		CHECK(c.dct_q_off == 6);
		CHECK(c.dct_worst_mean_q == doctest::Approx(0.00745).epsilon(0.01));
		CHECK(c.dct_datasize_residual.size() == 2);
		CHECK(c.dct_datasize_residual[0] == 156);
		CHECK(c.dct_datasize_residual[1] == 3142);
		CHECK(c.dct_bitstream_bytes / 1024 == 22863);
		CHECK(c.raw_exact == 1490);
		CHECK(c.frame_exact == 659);

		CHECK(c.clips_with_controller == 5592);
		CHECK(c.clips_without_controller == 1);
		CHECK(c.clips_kind_consistent == 5592);
		CHECK(c.channels_total == 684459);
		CHECK(c.channels_ok == 684459);
		CHECK(c.channels_on_delta_traj == 0);
		CHECK(c.bind_errors == 0);
		CHECK(c.codecs["DctAnimationAsset"].mapped_perfect == 3297);
		CHECK(c.codecs["RawAnimationAsset"].mapped_perfect == 1490);
		CHECK(c.codecs["FrameAnimationAsset"].mapped_perfect == 659);
		CHECK(c.codecs["VbrAnimationAsset"].mapped_perfect == 146);

		CHECK(c.codecs["VbrAnimationAsset"].unsupported == 146);
		CHECK(c.codecs["CurveAnimationAsset"].unsupported == 35);
		for (const auto &[name, cc] : c.codecs) {
			CAPTURE(name);
			CHECK(cc.errors == 0);
			CHECK(cc.key_count_mismatch == 0);
			CHECK(cc.non_unit_quats == 0);
		}
	}

	TEST_CASE("ant: census of all packages equals the probe's") {
		AntData *d = data_or_skip();
		if (!d) {
			return;
		}
		const Censuses &both = censuses(*d);
		REQUIRE_MESSAGE(both.ok, both.error);
		antdump::Census c = both.all;
		MESSAGE(antdump::format_census(c));
		CHECK(c.packages == 321);
		CHECK(c.packages_failed == 0);
		CHECK(c.codecs["DctAnimationAsset"].clips == 3778);
		CHECK(c.codecs["VbrAnimationAsset"].clips == 2225);
		CHECK(c.codecs["RawAnimationAsset"].clips == 1534);
		CHECK(c.codecs["CurveAnimationAsset"].clips == 775);
		CHECK(c.codecs["FrameAnimationAsset"].clips == 660);
		CHECK(c.dct_ok == 3778);
		CHECK(c.dct_overruns == 0);
		CHECK(c.dct_q_off == 9);
		CHECK(c.dct_datasize_residual[0] == 182);
		CHECK(c.dct_datasize_residual[1] == 3596);
		CHECK(c.dct_bitstream_bytes / 1024 == 37467);
		CHECK(c.raw_exact == 1522);
		CHECK(c.raw_padded == 12);
		CHECK(c.frame_exact == 660);
		CHECK(c.bind_errors == 0);
		CHECK(c.channels_ok == c.channels_total);
		for (const auto &[name, cc] : c.codecs) {
			CAPTURE(name);
			CHECK(cc.errors == 0);
			CHECK(cc.key_count_mismatch == 0);
			CHECK(cc.non_unit_quats == 0);
		}
	}

	TEST_CASE("ant: M9 clips against the uncompressed IdlePose and by sampling") {
		AntData *d = data_or_skip();
		if (!d) {
			return;
		}
		const AntPackageInfo *info = d->source().find("m9");
		REQUIRE(info != nullptr);
		std::string error;
		auto m9 = antdump::load_package(d->source(), *info, error);
		REQUIRE(m9);
		DecodedClip idle, undeploy;
		REQUIRE(decode_clip(AntLibrary::find_clip(*m9, "IdlePose Anim"), idle).ok());
		REQUIRE(decode_clip(AntLibrary::find_clip(*m9, "Undeploy Anim"), undeploy).ok());
		CHECK(undeploy.key_times == std::vector<uint16_t>{ 0, 2, 3, 4, 5, 6, 7, 9, 10 });
		REQUIRE(idle.num_quats == undeploy.num_quats);
		double max_deg = 0.0, max_mm = 0.0;
		for (uint32_t q = 0; q < idle.num_quats; ++q) {
			max_deg = std::max(max_deg, quat_angle_deg(idle.quat(0, q), undeploy.quat(0, q)));
		}
		for (uint32_t v = 0; v < idle.num_vec3s; ++v) {
			const Vec3 &a = idle.vec3(0, v), &b = undeploy.vec3(0, v);
			max_mm = std::max(max_mm, 1000.0 * std::sqrt(double(a.x - b.x) * (a.x - b.x) + double(a.y - b.y) * (a.y - b.y) + double(a.z - b.z) * (a.z - b.z)));
		}
		MESSAGE("Undeploy frame 0 vs IdlePose: max " << max_deg << " deg, " << max_mm << " mm (probe: 0.07 deg, 0.4 mm)");
		CHECK(max_deg < 0.1);
		CHECK(max_mm < 0.5);

		ClipPose pose;
		sample_clip(undeploy, 2.0f, pose);
		CHECK(quat_angle_deg(pose.quats[5], undeploy.quat(1, 5)) < 1e-3);
		sample_clip(undeploy, 1.0f, pose);
		for (uint32_t q = 0; q < undeploy.num_quats; ++q) {
			const Quat mid = quat_slerp(undeploy.quat(0, q), undeploy.quat(1, q), 0.5f);
			CHECK(quat_angle_deg(pose.quats[q], mid) < 1e-3);
		}
		ClipBinding b;
		REQUIRE(d->library.bind(*m9, AntLibrary::find_clip(*m9, "ReloadEmpty Anim"), b).ok());
		CHECK(b.controller_name == "ReloadEmpty");
		CHECK(b.target_name == "1P_Upperbody_DOF");
		REQUIRE(b.rig != nullptr);
		CHECK(b.rig->name == "Venice1pAntSke01");
		CHECK(b.rig->skeleton_name == "Venice1pAntSke01_New");
		CHECK(b.rig->joints.size() == 181);
		CHECK(b.fps == 30.0f);
		CHECK(b.tracks.size() == 135);
		CHECK(b.kind_mismatches == 0);
	}

	TEST_CASE("ant: decoded clips equal the probe's clip JSON within 1e-5") {
		AntData *d = data_or_skip();
		if (!d) {
			return;
		}
		const std::string oracle_dir = env_var("OPENBF3_ANT_ORACLE");
		if (oracle_dir.empty() || !std::filesystem::is_directory(oracle_dir)) {
			optional_input_missing("OPENBF3_ANT_ORACLE does not name the probe's output directory");
			return;
		}
		int compared = 0;
		for (std::string file : { "m9_Fire_Anim.json", "m9_IdlePose_Anim.json", "m9_ReloadEmpty_Anim.json", "b_basicassetsmp_Rifle_RunFwd.json",
					 "b_basicassetsmp_Reload_NoAddon_Stand.json" }) {
			CAPTURE(file);
			std::vector<uint8_t> bytes;
			std::string error;
			REQUIRE(antdump::read_whole_file(std::filesystem::path(oracle_dir) / file, bytes, error));
			const std::string oracle(bytes.begin(), bytes.end());
			std::string package;
			REQUIRE(antdump::oracle_package(oracle, package, error));
			std::unique_ptr<AntPackage> holder;
			const AntPackage *p = d->library.static_package();
			if (package != "s_basicassets") {
				holder = antdump::load_package(d->source(), *d->source().find(package), error);
				p = holder.get();
			}
			REQUIRE(p != nullptr);
			GdStruct clip;
			REQUIRE_MESSAGE(antdump::find_oracle_clip(d->library, *p, oracle, clip, error), error);
			std::string ours;
			REQUIRE(antdump::clip_to_json(d->library, *p, clip, -1, ours, error));
			antdump::CompareResult r;
			REQUIRE(antdump::compare_clip_json(ours, oracle, r, error));
			for (const std::string &diff : r.differences) {
				MESSAGE(diff);
			}
			MESSAGE(file << ": " << r.values << " values, max |diff| " << r.max_track_diff << "; joints max |diff| " << r.max_joint_diff);
			CHECK(r.shape_ok);
			CHECK(r.values > 0);
			CHECK(r.max_track_diff <= 1e-5);
			CHECK(r.max_joint_diff <= 1e-5);
			++compared;
		}
		CHECK(compared == 5);
	}

	TEST_CASE("ant: real banks survive truncation and corruption") {
		AntData *d = data_or_skip();
		if (!d) {
			return;
		}
		std::vector<uint8_t> bytes;
		std::string error;
		REQUIRE(d->source().read(*d->source().find("m9"), bytes, error));
		uint32_t decoded = 0;
		auto exercise = [&](std::vector<uint8_t> p_bytes) {
			AntPackage p;
			if (!p.parse("m9", std::move(p_bytes), error)) {
				return;
			}
			for (uint32_t i : AntLibrary::clips(p)) {
				DecodedClip c;
				decoded += decode_clip(p.object(i), c).ok() ? 1 : 0;
				ClipBinding b;
				d->library.bind(p, p.object(i), b);
			}
		};
		for (size_t n = 0; n < bytes.size(); n += 97) {
			exercise(std::vector<uint8_t>(bytes.begin(), bytes.begin() + std::ptrdiff_t(n)));
		}
		uint32_t state = 12345;
		for (int round = 0; round < 400; ++round) {
			std::vector<uint8_t> bad = bytes;
			for (int k = 0; k < 4; ++k) {
				state = state * 1664525u + 1013904223u;
				bad[state % bad.size()] ^= uint8_t(1u << (state >> 29));
			}
			exercise(std::move(bad));
		}
		MESSAGE("clips still decodable after corruption: " << decoded);
		CHECK(true);
	}

	TEST_CASE("ant: M9 1P ReloadEmpty decodes and binds to the 1P rig") {
		AntData *d = data_or_skip();
		if (!d) {
			return;
		}
		const AntPackageInfo *info = d->source().find("m9");
		REQUIRE(info != nullptr);
		CHECK(info->packaging == AntPackagingType::Chunk);
		CHECK(info->chunk_size == 51948);
		std::string error;
		auto m9 = antdump::load_package(d->source(), *info, error);
		REQUIRE_MESSAGE(m9, error);
		CHECK(m9->byte_size() == 51948);
		const GdStruct clip = AntLibrary::find_clip(*m9, "ReloadEmpty Anim");
		REQUIRE(clip.valid());
		DecodedClip c;
		REQUIRE(decode_clip(clip, c).ok());
		CHECK(c.codec == ClipCodec::Dct);
		CHECK(c.num_keys == 57);
		CHECK(c.end_frame == 65);
		REQUIRE(c.key_times.size() == 57);
		CHECK(c.key_times.front() == 0);
		CHECK(c.key_times.back() == 65);
		CHECK(c.num_quats == 96);
		CHECK(c.num_vec3s == 39);
		CHECK(c.num_floats == 0);
		CHECK_FALSE(c.cycle);
		CHECK_FALSE(c.additive);
		ClipBinding b;
		REQUIRE(d->library.bind(*m9, clip, b).ok());
		CHECK(b.controller_name == "ReloadEmpty");
		CHECK(b.target_name == "1P_Upperbody_DOF");
		CHECK(b.fps == 30.0f);
		CHECK(b.fps_scale == 0.5f);
		CHECK(b.num_ticks == 130.0f);
		REQUIRE(b.rig != nullptr);
		CHECK(b.rig->name == "Venice1pAntSke01");
		CHECK(b.rig->joints.size() == 181);
		REQUIRE(b.tracks.size() == 135);

		std::set<std::pair<int32_t, char>> driven;
		uint32_t on_joint = 0;
		for (const ChannelTrack &t : b.tracks) {
			on_joint += (t.joint >= 0 && t.kind_ok) ? 1 : 0;
			driven.insert({ t.joint, t.kind });
		}
		CHECK(on_joint == 135);
		CHECK(driven.size() == 135);
		CHECK(b.kind_mismatches == 0);
		CHECK(b.delta_traj_channels == 0);

		for (std::string_view joint : { "Wep_Root", "Wep_Slide", "Wep_Mag", "LeftHand", "RightHand", "CameraJoint" }) {
			CAPTURE(joint);
			const int32_t j = b.rig->find_joint(joint);
			CHECK(j >= 0);
			CHECK(driven.contains({ j, 'q' }));
		}

		double max_deg = 0.0;
		for (uint32_t q = 0; q < c.num_quats; ++q) {
			max_deg = std::max(max_deg, quat_angle_deg(c.quat(0, q), c.quat(c.num_keys - 1, q)));
		}
		MESSAGE("ReloadEmpty first vs last key: max " << max_deg << " deg");
		CHECK(max_deg < 1.0);
	}

	TEST_CASE("ant: the install source equals the export directory") {
		const std::string dir = env_var("OPENBF3_ANT_DATA");
		antdump::OpenedSource exported, install;
		antdump::SourceOptions export_options, install_options;
		export_options.export_dir = dir;
		install_options.install = true;
		std::string error;
		bool unavailable = false;
		if (dir.empty() || !antdump::open_source(export_options, exported, error) || !antdump::open_source(install_options, install, error, &unavailable)) {
			optional_input_missing("the install-vs-export case needs both OPENBF3_ANT_DATA and the install");
			return;
		}
		antdump::ParityResult r;
		antdump::compare_sources(*install.source, *exported.source, false, r);
		MESSAGE(antdump::format_parity(r));
		CHECK(r.packages_a == 322);
		CHECK(r.banks_a == 397);
		CHECK(r.ok());

		for (const char *name : { "s_basicassets", "b_basicassetsmp", "m9" }) {
			CAPTURE(name);
			const AntPackageInfo *a = install.source->find(name), *b = exported.source->find(name);
			REQUIRE(a != nullptr);
			REQUIRE(b != nullptr);
			std::vector<uint8_t> ba, bb;
			REQUIRE(install.source->read(*a, ba, error));
			REQUIRE(exported.source->read(*b, bb, error));
			CHECK(ba.size() == bb.size());
			CHECK(ba == bb);
		}
		CHECK(install.source->find("s_basicassets")->superbundle == "Win32/Globals");
	}
}
