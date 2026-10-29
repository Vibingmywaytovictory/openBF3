#include "bf3/core/level/source_digest.h"

#include "bf3/core/bundle_manifest.h"
#include "bf3/core/ids.h"
#include "bf3/core/level/level.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/thread_pool.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bf3::core::level {

namespace {

constexpr const char *kDigestTag = "openbf3-source-digest-v1";

bool is_zero(const Sha1 &p_sha1) {
	return std::all_of(p_sha1.bytes.begin(), p_sha1.bytes.end(), [](uint8_t p_b) { return p_b == 0; });
}

class Canon {
public:
	void u8(uint8_t p_v) { bytes_.push_back(p_v); }
	void u32(uint32_t p_v) {
		for (int i = 0; i < 4; ++i) {
			bytes_.push_back(uint8_t(p_v >> (8 * i)));
		}
	}
	void u64(uint64_t p_v) {
		for (int i = 0; i < 8; ++i) {
			bytes_.push_back(uint8_t(p_v >> (8 * i)));
		}
	}
	void str(std::string_view p_text) {
		u64(p_text.size());
		bytes_.insert(bytes_.end(), p_text.begin(), p_text.end());
	}
	template <size_t N>
	void raw(const std::array<uint8_t, N> &p_bytes) {
		bytes_.insert(bytes_.end(), p_bytes.begin(), p_bytes.end());
	}

	bool payload(const Sha1 &p_sha1, const Payload &p_payload) {
		if (!is_zero(p_sha1)) {
			u8(1);
			raw(p_sha1.bytes);
			return true;
		}
		if (p_payload.kind == Payload::Kind::Inline && p_payload.inline_data != nullptr) {
			u8(2);
			const std::vector<uint8_t> &data = *p_payload.inline_data;
			u64(data.size());
			bytes_.insert(bytes_.end(), data.begin(), data.end());
			return true;
		}
		return false;
	}

	std::vector<uint8_t> take() { return std::move(bytes_); }

private:
	std::vector<uint8_t> bytes_;
};

struct BundleDigest {
	std::string key;
	Sha256Digest digest;
	size_t entries = 0;
	bool usable = true;
};

Result<BundleDigest> digest_bundle(std::string p_key, const BundleManifest &p_manifest) {
	BundleDigest out;
	out.key = std::move(p_key);
	Hasher hasher(HashAlgorithm::Sha256);
	const auto feed = [&](std::vector<std::vector<uint8_t>> &r_records, const char *p_kind) {
		std::sort(r_records.begin(), r_records.end());
		Canon c;
		c.str(p_kind);
		c.u64(r_records.size());
		hasher.update(c.take());
		for (const std::vector<uint8_t> &record : r_records) {
			hasher.update(record);
		}
		out.entries += r_records.size();
		r_records = {};
	};

	std::vector<std::vector<uint8_t>> records;
	records.reserve(p_manifest.ebx.size());
	for (const EbxEntry &e : p_manifest.ebx) {
		Canon c;
		c.str(asset_key(e.name));
		out.usable = c.payload(e.sha1, e.payload) && out.usable;
		records.push_back(c.take());
	}
	feed(records, "ebx");
	records.reserve(p_manifest.res.size());
	for (const ResEntry &e : p_manifest.res) {
		Canon c;
		c.str(asset_key(e.name));
		c.u32(e.res_type);
		c.raw(e.res_meta);
		out.usable = c.payload(e.sha1, e.payload) && out.usable;
		records.push_back(c.take());
	}
	feed(records, "res");
	records.reserve(p_manifest.chunks.size());
	for (const ChunkEntry &e : p_manifest.chunks) {
		Canon c;
		c.raw(e.id.bytes);
		out.usable = c.payload(e.sha1, e.payload) && out.usable;
		if (e.range) {
			c.u8(1);
			c.u32(e.range->start);
			c.u32(e.range->end);
			c.u32(e.range->logical_offset);
		} else {
			c.u8(0);
		}
		c.u8(e.tail_only ? 1 : 0);
		records.push_back(c.take());
	}
	feed(records, "chunk");
	if (!hasher.finish(out.digest.bytes)) {
		return make_error("source digest: SHA-256 failed for bundle " + out.key);
	}
	return out;
}

Result<SourceDigest> digest_bundles(
		const LevelMount &p_mount, std::vector<size_t> p_at, std::chrono::steady_clock::time_point p_start) {
	std::vector<size_t> &at = p_at;
	std::sort(at.begin(), at.end());
	at.erase(std::unique(at.begin(), at.end()), at.end());

	std::vector<Result<BundleDigest>> bundles =
			parallel_map<Result<BundleDigest>>(at.size(), [&](size_t i) -> Result<BundleDigest> {
				Result<BundleManifest> manifest = p_mount.read_manifest(at[i]);
				if (!manifest) {
					return error_at("source digest: bundle " + p_mount.bundle_name(at[i]), manifest.error());
				}
				return digest_bundle(bundle_key(p_mount.bundle_name(at[i])), *manifest);
			});

	SourceDigest out;
	std::vector<std::pair<std::string, Sha256Digest>> sorted;
	bool usable = true;
	for (Result<BundleDigest> &b : bundles) {
		if (!b) {
			return b.error();
		}
		out.entries += b->entries;
		usable = usable && b->usable;
		sorted.emplace_back(std::move(b->key), b->digest);
	}
	std::sort(sorted.begin(), sorted.end());
	out.bundles = sorted.size();
	if (usable) {
		Canon c;
		c.str(kDigestTag);
		c.u64(sorted.size());
		for (const auto &[key, digest] : sorted) {
			c.str(key);
			c.raw(digest.bytes);
		}
		Hasher hasher(HashAlgorithm::Sha256);
		hasher.update(c.take());
		Sha256Digest digest;
		if (!hasher.finish(digest.bytes)) {
			return make_error("source digest: SHA-256 failed");
		}
		out.digest = digest;
	}
	out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
	return out;
}
}

Result<SourceDigest> level_source_digest(const LoadedLevel &p_level) {
	const auto start = std::chrono::steady_clock::now();
	const LevelMount &mount = p_level.mount();
	std::unordered_map<std::string, size_t> index;
	for (size_t i = 0; i < mount.bundle_count(); ++i) {
		index.emplace(mount.bundle_name(i), i);
	}
	std::vector<std::string> names;
	names.push_back(p_level.bundles.level);
	names.insert(names.end(), p_level.bundles.settings.begin(), p_level.bundles.settings.end());
	names.insert(names.end(), p_level.bundles.subworlds.begin(), p_level.bundles.subworlds.end());
	std::vector<size_t> at;
	for (const std::string &name : names) {
		const auto it = index.find(name);
		if (it == index.end()) {
			return make_error("source digest: bundle " + name + " is not in the level's superbundle");
		}
		at.push_back(it->second);
	}
	return digest_bundles(mount, std::move(at), start);
}

Result<SourceDigest> level_mount_digest(const LoadedLevel &p_level) {
	const auto start = std::chrono::steady_clock::now();
	const LevelMount &mount = p_level.mount();
	std::vector<size_t> at(mount.bundle_count());
	for (size_t i = 0; i < at.size(); ++i) {
		at[i] = i;
	}
	return digest_bundles(mount, std::move(at), start);
}

}
