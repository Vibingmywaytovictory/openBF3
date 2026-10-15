// Ported from IW4L asset_transport::bf3 (mount.rs), Apache-2.0.
#include "bf3/core/superbundle_mount.h"

#include "bf3/core/delta_bundle.h"
#include "bf3/core/payload.h"
#include "bf3/core/sb_formats.h"
#include "bf3/core/thread_pool.h"
#include "format_errors.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

namespace bf3::core {

std::string asset_key(std::string_view p_name) {
	std::string key = to_lower_ascii(p_name);
	std::replace(key.begin(), key.end(), '\\', '/');
	return key;
}

std::string bundle_key(std::string_view p_name) {
	const std::string key = asset_key(p_name);
	std::string_view k = key;
	while (!k.empty() && k.front() == '/') {
		k.remove_prefix(1);
	}
	if (k.substr(0, 6) == "win32/") {
		k.remove_prefix(6);
	}
	return std::string(k);
}

namespace {

constexpr const char *kChunkSuperbundles[] = {
	"Win32/Chunks0",
	"Win32/Chunks1",
	"Win32/Chunks2",
	"Win32/MpChunks",
	"Win32/SpChunks",
};

enum class BundleFormat { Cas, Binary };

struct BundleBytes {
	bool is_delta = false;
	// File
	std::shared_ptr<InstallFile> file;
	uint64_t offset = 0;
	uint64_t len = 0;
	// Delta
	DeltaBundle delta;
	std::shared_ptr<InstallFile> patch;
	std::shared_ptr<InstallFile> base;

	uint64_t size() const { return is_delta ? delta.len() : len; }
	const std::string &path() const { return is_delta ? patch->name() : file->name(); }

	Result<std::vector<uint8_t>> read_vec(uint64_t p_offset, uint64_t p_len) const {
		if (p_offset + p_len < p_offset || p_offset + p_len > size()) {
			return make_error(path() + ": read of " + std::to_string(p_len) + " bytes at " + std::to_string(p_offset) +
					" runs past the end (" + std::to_string(size()) + " bytes)");
		}
		if (!is_delta) {
			return file->read_vec(offset + p_offset, p_len);
		}
		if (p_len > detail::kMaxMetadataSizedAlloc) {
			return error_at(patch->name(), detail::out_of_memory(p_len)); // run lengths are unchecked metadata
		}
		std::vector<uint8_t> buf(static_cast<size_t>(p_len));
		Status st = delta.read_at(*patch, *base, p_offset, buf);
		if (!st) {
			return error_at(patch->name(), st.error());
		}
		return buf;
	}
};

struct BundleSite {
	std::string id;
	BundleFormat format = BundleFormat::Cas;
	BundleBytes bytes;
};

struct Stored {
	Sha1 sha1;
	uint64_t size = 0;
	Payload::Kind kind = Payload::Kind::Cas;
	size_t bundle = 0;
	uint64_t offset = 0;
	std::shared_ptr<const std::vector<uint8_t>> inline_data;
};

struct EbxRecord {
	std::string name;
	Stored stored;
	uint64_t original_size = 0;
};

struct ResRecord {
	std::string name;
	Stored stored;
	uint64_t original_size = 0;
	ResInfo info;
};

struct ChunkRecord {
	Stored stored;
	std::optional<ChunkRange> range;
	bool tail_only = false;
	std::optional<ChunkMeta> meta;
};

struct Index {
	std::vector<std::optional<BundleSummary>> loaded;
	std::vector<size_t> load_order;
	std::vector<EbxRecord> ebx;
	std::unordered_map<std::string, size_t> ebx_keys;
	std::vector<ResRecord> res;
	std::unordered_map<std::string, size_t> res_keys;
	std::unordered_map<Guid, ChunkRecord, GuidHash> chunks;
	std::vector<Guid> chunk_order;
	std::unordered_map<uint32_t, ChunkMeta> asset_chunk_meta;

	static Stored stored_of(size_t p_bundle, const Sha1 &p_sha1, uint64_t p_size, Payload &p_payload) {
		Stored s;
		s.sha1 = p_sha1;
		s.size = p_size;
		s.kind = p_payload.kind;
		s.bundle = p_bundle;
		s.offset = p_payload.offset;
		s.inline_data = std::move(p_payload.inline_data);
		return s;
	}

	void register_bundle(size_t p_bundle, const std::string &p_name, BundleManifest &&p_manifest) {
		if (loaded[p_bundle].has_value()) {
			return;
		}
		BundleSummary summary{ p_name, p_manifest.ebx.size(), p_manifest.res.size(), p_manifest.chunks.size() };
		for (EbxEntry &e : p_manifest.ebx) {
			const auto [it, fresh] = ebx_keys.try_emplace(asset_key(e.name), ebx.size());
			if (fresh) {
				ebx.push_back(EbxRecord{ std::move(e.name), stored_of(p_bundle, e.sha1, e.stored_size, e.payload), e.original_size });
			}
		}
		for (ResEntry &e : p_manifest.res) {
			const auto [it, fresh] = res_keys.try_emplace(asset_key(e.name), res.size());
			if (fresh) {
				res.push_back(ResRecord{ std::move(e.name), stored_of(p_bundle, e.sha1, e.stored_size, e.payload), e.original_size,
						ResInfo{ e.res_type, e.res_meta } });
			}
		}
		for (ChunkEntry &e : p_manifest.chunks) {
			ChunkRecord record{ stored_of(p_bundle, e.sha1, e.stored_size, e.payload), e.range, e.tail_only, e.meta };
			const auto it = chunks.find(e.id);
			if (it == chunks.end()) {
				chunks.emplace(e.id, std::move(record));
				chunk_order.push_back(e.id);
			} else if (it->second.tail_only && !record.tail_only) {
				it->second = std::move(record);
			}
		}
		for (const ChunkMeta &m : p_manifest.chunk_meta) {
			asset_chunk_meta.try_emplace(m.h32, m);
		}
		loaded[p_bundle] = std::move(summary);
		load_order.push_back(p_bundle);
	}
};

Error layout_at(const std::string &p_where, const char *p_what, const char *p_problem) {
	return error_at(p_where, detail::layout_error(p_what, p_problem));
}

Error size_mismatch(const std::string &p_what, uint64_t p_expected, uint64_t p_got) {
	return make_error(p_what + ": expected " + std::to_string(p_expected) + " bytes, got " + std::to_string(p_got));
}

Result<std::vector<BundleSite>> superbundle_bundles(const Install &p_install, const SuperbundleSite &p_site) {
	struct Opened {
		Toc toc;
		std::shared_ptr<InstallFile> file;
	};
	auto open = [&p_install](const std::string &p_toc_path) -> Result<Opened> {
		Result<Toc> toc = p_install.read_toc(p_toc_path);
		if (!toc) {
			return toc.error();
		}
		Result<std::shared_ptr<InstallFile>> file = p_install.file(Install::superbundle_file(p_toc_path));
		if (!file) {
			return file.error();
		}
		return Opened{ std::move(*toc), std::move(*file) };
	};
	std::optional<Opened> base, patch;
	if (p_site.base_toc) {
		Result<Opened> o = open(*p_site.base_toc);
		if (!o) {
			return o.error();
		}
		base = std::move(*o);
	}
	if (p_site.patch_toc) {
		Result<Opened> o = open(*p_site.patch_toc);
		if (!o) {
			return o.error();
		}
		patch = std::move(*o);
	}

	auto plain = [](const Opened &p_sb, const TocBundle &p_entry) {
		BundleSite site;
		site.id = p_entry.id;
		site.format = p_sb.toc.cas ? BundleFormat::Cas : BundleFormat::Binary;
		site.bytes.file = p_sb.file;
		site.bytes.offset = p_entry.offset;
		site.bytes.len = p_entry.size;
		return site;
	};

	std::vector<BundleSite> out;
	if (!patch) {
		if (base) {
			out.reserve(base->toc.bundles.size());
			for (const TocBundle &e : base->toc.bundles) {
				out.push_back(plain(*base, e));
			}
		}
		return out;
	}

	std::unordered_map<std::string, const TocBundle *> base_entries;
	if (base) {
		for (const TocBundle &e : base->toc.bundles) {
			base_entries[bundle_key(e.id)] = &e; // collect(): the last of equal keys wins
		}
	}
	auto where = [&patch](const TocBundle &p_entry) { return patch->file->name() + " bundle " + p_entry.id; };

	out.reserve(patch->toc.bundles.size());
	for (const TocBundle &entry : patch->toc.bundles) {
		if (entry.base) {
			const auto it = base ? base_entries.find(bundle_key(entry.id)) : base_entries.end();
			if (it == base_entries.end()) {
				return layout_at(where(entry), "patch bundle", "refers to a base bundle that does not exist");
			}
			out.push_back(plain(*base, *it->second));
		} else if (entry.delta) {
			if (!base) {
				return layout_at(where(entry), "patch bundle", "refers to a base bundle that does not exist");
			}
			if (base->toc.cas) {
				return layout_at(where(entry), "delta bundle", "its base superbundle is CAS");
			}
			Result<std::vector<uint8_t>> header = patch->file->read_vec(entry.offset, std::min<uint64_t>(entry.size, 16));
			if (!header) {
				return header.error();
			}
			Result<uint64_t> table_len = delta_table_len(*header);
			if (!table_len) {
				return error_at(where(entry), table_len.error());
			}
			Result<std::vector<uint8_t>> table = patch->file->read_vec(entry.offset, std::min(*table_len, entry.size));
			if (!table) {
				return table.error();
			}
			Result<DeltaBundle> delta = DeltaBundle::parse(*table, entry.offset, entry.size);
			if (!delta) {
				return error_at(where(entry), delta.error());
			}
			BundleSite site;
			site.id = entry.id;
			site.format = BundleFormat::Binary;
			site.bytes.is_delta = true;
			site.bytes.delta = std::move(*delta);
			site.bytes.patch = patch->file;
			site.bytes.base = base->file;
			out.push_back(std::move(site));
		} else {
			out.push_back(plain(*patch, entry));
		}
	}

	if (p_site.delta && base) {
		std::unordered_set<std::string> patched;
		for (const TocBundle &e : patch->toc.bundles) {
			patched.insert(bundle_key(e.id));
		}
		for (const TocBundle &e : base->toc.bundles) {
			if (!patched.contains(bundle_key(e.id))) {
				out.push_back(plain(*base, e));
			}
		}
	}
	return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------

struct LevelMount::Impl {
	std::shared_ptr<const Install> install;
	Level level;
	std::vector<BundleSite> bundles;
	std::unordered_map<std::string, size_t> bundle_keys;
	std::vector<std::string> chunk_superbundles;
	// Chunk superbundle tocs in priority order (the first map that has a GUID wins).
	std::vector<std::shared_ptr<const TocChunkMap>> toc_chunk_maps;
	size_t toc_chunk_count = 0;
	mutable std::shared_mutex mutex;
	Index index;

	const TocChunkSite *toc_chunk(const Guid &p_id) const {
		for (const auto &m : toc_chunk_maps) {
			const auto it = m->find(p_id);
			if (it != m->end()) {
				return &it->second;
			}
		}
		return nullptr;
	}

	Result<BundleManifest> read_manifest(size_t p_at) const {
		const BundleSite &bundle = bundles[p_at];
		auto where = [&bundle]() { return bundle.bytes.path() + " bundle " + bundle.id; };
		if (bundle.format == BundleFormat::Cas) {
			Result<std::vector<uint8_t>> bytes = bundle.bytes.read_vec(0, bundle.bytes.size());
			if (!bytes) {
				return bytes.error();
			}
			Result<BundleManifest> m = parse_cas_manifest(*bytes);
			if (!m) {
				return error_at(where(), m.error());
			}
			return m;
		}
		Result<std::vector<uint8_t>> prefix = bundle.bytes.read_vec(0, 4);
		if (!prefix) {
			return prefix.error();
		}
		Result<uint64_t> len = binary_manifest_len(*prefix);
		if (!len) {
			return error_at(where(), len.error());
		}
		Result<std::vector<uint8_t>> head = bundle.bytes.read_vec(0, *len);
		if (!head) {
			return head.error();
		}
		Result<BundleManifest> m = parse_binary_manifest(*head, bundle.bytes.size());
		if (!m) {
			return error_at(where(), m.error());
		}
		return m;
	}

	Status load_bundle_indices(std::vector<size_t> p_wanted) {
		{
			std::shared_lock lock(mutex);
			std::erase_if(p_wanted, [this](size_t at) { return index.loaded[at].has_value(); });
		}
		if (p_wanted.empty()) {
			return {};
		}
		std::vector<Result<BundleManifest>> manifests =
				parallel_map<Result<BundleManifest>>(p_wanted.size(), [&](size_t i) { return read_manifest(p_wanted[i]); });
		std::unique_lock lock(mutex);
		std::optional<Error> failure;
		for (size_t i = 0; i < p_wanted.size(); ++i) {
			if (manifests[i]) {
				index.register_bundle(p_wanted[i], bundles[p_wanted[i]].id, std::move(*manifests[i]));
			} else if (!failure) {
				failure = manifests[i].error();
			}
		}
		if (failure) {
			return *failure;
		}
		return {};
	}

	Result<std::vector<uint8_t>> read_stored_bytes(const Stored &p_stored) const {
		Result<std::vector<uint8_t>> bytes = [&]() -> Result<std::vector<uint8_t>> {
			switch (p_stored.kind) {
				case Payload::Kind::Cas:
					return install->cas().read(p_stored.sha1);
				case Payload::Kind::Inline:
					return *p_stored.inline_data;
				case Payload::Kind::Bundle:
				default:
					return bundles[p_stored.bundle].bytes.read_vec(p_stored.offset, p_stored.size);
			}
		}();
		if (bytes && bytes->size() != p_stored.size) {
			return size_mismatch("payload " + p_stored.sha1.to_string(), p_stored.size, bytes->size());
		}
		return bytes;
	}

	Result<std::vector<uint8_t>> read_stored_prefix(const Stored &p_stored, uint64_t p_len) const {
		const uint64_t len = std::min(p_len, p_stored.size);
		switch (p_stored.kind) {
			case Payload::Kind::Cas:
				return install->cas().read_prefix(p_stored.sha1, len);
			case Payload::Kind::Inline:
				return std::vector<uint8_t>(p_stored.inline_data->begin(), p_stored.inline_data->begin() + static_cast<ptrdiff_t>(len));
			case Payload::Kind::Bundle:
			default:
				return bundles[p_stored.bundle].bytes.read_vec(p_stored.offset, len);
		}
	}

	Result<std::vector<uint8_t>> decode_named(const Stored &p_stored, uint64_t p_original_size, const std::string &p_name) const {
		Result<std::vector<uint8_t>> bytes = read_stored_bytes(p_stored);
		if (!bytes || p_stored.size == p_original_size) {
			return bytes;
		}
		Result<std::vector<uint8_t>> decoded = decode_blocks(*bytes);
		if (!decoded) {
			return error_at(p_name, decoded.error());
		}
		if (decoded->size() != p_original_size) {
			return size_mismatch(p_name, p_original_size, decoded->size());
		}
		return decoded;
	}

	Result<std::vector<uint8_t>> read_toc_chunk(const TocChunkSite &p_site) const {
		if (p_site.in_cas) {
			return install->cas().read(p_site.sha1);
		}
		return p_site.file->read_vec(p_site.offset, p_site.size);
	}

	Result<Stored> stored_of(const EntryRef &p_entry) const {
		std::shared_lock lock(mutex);
		switch (p_entry.kind) {
			case EntryRef::Kind::Ebx: {
				const auto it = index.ebx_keys.find(asset_key(p_entry.name));
				if (it == index.ebx_keys.end()) {
					return make_error("no loaded bundle holds ebx `" + std::string(p_entry.name) + "`");
				}
				return index.ebx[it->second].stored;
			}
			case EntryRef::Kind::Res: {
				const auto it = index.res_keys.find(asset_key(p_entry.name));
				if (it == index.res_keys.end()) {
					return make_error("no loaded bundle holds res `" + std::string(p_entry.name) + "`");
				}
				return index.res[it->second].stored;
			}
			case EntryRef::Kind::Chunk:
			default: {
				const auto it = index.chunks.find(p_entry.id);
				if (it == index.chunks.end()) {
					return make_error("chunk " + p_entry.id.to_string() + " is in no mounted bundle or toc");
				}
				return it->second.stored;
			}
		}
	}
};

LevelMount::LevelMount(std::unique_ptr<Impl> p_impl) :
		impl_(std::move(p_impl)) {}
LevelMount::LevelMount(LevelMount &&) noexcept = default;
LevelMount &LevelMount::operator=(LevelMount &&) noexcept = default;
LevelMount::~LevelMount() = default;

Result<LevelMount> LevelMount::mount(std::shared_ptr<const Install> p_install, Level p_level) {
	const SuperbundleSite *site = p_install->site(p_level.superbundle);
	if (site == nullptr) {
		return make_error("no Battlefield 3 level `" + p_level.id + "`");
	}
	Result<std::vector<BundleSite>> bundles = superbundle_bundles(*p_install, *site);
	if (!bundles) {
		return bundles.error();
	}
	auto impl = std::make_unique<Impl>();
	impl->bundles = std::move(*bundles);
	impl->bundle_keys.reserve(impl->bundles.size());
	for (size_t at = 0; at < impl->bundles.size(); ++at) {
		impl->bundle_keys.try_emplace(bundle_key(impl->bundles[at].id), at);
	}

	std::vector<std::string> names(std::begin(kChunkSuperbundles), std::end(kChunkSuperbundles));
	if (p_level.expansion) {
		names.push_back("Win32/Xp" + std::to_string(*p_level.expansion) + "Chunks");
	}
	for (const std::string &name : names) {
		const SuperbundleSite *chunk_site = p_install->site(name);
		if (chunk_site == nullptr) {
			continue;
		}
		Result<std::shared_ptr<const TocChunkMap>> map = p_install->chunk_toc(*chunk_site);
		if (!map) {
			return map.error();
		}
		for (const auto &[id, located] : **map) {
			bool seen = false;
			for (const auto &earlier : impl->toc_chunk_maps) {
				if (earlier->contains(id)) {
					seen = true;
					break;
				}
			}
			impl->toc_chunk_count += seen ? 0 : 1;
		}
		impl->toc_chunk_maps.push_back(std::move(*map));
		impl->chunk_superbundles.push_back(chunk_site->name);
	}
	impl->index.loaded.resize(impl->bundles.size());
	impl->install = std::move(p_install);
	impl->level = std::move(p_level);
	return LevelMount(std::move(impl));
}

const Level &LevelMount::level() const {
	return impl_->level;
}

const std::shared_ptr<const Install> &LevelMount::install() const {
	return impl_->install;
}

size_t LevelMount::bundle_count() const {
	return impl_->bundles.size();
}

const std::string &LevelMount::bundle_name(size_t p_bundle) const {
	return impl_->bundles[p_bundle].id;
}

std::vector<std::string> LevelMount::bundle_names() const {
	std::vector<std::string> out;
	out.reserve(impl_->bundles.size());
	for (const BundleSite &b : impl_->bundles) {
		out.push_back(b.id);
	}
	return out;
}

const std::vector<std::string> &LevelMount::chunk_superbundles() const {
	return impl_->chunk_superbundles;
}

size_t LevelMount::toc_chunk_count() const {
	return impl_->toc_chunk_count;
}

bool LevelMount::has_chunk(const Guid &p_id) const {
	if (impl_->toc_chunk(p_id) != nullptr) {
		return true;
	}
	std::shared_lock lock(impl_->mutex);
	return impl_->index.chunks.contains(p_id);
}

Result<BundleManifest> LevelMount::read_manifest(size_t p_bundle) const {
	return impl_->read_manifest(p_bundle);
}

std::vector<std::pair<std::string, Result<BundleManifest>>> LevelMount::bundle_manifests() const {
	std::vector<Result<BundleManifest>> manifests =
			parallel_map<Result<BundleManifest>>(impl_->bundles.size(), [this](size_t i) { return impl_->read_manifest(i); });
	std::vector<std::pair<std::string, Result<BundleManifest>>> out;
	out.reserve(manifests.size());
	for (size_t i = 0; i < manifests.size(); ++i) {
		out.emplace_back(impl_->bundles[i].id, std::move(manifests[i]));
	}
	return out;
}

Status LevelMount::load_all_bundles() const {
	std::vector<size_t> all(impl_->bundles.size());
	for (size_t i = 0; i < all.size(); ++i) {
		all[i] = i;
	}
	return impl_->load_bundle_indices(std::move(all));
}

Status LevelMount::load_bundles(const std::vector<std::string> &p_names) const {
	std::vector<size_t> wanted;
	wanted.reserve(p_names.size());
	for (const std::string &name : p_names) {
		const auto it = impl_->bundle_keys.find(bundle_key(name));
		if (it == impl_->bundle_keys.end()) {
			return make_error("no bundle `" + name + "` in this level");
		}
		if (std::find(wanted.begin(), wanted.end(), it->second) == wanted.end()) {
			wanted.push_back(it->second);
		}
	}
	return impl_->load_bundle_indices(std::move(wanted));
}

std::vector<BundleSummary> LevelMount::loaded_bundles() const {
	std::shared_lock lock(impl_->mutex);
	std::vector<BundleSummary> out;
	for (size_t at : impl_->index.load_order) {
		if (impl_->index.loaded[at]) {
			out.push_back(*impl_->index.loaded[at]);
		}
	}
	return out;
}

std::vector<std::string> LevelMount::ebx_names() const {
	std::shared_lock lock(impl_->mutex);
	std::vector<std::string> out;
	out.reserve(impl_->index.ebx.size());
	for (const EbxRecord &r : impl_->index.ebx) {
		out.push_back(r.name);
	}
	return out;
}

std::vector<std::string> LevelMount::res_names() const {
	std::shared_lock lock(impl_->mutex);
	std::vector<std::string> out;
	out.reserve(impl_->index.res.size());
	for (const ResRecord &r : impl_->index.res) {
		out.push_back(r.name);
	}
	return out;
}

std::vector<Guid> LevelMount::chunk_ids() const {
	std::shared_lock lock(impl_->mutex);
	return impl_->index.chunk_order;
}

bool LevelMount::has_ebx(std::string_view p_name) const {
	std::shared_lock lock(impl_->mutex);
	return impl_->index.ebx_keys.contains(asset_key(p_name));
}

std::optional<ResInfo> LevelMount::res_info(std::string_view p_name) const {
	std::shared_lock lock(impl_->mutex);
	const auto it = impl_->index.res_keys.find(asset_key(p_name));
	if (it == impl_->index.res_keys.end()) {
		return std::nullopt;
	}
	return impl_->index.res[it->second].info;
}

std::optional<ChunkMeta> LevelMount::chunk_meta(const Guid &p_id) const {
	const std::optional<ChunkInfo> info = chunk_info(p_id);
	return info ? info->meta : std::nullopt;
}

std::optional<ChunkMeta> LevelMount::asset_chunk_meta(std::string_view p_owner) const {
	std::shared_lock lock(impl_->mutex);
	const auto it = impl_->index.asset_chunk_meta.find(asset_name_hash(p_owner));
	if (it == impl_->index.asset_chunk_meta.end()) {
		return std::nullopt;
	}
	return it->second;
}

std::optional<ChunkInfo> LevelMount::chunk_info(const Guid &p_id) const {
	std::shared_lock lock(impl_->mutex);
	const auto it = impl_->index.chunks.find(p_id);
	if (it == impl_->index.chunks.end()) {
		return std::nullopt;
	}
	const ChunkRecord &r = it->second;
	return ChunkInfo{ r.stored.sha1, r.stored.size, r.range, r.tail_only, r.meta };
}

Result<std::vector<uint8_t>> LevelMount::read_ebx(std::string_view p_name) const {
	Stored stored;
	uint64_t original_size = 0;
	std::string name;
	{
		std::shared_lock lock(impl_->mutex);
		const auto it = impl_->index.ebx_keys.find(asset_key(p_name));
		if (it == impl_->index.ebx_keys.end()) {
			return make_error("no loaded bundle holds ebx `" + std::string(p_name) + "`");
		}
		const EbxRecord &r = impl_->index.ebx[it->second];
		stored = r.stored;
		original_size = r.original_size;
		name = r.name;
	}
	return impl_->decode_named(stored, original_size, name);
}

Result<std::pair<ResInfo, std::vector<uint8_t>>> LevelMount::read_res(std::string_view p_name) const {
	Stored stored;
	uint64_t original_size = 0;
	ResInfo info;
	std::string name;
	{
		std::shared_lock lock(impl_->mutex);
		const auto it = impl_->index.res_keys.find(asset_key(p_name));
		if (it == impl_->index.res_keys.end()) {
			return make_error("no loaded bundle holds res `" + std::string(p_name) + "`");
		}
		const ResRecord &r = impl_->index.res[it->second];
		stored = r.stored;
		original_size = r.original_size;
		info = r.info;
		name = r.name;
	}
	Result<std::vector<uint8_t>> bytes = impl_->decode_named(stored, original_size, name);
	if (!bytes) {
		return bytes.error();
	}
	return std::make_pair(info, std::move(*bytes));
}

Result<std::vector<uint8_t>> LevelMount::read_chunk(const Guid &p_id) const {
	std::optional<ChunkRecord> record;
	{
		std::shared_lock lock(impl_->mutex);
		const auto it = impl_->index.chunks.find(p_id);
		if (it != impl_->index.chunks.end()) {
			record = it->second;
		}
	}
	const std::string id_text = p_id.to_string();
	Result<std::vector<uint8_t>> stored = [&]() -> Result<std::vector<uint8_t>> {
		if (record && !record->tail_only) {
			return impl_->read_stored_bytes(record->stored);
		}
		if (record && impl_->install->cas().contains(record->stored.sha1)) {
			return impl_->install->cas().read(record->stored.sha1);
		}
		const TocChunkSite *site = impl_->toc_chunk(p_id);
		if (record) {
			if (site == nullptr) {
				return make_error("chunk " + id_text + " is only a tail in its bundle and has no whole copy");
			}
			Result<std::vector<uint8_t>> whole = impl_->read_toc_chunk(*site);
			if (!whole) {
				return whole;
			}
			const uint64_t expected = record->range ? record->range->end : 0;
			if (whole->size() != expected) {
				return size_mismatch("whole copy of chunk " + id_text, expected, whole->size());
			}
			return whole;
		}
		if (site == nullptr) {
			return make_error("chunk " + id_text + " is in no mounted bundle or toc");
		}
		return impl_->read_toc_chunk(*site);
	}();
	if (!stored) {
		return stored;
	}
	Result<std::vector<uint8_t>> decoded = decode_chunk(p_id, *stored);
	if (!decoded) {
		return error_at("chunk " + id_text, decoded.error());
	}
	return decoded;
}

Result<std::unordered_map<Guid, std::string, GuidHash>> LevelMount::ebx_partition_index() const {
	struct Item {
		std::string name;
		Stored stored;
		uint64_t original_size;
	};
	std::vector<Item> records;
	{
		std::shared_lock lock(impl_->mutex);
		records.reserve(impl_->index.ebx.size());
		for (const EbxRecord &r : impl_->index.ebx) {
			records.push_back(Item{ r.name, r.stored, r.original_size });
		}
	}
	std::vector<Result<Guid>> partitions = parallel_map<Result<Guid>>(records.size(), [&](size_t i) -> Result<Guid> {
		const Item &item = records[i];
		Result<std::vector<uint8_t>> head = item.stored.size == item.original_size
				? impl_->read_stored_prefix(item.stored, kEbxHeadLen)
				: impl_->decode_named(item.stored, item.original_size, item.name);
		if (!head) {
			return head.error();
		}
		Result<Guid> g = ebx_partition(*head);
		if (!g) {
			return error_at("ebx " + item.name, g.error());
		}
		return g;
	});
	std::unordered_map<Guid, std::string, GuidHash> out;
	out.reserve(records.size());
	for (size_t i = 0; i < records.size(); ++i) {
		if (!partitions[i]) {
			return partitions[i].error();
		}
		out.try_emplace(*partitions[i], records[i].name);
	}
	return out;
}

Result<Guid> LevelMount::ebx_partition_guid(std::string_view p_name) const {
	Stored stored;
	uint64_t original_size = 0;
	std::string name;
	{
		std::shared_lock lock(impl_->mutex);
		const auto it = impl_->index.ebx_keys.find(asset_key(p_name));
		if (it == impl_->index.ebx_keys.end()) {
			return make_error("no loaded bundle holds ebx `" + std::string(p_name) + "`");
		}
		const EbxRecord &r = impl_->index.ebx[it->second];
		stored = r.stored;
		original_size = r.original_size;
		name = r.name;
	}
	Result<std::vector<uint8_t>> head =
			stored.size == original_size ? impl_->read_stored_prefix(stored, kEbxHeadLen) : impl_->decode_named(stored, original_size, name);
	if (!head) {
		return head.error();
	}
	Result<Guid> g = ebx_partition(*head);
	if (!g) {
		return error_at("ebx " + name, g.error());
	}
	return g;
}

std::optional<std::string> LevelMount::ebx_bundle(std::string_view p_name) const {
	std::shared_lock lock(impl_->mutex);
	const auto it = impl_->index.ebx_keys.find(asset_key(p_name));
	if (it == impl_->index.ebx_keys.end()) {
		return std::nullopt;
	}
	return impl_->bundles[impl_->index.ebx[it->second].stored.bundle].id;
}

std::optional<Sha1> LevelMount::entry_sha1(const EntryRef &p_entry) const {
	Result<Stored> s = impl_->stored_of(p_entry);
	if (!s) {
		return std::nullopt;
	}
	return s->sha1;
}

Result<std::vector<uint8_t>> LevelMount::read_stored(const EntryRef &p_entry) const {
	Result<Stored> s = impl_->stored_of(p_entry);
	if (!s) {
		return s.error();
	}
	return impl_->read_stored_bytes(*s);
}

std::vector<Sha1> LevelMount::chunk_source_sha1s(const Guid &p_id) const {
	// Mirrors read_chunk's choice of bytes.
	std::optional<Sha1> record_sha1;
	bool tail_only = false;
	{
		std::shared_lock lock(impl_->mutex);
		const auto it = impl_->index.chunks.find(p_id);
		if (it != impl_->index.chunks.end()) {
			record_sha1 = it->second.stored.sha1;
			tail_only = it->second.tail_only;
		}
	}
	std::vector<Sha1> out;
	if (record_sha1 && (!tail_only || impl_->install->cas().contains(*record_sha1))) {
		out.push_back(*record_sha1);
		return out;
	}
	const TocChunkSite *site = impl_->toc_chunk(p_id);
	if (record_sha1) {
		out.push_back(*record_sha1);
	}
	if (site != nullptr) {
		out.push_back(site->sha1);
	} else {
		out.clear(); // read_chunk fails too
	}
	return out;
}

} // namespace bf3::core
