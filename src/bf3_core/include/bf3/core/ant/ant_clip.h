#pragma once
// Ant clip codecs: decode an AnimationAsset object into a dense per-key table of local rotations
// (unit quaternions), vec3s (translation or scale, depending on the DOF the channel maps to) and floats,
// and sample it at any frame.
//
// Clean-room implementation from research/probes/ant-animation-probe.md (§3.3, §4, §6). No code from Rime,
// IceBloc or AssetBankPlugin was used.
//
// Channel order in every codec: all quaternion channels, then all vec3 channels, then all float channels.
// Codecs: DctAnimationAsset (bit-exact, §4.1), RawAnimationAsset and FrameAnimationAsset (raw floats).
// VbrAnimationAsset and CurveAnimationAsset return AntStatus::Unsupported (§6).

#include "bf3/core/ant/gd_archive.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::ant {

enum class AntStatus {
	Ok,
	Malformed, // the data contradicts the format (bounds, counts, overrun)
	Unsupported, // a valid clip of a codec we do not decode (VBR, Curve)
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

// Largest frame number a clip may use: key times are 16-bit, so EndFrame must fit too (decode_clip
// rejects larger EndFrame values as Malformed).
constexpr uint32_t kMaxFrame = 0xFFFF;

const char *codec_name(ClipCodec p_codec); // layout name, e.g. "DctAnimationAsset"
// Codec of an AnimationAsset object, by its layout name ("Unknown" for anything else).
ClipCodec clip_codec(const GdStruct &p_clip);
// True for any object deriving from AnimationAsset.
bool is_clip(const GdStruct &p_clip);

struct Quat {
	float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};
struct Vec3 {
	float x = 0.0f, y = 0.0f, z = 0.0f;
};

// Channel counts of a clip (quaternion, vec3, float channels) as its codec header states them.
// VBR: animated + constant channels. Fails for Curve and unknown codecs.
bool clip_channel_counts(const GdStruct &p_clip, uint32_t &r_quats, uint32_t &r_vec3s, uint32_t &r_floats);

// AnimationAsset base fields.
struct ClipHeader {
	ClipCodec codec = ClipCodec::Unknown;
	std::string_view name;
	uint32_t codec_type = 0; // FourCC, e.g. 'DCT '
	uint32_t anim_id = 0;
	float trim_offset = 0.0f;
	uint32_t end_frame = 0;
	bool additive = false;
	GdGuid channel_to_dof;
};
bool read_clip_header(const GdStruct &p_clip, ClipHeader &r_header);

// A decoded clip: key k is at frame key_times[k]; arrays are key-major (value of channel c at key k is
// quats[k * num_quats + c]).
struct DecodedClip {
	ClipCodec codec = ClipCodec::Unknown;
	uint32_t num_keys = 0;
	uint32_t num_quats = 0;
	uint32_t num_vec3s = 0;
	uint32_t num_floats = 0;
	uint32_t end_frame = 0;
	bool cycle = false;
	bool additive = false;
	std::vector<uint16_t> key_times; // frame numbers, ascending, may be sparse
	std::vector<Quat> quats; // normalised
	std::vector<Vec3> vec3s;
	std::vector<float> floats;

	uint64_t channel_count() const { return uint64_t(num_quats) + num_vec3s + num_floats; }
	// Array sizes agree with num_keys and the channel counts (always true for decode_clip output).
	bool consistent() const;
	const Quat &quat(uint32_t p_key, uint32_t p_channel) const { return quats[size_t(p_key) * num_quats + p_channel]; }
	const Vec3 &vec3(uint32_t p_key, uint32_t p_channel) const { return vec3s[size_t(p_key) * num_vec3s + p_channel]; }
	float value(uint32_t p_key, uint32_t p_channel) const { return floats[size_t(p_key) * num_floats + p_channel]; }
};

// Diagnostics gathered while decoding (used by the census and the tests).
struct DecodeStats {
	// DCT
	uint64_t bits_read = 0; // bits the tables predict and the decoder consumed
	uint64_t bits_available = 0; // 8 * Data.size()
	int64_t data_size_residual = 0; // DataSize - (12 + n + 8n + 2*subblocks + len(Data))
	uint64_t bitstream_bytes = 0;
	// all codecs: |q| before normalisation
	double mean_abs_quat_error = 0.0; // mean | |q| - 1 |
	double max_abs_quat_error = 0.0;
	// Raw/Frame
	bool vec3_padded = true; // vec3 stored as 4 floats
	bool float_block_padded = false; // float block of a key padded to a multiple of 4
};

// Decodes an AnimationAsset object (any supported codec).
AntResult decode_clip(const GdStruct &p_clip, DecodedClip &r_out, DecodeStats *r_stats = nullptr);

// The DCT codec on plain arrays (§4.1), independent of the GD object model.
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
	std::span<const int16_t> delta_base[4]; // X, Y, Z, W
	std::span<const uint16_t> bits_per_subblock;
	std::span<const uint8_t> data;
};
AntResult decode_dct(const DctInput &p_in, DecodedClip &r_out, DecodeStats *r_stats = nullptr);

// A pose: one value per channel (same counts as the clip).
struct ClipPose {
	std::vector<Quat> quats;
	std::vector<Vec3> vec3s;
	std::vector<float> floats;
};

// Samples at a frame (fractional frames interpolate between the surrounding keys: slerp for quaternions,
// lerp otherwise). Frames before the first / after the last key clamp. Returns false (and an identity
// pose) when the clip's arrays are inconsistent (see DecodedClip::consistent()).
bool sample_clip(const DecodedClip &p_clip, float p_frame, ClipPose &r_pose);
// Samples at a time in seconds; p_frames_per_second is the controller's FPS (30 for the MP clips).
inline void sample_clip_seconds(const DecodedClip &p_clip, float p_seconds, float p_frames_per_second, ClipPose &r_pose) {
	sample_clip(p_clip, p_seconds * p_frames_per_second, r_pose);
}
// Resamples to one key per integer frame 0..max(end_frame, last key time) (a dense frame table).
// Fails (Malformed) for an inconsistent or keyless clip, unsorted key times, EndFrame > kMaxFrame, or a
// table larger than the decode bound; r_out is then empty.
AntResult bake_frames(const DecodedClip &p_clip, DecodedClip &r_out);

Quat quat_slerp(const Quat &p_a, const Quat &p_b, float p_t);

} // namespace bf3::core::ant
