#include "test_support.h"

#include "bf3/audio/audio_analysis.h"
#include "bf3/audio/dsp.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/sound_stream.h"
#include "bf3/core/audio/wave_bank.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::audio;
namespace cp = bf3::core::audio::port;
namespace ba = bf3::audio;
using ba::SamplerTraceEntry;
using ba::SamplerTraceKind;

namespace {

constexpr double kPi = 3.14159265358979323846;

void put_be24(std::vector<uint8_t> &r, uint32_t v) {
	r.push_back(uint8_t(v >> 16));
	r.push_back(uint8_t(v >> 8));
	r.push_back(uint8_t(v));
}
void put_be32(std::vector<uint8_t> &r, uint32_t v) {
	r.push_back(uint8_t(v >> 24));
	put_be24(r, v & 0xffffff);
}

void header_block(std::vector<uint8_t> &r, uint8_t p_codec, uint32_t p_channels, uint32_t p_rate, uint32_t p_frames) {
	r.push_back(0x48);
	put_be24(r, 12);
	r.push_back(p_codec);
	r.push_back(uint8_t(((p_channels - 1) << 2) | ((p_rate >> 16) & 3)));
	r.push_back(uint8_t(p_rate >> 8));
	r.push_back(uint8_t(p_rate));
	put_be32(r, p_frames);
}

std::vector<uint8_t> pcm_stream(const std::vector<int16_t> &p_samples, uint32_t p_channels, uint32_t p_rate) {
	std::vector<uint8_t> r;
	const uint32_t frames = uint32_t(p_samples.size() / p_channels);
	header_block(r, 2, p_channels, p_rate, frames);
	r.push_back(0x44);
	put_be24(r, uint32_t(8 + p_samples.size() * 2));
	put_be32(r, frames);
	for (int16_t s : p_samples) {
		r.push_back(uint8_t(uint16_t(s) >> 8));
		r.push_back(uint8_t(uint16_t(s)));
	}
	r.push_back(0x45);
	put_be24(r, 4);
	return r;
}

std::array<int16_t, 128> reference_xas(const uint8_t *f) {
	static const int k[4][2] = { { 0, 0 }, { 240, 0 }, { 460, -208 }, { 392, -220 } };
	std::array<int16_t, 128> out{};
	for (int g = 0; g < 4; ++g) {
		const uint32_t h = uint32_t(f[4 * g]) | uint32_t(f[4 * g + 1]) << 8 | uint32_t(f[4 * g + 2]) << 16 | uint32_t(f[4 * g + 3]) << 24;
		const int c1 = k[h & 3][0], c2 = k[h & 3][1];
		int s0 = int16_t(h & 0xfff0), s1 = int16_t((h >> 16) & 0xfff0);
		const int shift = 20 - int((h >> 16) & 0xf);
		out[g * 32] = int16_t(s0);
		out[g * 32 + 1] = int16_t(s1);
		int prev2 = s0, prev1 = s1;
		for (int r = 0; r < 15; ++r) {
			const uint8_t b = f[16 + r * 4 + g];
			for (int half = 0; half < 2; ++half) {
				int nib = half == 0 ? (b >> 4) : (b & 0xf);
				if (nib >= 8) {
					nib -= 16;
				}
				int v = (nib * (1 << shift) + prev1 * c1 + prev2 * c2 + 128) >> 8;
				v = std::clamp(v, -32768, 32767);
				out[g * 32 + 2 + r * 2 + half] = int16_t(v);
				prev2 = prev1;
				prev1 = v;
			}
		}
	}
	return out;
}

std::vector<uint8_t> random_xas_frame(std::mt19937 &r_rng, uint32_t p_coef) {
	std::vector<uint8_t> f(76);
	for (uint8_t &b : f) {
		b = uint8_t(r_rng());
	}
	for (int g = 0; g < 4; ++g) {
		f[4 * g] = uint8_t((f[4 * g] & 0xf0) | p_coef);
		f[4 * g + 2] = uint8_t((f[4 * g + 2] & 0xf0) | (8 + (r_rng() % 8)));
	}
	return f;
}

std::vector<uint8_t> xas_stream(std::mt19937 &r_rng, uint32_t p_channels, uint32_t p_blocks, uint32_t p_groups, std::vector<std::vector<int16_t>> &r_lanes,
		uint32_t p_total = 0, bool p_end_block = true) {
	std::vector<uint8_t> r;
	const uint32_t frames = p_blocks * p_groups * 128;
	header_block(r, 4, p_channels, 48000, p_total ? p_total : frames);
	r_lanes.assign(p_channels, {});
	for (uint32_t b = 0; b < p_blocks; ++b) {
		r.push_back(0x44);
		put_be24(r, 8 + p_groups * p_channels * 76);
		put_be32(r, p_groups * 128);
		for (uint32_t g = 0; g < p_groups; ++g) {
			for (uint32_t c = 0; c < p_channels; ++c) {
				const std::vector<uint8_t> f = random_xas_frame(r_rng, (b + g + c) % 4);
				r.insert(r.end(), f.begin(), f.end());
				const std::array<int16_t, 128> ref = reference_xas(f.data());
				r_lanes[c].insert(r_lanes[c].end(), ref.begin(), ref.end());
			}
		}
	}
	if (p_end_block) {
		r.push_back(0x45);
		put_be24(r, 4);
	}
	return r;
}

struct Builder {
	SoundProgram p;
	Builder() {
		p.name = "test";
	}
	uint16_t reg() {
		return uint16_t(p.registers++);
	}
	uint16_t input(uint32_t p_hash, bool p_event, float p_default = 0.0f) {
		const uint16_t r = reg();
		p.inputs.push_back(SoundInput{ p_hash, r, p_default, p_event });
		return r;
	}
	static SoundPort R(uint16_t p_reg) {
		return SoundPort{ p_reg, 0.0f };
	}
	static SoundPort C(float p_value) {
		return SoundPort{ kNoRegister, p_value };
	}
	SoundNode make(SoundNodeType p_type, uint8_t p_option, uint8_t p_flags) {
		const SoundNodeTypeInfo &info = sound_node_type_info(p_type);
		SoundNode n;
		n.type = p_type;
		n.option = p_option;
		n.flags = p_flags;
		n.ports_begin = uint32_t(p.ports.size());
		n.ports_count = uint16_t(info.ports.size() + cp::extra_ports(p_type));
		p.ports.resize(p.ports.size() + n.ports_count);
		n.floats_begin = uint32_t(p.floats.size());
		n.floats_count = uint16_t(info.floats.size());
		p.floats.resize(p.floats.size() + n.floats_count, 0.0f);
		return n;
	}
	uint32_t node(SoundNodeType p_type, uint8_t p_option = 0, uint8_t p_flags = 0, std::vector<uint32_t> p_children = {}) {
		SoundNode n = make(p_type, p_option, p_flags);
		n.children_begin = uint32_t(p.children.size());
		n.children_count = uint16_t(p_children.size());
		p.children.insert(p.children.end(), p_children.begin(), p_children.end());
		p.nodes.push_back(n);
		return uint32_t(p.nodes.size() - 1);
	}
	uint32_t child(SoundNodeType p_type, uint8_t p_option = 0) {
		p.child_nodes.push_back(make(p_type, p_option, 0));
		return uint32_t(p.child_nodes.size() - 1);
	}
	void set(uint32_t p_node, uint32_t p_port, SoundPort p_value) {
		p.ports[p.nodes[p_node].ports_begin + p_port] = p_value;
	}
	void cset(uint32_t p_child, uint32_t p_port, SoundPort p_value) {
		p.ports[p.child_nodes[p_child].ports_begin + p_port] = p_value;
	}
	void setf(uint32_t p_node, uint32_t p_index, float p_value) {
		p.floats[p.nodes[p_node].floats_begin + p_index] = p_value;
	}
	void csetf(uint32_t p_child, uint32_t p_index, float p_value) {
		p.floats[p.child_nodes[p_child].floats_begin + p_index] = p_value;
	}

	uint16_t sampler_voice() {
		SoundVoice v;
		v.stage = 0;
		v.plugins_begin = uint32_t(p.plugins.size());
		for (const char *id : { "SnP1", "Rsp0", "Pau0", "Gai0", "Sen0" }) {
			SoundVoicePlugin pl;
			pl.id = (uint32_t(uint8_t(id[0])) << 24) | (uint32_t(uint8_t(id[1])) << 16) | (uint32_t(uint8_t(id[2])) << 8) | uint32_t(uint8_t(id[3]));
			pl.channels = 1;
			if (std::strcmp(id, "Sen0") == 0) {
				pl.connection = uint16_t(p.connections.size());
				p.connections.push_back(SoundConnection{ true, 0, MasterBus::GameMixMain });
			}
			p.plugins.push_back(pl);
		}
		v.plugins_count = 5;
		p.voices.push_back(v);
		p.voice_order.push_back(uint16_t(p.voices.size() - 1));
		return uint16_t(p.voices.size() - 1);
	}

	uint32_t sampler(uint32_t p_slots, uint16_t p_wave, SamplerLoop p_loop, uint8_t p_flags = 0) {
		SoundNode n = make(SoundNodeType::Sampler, uint8_t(p_loop), p_flags);
		n.waves_begin = uint32_t(p.wave_refs.size());
		n.waves_count = 1;
		p.wave_refs.push_back(p_wave);
		n.plugins_begin = uint32_t(p.plugin_refs.size());
		n.plugins_count = uint16_t(p_slots * cp::kSamplerSlotPlugins);
		for (uint32_t s = 0; s < p_slots; ++s) {
			const uint16_t v = sampler_voice();
			for (uint16_t k = 0; k < 4; ++k) {
				p.plugin_refs.push_back(SoundPluginRef{ v, k });
			}
		}
		p.nodes.push_back(n);
		const uint32_t idx = uint32_t(p.nodes.size() - 1);
		setf(idx, cp::SmpBasePitch, 1.0f);
		set(idx, cp::SmpAmplitude, C(1.0f));
		set(idx, cp::SmpPitch, C(1.0f));
		return idx;
	}
	std::shared_ptr<const SoundProgram> build() {
		REQUIRE_MESSAGE(validate_sound_program(p).ok(), (validate_sound_program(p).ok() ? std::string() : validate_sound_program(p).error().message));
		return std::make_shared<SoundProgram>(p);
	}
};

std::unique_ptr<ba::PatchInstance> instance(std::shared_ptr<const SoundProgram> p_prog, std::shared_ptr<const WaveBank> p_bank = nullptr, uint64_t p_seed = 1) {
	Result<std::unique_ptr<ba::PatchInstance>> r = ba::PatchInstance::create(std::move(p_prog), std::move(p_bank), ba::AudioTuning{}, p_seed, 48000);
	REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
	return std::move(*r);
}

struct Buses {
	std::vector<float> storage = std::vector<float>(size_t(MasterBus::Unknown) * 2 * 512, 0.0f);
	ba::BusBuffers b;
	Buses() {
		for (size_t i = 0; i < size_t(MasterBus::Unknown); ++i) {
			b.channels[i][0] = storage.data() + (i * 2) * 512;
			b.channels[i][1] = storage.data() + (i * 2 + 1) * 512;
		}
	}
	void block(ba::PatchInstance &r_inst, uint64_t p_first, uint32_t p_frames = 512) {
		std::fill(storage.begin(), storage.end(), 0.0f);
		b.frames = p_frames;
		b.first_frame = p_first;
		r_inst.process_block(b);
	}
	float left(uint32_t i) const {
		return b.channels[size_t(MasterBus::GameMixMain)][0][i];
	}
};

struct TestWave {
	std::shared_ptr<const WaveBank> bank;
	Guid partition;
};
TestWave make_wave(uint32_t p_segments, uint32_t p_frames, int16_t p_first_loop, int16_t p_last_loop,
		uint32_t p_rate = 48000, bool p_sine = false, uint32_t p_channels = 1, uint32_t p_sounding_channel = 0) {
	SoundWaveInfo w;
	w.name = "test_wave";
	static const uint8_t kPartition[16] = { 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33, 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 };
	w.partition = Guid::from_bytes(kPartition);
	w.channels = uint8_t(p_channels);
	std::vector<uint8_t> chunk;
	WaveVariation var;
	var.chunk_index = 0;
	var.first_loop_segment = p_first_loop;
	var.last_loop_segment = p_last_loop;
	for (uint32_t s = 0; s < p_segments; ++s) {
		std::vector<int16_t> samples(size_t(p_frames) * p_channels, 0);
		if (p_sine) {
			for (uint32_t i = 0; i < p_frames; ++i) {
				samples[size_t(i) * p_channels + p_sounding_channel] =
						int16_t(std::lrint(16000.0 * std::sin(2.0 * kPi * 1000.0 * double(i) / double(p_rate))));
			}
		} else {
			samples[p_sounding_channel] = int16_t(1000 * (s + 1));
		}
		WaveSegment seg;
		seg.samples_offset = uint32_t(chunk.size());
		seg.length_seconds = float(p_frames) / float(p_rate);
		const std::vector<uint8_t> st = pcm_stream(samples, p_channels, p_rate);
		chunk.insert(chunk.end(), st.begin(), st.end());
		var.segments.push_back(seg);
	}
	w.chunks.push_back(WaveChunkRef{ Guid{}, uint32_t(chunk.size()) });
	w.variations.push_back(var);
	std::vector<SoundWaveInfo> waves{ w };
	std::vector<std::vector<std::vector<uint8_t>>> bytes{ { chunk } };
	Result<std::shared_ptr<const WaveBank>> bank = WaveBank::from_memory(std::move(waves), std::move(bytes));
	REQUIRE_MESSAGE(bank.ok(), (bank.ok() ? std::string() : bank.error().message));
	return TestWave{ *bank, w.partition };
}

std::vector<SamplerTraceEntry> entries(const ba::SamplerTrace &p_trace) {
	return std::vector<SamplerTraceEntry>(p_trace.entries.begin(), p_trace.entries.begin() + p_trace.count);
}

}

TEST_SUITE("audio-xas") {
	TEST_CASE("frames match the reference decoder for every coefficient pair") {
		std::mt19937 rng(7);
		for (uint32_t coef = 0; coef < 4; ++coef) {
			for (int trial = 0; trial < 64; ++trial) {
				const std::vector<uint8_t> f = random_xas_frame(rng, coef);
				std::array<int16_t, 128> got{};
				decode_xas_frame(std::span<const uint8_t, 76>(f.data(), 76), got);
				CHECK(got == reference_xas(f.data()));
			}
		}
	}
	TEST_CASE("header fields") {
		std::vector<uint8_t> s;
		header_block(s, 4, 6, 44100, 1000);
		s.resize(s.size() + 76 * 6 * 8 + 8, 0);
		Result<SoundStreamHeader> h = parse_stream_header(s, 0);
		REQUIRE(h.ok());
		CHECK(h->codec == SoundCodec::EaXas);
		CHECK(h->channels == 6);
		CHECK(h->sample_rate == 44100);
		CHECK(h->total_frames == 1000);
		CHECK(h->header_bytes == 12);
		std::vector<uint8_t> hi;
		header_block(hi, 4, 1, 96000 + 65536, 10);
		hi.resize(hi.size() + 200, 0);
		REQUIRE(parse_stream_header(hi, 0).ok());
		CHECK(parse_stream_header(hi, 0)->sample_rate == 96000 + 65536);
	}
	TEST_CASE("multi-block streams: decode_stream, StreamDecoder reads and skip_to") {
		std::mt19937 rng(11);
		for (uint32_t channels : { 1u, 2u, 4u }) {
			std::vector<std::vector<int16_t>> lanes;
			const std::vector<uint8_t> s = xas_stream(rng, channels, 3, 2, lanes);
			Result<DecodedStream> d = decode_stream(s, 0);
			REQUIRE(d.ok());
			REQUIRE(d->frames() == 3 * 2 * 128);
			bool same = true;
			for (uint32_t f = 0; f < d->frames(); ++f) {
				for (uint32_t c = 0; c < channels; ++c) {
					same = same && d->samples[size_t(f) * channels + c] == lanes[c][f];
				}
			}
			CHECK(same);
			Result<StreamScan> scan = scan_stream(s, 0, true);
			REQUIRE(scan.ok());
			CHECK(scan->decoded_frames == d->frames());
			CHECK(scan->data_blocks == 3);
			CHECK(scan->ended_by_end_block);
			CHECK(scan->stream_bytes == s.size());

			StreamDecoder dec;
			REQUIRE(dec.open(s, 0).ok());
			std::vector<float> got;
			std::vector<float> buf(97 * channels);
			for (;;) {
				Result<uint32_t> n = dec.read(buf.data(), 97);
				REQUIRE(n.ok());
				if (*n == 0) {
					break;
				}
				got.insert(got.end(), buf.begin(), buf.begin() + ptrdiff_t(*n * channels));
			}
			REQUIRE(got.size() == d->samples.size());
			bool exact = true;
			for (size_t i = 0; i < got.size(); ++i) {
				exact = exact && got[i] == float(d->samples[i]) / 32768.0f;
			}
			CHECK(exact);
			CHECK(dec.finished());

			for (uint32_t at : { 0u, 1u, 127u, 128u, 300u, 767u }) {
				StreamDecoder sk;
				REQUIRE(sk.open(s, 0).ok());
				REQUIRE(sk.skip_to(at).ok());
				CHECK(sk.position() == at);
				std::vector<float> one(channels);
				Result<uint32_t> n = sk.read(one.data(), 1);
				REQUIRE(n.ok());
				REQUIRE(*n == 1);
				CHECK(one[0] == float(d->samples[size_t(at) * channels]) / 32768.0f);
			}
		}
	}
	TEST_CASE("PCM16 big-endian streams") {
		std::vector<int16_t> samples{ 1, -2, 32767, -32768, 1234, -4321 };
		const std::vector<uint8_t> s = pcm_stream(samples, 2, 22050);
		Result<DecodedStream> d = decode_stream(s, 0);
		REQUIRE(d.ok());
		CHECK(d->header.codec == SoundCodec::Pcm16Be);
		CHECK(d->samples == samples);
	}
	TEST_CASE("hostile streams fail with an error") {
		std::mt19937 rng(3);
		std::vector<std::vector<int16_t>> lanes;
		const std::vector<uint8_t> good = xas_stream(rng, 1, 2, 2, lanes);
		const auto fails = [](const std::vector<uint8_t> &s) {
			Result<DecodedStream> d = decode_stream(s, 0);
			Result<StreamScan> sc = scan_stream(s, 0, true);
			return !d.ok() && !sc.ok();
		};

		CHECK(fails(std::vector<uint8_t>(good.begin(), good.begin() + 12 + 8 + 100)));

		{
			std::vector<uint8_t> s = good;
			s[12 + 1] = s[12 + 2] = s[12 + 3] = 0;
			CHECK(fails(s));
		}

		{
			std::vector<uint8_t> s = good;
			s[12 + 1] = 0x7f;
			CHECK(fails(s));
		}

		{
			std::vector<uint8_t> s = good;
			s[5] = uint8_t((8 << 2) | (s[5] & 3));
			CHECK(!parse_stream_header(s, 0).ok());
		}

		{
			std::vector<uint8_t> s = good;
			s[5] &= 0xfc;
			s[6] = 0;
			s[7] = 10;
			CHECK(!parse_stream_header(s, 0).ok());
		}

		{
			std::vector<uint8_t> s = good;
			s[4] = 7;
			CHECK(!parse_stream_header(s, 0).ok());
		}

		{
			std::vector<uint8_t> s = good;
			s[8] = 0x0f;
			CHECK(!parse_stream_header(s, 0).ok());
		}

		{
			std::vector<uint8_t> s = good;
			s[12 + 4 + 2] = 0x40;
			CHECK(fails(s));
		}

		{
			std::vector<uint8_t> s = good;
			s[12] = 0x50;
			CHECK(fails(s));
		}

		{
			std::vector<std::vector<int16_t>> l2;
			const std::vector<uint8_t> s = xas_stream(rng, 1, 2, 2, l2, 300);
			Result<DecodedStream> d = decode_stream(s, 0);
			REQUIRE(d.ok());
			CHECK(d->frames() == 300);
		}

		{
			std::vector<std::vector<int16_t>> l3;
			const std::vector<uint8_t> s = xas_stream(rng, 1, 1, 1, l3, 0, false);
			Result<StreamScan> sc = scan_stream(s, 0, false);
			REQUIRE(sc.ok());
			CHECK_FALSE(sc->ended_by_end_block);
			CHECK(sc->decoded_frames == 128);
		}

		{
			std::vector<uint8_t> s(good.begin(), good.begin() + 12 + 8 + 76 * 2 + 8 + 76);
			s[12 + 8 + 76 * 2 + 1] = 0;
			s[12 + 8 + 76 * 2 + 2] = 0;
			s[12 + 8 + 76 * 2 + 3] = 8 + 76 * 2;
			StreamDecoder dec;
			if (dec.open(s, 0).ok()) {
				std::vector<float> buf(1024);
				bool failed = false;
				for (int i = 0; i < 16 && !failed; ++i) {
					Result<uint32_t> n = dec.read(buf.data(), 64);
					failed = !n.ok();
					if (n.ok() && *n == 0) {
						break;
					}
				}
				CHECK(failed);
			}
		}
	}
}

TEST_SUITE("audio-patch") {
	TEST_CASE("ScaleClamp, SimpleTransform, Crossfader2 at edge values") {
		Builder b;
		const uint16_t x = b.input(1, false), y = b.input(2, false);
		const uint16_t sc = b.reg(), step = b.reg(), cf1 = b.reg(), cf2 = b.reg();
		uint32_t n = b.node(SoundNodeType::ScaleClamp);
		b.set(n, cp::ScIn, Builder::R(x));
		b.set(n, cp::ScInMin, Builder::C(17));
		b.set(n, cp::ScInMax, Builder::C(200));
		b.set(n, cp::ScOutMin, Builder::C(0));
		b.set(n, cp::ScOutMax, Builder::C(310));
		b.set(n, cp::ScOut, Builder::R(sc));
		n = b.node(SoundNodeType::ScaleClamp);
		b.set(n, cp::ScIn, Builder::R(x));
		b.set(n, cp::ScInMin, Builder::C(5));
		b.set(n, cp::ScInMax, Builder::C(5));
		b.set(n, cp::ScOutMin, Builder::C(-1));
		b.set(n, cp::ScOutMax, Builder::C(1));
		b.set(n, cp::ScOut, Builder::R(step));
		n = b.node(SoundNodeType::Crossfader2);
		b.set(n, cp::Cf2Ctrl, Builder::R(y));
		b.set(n, cp::Cf2Out1, Builder::R(cf1));
		b.set(n, cp::Cf2Out2, Builder::R(cf2));
		std::vector<uint16_t> ops;
		for (uint8_t op = 0; op < uint8_t(SimpleTransformOp::Unknown); ++op) {
			const uint16_t z = b.reg();
			ops.push_back(z);
			n = b.node(SoundNodeType::SimpleTransform, op);
			b.set(n, cp::StX, Builder::R(x));
			b.set(n, cp::StY, Builder::R(y));
			b.set(n, cp::StZ, Builder::R(z));
		}
		auto inst = instance(b.build());
		const auto eval = [&](float vx, float vy) {
			inst->set_parameter(1, vx);
			inst->set_parameter(2, vy);
			inst->evaluate(0.0);
		};
		eval(150, 0.25f);
		CHECK(inst->value(sc) == doctest::Approx(225.30).epsilon(1e-5));
		CHECK(inst->value(cf1) == doctest::Approx(0.75));
		CHECK(inst->value(cf2) == doctest::Approx(0.25));
		eval(0, 2.0f);
		CHECK(inst->value(sc) == 0.0f);
		CHECK(inst->value(step) == -1.0f);
		CHECK(inst->value(cf2) == 1.0f);
		eval(1000, -1.0f);
		CHECK(inst->value(sc) == 310.0f);
		CHECK(inst->value(step) == 1.0f);
		CHECK(inst->value(cf1) == 1.0f);

		eval(2, 3);
		const auto z = [&](SimpleTransformOp op) { return inst->value(ops[size_t(op)]); };
		CHECK(z(SimpleTransformOp::Add) == 5.0f);
		CHECK(z(SimpleTransformOp::Subtract) == -1.0f);
		CHECK(z(SimpleTransformOp::Multiply) == 6.0f);
		CHECK(z(SimpleTransformOp::Divide) == doctest::Approx(2.0 / 3.0));
		CHECK(z(SimpleTransformOp::Power) == 8.0f);
		CHECK(z(SimpleTransformOp::Modulus) == 2.0f);
		CHECK(z(SimpleTransformOp::Sine) == doctest::Approx(std::sin(2.0)));
		CHECK(z(SimpleTransformOp::SquareRoot) == doctest::Approx(std::sqrt(2.0)));
		eval(-4, 0);
		CHECK(z(SimpleTransformOp::Divide) == 0.0f);
		CHECK(z(SimpleTransformOp::Modulus) == 0.0f);
		CHECK(z(SimpleTransformOp::SquareRoot) == 0.0f);
		CHECK(z(SimpleTransformOp::Log) == 0.0f);
		CHECK(z(SimpleTransformOp::Log10) == 0.0f);
		CHECK(z(SimpleTransformOp::ArcSine) == 0.0f);
		CHECK(z(SimpleTransformOp::ArcCosine) == 0.0f);
		CHECK(z(SimpleTransformOp::Absolute) == 4.0f);
		CHECK(z(SimpleTransformOp::Floor) == -4.0f);
		eval(-2, 0.5f);
		CHECK(z(SimpleTransformOp::Power) == 0.0f);
		eval(1000, 1);
		CHECK(z(SimpleTransformOp::Exp) == 0.0f);
	}

	TEST_CASE("Condition, LogicalExpression, DeMux, Switcher, ValueSelector, SimpleValueSelector") {
		Builder b;
		const uint16_t x = b.input(1, false), y = b.input(2, false), ev = b.input(3, true);
		std::vector<uint16_t> cond_out;
		std::vector<uint32_t> groups;
		for (uint8_t op = 0; op < uint8_t(ConditionOp::Unknown); ++op) {
			const uint32_t c = b.child(SoundNodeType::ConditionGroup, op);
			const uint16_t o = b.reg();
			b.cset(c, cp::CondX, Builder::R(x));
			b.cset(c, cp::CondY, Builder::R(y));
			b.cset(c, cp::CondTrue, Builder::R(o));
			groups.push_back(c);
			cond_out.push_back(o);
		}
		b.node(SoundNodeType::Condition, 0, 0, groups);
		std::vector<uint16_t> logic_out;
		for (uint8_t op = 0; op < uint8_t(LogicalOp::Unknown); ++op) {
			const uint32_t e1 = b.child(SoundNodeType::LogicalExpressionEntry), e2 = b.child(SoundNodeType::LogicalExpressionEntry);
			b.cset(e1, cp::LogicEntryTrigger, Builder::R(x));
			b.cset(e2, cp::LogicEntryTrigger, Builder::R(y));
			const uint16_t o = b.reg();
			const uint32_t n = b.node(SoundNodeType::LogicalExpression, op, 0, { e1, e2 });
			b.set(n, cp::LogicTrigger, Builder::R(o));
			logic_out.push_back(o);
		}
		std::vector<uint16_t> demux_clamp, demux_wrap;
		for (int wrap = 0; wrap < 2; ++wrap) {
			std::vector<uint32_t> outs;
			for (int i = 0; i < 3; ++i) {
				const uint32_t c = b.child(SoundNodeType::DeMuxOutput);
				const uint16_t o = b.reg();
				b.cset(c, cp::DeMuxOutTrigger, Builder::R(o));
				outs.push_back(c);
				(wrap ? demux_wrap : demux_clamp).push_back(o);
			}
			const uint32_t n = b.node(SoundNodeType::DeMux, 0, wrap ? kFlagWrapValue : 0, outs);
			b.set(n, cp::DeMuxTrigger, Builder::R(ev));
			b.set(n, cp::DeMuxValue, Builder::R(x));
		}
		std::vector<uint16_t> sw_out;
		{
			std::vector<uint32_t> cases;
			for (float cv : { 0.0f, 1.0f, -1.0f }) {
				const uint32_t c = b.child(SoundNodeType::SwitcherEntry);
				b.csetf(c, cp::CaseValue, cv);
				const uint16_t o = b.reg();
				b.cset(c, cp::SwitchCaseTrigger, Builder::R(o));
				cases.push_back(c);
				sw_out.push_back(o);
			}
			const uint32_t n = b.node(SoundNodeType::Switcher, 0, 0, cases);
			b.set(n, cp::SwTrigger, Builder::R(ev));
			b.set(n, cp::SwValue, Builder::R(x));
			b.setf(n, cp::SwDefault, -1.0f);
		}
		const uint16_t vs_out = b.reg();
		{
			std::vector<uint32_t> cases;
			for (std::pair<float, float> cv : { std::pair{ 0.0f, 10.0f }, std::pair{ 1.0f, 20.0f } }) {
				const uint32_t c = b.child(SoundNodeType::ValueSelectorEntry);
				b.csetf(c, cp::CaseValue, cv.first);
				b.cset(c, cp::VsEntryInput, Builder::C(cv.second));
				cases.push_back(c);
			}
			const uint32_t n = b.node(SoundNodeType::ValueSelector, 0, 0, cases);
			b.set(n, cp::VsValue, Builder::R(x));
			b.set(n, cp::VsOut, Builder::R(vs_out));
			b.setf(n, cp::VsDefault, 1.0f);
		}
		const uint16_t svs_out = b.reg();
		{
			SoundNode n = b.make(SoundNodeType::SimpleValueSelector, 0, 0);
			for (float v : { 0.0f, 1.9f, 3.0f }) {
				b.p.floats.push_back(v);
			}
			n.floats_count = 3;
			b.p.nodes.push_back(n);
			const uint32_t idx = uint32_t(b.p.nodes.size() - 1);
			b.set(idx, cp::SvsIndex, Builder::R(x));
			b.set(idx, cp::SvsOut, Builder::R(svs_out));
		}
		auto inst = instance(b.build());
		const auto eval = [&](float vx, float vy, bool event) {
			inst->set_parameter(1, vx);
			inst->set_parameter(2, vy);
			if (event) {
				inst->post_event(3, 0);
			}
			inst->evaluate(0.0);
		};

		const float xs[] = { 0, 1, 0, 1 }, ys[] = { 0, 0, 1, 1 };
		const bool expect_and[] = { 0, 0, 0, 1 }, expect_or[] = { 0, 1, 1, 1 }, expect_xor[] = { 0, 1, 1, 0 };
		for (int i = 0; i < 4; ++i) {
			eval(xs[i], ys[i], false);
			CHECK(inst->value(cond_out[0]) == float(expect_and[i]));
			CHECK(inst->value(cond_out[1]) == float(expect_or[i]));
			CHECK(inst->value(cond_out[2]) == float(expect_xor[i]));
			CHECK(inst->value(logic_out[0]) == float(expect_and[i]));
			CHECK(inst->value(logic_out[1]) == float(expect_or[i]));
			CHECK(inst->value(logic_out[2]) == float(!expect_and[i]));
			CHECK(inst->value(logic_out[3]) == float(!expect_or[i]));
		}
		eval(2, 3, false);
		CHECK(inst->value(cond_out[3]) == 0.0f);
		CHECK(inst->value(cond_out[4]) == 1.0f);
		CHECK(inst->value(cond_out[5]) == 0.0f);
		CHECK(inst->value(cond_out[6]) == 1.0f);
		CHECK(inst->value(cond_out[7]) == 0.0f);
		eval(3.0005f, 3, false);
		CHECK(inst->value(cond_out[3]) == 1.0f);

		eval(1.7f, 0, true);
		CHECK(inst->value(demux_clamp[1]) == 1.0f);
		CHECK(inst->value(demux_clamp[0]) + inst->value(demux_clamp[2]) == 0.0f);
		eval(1.7f, 0, false);
		CHECK(inst->value(demux_clamp[1]) == 0.0f);
		eval(7.2f, 0, true);
		CHECK(inst->value(demux_clamp[2]) == 1.0f);
		CHECK(inst->value(demux_wrap[1]) == 1.0f);
		eval(-1.5f, 0, true);
		CHECK(inst->value(demux_clamp[0]) == 1.0f);
		CHECK(inst->value(demux_wrap[1]) == 1.0f);

		eval(1, 0, true);
		CHECK(inst->value(sw_out[1]) == 1.0f);
		CHECK(inst->value(sw_out[0]) + inst->value(sw_out[2]) == 0.0f);
		eval(5, 0, true);
		CHECK(inst->value(sw_out[2]) == 1.0f);
		eval(0, 0, false);
		CHECK(inst->value(sw_out[0]) == 0.0f);

		eval(0, 0, false);
		CHECK(inst->value(vs_out) == 10.0f);
		eval(9, 0, false);
		CHECK(inst->value(vs_out) == 20.0f);

		eval(1.99f, 0, false);
		CHECK(inst->value(svs_out) == doctest::Approx(1.9));
		eval(-3, 0, false);
		CHECK(inst->value(svs_out) == 0.0f);
		eval(42, 0, false);
		CHECK(inst->value(svs_out) == 3.0f);
	}

	TEST_CASE("Adsr, Lfo, NumberGenerator, ParameterFilterLinear, Timer, EventGate over time") {
		Builder b;
		const uint16_t trig = b.input(1, true), rel = b.input(2, true), in = b.input(3, false), gate_en = b.input(4, false, 1.0f);
		const uint16_t adsr = b.reg(), fin = b.reg(), lfo = b.reg(), num = b.reg(), pfl = b.reg(), tick = b.reg(), progress = b.reg(), gated = b.reg();
		uint32_t n = b.node(SoundNodeType::Adsr);
		b.set(n, cp::AdsrA, Builder::C(0.1f));
		b.set(n, cp::AdsrD, Builder::C(0.1f));
		b.set(n, cp::AdsrS, Builder::C(0.5f));
		b.set(n, cp::AdsrR, Builder::C(0.2f));
		b.set(n, cp::AdsrTrigger, Builder::R(trig));
		b.set(n, cp::AdsrRelease, Builder::R(rel));
		b.set(n, cp::AdsrValue, Builder::R(adsr));
		b.set(n, cp::AdsrFinished, Builder::R(fin));
		n = b.node(SoundNodeType::Lfo);
		b.setf(n, cp::LfoMin, 0.96f);
		b.setf(n, cp::LfoMax, 1.0f);
		b.set(n, cp::LfoAmplitude, Builder::C(1.0f));
		b.set(n, cp::LfoHz, Builder::C(4.0f));
		b.set(n, cp::LfoOut, Builder::R(lfo));
		n = b.node(SoundNodeType::NumberGenerator);
		b.setf(n, cp::NumMin, 1.21f);
		b.setf(n, cp::NumMax, 1.262f);
		b.set(n, cp::NumTrigger, Builder::R(trig));
		b.set(n, cp::NumY, Builder::R(num));
		n = b.node(SoundNodeType::ParameterFilterLinear);
		b.set(n, cp::PflIn, Builder::R(in));
		b.set(n, cp::PflAttack, Builder::C(10.0f));
		b.set(n, cp::PflRelease, Builder::C(0.5f));
		b.set(n, cp::PflOut, Builder::R(pfl));
		n = b.node(SoundNodeType::Timer, uint8_t(TimerMode::Repeating));
		b.set(n, cp::TimerStart, Builder::R(trig));
		b.set(n, cp::TimerStop, Builder::R(rel));
		b.set(n, cp::TimerPeriod, Builder::C(0.25f));
		b.set(n, cp::TimerTick, Builder::R(tick));
		b.set(n, cp::TimerProgress, Builder::R(progress));
		n = b.node(SoundNodeType::EventGate);
		b.set(n, cp::GateIn, Builder::R(trig));
		b.set(n, cp::GateEnable, Builder::R(gate_en));
		b.set(n, cp::GateCoolDown, Builder::C(0.5f));
		b.set(n, cp::GateOut, Builder::R(gated));
		const std::shared_ptr<const SoundProgram> prog = b.build();
		auto inst = instance(prog, nullptr, 5);
		const double dt = 0.001;
		inst->evaluate(dt);
		CHECK(inst->value(num) == doctest::Approx(1.21f));
		CHECK(inst->value(adsr) == 0.0f);
		inst->post_event(1, 0);
		inst->evaluate(dt);
		CHECK(inst->value(gated) == 1.0f);
		const float drawn = inst->value(num);
		CHECK(drawn >= 1.21f);
		CHECK(drawn <= 1.262f);
		double t = 0.0;
		float lfo_min = 10, lfo_max = -10;
		uint32_t ticks = 0;
		bool reached_one = false;
		for (int i = 0; i < 400; ++i) {
			inst->evaluate(dt);
			t += dt;
			reached_one = reached_one || inst->value(adsr) >= 0.999f;
			lfo_min = std::min(lfo_min, inst->value(lfo));
			lfo_max = std::max(lfo_max, inst->value(lfo));
			ticks += inst->value(tick) == 1.0f ? 1 : 0;
			if (i == 50) {
				inst->post_event(1, 0);
			}
			if (i == 51) {
				CHECK(inst->value(gated) == 0.0f);
			}
		}
		CHECK(reached_one);
		CHECK(inst->value(adsr) == doctest::Approx(0.5f));
		CHECK(lfo_min == doctest::Approx(0.96f).epsilon(1e-3));
		CHECK(lfo_max == doctest::Approx(1.0f).epsilon(1e-3));
		CHECK(ticks == 1);

		inst->post_event(2, 0);
		inst->evaluate(dt);
		bool finished = false;
		for (int i = 0; i < 300 && !finished; ++i) {
			inst->evaluate(dt);
			finished = inst->value(fin) == 1.0f;
		}
		CHECK(finished);
		CHECK(inst->value(adsr) == 0.0f);

		inst->set_parameter(3, 1.0f);
		inst->evaluate(0.05);
		inst->evaluate(0.0);
		CHECK(inst->value(pfl) == doctest::Approx(0.5f));
		inst->evaluate(1.0);
		inst->evaluate(0.0);
		CHECK(inst->value(pfl) == 1.0f);
		inst->set_parameter(3, 0.0f);
		inst->evaluate(1.0);
		inst->evaluate(0.0);
		CHECK(inst->value(pfl) == doctest::Approx(0.5f));

		auto same = instance(prog, nullptr, 5), other = instance(prog, nullptr, 6);
		for (auto *p : { same.get(), other.get() }) {
			p->evaluate(dt);
			p->post_event(1, 0);
			p->evaluate(dt);
		}
		CHECK(same->value(num) == drawn);
		CHECK(other->value(num) != drawn);
	}

	TEST_CASE("MultiCrossfader reading R: bands, Start/Stop gating, LockControlValue") {
		Builder b;
		const uint16_t d = b.input(1, false), start = b.input(2, true), stop = b.input(3, true);
		struct G {
			float begin, end;
			FaderType type;
			uint16_t amp, s, e;
		};
		std::vector<G> gs{ { 1, 5, FaderType::Linear, 0, 0, 0 }, { 20, 50, FaderType::Cosine, 0, 0, 0 }, { 250, 300, FaderType::Cosine, 0, 0, 0 } };
		std::vector<uint32_t> kids;
		for (G &g : gs) {
			const uint32_t c = b.child(SoundNodeType::MultiCrossfaderGroup, uint8_t(g.type));
			g.amp = b.reg();
			g.s = b.reg();
			g.e = b.reg();
			b.csetf(c, cp::GroupFadeAmplitude, 1.0f);
			b.csetf(c, cp::GroupFadeBegin, g.begin);
			b.csetf(c, cp::GroupFadeEnd, g.end);
			b.cset(c, cp::GroupAmplitude, Builder::R(g.amp));
			b.cset(c, cp::GroupStart, Builder::R(g.s));
			b.cset(c, cp::GroupStop, Builder::R(g.e));
			kids.push_back(c);
		}
		const uint32_t mcf = b.node(SoundNodeType::MultiCrossfader, 0, 0, kids);
		b.set(mcf, cp::McfControl, Builder::R(d));
		b.set(mcf, cp::McfStart, Builder::R(start));
		b.set(mcf, cp::McfStop, Builder::R(stop));

		const uint32_t gc = b.child(SoundNodeType::MultiCrossfaderGroup, uint8_t(FaderType::Linear));
		const uint16_t gate_start = b.reg(), gate_stop = b.reg();
		b.csetf(gc, cp::GroupFadeAmplitude, 1.0f);
		b.csetf(gc, cp::GroupFadeBegin, 300);
		b.csetf(gc, cp::GroupFadeEnd, 301);
		b.cset(gc, cp::GroupStart, Builder::R(gate_start));
		b.cset(gc, cp::GroupStop, Builder::R(gate_stop));
		const uint32_t gate = b.node(SoundNodeType::MultiCrossfader, 0, kFlagLockControlValue, { gc });
		b.set(gate, cp::McfControl, Builder::R(d));
		b.set(gate, cp::McfStart, Builder::R(start));
		b.set(gate, cp::McfStop, Builder::R(stop));
		auto inst = instance(b.build());
		const auto at = [&](float metres) {
			inst->set_parameter(1, metres);
			inst->evaluate(0.0);
		};
		at(0);
		CHECK(inst->value(gs[0].amp) == 1.0f);
		CHECK(inst->value(gs[1].amp) == 0.0f);
		at(3);
		CHECK(inst->value(gs[0].amp) == doctest::Approx(0.5f));
		CHECK(inst->value(gs[1].amp) == doctest::Approx(0.5f));
		at(10);
		CHECK(inst->value(gs[0].amp) == 0.0f);
		CHECK(inst->value(gs[1].amp) == 1.0f);
		at(35);
		CHECK(inst->value(gs[1].amp) == doctest::Approx(0.5f));
		CHECK(inst->value(gs[2].amp) == doctest::Approx(0.5f));
		at(100);
		CHECK(inst->value(gs[2].amp) == 1.0f);
		at(275);
		CHECK(inst->value(gs[2].amp) == doctest::Approx(0.5f));
		at(310);
		CHECK(inst->value(gs[2].amp) == 0.0f);
		CHECK(inst->crossfader_group_amplitude(mcf, 2) == 0.0f);

		inst->set_parameter(1, 302);
		inst->post_event(2, 0);
		inst->evaluate(0.0);
		CHECK(inst->value(gate_start) == 0.0f);
		inst->set_parameter(1, 299);
		inst->post_event(2, 0);
		inst->evaluate(0.0);
		CHECK(inst->value(gate_start) == 1.0f);
		CHECK(inst->value(gs[2].s) == 1.0f);
		CHECK(inst->value(gs[0].s) == 0.0f);

		inst->set_parameter(1, 400);
		inst->evaluate(0.0);
		CHECK(inst->crossfader_group_amplitude(gate, 0) == 1.0f);
		inst->post_event(3, 0);
		inst->evaluate(0.0);
		CHECK(inst->value(gate_stop) == 1.0f);
		CHECK(inst->value(gs[2].e) == 1.0f);
	}

	TEST_CASE("Sampler: stepped steps on their frames on alternating slots, Delay latched, Release plays the tail") {
		const TestWave w = make_wave(5, 1000, 1, 3);
		Builder b;
		const uint16_t trig = b.input(1, true), step = b.input(2, true), rel = b.input(3, true), es = b.input(4, false, 1.0f);
		b.p.waves.push_back(SoundWaveRef{ w.partition, "test_wave" });
		const uint32_t s = b.sampler(2, 0, SamplerLoop::Release);
		b.set(s, cp::SmpTrigger, Builder::R(trig));
		b.set(s, cp::SmpStep, Builder::R(step));
		b.set(s, cp::SmpRelease, Builder::R(rel));
		b.set(s, cp::SmpEnableStep, Builder::R(es));
		b.set(s, cp::SmpDelay, Builder::C(0.01f));
		auto inst = instance(b.build(), w.bank);
		ba::SamplerTrace trace;
		trace.entries.resize(64);
		inst->set_sampler_trace(&trace);
		Buses buses;
		inst->post_event(1, 100);
		buses.block(*inst, 0);
		inst->post_event(2, 300);
		buses.block(*inst, 512);
		buses.block(*inst, 1024);
		inst->post_event(2, 7);
		buses.block(*inst, 1536);
		inst->post_event(3, 0);
		for (uint64_t f = 2048; f < 8192; f += 512) {
			buses.block(*inst, f);
		}
		const std::vector<SamplerTraceEntry> e = entries(trace);
		REQUIRE(e.size() >= 5);
		CHECK(e[0].kind == SamplerTraceKind::Trigger);
		CHECK(e[0].frame == 100 + 480);
		CHECK(e[0].segment == 0);
		CHECK(e[1].kind == SamplerTraceKind::Step);
		CHECK(e[1].frame == 812 + 480);
		CHECK(e[1].slot != e[0].slot);
		CHECK(e[1].segment == 1);

		bool slot0_end = false, tail = false;
		uint64_t step2 = 0;
		uint8_t step2_slot = 0;
		for (const SamplerTraceEntry &x : e) {
			slot0_end = slot0_end || (x.kind == SamplerTraceKind::End && x.slot == e[0].slot && x.frame >= 580 + 1000 && x.frame <= 580 + 1003);
			if (x.kind == SamplerTraceKind::Step && x.frame == 1543 + 480) {
				step2 = x.frame;
				step2_slot = x.slot;
			}
			if (x.kind == SamplerTraceKind::Tail) {
				tail = true;
				CHECK(x.segment == 4);
				CHECK(x.slot == step2_slot);
				CHECK(x.prev_end == x.frame);
			}
		}
		CHECK(slot0_end);
		CHECK(step2 == 1543 + 480);
		CHECK(tail);
		CHECK(e.back().kind == SamplerTraceKind::End);
		CHECK(inst->finished());

		Buses again;
		auto second = instance(std::make_shared<SoundProgram>(inst->program()), w.bank);
		second->post_event(1, 100);
		again.block(*second, 0);
		again.block(*second, 512);
		CHECK(again.left(100 + 480 - 512 - 1) == 0.0f);
		CHECK(again.left(100 + 480 - 512) == doctest::Approx(1000.0f / 32768.0f));
	}

	TEST_CASE("Sampler: a slot stepped twice within a block or across a block edge fades every old play out") {
		const TestWave w = make_wave(5, 4800, 1, 3, 48000, true);
		Builder b;
		const uint16_t trig = b.input(1, true), step = b.input(2, true);
		b.p.waves.push_back(SoundWaveRef{ w.partition, "test_wave" });
		const uint32_t s = b.sampler(1, 0, SamplerLoop::Release);
		b.set(s, cp::SmpTrigger, Builder::R(trig));
		b.set(s, cp::SmpStep, Builder::R(step));
		b.set(s, cp::SmpEnableStep, Builder::C(1.0f));
		auto inst = instance(b.build(), w.bank);
		Buses buses;
		std::vector<float> out;
		const auto block = [&](uint64_t p_first) {
			buses.block(*inst, p_first);
			out.insert(out.end(), buses.b.channels[size_t(MasterBus::GameMixMain)][0],
					buses.b.channels[size_t(MasterBus::GameMixMain)][0] + 512);
		};
		inst->post_event(1, 0);
		block(0);
		inst->post_event(2, 100);
		inst->post_event(2, 160);
		block(512);
		inst->post_event(2, 500);
		block(1024);
		inst->post_event(2, 20);
		block(1536);
		block(2048);
		float largest = 0.0f, loudest = 0.0f;
		for (size_t i = 1; i < out.size(); ++i) {
			largest = std::max(largest, std::fabs(out[i] - out[i - 1]));
			loudest = std::max(loudest, std::fabs(out[i]));
		}
		const float sine_step = 16000.0f / 32768.0f * float(2.0 * kPi * 1000.0 / 48000.0);
		MESSAGE("largest sample step " << largest << ", sine step " << sine_step << ", peak " << loudest);
		CHECK(loudest > 0.4f);
		CHECK(largest < 2.5f * sine_step);
		CHECK(inst->stats().slot_takeovers == 4);
		CHECK(inst->stats().takeover_cuts == 0);
	}

	TEST_CASE("Sampler: unstepped LtRelease plays loop segments back to back; Trigger and Release together play intro then tail") {
		const TestWave w = make_wave(5, 1000, 1, 3);
		Builder b;
		const uint16_t trig = b.input(1, true), rel = b.input(3, true);
		b.p.waves.push_back(SoundWaveRef{ w.partition, "test_wave" });
		const uint32_t s = b.sampler(2, 0, SamplerLoop::Release);
		b.set(s, cp::SmpTrigger, Builder::R(trig));
		b.set(s, cp::SmpRelease, Builder::R(rel));
		const std::shared_ptr<const SoundProgram> prog = b.build();
		{
			auto inst = instance(prog, w.bank);
			ba::SamplerTrace trace;
			trace.entries.resize(64);
			inst->set_sampler_trace(&trace);
			Buses buses;
			inst->post_event(1, 0);
			uint64_t f = 0;
			for (; f < 6000; f += 512) {
				buses.block(*inst, f);
			}
			inst->post_event(3, 0);
			for (; f < 12000; f += 512) {
				buses.block(*inst, f);
			}
			const std::vector<SamplerTraceEntry> e = entries(trace);
			REQUIRE(e.size() >= 8);
			CHECK(e[0].kind == SamplerTraceKind::Trigger);
			for (size_t i = 1; i + 1 < e.size(); ++i) {
				CHECK(e[i].slot == e[0].slot);
				CHECK(e[i].frame == e[i - 1].frame + 1000);
				CHECK(e[i].prev_end == e[i].frame);
			}

			CHECK(e[1].segment == 1);
			CHECK(e[3].segment == 3);
			CHECK(e[4].segment == 1);
			const SamplerTraceEntry &tail = e[e.size() - 2];
			CHECK(tail.kind == SamplerTraceKind::Tail);
			CHECK(tail.segment == 4);
			CHECK(tail.frame == 7000);
			CHECK(e.back().kind == SamplerTraceKind::End);
		}
		{

			auto inst = instance(prog, w.bank);
			ba::SamplerTrace trace;
			trace.entries.resize(16);
			inst->set_sampler_trace(&trace);
			Buses buses;
			inst->post_event(1, 10);
			inst->post_event(3, 10);
			for (uint64_t f = 0; f < 4096; f += 512) {
				buses.block(*inst, f);
			}
			const std::vector<SamplerTraceEntry> e = entries(trace);
			REQUIRE(e.size() == 3);
			CHECK(e[0].segment == 0);
			CHECK(e[1].kind == SamplerTraceKind::Tail);
			CHECK(e[1].segment == 4);
			CHECK(e[1].frame == 1010);
			CHECK(e[2].kind == SamplerTraceKind::End);
			CHECK(inst->finished());
		}
	}

	TEST_CASE("Sampler: resampler is bit-exact at ratio 1 and clean at 0.5 and 2") {
		const TestWave w = make_wave(1, 48000, -1, -1, 48000, true);
		for (float pitch : { 1.0f, 0.5f, 2.0f }) {
			Builder b;
			const uint16_t trig = b.input(1, true);
			b.p.waves.push_back(SoundWaveRef{ w.partition, "test_wave" });
			const uint32_t s = b.sampler(1, 0, SamplerLoop::None);
			b.set(s, cp::SmpTrigger, Builder::R(trig));
			b.set(s, cp::SmpPitch, Builder::C(pitch));
			auto inst = instance(b.build(), w.bank);
			Buses buses;
			inst->post_event(1, 0);
			std::vector<float> out;
			for (uint64_t f = 0; f < 16384; f += 512) {
				buses.block(*inst, f);
				out.insert(out.end(), buses.b.channels[size_t(MasterBus::GameMixMain)][0], buses.b.channels[size_t(MasterBus::GameMixMain)][0] + 512);
			}
			if (pitch == 1.0f) {
				bool exact = true;
				for (uint32_t i = 0; i < 16384; ++i) {
					const float want = float(int16_t(std::lrint(16000.0 * std::sin(2.0 * kPi * 1000.0 * double(i) / 48000.0)))) / 32768.0f;
					exact = exact && out[i] == want;
				}
				CHECK(exact);
			} else {

				std::vector<std::complex<double>> x(8192);
				for (size_t i = 0; i < x.size(); ++i) {
					const double a = 2.0 * kPi * double(i) / 8192.0;
					const double win = 0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a);
					x[i] = out[4096 + i] * win;
				}

				double fund = 0.0, rest = 0.0;
				const double f0 = 1000.0 * pitch;
				for (int k = 1; k < 4096; k += 1) {
					const double hz = k * 48000.0 / 8192.0;
					if (hz > 12000.0) {
						break;
					}
					std::complex<double> acc = 0.0;
					for (size_t i = 0; i < x.size(); i += 1) {
						acc += x[i] * std::polar(1.0, -2.0 * kPi * double(k) * double(i) / 8192.0);
					}
					(std::fabs(hz - f0) < 60.0 ? fund : rest) += std::norm(acc);
				}
				CHECK(10.0 * std::log10(rest / fund) < -60.0);
			}
		}
	}

	TEST_CASE("the M16A3 fire-mode network: EnableStep 0 releases on every Fire, 1 only on the stop (until 3 s after it falls)") {
		Builder b;
		const uint16_t fire = b.input(10, true), stop = b.input(11, true), enable = b.input(12, false), extra = b.input(13, false);
		const uint16_t v118 = b.reg(), v119 = b.reg(), v101 = b.reg(), v120 = b.reg(), v102 = b.reg(), v100 = b.reg();
		uint32_t n = b.node(SoundNodeType::SimpleTransform, uint8_t(SimpleTransformOp::Add));
		b.set(n, cp::StX, Builder::R(extra));
		b.set(n, cp::StY, Builder::R(enable));
		b.set(n, cp::StZ, Builder::R(v118));
		{
			SoundNode svs = b.make(SoundNodeType::SimpleValueSelector, 0, 0);
			for (float v : { 0.0f, 1.9f, 1.9f, 1.9f, 1.9f }) {
				b.p.floats.push_back(v);
			}
			svs.floats_count = 5;
			b.p.nodes.push_back(svs);
			n = uint32_t(b.p.nodes.size() - 1);
			b.set(n, cp::SvsIndex, Builder::R(v118));
			b.set(n, cp::SvsOut, Builder::R(v119));
		}
		{
			const uint32_t o0 = b.child(SoundNodeType::DeMuxOutput), o1 = b.child(SoundNodeType::DeMuxOutput);
			b.cset(o0, cp::DeMuxOutTrigger, Builder::R(v101));
			n = b.node(SoundNodeType::DeMux, 0, 0, { o0, o1 });
			b.set(n, cp::DeMuxTrigger, Builder::R(fire));
			b.set(n, cp::DeMuxValue, Builder::R(v119));
		}
		n = b.node(SoundNodeType::ParameterFilterLinear);
		b.set(n, cp::PflIn, Builder::R(v119));
		b.set(n, cp::PflAttack, Builder::C(10000.0f));
		b.set(n, cp::PflRelease, Builder::C(0.3f));
		b.set(n, cp::PflOut, Builder::R(v120));
		{
			const uint32_t o0 = b.child(SoundNodeType::DeMuxOutput), o1 = b.child(SoundNodeType::DeMuxOutput);
			b.cset(o1, cp::DeMuxOutTrigger, Builder::R(v102));
			n = b.node(SoundNodeType::DeMux, 0, 0, { o0, o1 });
			b.set(n, cp::DeMuxTrigger, Builder::R(stop));
			b.set(n, cp::DeMuxValue, Builder::R(v120));
		}
		{
			const uint32_t e1 = b.child(SoundNodeType::LogicalExpressionEntry), e2 = b.child(SoundNodeType::LogicalExpressionEntry);
			b.cset(e1, cp::LogicEntryTrigger, Builder::R(v101));
			b.cset(e2, cp::LogicEntryTrigger, Builder::R(v102));
			n = b.node(SoundNodeType::LogicalExpression, uint8_t(LogicalOp::Or), 0, { e1, e2 });
			b.set(n, cp::LogicTrigger, Builder::R(v100));
		}
		const std::shared_ptr<const SoundProgram> prog = b.build();
		{
			auto single = instance(prog);
			single->set_parameter(12, 0.0f);
			single->evaluate(0.01);
			single->post_event(10, 0);
			single->evaluate(0.01);
			CHECK(single->value(v100) == 1.0f);
		}
		{
			auto automatic = instance(prog);
			automatic->set_parameter(12, 1.0f);
			automatic->evaluate(0.01);
			automatic->post_event(10, 0);
			automatic->evaluate(0.01);
			CHECK(automatic->value(v100) == 0.0f);
			automatic->post_event(11, 0);
			automatic->evaluate(0.01);
			CHECK(automatic->value(v100) == 1.0f);
		}
		for (double after : { 2.9, 3.1 }) {
			auto inst = instance(prog);
			inst->set_parameter(12, 1.0f);
			inst->evaluate(0.0);
			inst->set_parameter(12, 0.0f);
			inst->evaluate(after);
			inst->post_event(11, 0);
			inst->evaluate(0.0);
			CHECK(inst->value(v100) == (after < 3.0 ? 1.0f : 0.0f));
		}
	}

	TEST_CASE("events pulse for one evaluation and split the block at their frames") {
		const TestWave w = make_wave(2, 4000, -1, -1);
		Builder b;
		const uint16_t trig = b.input(1, true);
		b.p.waves.push_back(SoundWaveRef{ w.partition, "test_wave" });
		const uint32_t s = b.sampler(2, 0, SamplerLoop::None);
		b.set(s, cp::SmpTrigger, Builder::R(trig));
		auto inst = instance(b.build(), w.bank);
		ba::SamplerTrace trace;
		trace.entries.resize(16);
		inst->set_sampler_trace(&trace);
		Buses buses;
		inst->post_event(1, 37);
		inst->post_event(1, 411);
		buses.block(*inst, 1024);
		CHECK(inst->stats().evaluations == 4);
		CHECK(inst->value(trig) == 1.0f);
		buses.block(*inst, 1536);
		CHECK(inst->value(trig) == 0.0f);
		const std::vector<SamplerTraceEntry> e = entries(trace);
		REQUIRE(e.size() >= 2);
		CHECK(e[0].frame == 1024 + 37);
		CHECK(e[1].frame == 1024 + 411);
		CHECK(e[0].slot != e[1].slot);
	}

	TEST_CASE("trigger inputs fire on a rising edge: a Condition held true by a drawn number starts its sampler once") {
		const TestWave w = make_wave(1, 4000, -1, -1);
		Builder b;
		const uint16_t trig = b.input(1, true);
		const uint16_t drawn = b.reg(), held = b.reg();
		b.p.waves.push_back(SoundWaveRef{ w.partition, "test_wave" });
		uint32_t n = b.node(SoundNodeType::NumberGenerator);
		b.setf(n, cp::NumMin, 0.9f);
		b.setf(n, cp::NumMax, 0.9f);
		b.set(n, cp::NumTrigger, Builder::R(trig));
		b.set(n, cp::NumY, Builder::R(drawn));
		const uint32_t c = b.child(SoundNodeType::ConditionGroup, uint8_t(ConditionOp::GreaterOrEqual));
		b.cset(c, cp::CondX, Builder::R(drawn));
		b.cset(c, cp::CondY, Builder::C(0.6f));
		b.cset(c, cp::CondTrue, Builder::R(held));
		b.node(SoundNodeType::Condition, 0, 0, { c });
		const uint32_t s = b.sampler(2, 0, SamplerLoop::None);
		b.set(s, cp::SmpTrigger, Builder::R(held));
		auto inst = instance(b.build(), w.bank);
		Buses buses;
		inst->post_event(1, 0);
		for (uint64_t f = 0; f < 40 * 512; f += 512) {
			buses.block(*inst, f);
		}
		CHECK(inst->value(held) == 1.0f);
		CHECK(inst->stats().sampler_triggers == 1);
		CHECK(inst->stats().held_triggers == 0);
		CHECK(inst->finished());
	}

	TEST_CASE("Sampler: Release without a tail ends with the playing segment; LtNone stops after its segment") {
		const TestWave w = make_wave(3, 1000, 1, 2);
		for (SamplerLoop loop : { SamplerLoop::Release, SamplerLoop::None }) {
			Builder b;
			const uint16_t trig = b.input(1, true), rel = b.input(3, true);
			b.p.waves.push_back(SoundWaveRef{ w.partition, "test_wave" });
			const uint32_t s = b.sampler(1, 0, loop);
			b.set(s, cp::SmpTrigger, Builder::R(trig));
			b.set(s, cp::SmpRelease, Builder::R(rel));
			auto inst = instance(b.build(), w.bank);
			ba::SamplerTrace trace;
			trace.entries.resize(32);
			inst->set_sampler_trace(&trace);
			Buses buses;
			inst->post_event(1, 0);
			buses.block(*inst, 0);
			buses.block(*inst, 512);
			inst->post_event(3, 100);
			for (uint64_t f = 1024; f < 8192; f += 512) {
				buses.block(*inst, f);
			}
			const std::vector<SamplerTraceEntry> e = entries(trace);
			REQUIRE(e.size() == 3);
			CHECK(e[0].kind == SamplerTraceKind::Trigger);
			CHECK(e[1].segment == 1);
			CHECK(e[1].frame == 1000);
			CHECK(e[2].kind == SamplerTraceKind::End);
			CHECK(e[2].frame == 2000);
			CHECK(inst->finished());
		}
	}

	TEST_CASE("node order: register dependencies, stable ties, a cycle is one feedback edge, hostile input") {
		using Regs = std::vector<std::vector<uint16_t>>;
		const auto order = [](const Regs &p_reads, const Regs &p_writes, uint32_t p_regs, size_t &r_feedback, size_t p_budget = 1000) {
			bf3::core::gameplay::WorkBudget budget(p_budget);
			return order_by_register_dependencies(p_reads, p_writes, p_regs, budget, r_feedback);
		};
		size_t fb = 0;

		Result<std::vector<uint32_t>> r = order({ { 2 }, { 3 }, {} }, { { 1 }, { 2 }, { 3 } }, 4, fb);
		REQUIRE(r.ok());
		CHECK(*r == std::vector<uint32_t>{ 2, 1, 0 });
		CHECK(fb == 0);

		fb = 0;
		r = order({ { 1 }, {}, { 5 } }, { { 1 }, { 4 }, { 5 } }, 6, fb);
		REQUIRE(r.ok());
		CHECK(*r == std::vector<uint32_t>{ 0, 1, 2 });
		CHECK(fb == 0);

		fb = 0;
		r = order({ { 1 }, { 2 }, { 1, 2 } }, { { 2 }, { 1 }, {} }, 3, fb);
		REQUIRE(r.ok());
		CHECK(*r == std::vector<uint32_t>{ 0, 1, 2 });
		CHECK(fb == 1);

		fb = 0;
		r = order({ { 3 }, { 1 }, { 2 } }, { { 1 }, { 2 }, { 3 } }, 4, fb);
		REQUIRE(r.ok());
		CHECK(*r == std::vector<uint32_t>{ 0, 1, 2 });
		CHECK(fb == 1);

		fb = 0;
		CHECK_FALSE(order({ { 9 } }, { {} }, 4, fb).ok());
		CHECK_FALSE(order({ {} }, { { 9 } }, 4, fb).ok());
		CHECK_FALSE(order({ {}, {} }, { {} }, 4, fb).ok());
		Regs reads(200, std::vector<uint16_t>{ 1 }), writes(200, std::vector<uint16_t>{ 1 });
		CHECK_FALSE(order(reads, writes, 2, fb, 1000).ok());
	}
	TEST_CASE("program blob: round trip, corrupted bytes, connection misreads") {
		const TestWave w = make_wave(5, 1000, 1, 3);
		Builder b;
		const uint16_t trig = b.input(1, true);
		b.p.waves.push_back(SoundWaveRef{ w.partition, "test_wave" });
		const uint32_t s = b.sampler(2, 0, SamplerLoop::Release, kFlagShuffleSegments);
		b.set(s, cp::SmpTrigger, Builder::R(trig));
		const uint32_t sc = b.node(SoundNodeType::ScaleClamp);
		b.set(sc, cp::ScIn, Builder::R(trig));
		b.set(sc, cp::ScOut, Builder::R(b.reg()));
		const std::shared_ptr<const SoundProgram> prog = b.build();
		const std::vector<uint8_t> blob = encode_sound_program(*prog);
		Result<SoundProgram> back = decode_sound_program(blob);
		REQUIRE(back.ok());
		CHECK(encode_sound_program(*back) == blob);

		std::mt19937 rng(9);
		size_t refused = 0, accepted = 0;
		for (size_t i = 0; i < blob.size(); ++i) {
			std::vector<uint8_t> bad = blob;
			bad[i] ^= uint8_t(1u << (rng() % 8));
			Result<SoundProgram> d = decode_sound_program(bad);
			if (!d.ok()) {
				++refused;
				continue;
			}
			++accepted;
			REQUIRE(validate_sound_program(*d).ok());
			Result<std::unique_ptr<ba::PatchInstance>> inst =
					ba::PatchInstance::create(std::make_shared<SoundProgram>(std::move(*d)), w.bank, ba::AudioTuning{}, 1, 48000);
			if (inst.ok()) {
				Buses buses;
				(*inst)->post_event(1, 0);
				buses.block(**inst, 0);
				buses.block(**inst, 512);
			}
		}
		CHECK(refused > 0);
		MESSAGE("blob " << blob.size() << " bytes: " << refused << " corruptions refused, " << accepted << " decoded to valid programs");
		for (size_t cut = 0; cut < blob.size(); cut += 7) {
			CHECK_FALSE(decode_sound_program(std::span<const uint8_t>(blob.data(), cut)).ok());
		}
		std::vector<uint8_t> trailing = blob;
		trailing.push_back(0);
		CHECK_FALSE(decode_sound_program(trailing).ok());

		SoundProgram p = *prog;
		const auto sen0 = [&](size_t k) -> SoundVoicePlugin & {
			size_t seen = 0;
			for (SoundVoicePlugin &pl : p.plugins) {
				if (pl.connection != kNoRegister && seen++ == k) {
					return pl;
				}
			}
			return p.plugins.front();
		};
		sen0(1).connection = uint16_t(p.connections.size());
		CHECK_FALSE(validate_sound_program(p).ok());
		p = *prog;
		sen0(1).connection = sen0(0).connection;
		CHECK_FALSE(validate_sound_program(p).ok());
		p = *prog;
		p.connections.push_back(SoundConnection{ true, 0, MasterBus::GameMixMain });
		CHECK_FALSE(validate_sound_program(p).ok());
		p = *prog;
		p.ports[0].reg = uint16_t(p.registers);
		CHECK_FALSE(validate_sound_program(p).ok());
		CHECK_FALSE(ba::PatchInstance::create(std::make_shared<SoundProgram>(p), w.bank, ba::AudioTuning{}, 1, 48000).ok());
	}
}

TEST_SUITE("audio-dsp") {

	double magnitude_db(const ba::BiquadCoefficients &c, double p_hz, double p_rate) {
		const double w = 2.0 * kPi * p_hz / p_rate;
		const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
		const std::complex<double> h = (double(c.b0) + double(c.b1) * z1 + double(c.b2) * z2) / (1.0 + double(c.a1) * z1 + double(c.a2) * z2);
		return 20.0 * std::log10(std::abs(h));
	}
	double measured_db(ba::BiquadShape p_shape, double p_fc, double p_hz, double p_q, double p_gain) {
		const double rate = 48000.0;
		const ba::BiquadCoefficients c = ba::BiquadCoefficients::design(p_shape, float(rate), float(p_fc), float(p_q), float(p_gain));
		ba::Biquad bq;
		bq.c = c;
		std::vector<float> x(48000);
		for (size_t i = 0; i < x.size(); ++i) {
			x[i] = float(0.5 * std::sin(2.0 * kPi * p_hz * double(i) / rate));
		}
		bq.process(x.data(), uint32_t(x.size()), c);
		double peak = 0.0;
		for (size_t i = 24000; i < x.size(); ++i) {
			peak = std::max(peak, double(std::fabs(x[i])));
		}
		return 20.0 * std::log10(peak / 0.5);
	}

	TEST_CASE("biquad magnitudes at fc, fc/2 and 2 fc within 0.1 dB of the analytic response") {
		for (ba::BiquadShape shape : { ba::BiquadShape::LowPass, ba::BiquadShape::HighPass, ba::BiquadShape::BandPass, ba::BiquadShape::Peaking }) {
			const double fc = 1000.0, q = 0.70710678, gain = 6.0;
			const ba::BiquadCoefficients c = ba::BiquadCoefficients::design(shape, 48000.0f, float(fc), float(q), float(gain));
			for (double hz : { fc, fc / 2, fc * 2 }) {
				CHECK(std::fabs(measured_db(shape, fc, hz, q, gain) - magnitude_db(c, hz, 48000.0)) < 0.1);
			}
		}

		CHECK(magnitude_db(ba::BiquadCoefficients::design(ba::BiquadShape::LowPass, 48000, 1000, 0.70710678f, 0), 1000, 48000) == doctest::Approx(-3.0103).epsilon(0.003));
		CHECK(magnitude_db(ba::BiquadCoefficients::design(ba::BiquadShape::HighPass, 48000, 1000, 0.70710678f, 0), 1000, 48000) == doctest::Approx(-3.0103).epsilon(0.003));
		CHECK(magnitude_db(ba::BiquadCoefficients::design(ba::BiquadShape::Peaking, 48000, 1000, 1.0f, 6), 1000, 48000) == doctest::Approx(6.0).epsilon(0.003));
		CHECK(std::fabs(magnitude_db(ba::BiquadCoefficients::design(ba::BiquadShape::BandPass, 48000, 1000, 2.0f, 0), 1000, 48000)) < 0.01);
	}
	TEST_CASE("a 0 Hz high-pass is the bit-exact identity; coefficient ramps are continuous") {
		std::mt19937 rng(1);
		std::uniform_real_distribution<float> u(-1.0f, 1.0f);
		std::vector<float> x(4096), y;
		for (float &v : x) {
			v = u(rng);
		}
		y = x;
		ba::Biquad bq;
		const ba::BiquadCoefficients zero = ba::BiquadCoefficients::design(ba::BiquadShape::HighPass, 48000, 0, 0.707f, 0);
		bq.c = zero;
		for (size_t i = 0; i < y.size(); i += 512) {
			bq.process(y.data() + i, 512, zero);
		}
		CHECK(std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0);

		std::vector<float> s(512 * 94);
		for (size_t i = 0; i < s.size(); ++i) {
			s[i] = float(std::sin(2.0 * kPi * 3000.0 * double(i) / 48000.0));
		}
		ba::Biquad r;
		r.c = ba::BiquadCoefficients::design(ba::BiquadShape::HighPass, 48000, 100, 0.707f, 0);
		for (size_t i = 0; i < s.size(); i += 512) {
			const float f = 100.0f + 1900.0f * float(i) / float(s.size());
			r.process(s.data() + i, 512, ba::BiquadCoefficients::design(ba::BiquadShape::HighPass, 48000, f, 0.707f, 0));
		}
		float worst = 0.0f;
		for (size_t i = 1; i < s.size(); ++i) {
			worst = std::max(worst, std::fabs(s[i] - s[i - 1]));
		}
		CHECK(worst < 0.45f);
	}

	TEST_CASE("a high-pass whose cutoff moves near 10 Hz block after block stays stable, single and paired") {
		const std::pair<float, float> cutoffs[] = { { 10.174751f, 7.6397793f }, { 10.484704f, 9.2802942f },
			{ 10.5f, 10.0f } };
		for (const auto &[fa, fb] : cutoffs) {
			const ba::BiquadCoefficients a =
					ba::BiquadCoefficients::design(ba::BiquadShape::HighPass, 48000, fa, 0.70710678f, 0);
			const ba::BiquadCoefficients b =
					ba::BiquadCoefficients::design(ba::BiquadShape::HighPass, 48000, fb, 0.70710678f, 0);
			ba::Biquad single;
			ba::Biquad pair[2];
			single.c = pair[0].c = pair[1].c = a;
			float peak_single = 0.0f, peak_pair = 0.0f;
			std::vector<float> x(512), l(512), r(512);
			for (int k = 0; k < 60; ++k) {
				for (uint32_t i = 0; i < 512; ++i) {
					x[i] = (i / 64) % 2 == 0 ? 0.5f : -0.5f;
				}
				l = x;
				r = x;
				const ba::BiquadCoefficients &target = k % 2 == 0 ? b : a;
				single.process(x.data(), 512, target);
				ba::process_biquad_pair(pair, l.data(), r.data(), 512, target);
				for (uint32_t i = 0; i < 512; ++i) {
					peak_single = std::max(peak_single, std::fabs(x[i]));
					peak_pair = std::max(peak_pair, std::max(std::fabs(l[i]), std::fabs(r[i])));
				}
			}
			MESSAGE("high-pass " << fa << " / " << fb << " Hz: peak " << peak_single << " single, " << peak_pair
								 << " paired");
			CHECK(peak_single < 1.0f);
			CHECK(peak_pair < 1.0f);
		}
	}
	TEST_CASE("one-pole cutoff, gain ramp, pan law") {
		ba::OnePoleLowPass lp;
		lp.set_cutoff(48000, 1000);
		std::vector<float> x(48000);
		for (size_t i = 0; i < x.size(); ++i) {
			x[i] = float(std::sin(2.0 * kPi * 1000.0 * double(i) / 48000.0));
		}
		lp.process(x.data(), uint32_t(x.size()));
		float peak = 0.0f;
		for (size_t i = 24000; i < x.size(); ++i) {
			peak = std::max(peak, std::fabs(x[i]));
		}
		CHECK(20.0 * std::log10(peak) == doctest::Approx(-3.0).epsilon(0.1));
		std::vector<float> g(4, 1.0f);
		ba::apply_gain_ramp(g.data(), 4, 0.0f, 1.0f);
		CHECK(g[0] == doctest::Approx(0.25));
		CHECK(g[3] == doctest::Approx(1.0));
		const ba::StereoGains centre = ba::equal_power_pan(0.0f, 0.0f);
		CHECK(centre.left == doctest::Approx(0.70710678));
		CHECK(centre.right == doctest::Approx(0.70710678));
		const ba::StereoGains right = ba::equal_power_pan(float(kPi / 2), 0.0f);
		CHECK(right.left == doctest::Approx(0.0).epsilon(1e-6));
		CHECK(right.right == doctest::Approx(1.0));
		const ba::StereoGains left = ba::equal_power_pan(float(-kPi / 2), 0.0f);
		CHECK(left.left == doctest::Approx(1.0));
		CHECK(left.right == doctest::Approx(0.0).epsilon(1e-6));
		const ba::StereoGains narrow = ba::equal_power_pan(float(kPi / 2), 1.0f);
		CHECK(narrow.left == doctest::Approx(narrow.right));
		CHECK(ba::db_to_gain(-6.0206f) == doctest::Approx(0.5).epsilon(1e-4));
		CHECK(ba::gain_to_db(0.0f) == -144.0f);
	}
	TEST_CASE("onsets: a 75 ms click train over a noise bed 20 dB down gives exactly the clicks, each within 2 frames") {
		std::mt19937 rng(4);
		std::normal_distribution<float> noise(0.0f, 0.05f);
		std::vector<float> x(48000 * 2);
		for (float &v : x) {
			v = noise(rng);
		}
		std::vector<uint64_t> clicks;
		for (uint64_t c = 4800; c + 2000 < x.size(); c += 3600) {
			clicks.push_back(c);
			for (int i = 0; i < 400; ++i) {
				x[c + i] += float(0.5 * std::exp(-double(i) / 80.0) * (i % 2 ? -1 : 1));
			}
		}
		const std::vector<uint64_t> on = ba::detect_onsets(x, 1, 48000, ba::OnsetOptions{});
		REQUIRE(on.size() == clicks.size());
		for (size_t i = 0; i < on.size(); ++i) {
			CHECK(std::llabs(int64_t(on[i]) - int64_t(clicks[i])) <= 2);
		}
		const ba::OnsetSpacing sp = ba::onset_spacing(on, ba::BurstWindow{ 0, x.size() }, 3600.0);
		CHECK(sp.count == clicks.size());
		CHECK(sp.max_deviation_frames <= 4.0);
	}
	TEST_CASE("band energy of sines and the high-pass prediction on white noise") {
		std::vector<float> lo(48000), hi(48000);
		for (size_t i = 0; i < lo.size(); ++i) {
			lo[i] = float(std::sin(2.0 * kPi * 60.0 * double(i) / 48000.0));
			hi[i] = float(std::sin(2.0 * kPi * 1000.0 * double(i) / 48000.0));
		}
		const float in_band = ba::band_energy_db(lo, 1, 48000, 0, lo.size(), 20, 100);
		const float out_band = ba::band_energy_db(hi, 1, 48000, 0, hi.size(), 20, 100);
		CHECK(in_band - out_band > 60.0f);
		CHECK(ba::rms_db(lo, 1, 0, lo.size()) == doctest::Approx(-3.0103).epsilon(0.01));
		CHECK(ba::peak_db(lo, 1, 0, lo.size()) == doctest::Approx(0.0).epsilon(0.01));
		std::mt19937 rng(8);
		std::normal_distribution<float> n(0.0f, 0.1f);
		std::vector<float> white(48000 * 4);
		for (float &v : white) {
			v = n(rng);
		}
		std::vector<float> filtered = white;
		ba::Biquad bq;
		const ba::BiquadCoefficients c = ba::BiquadCoefficients::design(ba::BiquadShape::HighPass, 48000, 225.3f, 0.70710678f, 0);
		bq.c = c;
		bq.process(filtered.data(), uint32_t(filtered.size()), c);
		const float measured = ba::band_energy_db(filtered, 1, 48000, 8192, white.size() - 8192, 20, 100);
		const float predicted = ba::predicted_highpass_band_db(white, 1, 48000, 8192, white.size() - 8192, 20, 100, 225.3f, 0.70710678f);
		CHECK(std::fabs(measured - predicted) < 0.5f);
	}
}

TEST_SUITE("audio-mixer") {
	struct CountingTap final : ba::MixTap {
		uint64_t frames = 0, next = 0;
		bool contiguous = true;
		void on_block(const float *, uint32_t p_frames, uint64_t p_first) override {
			contiguous = contiguous && p_first == next;
			next = p_first + p_frames;
			frames += p_frames;
		}
	};

	std::shared_ptr<const SoundProgram> one_shot_program(const TestWave &w, uint32_t p_slots = 2) {
		Builder b;
		const uint16_t trig = b.input(1, true);
		b.input(2, true);
		b.p.start_event = 1;
		b.p.stop_event = 2;
		b.p.waves.push_back(SoundWaveRef{ w.partition, "test_wave" });
		const uint32_t s = b.sampler(p_slots, 0, SamplerLoop::None);
		b.set(s, cp::SmpTrigger, Builder::R(trig));
		return b.build();
	}

	TEST_CASE("silence when idle; the tap sees every frame once; earliest_frame") {
		ba::MixerConfig cfg;
		ba::MixerCore mixer(cfg);
		CountingTap tap;
		mixer.set_tap(&tap);
		std::vector<float> buf(2 * 1000, 1.0f);
		CHECK(mixer.mix(buf.data(), 1000) == 1000);
		CHECK(std::all_of(buf.begin(), buf.end(), [](float v) { return v == 0.0f; }));
		CHECK(mixer.clock() == 1024);
		CHECK(mixer.earliest_frame() == 1024 + 2 * 512);
		CHECK(tap.contiguous);
		CHECK(tap.frames == 1024);
		CHECK(mixer.stats().short_blocks.load() == 0);
	}
	TEST_CASE("frame placement, late commands, ring overflow, ownership, return and reclaim, bank epoch") {
		const TestWave w = make_wave(1, 2000, -1, -1);
		const std::shared_ptr<const SoundProgram> prog = one_shot_program(w);
		ba::MixerConfig cfg;
		cfg.command_capacity = 4;
		ba::MixerCore mixer(cfg);
		std::vector<float> buf(2 * 512);
		mixer.mix(buf.data(), 512);
		auto inst = instance(prog, w.bank);
		ba::SamplerTrace trace;
		trace.entries.resize(16);
		inst->set_sampler_trace(&trace);
		ba::MixerCommand start;
		start.type = ba::MixerCommandType::Start;
		start.handle = mixer.next_handle();
		start.instance = inst.get();
		start.at_frame = 1100;
		ba::MixerCommand ev;
		ev.type = ba::MixerCommandType::PostEvent;
		ev.handle = start.handle;
		ev.name_hash = 1;
		ev.at_frame = 1300;
		REQUIRE(mixer.push(start));
		(void)inst.release();
		REQUIRE(mixer.push(ev));
		ba::MixerCommand bank;
		bank.type = ba::MixerCommandType::SetBank;
		bank.epoch = 7;
		REQUIRE(mixer.push(bank));
		ba::MixerCommand late = ev;
		late.at_frame = 100;
		REQUIRE(mixer.push(late));

		auto spare = instance(prog, w.bank);
		ba::MixerCommand refused = start;
		refused.instance = spare.get();
		CHECK_FALSE(mixer.push(refused));
		CHECK(mixer.stats().ring_overflows.load() == 1);
		spare.reset();

		mixer.mix(buf.data(), 512);
		CHECK(mixer.stats().late_commands.load() == 1);
		mixer.mix(buf.data(), 512);
		CHECK(mixer.stats().acked_epoch.load() == 7);
		CHECK(mixer.stats().late_commands.load() == 1);
		REQUIRE(trace.count >= 2);
		CHECK(trace.entries[0].frame == 1100);
		CHECK(trace.entries[1].frame == 1300);

		ba::MixerCommand stop;
		stop.type = ba::MixerCommandType::Stop;
		stop.handle = start.handle;
		stop.at_frame = 2000;
		REQUIRE(mixer.push(stop));
		for (int i = 0; i < 16; ++i) {
			mixer.mix(buf.data(), 512);
			mixer.reclaim();
		}
		CHECK(mixer.stats().active_instances.load() == 0);
		std::array<ba::MixerFeedback, 8> fb{};
		const size_t n = mixer.poll_feedback(fb);
		REQUIRE(n >= 1);
		CHECK(fb[n - 1].finished);
		CHECK(fb[n - 1].handle == start.handle);
	}
	TEST_CASE("a command stamped 0 for a handle whose Start is stamped later waits for the Start") {
		const TestWave w = make_wave(1, 2000, -1, -1);
		const std::shared_ptr<const SoundProgram> prog = one_shot_program(w);
		ba::MixerCore mixer(ba::MixerConfig{});
		std::vector<float> buf(2 * 512);
		auto inst = instance(prog, w.bank);
		ba::SamplerTrace trace;
		trace.entries.resize(16);
		inst->set_sampler_trace(&trace);
		ba::MixerCommand start;
		start.type = ba::MixerCommandType::Start;
		start.handle = mixer.next_handle();
		start.instance = inst.get();
		start.at_frame = 1700;
		REQUIRE(mixer.push(start));
		(void)inst.release();
		ba::MixerCommand param;
		param.type = ba::MixerCommandType::SetParameter;
		param.handle = start.handle;
		param.name_hash = 0x1234;
		param.value = 1.0f;
		REQUIRE(mixer.push(param));
		ba::MixerCommand ev;
		ev.type = ba::MixerCommandType::PostEvent;
		ev.handle = start.handle;
		ev.name_hash = 1;
		REQUIRE(mixer.push(ev));
		for (int i = 0; i < 3; ++i) {
			mixer.mix(buf.data(), 512);
			CHECK(trace.count == 0);
			CHECK(mixer.stats().active_instances.load() == 0);
		}
		mixer.mix(buf.data(), 512);
		REQUIRE(trace.count >= 1);
		CHECK(trace.entries[0].frame == 1700);
		CHECK(mixer.stats().late_commands.load() == 0);
		CHECK(mixer.stats().active_instances.load() == 1);
	}
	TEST_CASE("mix() with odd and varying call sizes gives the samples of 512-frame calls") {
		const TestWave w = make_wave(1, 3000, -1, -1, 48000, true);
		const std::shared_ptr<const SoundProgram> prog = one_shot_program(w);
		std::vector<std::vector<float>> outs;
		for (const std::vector<uint32_t> &sizes : { std::vector<uint32_t>{ 512 }, std::vector<uint32_t>{ 37, 1000, 1, 4096, 480 } }) {
			ba::MixerCore mixer(ba::MixerConfig{});
			auto inst = instance(prog, w.bank);
			ba::MixerCommand start;
			start.type = ba::MixerCommandType::Start;
			start.handle = mixer.next_handle();
			start.instance = inst.get();
			start.at_frame = 600;
			REQUIRE(mixer.push(start));
			(void)inst.release();
			ba::MixerCommand ev;
			ev.type = ba::MixerCommandType::PostEvent;
			ev.handle = start.handle;
			ev.name_hash = 1;
			ev.at_frame = 1777;
			REQUIRE(mixer.push(ev));
			std::vector<float> all, buf(2 * 4096);
			size_t k = 0;
			while (all.size() < 2 * 8192) {
				const uint32_t n = sizes[k++ % sizes.size()];
				mixer.mix(buf.data(), n);
				all.insert(all.end(), buf.begin(), buf.begin() + 2 * n);
			}
			all.resize(2 * 8192);
			outs.push_back(all);
		}
		CHECK(outs[0] == outs[1]);
		CHECK(std::any_of(outs[0].begin(), outs[0].end(), [](float v) { return v != 0.0f; }));
	}
	TEST_CASE("a stereo wave's first channel plays on the left output channel and its second on the right") {
		for (uint32_t sounding = 0; sounding < 2; ++sounding) {
			const TestWave w = make_wave(1, 3000, -1, -1, 48000, true, 2, sounding);
			const std::shared_ptr<const SoundProgram> prog = one_shot_program(w);
			ba::MixerCore mixer(ba::MixerConfig{});
			auto inst = instance(prog, w.bank);
			ba::MixerCommand start;
			start.type = ba::MixerCommandType::Start;
			start.handle = mixer.next_handle();
			start.instance = inst.get();
			start.at_frame = 600;
			REQUIRE(mixer.push(start));
			(void)inst.release();
			ba::MixerCommand ev;
			ev.type = ba::MixerCommandType::PostEvent;
			ev.handle = start.handle;
			ev.name_hash = 1;
			ev.at_frame = 700;
			REQUIRE(mixer.push(ev));
			std::vector<float> buf(2 * 512);
			double energy[2] = { 0.0, 0.0 };
			for (int b = 0; b < 8; ++b) {
				mixer.mix(buf.data(), 512);
				for (size_t i = 0; i < 512; ++i) {
					energy[0] += double(buf[i * 2]) * double(buf[i * 2]);
					energy[1] += double(buf[i * 2 + 1]) * double(buf[i * 2 + 1]);
				}
			}
			CHECK(energy[sounding] > 100.0);
			CHECK(energy[1 - sounding] == 0.0);
		}
	}

	TEST_CASE("a bus gain scales only that bus's share of the output and reads back once applied") {
		const TestWave w = make_wave(1, 3000, -1, -1, 48000, true);
		const std::shared_ptr<const SoundProgram> prog = one_shot_program(w);
		const auto render = [&](MasterBus p_bus, float p_gain, float &r_seen) {
			ba::MixerCore mixer(ba::MixerConfig{});
			CHECK(mixer.bus_gain(p_bus) == 1.0f);
			ba::MixerCommand gain;
			gain.type = ba::MixerCommandType::SetBusGain;
			gain.name_hash = uint32_t(p_bus);
			gain.value = p_gain;
			REQUIRE(mixer.push(gain));
			ba::MixerCommand bad = gain;
			bad.name_hash = uint32_t(MasterBus::Unknown);
			REQUIRE(mixer.push(bad));
			auto inst = instance(prog, w.bank);
			ba::MixerCommand start;
			start.type = ba::MixerCommandType::Start;
			start.handle = mixer.next_handle();
			start.instance = inst.get();
			start.at_frame = 600;
			REQUIRE(mixer.push(start));
			(void)inst.release();
			ba::MixerCommand ev;
			ev.type = ba::MixerCommandType::PostEvent;
			ev.handle = start.handle;
			ev.name_hash = 1;
			ev.at_frame = 700;
			REQUIRE(mixer.push(ev));
			std::vector<float> all, buf(2 * 512);
			while (all.size() < 2 * 4096) {
				mixer.mix(buf.data(), 512);
				all.insert(all.end(), buf.begin(), buf.end());
			}
			r_seen = mixer.bus_gain(p_bus);
			return all;
		};
		float seen = 0.0f;
		const std::vector<float> full = render(MasterBus::GameMixMain, 1.0f, seen);
		const std::vector<float> half = render(MasterBus::GameMixMain, 0.5f, seen);
		CHECK(seen == 0.5f);
		const std::vector<float> muted = render(MasterBus::GameMixMain, 0.0f, seen);
		const std::vector<float> other = render(MasterBus::Music, 0.0f, seen);
		CHECK(seen == 0.0f);
		REQUIRE(full.size() == half.size());
		float peak = 0.0f;
		for (size_t i = 0; i < full.size(); ++i) {
			peak = std::max(peak, std::fabs(full[i]));
			CHECK(half[i] == doctest::Approx(full[i] * 0.5f).epsilon(1e-6));
		}
		CHECK(peak > 0.1f);
		CHECK(std::all_of(muted.begin(), muted.end(), [](float v) { return v == 0.0f; }));
		CHECK(other == full);
	}

	TEST_CASE("a Start refused with every instance slot full yields one finished feedback") {
		const TestWave w = make_wave(1, 48000, -1, -1, 48000, true);
		const std::shared_ptr<const SoundProgram> prog = one_shot_program(w);
		ba::MixerConfig cfg;
		cfg.max_instances = 1;
		ba::MixerCore mixer(cfg);
		ba::OneShotPool pool(8);
		ba::EmitterState emitter;
		ba::ListenerState listener;
		REQUIRE(pool.start(mixer, prog, w.bank, ba::AudioTuning{}, 1, emitter, listener, 600));
		const ba::InstanceHandle sounding = pool.last_handle();
		REQUIRE(pool.start(mixer, prog, w.bank, ba::AudioTuning{}, 2, emitter, listener, 700));
		const ba::InstanceHandle refused = pool.last_handle();
		CHECK(pool.live() == 2);
		std::vector<float> buf(2 * 512);
		std::vector<ba::MixerFeedback> feedback;
		for (int b = 0; b < 4; ++b) {
			mixer.mix(buf.data(), 512);
			mixer.reclaim();
			std::array<ba::MixerFeedback, 16> fb{};
			const size_t n = mixer.poll_feedback(fb);
			for (size_t i = 0; i < n; ++i) {
				feedback.push_back(fb[i]);
				if (fb[i].finished) {
					pool.on_finished(fb[i].handle);
				}
			}
		}
		CHECK(mixer.stats().starved_voices.load() == 1);
		CHECK(std::count_if(feedback.begin(), feedback.end(),
					  [&](const ba::MixerFeedback &f) { return f.handle == refused && f.finished; }) == 1);
		CHECK(std::none_of(feedback.begin(), feedback.end(),
				[&](const ba::MixerFeedback &f) { return f.handle == sounding && f.finished; }));
		CHECK(pool.live() == 1);
		CHECK(pool.contains(sounding));
		CHECK_FALSE(pool.contains(refused));
		CHECK(mixer.stats().active_instances.load() == 1);
	}
}
