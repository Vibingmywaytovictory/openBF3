#include "bf3/core/audio/impulse_response.h"

#include "bf3/core/util/big_endian.h"
#include "bf3/core/util/fft.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bf3::core::audio {

namespace {

Error ir_error(const std::string &p_what) {
	return make_error("irf1: " + p_what);
}

bool is_power_of_two(uint32_t p_v) {
	return p_v != 0 && (p_v & (p_v - 1)) == 0;
}

}

size_t ImpulseResponseHeader::block_bytes() const {
	return size_t(channels) * (kIrfBlockHeaderBytes + size_t(bins) * 4);
}

size_t ImpulseResponseHeader::chunk_bytes() const {
	const size_t blocks = block == 0 ? 0 : (size_t(frames) + block - 1) / block;
	return kIrfHeaderBytes + blocks * block_bytes();
}

std::span<const float> ImpulseResponse::channel(uint32_t p_channel) const {
	if (p_channel >= channels || samples.size() < size_t(channels) * frames) {
		return {};
	}
	return std::span<const float>(samples.data() + size_t(p_channel) * frames, frames);
}

Result<ImpulseResponseHeader> read_impulse_header(std::span<const uint8_t> p_chunk) {
	if (p_chunk.size() < kIrfHeaderBytes) {
		return ir_error("a chunk of " + std::to_string(p_chunk.size()) + " bytes has no header");
	}
	if (be32(p_chunk, 0) != kIrfMagic) {
		return ir_error("no irf1 magic");
	}
	ImpulseResponseHeader h;
	h.version = be32(p_chunk, 4);
	h.channels = be32(p_chunk, 8);
	h.frames = be32(p_chunk, 12);
	h.rate = be32(p_chunk, 16);
	h.block = be32(p_chunk, 20);
	h.bins = be32(p_chunk, 24);
	h.bin_values = be32(p_chunk, 28);
	h.layout = be32(p_chunk, 32);
	if (h.version != 1 || h.channels == 0 || h.channels > kMaxImpulseChannels || h.frames == 0 ||
			h.frames > kMaxImpulseFrames || h.rate == 0) {
		return ir_error("header: version " + std::to_string(h.version) + ", " + std::to_string(h.channels) +
				" channels, " + std::to_string(h.frames) + " frames at " + std::to_string(h.rate) + " Hz");
	}
	if (!is_power_of_two(h.block) || h.bins == 0 || h.bins > h.block + 1 || h.bin_values != h.bins * 2 ||
			h.layout != 1) {
		return ir_error("block " + std::to_string(h.block) + ", " + std::to_string(h.bins) + " bins, " +
				std::to_string(h.bin_values) + " values, layout " + std::to_string(h.layout));
	}
	for (size_t at = 36; at < kIrfHeaderBytes; ++at) {
		if (p_chunk[at] != 0) {
			return ir_error("header byte " + std::to_string(at) + " is not zero");
		}
	}
	return h;
}

Result<ImpulseResponse> decode_impulse_response(std::span<const uint8_t> p_chunk) {
	Result<ImpulseResponseHeader> h = read_impulse_header(p_chunk);
	if (!h) {
		return h.error();
	}
	if (p_chunk.size() != h->chunk_bytes()) {
		return ir_error("a chunk of " + std::to_string(p_chunk.size()) + " bytes, the header gives " +
				std::to_string(h->chunk_bytes()));
	}
	const uint32_t n = h->block * 2;
	const uint32_t blocks = (h->frames + h->block - 1) / h->block;
	const Fft<float> fft(n);
	std::vector<float> re(n), im(n);
	ImpulseResponse ir;
	ir.header = *h;
	ir.channels = h->channels;
	ir.rate = h->rate;
	ir.frames = h->frames;
	ir.samples.assign(size_t(h->channels) * h->frames, 0.0f);
	const size_t channel_bytes = kIrfBlockHeaderBytes + size_t(h->bins) * 4;
	for (uint32_t k = 0; k < blocks; ++k) {
		for (uint32_t c = 0; c < h->channels; ++c) {
			const size_t at = kIrfHeaderBytes + size_t(k) * h->block_bytes() + size_t(c) * channel_bytes;
			const int16_t scale = be16s(p_chunk, at);
			if (scale <= 0 || !is_power_of_two(uint32_t(scale) + 1)) {
				return ir_error("block " + std::to_string(k) + " channel " + std::to_string(c) + " has scale " +
						std::to_string(scale));
			}
			for (size_t z = 2; z < kIrfBlockHeaderBytes; z += 2) {
				if (be16s(p_chunk, at + z) != 0) {
					return ir_error("block " + std::to_string(k) + " channel " + std::to_string(c) +
							" has a block header value at " + std::to_string(z));
				}
			}
			const float unit = 1.0f / float(uint32_t(scale) + 1);
			std::fill(re.begin(), re.end(), 0.0f);
			std::fill(im.begin(), im.end(), 0.0f);
			for (uint32_t b = 0; b < h->bins; ++b) {
				const size_t v = at + kIrfBlockHeaderBytes + size_t(b) * 4;
				re[b] = float(be16s(p_chunk, v)) * unit;
				im[b] = float(be16s(p_chunk, v + 2)) * unit;
				if (b == 0 || b == h->block) {
					im[b] = 0.0f;
				} else {
					re[n - b] = re[b];
					im[n - b] = -im[b];
				}
			}
			fft.inverse(re.data(), im.data());
			const uint32_t start = k * h->block;
			const uint32_t count = std::min(h->block, h->frames - start);
			float *out = ir.samples.data() + size_t(c) * h->frames + start;
			for (uint32_t i = 0; i < count; ++i) {
				out[i] = re[i] / float(n);
			}
		}
	}
	return ir;
}

Result<ImpulseResponse> read_impulse_response(const EbxInstance &p_asset, const LevelMount &p_mount) {
	if (!p_asset.is_a("ImpulseResponseAsset")) {
		return ir_error(std::string(p_asset.type_name()) + " is not an ImpulseResponseAsset");
	}
	const std::optional<EbxValue> chunks = p_asset.get("Chunks");
	if (!chunks || !chunks->as_array() || chunks->as_array()->size() != 1) {
		return ir_error("the asset does not have one chunk");
	}
	const std::optional<EbxValue> c = chunks->as_array()->get(0);
	const std::optional<EbxValue> id = c ? c->get("ChunkId") : std::nullopt;
	const std::optional<Guid> guid = id ? id->as_guid() : std::nullopt;
	if (!guid) {
		return ir_error("the chunk has no ChunkId");
	}
	Result<std::vector<uint8_t>> bytes = p_mount.read_chunk(*guid);
	if (!bytes) {
		return bytes.error();
	}
	Result<ImpulseResponse> ir = decode_impulse_response(*bytes);
	if (!ir) {
		return ir.error();
	}
	ir->partition = p_asset.partition().guid();
	const std::optional<EbxValue> name = p_asset.get("Name");
	ir->name = std::string(name ? name->as_str().value_or("") : std::string_view());
	return ir;
}

std::vector<LibraryImpulse> read_library_impulses(const EbxLibrary &p_library) {
	std::vector<LibraryImpulse> out;
	for (const EbxLibrary::Entry &e : p_library.entries()) {
		const EbxInstance primary = e.partition->primary();
		if (!primary.is_a("ImpulseResponseAsset")) {
			continue;
		}
		const std::optional<EbxLibrary::Origin> origin = p_library.origin(e.name);
		const LevelMount *mount = nullptr;
		for (size_t m = 0; origin && m < p_library.superbundle_count(); ++m) {
			if (p_library.superbundle(m) == origin->superbundle) {
				mount = &p_library.mount(m);
			}
		}
		out.push_back(LibraryImpulse{ std::string(e.name),
				mount != nullptr ? read_impulse_response(primary, *mount)
								 : Result<ImpulseResponse>(ir_error(std::string(e.name) + " has no mount")) });
	}
	return out;
}

}
