// Every SwfMovie res of the install (bf3/core/ui/swf_library.h; docs/design/ui-swf-fonts.md §4.1, §5.2).
#include "bf3/core/ui/swf_library.h"

#include "bf3/core/install.h"
#include "bf3/core/res_types.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/thread_pool.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>

namespace bf3::core::ui {

namespace {

bool listed_superbundle(std::string_view p_name) {
	const std::string l = to_lower_ascii(p_name);
	return l.find("chunks") == std::string::npos && l.rfind("win32/loc/", 0) != 0;
}

std::string_view file_component(std::string_view p_url) {
	const size_t slash = p_url.find_last_of("/\\");
	return slash == std::string_view::npos ? p_url : p_url.substr(slash + 1);
}

std::string_view strip_extension(std::string_view p_file) {
	const size_t dot = p_file.find_last_of('.');
	return dot == std::string_view::npos ? p_file : p_file.substr(0, dot);
}

} // namespace

std::vector<std::string> swf_superbundle_order(const Install &p_install) {
	std::vector<std::string> ui, frontend, levels, rest;
	for (const std::string &sb : p_install.superbundle_names()) {
		if (!listed_superbundle(sb)) {
			continue;
		}
		const std::string l = to_lower_ascii(sb);
		if (l == "win32/ui") {
			ui.push_back(sb);
		} else if (l == "win32/levels/frontend/frontend") {
			frontend.push_back(sb);
		} else if (multiplayer_level_id(sb).has_value()) {
			levels.push_back(sb);
		} else {
			rest.push_back(sb);
		}
	}
	std::vector<std::string> out;
	for (auto *group : { &ui, &frontend, &levels, &rest }) {
		out.insert(out.end(), group->begin(), group->end());
	}
	return out;
}

std::string swf_movie_name_for_url(std::string_view p_url) {
	if (p_url.empty() || is_font_library_url(p_url)) {
		return {};
	}
	const std::string stem = to_lower_ascii(strip_extension(file_component(p_url)));
	if (stem.empty()) {
		return {};
	}
	const std::string lower = to_lower_ascii(p_url);
	const bool is_static = lower.find("static/") != std::string::npos || lower.find("static\\") != std::string::npos;
	return (is_static ? "ui/static/" : "ui/assets/") + stem;
}

bool is_font_library_url(std::string_view p_url) {
	return iequals_ascii(file_component(p_url), "gfxfontlib.swf");
}

std::string swf_atlas_texture_name(std::string_view p_movie_name, std::string_view p_file_name) {
	return to_lower_ascii(p_movie_name) + "/" + to_lower_ascii(strip_extension(file_component(p_file_name)));
}

struct SwfLibrary::Impl {
	struct Holder {
		std::string superbundle;
		std::optional<LevelMount> mount; // from open(); bundles indexed on first use
		std::vector<std::string> bundles; // the bundles holding first-holder movies
		std::mutex mutex;
		bool created = false;
		Status status;
	};
	struct Parsed {
		std::once_flag once;
		std::optional<Result<std::shared_ptr<const SwfMovie>>> result;
	};

	std::shared_ptr<const Install> install;
	std::vector<SwfMovieSource> movies;
	std::unordered_map<std::string, size_t> by_name;
	std::vector<std::unique_ptr<Holder>> holders;
	std::unordered_map<std::string, size_t> holder_of; // superbundle -> holders index
	std::unordered_map<std::string, std::vector<uint8_t>> memory; // from_memory payloads by name
	mutable std::mutex parsed_mutex;
	mutable std::unordered_map<std::string, std::shared_ptr<Parsed>> parsed;
	double open_seconds = 0.0;
	size_t superbundles_listed = 0;

	// The holder mount with its bundles indexed (created once; a failure is memoised).
	Result<const LevelMount *> holder_mount(const SwfMovieSource &p_source) const {
		const auto it = holder_of.find(p_source.superbundle);
		if (it == holder_of.end()) {
			return make_error("swf " + p_source.name + ": no mount for " + p_source.superbundle);
		}
		Holder &h = *holders[it->second];
		std::lock_guard lock(h.mutex);
		if (!h.created) {
			h.created = true;
			if (!h.mount) {
				h.status = make_error("swf: " + h.superbundle + " did not mount");
			} else {
				Status st = h.mount->load_bundles(h.bundles);
				if (!st) {
					h.status = error_at("swf: load bundles of " + h.superbundle, st.error());
				}
			}
		}
		if (!h.status) {
			return h.status.error();
		}
		return &*h.mount;
	}
};

SwfLibrary::SwfLibrary() = default;
SwfLibrary::~SwfLibrary() = default;
SwfLibrary::SwfLibrary(SwfLibrary &&) noexcept = default;
SwfLibrary &SwfLibrary::operator=(SwfLibrary &&) noexcept = default;

Result<SwfLibrary> SwfLibrary::open(std::shared_ptr<const Install> p_install, const SwfLibraryOptions &p_options) {
	const auto start = std::chrono::steady_clock::now();
	if (!p_install) {
		return make_error("swf library: no install");
	}
	auto impl = std::make_unique<Impl>();
	impl->install = p_install;
	const std::vector<std::string> order = swf_superbundle_order(*p_install);
	impl->superbundles_listed = order.size();
	std::vector<Result<LevelMount>> mounts =
			parallel_map<Result<LevelMount>>(order.size(), [&](size_t i) { return p_install->mount_superbundle(order[i]); }, p_options.threads);
	struct Job {
		size_t sb;
		size_t bundle;
	};
	std::vector<Job> jobs;
	for (size_t s = 0; s < mounts.size(); ++s) {
		if (!mounts[s]) {
			return error_at("swf library: mount " + order[s], mounts[s].error());
		}
		for (size_t b = 0; b < mounts[s]->bundle_count(); ++b) {
			jobs.push_back({ s, b });
		}
	}
	struct Found {
		std::string name;
		Sha1 sha1;
		uint64_t size = 0;
	};
	struct JobResult {
		std::vector<Found> movies;
		std::string error;
	};
	std::vector<JobResult> found = parallel_map<JobResult>(
			jobs.size(),
			[&](size_t j) {
				JobResult r;
				Result<BundleManifest> manifest = mounts[jobs[j].sb]->read_manifest(jobs[j].bundle);
				if (!manifest) {
					r.error = order[jobs[j].sb] + " " + mounts[jobs[j].sb]->bundle_name(jobs[j].bundle) + ": " + manifest.error().message;
					return r;
				}
				for (const ResEntry &e : manifest->res) {
					if (e.res_type == res_type::kSwfMovie) {
						r.movies.push_back({ asset_key(e.name), e.sha1, e.original_size });
					}
				}
				return r;
			},
			p_options.threads);
	struct Acc {
		SwfMovieSource source;
		std::set<std::string> sha1s;
	};
	std::map<std::string, Acc> acc;
	std::map<size_t, std::set<std::string>> holder_bundles; // sb index -> bundles
	for (size_t j = 0; j < jobs.size(); ++j) {
		if (!found[j].error.empty()) {
			return make_error("swf library: " + found[j].error);
		}
		for (const Found &f : found[j].movies) {
			Acc &a = acc[f.name];
			if (a.source.holders == 0) {
				a.source.name = f.name;
				a.source.superbundle = order[jobs[j].sb];
				a.source.bundle = mounts[jobs[j].sb]->bundle_name(jobs[j].bundle);
				a.source.sha1 = f.sha1;
				a.source.size = f.size;
				holder_bundles[jobs[j].sb].insert(a.source.bundle);
			}
			a.source.holders++;
			a.sha1s.insert(f.sha1.to_string());
		}
	}
	for (auto &[name, a] : acc) {
		a.source.distinct_sha1s = uint32_t(a.sha1s.size());
		impl->by_name[name] = impl->movies.size();
		impl->movies.push_back(std::move(a.source));
	}
	for (auto &[sb, bundles] : holder_bundles) {
		auto h = std::make_unique<Impl::Holder>();
		h->superbundle = order[sb];
		h->mount.emplace(std::move(*mounts[sb]));
		h->bundles.assign(bundles.begin(), bundles.end());
		impl->holder_of[order[sb]] = impl->holders.size();
		impl->holders.push_back(std::move(h));
	}
	impl->open_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	SwfLibrary lib;
	lib.impl_ = std::move(impl);
	return lib;
}

SwfLibrary SwfLibrary::from_memory(std::vector<std::pair<std::string, std::vector<uint8_t>>> p_payloads) {
	auto impl = std::make_unique<Impl>();
	std::map<std::string, std::vector<uint8_t>> sorted;
	for (auto &[name, bytes] : p_payloads) {
		sorted.emplace(asset_key(name), std::move(bytes));
	}
	for (auto &[name, bytes] : sorted) {
		SwfMovieSource s;
		s.name = name;
		s.superbundle = "memory";
		s.bundle = "memory";
		s.size = bytes.size();
		s.holders = 1;
		s.distinct_sha1s = 1;
		impl->by_name[name] = impl->movies.size();
		impl->movies.push_back(std::move(s));
		impl->memory.emplace(name, std::move(bytes));
	}
	SwfLibrary lib;
	lib.impl_ = std::move(impl);
	return lib;
}

const std::shared_ptr<const Install> &SwfLibrary::install() const {
	return impl_->install;
}

const std::vector<SwfMovieSource> &SwfLibrary::movies() const {
	return impl_->movies;
}

const SwfMovieSource *SwfLibrary::find(std::string_view p_name) const {
	const auto it = impl_->by_name.find(asset_key(p_name));
	return it == impl_->by_name.end() ? nullptr : &impl_->movies[it->second];
}

Result<std::vector<uint8_t>> SwfLibrary::read_payload(std::string_view p_name) const {
	const SwfMovieSource *source = find(p_name);
	if (source == nullptr) {
		return make_error("swf " + asset_key(p_name) + ": no such SwfMovie in the install");
	}
	if (const auto it = impl_->memory.find(source->name); it != impl_->memory.end()) {
		return it->second;
	}
	Result<const LevelMount *> mount = impl_->holder_mount(*source);
	if (!mount) {
		return mount.error();
	}
	Result<std::pair<ResInfo, std::vector<uint8_t>>> res = (*mount)->read_res(source->name);
	if (!res) {
		return error_at("swf " + source->name, res.error());
	}
	if (res->first.res_type != res_type::kSwfMovie) {
		return make_error("swf " + source->name + ": res type is not SwfMovie");
	}
	return std::move(res->second);
}

Result<std::shared_ptr<const SwfMovie>> SwfLibrary::load(std::string_view p_name) const {
	const std::string key = asset_key(p_name);
	std::shared_ptr<Impl::Parsed> slot;
	{
		std::lock_guard lock(impl_->parsed_mutex);
		std::shared_ptr<Impl::Parsed> &s = impl_->parsed[key];
		if (!s) {
			s = std::make_shared<Impl::Parsed>();
		}
		slot = s;
	}
	std::call_once(slot->once, [&]() {
		Result<std::vector<uint8_t>> payload = read_payload(key);
		if (!payload) {
			slot->result.emplace(payload.error());
			return;
		}
		Result<SwfMovie> movie = SwfMovie::parse(*payload);
		if (!movie) {
			slot->result.emplace(error_at("swf " + key, movie.error()));
			return;
		}
		slot->result.emplace(std::make_shared<const SwfMovie>(std::move(*movie)));
	});
	return *slot->result;
}

Result<res::LoadedTexture> SwfLibrary::load_atlas_texture(std::string_view p_movie_name, const SwfExternalImage &p_image) const {
	const SwfMovieSource *source = find(p_movie_name);
	if (source == nullptr) {
		return make_error("swf " + asset_key(p_movie_name) + ": no such SwfMovie in the install");
	}
	Result<const LevelMount *> mount = impl_->holder_mount(*source);
	if (!mount) {
		return mount.error();
	}
	const std::string name = swf_atlas_texture_name(source->name, p_image.file_name);
	Result<res::LoadedTexture> texture = res::load_texture(**mount, name);
	if (!texture) {
		return error_at("swf " + source->name + " atlas " + name, texture.error());
	}
	return texture;
}

Result<res::DxTexture> SwfLibrary::read_atlas_texture_header(std::string_view p_movie_name, const SwfExternalImage &p_image) const {
	const SwfMovieSource *source = find(p_movie_name);
	if (source == nullptr) {
		return make_error("swf " + asset_key(p_movie_name) + ": no such SwfMovie in the install");
	}
	Result<const LevelMount *> mount = impl_->holder_mount(*source);
	if (!mount) {
		return mount.error();
	}
	const std::string name = swf_atlas_texture_name(source->name, p_image.file_name);
	Result<std::pair<ResInfo, std::vector<uint8_t>>> res = (*mount)->read_res(name);
	if (!res) {
		return error_at("swf " + source->name + " atlas " + name, res.error());
	}
	if (res->first.res_type != res_type::kDxTexture) {
		return make_error("swf " + source->name + " atlas " + name + ": not a DxTexture");
	}
	res::DxTexture texture;
	const res::ResError e = res::DxTexture::parse(res::Bytes(res->second.data(), res->second.size()), texture);
	if (!e.ok()) {
		return make_error("swf " + source->name + " atlas " + name + ": " + e.message());
	}
	return texture;
}

double SwfLibrary::open_seconds() const {
	return impl_->open_seconds;
}

size_t SwfLibrary::superbundles_listed() const {
	return impl_->superbundles_listed;
}

} // namespace bf3::core::ui
