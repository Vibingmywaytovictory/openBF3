// Ant clip codecs. Clean-room implementation from research/probes/ant-animation-probe.md (§3.3, §4, §6);
// see ant_clip.h.
#include "bf3/core/ant/ant_clip.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bf3::core::ant {

namespace {

constexpr uint32_t kDctBlock = 8;
constexpr uint32_t kMaxKeys = 1u << 20; // sanity bound for allocations driven by data
// Bound on keys * channels of one decoded or baked clip (16 Mi channel-keys, <= 256 MiB of quaternions).
// The largest retail clip is far below it; it stops a small crafted bank from requesting gigabytes.
constexpr uint64_t kMaxChannelKeys = uint64_t(1) << 24;

// MSB-first bit reader over a byte array (no word swapping, §4.1). Widths 0..32.
class BitReader {
public:
	explicit BitReader(std::span<const uint8_t> p_data) : data_(p_data), size_bits_(uint64_t(p_data.size()) * 8) {}

	// Reads a two's-complement value of p_width bits. Width 0 reads nothing and yields 0.
	int32_t read_signed(uint32_t p_width) {
		if (p_width == 0) {
			return 0;
		}
		if (p_width > 32 || pos_ + p_width > size_bits_) {
			overrun_ = true;
			pos_ += p_width;
			return 0;
		}
		const uint64_t byte = pos_ >> 3;
		uint64_t window = 0;
		for (uint64_t i = 0; i < 5; ++i) {
			window = (window << 8) | (byte + i < data_.size() ? data_[size_t(byte + i)] : 0u);
		}
		// window holds 40 bits starting at bit (byte * 8); drop the (pos & 7) bits already consumed.
		const uint32_t skip = uint32_t(pos_ & 7);
		const uint64_t raw = (window >> (40 - skip - p_width)) & ((uint64_t(1) << p_width) - 1);
		pos_ += p_width;
		const uint64_t sign = uint64_t(1) << (p_width - 1);
		return int32_t(int64_t(raw ^ sign) - int64_t(sign));
	}
	uint64_t position() const { return pos_; }
	bool overrun() const { return overrun_; }

private:
	std::span<const uint8_t> data_;
	uint64_t size_bits_ = 0;
	uint64_t pos_ = 0;
	bool overrun_ = false;
};

// The 8-point basis of §4.1: c_i * cos((2n+1) * i * pi / 16), c_0 = 1/4, c_i = 1/2.
struct DctBasis {
	double m[kDctBlock][kDctBlock];
	DctBasis() {
		for (uint32_t n = 0; n < kDctBlock; ++n) {
			for (uint32_t i = 0; i < kDctBlock; ++i) {
				const double c = i == 0 ? 0.25 : 0.5;
				m[n][i] = c * std::cos(double(2 * n + 1) * double(i) * std::numbers::pi / 16.0);
			}
		}
	}
};
const DctBasis &dct_basis() {
	static const DctBasis basis;
	return basis;
}

void normalise(Quat &r_q, double &r_error_sum, double &r_error_max) {
	const double len = std::sqrt(double(r_q.x) * r_q.x + double(r_q.y) * r_q.y + double(r_q.z) * r_q.z + double(r_q.w) * r_q.w);
	const double err = std::fabs(len - 1.0);
	r_error_sum += err;
	r_error_max = std::max(r_error_max, err);
	if (len > 1e-12) {
		const double inv = 1.0 / len;
		r_q.x = float(r_q.x * inv);
		r_q.y = float(r_q.y * inv);
		r_q.z = float(r_q.z * inv);
		r_q.w = float(r_q.w * inv);
	} else {
		r_q = Quat{};
	}
}

bool read_count(const GdStruct &p_clip, std::string_view p_name, uint32_t &r_value) {
	return p_clip.read_u32(p_name, r_value);
}

} // namespace

const char *codec_name(ClipCodec p_codec) {
	switch (p_codec) {
		case ClipCodec::Dct:
			return "DctAnimationAsset";
		case ClipCodec::Raw:
			return "RawAnimationAsset";
		case ClipCodec::Frame:
			return "FrameAnimationAsset";
		case ClipCodec::Vbr:
			return "VbrAnimationAsset";
		case ClipCodec::Curve:
			return "CurveAnimationAsset";
		case ClipCodec::Unknown:
			break;
	}
	return "Unknown";
}

ClipCodec clip_codec(const GdStruct &p_clip) {
	const std::string_view t = p_clip.type_name();
	if (t == "DctAnimationAsset") {
		return ClipCodec::Dct;
	}
	if (t == "RawAnimationAsset") {
		return ClipCodec::Raw;
	}
	if (t == "FrameAnimationAsset") {
		return ClipCodec::Frame;
	}
	if (t == "VbrAnimationAsset") {
		return ClipCodec::Vbr;
	}
	if (t == "CurveAnimationAsset") {
		return ClipCodec::Curve;
	}
	return ClipCodec::Unknown;
}

bool is_clip(const GdStruct &p_clip) {
	return p_clip.valid() && p_clip.type_name() != "AnimationAsset" && gd_is_a(p_clip, "AnimationAsset");
}

bool read_clip_header(const GdStruct &p_clip, ClipHeader &r_header) {
	r_header = {};
	r_header.codec = clip_codec(p_clip);
	r_header.name = p_clip.name();
	bool ok = p_clip.read_u32("CodecType", r_header.codec_type);
	ok = p_clip.read_u32("AnimId", r_header.anim_id) && ok;
	ok = p_clip.read_float("TrimOffset", r_header.trim_offset) && ok;
	ok = p_clip.read_u32("EndFrame", r_header.end_frame) && ok;
	ok = p_clip.read_bool("Additive", r_header.additive) && ok;
	ok = p_clip.read_guid("ChannelToDofAsset", r_header.channel_to_dof) && ok;
	return ok;
}

bool clip_channel_counts(const GdStruct &p_clip, uint32_t &r_quats, uint32_t &r_vec3s, uint32_t &r_floats) {
	switch (clip_codec(p_clip)) {
		case ClipCodec::Dct:
			return read_count(p_clip, "NumQuats", r_quats) && read_count(p_clip, "NumVec3", r_vec3s) && read_count(p_clip, "NumFloat", r_floats);
		case ClipCodec::Raw:
		case ClipCodec::Frame:
			return read_count(p_clip, "QuatCount", r_quats) && read_count(p_clip, "Vec3Count", r_vec3s) && read_count(p_clip, "FloatCount", r_floats);
		case ClipCodec::Vbr: {
			uint32_t q, cq, v, cv, f, cf;
			if (!read_count(p_clip, "QuaternionCount", q) || !read_count(p_clip, "ConstQuaternionCount", cq) || !read_count(p_clip, "Vector3Count", v) ||
					!read_count(p_clip, "ConstVector3Count", cv) || !read_count(p_clip, "FloatCount", f) || !read_count(p_clip, "ConstFloatCount", cf)) {
				return false;
			}
			r_quats = q + cq;
			r_vec3s = v + cv;
			r_floats = f + cf;
			return true;
		}
		default:
			return false;
	}
}

// ---------------------------------------------------------------------------------------------
// DCT (§4.1)

AntResult decode_dct(const DctInput &p_in, DecodedClip &r_out, DecodeStats *r_stats) {
	const uint32_t nq = p_in.num_quats;
	const uint32_t nv = p_in.num_vec3s;
	const uint32_t nf = p_in.num_floats;
	const uint64_t dofs = uint64_t(nq) + nv + p_in.num_float_vecs;
	const uint32_t keys = p_in.num_keys;
	if (keys == 0 || keys > kMaxKeys) {
		return AntResult::fail(AntStatus::Malformed, "DCT: NumKeys " + std::to_string(keys));
	}
	if (p_in.key_times.size() != keys) {
		return AntResult::fail(AntStatus::Malformed, "DCT: KeyTimes size != NumKeys");
	}
	for (uint32_t k = 1; k < keys; ++k) {
		if (p_in.key_times[k] < p_in.key_times[k - 1]) {
			return AntResult::fail(AntStatus::Malformed, "DCT: KeyTimes not ascending");
		}
	}
	if (uint64_t(keys) * (uint64_t(nq) + nv + nf) > kMaxChannelKeys) {
		return AntResult::fail(AntStatus::Malformed, "DCT: NumKeys * channels exceeds the decode bound");
	}
	if (uint64_t(nf) > 4ull * p_in.num_float_vecs) {
		return AntResult::fail(AntStatus::Malformed, "DCT: NumFloat exceeds 4 * NumFloatVec");
	}
	if (p_in.dof_table_desc.size() != dofs) {
		return AntResult::fail(AntStatus::Malformed, "DCT: DofTableDescBytes size != dof count");
	}
	for (const auto &b : p_in.delta_base) {
		if (b.size() != dofs) {
			return AntResult::fail(AntStatus::Malformed, "DCT: DeltaBase size != dof count");
		}
	}
	if (p_in.quantize_mult_block == 0) {
		return AntResult::fail(AntStatus::Malformed, "DCT: QuantizeMultBlock is 0");
	}
	if (p_in.catch_all_bit_count > 32) {
		return AntResult::fail(AntStatus::Malformed, "DCT: CatchAllBitCount > 32");
	}
	// Per-DOF sub-block count (high nibble) and its first entry in BitsPerSubblock.
	std::vector<uint32_t> first_width(size_t(dofs) + 1, 0);
	for (uint64_t d = 0; d < dofs; ++d) {
		const uint32_t sub = p_in.dof_table_desc[size_t(d)] >> 4;
		if (sub > kDctBlock) {
			return AntResult::fail(AntStatus::Malformed, "DCT: more than 8 sub-blocks");
		}
		first_width[size_t(d) + 1] = first_width[size_t(d)] + sub;
	}
	const uint32_t total_sub = first_width[size_t(dofs)];
	if (p_in.bits_per_subblock.size() != total_sub) {
		return AntResult::fail(AntStatus::Malformed, "DCT: BitsPerSubblock size != sum of sub-block counts");
	}

	r_out = {};
	r_out.codec = ClipCodec::Dct;
	r_out.num_keys = keys;
	r_out.num_quats = nq;
	r_out.num_vec3s = nv;
	r_out.num_floats = nf;
	r_out.key_times.assign(p_in.key_times.begin(), p_in.key_times.end());
	r_out.quats.resize(size_t(keys) * nq);
	r_out.vec3s.resize(size_t(keys) * nv);
	r_out.floats.resize(size_t(keys) * nf);

	// m[n][i] = basis * (1 + 0.1 * QuantizeMultSubblock * i) / QuantizeMultBlock
	double m[kDctBlock][kDctBlock];
	const DctBasis &basis = dct_basis();
	for (uint32_t i = 0; i < kDctBlock; ++i) {
		const double scale = (1.0 + 0.1 * double(p_in.quantize_mult_subblock) * double(i)) / double(p_in.quantize_mult_block);
		for (uint32_t n = 0; n < kDctBlock; ++n) {
			m[n][i] = basis.m[n][i] * scale;
		}
	}

	BitReader bits(p_in.data);
	const uint32_t blocks = (keys + kDctBlock - 1) / kDctBlock;
	for (uint32_t b = 0; b < blocks; ++b) {
		const uint32_t first_key = b * kDctBlock;
		const uint32_t block_keys = std::min(kDctBlock, keys - first_key);
		for (uint64_t d = 0; d < dofs; ++d) {
			const uint32_t sub = p_in.dof_table_desc[size_t(d)] >> 4;
			// 64-bit: a CatchAllBitCount-wide (up to 32-bit) coefficient plus DeltaBase must not overflow.
			int64_t c[kDctBlock][4] = {};
			// Block 0 does not store the DC coefficient (j = 0).
			for (uint32_t j = (b == 0 ? 1u : 0u); j < sub; ++j) {
				const uint16_t widths = p_in.bits_per_subblock[first_width[size_t(d)] + j];
				for (uint32_t comp = 0; comp < 4; ++comp) {
					uint32_t w = (widths >> (12 - 4 * comp)) & 0xF;
					if (w == 15) {
						w = p_in.catch_all_bit_count;
					}
					c[j][comp] = bits.read_signed(w);
				}
			}
			// The DC term is relative to the constant per-DOF base, in every block.
			for (uint32_t comp = 0; comp < 4; ++comp) {
				c[0][comp] += p_in.delta_base[comp][size_t(d)];
			}
			const uint32_t terms = std::max(sub, 1u);
			for (uint32_t n = 0; n < block_keys; ++n) {
				double v[4] = {};
				for (uint32_t i = 0; i < terms; ++i) {
					for (uint32_t comp = 0; comp < 4; ++comp) {
						v[comp] += double(c[i][comp]) * m[n][i];
					}
				}
				const size_t k = first_key + n;
				if (d < nq) {
					r_out.quats[k * nq + size_t(d)] = Quat{ float(v[0]), float(v[1]), float(v[2]), float(v[3]) };
				} else if (d < uint64_t(nq) + nv) {
					r_out.vec3s[k * nv + size_t(d - nq)] = Vec3{ float(v[0]), float(v[1]), float(v[2]) };
				} else {
					const uint64_t f0 = 4 * (d - nq - nv);
					for (uint32_t comp = 0; comp < 4 && f0 + comp < nf; ++comp) {
						r_out.floats[k * nf + size_t(f0 + comp)] = float(v[comp]);
					}
				}
			}
		}
	}
	if (bits.overrun()) {
		return AntResult::fail(AntStatus::Malformed, "DCT: bitstream overrun (" + std::to_string(bits.position()) + " bits needed, " +
				std::to_string(p_in.data.size() * 8) + " available)");
	}
	double err_sum = 0.0, err_max = 0.0;
	for (Quat &q : r_out.quats) {
		normalise(q, err_sum, err_max);
	}
	if (r_stats) {
		r_stats->bits_read = bits.position();
		r_stats->bits_available = uint64_t(p_in.data.size()) * 8;
		r_stats->bitstream_bytes = p_in.data.size();
		r_stats->data_size_residual = -int64_t(12 + dofs + 8 * dofs + 2ull * total_sub + p_in.data.size());
		r_stats->mean_abs_quat_error = r_out.quats.empty() ? 0.0 : err_sum / double(r_out.quats.size());
		r_stats->max_abs_quat_error = err_max;
	}
	return {};
}

namespace {

AntResult decode_dct_object(const GdStruct &p_clip, DecodedClip &r_out, DecodeStats *r_stats) {
	DctInput in;
	uint32_t data_size = 0;
	if (!read_count(p_clip, "NumKeys", in.num_keys) || !read_count(p_clip, "NumQuats", in.num_quats) || !read_count(p_clip, "NumVec3", in.num_vec3s) ||
			!read_count(p_clip, "NumFloat", in.num_floats) || !read_count(p_clip, "NumFloatVec", in.num_float_vecs) ||
			!read_count(p_clip, "QuantizeMultBlock", in.quantize_mult_block) || !read_count(p_clip, "QuantizeMultSubblock", in.quantize_mult_subblock) ||
			!read_count(p_clip, "CatchAllBitCount", in.catch_all_bit_count) || !read_count(p_clip, "DataSize", data_size)) {
		return AntResult::fail(AntStatus::Malformed, "DCT: missing header field");
	}
	std::vector<uint16_t> key_times, widths;
	std::vector<uint8_t> desc, data;
	std::vector<int16_t> delta[4];
	static constexpr const char *kDelta[4] = { "DeltaBaseX", "DeltaBaseY", "DeltaBaseZ", "DeltaBaseW" };
	if (!p_clip.array("KeyTimes").copy_u16(key_times) || !p_clip.array("DofTableDescBytes").copy_u8(desc) ||
			!p_clip.array("BitsPerSubblock").copy_u16(widths) || !p_clip.array("Data").copy_u8(data)) {
		return AntResult::fail(AntStatus::Malformed, "DCT: bad array field");
	}
	for (int i = 0; i < 4; ++i) {
		if (!p_clip.array(kDelta[i]).copy_i16(delta[i])) {
			return AntResult::fail(AntStatus::Malformed, "DCT: bad DeltaBase array");
		}
		in.delta_base[i] = delta[i];
	}
	in.key_times = key_times;
	in.dof_table_desc = desc;
	in.bits_per_subblock = widths;
	in.data = data;
	AntResult r = decode_dct(in, r_out, r_stats);
	if (r.ok()) {
		bool cycle = false;
		p_clip.read_bool("Cycle", cycle);
		r_out.cycle = cycle;
		if (r_stats) {
			r_stats->data_size_residual += int64_t(data_size);
		}
	}
	return r;
}

// Raw and Frame: per key 4 floats per quaternion, 4 per vec3 (w unused), then the floats (§3.3). Some
// non-MP Raw clips pad each key's float block to a multiple of 4.
AntResult decode_float_object(const GdStruct &p_clip, ClipCodec p_codec, DecodedClip &r_out, DecodeStats *r_stats) {
	uint32_t nq = 0, nv = 0, nf = 0, keys = 1;
	if (!read_count(p_clip, "QuatCount", nq) || !read_count(p_clip, "Vec3Count", nv) || !read_count(p_clip, "FloatCount", nf)) {
		return AntResult::fail(AntStatus::Malformed, "missing channel counts");
	}
	std::vector<int16_t> key_times;
	if (p_codec == ClipCodec::Raw) {
		if (!read_count(p_clip, "NumKeys", keys) || !p_clip.array("KeyTimes").copy_i16(key_times)) {
			return AntResult::fail(AntStatus::Malformed, "Raw: missing NumKeys/KeyTimes");
		}
		if (key_times.size() != keys) {
			return AntResult::fail(AntStatus::Malformed, "Raw: KeyTimes size != NumKeys");
		}
	} else {
		key_times.assign(1, 0);
	}
	if (keys == 0 || keys > kMaxKeys || nq > kMaxKeys || nv > kMaxKeys || nf > kMaxKeys ||
			uint64_t(keys) * (uint64_t(nq) + nv + nf) > kMaxChannelKeys) {
		return AntResult::fail(AntStatus::Malformed, "bad key or channel count");
	}
	const GdArray data = p_clip.array("Data");
	if (!data.valid() || data.element_hash() != uint32_t(GdPrimitive::Float) || data.stride() != 4) {
		return AntResult::fail(AntStatus::Malformed, "bad Data array");
	}
	const uint64_t stride_exact = 4ull * nq + 4ull * nv + nf;
	const uint64_t stride_padded = 4ull * nq + 4ull * nv + (uint64_t(nf) + 3) / 4 * 4;
	uint64_t stride = 0;
	if (data.size() == stride_exact * keys) {
		stride = stride_exact;
	} else if (data.size() == stride_padded * keys) {
		stride = stride_padded;
	} else {
		return AntResult::fail(AntStatus::Malformed, "Data size " + std::to_string(data.size()) + " != keys * (4Q + 4V + F)");
	}
	std::vector<float> f;
	data.copy_f32(f);
	r_out = {};
	r_out.codec = p_codec;
	r_out.num_keys = keys;
	r_out.num_quats = nq;
	r_out.num_vec3s = nv;
	r_out.num_floats = nf;
	r_out.key_times.resize(keys);
	for (uint32_t k = 0; k < keys; ++k) {
		if (key_times[k] < 0 || (k > 0 && key_times[k] < key_times[k - 1])) {
			return AntResult::fail(AntStatus::Malformed, "Raw: KeyTimes not ascending");
		}
		r_out.key_times[k] = uint16_t(key_times[k]);
	}
	r_out.quats.resize(size_t(keys) * nq);
	r_out.vec3s.resize(size_t(keys) * nv);
	r_out.floats.resize(size_t(keys) * nf);
	double err_sum = 0.0, err_max = 0.0;
	for (uint32_t k = 0; k < keys; ++k) {
		const float *src = f.data() + size_t(k) * stride;
		for (uint32_t q = 0; q < nq; ++q, src += 4) {
			Quat &dst = r_out.quats[size_t(k) * nq + q];
			dst = Quat{ src[0], src[1], src[2], src[3] };
			normalise(dst, err_sum, err_max);
		}
		for (uint32_t v = 0; v < nv; ++v, src += 4) {
			r_out.vec3s[size_t(k) * nv + v] = Vec3{ src[0], src[1], src[2] };
		}
		for (uint32_t i = 0; i < nf; ++i) {
			r_out.floats[size_t(k) * nf + i] = src[i];
		}
	}
	bool cycle = false;
	p_clip.read_bool("Cycle", cycle);
	r_out.cycle = cycle;
	if (r_stats) {
		r_stats->vec3_padded = true;
		r_stats->float_block_padded = stride != stride_exact;
		r_stats->mean_abs_quat_error = r_out.quats.empty() ? 0.0 : err_sum / double(r_out.quats.size());
		r_stats->max_abs_quat_error = err_max;
	}
	return {};
}

} // namespace

AntResult decode_clip(const GdStruct &p_clip, DecodedClip &r_out, DecodeStats *r_stats) {
	if (r_stats) {
		*r_stats = {};
	}
	ClipHeader header;
	if (!p_clip.valid() || !read_clip_header(p_clip, header)) {
		return AntResult::fail(AntStatus::Malformed, "not an AnimationAsset (missing base fields)");
	}
	if (header.end_frame > kMaxFrame) {
		return AntResult::fail(AntStatus::Malformed, "EndFrame " + std::to_string(header.end_frame) + " exceeds the 16-bit frame range");
	}
	AntResult r;
	switch (header.codec) {
		case ClipCodec::Dct:
			r = decode_dct_object(p_clip, r_out, r_stats);
			break;
		case ClipCodec::Raw:
		case ClipCodec::Frame:
			r = decode_float_object(p_clip, header.codec, r_out, r_stats);
			break;
		case ClipCodec::Vbr:
			return AntResult::fail(AntStatus::Unsupported, "VbrAnimationAsset: animated VBR channels are not decoded (probe §6.1)");
		case ClipCodec::Curve:
			return AntResult::fail(AntStatus::Unsupported, "CurveAnimationAsset is not decoded (probe §6.2)");
		case ClipCodec::Unknown:
			return AntResult::fail(AntStatus::Unsupported, "unknown codec " + std::string(p_clip.type_name()));
	}
	if (r.ok()) {
		if (!r_out.key_times.empty() && header.end_frame < r_out.key_times.back()) {
			// Keys past EndFrame would make the dense frame table shorter than the keyed range.
			return AntResult::fail(AntStatus::Malformed, "EndFrame " + std::to_string(header.end_frame) + " before the last key time");
		}
		r_out.end_frame = header.end_frame;
		r_out.additive = header.additive;
	}
	return r;
}

// ---------------------------------------------------------------------------------------------
// Sampling

Quat quat_slerp(const Quat &p_a, const Quat &p_b, float p_t) {
	double ax = p_a.x, ay = p_a.y, az = p_a.z, aw = p_a.w;
	double bx = p_b.x, by = p_b.y, bz = p_b.z, bw = p_b.w;
	double dot = ax * bx + ay * by + az * bz + aw * bw;
	if (dot < 0.0) { // shortest arc
		bx = -bx;
		by = -by;
		bz = -bz;
		bw = -bw;
		dot = -dot;
	}
	double wa, wb;
	if (dot > 0.9995) { // nearly parallel: normalised lerp
		wa = 1.0 - p_t;
		wb = p_t;
	} else {
		const double theta = std::acos(std::min(dot, 1.0));
		const double s = std::sin(theta);
		wa = std::sin((1.0 - p_t) * theta) / s;
		wb = std::sin(p_t * theta) / s;
	}
	double x = wa * ax + wb * bx, y = wa * ay + wb * by, z = wa * az + wb * bz, w = wa * aw + wb * bw;
	const double len = std::sqrt(x * x + y * y + z * z + w * w);
	if (len < 1e-12) {
		return p_a;
	}
	return Quat{ float(x / len), float(y / len), float(z / len), float(w / len) };
}

bool DecodedClip::consistent() const {
	const uint64_t k = num_keys;
	return key_times.size() == k && quats.size() == k * num_quats && vec3s.size() == k * num_vec3s && floats.size() == k * num_floats;
}

bool sample_clip(const DecodedClip &p_clip, float p_frame, ClipPose &r_pose) {
	r_pose.quats.resize(p_clip.num_quats);
	r_pose.vec3s.resize(p_clip.num_vec3s);
	r_pose.floats.resize(p_clip.num_floats);
	const bool consistent = p_clip.consistent();
	if (p_clip.num_keys == 0 || !consistent) {
		std::fill(r_pose.quats.begin(), r_pose.quats.end(), Quat{});
		std::fill(r_pose.vec3s.begin(), r_pose.vec3s.end(), Vec3{});
		std::fill(r_pose.floats.begin(), r_pose.floats.end(), 0.0f);
		return consistent;
	}
	// Surrounding keys by key time (frames).
	uint32_t k0 = 0, k1 = 0;
	float t = 0.0f;
	const auto &kt = p_clip.key_times;
	if (p_clip.num_keys == 1 || !(p_frame > float(kt.front()))) {
		k0 = k1 = 0;
	} else if (p_frame >= float(kt.back())) {
		k0 = k1 = p_clip.num_keys - 1;
	} else {
		const auto it = std::upper_bound(kt.begin(), kt.end(), p_frame, [](float p_f, uint16_t p_k) { return p_f < float(p_k); });
		k1 = uint32_t(it - kt.begin());
		k0 = k1 - 1;
		const float span = float(kt[k1]) - float(kt[k0]);
		t = span > 0.0f ? (p_frame - float(kt[k0])) / span : 0.0f;
	}
	for (uint32_t c = 0; c < p_clip.num_quats; ++c) {
		r_pose.quats[c] = k0 == k1 ? p_clip.quat(k0, c) : quat_slerp(p_clip.quat(k0, c), p_clip.quat(k1, c), t);
	}
	for (uint32_t c = 0; c < p_clip.num_vec3s; ++c) {
		const Vec3 &a = p_clip.vec3(k0, c);
		const Vec3 &b = p_clip.vec3(k1, c);
		r_pose.vec3s[c] = Vec3{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
	}
	for (uint32_t c = 0; c < p_clip.num_floats; ++c) {
		const float a = p_clip.value(k0, c);
		r_pose.floats[c] = a + (p_clip.value(k1, c) - a) * t;
	}
	return true;
}

AntResult bake_frames(const DecodedClip &p_clip, DecodedClip &r_out) {
	r_out = {};
	// The clip may come from anywhere (not only decode_clip): validate before allocating or indexing.
	if (!p_clip.consistent() || p_clip.num_keys == 0) {
		return AntResult::fail(AntStatus::Malformed, "bake: clip has no keys or inconsistent array sizes");
	}
	for (size_t k = 1; k < p_clip.key_times.size(); ++k) {
		if (p_clip.key_times[k] < p_clip.key_times[k - 1]) {
			return AntResult::fail(AntStatus::Malformed, "bake: key times not ascending");
		}
	}
	if (p_clip.end_frame > kMaxFrame) {
		return AntResult::fail(AntStatus::Malformed, "bake: EndFrame " + std::to_string(p_clip.end_frame) + " exceeds the 16-bit frame range");
	}
	// Both bounds are <= kMaxFrame, so frames <= 65536 and frame numbers fit in uint16_t.
	const uint32_t last = std::max<uint32_t>(p_clip.end_frame, p_clip.key_times.back());
	const uint64_t frames = uint64_t(last) + 1;
	if (frames * p_clip.channel_count() > kMaxChannelKeys) {
		return AntResult::fail(AntStatus::Malformed, "bake: frames * channels exceeds the decode bound");
	}
	r_out.codec = p_clip.codec;
	r_out.num_keys = uint32_t(frames);
	r_out.num_quats = p_clip.num_quats;
	r_out.num_vec3s = p_clip.num_vec3s;
	r_out.num_floats = p_clip.num_floats;
	r_out.end_frame = p_clip.end_frame;
	r_out.cycle = p_clip.cycle;
	r_out.additive = p_clip.additive;
	r_out.key_times.resize(size_t(frames));
	r_out.quats.reserve(size_t(frames) * r_out.num_quats);
	r_out.vec3s.reserve(size_t(frames) * r_out.num_vec3s);
	r_out.floats.reserve(size_t(frames) * r_out.num_floats);
	ClipPose pose;
	for (uint32_t f = 0; f < uint32_t(frames); ++f) {
		r_out.key_times[f] = uint16_t(f);
		sample_clip(p_clip, float(f), pose);
		r_out.quats.insert(r_out.quats.end(), pose.quats.begin(), pose.quats.end());
		r_out.vec3s.insert(r_out.vec3s.end(), pose.vec3s.begin(), pose.vec3s.end());
		r_out.floats.insert(r_out.floats.end(), pose.floats.begin(), pose.floats.end());
	}
	return {};
}

} // namespace bf3::core::ant
