// Ported from IW4L asset_transport::bf3 (install.rs, cas.rs, file.rs), Apache-2.0.
#include "bf3/core/install.h"

#include "bf3/core/superbundle_mount.h"
#include "format_errors.h"

#include <algorithm>

namespace bf3::core {

// --- InstallFile --------------------------------------------------------------------------------

Status InstallFile::read_into(uint64_t p_offset, std::span<uint8_t> p_dst) const {
	const uint64_t len = p_dst.size();
	if (p_offset + len < p_offset || p_offset + len > size()) {
		return make_error(name_ + ": read of " + std::to_string(len) + " bytes at " + std::to_string(p_offset) + " runs past the end (" +
				std::to_string(size()) + " bytes)");
	}
	if (len == 0) {
		return {};
	}
	const int64_t got = file_.read_at(p_offset, p_dst.data(), p_dst.size());
	if (got != static_cast<int64_t>(len)) {
		return make_error(name_ + ": read failed at " + std::to_string(p_offset) + " (" + std::to_string(len) + " bytes)");
	}
	return {};
}

Result<std::vector<uint8_t>> InstallFile::read_vec(uint64_t p_offset, uint64_t p_len) const {
	if (p_offset + p_len < p_offset || p_offset + p_len > size()) {
		return make_error(name_ + ": read of " + std::to_string(p_len) + " bytes at " + std::to_string(p_offset) + " runs past the end (" +
				std::to_string(size()) + " bytes)");
	}
	std::vector<uint8_t> buf(static_cast<size_t>(p_len));
	BF3_TRY(read_into(p_offset, buf));
	return buf;
}

namespace {

std::string join(std::string_view p_dir, std::string_view p_relative) {
	if (p_dir.empty()) {
		return std::string(p_relative);
	}
	std::string out(p_dir);
	out += '/';
	out += p_relative;
	return out;
}

// IW4L file.rs `resolve`: the joined name when it exists (Windows paths are case-insensitive, so
// this also covers other casings), else nothing.
std::optional<std::string> resolve(const InstallReader &p_reader, std::string_view p_dir, std::string_view p_relative) {
	std::string path = join(p_dir, p_relative);
	if (p_reader.exists(path)) {
		return path;
	}
	return std::nullopt;
}

} // namespace

// --- CasStore -----------------------------------------------------------------------------------

Status CasStore::open(const InstallReader *p_reader, const std::vector<std::string> &p_dirs) {
	reader_ = p_reader;
	for (const std::string &dir : p_dirs) {
		const std::optional<std::string> path = resolve(*p_reader, dir, "cas.cat");
		if (!path) {
			continue;
		}
		ReadOnlyFile f = p_reader->open(*path);
		std::vector<uint8_t> bytes;
		if (!f.is_open() || !f.read_all(bytes)) {
			return make_error(*path + ": cannot read");
		}
		Status plain = deobfuscate(bytes);
		if (!plain) {
			return error_at(*path, plain.error());
		}
		Result<std::vector<CatalogEntry>> parsed = parse_catalog(bytes);
		if (!parsed) {
			return error_at(*path, parsed.error());
		}
		const uint8_t catalog = static_cast<uint8_t>(std::min<size_t>(catalogs_.size(), 255));
		entries_.reserve(entries_.size() + parsed->size());
		for (const CatalogEntry &e : *parsed) {
			entries_.try_emplace(e.sha1, CasLocation{ catalog, e.archive, e.offset, e.size });
		}
		catalogs_.push_back(dir);
	}
	return {};
}

std::optional<CasLocation> CasStore::location(const Sha1 &p_sha1) const {
	const auto it = entries_.find(p_sha1);
	if (it == entries_.end()) {
		return std::nullopt;
	}
	return it->second;
}

const std::string *CasStore::catalog_dir(uint8_t p_catalog) const {
	return p_catalog < catalogs_.size() ? &catalogs_[p_catalog] : nullptr;
}

Result<std::shared_ptr<InstallFile>> CasStore::archive(const CasLocation &p_location) const {
	const uint64_t key = (uint64_t(p_location.catalog) << 32) | p_location.archive;
	{
		std::lock_guard lock(archives_mutex_);
		const auto it = archives_.find(key);
		if (it != archives_.end()) {
			return it->second;
		}
	}
	const std::string *dir = catalog_dir(p_location.catalog);
	const std::string name = join(dir ? *dir : std::string(), cas_archive_name(p_location.archive));
	ReadOnlyFile f = reader_->open(name);
	if (!f.is_open()) {
		return make_error(name + ": cannot open");
	}
	auto opened = std::make_shared<InstallFile>(name, std::move(f));
	std::lock_guard lock(archives_mutex_);
	return archives_.try_emplace(key, std::move(opened)).first->second;
}

Result<std::vector<uint8_t>> CasStore::read(const Sha1 &p_sha1) const {
	const std::optional<CasLocation> loc = location(p_sha1);
	if (!loc) {
		return make_error("payload " + p_sha1.to_string() + " is in no CAS catalog");
	}
	Result<std::shared_ptr<InstallFile>> file = archive(*loc);
	if (!file) {
		return file.error();
	}
	return (*file)->read_vec(loc->offset, loc->size);
}

Result<std::vector<uint8_t>> CasStore::read_prefix(const Sha1 &p_sha1, uint64_t p_len) const {
	const std::optional<CasLocation> loc = location(p_sha1);
	if (!loc) {
		return make_error("payload " + p_sha1.to_string() + " is in no CAS catalog");
	}
	Result<std::shared_ptr<InstallFile>> file = archive(*loc);
	if (!file) {
		return file.error();
	}
	return (*file)->read_vec(loc->offset, std::min<uint64_t>(p_len, loc->size));
}

Status CasStore::check_entry_header(const Sha1 &p_sha1) const {
	const std::optional<CasLocation> loc = location(p_sha1);
	if (!loc) {
		return make_error("payload " + p_sha1.to_string() + " is in no CAS catalog");
	}
	Result<std::shared_ptr<InstallFile>> file = archive(*loc);
	if (!file) {
		return file.error();
	}
	if (loc->offset < kCasEntryHeaderLen) {
		return make_error((*file)->name() + ": read of " + std::to_string(kCasEntryHeaderLen) + " bytes at " + std::to_string(loc->offset) +
				" runs past the end (" + std::to_string((*file)->size()) + " bytes)");
	}
	Result<std::vector<uint8_t>> header = (*file)->read_vec(loc->offset - kCasEntryHeaderLen, kCasEntryHeaderLen);
	if (!header) {
		return header.error();
	}
	const CatalogEntry entry{ p_sha1, loc->offset, loc->size, loc->archive };
	Status st = check_cas_entry_header(*header, entry);
	if (!st) {
		return error_at((*file)->name(), st.error());
	}
	return {};
}

// --- Install ------------------------------------------------------------------------------------

namespace {

std::vector<Package> discover_packages(const InstallReader &p_reader) {
	std::vector<Package> packages;
	if (!p_reader.exists("Update")) {
		return packages;
	}
	for (const std::string &folder : p_reader.list_subdirectories("Update")) {
		const std::string dir = "Update/" + folder;
		const std::optional<std::string> mft = resolve(p_reader, dir, "package.mft");
		if (!mft) {
			continue;
		}
		ReadOnlyFile f = p_reader.open(*mft);
		std::vector<uint8_t> bytes;
		if (!f.is_open() || !f.read_all(bytes) || !detail::valid_utf8(bytes)) {
			continue; // IW4L: read_to_string failed -> not a package
		}
		const PackageManifest m = parse_package_manifest(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
		Package p;
		p.name = m.name.value_or(folder);
		p.data_dir = dir + "/Data";
		p.authoritative = m.authoritative;
		p.version = m.version;
		p.require_version = m.require_version;
		p.mount_order = m.mount_order.value_or(0);
		packages.push_back(std::move(p));
	}
	std::stable_sort(packages.begin(), packages.end(), [](const Package &a, const Package &b) { return a.name < b.name; });
	return packages;
}

} // namespace

std::optional<uint8_t> expansion_of(std::string_view p_level_id) {
	if (p_level_id.size() >= 4 && (p_level_id[0] == 'x' || p_level_id[0] == 'X') && (p_level_id[1] == 'p' || p_level_id[1] == 'P') &&
			p_level_id[2] >= '0' && p_level_id[2] <= '9' && p_level_id[3] == '_') {
		return static_cast<uint8_t>(p_level_id[2] - '0');
	}
	return std::nullopt;
}

std::optional<std::string> multiplayer_level_id(std::string_view p_superbundle) {
	std::string_view parts[5];
	size_t n = 0;
	size_t pos = 0;
	for (;;) {
		const size_t slash = p_superbundle.find('/', pos);
		if (n == 5) {
			return std::nullopt;
		}
		parts[n++] = p_superbundle.substr(pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
		if (slash == std::string_view::npos) {
			break;
		}
		pos = slash + 1;
	}
	if (n != 4) {
		return std::nullopt;
	}
	const std::string_view id = parts[3];
	const bool multiplayer = (id.size() >= 3 && iequals_ascii(id.substr(0, 3), "mp_")) || expansion_of(id).has_value();
	if (iequals_ascii(parts[0], "win32") && iequals_ascii(parts[1], "levels") && iequals_ascii(parts[2], id) && multiplayer) {
		return std::string(id);
	}
	return std::nullopt;
}

Result<std::shared_ptr<Install>> Install::open(const InstallReader &p_reader) {
	std::shared_ptr<Install> self(new Install());
	self->reader_ = p_reader;
	const InstallReader &reader = self->reader_;
	const std::string root = reader.status().root_utf8;
	const Error not_an_install = make_error(root + " is not a Battlefield 3 install (no Data/layout.toc and Data/cas.cat)");
	if (reader.root().empty()) {
		return not_an_install;
	}
	const std::optional<std::string> data_dir = resolve(reader, "", "Data");
	if (!data_dir) {
		return not_an_install;
	}
	const std::optional<std::string> base_layout = resolve(reader, *data_dir, "layout.toc");
	if (!base_layout || !resolve(reader, *data_dir, "cas.cat")) {
		return not_an_install;
	}
	self->data_dir_ = *data_dir;

	// mounted_packages: the authoritative package is the patch; keep packages whose
	// RequireVersion the patch satisfies; order by (MountOrder, Name).
	std::vector<Package> packages = discover_packages(reader);
	std::optional<Package> patch;
	const auto auth = std::find_if(packages.begin(), packages.end(), [](const Package &p) { return p.authoritative; });
	if (auth != packages.end()) {
		patch = *auth;
		packages.erase(auth);
	}
	const std::optional<int64_t> patch_version = patch ? patch->version : std::nullopt;
	std::erase_if(packages, [&](const Package &p) {
		if (!p.require_version) {
			return false;
		}
		return !patch_version || *p.require_version > *patch_version;
	});
	std::stable_sort(packages.begin(), packages.end(), [](const Package &a, const Package &b) {
		return a.mount_order != b.mount_order ? a.mount_order < b.mount_order : a.name < b.name;
	});

	const std::optional<std::string> patch_dir = patch ? std::optional<std::string>(patch->data_dir) : std::nullopt;
	std::string layout_path = *base_layout;
	if (patch_dir) {
		if (std::optional<std::string> p = resolve(reader, *patch_dir, "layout.toc")) {
			layout_path = *p;
		}
	}
	{
		Result<std::vector<uint8_t>> bytes = self->read_whole(layout_path);
		if (!bytes) {
			return bytes.error();
		}
		Status plain = deobfuscate(*bytes);
		if (!plain) {
			return error_at(layout_path, plain.error());
		}
		Result<Layout> layout = parse_layout(*bytes);
		if (!layout) {
			return error_at(layout_path, layout.error());
		}
		self->layout_ = std::move(*layout);
		self->layout_path_ = layout_path;
	}

	std::vector<std::string> base_dirs{ *data_dir };
	for (const Package &p : packages) {
		base_dirs.push_back(p.data_dir);
	}
	self->sites_.reserve(self->layout_.superbundles.size());
	for (const SuperbundleDecl &decl : self->layout_.superbundles) {
		const std::string toc = decl.name + ".toc";
		SuperbundleSite site;
		site.name = decl.name;
		site.delta = decl.delta;
		for (const std::string &dir : base_dirs) {
			if (std::optional<std::string> p = resolve(reader, dir, toc)) {
				site.base_toc = std::move(p);
				break;
			}
		}
		if (patch_dir) {
			site.patch_toc = resolve(reader, *patch_dir, toc);
		}
		self->sites_.push_back(std::move(site));
	}

	std::vector<std::string> catalog_dirs;
	if (patch_dir) {
		catalog_dirs.push_back(*patch_dir);
	}
	catalog_dirs.push_back(*data_dir);
	BF3_TRY(self->cas_.open(&self->reader_, catalog_dirs));

	if (patch) {
		packages.push_back(std::move(*patch));
	}
	self->packages_ = std::move(packages);
	return self;
}

std::vector<std::string> Install::superbundle_names() const {
	std::vector<std::string> out;
	out.reserve(sites_.size());
	for (const SuperbundleSite &s : sites_) {
		out.push_back(s.name);
	}
	return out;
}

const SuperbundleSite *Install::site(std::string_view p_name) const {
	for (const SuperbundleSite &s : sites_) {
		if (iequals_ascii(s.name, p_name)) {
			return &s;
		}
	}
	return nullptr;
}

std::vector<Level> Install::levels() const {
	std::vector<Level> out;
	for (const SuperbundleSite &s : sites_) {
		if (!(s.base_toc || (s.patch_toc && !s.delta))) {
			continue;
		}
		const std::optional<std::string> id = multiplayer_level_id(s.name);
		if (!id) {
			continue;
		}
		out.push_back(Level{ *id, s.name, expansion_of(*id), s.base_toc, s.patch_toc });
	}
	return out;
}

std::optional<Level> Install::level(std::string_view p_level_id) const {
	for (Level &l : levels()) {
		if (iequals_ascii(l.id, p_level_id)) {
			return std::move(l);
		}
	}
	return std::nullopt;
}

Result<LevelMount> Install::mount_level(std::string_view p_level_id) const {
	std::optional<Level> l = level(p_level_id);
	if (!l) {
		return make_error("no Battlefield 3 level `" + std::string(p_level_id) + "`");
	}
	return LevelMount::mount(shared_from_this(), std::move(*l));
}

Result<LevelMount> Install::mount_superbundle(std::string_view p_name) const {
	const SuperbundleSite *s = site(p_name);
	if (s == nullptr) {
		return make_error("no Battlefield 3 level `" + std::string(p_name) + "`");
	}
	const size_t slash = p_name.rfind('/');
	Level l;
	l.id = std::string(slash == std::string_view::npos ? p_name : p_name.substr(slash + 1));
	l.expansion = expansion_of(l.id);
	l.superbundle = s->name;
	l.base_toc = s->base_toc;
	l.patch_toc = s->patch_toc;
	return LevelMount::mount(shared_from_this(), std::move(l));
}

Result<std::shared_ptr<InstallFile>> Install::file(std::string_view p_relative) const {
	const std::string key = to_lower_ascii(p_relative);
	{
		std::lock_guard lock(files_mutex_);
		const auto it = files_.find(key);
		if (it != files_.end()) {
			return it->second;
		}
	}
	ReadOnlyFile f = reader_.open(p_relative);
	if (!f.is_open()) {
		return make_error(std::string(p_relative) + ": cannot open");
	}
	auto opened = std::make_shared<InstallFile>(std::string(p_relative), std::move(f));
	std::lock_guard lock(files_mutex_);
	return files_.try_emplace(key, std::move(opened)).first->second;
}

Result<std::vector<uint8_t>> Install::read_whole(std::string_view p_relative) const {
	ReadOnlyFile f = reader_.open(p_relative);
	std::vector<uint8_t> bytes;
	if (!f.is_open() || !f.read_all(bytes)) {
		return make_error(std::string(p_relative) + ": cannot read");
	}
	return bytes;
}

Result<Toc> Install::read_toc(std::string_view p_relative) const {
	Result<std::vector<uint8_t>> bytes = read_whole(p_relative);
	if (!bytes) {
		return bytes.error();
	}
	Status plain = deobfuscate(*bytes);
	if (!plain) {
		return error_at(std::string(p_relative), plain.error());
	}
	Result<Toc> toc = parse_toc(*bytes);
	if (!toc) {
		return error_at(std::string(p_relative), toc.error());
	}
	return toc;
}

std::string Install::superbundle_file(std::string_view p_toc_relative) {
	const size_t slash = p_toc_relative.find_last_of("/\\");
	const size_t dot = p_toc_relative.rfind('.');
	if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
		return std::string(p_toc_relative) + ".sb";
	}
	return std::string(p_toc_relative.substr(0, dot)) + ".sb";
}

Result<std::shared_ptr<const TocChunkMap>> Install::chunk_toc(const SuperbundleSite &p_site) const {
	const std::string key = to_lower_ascii(p_site.name);
	{
		std::lock_guard lock(chunk_tocs_mutex_);
		const auto it = chunk_tocs_.find(key);
		if (it != chunk_tocs_.end()) {
			return it->second;
		}
	}
	auto chunks = std::make_shared<TocChunkMap>();
	for (const std::optional<std::string> *toc_path : { &p_site.base_toc, &p_site.patch_toc }) {
		if (!*toc_path) {
			continue;
		}
		Result<Toc> toc = read_toc(**toc_path);
		if (!toc) {
			return toc.error();
		}
		std::shared_ptr<InstallFile> sb;
		if (std::any_of(toc->chunks.begin(), toc->chunks.end(), [](const TocChunk &c) { return !c.in_cas; })) {
			Result<std::shared_ptr<InstallFile>> f = file(superbundle_file(**toc_path));
			if (!f) {
				return f.error();
			}
			sb = *f;
		}
		chunks->reserve(chunks->size() + toc->chunks.size());
		for (const TocChunk &c : toc->chunks) {
			TocChunkSite s;
			s.in_cas = c.in_cas;
			if (c.in_cas) {
				s.sha1 = c.sha1;
			} else {
				s.file = sb;
				s.offset = c.offset;
				s.size = c.size;
			}
			(*chunks)[c.id] = std::move(s); // patch toc wins
		}
	}
	std::lock_guard lock(chunk_tocs_mutex_);
	return std::shared_ptr<const TocChunkMap>(chunk_tocs_.try_emplace(key, std::move(chunks)).first->second);
}

} // namespace bf3::core
