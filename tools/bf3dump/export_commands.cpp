#include "export_commands.h"

#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/ids.h"
#include "bf3/core/path_guard.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/res_types.h"
#include "bf3/core/sha1_digest.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/elapsed.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string_view>
#include <unordered_set>

namespace bf3dump {

namespace {

using namespace bf3::core;
namespace fs = std::filesystem;

fs::path utf8_path(std::string_view p_text) {
	return fs::path(std::u8string(p_text.begin(), p_text.end()));
}

std::string hex_spaced(const uint8_t *p_bytes, size_t p_count) {
	std::string out;
	for (size_t i = 0; i < p_count; ++i) {
		char b[4];
		std::snprintf(b, sizeof(b), i ? " %02x" : "%02x", p_bytes[i]);
		out += b;
	}
	return out;
}

class ExportDir {
public:
	ExportDir(const InstallReader &p_reader, fs::path p_root) : guard_(p_reader.write_guard()), root_(std::move(p_root)) {}

	bool prepare(const std::vector<std::string> &p_subdirs) {
		for (const std::string &sub : p_subdirs) {
			const fs::path dir = root_ / utf8_path(sub);
			if (!allowed(dir)) {
				return false;
			}
			std::error_code ec;
			fs::create_directories(to_extended_length(dir), ec);
			if (ec) {
				std::fprintf(stderr, "cannot create %s: %s\n", dir.string().c_str(), ec.message().c_str());
				return false;
			}
		}
		return true;
	}

	bool write(std::string_view p_relative, std::span<const uint8_t> p_head, std::span<const uint8_t> p_body = {}) {
		const fs::path path = root_ / utf8_path(p_relative);
		if (!allowed(path)) {
			return false;
		}
		FILE *f = nullptr;
		if (_wfopen_s(&f, to_extended_length(path).c_str(), L"wb") != 0 || f == nullptr) {
			std::fprintf(stderr, "cannot create %s\n", path.string().c_str());
			return false;
		}
		const bool ok = std::fwrite(p_head.data(), 1, p_head.size(), f) == p_head.size() &&
				std::fwrite(p_body.data(), 1, p_body.size(), f) == p_body.size() && std::fflush(f) == 0;
		std::fclose(f);
		if (!ok) {
			std::fprintf(stderr, "short write of %s\n", path.string().c_str());
			return false;
		}
		bytes_ += p_head.size() + p_body.size();
		++files_;
		return true;
	}

	bool write_text(std::string_view p_relative, const std::string &p_text) {
		return write(p_relative, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(p_text.data()), p_text.size()));
	}

	uint64_t bytes() const { return bytes_; }
	size_t files() const { return files_; }

private:
	bool allowed(const fs::path &p_path) const {
		const PathGuard::Verdict verdict = guard_.check_write_target(p_path);
		if (verdict != PathGuard::Verdict::Allowed) {
			std::fprintf(stderr, "refusing to write %s: %s\n", p_path.string().c_str(), PathGuard::verdict_name(verdict));
			return false;
		}
		return true;
	}

	PathGuard guard_;
	fs::path root_;
	std::atomic<uint64_t> bytes_{ 0 };
	std::atomic<size_t> files_{ 0 };
};

Result<LevelMount> mount_loaded(const Install &p_install, const std::string &p_sb) {
	Result<LevelMount> m = p_install.mount_superbundle(p_sb);
	if (!m) {
		return m;
	}
	const Status st = m->load_all_bundles();
	if (!st) {
		return error_at("load bundles " + p_sb, st.error());
	}
	return m;
}

bool parse_megabytes(const std::string &p_text, uint64_t &r_bytes) {
	uint64_t mb = 0;
	const auto res = std::from_chars(p_text.data(), p_text.data() + p_text.size(), mb);
	if (res.ec != std::errc() || res.ptr != p_text.data() + p_text.size() || mb == 0 || mb > (uint64_t(1) << 20)) {
		std::fprintf(stderr, "max_mb: not a size in MiB: %s\n", p_text.c_str());
		return false;
	}
	r_bytes = mb << 20;
	return true;
}

int export_cache(const InstallReader &p_reader, const Install &p_install, const std::string &p_sb, const std::string &p_out, uint64_t p_budget) {
	const auto start = std::chrono::steady_clock::now();
	Result<LevelMount> m = mount_loaded(p_install, p_sb);
	if (!m) {
		std::fprintf(stderr, "mount %s: %s\n", p_sb.c_str(), m.error().message.c_str());
		return 1;
	}
	ExportDir dir(p_reader, utf8_path(p_out));
	if (!dir.prepare({ "stored", "payload" })) {
		return 1;
	}
	std::vector<std::string> res_names = m->res_names();
	std::sort(res_names.begin(), res_names.end());
	std::vector<Guid> chunks;
	for (const Guid &id : m->chunk_ids()) {
		const std::optional<ChunkInfo> info = m->chunk_info(id);
		if (info && !info->tail_only) {
			chunks.push_back(id);
		}
	}
	std::sort(chunks.begin(), chunks.end());

	std::string index = "kind\tname\tsha1\tstored_size\tdecoded_size\tres_type\tres_meta\n";
	std::set<Sha1> seen;
	uint64_t stored_total = 0, decoded_total = 0;
	size_t rows = 0, errors = 0;
	bool full = false;
	auto take = [&](const EntryRef &p_ref, const std::string &p_kind, const std::string &p_name) {
		const std::optional<Sha1> sha1 = m->entry_sha1(p_ref);
		if (!sha1) {
			std::fprintf(stderr, "%s %s: no manifest SHA-1\n", p_kind.c_str(), p_name.c_str());
			++errors;
			return;
		}
		if (seen.contains(*sha1)) {
			return;
		}
		Result<std::vector<uint8_t>> stored = m->read_stored(p_ref);
		if (!stored) {
			std::fprintf(stderr, "%s %s: %s\n", p_kind.c_str(), p_name.c_str(), stored.error().message.c_str());
			++errors;
			return;
		}
		if (stored_total + stored->size() > p_budget) {
			full = true;
			return;
		}
		if (sha1_of(*stored) != *sha1) {
			std::fprintf(stderr, "%s %s: stored bytes do not hash to the manifest SHA-1\n", p_kind.c_str(), p_name.c_str());
			++errors;
			return;
		}
		std::vector<uint8_t> decoded;
		std::string type, meta;
		if (p_ref.kind == EntryRef::Kind::Res) {
			Result<std::pair<ResInfo, std::vector<uint8_t>>> r = m->read_res(p_name);
			if (!r) {
				std::fprintf(stderr, "res %s: %s\n", p_name.c_str(), r.error().message.c_str());
				++errors;
				return;
			}
			type = res_type_name(r->first.res_type);
			meta = hex_spaced(r->first.res_meta.data(), r->first.res_meta.size());
			decoded = std::move(r->second);
		} else {
			Result<std::vector<uint8_t>> c = m->read_chunk(p_ref.id);
			if (!c) {
				std::fprintf(stderr, "chunk %s: %s\n", p_name.c_str(), c.error().message.c_str());
				++errors;
				return;
			}
			decoded = std::move(*c);
		}
		const std::string hex = hex_lower(sha1->bytes.data(), sha1->bytes.size());
		if (!dir.write("stored/" + hex + ".bin", *stored) || !dir.write("payload/" + hex + ".bin", decoded)) {
			++errors;
			return;
		}
		seen.insert(*sha1);
		stored_total += stored->size();
		decoded_total += decoded.size();
		++rows;
		index += p_kind + "\t" + p_name + "\t" + hex + "\t" + std::to_string(stored->size()) + "\t" + std::to_string(decoded.size()) + "\t" + type + "\t" +
				meta + "\n";
	};
	for (const std::string &name : res_names) {
		if (full || errors > 0) {
			break;
		}
		take(EntryRef::res(name), "res", name);
	}
	for (const Guid &id : chunks) {
		if (full || errors > 0) {
			break;
		}
		take(EntryRef::chunk(id), "chunk", id.to_string());
	}
	if (errors == 0 && !dir.write_text("index.tsv", index)) {
		++errors;
	}
	std::fprintf(stderr, "export cache %s: %zu entries (%zu res and %zu chunks listed), stored %llu B, decoded %llu B, %zu files, %llu B written, %.3f s\n",
			p_sb.c_str(), rows, res_names.size(), chunks.size(), static_cast<unsigned long long>(stored_total), static_cast<unsigned long long>(decoded_total),
			dir.files(), static_cast<unsigned long long>(dir.bytes()), seconds_since(start));
	return errors == 0 && rows > 0 ? 0 : 1;
}

void put_u32(std::vector<uint8_t> &r_out, uint32_t p_value) {
	for (int i = 0; i < 4; ++i) {
		r_out.push_back(uint8_t(p_value >> (8 * i)));
	}
}

constexpr uint32_t kResFileVersion = 1;
constexpr uint32_t kNoView = UINT32_MAX;

std::vector<uint8_t> res_file_head(const res::RawRes &p_res, uint32_t p_view) {
	std::vector<uint8_t> head = { 'B', 'F', '3', 'R' };
	put_u32(head, kResFileVersion);
	put_u32(head, p_res.res_type);
	head.insert(head.end(), p_res.res_meta.begin(), p_res.res_meta.end());
	put_u32(head, p_view);
	put_u32(head, uint32_t(p_res.name.size()));
	head.insert(head.end(), p_res.name.begin(), p_res.name.end());
	const uint64_t len = p_res.payload.size();
	put_u32(head, uint32_t(len));
	put_u32(head, uint32_t(len >> 32));
	return head;
}

std::vector<res::ChunkId> referenced_chunks(const res::RawRes &p_res) {
	std::vector<res::ChunkId> out;
	if (p_res.res_type == res::kResTypeMeshSet) {
		res::MeshSet mesh;
		if (res::MeshSet::parse(p_res.bytes(), p_res.res_meta, mesh).ok()) {
			for (const res::MeshLod &lod : mesh.lods) {
				if (!lod.geometry.is_inline && std::find(out.begin(), out.end(), lod.geometry.chunk) == out.end()) {
					out.push_back(lod.geometry.chunk);
				}
			}
		}
	} else if (p_res.res_type == res::kResTypeDxTexture) {
		res::DxTexture tex;
		if (res::DxTexture::parse(p_res.bytes(), tex).ok() && !tex.chunk_id.is_zero()) {
			out.push_back(tex.chunk_id);
		}
	}
	return out;
}

int export_res(const InstallReader &p_reader, const Install &p_install, const std::string &p_sb, const std::string &p_tag, const std::string &p_out,
		const std::vector<std::string> &p_names) {
	const auto start = std::chrono::steady_clock::now();
	if (p_tag.empty() || p_tag.find_first_of("/\\:.") != std::string::npos) {
		std::fprintf(stderr, "tag must be a plain directory name: %s\n", p_tag.c_str());
		return 2;
	}
	Result<LevelMount> m = mount_loaded(p_install, p_sb);
	if (!m) {
		std::fprintf(stderr, "mount %s: %s\n", p_sb.c_str(), m.error().message.c_str());
		return 1;
	}
	ExportDir dir(p_reader, utf8_path(p_out));
	if (!dir.prepare({ p_tag + "/res", "chunks" })) {
		return 1;
	}
	const std::vector<std::string> keys = p_names.empty() ? res::decodable_res_names(*m) : p_names;
	std::mutex chunk_mutex;
	std::unordered_set<std::string> chunks_taken;
	std::atomic<size_t> errors{ 0 }, chunk_files{ 0 }, chunk_failures{ 0 };
	std::vector<std::string> types(keys.size());
	parallel_for(keys.size(), [&](size_t i) {
		Result<res::RawRes> raw = res::read_raw_res(*m, keys[i]);
		if (!raw) {
			std::fprintf(stderr, "res %s: %s\n", keys[i].c_str(), raw.error().message.c_str());
			++errors;
			return;
		}
		types[i] = res_type_name(raw->res_type);
		uint32_t view = raw->res_type == res::kResTypeDxTexture ? kNoView : 0;
		for (const res::ChunkId &id : referenced_chunks(*raw)) {
			const std::string hex = id.to_hex();
			Result<res::RawChunk> chunk = res::read_raw_chunk(*m, id);
			if (!chunk) {
				++chunk_failures;
				continue;
			}
			if (raw->res_type == res::kResTypeDxTexture) {
				view = chunk->view_offset;
			}
			{
				std::lock_guard lock(chunk_mutex);
				if (!chunks_taken.insert(hex).second) {
					continue;
				}
			}
			if (!dir.write("chunks/" + hex + ".chunk", chunk->bytes)) {
				++errors;
				continue;
			}
			++chunk_files;
		}
		char file[32];
		std::snprintf(file, sizeof(file), "%06zu.bin", i);
		if (!dir.write(p_tag + "/res/" + file, res_file_head(*raw, view), raw->payload)) {
			++errors;
		}
	});
	std::string index;
	for (size_t i = 0; i < keys.size(); ++i) {
		char file[32];
		std::snprintf(file, sizeof(file), "%06zu.bin", i);
		index += std::string(file) + "\t" + keys[i] + "\t" + types[i] + "\n";
	}
	if (errors == 0 && !dir.write_text(p_tag + "/index.tsv", index)) {
		++errors;
	}
	std::fprintf(stderr, "export res %s as %s: %zu res, %zu chunk files (%zu chunk reads failed), %zu files, %llu B written, %.3f s\n", p_sb.c_str(),
			p_tag.c_str(), keys.size(), chunk_files.load(), chunk_failures.load(), dir.files(), static_cast<unsigned long long>(dir.bytes()),
			seconds_since(start));
	return errors == 0 && !keys.empty() ? 0 : 1;
}

std::string ant_fields(const ant::AntPackageInfo &p_info) {
	return "Name=" + p_info.name + ";PackagingType=" + ant::packaging_name(p_info.packaging) + ";StreamingGuid=" + p_info.streaming_guid +
			";ChunkSize=" + std::to_string(p_info.chunk_size);
}

int export_ant(const InstallReader &p_reader, const std::shared_ptr<Install> &p_install, const std::string &p_out) {
	const auto start = std::chrono::steady_clock::now();
	Result<std::unique_ptr<ant::AntInstallSource>> opened = ant::AntInstallSource::open(p_install);
	if (!opened) {
		std::fprintf(stderr, "ant install source: %s\n", opened.error().message.c_str());
		return 1;
	}
	const ant::AntInstallSource &source = **opened;
	for (const std::string &w : source.report().warnings) {
		std::fprintf(stderr, "warning: %s\n", w.c_str());
	}
	ExportDir dir(p_reader, utf8_path(p_out));
	if (!dir.prepare({ "res", "chunk" })) {
		return 1;
	}
	const std::vector<ant::AntPackageInfo> &banks = source.all_banks();
	const std::vector<ant::AntPackageInfo> &packages = source.packages();

	std::map<std::string, std::set<std::string>> bundles_of;
	for (const ant::AntPackageInfo &b : banks) {
		bundles_of[b.superbundle].insert(b.bundle);
	}
	std::vector<std::string> sbs;
	for (const auto &[sb, bundles] : bundles_of) {
		sbs.push_back(sb);
	}
	std::vector<Result<LevelMount>> mounts = parallel_map<Result<LevelMount>>(sbs.size(), [&](size_t i) -> Result<LevelMount> {
		Result<LevelMount> mount = p_install->mount_superbundle(sbs[i]);
		if (!mount) {
			return mount;
		}
		const std::set<std::string> &bundles = bundles_of[sbs[i]];
		const Status st = mount->load_bundles(std::vector<std::string>(bundles.begin(), bundles.end()));
		if (!st) {
			return error_at("load bundles " + sbs[i], st.error());
		}
		return mount;
	});
	std::atomic<size_t> errors{ 0 };
	std::vector<std::string> res_rows(banks.size());
	parallel_for(banks.size(), [&](size_t i) {
		const ant::AntPackageInfo &bank = banks[i];
		const std::string key = to_lower_ascii(bank.name);
		const size_t sb = size_t(std::lower_bound(sbs.begin(), sbs.end(), bank.superbundle) - sbs.begin());
		if (!mounts[sb]) {
			std::fprintf(stderr, "%s: %s\n", bank.superbundle.c_str(), mounts[sb].error().message.c_str());
			++errors;
			return;
		}
		const std::optional<ResInfo> info = mounts[sb]->res_info(key);
		ant::AntPackageInfo as_res = bank;
		as_res.packaging = ant::AntPackagingType::Bundle;
		std::vector<uint8_t> bytes;
		std::string error;
		if (!info || !source.read(as_res, bytes, error)) {
			std::fprintf(stderr, "bank %s: %s\n", key.c_str(), info ? error.c_str() : "no res info");
			++errors;
			return;
		}
		char file[32];
		std::snprintf(file, sizeof(file), "res/%04zu.bin", i);
		if (!dir.write(file, bytes)) {
			++errors;
			return;
		}
		res_rows[i] = key + "\t" + file + "\t" + std::to_string(bytes.size()) + "\t" + hex_lower(info->res_meta.data(), info->res_meta.size()) + "\t" +
				bank.superbundle + "\t" + bank.bundle + "\n";
	});
	std::vector<std::string> chunk_files(packages.size());
	std::atomic<size_t> chunks{ 0 };
	parallel_for(packages.size(), [&](size_t i) {
		const ant::AntPackageInfo &p = packages[i];
		if (p.packaging != ant::AntPackagingType::Chunk) {
			return;
		}
		std::vector<uint8_t> bytes;
		std::string error;
		if (!source.read(p, bytes, error)) {
			std::fprintf(stderr, "package %s: %s\n", p.partition.c_str(), error.c_str());
			++errors;
			return;
		}
		const std::string file = "chunk/" + to_lower_ascii(p.streaming_guid) + ".bin";
		if (!dir.write(file, bytes)) {
			++errors;
			return;
		}
		chunk_files[i] = file;
		++chunks;
	});
	std::string res_tsv = "name\tfile\tsize\tres_meta\tsuperbundle\tbundle\n";
	for (const std::string &row : res_rows) {
		res_tsv += row;
	}
	std::string packages_tsv = "partition\ttype\tsuperbundle\tbundle\tchunk_file\tfields\n";
	for (size_t i = 0; i < packages.size(); ++i) {
		const ant::AntPackageInfo &p = packages[i];
		packages_tsv += p.partition + "\tAntPackageAsset\t" + p.superbundle + "\t" + p.bundle + "\t" + chunk_files[i] + "\t" + ant_fields(p) + "\n";
	}
	if (errors == 0 && (!dir.write_text("res.tsv", res_tsv) || !dir.write_text("packages.tsv", packages_tsv))) {
		++errors;
	}
	std::fprintf(stderr, "export ant: %zu banks, %zu packages (%zu chunks), %zu files, %llu B written, %.3f s\n", banks.size(), packages.size(), chunks.load(),
			dir.files(), static_cast<unsigned long long>(dir.bytes()), seconds_since(start));
	return errors == 0 && !banks.empty() ? 0 : 1;
}

}

const char *export_usage() {
	return "  export cache <sb> <out_dir> <max_mb>\n"
		   "  export res <sb> <tag> <data_dir> [<res>...]\n"
		   "  export ant <out_dir>\n";
}

int cmd_export(const InstallReader &p_reader, const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	if (p_args.size() == 4 && p_args[0] == "cache") {
		uint64_t budget = 0;
		if (!parse_megabytes(p_args[3], budget)) {
			return 2;
		}
		return export_cache(p_reader, *p_install, p_args[1], p_args[2], budget);
	}
	if (p_args.size() >= 4 && p_args[0] == "res") {
		return export_res(p_reader, *p_install, p_args[1], p_args[2], p_args[3], std::vector<std::string>(p_args.begin() + 4, p_args.end()));
	}
	if (p_args.size() == 2 && p_args[0] == "ant") {
		return export_ant(p_reader, p_install, p_args[1]);
	}
	std::fprintf(stderr, "usage:\n%s", export_usage());
	return 2;
}

}
