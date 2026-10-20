#include "bf3/core/ant/ant_install_source.h"

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/ids.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <unordered_set>
#include <utility>

namespace bf3::core::ant {

namespace {

bool is_scanned_superbundle(std::string_view p_name) {
	const std::string l = to_lower_ascii(p_name);
	return l.find("chunks") == std::string::npos && l.rfind("win32/loc/", 0) != 0;
}

void read_package_fields(const EbxInstance &p_asset, AntPackageInfo &r_info) {
	if (const std::optional<EbxValue> v = p_asset.get("Name")) {
		if (const std::optional<std::string_view> s = v->as_str()) {
			r_info.name = std::string(*s);
		}
	}
	if (const std::optional<EbxValue> v = p_asset.get("PackagingType")) {
		const std::optional<std::string_view> n = v->enum_name();
		r_info.packaging = packaging_from_enum_name(n ? *n : std::string_view());
	}
	if (const std::optional<EbxValue> v = p_asset.get("StreamingGuid")) {
		if (const std::optional<Guid> g = v->as_guid()) {
			r_info.streaming_guid = g->to_string();
		}
	}
	if (const std::optional<EbxValue> v = p_asset.get("ChunkSize")) {
		r_info.chunk_size = v->as_u64().value_or(0);
	}
}

}

Result<std::unique_ptr<AntInstallSource>> AntInstallSource::open(std::shared_ptr<const Install> p_install) {
	const auto start = std::chrono::steady_clock::now();
	if (!p_install) {
		return make_error("ant install source: no install");
	}
	std::unique_ptr<AntInstallSource> s(new AntInstallSource());
	s->install_ = p_install;
	std::unordered_set<std::string> taken_partitions;
	for (const std::string &sb : p_install->superbundle_names()) {
		if (!is_scanned_superbundle(sb)) {
			continue;
		}
		Result<LevelMount> m = p_install->mount_superbundle(sb);
		if (!m) {
			s->report_.warnings.push_back(sb + ": mount: " + m.error().message);
			continue;
		}
		++s->report_.superbundles_scanned;

		std::vector<std::pair<std::string, std::string>> want_res, want_ebx;
		std::unordered_set<std::string> want_res_keys, want_ebx_keys;
		std::set<std::string> want_bundles;
		for (auto &[bundle, manifest] : m->bundle_manifests()) {
			if (!manifest) {
				s->report_.warnings.push_back(sb + " bundle " + bundle + ": " + manifest.error().message);
				continue;
			}
			for (const ResEntry &e : manifest->res) {
				std::string key = to_lower_ascii(e.name);
				if (e.res_type == kAssetBankResType && !s->res_.contains(key) && want_res_keys.insert(key).second) {
					want_res.emplace_back(std::move(key), bundle);
					want_bundles.insert(bundle);
				}
			}
			for (const EbxEntry &e : manifest->ebx) {
				std::string key = to_lower_ascii(e.name);
				if (key.starts_with(kAntPackagePrefix) && !taken_partitions.contains(key) && want_ebx_keys.insert(key).second) {
					want_ebx.emplace_back(std::move(key), bundle);
					want_bundles.insert(bundle);
				}
			}
		}
		if (want_res.empty() && want_ebx.empty()) {
			continue;
		}
		const Status loaded = m->load_bundles(std::vector<std::string>(want_bundles.begin(), want_bundles.end()));
		if (!loaded) {
			s->report_.warnings.push_back(sb + ": load bundles: " + loaded.error().message);
			continue;
		}
		const uint32_t mount_index = uint32_t(s->mounts_.size());
		bool used = !want_res.empty();
		for (auto &[key, bundle] : want_res) {
			s->res_.emplace(key, ResSite{ mount_index, sb, bundle });
		}

		EbxDb db;
		for (auto &[key, bundle] : want_ebx) {
			Result<std::vector<uint8_t>> bytes = m->read_ebx(key);
			if (!bytes) {
				s->report_.warnings.push_back(sb + " ebx " + key + ": " + bytes.error().message);
				continue;
			}
			Result<bool> inserted = db.parse_insert(key, std::move(*bytes));
			if (!inserted) {
				s->report_.warnings.push_back(sb + " ebx " + key + ": " + inserted.error().message);
				continue;
			}
			const EbxPartition *part = db.partition_by_name(key);
			if (part == nullptr) {
				continue;
			}
			taken_partitions.insert(key);
			++s->report_.partitions;
			used = true;
			const EbxInstance asset = part->primary();
			if (asset.type_name() != "AntPackageAsset") {
				continue;
			}
			AntPackageInfo info;
			info.partition = key;
			read_package_fields(asset, info);
			info.short_name = package_short_name(info.name.empty() ? info.partition : info.name);
			info.superbundle = sb;
			info.bundle = bundle;
			s->package_mount_.emplace(key, mount_index);
			s->packages_.push_back(std::move(info));
		}
		if (used) {
			++s->report_.superbundles_used;
			s->mounts_.push_back(std::make_unique<LevelMount>(std::move(*m)));
		}
	}
	std::sort(s->packages_.begin(), s->packages_.end(), [](const AntPackageInfo &a, const AntPackageInfo &b) { return a.partition < b.partition; });

	for (AntPackageInfo &p : s->packages_) {
		if (p.packaging == AntPackagingType::Chunk) {
			continue;
		}
		const auto it = s->res_.find(to_lower_ascii(p.name));
		if (it != s->res_.end()) {
			p.superbundle = it->second.superbundle;
			p.bundle = it->second.bundle;
		}
	}
	std::vector<std::string> res_names;
	res_names.reserve(s->res_.size());
	for (const auto &[key, site] : s->res_) {
		res_names.push_back(key);
	}
	std::sort(res_names.begin(), res_names.end());
	for (const std::string &key : res_names) {
		const ResSite &site = s->res_.at(key);
		AntPackageInfo info;
		info.partition = key;
		info.name = key;
		info.short_name = package_short_name(key);
		info.is_asset_package = false;
		for (const AntPackageInfo &p : s->packages_) {
			if (to_lower_ascii(p.name) == key) {
				info = p;
				break;
			}
		}
		info.superbundle = site.superbundle;
		info.bundle = site.bundle;
		s->banks_.push_back(std::move(info));
	}
	s->report_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	return s;
}

const LevelMount *AntInstallSource::weapons_mount(std::string &r_error) const {
	std::call_once(weapons_once_, [this] {
		Result<LevelMount> m = install_->mount_superbundle("Win32/Weapons");
		if (!m) {
			weapons_error_ = m.error().message;
			return;
		}
		const Status loaded = m->load_all_bundles();
		if (!loaded) {
			weapons_error_ = loaded.error().message;
			return;
		}
		weapons_ = std::make_unique<LevelMount>(std::move(*m));
	});
	if (!weapons_) {
		r_error = "Win32/Weapons: " + weapons_error_;
		return nullptr;
	}
	return weapons_.get();
}

bool AntInstallSource::read(const AntPackageInfo &p_info, std::vector<uint8_t> &r_bytes, std::string &r_error) const {
	if (p_info.packaging == AntPackagingType::Chunk && p_info.is_asset_package) {
		const auto it = package_mount_.find(p_info.partition);
		if (it == package_mount_.end()) {
			r_error = p_info.partition + ": unknown package";
			return false;
		}
		const std::optional<Guid> guid = Guid::parse(p_info.streaming_guid);
		if (!guid) {
			r_error = p_info.partition + ": bad StreamingGuid `" + p_info.streaming_guid + "`";
			return false;
		}
		Result<std::vector<uint8_t>> bytes = mounts_[it->second]->read_chunk(*guid);
		if (!bytes) {

			const std::string first = bytes.error().message;
			std::string error;
			const LevelMount *weapons = weapons_mount(error);
			if (weapons == nullptr) {
				r_error = p_info.partition + ": chunk " + p_info.streaming_guid + ": " + first + "; " + error;
				return false;
			}
			bytes = weapons->read_chunk(*guid);
			if (!bytes) {
				r_error = p_info.partition + ": chunk " + p_info.streaming_guid + ": " + first + "; Win32/Weapons: " + bytes.error().message;
				return false;
			}
		}
		r_bytes = std::move(*bytes);
		return true;
	}

	const std::string key = to_lower_ascii(p_info.name);
	const auto it = res_.find(key);
	if (it == res_.end()) {
		r_error = p_info.name + ": no AssetBank res of that name";
		return false;
	}
	Result<std::pair<ResInfo, std::vector<uint8_t>>> res = mounts_[it->second.mount]->read_res(key);
	if (!res) {
		r_error = p_info.name + ": " + res.error().message;
		return false;
	}
	r_bytes = std::move(res->second);
	return true;
}

}
