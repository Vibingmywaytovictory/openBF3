#pragma once

#include "bf3/core/ant/gd_archive.h"
#include "bf3/core/math.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::ant {

enum class AntStatus {
	Ok,
	Malformed,
	Unsupported,
	NotFound,
};

struct AntResult {
	AntStatus status = AntStatus::Ok;
	std::string message;

	bool ok() const { return status == AntStatus::Ok; }
	static AntResult fail(AntStatus p_status, std::string p_message) { return AntResult{ p_status, std::move(p_message) }; }
};

enum class ClipCodec {
	Unknown,
	Dct,
	Raw,
	Frame,
	Vbr,
	Curve,
};

constexpr uint32_t kMaxFrame = 0xFFFF;

const char *codec_name(ClipCodec p_codec);

ClipCodec clip_codec(const GdStruct &p_clip);

bool is_clip(const GdStruct &p_clip);

struct Quat {
	float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};
using Vec3 = core::Vec3;

bool clip_channel_counts(const GdStruct &p_clip, uint32_t &r_quats, uint32_t &r_vec3s, uint32_t &r_floats);

struct ClipHeader {
	ClipCodec codec = ClipCodec::Unknown;
	std::string_view name;
	uint32_t codec_type = 0;
	uint32_t anim_id = 0;
	float trim_offset = 0.0f;
	uint32_t end_frame = 0;
	bool additive = false;
	GdGuid channel_to_dof;
};
bool read_clip_header(const GdStruct &p_clip, ClipHeader &r_header);

struct DecodedClip {
	ClipCodec codec = ClipCodec::Unknown;
	uint32_t num_keys = 0;
	uint32_t num_quats = 0;
	uint32_t num_vec3s = 0;
	uint32_t num_floats = 0;
	uint32_t end_frame = 0;
	bool cycle = false;
	bool additive = false;
	std::vector<uint16_t> key_times;
	std::vector<Quat> quats;
	std::vector<Vec3> vec3s;
	std::vector<float> floats;

	uint64_t channel_count() const { return uint64_t(num_quats) + num_vec3s + num_floats; }

	bool consistent() const;
	const Quat &quat(uint32_t p_key, uint32_t p_channel) const { return quats[size_t(p_key) * num_quats + p_channel]; }
	const Vec3 &vec3(uint32_t p_key, uint32_t p_channel) const { return vec3s[size_t(p_key) * num_vec3s + p_channel]; }
	float value(uint32_t p_key, uint32_t p_channel) const { return floats[size_t(p_key) * num_floats + p_channel]; }
};

struct DecodeStats {

	uint64_t bits_read = 0;
	uint64_t bits_available = 0;
	int64_t data_size_residual = 0;
	uint64_t bitstream_bytes = 0;

	double mean_abs_quat_error = 0.0;
	double max_abs_quat_error = 0.0;

	bool vec3_padded = true;
	bool float_block_padded = false;
};

AntResult decode_clip(const GdStruct &p_clip, DecodedClip &r_out, DecodeStats *r_stats = nullptr);

inline constexpr std::array<std::array<uint8_t, 3>, 6> kEulerOrders{ { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 },
		{ 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } } };
inline constexpr uint32_t kCurveEulerOrder = 5;

Quat euler_quat(const float p_angles[3], uint32_t p_order);

struct DctInput {
	uint32_t num_keys = 0;
	uint32_t num_quats = 0;
	uint32_t num_vec3s = 0;
	uint32_t num_floats = 0;
	uint32_t num_float_vecs = 0;
	uint32_t quantize_mult_block = 0;
	uint32_t quantize_mult_subblock = 0;
	uint32_t catch_all_bit_count = 0;
	std::span<const uint16_t> key_times;
	std::span<const uint8_t> dof_table_desc;
	std::span<const int16_t> delta_base[4];
	std::span<const uint16_t> bits_per_subblock;
	std::span<const uint8_t> data;
};
AntResult decode_dct(const DctInput &p_in, DecodedClip &r_out, DecodeStats *r_stats = nullptr);

struct ClipPose {
	std::vector<Quat> quats;
	std::vector<Vec3> vec3s;
	std::vector<float> floats;
};

bool sample_clip(const DecodedClip &p_clip, float p_frame, ClipPose &r_pose);

inline void sample_clip_seconds(const DecodedClip &p_clip, float p_seconds, float p_frames_per_second, ClipPose &r_pose) {
	sample_clip(p_clip, p_seconds * p_frames_per_second, r_pose);
}

AntResult bake_frames(const DecodedClip &p_clip, DecodedClip &r_out);

Quat quat_slerp(const Quat &p_a, const Quat &p_b, float p_t);

}
